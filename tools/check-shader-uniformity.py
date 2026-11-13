#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M-PERF0（候选 A）：全量 shader 静态 uniformity 门禁。

对 CMakeLists.txt 定义的每个 shader artifact（file.entry × dxil/spirv 双 target，
参数与 CMake shader 构建命令严格同源）跑 slangc -validate-uniformity：

  python tools/check-shader-uniformity.py                # 全量跑，exit 0=PASS / 1=违规
  python tools/check-shader-uniformity.py --list         # 只列 artifact 清单不编译
  python tools/check-shader-uniformity.py --only direct.csrefgi   # 只跑指定 artifact
  python tools/check-shader-uniformity.py --target dxil  # dxil|spirv|both（默认 both）
  python tools/check-shader-uniformity.py --jobs 4       # 并行（默认 4）
  python tools/check-shader-uniformity.py selftest       # 分类器/解析器自测，exit 0/1

门禁口径（Q-P3 拍板 2026-11-13）：
  - slangc 编译 error[...]                        -> 违规（无法验证 uniformity 即不过）
  - warning 行含 "uniform"（大小写无关）           -> uniformity 违规
  - 其余 warning（E30081 隐式转换等）              -> informational，只报告不挡
  - clean 基线实锤（2026-11-13，10 代表入口 rc=0 零警告）：新增 uniformity 违规即 FAIL。

