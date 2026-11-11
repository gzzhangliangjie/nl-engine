#!/usr/bin/env python3
"""M10.2 / S10 -- s10-gather acceptance gate (final gather + ambient replace).

Q13-1/Q13-2/Q13-3 UE-style final gather:
  - per-pixel depth-banded cascade level (LINEAR depth thresholds 3/5/7 in CB;
    deviceZ banding was smoke-tested dead -- nonlinear squeeze -> all L3)
  - bilinear corner weights on the csPlace screen grid (grid pitch =
    viewport/res_l, NOT baseRes>>level -- M10.2 bug found by debug viz)
  - plane-depth weight exp2(-10000*(planeDist/sceneDepth)^2) + fallback
    exp2(-1000*...) two-stage degradation, MIN=0.01 (UE #define)
  - compose = de-gamma + gather linear + re-gamma; ambient reduced to a
    0.03 floor in the raster path via g_options.z=1 (2026-11-11 user call:
    pure zero left 3.66% dead black -- M10.1 probe GI has no bounces yet;
    0.03 floor = UE-style ambient base, S11 tightens the gate)

Modes:
  bbox <png> [min_px] [min_p5] [max_dead] -- model-region gates:
      - model pixels (non-clearColor) count >= 50000  (coverage)
      - model-region luminance p5 >= 18               (GI lift, no dead black)
      - dead-black (<10) fraction < 0.02
"""
import sys

import numpy as np
from PIL import Image

BG = np.array([13.0, 20.0, 38.0])   # clearColor (viewer default, matches baseline)


def bbox_gate(path: str, min_model_px: int, min_p5: float, max_dead: float) -> int:
    im = np.asarray(Image.open(path).convert("RGB"), dtype=float)
    lum = im.mean(axis=2)
    mask = np.abs(im - BG).sum(axis=2) > 25.0
    n_model = int(mask.sum())
    if n_model < min_model_px:
        print(f"[FAIL] model coverage {n_model} < {min_model_px}")
        return 1
    p5 = float(np.percentile(lum[mask], 5))
    dead = float((lum[mask] < 10.0).mean())
    if p5 < min_p5:
        print(f"[FAIL] model-region p5 {p5:.1f} < {min_p5} (GI lift missing)")
        return 1
    if dead > max_dead:
        print(f"[FAIL] dead-black fraction {dead:.4f} > {max_dead}")
        return 1
    print(f"[OK] bbox gate: model_px={n_model} p5={p5:.1f} dead={dead:.5f}")
    return 0


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mode = sys.argv[1]
    path = sys.argv[2]
    if mode == "bbox":
        min_px = int(sys.argv[3]) if len(sys.argv) > 3 else 50000
        min_p5 = float(sys.argv[4]) if len(sys.argv) > 4 else 18.0
        max_dead = float(sys.argv[5]) if len(sys.argv) > 5 else 0.02
        return bbox_gate(path, min_px, min_p5, max_dead)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
