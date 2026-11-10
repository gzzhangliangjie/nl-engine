#!/usr/bin/env python3
# P8 M-CAM3a evidence: CinematicCamera playback scenario from real cine-trace
# TSV output (no hand-drawn diagrams — tool-card evidence rule).
# Deterministic: same TSV -> byte-identical PNG (run.bat fc /b gate).
#
# usage: python tools/make-cam-3a-evidence.py <dir-with-tsv> <out.png>

import io
import math
import sys

from PIL import Image, ImageDraw

W, H = 960, 540
BG = (18, 20, 26)
PANEL = (28, 31, 40)
PLAY_COL = (120, 220, 180)
PAUSE_COL = (90, 96, 110)
GRID = (45, 50, 62)
TXT = (200, 205, 215)
RED = (255, 90, 90)


def read_rows(path):
    rows = []
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("frame\t"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) != 7:
                continue
            rows.append((int(p[0]), float(p[1]),
                         float(p[2]), float(p[3]), float(p[4]),
                         float(p[5]), int(p[6])))
    return rows


def main():
    if len(sys.argv) != 3:
        print("usage: make-cam-3a-evidence.py <tsv-dir> <out.png>")
        return 2
    rows = read_rows(sys.argv[1].rstrip("/\\") + "/cine-trace.tsv")
    assert len(rows) == 90, "expected 90 cine-trace rows, got %d" % len(rows)

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((16, 10), "P8 M-CAM3a  CinematicCamera playback trace (90 frames, dt=1/60)",
           fill=TXT)
    d.text((16, 28), "speed 1 -> pause 5f -> speed 2 -> loop wrap at f64 (second lap)",
           fill=(150, 156, 168))

    # ---- panel 1: eye top view (x-z), skip nan (pause) rows ----
    px, py, pw, ph = 60, 60, 560, 400
    d.rectangle([px, py, px + pw, py + ph], fill=PANEL)
    pts = [(r[2], r[4]) for r in rows if r[6] == 1 and
           not math.isnan(r[2]) and not math.isnan(r[4])]
    xs = [p[0] for p in pts]
    zs = [p[1] for p in pts]
    xmin, xmax, zmin, zmax = min(xs), max(xs), min(zs), max(zs)
    xpad = max((xmax - xmin) * 0.08, 0.5)
    zpad = max((zmax - zmin) * 0.08, 0.5)
    xmin, xmax, zmin, zmax = xmin - xpad, xmax + xpad, zmin - zpad, zmax + zpad

    def mx(x):
        return px + (x - xmin) / (xmax - xmin) * pw

    def mz(z):
        return py + ph - (z - zmin) / (zmax - zmin) * ph

    for gx in range(5):
        d.line([px + pw * gx / 4, py, px + pw * gx / 4, py + ph], fill=GRID)
    for gy in range(5):
        d.line([px, py + ph * gy / 4, px + pw, py + ph * gy / 4], fill=GRID)
    d.text((px + 8, py + 8), "eye top view (X right, Z up) - teal=playing path",
           fill=(150, 156, 168))
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        if a[6] != 1 or b[6] != 1 or math.isnan(a[2]) or math.isnan(b[2]):
            continue
        d.line([mx(a[2]), mz(a[4]), mx(b[2]), mz(b[4])], fill=PLAY_COL, width=3)
    # keyframes k0..k3 markers
    for (kx, kz) in ((0, 6), (4, 6), (4, 2), (0, 2)):
        d.ellipse([mx(kx) - 4, mz(kz) - 4, mx(kx) + 4, mz(kz) + 4],
                  outline=(250, 190, 60), width=2)
    # wrap marker f64
    r = rows[64]
    d.ellipse([mx(r[2]) - 6, mz(r[4]) - 6, mx(r[2]) + 6, mz(r[4]) + 6],
              outline=RED, width=2)
    d.text((mx(r[2]) + 10, mz(r[4]) + 8), "f64 wrap", fill=RED)
    d.text((px, py + ph + 6),
           "X %.2f..%.2f  Z %.2f..%.2f  (orange = keyframes)" %
           (xmin + xpad, xmax - xpad, zmin + zpad, zmax - zpad),
           fill=(110, 116, 128))

    # ---- panel 2: state band + progress timeline + fov curve ----
    bx1, by1, bx2, by2 = 660, 60, 940, 460
    d.rectangle([bx1, by1, bx2, by2], fill=PANEL)
    d.text((bx1 + 8, by1 + 8), "state / progress / fov", fill=(150, 156, 168))
    bh = 26
    for i in range(len(rows)):
        x1 = bx1 + (bx2 - bx1) * i / len(rows)
        x2 = bx1 + (bx2 - bx1) * (i + 1) / len(rows)
        col = PLAY_COL if rows[i][6] == 1 else PAUSE_COL
        d.rectangle([x1, by1 + 30, x2, by1 + 30 + bh], fill=col)
    d.text((bx1 + 8, by1 + 30 + bh + 4), "playing / paused band", fill=(150, 156, 168))

    # progress curve (0..1.5 normalized; wrap drop visible)
    tmax = max(r[1] for r in rows)
    ty1, ty2 = by1 + 90, by1 + 200

    def mt(t):
        return ty2 - (t / (tmax + 0.05)) * (ty2 - ty1)

    for i in range(len(rows) - 1):
        x1 = bx1 + (bx2 - bx1) * i / len(rows)
        x2 = bx1 + (bx2 - bx1) * (i + 1) / len(rows)
        d.line([x1, mt(rows[i][1]), x2, mt(rows[i + 1][1])], fill=(120, 190, 255), width=2)
    for i in range(30, 35):
        x1 = bx1 + (bx2 - bx1) * i / len(rows)
        x2 = bx1 + (bx2 - bx1) * (i + 1) / len(rows)
        d.line([x1, mt(rows[i][1]), x2, mt(rows[i + 1][1])], fill=PAUSE_COL, width=2)
    d.text((bx1 + 8, ty2 + 2), "progress t (flat = pause, drop = wrap f64)",
           fill=(110, 116, 128))

    # fov curve (skip nan)
    fy1, fy2 = by1 + 260, by2 - 20
    fmin, fmax = 40.0, 62.0

    def mf(v):
        return fy2 - (v - fmin) / (fmax - fmin) * (fy2 - fy1)

    prev = None
    for i in range(len(rows)):
        r = rows[i]
        x = bx1 + (bx2 - bx1) * i / len(rows)
        if math.isnan(r[5]):
            prev = None
            continue
        if prev is not None:
            d.line([prev[0], prev[1], x, mf(r[5])], fill=(250, 190, 60), width=2)
        prev = (x, mf(r[5]))
    d.text((bx1 + 8, by2 - 14), "fov deg (45..60, yellow)", fill=(110, 116, 128))

    # footer
    d.text((16, H - 26),
           "source: cam-tools cine-trace (real output)  |  anchors: cine-check "
           "12 (90 rows)", fill=(120, 126, 138))

    img.save(sys.argv[2])
    print("evidence written:", sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