唯一数据源 = CMakeLists.txt（_ENTRIES_* 定义 + 文件映射 if/elseif 链 + _SM66_ARTIFACTS
+ shader GLOB 目录），禁手写 artifact 清单（锚与 CMake 漂移即失真）。
"""
import glob
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE = ROOT / "CMakeLists.txt"
SLANGC = ROOT / "thirdparty" / "slang" / "dist" / "bin" / "slangc.exe"
SHADER_DIR = ROOT / "renderer" / "shaders"

# 与 CMake 相同口径的 dxcompiler.dll 探测（版本最高的 SDK）。
_DXC_GLOB = "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxcompiler.dll"


def find_dxc_dir():
    hits = sorted(glob.glob(_DXC_GLOB))
    if not hits:
        return None
    return str(Path(hits[-1]).parent)


def _extract_set_items(text: str, var: str) -> list:
    """提取 set(_VAR "a|b" "c|d" ...)（可跨行）里的全部引号项。"""
    m = re.search(r'set\(' + re.escape(var) + r'\b(.*?)\)', text, re.DOTALL)
    if not m:
        raise SystemExit(f"[perf-m0] CMakeLists.txt 里找不到 {var}")
    return re.findall(r'"([^"]+)"', m.group(1))


def parse_artifacts(cmake_text: str) -> list:
    """从 CMakeLists.txt 解析 (file, entry, suffix, stage, sm66) 列表。

    与 CMake 的 if/elseif 文件映射链同源：映射链里没有的文件走 _ENTRIES_DEFAULT
    （vsMain/psMain）。禁手写清单——锚与 CMake 漂移即失真。
    """
    entries_defs = {}
    for m in re.finditer(r'set\(_ENTRIES_([A-Z0-9_]+)\b(.*?)\)', cmake_text, re.DOTALL):
        items = re.findall(r'"([^"]+)"', m.group(2))
        if items:
            entries_defs[m.group(1)] = items
    if "DEFAULT" not in entries_defs:
        raise SystemExit("[perf-m0] CMakeLists.txt 里找不到 _ENTRIES_DEFAULT")
    sm66 = set(_extract_set_items(cmake_text, "_SM66_ARTIFACTS"))
    # 文件映射链：(_SRC_NAME STREQUAL "x") 紧跟 set(_ENTRIES "${_ENTRIES_Y}")
    mapping = dict(re.findall(
        r'\(\s*_SRC_NAME\s+STREQUAL\s+"(\w+)"\s*\)\s*\n\s*set\(_ENTRIES\s+"\$\{_ENTRIES_(\w+)\}"\)',
        cmake_text))
    # 主绘制外挂 shader（如 m-sc 系列若存在）一律走 DEFAULT，与 CMake else() 同口径。
    arts = []
    for src in sorted(SHADER_DIR.glob("*.slang")):
        name = src.stem
        var = mapping.get(name, "DEFAULT")
        for item in entries_defs.get(var, []):
            entry, suffix, stage = item.split("|")
            arts.append({
                "name": f"{name}.{suffix}",
                "src": str(src),
                "entry": entry,
                "stage": stage,
                "sm66": f"{name}.{suffix}" in sm66,
            })
    return arts


def build_args(art: dict, target: str, dxc_dir: str | None) -> list:
    """与 CMake shader 构建命令严格同源（CMakeLists.txt L565/L572 两条 custom_command）。"""
    a = [str(SLANGC), art["src"]]
    if target == "spirv":
        a += ["-target", "spirv", "-entry", art["entry"], "-stage", art["stage"],
              "-fvk-b-shift", "256", "0", "-fvk-t-shift", "0", "0",
              "-fvk-s-shift", "128", "0", "-fvk-u-shift", "384", "0"]
    else:  # dxil
        a += ["-dxc-path", dxc_dir, "-target", "dxil",
              "-entry", art["entry"], "-stage", art["stage"]]
        if art["sm66"]:
            a += ["-profile", "sm_6_6"]
    a += ["-validate-uniformity"]
    return a


def classify(lines: list) -> dict:
    """逐行分类：uniformity 违规 / 编译 error / 其他 warning（informational）。"""
    uni, err, info = [], [], []
    for ln in lines:
        s = ln.strip()
        if s.startswith("error[") or s.startswith("error:"):
            err.append(s)
        elif s.startswith("warning") and re.search(r"uniform", s, re.IGNORECASE):
            uni.append(s)
        elif s.startswith("warning"):
            info.append(s)
    return {"uniformity": uni, "errors": err, "info": info}


def run_one(art: dict, target: str, dxc_dir: str | None) -> dict:
    args = build_args(art, target, dxc_dir)
    with tempfile.TemporaryDirectory() as td:
        out = str(Path(td) / f"out.{target}")
        args = args + ["-o", out]
        try:
            p = subprocess.run(args, capture_output=True, text=True,
                               stdin=subprocess.DEVNULL, timeout=30,
                               errors="replace")
            combined = (p.stdout or "") + (p.stderr or "")
        except subprocess.TimeoutExpired:
            combined = "error[perf-m0]: slangc timeout (30s)"
        except OSError as e:
            combined = f"error[perf-m0]: slangc spawn failed: {e}"
    c = classify(combined.splitlines())
    return {**art, "target": target, **c}


def cmd_list(arts: list) -> int:
    for a in arts:
        print(f"{a['name']:<28} entry={a['entry']:<18} stage={a['stage']:<8}"
              f"sm66={'y' if a['sm66'] else 'n'}")
    print(f"total: {len(arts)} artifacts")
    return 0


def cmd_run(arts: list, targets: list, dxc_dir: str | None, jobs: int) -> int:
    if dxc_dir is None and "dxil" in targets:
        print("[perf-m0] FAIL: dxcompiler.dll not found (dxil target unavailable)")
        return 2
    t0 = time.time()
    print(f"check-shader-uniformity: {len(arts)} artifacts x {len(targets)} targets "
          f"(slangc={SLANGC.as_posix()}, dxc={dxc_dir})")
    tasks = [(a, t) for a in arts for t in targets]
    results = []
    if jobs <= 1:
        for a, t in tasks:
            results.append(run_one(a, t, dxc_dir))
    else:
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            results = list(ex.map(lambda p: run_one(p[0], p[1], dxc_dir), tasks))
    n_uni = sum(len(r["uniformity"]) for r in results)
    n_err = sum(len(r["errors"]) for r in results)
    n_info = sum(len(r["info"]) for r in results)
    for r in results:
        for s in r["errors"]:
            print(f"[ERR ] {r['name']}/{r['target']}: {s}")
        for s in r["uniformity"]:
            print(f"[UNIF] {r['name']}/{r['target']}: {s}")
        for s in r["info"]:
            print(f"[info] {r['name']}/{r['target']}: {s}")
    ok = (n_uni == 0 and n_err == 0)
    verdict = "PASS" if ok else "FAIL"
    print(f"check-shader-uniformity: {verdict} "
          f"(artifacts={len(arts)} targets={len(targets)} uniformity={n_uni} "
          f"compileErrors={n_err} otherWarnings={n_info} elapsed={time.time()-t0:.1f}s)")
    return 0 if ok else 1


def selftest() -> int:
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("  ok   " if cond else "  FAIL ") + msg)
        ok = ok and cond

    # 分类器：uniformity 违规（关键词匹配，不依赖具体警告码）
    c = classify(["warning[E99999]: uniformity validation failed: divergent wave op"])
    check(len(c["uniformity"]) == 1, "classify: uniformity keyword -> violation")
    c = classify(["warning[E30081]: implicit conversion not recommended"])
    check(c["uniformity"] == [] and len(c["info"]) == 1,
          "classify: implicit conversion -> informational")
    c = classify(["error[E30015]: undefined identifier"])
    check(len(c["errors"]) == 1, "classify: compile error -> violation")
    c = classify(["note: did you mean 'csGather'?"])
    check(sum(len(v) for v in c.values()) == 0, "classify: note lines ignored")

    # 解析器：内嵌 mini CMake 片段（结构同源）
    mini = """
