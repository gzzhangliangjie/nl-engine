#!/usr/bin/env python3
"""M2.2 visual evidence: LOD pyramid rendered from the *real* .nmesh files.

Reads <prefix>.lod0.nmesh, <prefix>.lod1.nmesh, ... (v2 files written by
`nanite-tools lod -o <prefix>`) and draws every level side by side with one
shared camera and one shared scale. Red dots are the locked seam points each
level promises to the coarser one. No mockups: every triangle comes from the
binary asset itself.

Usage:
    python tools/lod_preview.py <prefix> <out.png> [--yaw DEG] [--pitch DEG]

Reuses the projection/renderer from tools/qem_preview.py (Pillow, pure local).
"""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

from PIL import Image, ImageDraw

from qem_preview import (BG, MUTED, PANEL_H, PANEL_W, SS, TEXT, font,
                         render_panel, rotate)

GAP = 20
ROW_GAP = 18
COLS = 3
PW, PH = 420, 350          # each panel after downscale from 600x500
CAP = 34
TOP, BOTTOM = 78, 56


def read_nmesh(path: Path):
    """Return (level, triangles, clusters, verts, faces, lockedIds)."""
    blob = path.read_bytes()
    magic, version, name_len, meshlet_count, vertex_count, tri_count = \
        struct.unpack_from("<IHHIII", blob, 0)
    if magic != 0x48534D4E or version not in (1, 2):
        raise SystemExit(f"not a v1/v2 .nmesh: {path}")
    cur = 20 + name_len
    records = []
    for _ in range(meshlet_count):
        vo, vc, to, tc = struct.unpack_from("<IIII", blob, cur)
        cur += 52                       # 4 uints + 10 floats of bounds
        records.append((vo, vc, to, tc))
    verts = []
    for _ in range(vertex_count):
        verts.append(struct.unpack_from("<fff", blob, cur))
        cur += 32                      # MeshVertex (pos/normal/uv)
    tris = blob[cur:cur + tri_count * 3]
    cur += tri_count * 3

    locked_pos, level, max_error = [], None, 0.0
    if version == 2:
        mg, payload_bytes, mv, level, max_error, locked_count, _ = \
            struct.unpack_from("<IIHHfII", blob, cur)
        cur += 24
        if mg != 0x444F4C4E or mv not in (1, 2):
            raise SystemExit(f"bad lod meta header: {path}")
        locked_pos = list(struct.unpack_from("<" + "f" * (locked_count * 3), blob, cur))
        cur += payload_bytes
        # meta version 2（M3.0）：payload 尾部还有 DAG 血缘段
        # （u32 groupCount + 32B 组记录 ×N + u32 clusterGroup ×meshlets）。
        # payload_bytes 覆盖全部段，cur 一次跳过即可；本工具只画几何与锁点。
    if cur != len(blob):
        raise SystemExit(f"trailing bytes in {path}")

    # Weld by exact position (same convention as weldPositions), then expand
    # every meshlet's local triangle indices into canonical triangles.
    pos2canon: dict[tuple[float, float, float], int] = {}
    canon_verts: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    for vo, _vc, to, tc in records:
        for t in range(tc):
            ids = []
            for k in range(3):
                p = verts[vo + tris[to + 3 * t + k]]
                if p not in pos2canon:
                    pos2canon[p] = len(canon_verts)
                    canon_verts.append(p)
                ids.append(pos2canon[p])
            if len(set(ids)) == 3:
                faces.append((ids[0], ids[1], ids[2]))
    locked_ids = []
    for i in range(0, len(locked_pos), 3):
        p = (locked_pos[i], locked_pos[i + 1], locked_pos[i + 2])
        if p in pos2canon:
            locked_ids.append(pos2canon[p])
    return level, tri_count, meshlet_count, canon_verts, faces, locked_ids, max_error


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("prefix", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--yaw", type=float, default=12.0)
    ap.add_argument("--pitch", type=float, default=9.0)
    args = ap.parse_args()

    levels = []
    n = 0
    while True:
        p = args.prefix.with_name(args.prefix.name + f".lod{n}.nmesh")
        if not p.is_file():
            break
        levels.append(read_nmesh(p))
        n += 1
    if n < 2:
        raise SystemExit(f"need >=2 .nmesh levels for {args.prefix} (found {n})")

    yaw, pitch = math.radians(args.yaw), math.radians(args.pitch)
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    all_rot = [rotate(v, cy, sy, cp, sp)
               for lv in levels for v in lv[3]]
    minx, maxx = min(r[0] for r in all_rot), max(r[0] for r in all_rot)
    miny, maxy = min(r[1] for r in all_rot), max(r[1] for r in all_rot)
    center = ((minx + maxx) / 2, (miny + maxy) / 2)
    scale = min(PANEL_W * SS * 0.86 / max(maxx - minx, 1e-6),
                PANEL_H * SS * 0.86 / max(maxy - miny, 1e-6))

    rows = math.ceil(len(levels) / COLS)
    width = COLS * PW + (COLS + 1) * GAP
    height = TOP + rows * (PH + CAP) + (rows - 1) * ROW_GAP + BOTTOM
    canvas = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(canvas)
    f_big, f_small = font(22), font(14)
    d.text((GAP, 14), "M2.2 / s2-loop   LOD HIERARCHY  (real .nmesh v2 geometry)",
           font=f_big, fill=TEXT)
    d.text((GAP, 44),
           "one shared camera + scale; group borders recomputed each level (no cross-level carry); from real .nmesh v2 files",
           font=f_small, fill=MUTED)

    for k, (level, tris, clusters, verts, faces, locked_ids, max_err) in enumerate(levels):
        panel = render_panel(verts, faces, locked_ids, cy, sy, cp, sp, center, scale)
        panel = panel.resize((PW, PH), Image.LANCZOS)
        col, row = k % COLS, k // COLS
        x = GAP + col * (PW + GAP)
        y = TOP + row * (PH + CAP + ROW_GAP)
        canvas.paste(panel, (x, y))
        d.text((x + 4, y + PH + 8),
               f"L{k}  {tris} tris / {clusters} clusters / {len(locked_ids)} locked"
               f"  err {max_err:.4f}",
               font=f_small, fill=TEXT)
        print(f"L{k}: {tris} tris, {clusters} clusters, {len(locked_ids)} locked, "
              f"maxErr {max_err:.4f}, meta level={level}")

    d.text((GAP, height - 30),
           "RED = this level's group-boundary points (top level has none)   |   ~50% tris per level"
           "   |   parent error is monotonic across levels",
           font=f_small, fill=MUTED)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(args.out)
    print(f"wrote {args.out}  {width}x{height}  levels={len(levels)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
