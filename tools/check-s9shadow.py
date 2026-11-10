#!/usr/bin/env python3
"""M9.1 shadow-ray acceptance gates (machine assertions for example/s9-shadow/run.bat).

Two checks, selected by the first argument:

  darken <m91.png> <m90_baseline.png>
      Shadow rays can ONLY attenuate a pixel (occlusion multiplies radiance by 0
      for the blocked case, ambient is preserved). Comparing the M9.1 render
      (rays on) against the M9.0 no-ray baseline (same model/params) must yield
      ZERO pixels that got brighter. Any brightened pixel is a ray/shading bug.
      Also reports the darker-pixel count so the reviewer sees real shadowing.

  near <dx12.png> <vulkan.png> <maxfrac>
      Both backends must agree to within <maxfrac> of pixels (float->UNORM
      rounding can flip a pixel or two). A large divergence means the shadow
      ray path diverged between DX12 and Vulkan.

Exit 0 = gate passed, 1 = failed, 2 = usage.
"""
import sys
from PIL import Image


def luminance(px):
    r, g, b = px
    return 0.299 * r + 0.587 * g + 0.114 * b


def gate_darken(m91_path, m90_path):
    a = Image.open(m91_path).convert("RGB")    # M9.1: shadow rays ON
    b = Image.open(m90_path).convert("RGB")    # M9.0 baseline: no rays
    if a.size != b.size:
        sys.stderr.write(f"[darken] size mismatch {a.size} vs {b.size}\n")
        return 2
    pa, pb = a.load(), b.load()
    w, h = a.size
    brighter = darker = 0
    for y in range(h):
        for x in range(w):
            d = luminance(pa[x, y]) - luminance(pb[x, y])
            if d > 3.0:
                brighter += 1
            elif d < -3.0:
                darker += 1
    print(f"[darken] M9.1 vs M9.0 baseline: brighter={brighter} "
          f"darker={darker} total={w * h}")
    if brighter > 0:
        sys.stderr.write(
            f"[darken] FAIL: {brighter} pixels got BRIGHTER under shadow rays "
            f"(shadows must only attenuate, never add light)\n")
        return 1
    if darker == 0:
        sys.stderr.write("[darken] FAIL: zero shadowed pixels -- rays may not "
                         "be casting\n")
        return 1
    print("[darken] OK: zero brightened pixels (shadow rays only attenuate); "
          f"{darker} px shadowed")
    return 0


def gate_near(dx12_path, vulkan_path, maxfrac):
    a = Image.open(dx12_path).convert("RGB")
    b = Image.open(vulkan_path).convert("RGB")
    if a.size != b.size:
        sys.stderr.write(f"[near] size mismatch {a.size} vs {b.size}\n")
        return 2
    pa, pb = a.load(), b.load()
    w, h = a.size
    diff = 0
    for y in range(h):
        for x in range(w):
            r1, g1, b1 = pa[x, y]
            r2, g2, b2 = pb[x, y]
            if abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2) > 6:
                diff += 1
    frac = diff / (w * h)
    print(f"[near] dx12 vs vulkan: diff pixels={diff} frac={frac:.4f} "
          f"maxfrac={maxfrac}")
    if frac > float(maxfrac):
        sys.stderr.write(
            f"[near] FAIL: dx12/vulkan divergence {frac:.4f} > {maxfrac}\n")
        return 1
    print("[near] OK: backends agree within tolerance")
    return 0


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    mode = sys.argv[1]
    if mode == "darken" and len(sys.argv) == 4:
        return gate_darken(sys.argv[2], sys.argv[3])
    if mode == "near" and len(sys.argv) == 5:
        return gate_near(sys.argv[2], sys.argv[3], sys.argv[4])
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
