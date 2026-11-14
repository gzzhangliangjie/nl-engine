#!/usr/bin/env python3
"""M-WAT0 (water track) SSR acceptance gates for example/water-t0/run.bat.

Two checks, selected by the first argument:

  ab <off.png> <on.png> [mindiff_frac=0.005]
      --ssr must actually change the frame: at least mindiff_frac of pixels
      differ by more than the per-pixel threshold (sum of channel deltas > 6).
      A near-zero changed fraction means the compose pass is a no-op (broken
      march or weight always 0). Background pixels stay untouched (SSR weight
      is 0 on VIS_EMPTY), so the changed fraction is bounded well below 1.

  near <dx12.png> <vulkan.png> [maxfrac=0.02]
      Both backends must agree to within <maxfrac> of pixels (float march in
      screen space can flip a pixel at silhouette edges; a large divergence
      means the march/pyramid path diverged).

Exit 0 = gate passed, 1 = failed, 2 = usage.
"""
import sys
from PIL import Image


def changed_stats(a_path, b_path, thresh=6):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        sys.stderr.write(f"[ab] size mismatch {a.size} vs {b.size}\n")
        return None
    pa, pb = a.load(), b.load()
    w, h = a.size
    diff = 0
    maxd = 0
    for y in range(h):
        for x in range(w):
            r1, g1, b1 = pa[x, y]
            r2, g2, b2 = pb[x, y]
            d = abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2)
            if d > thresh:
                diff += 1
            if d > maxd:
                maxd = d
    return diff, w * h, maxd


def gate_ab(off_path, on_path, mindiff_frac):
    st = changed_stats(off_path, on_path)
    if st is None:
        return 2
    diff, total, maxd = st
    frac = diff / total
    print(f"[ab] ssr off vs on: changed px={diff} frac={frac:.4f} "
          f"(threshold {mindiff_frac}) maxChannelSumDiff={maxd}")
    if frac < float(mindiff_frac):
        sys.stderr.write(
            f"[ab] FAIL: only {frac:.4f} of pixels changed -- compose pass "
            f"is effectively a no-op\n")
        return 1
    print("[ab] OK: reflections visibly applied")
    return 0


def gate_near(dx12_path, vulkan_path, maxfrac):
    st = changed_stats(dx12_path, vulkan_path)
    if st is None:
        return 2
    diff, total, _ = st
    frac = diff / total
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
    if mode == "ab" and len(sys.argv) in (4, 5):
        mind = sys.argv[4] if len(sys.argv) == 5 else "0.005"
        return gate_ab(sys.argv[2], sys.argv[3], mind)
    if mode == "near" and len(sys.argv) in (4, 5):
        maxf = sys.argv[4] if len(sys.argv) == 5 else "0.02"
        return gate_near(sys.argv[2], sys.argv[3], maxf)
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
