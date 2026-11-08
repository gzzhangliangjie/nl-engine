#!/usr/bin/env python3
"""Rebuild the historical viewer.exe snapshots used by the S0 showcase.

build/ is gitignored, so a fresh clone only has the baseline PNGs. Running this
script checks out each milestone tag into build/showcase/<tag>/, restores the
gitignored Slang toolchain via an NTFS junction, then builds both backends with
that tag's own build script. Existing exes are kept unless --force is given.

    python tools/showcase/rebuild.py                 # missing exes only
    python tools/showcase/rebuild.py --tags s0-triangle --force
    python tools/showcase/rebuild.py --backend dx12

Prerequisites: MSVC 14.38 + Windows SDK 10.0.26100 + the portable CMake at
E:/UE_STUDY_2026/.build-tools/ (all encoded in scripts/build-m0.2.bat), and
thirdparty/slang/dist present in the main worktree.
"""
from __future__ import annotations

import argparse
import io
import json
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORK = ROOT / "build" / "showcase"
BACKENDS = {"dx12": "DX12", "vulkan": "VULKAN"}

with (ROOT / "example" / "cases.json").open(encoding="utf-8") as f:
    DATA = json.load(f)


def log(msg: str) -> None:
    print(msg, flush=True)


def checkout(tag: str, dest: Path) -> None:
    if dest.exists():
        return
    dest.mkdir(parents=True)
    log(f"  git archive {tag} -> {dest.relative_to(ROOT).as_posix()}")
    raw = subprocess.run(["git", "archive", tag], cwd=ROOT, check=True,
                         stdout=subprocess.PIPE).stdout
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as tf:
        for member in tf.getmembers():
            target = (dest / member.name).resolve()
            if not str(target).startswith(str(dest.resolve())):
                raise RuntimeError(f"unsafe path in archive: {member.name}")
        tf.extractall(dest)

    # s0-init predates the stage-neutral build script; reuse the current one.
    script = dest / "scripts" / "build-m0.2.bat"
    if not script.is_file():
        shutil.copy2(ROOT / "scripts" / "build-m0.2.bat", script)
        log("  injected scripts/build-m0.2.bat (tag predates it)")

    # Slang dist is gitignored. Link the main worktree's copy instead of
    # duplicating ~180 MB per tag. cmd's mklink avoids MSYS path mangling.
    slang = dest / "thirdparty" / "slang"
    if not slang.exists():
        target = ROOT / "thirdparty" / "slang"
        if target.is_dir():
            subprocess.run(["cmd", "/c", "mklink", "/J", str(slang), str(target)],
                           check=True, stdout=subprocess.DEVNULL)
            log("  junction thirdparty/slang -> main worktree")


# ---------------------------------------------------------------------------
# 隐藏窗口补丁（2026-11-08）：tag 快照源码本无此修复，截图模式建可见窗口会
# 抢前台焦点（~0.85 s/用例，--jobs 并行跑闸打断打字）。这里给 tag 源码注入
# 与 master commit dd0130a 等价的最小三处改动——不改任何渲染行为，基线像素
# 零影响（回归 bytes 验证）。幂等：已打过补丁的目录直接跳过。
# s0-init（m0.1）不适用：该版本无 --screenshot，且其 window 用例是 manual，
# 回归不会建窗口。
# ---------------------------------------------------------------------------
def apply_hidden_window_patch(dest: Path, tag: str) -> None:
    main_cpp = dest / "viewer" / "main.cpp"
    if not main_cpp.is_file():
        return  # s0-init 等无截图模式/源码布局不同的 tag，跳过
    src = main_cpp.read_text(encoding="utf-8")
    if "desc.visible" in src:
        return  # 已打补丁

    hpp = dest / "core" / "include" / "nl" / "core" / "window.hpp"
    cpp = dest / "core" / "src" / "window.cpp"
    text_hpp = hpp.read_text(encoding="utf-8")
    text_cpp = cpp.read_text(encoding="utf-8")

    a1 = "    bool        resizable = true;\n"
    assert text_hpp.count(a1) == 1, f"{tag}: window.hpp anchor not unique"
    hpp.write_text(text_hpp.replace(
        a1, a1 + "    bool        visible = true;  // false = 隐藏窗口（截图回归防抢前台焦点）\n"),
        encoding="utf-8")

    a2 = "    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);\n"
    assert text_cpp.count(a2) == 1, f"{tag}: window.cpp anchor not unique"
    cpp.write_text(text_cpp.replace(
        a2, a2 + "    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);\n"),
        encoding="utf-8")

    a3 = "    nl::core::Window window(desc);\n"
    assert src.count(a3) == 1, f"{tag}: main.cpp anchor not unique"
    main_cpp.write_text(src.replace(
        a3, "    desc.visible = !o.screenshot;  // 截图回归建隐藏窗口，防抢前台焦点\n" + a3),
        encoding="utf-8")
    log("  patched hidden-window fix (dd0130a equivalent)")


def exe_path(version: dict, backend: str) -> Path:
    tmpl = version["exe"][backend]
    return ROOT / tmpl.replace("{tag}", version["tag"])


def build(version: dict, backend: str, log_path: Path) -> bool:
    tag = version["tag"]
    dest = WORK / tag
    bat = dest / "scripts" / "build-m0.2.bat"
    if not bat.is_file():
        log(f"  [FAIL] {tag}: no build script at {bat}")
        return False
    log(f"  building {tag} [{BACKENDS[backend]}] …")
    proc = subprocess.run(["cmd", "/c", str(bat), BACKENDS[backend]],
                          cwd=str(dest), capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    log_path.write_text((proc.stdout or "") + (proc.stderr or ""), encoding="utf-8")
    if proc.returncode != 0:
        log(f"  [FAIL] {tag} {backend}: exit={proc.returncode}, see {log_path.name}")
        for line in (proc.stdout or "").splitlines()[-12:]:
            log("    " + line)
        return False
    log(f"  [OK] {tag} {backend}: {exe_path(version, backend).relative_to(ROOT).as_posix()}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tags", nargs="*", help="limit to these tags")
    ap.add_argument("--backend", choices=["dx12", "vulkan", "all"], default="all")
    ap.add_argument("--force", action="store_true", help="rebuild even if exe exists")
    args = ap.parse_args()

    backends = list(BACKENDS) if args.backend == "all" else [args.backend]
    versions = [v for v in DATA["versions"]
                if "showcase" in v["exe"]["dx12"]]  # skip M0.4 (built from HEAD)
    if args.tags:
        versions = [v for v in versions if v["tag"] in args.tags]

    failed = 0
    for v in versions:
        log(f"\n=== {v['label']} {v['tag']} ===")
        need = [b for b in backends if args.force or not exe_path(v, b).is_file()]
        if not need:
            log("  both backends already built, nothing to do (use --force)")
            continue
        checkout(v["tag"], WORK / v["tag"])
        apply_hidden_window_patch(WORK / v["tag"], v["tag"])
        for b in need:
            if not build(v, b, WORK / v["tag"] / f"build-{b}.log"):
                failed += 1
    log(f"\nDone. {failed} build(s) failed." if failed else "\nDone. All requested snapshots built.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