set(_ENTRIES_DEFAULT "vsMain|vs|vertex" "psMain|ps|fragment")
set(_ENTRIES_RT "csRt|cs|compute" "psTonemap|pstonemap|fragment")
set(_SM66_ARTIFACTS "rt.cs")
foreach(_SRC ${NL_SHADER_SRCS})
    if(_SRC_NAME STREQUAL "model")
        set(_ENTRIES "${_ENTRIES_MODEL}")
    elseif(_SRC_NAME STREQUAL "rt")
        set(_ENTRIES "${_ENTRIES_RT}")
    else()
        set(_ENTRIES "${_ENTRIES_DEFAULT}")
    endif()
"""
    m = dict(re.findall(
        r'\(\s*_SRC_NAME\s+STREQUAL\s+"(\w+)"\s*\)\s*\n\s*set\(_ENTRIES\s+"\$\{_ENTRIES_(\w+)\}"\)',
        mini))
    check(m.get("model") == "MODEL" and m.get("rt") == "RT",
          "parse: file->entries mapping chain")
    items = _extract_set_items(mini, "_SM66_ARTIFACTS")
    check(items == ["rt.cs"], "parse: _SM66_ARTIFACTS extraction")

    print("check-shader-uniformity selftest: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def usage() -> int:
    print(__doc__.strip())
    return 2


def main(argv: list) -> int:
    if argv and argv[0] == "selftest":
        return selftest()
    only = None
    target = "both"
    jobs = 4
    list_only = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--list":
            list_only = True; i += 1
        elif a == "--only" and i + 1 < len(argv):
            only = argv[i + 1]; i += 2
        elif a == "--target" and i + 1 < len(argv):
            target = argv[i + 1]; i += 2
        elif a == "--jobs" and i + 1 < len(argv):
            jobs = int(argv[i + 1]); i += 2
        elif a in ("-h", "--help", "usage"):
            return usage()
        else:
            return usage()
    if target not in ("dxil", "spirv", "both"):
        return usage()
    cmake_text = CMAKE.read_text(encoding="utf-8", errors="replace")
    arts = parse_artifacts(cmake_text)
    if only is not None:
        arts = [a for a in arts if a["name"] == only]
        if not arts:
            print(f"[perf-m0] no artifact matches --only '{only}'")
            return 2
    if not arts:
        print("[perf-m0] no shader artifacts parsed from CMakeLists.txt")
        return 2
    if list_only:
        return cmd_list(arts)
    targets = ["dxil", "spirv"] if target == "both" else [target]
    dxc = find_dxc_dir()
    return cmd_run(arts, targets, dxc, jobs)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
