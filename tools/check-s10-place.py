#!/usr/bin/env python3
"""M10.0 probe-place acceptance gates (machine assertions for example/s10-place/run.bat).

  dots <png> <minRed> <minGreen> <minBlue>
      Probe-dot coverage + near-dense/far-sparse evidence. The viz layer paints
      cascade-level colored dots (L0 red / L1 green / L2 blue / L3 yellow) and
      tones them down through the same tonemap as the scene, so thresholds are
      loose-signature (red: r>200 && g<130 && b<130; green: g>200 && r<130 &&
      b<160; blue: b>200 && r<130 && g<180) to avoid matching Suzanne's warm
      highlights. L3 (2x2 grid) is NOT asserted: at this camera distance the 4
      coarsest probes can legitimately all land on sky (culled, a=0) - grid
      phase vs. model silhouette, not a pipeline bug (verified 2026-11-11).
      Also asserts red > green > blue (pixel counts shrink per level: same dot
      size, fewer probes per level => fewer lit pixels = near-dense/far-sparse).

Exit 0 = gate passed, 1 = failed, 2 = usage.
"""
import sys
from PIL import Image


def classify(p):
    r, g, b = p
    if r > 200 and g < 130 and b < 130:
        return "red"
    if g > 200 and r < 130 and b < 160:
        return "green"
    if b > 200 and r < 130 and g < 180:
        return "blue"
    if r > 230 and g > 210 and b < 120:
        return "yellow"
    return None


def gate_dots(path, min_red, min_green, min_blue):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    p = im.load()
    counts = {"red": 0, "green": 0, "blue": 0, "yellow": 0}
    for y in range(h):
        for x in range(w):
            c = classify(p[x, y])
            if c:
                counts[c] += 1
    print(f"[dots] {path}: red={counts['red']} green={counts['green']} "
          f"blue={counts['blue']} yellow={counts['yellow']} (yellow not asserted)")

    fails = []
    if counts["red"] < min_red:
        fails.append(f"red {counts['red']} < {min_red} (L0 probes missing)")
    if counts["green"] < min_green:
        fails.append(f"green {counts['green']} < {min_green} (L1 probes missing)")
    if counts["blue"] < min_blue:
        fails.append(f"blue {counts['blue']} < {min_blue} (L2 probes missing)")
    if not (counts["red"] > counts["green"] > counts["blue"]):
        fails.append(f"near-dense/far-sparse violated: "
                     f"red {counts['red']} green {counts['green']} blue {counts['blue']} "
                     f"must shrink per cascade level")
    if fails:
        for f in fails:
            sys.stderr.write(f"[dots] FAIL: {f}\n")
        return 1
    print("[dots] OK: coverage + near-dense/far-sparse ordering passed")
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "dots" and len(sys.argv) == 6:
        return gate_dots(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
