#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M-COV0：全量 shader 静态 coverage manifest 门禁。

对 CMakeLists.txt 定义的每个 shader artifact（file.entry × dxil/spirv 双 target，
参数与 CMake shader 构建命令严格同源）跑 slangc -trace-coverage -trace-coverage-boolean，
解析编译期吐出的 <out>.coverage-manifest.json，校验结构合法性并汇总 counter↔源码行映射。

  python tools/check-shader-coverage.py                 # 全量跑，exit 0=PASS / 1=违规
  python tools/check-shader-coverage.py --list          # 只列 artifact 清单不编译
  python tools/check-shader-coverage.py --only cull.cs  # 只跑指定 artifact
  python tools/check-shader-coverage.py --target spirv  # dxil|spirv|both（默认 both）
  python tools/check-shader-coverage.py --jobs 4        # 并行（默认 4）
  python tools/check-shader-coverage.py selftest        # manifest 校验器/解析器自测

门禁口径（C 轨第③项方案 A 拍板 2026-11-16）：
  - 编译 error[...]                                    -> 违规（拿不到合法 manifest 即不过）
  - manifest 缺 format/version/buffer/entries，或
    counter_count=0 / buffer.element_stride!=8 / entries 空 -> 违规
  - 合法 manifest（counter_count>0 + entry 映射非空 + buffer stride=8）-> PASS
  - 纯编译期检查：产物落系统临时目录自动清理，主构建链/运行时零改动、零基线重拍

