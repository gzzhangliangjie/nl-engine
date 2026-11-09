#!/usr/bin/env python3
"""Make the M6.1 Hi-Z occlusion acceptance scene GLB (wall + occluded spheres).

Geometry (world space, pre-baked transforms; loader bakes node transforms but we
write final coordinates directly):
  - wall : thin box at z=+0.5, half extents (1.8, 1.1, 0.15)  -> the occluder
  - s1..s3 : spheres BEHIND the wall (camera side is +z), hidden by it
  - ctrl  : sphere beside the wall edge (x=3.3), stays visible -> proves the
            culling is selective, not global

Usage: python scripts/make-hiz-scene-glb.py assets/models/HizScene.glb
"""

import json
import math
import struct
import sys
from pathlib import Path


def box(center, half):
    """24 verts (4 per face, flat normals), 12 tris.

    每面四角按「从外侧看逆时针」排列（glTF 右手系 CCW 正面约定）：
    cross(v1-v0, v2-v0) 必须与声明法线同向（否则 check-orient 全链 FAIL）。
    """
    cx, cy, cz = center
    hx, hy, hz = half
    faces = [
        ((0, 0, -1), [(-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz), (hx, -hy, -hz)]),
        ((0, 0, 1),  [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((1, 0, 0),  [(hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz), (hx, -hy, hz)]),
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
        ((0, 1, 0),  [(-hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz)]),
    ]
    pos, nrm, idx = [], [], []
    for n, corners in faces:
        base = len(pos)
        for ox, oy, oz in corners:
            pos.append((cx + ox, cy + oy, cz + oz))
            nrm.append(n)
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, idx


def sphere(center, radius, lat=12, lon=24):
    """UV sphere, smooth normals. 三角绕序 [a, a+1, b] 保证外向 CCW（glTF 约定）。"""
    cx, cy, cz = center
    pos, nrm, idx = [], [], []
    for i in range(lat + 1):
        theta = math.pi * i / lat            # 0..pi from +y
        for j in range(lon + 1):
            phi = 2.0 * math.pi * j / lon    # 0..2pi
            nx = math.sin(theta) * math.cos(phi)
            ny = math.cos(theta)
            nz = math.sin(theta) * math.sin(phi)
            pos.append((cx + radius * nx, cy + radius * ny, cz + radius * nz))
            nrm.append((nx, ny, nz))
    for i in range(lat):
        for j in range(lon):
            a = i * (lon + 1) + j
            b = a + lon + 1
            idx += [a, a + 1, b, a + 1, b + 1, b]
    return pos, nrm, idx


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    dst = Path(sys.argv[1])

    prims = [
        box((0.0, 0.0, 0.5), (1.8, 1.1, 0.15)),    # wall (occluder)
        sphere((-1.4, 0.6, -1.5), 0.55),           # s1 behind
        sphere((1.5, 0.2, -1.6), 0.55),            # s2 behind
        sphere((-0.2, -0.85, -1.7), 0.50),         # s3 behind
        sphere((3.3, 0.1, -1.5), 0.50),            # ctrl beside wall -> visible
    ]

    # 单 primitive 合并（nanite-tools cluster -o 只吃一个 submesh）：各部分
    # 顶点拼接 + 索引重定基。
    pos: list = []
    nrm: list = []
    idx: list = []
    for p, n, i in prims:
        base = len(pos)
        pos.extend(p)
        nrm.extend(n)
        idx.extend(base + v for v in i)

    bin_data = bytearray()
    buffer_views, accessors, primitives = [], [], []
    min_p, max_p = [1e9] * 3, [-1e9] * 3

    def add_view(data, target):
        off = len(bin_data)
        bin_data.extend(data)
        pad = (4 - len(bin_data) % 4) % 4
        bin_data.extend(b"\x00" * pad)
        buffer_views.append({"buffer": 0, "byteOffset": off,
                             "byteLength": len(data), "target": target})
        return len(buffer_views) - 1

    p_bytes = struct.pack(f"<{len(pos)*3}f", *(v for pt in pos for v in pt))
    n_bytes = struct.pack(f"<{len(nrm)*3}f", *(v for nt in nrm for v in nt))
    i_bytes = struct.pack(f"<{len(idx)}I", *idx)
    pv = add_view(p_bytes, 34962)
    nv = add_view(n_bytes, 34962)
    iv = add_view(i_bytes, 34963)
    for pt in pos:
        for k in range(3):
            min_p[k] = min(min_p[k], pt[k])
            max_p[k] = max(max_p[k], pt[k])
    accessors.append({"bufferView": pv, "componentType": 5126, "count": len(pos),
                      "type": "VEC3", "min": min_p, "max": max_p})
    accessors.append({"bufferView": nv, "componentType": 5126,
                      "count": len(nrm), "type": "VEC3"})
    accessors.append({"bufferView": iv, "componentType": 5125,
                      "count": len(idx), "type": "SCALAR"})
    primitives.append({"attributes": {"POSITION": 0, "NORMAL": 1},
                       "indices": 2, "material": 0})

    doc = {
        "asset": {"version": "2.0", "generator": "make-hiz-scene-glb.py (M6.1)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "HizScene"}],
        "meshes": [{"primitives": primitives, "name": "HizScene"}],
        "materials": [{"name": "white", "pbrMetallicRoughness":
                       {"baseColorFactor": [0.85, 0.85, 0.85, 1.0]}}],
        "buffers": [{"byteLength": len(bin_data)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }

    json_bytes = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    bin_chunk = bytes(bin_data)
    bin_chunk += b"\x00" * ((4 - len(bin_chunk) % 4) % 4)

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_chunk)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A)
    out += json_bytes
    out += struct.pack("<II", len(bin_chunk), 0x004E4942)
    out += bin_chunk
    dst.write_bytes(bytes(out))
    n_tri = len(idx) // 3
    print(f"wrote {dst} (1 primitive merged from {len(prims)} parts, "
          f"{n_tri} triangles, {total} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
