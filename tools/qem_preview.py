#!/usr/bin/env python3
"""M2.1 visual evidence: side-by-side before/after render of a real QEM simplify.

Input is the geometry dumped by `nanite-tools simplify <model.nmesh> --obj <prefix>`:
    <prefix>.before.obj   input canonical triangle soup
    <prefix>.after.obj    QEM-simplified triangle soup
    <prefix>.locked       group-boundary canonical point ids (0-based, one per line)

Both panels use the SAME camera and SAME scale, so locked points (red dots)
must land on identical pixels -- that is the visual counterpart of the
bit-identical boundary assertion in `selftest qem`.

Usage:
    python tools/qem_preview.py <prefix> <out.png> [--yaw DEG] [--pitch DEG]

Pure local rendering (Pillow), no GPU, no network. Not part of the regression gate.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SS = 2                       # supersampling factor
PANEL_W, PANEL_H = 600, 500
TOP, BOTTOM = 76, 64
GAP = 20
W = PANEL_W * 2 + GAP * 3
H = TOP + PANEL_H + BOTTOM
BG = (18, 22, 28)
PANEL_BG = (24, 29, 36)
EDGE = (46, 56, 68)
RED = (235, 72, 72)
TEXT = (225, 232, 240)
MUTED = (140, 155, 170)


def read_obj(path: Path):
    verts: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("v "):
            p = line.split()
            verts.append((float(p[1]), float(p[2]), float(p[3])))
        elif line.startswith("f "):
            p = line.split()
            faces.append(tuple(int(s.split("/")[0]) - 1 for s in p[1:4]))
    return verts, faces


def read_locked(path: Path) -> list[int]:
    if not path.is_file():
        return []
    return [int(s) for s in path.read_text(encoding="utf-8").split() if s.strip()]


def rotate(v, cy, sy, cp, sp):
    """Yaw about Y, then pitch about X. Returns (screen x, screen y up, depth)."""
    x, y, z = v
    xr = x * cy + z * sy
    zr = -x * sy + z * cy
    yr = y * cp - zr * sp
    zz = y * sp + zr * cp
    return xr, yr, zz


def font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # older Pillow
        return ImageFont.load_default()


def render_panel(verts, faces, locked, cy, sy, cp, sp, center, scale):
    img = Image.new("RGB", (PANEL_W * SS, PANEL_H * SS), PANEL_BG)
    d = ImageDraw.Draw(img)
    cx, cy0 = PANEL_W * SS / 2, PANEL_H * SS / 2

    rot = [rotate(v, cy, sy, cp, sp) for v in verts]
    pts = [(cx + (r[0] - center[0]) * scale,
            cy0 - (r[1] - center[1]) * scale,
            r[2]) for r in rot]

    # Painter's algorithm: far triangles first. Light gives the surface volume;
    # the wireframe on top makes triangle density (the actual QEM result) visible.
    light = (-0.42, 0.78, 0.47)
    ln = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ln for c in light)
    ordered = []
    for f in faces:
        a, b, c = verts[f[0]], verts[f[1]], verts[f[2]]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        w = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (u[1] * w[2] - u[2] * w[1],
             u[2] * w[0] - u[0] * w[2],
             u[0] * w[1] - u[1] * w[0])
        nl = math.sqrt(sum(k * k for k in n)) or 1.0
        shade = abs(sum(n[k] / nl * light[k] for k in range(3)))
        g = int(96 + 118 * shade)
        depth = (pts[f[0]][2] + pts[f[1]][2] + pts[f[2]][2]) / 3.0
        ordered.append((depth, f, (g, g + 3, g + 8)))
    ordered.sort(key=lambda t: t[0])

    for _, f, col in ordered:
        poly = [(int(pts[i][0]), int(pts[i][1])) for i in f]
        d.polygon(poly, fill=col)
        d.line(poly + [poly[0]], fill=EDGE, width=SS)

    r = int(1.6 * SS)
    for p in locked:
        if 0 <= p < len(pts):
            x, y = int(pts[p][0]), int(pts[p][1])
            d.ellipse((x - r, y - r, x + r, y + r), fill=RED)

    return img.resize((PANEL_W, PANEL_H), Image.LANCZOS)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("prefix", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--yaw", type=float, default=12.0)
    ap.add_argument("--pitch", type=float, default=9.0)
    args = ap.parse_args()

    before_v, before_f = read_obj(args.prefix.with_name(args.prefix.name + ".before.obj"))
    after_v, after_f = read_obj(args.prefix.with_name(args.prefix.name + ".after.obj"))
    locked = read_locked(args.prefix.with_name(args.prefix.name + ".locked"))
    if not before_f or not after_f:
        raise SystemExit("empty triangle soup -- did --obj run?")

    yaw, pitch = math.radians(args.yaw), math.radians(args.pitch)
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    # One shared camera/scale for both panels, computed from the union of points.
    rot = [rotate(v, cy, sy, cp, sp) for v in before_v + after_v]
    minx, maxx = min(r[0] for r in rot), max(r[0] for r in rot)
    miny, maxy = min(r[1] for r in rot), max(r[1] for r in rot)
    center = ((minx + maxx) / 2, (miny + maxy) / 2)
    scale = min(PANEL_W * SS * 0.86 / max(maxx - minx, 1e-6),
                PANEL_H * SS * 0.86 / max(maxy - miny, 1e-6))

    left = render_panel(before_v, before_f, locked, cy, sy, cp, sp, center, scale)
    right = render_panel(after_v, after_f, locked, cy, sy, cp, sp, center, scale)

    canvas = Image.new("RGB", (W, H), BG)
    canvas.paste(left, (GAP, TOP))
    canvas.paste(right, (GAP * 2 + PANEL_W, TOP))
    d = ImageDraw.Draw(canvas)
    f_big, f_mid, f_small = font(22), font(18), font(14)
    d.text((GAP, 16), "M2.1 / s2-qem   QEM SIMPLIFY + BOUNDARY LOCK", font=f_big, fill=TEXT)
    d.text((GAP, 44), "Suzanne - real geometry dumped by nanite-tools simplify --obj (same camera, same scale)",
           font=f_small, fill=MUTED)
    d.text((GAP + 8, TOP + PANEL_H + 10),
           f"BEFORE   {len(before_f)} tris / {len(before_v)} pts", font=f_mid, fill=TEXT)
    d.text((GAP * 2 + PANEL_W + 8, TOP + PANEL_H + 10),
           f"AFTER   {len(after_f)} tris / {len(after_v)} pts   ({100.0 * len(after_f) / len(before_f):.1f}%)",
           font=f_mid, fill=TEXT)
    d.text((GAP, H - 26),
           f"RED = {len(locked)} locked group-boundary points, bit-identical in both panels"
           "   |   wireframe density is the QEM result, not a mockup",
           font=f_small, fill=MUTED)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(args.out)
    print(f"wrote {args.out}  {W}x{H}  before={len(before_f)} after={len(after_f)} locked={len(locked)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
