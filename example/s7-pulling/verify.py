"""M7.1 (S7.1) acceptance re-run: vis buffer material resolve vs direct draw.

Checks (all backends, all deterministic screenshots):
  1) pulling / normal-reference shots are byte-identical to their baselines;
  2) normal-reference is byte-identical across backends; pulling across
     backends is near-tier (analytic derivatives vs hardware quad derivatives
     differ by rounding only);
  3) foreground A/B: --mode pulling vs traditional --mode normal --shading 0.
     The shading formula is byte-for-byte the same, but attribute delivery is
     not: pulling reconstructs screen-space barycentrics + analytic uv
     derivatives and samples with SampleGrad, the traditional path uses
     hardware interpolation + implicit mip selection. Expect equivalence, not
     bit-equality: silhouette must match exactly, most pixels identical, no
     systematic bias (a light/color mismatch would shift every pixel).
     Measured on the baseline pair (960x540, 64203 fg px):
       73.2% delta==0, 98.9% <=1, 99.9% <=4, isolated tail to 15.

Run from anywhere:  python example/s7-pulling/verify.py
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
    "pulling": ["--mode", "pulling"],
    "normal-reference": ["--mode", "normal", "--shading", "0"],
}
# pulling vs normal-reference: silhouette strict; 99.5% of fg px within 2;
# isolated sample/mip tail capped at 16 (1/16 of full scale); mean |bias|<=1.
AB_MAX_SIL = 0
AB_FRAC_DELTA2 = 0.995
AB_MAX_DELTA = 16
AB_MAX_MEAN_BIAS = 1.0
# pulling cross-backend: rounding-only (baseline measured: 23 px, maxDelta 1).
XB_MAX_COLOR = 64
XB_MAX_DELTA = 2


def md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


def foreground_compare(a: Path, b: Path) -> dict:
    """Foreground A/B stats keyed by each image's own background color."""
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return {}
    pa, pb = ia.load(), ib.load()
    w, h = ia.size
    bg_a, bg_b = pa[0, 0], pb[0, 0]
    sil = 0
    fg = 0
    hist: dict[int, int] = {}
    sum_a = 0
    sum_b = 0
    for y in range(h):
        for x in range(w):
            fa, fb = pa[x, y] != bg_a, pb[x, y] != bg_b
            if fa != fb:
                sil += 1
                continue
            if not fa:
                continue
            fg += 1
            d = max(abs(pa[x, y][k] - pb[x, y][k]) for k in range(3))
            hist[d] = hist.get(d, 0) + 1
            sum_a += sum(pa[x, y])
            sum_b += sum(pb[x, y])
    le2 = sum(v for d, v in hist.items() if d <= 2)
    maxd = max(hist) if hist else 0
    mean_bias = (abs(sum_a - sum_b) / (3.0 * fg)) if fg else 0.0
    return {"sil": sil, "fg": fg, "maxd": maxd, "le2": le2, "bias": mean_bias}


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
            out = RUNS / f"s7-pulling-{shot}-{backend}.png"
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

    # Cross-backend: normal-reference must be bytes-identical (same as M0.4);
    # pulling is near-tier (analytic vs hardware derivative rounding).
    nrm = [produced.get(("dx12", "normal-reference")),
           produced.get(("vulkan", "normal-reference"))]
    if all(nrm):
        if md5(nrm[0]) == md5(nrm[1]):
            print("[PASS] normal-reference: DX12 == VULKAN (bytes)")
        else:
            print("[FAIL] normal-reference: DX12 != VULKAN (bytes)")
            bad += 1
    pol = [produced.get(("dx12", "pulling")), produced.get(("vulkan", "pulling"))]
    if all(pol):
        st = foreground_compare(pol[0], pol[1])
        if st and st["sil"] == 0 and st["maxd"] <= XB_MAX_DELTA \
                and (st["fg"] - st["le2"]) <= XB_MAX_COLOR:
            print(f"[PASS] pulling: DX12 == VULKAN near "
                  f"(sil=0, color>{XB_MAX_DELTA}px: {st['fg'] - st['le2']}, "
                  f"maxDelta={st['maxd']})")
        else:
            print(f"[FAIL] pulling cross-backend: {st}")
            bad += 1

    # Foreground A/B per backend: vis resolve vs traditional direct draw.
    for backend in EXES:
        a = produced.get((backend, "pulling"))
        b = produced.get((backend, "normal-reference"))
        if not (a and b):
            continue
        st = foreground_compare(a, b)
        frac_ok = (st["le2"] / st["fg"]) >= AB_FRAC_DELTA2 if st["fg"] else False
        if st and st["sil"] == AB_MAX_SIL and frac_ok \
                and st["maxd"] <= AB_MAX_DELTA and st["bias"] <= AB_MAX_MEAN_BIAS:
            print(f"[PASS] {backend}: pulling vs normal foreground equivalent "
                  f"(sil=0, {100 * st['le2'] / st['fg']:.2f}% within 2, "
                  f"maxDelta={st['maxd']}, meanBias={st['bias']:.3f})")
        else:
            print(f"[FAIL] {backend}: pulling vs normal {st}")
            bad += 1

    print("[M7.1 verify] " + ("ALL GREEN" if bad == 0 else f"{bad} FAILURE(S)"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
