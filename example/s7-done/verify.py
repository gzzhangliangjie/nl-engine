"""M7.2 (S7.2) acceptance re-run: multi-material indirect table, vis vs direct.

Checks (all backends, all deterministic screenshots):
  1) materials-pulling / materials-normalref shots are byte-identical to their
     baselines;
  2) normal-reference is byte-identical across backends; pulling across
     backends is near-tier (analytic derivatives vs hardware quad derivatives
     differ by rounding only);
  3) foreground A/B per backend: --mode pulling (vis buffer -> clusterMeta[3]
     materialId -> GPUMaterial indirect table) vs traditional --mode normal
     (per-prim batches, same shading formula in model.slang). Equivalence, not
     bit-equality: silhouette must match exactly, nearly all pixels identical,
     no systematic bias (a wrong material table would swap tint/roughness and
     blow the bias).
     Measured on the baseline pair (960x540, 42498 fg px, 3 materials):
       99.991% delta<=2, isolated tail to 4, mean |bias| 0.0065.

Run from anywhere:  python example/s7-done/verify.py
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
          "--yaw", "12", "--pitch", "9", "--dist", "9"]
SHOTS = {
    "materials-pulling": ["--mode", "pulling", "--shading", "5",
                          "--model", "Suzanne-materials.nmesh"],
    "materials-normalref": ["--mode", "normal", "--shading", "5",
                            "--model", "Suzanne-materials.glb"],
}
# pulling vs normal-reference: silhouette strict; 99.5% of fg px within 2;
# isolated sample/mip tail capped at 8 (baseline measured 4); mean |bias|<=0.05.
AB_MAX_SIL = 0
AB_FRAC_DELTA2 = 0.995
AB_MAX_DELTA = 8
AB_MAX_MEAN_BIAS = 0.05
# pulling cross-backend: rounding-only (baseline measured: tail to 10,
# 99.998% <= 2).
XB_MAX_COLOR = 16
XB_MAX_DELTA = 16


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
            out = RUNS / f"s7-done-{shot}-{backend}.png"
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

    # Cross-backend: normal-reference is near-tier too（PBR mode 5 的 pow 舍入
    # 跨后端尾，基线实测 4 px / maxDelta 10 / 7 bytes——pbr-direct 的 scene/scale
    # 同族口径：跨后端 near、各自对本后端基线 bytes）；pulling 同为 near-tier
    # （analytic vs hardware derivative rounding，基线实测尾到 10）。
    nrm = [produced.get(("dx12", "materials-normalref")),
           produced.get(("vulkan", "materials-normalref"))]
    if all(nrm):
        st = foreground_compare(nrm[0], nrm[1])
        if st and st["sil"] == 0 and st["maxd"] <= XB_MAX_DELTA \
                and (st["fg"] - st["le2"]) <= XB_MAX_COLOR:
            print(f"[PASS] materials-normalref: DX12 == VULKAN near "
                  f"(sil=0, color>2px: {st['fg'] - st['le2']}, "
                  f"maxDelta={st['maxd']})")
        else:
            print(f"[FAIL] materials-normalref cross-backend: {st}")
            bad += 1
    pol = [produced.get(("dx12", "materials-pulling")),
           produced.get(("vulkan", "materials-pulling"))]
    if all(pol):
        st = foreground_compare(pol[0], pol[1])
        if st and st["sil"] == 0 and st["maxd"] <= XB_MAX_DELTA \
                and (st["fg"] - st["le2"]) <= XB_MAX_COLOR:
            print(f"[PASS] materials-pulling: DX12 == VULKAN near "
                  f"(sil=0, color>{2}px: {st['fg'] - st['le2']}, "
                  f"maxDelta={st['maxd']})")
        else:
            print(f"[FAIL] materials-pulling cross-backend: {st}")
            bad += 1

    # Foreground A/B per backend: vis resolve vs traditional direct draw.
    for backend in EXES:
        a = produced.get((backend, "materials-pulling"))
        b = produced.get((backend, "materials-normalref"))
        if not (a and b):
            continue
        st = foreground_compare(a, b)
        frac2 = (st["le2"] / st["fg"]) if st and st["fg"] else 0.0
        if st and st["sil"] == AB_MAX_SIL and st["maxd"] <= AB_MAX_DELTA \
                and frac2 >= AB_FRAC_DELTA2 and st["bias"] <= AB_MAX_MEAN_BIAS:
            print(f"[PASS] {backend} A/B: vis multi-material == direct draw "
                  f"(sil=0, delta<=2: {frac2 * 100:.3f}%, maxDelta={st['maxd']}, "
                  f"bias={st['bias']:.4f})")
        else:
            print(f"[FAIL] {backend} A/B: {st} frac2={frac2:.5f}")
            bad += 1

    print()
    if bad == 0:
        print("verify: ALL GREEN")
        return 0
    print(f"verify: FAIL ({bad} check(s))")
    return 1


if __name__ == "__main__":
    sys.exit(main())