唯一数据源 = CMakeLists.txt（解析逻辑与 check-shader-uniformity.py 同源），禁手写 artifact 清单。
实测（2026-11-16）：slangc 的 -trace-coverage 有 statement/function/branch 三种模式，
-trace-coverage-boolean 去原子竞争；dxil/spirv 双 target 均吐 v2 manifest，含
counter_count / 源码行↔counter 映射 / __slang_coverage binding 元数据。
"""
import glob
import json
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
_DXC_GLOB = "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxcompiler.dll"


def find_dxc_dir():
    hits = sorted(glob.glob(_DXC_GLOB))
    return str(Path(hits[-1]).parent) if hits else None


def _extract_set_items(text: str, var: str) -> list:
    m = re.search(r'set\(' + re.escape(var) + r'\b(.*?)\)', text, re.DOTALL)
    if not m:
        raise SystemExit(f"[cov-m0] CMakeLists.txt 里找不到 {var}")
    return re.findall(r'"([^"]+)"', m.group(1))


def parse_artifacts(cmake_text: str) -> list:
    """从 CMakeLists.txt 解析 (file, entry, suffix, stage, sm66) 列表。
    与 check-shader-uniformity.py 严格同源：_ENTRIES_* + 文件映射链 + _SM66_ARTIFACTS。
    """
    entries_defs = {}
    for m in re.finditer(r'set\(_ENTRIES_([A-Z0-9_]+)\b(.*?)\)', cmake_text, re.DOTALL):
        items = re.findall(r'"([^"]+)"', m.group(2))
        if items:
            entries_defs[m.group(1)] = items
    if "DEFAULT" not in entries_defs:
        raise SystemExit("[cov-m0] CMakeLists.txt 里找不到 _ENTRIES_DEFAULT")
    sm66 = set(_extract_set_items(cmake_text, "_SM66_ARTIFACTS"))
    mapping = dict(re.findall(
        r'\(\s*_SRC_NAME\s+STREQUAL\s+"(\w+)"\s*\)\s*\n\s*set\(_ENTRIES\s+"\$\{_ENTRIES_(\w+)\}"\)',
        cmake_text))
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
    """与 CMake shader 构建命令严格同源 + coverage 编译期插桩开关。"""
    a = [str(SLANGC), art["src"]]
    if target == "spirv":
        a += ["-target", "spirv", "-entry", art["entry"], "-stage", art["stage"],
              "-fvk-b-shift", "256", "0", "-fvk-t-shift", "0", "0",
              "-fvk-s-shift", "128", "0", "-fvk-u-shift", "384", "0"]
    else:
        a += ["-dxc-path", dxc_dir, "-target", "dxil",
              "-entry", art["entry"], "-stage", art["stage"]]
        if art["sm66"]:
            a += ["-profile", "sm_6_6"]
    a += ["-trace-coverage", "-trace-coverage-boolean"]
    return a


def validate_manifest(d) -> tuple:
    """校验 v2 coverage manifest 结构，返回 (ok, errors, stats)。"""
    errors = []
    if not isinstance(d, dict):
        return False, ["manifest root is not an object"], {}
    if d.get("format") != "slang-coverage":
        errors.append(f"format={d.get('format')!r} != 'slang-coverage'")
    if d.get("version") != 2:
        errors.append(f"version={d.get('version')!r} != 2")
    buf = d.get("buffer")
    if not isinstance(buf, dict):
        errors.append("buffer missing")
    else:
        if buf.get("name") != "__slang_coverage":
            errors.append(f"buffer.name={buf.get('name')!r}")
        if buf.get("element_stride") != 8:
            errors.append(f"buffer.element_stride={buf.get('element_stride')!r} != 8")
    n = d.get("counter_count")
    if not isinstance(n, int) or n <= 0:
        errors.append(f"counter_count={n!r} not positive int")
    entries = d.get("entries")
    if not isinstance(entries, list) or not entries:
        errors.append("entries missing or empty")
    else:
        for i, e in enumerate(entries):
            if not isinstance(e, dict) or not all(k in e for k in ("counter", "mode", "file", "line")):
                errors.append(f"entries[{i}] malformed")
                break
            if not isinstance(e["counter"], int) or e["counter"] < 0 or e["counter"] >= (n or 0):
                errors.append(f"entries[{i}].counter out of range")
                break
    stats = {
        "counters": n if isinstance(n, int) else 0,
        "entries": len(entries) if isinstance(entries, list) else 0,
        "modes": sorted({e.get("mode") for e in entries if isinstance(e, dict)}),
    }
    return (not errors), errors, stats


def classify(lines: list) -> list:
    """只收编译 error；其余 warning 属 informational（coverage 插桩常见转换提示）。"""
    return [s.strip() for s in lines
            if s.strip().startswith("error[") or s.strip().startswith("error:")]


def run_one(art: dict, target: str, dxc_dir: str | None) -> dict:
    args = build_args(art, target, dxc_dir)
    with tempfile.TemporaryDirectory() as td:
        out = str(Path(td) / f"out.{target}")
        try:
            p = subprocess.run(args + ["-o", out], capture_output=True, text=True,
                               stdin=subprocess.DEVNULL, timeout=60, errors="replace")
            combined = (p.stdout or "") + (p.stderr or "")
        except subprocess.TimeoutExpired:
            combined = "error[cov-m0]: slangc timeout (60s)"
        except OSError as e:
            combined = f"error[cov-m0]: slangc spawn failed: {e}"
        errors = classify(combined.splitlines())
        mf = Path(out + ".coverage-manifest.json")
        manifest = None
        if mf.exists():
            try:
                manifest = json.loads(mf.read_text(encoding="utf-8"))
            except (OSError, ValueError) as e:
                errors.append(f"error[cov-m0]: manifest parse failed: {e}")
        else:
            errors.append("error[cov-m0]: no coverage-manifest.json produced")
    ok, verr, stats = validate_manifest(manifest)
    return {**art, "target": target, "ok": ok and not errors,
            "errors": errors + verr, "stats": stats}


def cmd_list(arts: list) -> int:
    for a in arts:
        print(f"{a['name']:<28} entry={a['entry']:<18} stage={a['stage']:<8}"
              f"sm66={'y' if a['sm66'] else 'n'}")
    print(f"total: {len(arts)} artifacts")
    return 0


def cmd_run(arts: list, targets: list, dxc_dir: str | None, jobs: int) -> int:
    if dxc_dir is None and "dxil" in targets:
        print("[cov-m0] FAIL: dxcompiler.dll not found (dxil target unavailable)")
        return 2
    t0 = time.time()
    print(f"check-shader-coverage: {len(arts)} artifacts x {len(targets)} targets "
          f"(slangc={SLANGC.as_posix()}, dxc={dxc_dir})")
    tasks = [(a, t) for a in arts for t in targets]
    if jobs <= 1:
        results = [run_one(a, t, dxc_dir) for a, t in tasks]
    else:
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            results = list(ex.map(lambda p: run_one(p[0], p[1], dxc_dir), tasks))
    for r in results:
        for s in r["errors"]:
            print(f"[FAIL] {r['name']}/{r['target']}: {s}")
    n_manifest = sum(1 for r in results if r["ok"])
    n_counters = sum(r["stats"]["counters"] for r in results if r["ok"])
    n_entries = sum(r["stats"]["entries"] for r in results if r["ok"])
    all_modes = sorted({m for r in results for m in r["stats"]["modes"]})
    ok = all(r["ok"] for r in results)
    verdict = "PASS" if ok else "FAIL"
    print(f"check-shader-coverage: {verdict} "
          f"(artifacts={len(arts)} targets={len(targets)} manifests={n_manifest} "
          f"counters={n_counters} entries={n_entries} modes={','.join(all_modes) or '-'} "
          f"elapsed={time.time()-t0:.1f}s)")
    return 0 if ok else 1


def selftest() -> int:
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("  ok   " if cond else "  FAIL ") + msg)
        ok = ok and cond

    good = {
        "format": "slang-coverage", "version": 2, "counter_count": 2,
        "buffer": {"name": "__slang_coverage", "element_stride": 8, "space": 0, "binding": 0},
        "entries": [
            {"kind": "line", "counter": 0, "mode": "boolean", "file": "a.slang", "line": 5},
            {"kind": "line", "counter": 1, "mode": "boolean", "file": "a.slang", "line": 9},
        ],
    }
    v, err, st = validate_manifest(good)
    check(v and st == {"counters": 2, "entries": 2, "modes": ["boolean"]},
          "manifest: valid v2 -> PASS + correct stats")
    d = dict(good); d["format"] = "wrong"
    check(not validate_manifest(d)[0], "manifest: wrong format -> FAIL")
    d = dict(good); d["version"] = 1
    check(not validate_manifest(d)[0], "manifest: wrong version -> FAIL")
    d = dict(good); d["counter_count"] = 0
    check(not validate_manifest(d)[0], "manifest: counter_count=0 -> FAIL")
    d = dict(good); d["buffer"] = dict(good["buffer"], element_stride=4)
    check(not validate_manifest(d)[0], "manifest: buffer stride!=8 -> FAIL")
    d = dict(good); d["entries"] = []
    check(not validate_manifest(d)[0], "manifest: empty entries -> FAIL")
    check(classify(["error[E30015]: undefined identifier"]) == ["error[E30015]: undefined identifier"],
          "classify: compile error captured")
    check(classify(["warning[E30081]: implicit conversion"]) == [],
          "classify: informational warning ignored")
    mini = """
