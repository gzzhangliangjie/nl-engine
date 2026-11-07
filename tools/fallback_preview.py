#!/usr/bin/env python3
"""M3.1 visual evidence: offline render of the fallback mesh section.

Input is the .nmesh (convention: <prefix>.lod0.nmesh, file version 3) written
by `nanite-tools lod` — the fallback tail section ('NFLB' magic) holds a plain
indexed mesh stitched from the topmost LOD level (weldPositions-equal merge).

This is the render-only counterpart of the "fallback renders standalone
without holes" acceptance line in S3-PLAN M3.1: the picture is generated from
the real committed asset bytes, no GPU, no network.

Usage:
    python tools/fallback_preview.py <model.lod0.nmesh> <out.png> [--yaw D] [--pitch D]
"""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SS = 2
PANEL_W, PANEL_H = 760, 620
TOP, BOTTOM = 76, 64
W, H = PANEL_W * SS, TOP + PANEL_H * SS + BOTTOM
BG = (18, 22, 28)
PANEL_BG = (24, 29, 36)
EDGE = (46, 56, 68)
TEXT = (225, 232, 240)
MUTED = (140, 155, 170)

kNMeshMagic = 0x48534D4E
kNMeshLodMagic = 0x444F4C4E
kNMeshFallbackMagic = 0x424C464E


def font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # older Pillow
        return ImageFont.load_default()


def read_fallback(path: Path):
    """Parse .nmesh v1/2/3, return (vertices, indices, sourceLevel, maxError)."""
    blob = path.read_bytes()
    magic, ver, name_len, meshlets, verts, tris = struct.unpack_from("<IHHIII", blob, 0)
    if magic != kNMeshMagic:
        raise SystemExit(f"not a .nmesh: {path}")
    if ver not in (1, 2, 3):
        raise SystemExit(f"unsupported .nmesh version {ver}: {path}")
    cur = 20 + name_len
    cur += meshlets * 52           # NMeshletRecord[]
    cur += verts * 32              # MeshVertex[]
    cur += tris * 3                # triangles

    fb = None
    while cur < len(blob):
        sec_magic, payload = struct.unpack_from("<II", blob, cur)
        cur += 8
        if sec_magic == kNMeshLodMagic:
            cur += 16 + payload    # NMeshMetaHeader 24 B：已读 magic+payload，余 16 B 头 + payload
        elif sec_magic == kNMeshFallbackMagic:
            # NMeshFallbackHeader: magic(4) payload(4) verts(4) idx(4) level(4) err(4)
            vcount, icount, level, max_err = struct.unpack_from("<IIIf", blob, cur - 8 + 8)
            cur += 16
            vtx = []
            for i in range(vcount):
                px, py, pz, nx, ny, nz, u, w = struct.unpack_from("<8f", blob, cur)
                cur += 32
                vtx.append(((px, py, pz), (nx, ny, nz)))
            idx = list(struct.unpack_from("<" + "I" * icount, blob, cur))
            cur += icount * 4
            fb = (vtx, idx, level, max_err)
        else:
            raise SystemExit(f"unknown tail section magic {sec_magic:#x}: {path}")
    if fb is None:
        raise SystemExit(f"no fallback section ('NFLB') in: {path}")
    return fb


def rotate(v, cy, sy, cp, sp):
    x, y, z = v
    xr = x * cy + z * sy
    zr = -x * sy + z * cy
    yr = y * cp - zr * sp
    zz = y * sp + zr * cp
    return xr, yr, zz


def render(vdata, indices, cy, sy, cp, sp, center, scale):
    img = Image.new("RGB", (PANEL_W * SS, PANEL_H * SS), PANEL_BG)
    d = ImageDraw.Draw(img)
    cx, cy0 = PANEL_W * SS / 2, PANEL_H * SS / 2

    pts = []
    for (pos, _n) in vdata:
        xr, yr, zz = rotate(pos, cy, sy, cp, sp)
        pts.append((cx + (xr - center[0]) * scale,
                    cy0 - (yr - center[1]) * scale, zz))

    # Painter's algorithm: flat shade from vertex normals, far triangles first.
    # Holes show as panel background bleeding through — the visual counterpart
    # of the edge-manifold assertion in `selftest fallback`.
    light = (-0.42, 0.78, 0.47)
    ln = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ln for c in light)
    ordered = []
    for t in range(0, len(indices), 3):
        a, b, c = indices[t], indices[t + 1], indices[t + 2]
        shade = 0.0
        for i in (a, b, c):
            n = vdata[i][1]
            nl = math.sqrt(sum(k * k for k in n)) or 1.0
            shade += abs(sum(n[k] / nl * light[k] for k in range(3)))
        shade /= 3.0
        g = int(96 + 118 * shade)
        depth = (pts[a][2] + pts[b][2] + pts[c][2]) / 3.0
        ordered.append((depth, (a, b, c), (g, g + 3, g + 8)))
    ordered.sort(key=lambda t: t[0])

    for _, f, col in ordered:
        poly = [(int(pts[i][0]), int(pts[i][1])) for i in f]
        d.polygon(poly, fill=col)
        d.line(poly + [poly[0]], fill=EDGE, width=SS)
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("model", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--yaw", type=float, default=12.0)
    ap.add_argument("--pitch", type=float, default=9.0)
    args = ap.parse_args()

    vdata, indices, level, max_err = read_fallback(args.model)
    tri_count = len(indices) // 3

    cy, sy = math.cos(math.radians(args.yaw)), math.sin(math.radians(args.yaw))
    cp, sp = math.cos(math.radians(args.pitch)), math.sin(math.radians(args.pitch))

    # 归一化：包围盒中心 + 最大维度铺满面板
    lo = [min(v[0][k] for v in vdata) for k in range(3)]
    hi = [max(v[0][k] for v in vdata) for k in range(3)]
    center = tuple(0.5 * (lo[k] + hi[k]) for k in range(3))
    extent = max(hi[k] - lo[k] for k in range(3)) or 1.0
    scale = min(PANEL_W, PANEL_H) * 0.78 / extent

    panel = render(vdata, indices, cy, sy, cp, sp, center, scale)

    img = Image.new("RGB", (W, H), BG)
    img.paste(panel, (0, TOP))
    d = ImageDraw.Draw(img)
    d.text((18, 22), "fallback mesh (stitched from topmost LOD level, plain indexed)",
           font=font(17), fill=TEXT)
    d.text((18, 46),
           f"{Path(args.model).name}  |  {tri_count} tris / {len(vdata)} verts  |  "
           f"from L{level}, maxErr {max_err:.4f}  |  UE FallbackPercentTriangles ~2% same scale",
           font=font(13), fill=MUTED)
    d.text((18, H - BOTTOM + 20),
           "flat shade + edges; background bleeding through = hole (must be none)",
           font=font(13), fill=MUTED)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out)
    print(f"wrote {args.out} ({tri_count} tris / {len(vdata)} verts, L{level}, "
          f"maxErr {max_err:.4f})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
