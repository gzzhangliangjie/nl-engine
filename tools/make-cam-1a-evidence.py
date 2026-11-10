#!/usr/bin/env python3
# P8 M-CAM1a evidence: CameraDirector mode-switch scenario from real
# director-trace TSV output (no hand-drawn diagrams — tool-card evidence rule).
# Deterministic: same TSV -> byte-identical PNG (run.bat fc /b gate).
#
# usage: python tools/make-cam-1a-evidence.py <dir-with-tsv> <out.png>

import io
import math
import sys

from PIL import Image, ImageDraw

W, H = 960, 540
BG = (18, 20, 26)
PANEL = (28, 31, 40)
CHASE_COL = (120, 190, 255)
FREE_COL = (250, 190, 60)
GRID = (45, 50, 62)
TXT = (200, 205, 215)


def read_rows(path):
    rows = []
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("frame\t"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) != 7:
                continue
            rows.append((int(p[0]), int(p[1]),
                         float(p[2]), float(p[3]), float(p[4]),
                         float(p[5]), float(p[6])))
    return rows


def main():
    if len(sys.argv) != 3:
        print("usage: make-cam-1a-evidence.py <tsv-dir> <out.png>")
        return 2
    rows = read_rows(sys.argv[1].rstrip("/\\") + "/director-trace.tsv")
    assert len(rows) == 90, "expected 90 director-trace rows, got %d" % len(rows)

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((16, 10), "P8 M-CAM1a  CameraDirector wiring seam - mode-switch trace "
                    "(90 frames, dt=1/60)", fill=TXT)
    d.text((16, 28), "chase follow -> C edge -> free fly -> C edge -> damped follow "
                    "(switch frames pose-continuous)", fill=(150, 156, 168))

    # ---- panel 1: eye top view (x-z), mode-colored segments ----
    px, py, pw, ph = 60, 60, 560, 400
    d.rectangle([px, py, px + pw, py + ph], fill=PANEL)
    xs = [r[2] for r in rows]
    zs = [r[4] for r in rows]
    xmin, xmax = min(xs), max(xs)
    zmin, zmax = min(zs), max(zs)
    xpad = max((xmax - xmin) * 0.08, 0.5)
    zpad = max((zmax - zmin) * 0.08, 0.5)
    xmin, xmax = xmin - xpad, xmax + xpad
    zmin, zmax = zmin - zpad, zmax + zpad

    def mx(x):
        return px + (x - xmin) / (xmax - xmin) * pw

    def mz(z):
        return py + ph - (z - zmin) / (zmax - zmin) * ph

    for gx in range(5):
        gxv = px + pw * gx / 4
        d.line([gxv, py, gxv, py + ph], fill=GRID)
    for gy in range(5):
        gyv = py + ph * gy / 4
        d.line([px, gyv, px + pw, gyv], fill=GRID)
    d.text((px + 8, py + 8), "eye top view (X right, Z up) - blue=chase yellow=free",
           fill=(150, 156, 168))
    for i in range(len(rows) - 1):
        col = FREE_COL if rows[i + 1][1] == 1 else CHASE_COL
        d.line([mx(rows[i][2]), mz(rows[i][4]), mx(rows[i + 1][2]), mz(rows[i + 1][4])],
               fill=col, width=3)
    # switch markers f30 / f60
    for f in (30, 60):
        r = rows[f]
        d.ellipse([mx(r[2]) - 5, mz(r[4]) - 5, mx(r[2]) + 5, mz(r[4]) + 5],
                  outline=(255, 90, 90), width=2)
    d.text((mx(rows[30][2]) + 8, mz(rows[30][4]) - 18), "f30->free", fill=(255, 120, 120))
    d.text((mx(rows[60][2]) + 8, mz(rows[60][4]) - 18), "f60->chase", fill=(255, 120, 120))
    d.text((px, py + ph + 6),
           "X %.2f..%.2f  Z %.2f..%.2f" % (xmin + xpad, xmax - xpad, zmin + zpad, zmax - zpad),
           fill=(110, 116, 128))

    # ---- panel 2: mode band + eye_z timeline ----
    bx1, by1, bx2, by2 = 660, 60, 940, 460
    d.rectangle([bx1, by1, bx2, by2], fill=PANEL)
    d.text((bx1 + 8, by1 + 8), "mode / eye_z", fill=(150, 156, 168))
    bh = 26
    for i in range(len(rows)):
        bxx = bx1 + (bx2 - bx1) * i / len(rows)
        bxx2 = bx1 + (bx2 - bx1) * (i + 1) / len(rows)
        col = FREE_COL if rows[i][1] == 1 else (70, 110, 160)
        d.rectangle([bxx, by1 + 30, bxx2, by1 + 30 + bh], fill=col)
    d.text((bx1 + 8, by1 + 30 + bh + 4), "chase / free band", fill=(150, 156, 168))
    zmin2, zmax2 = min(zs), max(zs)
    zpad2 = max((zmax2 - zmin2) * 0.1, 0.5)
    zmin2 -= zpad2
    zmax2 += zpad2

    def mz2(z):
        return by2 - (z - zmin2) / (zmax2 - zmin2) * (by2 - by1 - 80)

    for i in range(len(rows) - 1):
        x1 = bx1 + (bx2 - bx1) * i / len(rows)
        x2 = bx1 + (bx2 - bx1) * (i + 1) / len(rows)
        d.line([x1, mz2(rows[i][4]), x2, mz2(rows[i + 1][4])], fill=CHASE_COL, width=2)
    for f in (30, 60):
        bxx = bx1 + (bx2 - bx1) * f / len(rows)
        d.line([bxx, by1 + 30, bxx, by2], fill=(255, 90, 90), width=1)
    d.text((bx1 + 8, by2 - 14), "eye_z (red = switch frames)", fill=(110, 116, 128))

    # footer
    d.text((16, H - 26),
           "source: cam-tools director-trace (real output)  |  anchors: director-check "
           "9 (90 rows)", fill=(120, 126, 138))

    img.save(sys.argv[2])
    print("evidence written:", sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