set(_ENTRIES_DEFAULT "vsMain|vs|vertex" "psMain|ps|fragment")
set(_ENTRIES_CULL "csCull|cs|compute")
set(_SM66_ARTIFACTS "rt.cs")
foreach(_SRC ${NL_SHADER_SRCS})
    if(_SRC_NAME STREQUAL "cull")
        set(_ENTRIES "${_ENTRIES_CULL}")
    else()
        set(_ENTRIES "${_ENTRIES_DEFAULT}")
    endif()
"""
    m = dict(re.findall(
        r'\(\s*_SRC_NAME\s+STREQUAL\s+"(\w+)"\s*\)\s*\n\s*set\(_ENTRIES\s+"\$\{_ENTRIES_(\w+)\}"\)',
        mini))
    check(m.get("cull") == "CULL", "parse: file->entries mapping chain")
    check(_extract_set_items(mini, "_SM66_ARTIFACTS") == ["rt.cs"],
          "parse: _SM66_ARTIFACTS extraction")
    print("check-shader-coverage selftest: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def usage() -> int:
    print(__doc__.strip())
    return 2


def main(argv: list) -> int:
    if argv and argv[0] == "selftest":
        return selftest()
    only, target, jobs, list_only = None, "both", 4, False
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
    arts = parse_artifacts(CMAKE.read_text(encoding="utf-8", errors="replace"))
    if only is not None:
        arts = [a for a in arts if a["name"] == only]
        if not arts:
            print(f"[cov-m0] no artifact matches '{only}'")
            return 2
    if not arts:
        print("[cov-m0] no shader artifacts parsed from CMakeLists.txt")
        return 2
    if list_only:
        return cmd_list(arts)
    targets = ["dxil", "spirv"] if target == "both" else [target]
    return cmd_run(arts, targets, find_dxc_dir(), jobs)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
