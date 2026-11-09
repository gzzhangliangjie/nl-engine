"""M7.0 (S7.0) acceptance re-run: visibility buffer decode vs direct draw.

Checks (all backends, all deterministic screenshots):
  1) visbuf cluster / triangle shots are byte-identical to their baselines;
  2) DX12 and Vulkan produce byte-identical PNGs for each shot;
  3) foreground of visbuf-cluster and traditional `--mode cull --frustum off
     --shading 4` is pixel-identical (same pcg hash, same occlusion winner).
     Background differs on purpose: viz pass overwrites the clear color.

Run from anywhere:  python example/s7-visbuf/verify.py
Exit 0 = all green, 1 = any mismatch, 2 = viewer.exe missing.
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
RUNS = ROOT / "build" / "showcase" / "runs"
EXES = {"dx12": ROOT / "build" / "DX12" / "viewer.exe",
        "vulkan": ROOT / "build" / "VULKAN" / "viewer.exe"}
COMMON = ["--frames", "8", "--no-hud", "--size", "960x540",
          "--yaw", "12", "--pitch", "9", "--dist", "4.2"]
SHOTS = {
    "cluster": ["--mode", "visbuf", "--vis-viz", "cluster"],
    "triangle": ["--mode", "visbuf", "--vis-viz", "triangle"],
    "cull-reference": ["--mode", "cull", "--frustum", "off", "--shading", "4"],
}


def md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


def foreground_compare(a: Path, b: Path) -> tuple[int, int, int]:
    """(silhouette mismatches, foreground color mismatches, max channel delta)."""
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return (-1, -1, -1)
    pa, pb = ia.load(), ib.load()
    w, h = ia.size
    bg_a, bg_b = pa[0, 0], pb[0, 0]
    sil, color, maxd = 0, 0, 0
    for y in range(h):
        for x in range(w):
            fa, fb = pa[x, y] != bg_a, pb[x, y] != bg_b
            if fa != fb:
                sil += 1
            elif fa and pa[x, y] != pb[x, y]:
                color += 1
                maxd = max(maxd, *(abs(pa[x, y][k] - pb[x, y][k]) for k in range(3)))
    return sil, color, maxd


def main() -> int:
    missing = [str(p) for p in EXES.values() if not p.is_file()]
    if missing:
        print("[FAIL] viewer.exe missing: " + ", ".join(missing))
        print("       build first: scripts\\build-m0.2.bat DX12 / VULKAN")
        return 2
    RUNS.mkdir(parents=True, exist_ok=True)
    bad = 0
    produced: dict[tuple[str, str], Path] = {}
    for backend, exe in EXES.items():
        for shot, extra in SHOTS.items():
            out = RUNS / f"s7-visbuf-{shot}-{backend}.png"
            cmd = [str(exe), "--screenshot", str(out), *COMMON, *extra]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0 or not out.is_file():
                print(f"[FAIL] {backend}/{shot}: viewer exit {r.returncode}")
                bad += 1
                continue
            produced[(backend, shot)] = out
            base = HERE / f"{shot}-{backend}.png"
            if md5(out) != md5(base):
                print(f"[FAIL] {backend}/{shot}: baseline MD5 differs")
                bad += 1
            else:
                print(f"[PASS] {backend}/{shot}: bytes == baseline")

    for shot in SHOTS:
        d, v = produced.get(("dx12", shot)), produced.get(("vulkan", shot))
        if d and v:
            if md5(d) == md5(v):
                print(f"[PASS] {shot}: DX12 == VULKAN (bytes)")
            else:
                print(f"[FAIL] {shot}: DX12 != VULKAN")
                bad += 1

    for backend in EXES:
        a, b = produced.get((backend, "cluster")), produced.get((backend, "cull-reference"))
        if not (a and b):
            continue
        sil, color, maxd = foreground_compare(a, b)
        if sil == 0 and color == 0 and maxd == 0:
            print(f"[PASS] {backend}: visbuf vs direct draw foreground identical "
                  "(silhouette=0, color=0, maxDelta=0)")
        else:
            print(f"[FAIL] {backend}: silhouette={sil} color={color} maxDelta={maxd}")
            bad += 1

    print("[M7.0 verify] " + ("ALL GREEN" if bad == 0 else f"{bad} FAILURE(S)"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
