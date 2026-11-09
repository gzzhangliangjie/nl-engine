#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M-E1b.4 shadow quantitative check.

Invariant: turning on one directional light's shadow can only multiply its
contribution by shadowFactor in [0,1] -- every output pixel must be equal or
darker than the no-shadow baseline (same camera / materials / lights), and a
healthy shadow must darken a significant number of pixels.

Usage: python check-shadow.py <baseline.png> <shadow.png> [min_dark_pixels]
Exit 0 = pass, 1 = fail. ASCII output only.
"""
import sys
from PIL import Image


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: check-shadow.py <baseline.png> <shadow.png> [min_dark_pixels]")
        return 2
    min_dark = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    base = Image.open(sys.argv[1]).convert("RGB")
    shad = Image.open(sys.argv[2]).convert("RGB")
    if base.size != shad.size:
        print(f"FAIL size mismatch: {base.size} vs {shad.size}")
        return 1
    bp, sp = base.load(), shad.load()
    w, h = base.size
    darker, brighter = 0, 0
    max_gain = 0
    for y in range(h):
        for x in range(w):
            b, s = bp[x, y], sp[x, y]
            diff = (s[0] - b[0]) + (s[1] - b[1]) + (s[2] - b[2])
            if diff < -3:
                darker += 1
            elif diff > 3:
                brighter += 1
                max_gain = max(max_gain, diff)
    print(f"pixels={w*h} darker={darker} brighter={brighter} min_dark={min_dark}")
    if brighter > 0:
        print(f"FAIL {brighter} pixels got BRIGHTER (shadow must only darken); max_gain={max_gain}")
        return 1
    if darker < min_dark:
        print(f"FAIL only {darker} pixels darkened (need >= {min_dark}) -- shadow likely absent")
        return 1
    print(f"PASS shadow darkens {darker} px, none brighter")
    return 0


if __name__ == "__main__":
    sys.exit(main())
