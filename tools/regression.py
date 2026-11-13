#!/usr/bin/env python3
"""nl-engine M0.9 full regression gate (one command, all example cases, both backends).

    python tools/regression.py                    # run everything, exit 0 only if all green
    python tools/regression.py --only m0.5/nohud  # filter by version[/case] id
    python tools/regression.py --backend dx12     # limit to one backend
    python tools/regression.py --jobs 4           # parallel across cases (default 4; 1 = serial)
    python tools/regression.py --report PATH.md   # where the markdown report goes
    python tools/regression.py --resume           # reuse last report's passes, rerun FAIL/missing

Usage discipline (2026-11-08): --only is the everyday loop ("current stage
only"); the full run is the commit/tag gate. Cases run in parallel across
(version, case) pairs, but the two backends of one case run serially — tool
cases write fixed shared paths (e.g. build/qem/showcase-*) with no backend in
the name, so same-case parallelism would collide. GPU screenshots are
deterministic, but if a suspicious near/FAIL appears under load, re-run with
--jobs 1 to rule out contention.

Reuses the showcase module for every shared semantic (exe resolution, argument
building, baseline lookup, pixel comparison) so there is exactly one parser of
example/cases.json in this repo. Historical milestone exes come from the tag
snapshots under build/showcase/<tag>/ (see tools/showcase/rebuild.py); the
current version runs from build/<backend>/viewer.exe.

exe freshness gate (2026-11-08): before any run that touches current-tree
exes, build/<backend>/*.exe must be newer than every source file they depend
on (core/ renderer/ viewer/ nanite-tools/ material-graph-tools/). A stale exe
refuses to run (exit 2) — this kills the "baseline retaken with an old exe"
false-green class (M4.2 vulkan HUD baselines incident).

Verdict tiers, matching the showcase convention:
  bytes  - PNG byte-identical to the baseline (the normal case; screenshots are
           deterministic)
  near   - MD5 differs but pixel diff is maxDelta <= 1/255 and <= 2 pixels
           (documented GPU rounding noise, e.g. M0.4 inner-wall / resize)
  FAIL   - anything else: wrong image, missing exe, non-zero exit, missing
           "SELFTEST PASS", deviceErrors > 0, timeout
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent          # tools/
ROOT = HERE.parent                              # repo root
sys.path.insert(0, str(HERE / "showcase"))      # reuse the showcase module

# Cross-tree gate (2026-11-10): refuse when cwd tree != this script's tree.
# Kills the "wt session ran the MAIN tree's regression / rewrote MAIN-tree
# baselines" contamination class. See tools/tree_gate.py.
sys.path.insert(0, str(HERE))
import tree_gate as _tree_gate  # noqa: E402
_tree_gate.gate(__file__)

import server as show  # noqa: E402  (module-level side effects: loads cases.json)

# 产物目录可用环境变量覆盖（CI/沙箱场景：指向全新目录则全程零删除——
# 每个用例跑前只清理「目标文件已存在」的陈旧产物，新目录里没有可删的东西）。
RUNS_DIR = Path(os.environ.get(
    "NL_REGRESSION_RUNS_DIR", str(ROOT / "build" / "regression" / "runs")))
DEFAULT_REPORT_MD = ROOT / "build" / "regression" / "report.md"
DEFAULT_REPORT_JSON = ROOT / "build" / "regression" / "report.json"

BACKENDS = ("dx12", "vulkan")
# 2026-11-13 S12 M12.1 收口实锤：120s 曾完全失效（见 run_viewer 注释），全量
# 挂 25 分钟零输出、无子进程、报告未生成，且是用户先问才被发现。现改为
# wait(timeout) 真生效 + 超时即告警。60s 的依据 = M12.0 全量 810 条实测
# p50 141ms / p99 7573ms / max 15798ms / >30s 0 条，留约 4 倍余量。
RUN_TIMEOUT = 60
# Documented rounding-noise tier (see module docstring).
NEAR_MAX_DELTA = 1
NEAR_MAX_PIXELS = 2

# ---- exe 新鲜度门禁（2026-11-08 固化，P8 合并总闸的假绿教训）----
# 事故：M4.2 重拍 HUD 基线时 vulkan exe 是旧构建（面板 cap 510），dx12 用新 exe
# （cap 560）→ 两后端基线口径不一致，假绿一直埋到 P8 合并后的总闸才炸出
# （m0.9/s2.0/s4.1/s4.2 四对 vulkan 基线全要返工重拍）。
# 规则：跑闸前，当前树 exe（build/<backend>/*.exe）的 mtime 必须新于其所依赖的
# 全部源文件，否则整闸拒绝运行（exit 2）——重拍基线、跑总闸都必须在重编之后。
# tag 快照 exe（build/showcase/<tag>/）是刻意冻结的历史产物，不参与本检查。
# 依赖域按 exe 分桶：NMake 只重链真正依赖变更 TU 的 exe（viewer 链 renderer，
# 工具不链），全局比对会造成「无关源文件 touch → 工具 exe 永不新鲜」死锁。
SOURCE_EXTS = {".cpp", ".hpp", ".h", ".slang", ".py"}
EXE_SOURCE_DIRS = {
    "viewer.exe": ("core/src", "core/include", "renderer/src", "renderer/include",
                   "renderer/shaders", "viewer"),
    "nanite-tools.exe": ("core/src", "core/include", "nanite-tools"),
    "material-graph-tools.exe": ("core/src", "core/include", "material-graph-tools"),
}


def stale_sources(exe: Path, dirs: tuple[str, ...]) -> list[Path]:
    """Return source files strictly newer than the given exe (may be empty)."""
    if not exe.is_file():
        return []  # exe 缺失由既有 per-case 逻辑报错，这里不管
    t = exe.stat().st_mtime
    out: list[Path] = []
    for d in dirs:
        dp = ROOT / d
        if not dp.is_dir():
            continue
        for p in dp.rglob("*"):
            if p.is_file() and p.suffix in SOURCE_EXTS and p.stat().st_mtime > t:
                out.append(p)
    return out


def check_exe_freshness(backends: tuple[str, ...]) -> list[str]:
    """Return human-readable problems; empty list = all current-tree exes fresh."""
    problems: list[str] = []
    for b in backends:
        for name, dirs in EXE_SOURCE_DIRS.items():
            exe = ROOT / "build" / b / name
            bad = stale_sources(exe, dirs)
            if bad:
                newest = max(bad, key=lambda p: p.stat().st_mtime)
                problems.append(
                    f"  build/{b}/{name} 落后于 {len(bad)} 个源文件"
                    f"（最新 {newest.relative_to(ROOT)}）——先 cmake --build 再跑闸")
    return problems


def judge_image(png: Path, baseline: Path | None) -> tuple[str, str]:
    """Return (verdict, detail) for one produced PNG against its baseline."""
    if not png.is_file():
        return "FAIL", "PNG 未生成"
    if baseline is None or not baseline.is_file():
        return "FAIL", f"基线缺失：{baseline}"
    if show.md5(png) == show.md5(baseline):
        return "bytes", "与基线逐字节相同"
    try:
        cmp = show.compare_pixels(png, baseline)
    except Exception as e:  # corrupt PNG (bad zlib stream etc.) is a FAIL, not a crash
        return "FAIL", f"基线或产物 PNG 无法解码：{type(e).__name__}: {e}"
    if not cmp.get("sameSize"):
        return "FAIL", "与基线尺寸不同"
    dp, md = cmp["diffPixels"], cmp["maxDelta"]
    if dp == 0:
        return "bytes", "MD5 不同但像素完全相同（PNG 编码差异）"
    if dp <= NEAR_MAX_PIXELS and md is not None and md <= NEAR_MAX_DELTA:
        return "near", f"GPU 取整噪声：{dp} 像素、maxDelta {md}/255"
    return "FAIL", f"与基线像素不符：{dp} 像素、maxDelta {md}/255"


def run_viewer(exe: Path, argv: list[str], cwd: Path | None = None) -> tuple[int, str]:
    """Launch one case and return (exit code, combined stdout+stderr).

    2026-11-13 S12 M12.1 收口实锤：**不能用 subprocess.run(capture_output=True)**。
    material-graph-tools 的 check / compilepar 会派生 `cmd /c slangc.exe` 孙进程，
    孙进程继承 stdout 管道写端；CPython 的 subprocess.run 超时后只 kill 直接子
    进程，随后再调一次**没有 timeout** 的 communicate() 等 EOF → 孙进程不退出
    就永久阻塞，RUN_TIMEOUT 形同虚设。实测全量挂 25 分钟、stdout 零新增、
    进程无子进程、报告未生成，孤儿 cmd 直到终止回归才消失。

    改为 Popen + 临时文件重定向：读日志不依赖管道 EOF，wait(timeout) 才真生效；
    stdin=DEVNULL 顺带断掉 cmd 继承输入句柄导致的挂起。超时 60s 内必被 kill、
    由 run_task 打 [STALL] 告警，不会再出现「静默挂死没人知道」。
    """
    with tempfile.TemporaryFile(mode="w+b") as logf:
        try:
            proc = subprocess.Popen(
                [str(exe), *argv],
                # 【2026-11-09】case 级 cwd 覆盖：默认 exe 所在目录；声明
                # "cwd": "{root}" 时改用仓库根（audio-tools debug 的快照存
                # 仓库根相对路径，必须仓库根 CWD 才能复开 clip）。
                cwd=str(cwd if cwd is not None else exe.parent),
                stdin=subprocess.DEVNULL,
                stdout=logf,
                stderr=subprocess.STDOUT,
            )
        except OSError as e:
            return -1, f"[regression] cannot launch {exe.name}: {e}"
        timed_out = False
        try:
            code = proc.wait(timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            code = -1
        logf.seek(0)
        log = logf.read().decode("utf-8", "replace")
        if timed_out:
            # marker 文本必须保持精确：regression_case 的「超时+零输出」自动重试
            # 判据是 log.strip() == f"[regression] timeout after {RUN_TIMEOUT}s"。
            log = log[-8000:] + f"\n[regression] timeout after {RUN_TIMEOUT}s"
        return code, log


def check_device_errors(log: str) -> int | None:
    m = re.findall(r"deviceErrors=(\d+)", log)
    return int(m[-1]) if m else None


def regression_case(version: dict, case: dict, backend: str, out_png: Path) -> dict:
    """Run one (version, case, backend) and return the verdict record."""
    rec: dict = {"version": version["id"], "case": case["id"], "backend": backend,
                 "kind": case.get("kind")}
    exe = show.exe_for(version, backend, case)
    if exe.is_file():
        # --resume 依据：exe 路径 + mtime 变过（重编）则该条沿用资格作废
        rec["exe"] = str(exe)
        rec["exeMtime"] = exe.stat().st_mtime
    if not exe.is_file():
        rec["verdict"] = "FAIL"
        rec["detail"] = (f"exe 缺失：{exe.relative_to(ROOT).as_posix()}"
                         f"（补建：python tools/showcase/rebuild.py --tags {version['tag']}）")
        return rec

    kind = case.get("kind")
    # 【2026-11-10】kind:tool + args 带 {out} 的工具用例（如 nav-m0/bake 烘焙
    # 输出 .nnav）：给临时产物路径让 argv 替换走通，判定只看 exit/expectLog，
    # 不进图像比对（out 是工具私有格式不是 PNG），跑完即删防垃圾堆积。
    # 此前 tool 一律 out=None → {out} 用例在参数解析层直接 FAIL（nav-m0 合入
    # master 后首次全量闸暴露）。
    if kind in ("image", "pair"):
        out = out_png
    elif kind == "tool" and "{out}" in case.get("args", []):
        out = out_png.with_suffix(".toolout")
        if out.exists():
            out.unlink()
    else:
        out = None
    try:
        params = show.validated_params(case, {})  # defaults = baseline params
        argv = show.build_argv(case, params, out)
    except ValueError as e:
        rec["verdict"] = "FAIL"
        rec["detail"] = f"用例参数解析失败：{e}"
        return rec

    started = time.time()
    # case 级 cwd 覆盖（与 showcase server.run_one 同语义）
    run_cwd = ROOT if case.get("cwd") == "{root}" else None
    code, log = run_viewer(exe, argv, run_cwd)
    # 【2026-11-09】Windows 并发拉起同镜像进程偶发「exit 0 + 空输出」（本机环境级
    # 抖动：单独重跑必过、双后端 bytes 一致；并发探针还复现过 0xC0000005）。
    # 对这种不可能签名——exit 等于预期 0 但 expectLog 用例拿到空日志——自动重试
    # 一次；仍失败才按原逻辑判 FAIL。
    if code == 0 and case.get("expectLog") and not log:
        code, log = run_viewer(exe, argv, run_cwd)
    # 同族抖动签名 2：超时（exit -1）且 marker 前零输出——selftest/文本用例启动
    # 即打印，120s 内一个字都没吐出来只可能是进程被并发环境卡死（M6.0 收口
    # 全量闸实测 s5.0/s5-selftest vulkan：单独复验 24ms 即 PASS）。同样重试一次。
    _timeout_marker = f"[regression] timeout after {RUN_TIMEOUT}s"
    if code == -1 and log.strip() == _timeout_marker:
        code, log = run_viewer(exe, argv, run_cwd)
    # 同族抖动签名 3（2026-11-10，M7.2 收口全量闸实测）：文本用例 fail-fast 崩溃码
    # —— 3221226505 (0xC0000409) / 3221225477 (0xC0000005)，进程秒崩（<350ms）。
    # 当晚主力成因后经追因实锤为 JobRuntime 析构次序竞争（MILESTONES M7.2 踩坑 8，
    # 已修复），但同签名还可能来自其他未知的进程启动期竞争，保留为兜底重试。
    # 仅对「预期 exit 0 的文本用例」重试一次——重试仍崩照 FAIL，闸门不放松；
    # 崩溃码若出现在渲染用例或预期非零用例上不重试（那些有真失败可能）。
    if (code in (3221226505, 3221225477) and case.get("expectExit", 0) == 0
            and not case.get("baselines")):
        code, log = run_viewer(exe, argv, run_cwd)
    rec["ms"] = int((time.time() - started) * 1000)
    rec["exit"] = code
    rec["deviceErrors"] = check_device_errors(log)

    # 工具用例（kind:tool）约定非零退出码可以是合法预期（usage=1、未实现=2），
    # 与展示台 server.py 的 expectExit 语义严格一致。
    expected = case.get("expectExit", 0)
    if code != expected:
        rec["verdict"] = "FAIL"
        rec["detail"] = (f"退出码 {code}（预期 {expected}）"
                         + ("（超时）" if code == -1 else ""))
        rec["logTail"] = log[-1500:]
        return rec
    if case.get("expectLog") and case["expectLog"] not in log:
        rec["verdict"] = "FAIL"
        rec["detail"] = f"日志缺 {case['expectLog']!r}"
        rec["logTail"] = log[-1500:]
        return rec
    if rec["deviceErrors"] not in (None, 0):
        rec["verdict"] = "FAIL"
        rec["detail"] = f"deviceErrors={rec['deviceErrors']}"
        rec["logTail"] = log[-1500:]
        return rec

    if out is None or kind == "tool":  # selftest / tool / 其他纯文本用例
        if out is not None and out.exists():
            out.unlink()
        rec["verdict"] = "bytes"
        label = "selftest 通过" if kind == "selftest" else "文本用例通过"
        rec["detail"] = f"{label}（exit {code}，{case.get('expectLog', 'ok')}）"
        return rec

    verdict, detail = judge_image(out, show.baseline_for(case, backend))
    rec["verdict"], rec["detail"] = verdict, detail
    if verdict != "FAIL":
        # resize-type cases also carry a pre-resize frame next to the main one.
        before = out.with_name(out.name + ".before.png")
        bb = show.before_baseline_for(case, backend)
        if bb and before.is_file():
            v2, d2 = judge_image(before, bb)
            rec["beforeVerdict"], rec["beforeDetail"] = v2, d2
            if v2 == "FAIL":
                rec["verdict"], rec["detail"] = "FAIL", f"before 图不符：{d2}"
        rec["md5"] = show.md5(out)
    return rec


def main() -> int:
    ap = argparse.ArgumentParser(description="nl-engine M0.9 full regression gate")
    ap.add_argument("--only", default=None, metavar="VERSION[/CASE]",
                    help="limit to one version (m0.5) or case (m0.5/nohud)")
    ap.add_argument("--backend", choices=BACKENDS, default=None)
    ap.add_argument("--jobs", type=int, default=4,
                    help="parallel cases (default 4; 1 = serial, 最保守)")
    ap.add_argument("--report", default=str(DEFAULT_REPORT_MD),
                    help="markdown report path (a .json sits next to it)")
    ap.add_argument("--resume", action="store_true",
                    help="沿用上次 report.json 里通过的 (version, case, backend)，"
                         "只重跑 FAIL / 缺失 / exe 变过的项（中断续跑）")
    args = ap.parse_args()

    backend_filter = (args.backend,) if args.backend else BACKENDS

    # ---- bat 编码门禁（M-GL4 固化，事前检查）：全部 bat = UTF-8 无 BOM + CRLF，
    # 含中文的头部必须 chcp 65001。违规即拒跑（LF+多字节曾实炸 gl4 run.bat）。
    bat_check = subprocess.run(
        [sys.executable, str(Path(__file__).parent / "check-bat-encoding.py")],
        capture_output=True, text=True)
    if bat_check.returncode != 0:
        print("[regression] bat 编码门禁未过（详见下述），拒绝运行：", file=sys.stderr)
        print(bat_check.stdout, file=sys.stderr)
        return 2

    if args.only:
        vid, _, cid = args.only.partition("/")
        versions = [v for v in show.DATA["versions"] if v["id"] == vid]
        if not versions:
            print(f"[regression] no version id {vid!r} in cases.json", file=sys.stderr)
            return 2
    else:
        cid = ""
        versions = show.DATA["versions"]

    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)

    # ---- exe 新鲜度门禁：所选版本里有跑「当前树 exe」的，exe 就必须新于源码 ----
    uses_current_exe = any(
        "showcase" not in (v.get("exe", {}).get(b, ""))
        for v in versions for b in backend_filter)
    if uses_current_exe:
        problems = check_exe_freshness(backend_filter)
        if problems:
            print("[regression] exe 过期门禁：以下 exe 落后于源码，拒绝运行：",
                  file=sys.stderr)
            print("\n".join(problems), file=sys.stderr)
            print("[regression] 重编后重跑（cmake --build build\\<backend>）。",
                  file=sys.stderr)
            return 2

    # ---- 任务表：以 (version, case) 为并行单元；同用例双后端串行 ----
    # 工具用例（kind:tool）写固定共享路径（如 build/qem/showcase-*，路径不含
    # 后端名），同用例并行会互相覆盖 → 用例间并行、用例内串行。
    tasks: list[tuple[dict, dict]] = []
    for v in versions:
        missing = [b for b in BACKENDS if b in backend_filter
                   and not show.exe_for(v, b).is_file()]
        if len(missing) == len(backend_filter):
            print(f"[regression] {v['id']}: 两后端 exe 都缺失，跳过该版本"
                  f"（{v['tag']}）")
        for c in v["cases"]:
            if c.get("kind") == "manual":
                continue
            if cid and c["id"] != cid:
                continue
            tasks.append((v, c))

    print_lock = threading.Lock()
    records: list[dict] = []
    t_all = time.time()

    # ---- --resume（2026-11-13 用户拍板）：全量闸中断后不再从头跑。读上次
    # report.json，verdict 为 bytes/near 且 exe 未变的 (version, case, backend)
    # 直接沿用；FAIL / 中断缺失 / exe 重编过（mtime 变）的重跑。--only 照常过滤。
    prev: dict[tuple[str, str, str], dict] = {}
    if args.resume:
        prev_json = Path(args.report).with_suffix(".json")
        if prev_json.is_file():
            try:
                prev_recs = json.loads(prev_json.read_text(encoding="utf-8")).get("records", [])
                prev = {(r["version"], r["case"], r["backend"]): r for r in prev_recs}
            except (json.JSONDecodeError, KeyError, TypeError, AttributeError):
                print(f"[regression] --resume: 上次报告 {prev_json.name} 无法解析，全部重跑",
                      file=sys.stderr)
        else:
            print(f"[regression] --resume: 无上次报告 {prev_json}，全部重跑", file=sys.stderr)

    def reusable(v: dict, c: dict, b: str) -> dict | None:
        r = prev.get((v["id"], c["id"], b))
        if not r or r.get("verdict") not in ("bytes", "near"):
            return None
        exe = show.exe_for(v, b, c)
        if not exe.is_file() or r.get("exe") != str(exe):
            return None
        if abs(r.get("exeMtime", -1.0) - exe.stat().st_mtime) > 1e-6:
            return None  # exe 重编过：旧通过不再可信
        return r

    reused_recs: list[dict] = []
    plan: list[tuple[dict, dict, list[str]]] = []
    for v, c in tasks:
        backends_run: list[str] = []
        for b in backend_filter:
            r = reusable(v, c, b)
            if r is not None:
                reused_recs.append(r)
            else:
                backends_run.append(b)
        plan.append((v, c, backends_run))
    n_reused = len(reused_recs)
    total = sum(len(bs) for _, _, bs in plan)
    if args.resume and prev:
        print(f"[regression] --resume: 沿用上次通过 {n_reused} 条，重跑 {total} 条",
              flush=True)
    if args.resume and n_reused:
        for r in reused_recs:
            print(f"[REUSE] {r['version']}/{r['case']} {r['backend']:<6} "
                  f"{r['verdict']:<5} {r.get('detail', '')}", flush=True)

    # 2026-11-13：可观测性三件套（起因 = 全量静默挂死 25 分钟没人发现）。
    # ① 每条完成即 flush，stdout 始终有增量；② 增量落盘 JSONL，整体卡死也能
    # 从尾部定位停在哪条；③ 每 50 条打进度 + 超时单独打 [STALL] 告警。
    progress_path = Path(args.report).with_suffix(".progress.jsonl")
    progress_path.write_text("", encoding="utf-8")
    done = [0]
    print(f"[regression] {total} runs "
          f"(timeout={RUN_TIMEOUT}s; progress={progress_path.as_posix()})", flush=True)

    def run_task(v: dict, c: dict, backends_run: list[str]) -> list[dict]:
        recs: list[dict] = []
        for b in backends_run:
            out = RUNS_DIR / f"{v['id']}-{c['id']}-{b}.png"
            for stale in (out, out.with_name(out.name + ".before.png")):
                if stale.exists():
                    stale.unlink()
            rec = regression_case(v, c, b, out)
            recs.append(rec)
            mark = "PASS" if rec["verdict"] in ("bytes", "near") else "FAIL"
            extra = f" ({rec['ms']} ms)" if "ms" in rec else ""
            with print_lock:
                print(f"[{mark}] {v['id']}/{c['id']} {b:<6} {rec['verdict']:<5} "
                      f"{rec['detail']}{extra}", flush=True)
                if rec.get("exit") == -1:
                    print(f"[STALL] {v['id']}/{c['id']} {b}: killed after "
                          f"{RUN_TIMEOUT}s, gate continues (see progress log)",
                          flush=True)
                with progress_path.open("a", encoding="utf-8") as pf:
                    pf.write(json.dumps(rec, ensure_ascii=False) + "\n")
                done[0] += 1
                if done[0] % 50 == 0 or done[0] == total:
                    print(f"[regression][progress] {done[0]}/{total} "
                          f"elapsed={time.time() - t_all:.0f}s", flush=True)
        return recs

    jobs = max(1, args.jobs)
    if jobs == 1:
        for v, c, bs in plan:
            if bs:
                records.extend(run_task(v, c, bs))
    else:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = [pool.submit(run_task, v, c, bs) for v, c, bs in plan if bs]
            for fut in as_completed(futs):
                records.extend(fut.result())
    records.extend(reused_recs)

    # 报告顺序与用例表一致（并行完成顺序不定，排序保证确定性）
    ver_order = {v["id"]: i for i, v in enumerate(show.DATA["versions"])}
    case_order = {(v["id"], c["id"]): i
                  for v in show.DATA["versions"] for i, c in enumerate(v["cases"])}
    b_order = {b: i for i, b in enumerate(BACKENDS)}
    records.sort(key=lambda r: (ver_order.get(r["version"], 999),
                                case_order.get((r["version"], r["case"]), 999),
                                b_order.get(r["backend"], 9)))

    # Cross-backend info tier: same case, do the two GPUs agree byte-wise?
    cross = []
    for v in versions:
        for c in v["cases"]:
            if c.get("kind") != "pair":
                continue
            recs = [r for r in records if r["version"] == v["id"] and r["case"] == c["id"]]
            md5s = {r["backend"]: r.get("md5") for r in recs if r.get("md5")}
            if "dx12" in md5s and "vulkan" in md5s:
                same = md5s["dx12"] == md5s["vulkan"]
                cross.append({"version": v["id"], "case": c["id"], "identical": same})
                if not same:
                    cmp = show.compare_pixels(RUNS_DIR / f"{v['id']}-{c['id']}-dx12.png",
                                              RUNS_DIR / f"{v['id']}-{c['id']}-vulkan.png")
                    cross[-1]["pixel"] = cmp

    passed = [r for r in records if r["verdict"] in ("bytes", "near")]
    failed = [r for r in records if r["verdict"] not in ("bytes", "near")]
    tiers = {t: sum(1 for r in records if r["verdict"] == t)
             for t in ("bytes", "near", "FAIL")}
    ok = not failed

    # ---- markdown report ----
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines = [
        "# nl-engine 全量回归报告（M0.9 总闸）", "",
        f"- 时间：{now}　用时 {int(time.time() - t_all)} s"
        + (f"（--resume：沿用上次通过 {n_reused} 条）" if n_reused else ""),
        f"- 结果：**{'ALL GREEN' if ok else 'FAILED'}** — "
        f"{len(passed)}/{len(records)} 通过"
        f"（bytes {tiers['bytes']} / near {tiers['near']} / FAIL {tiers['FAIL']}）",
        f"- 判据：PNG 与基线 MD5 相同 = `bytes`；像素差 ≤1/255 且 ≤2 像素 = `near`"
        f"（GPU 取整噪声档）；selftest 校验日志；deviceErrors=0。", "",
    ]
    if failed:
        lines += ["## 失败明细", ""]
        for r in failed:
            lines.append(f"- `{r['version']}/{r['case']}` {r['backend']}: {r['detail']}")
        lines.append("")
    lines += ["## 明细", "",
              "| 版本 | 用例 | 后端 | 判定 | 说明 |",
              "|---|---|---|---|---|"]
    for r in records:
        lines.append(f"| {r['version']} | {r['case']} | {r['backend']} | "
                     f"{r['verdict']} | {r['detail']} |")
    lines += ["", "## 双后端对照（信息档）", ""]
    for c in cross:
        tag = "逐字节相同" if c["identical"] else \
            f"不同（像素差 {c.get('pixel', {}).get('diffPixels', '?')}，maxDelta " \
            f"{c.get('pixel', {}).get('maxDelta', '?')}/255，HUD 文本差异属预期）"
        lines.append(f"- {c['version']}/{c['case']}: {tag}")
    Path(args.report).write_text("\n".join(lines) + "\n", encoding="utf-8")
    report_json = Path(args.report).with_suffix(".json")
    report_json.write_text(json.dumps(
        {"ok": ok, "when": now,
         "counts": {"passed": len(passed), "failed": len(failed), **tiers},
         "records": records, "crossBackend": cross},
        ensure_ascii=False, indent=1), encoding="utf-8")

    print(f"\n[regression] {len(passed)}/{len(records)} 通过"
          f"（bytes {tiers['bytes']} / near {tiers['near']} / FAIL {tiers['FAIL']}）"
          f" — {'ALL GREEN' if ok else 'FAILED'}，用时 {int(time.time() - t_all)} s")
    print(f"[regression] 报告：{args.report}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
