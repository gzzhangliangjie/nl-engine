#!/usr/bin/env python3
"""Make the M6.2 occlusion-performance scene GLB (1.3M-tri sphere + front wall).

Geometry (world space, pre-baked transforms):
  - sphere : IcoSphereDense1M.glb geometry reused verbatim (1310720 tris,
    radius 1 icosphere) — the S4 1M+ asset, loaded from its GLB source;
  - wall   : thin box at z=+1.5, half extents (0.45, 0.40, 0.12). Camera sits
    on +z (orbit dist ~3.2): the wall's angular half-width atan(0.45/1.7) ~
    14.8 deg covers the central ~2/3 of the sphere's 18.2 deg projection, so
    roughly two thirds of the sphere clusters get Hi-Z culled and the rest
    stays visible (partial occlusion, not a total wipe).

Usage: python scripts/make-occl-scene-glb.py assets/models/OcclScene1M.glb
"""

import json
import struct
import sys
from pathlib import Path

SRC = Path("assets/models/IcoSphereDense1M.glb")


def read_glb_primitive(path):
    """Return (positions, normals, indices) of the first primitive of a GLB."""
    data = path.read_bytes()
    magic, version, total = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2, "not a glTF 2.0 GLB"
    jlen, jtype = struct.unpack_from("<II", data, 12)
    assert jtype == 0x4E4F534A, "first chunk is not JSON"
    doc = json.loads(data[20:20 + jlen])
    jsz = jlen + (4 - jlen % 4) % 4
    blen, btype = struct.unpack_from("<II", data, 20 + jsz)
    assert btype == 0x004E4942, "second chunk is not BIN"
    bin_base = 20 + jsz + 8  # skip the BIN chunk header (8 bytes)

    prim = doc["meshes"][0]["primitives"][0]

    def read_acc(acc_idx):
        acc = doc["accessors"][acc_idx]
        bv = doc["bufferViews"][acc["bufferView"]]
        off = bin_base + bv.get("byteOffset", 0)
        n = acc["count"]
        if acc["componentType"] == 5126:      # float
            return list(struct.unpack_from(f"<{n*3}f", data, off))
        if acc["componentType"] == 5125:      # uint32
            return list(struct.unpack_from(f"<{n}I", data, off))
        raise AssertionError(f"unsupported componentType {acc['componentType']}")

    pos = read_acc(prim["attributes"]["POSITION"])
    nrm = read_acc(prim["attributes"]["NORMAL"])
    idx = read_acc(prim["indices"])
    return pos, nrm, idx


def box(center, half):
    """24 verts (4 per face, flat normals), 12 tris.

    每面四角按「从外侧看逆时针」排列（glTF 右手系 CCW 正面约定），
    同 make-hiz-scene-glb.py（check-orient 全链 PASS 的先例）。
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


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    dst = Path(sys.argv[1])

    s_pos, s_nrm, s_idx = read_glb_primitive(SRC)
    n_sv = len(s_pos) // 3
    w_pos, w_nrm, w_idx = box((0.0, 0.0, 1.5), (0.45, 0.40, 0.12))

    # 单 primitive 合并（nanite-tools cluster -o 只吃一个 submesh）：
    # 球在前、墙在后拼接，墙索引重定基（+球顶点数）。
    pos = s_pos + [v for pt in w_pos for v in pt]
    nrm = s_nrm + [v for nt in w_nrm for v in nt]
    idx = s_idx + [n_sv + v for v in w_idx]

    bin_data = bytearray()
    buffer_views, accessors = [], []
    min_p, max_p = [1e9] * 3, [-1e9] * 3

    def add_view(data, target):
        off = len(bin_data)
        bin_data.extend(data)
        pad = (4 - len(bin_data) % 4) % 4
        bin_data.extend(b"\x00" * pad)
        buffer_views.append({"buffer": 0, "byteOffset": off,
                             "byteLength": len(data), "target": target})
        return len(buffer_views) - 1

    p_bytes = struct.pack(f"<{len(pos)}f", *pos)
    n_bytes = struct.pack(f"<{len(nrm)}f", *nrm)
    i_bytes = struct.pack(f"<{len(idx)}I", *idx)
    pv = add_view(p_bytes, 34962)
    nv = add_view(n_bytes, 34962)
    iv = add_view(i_bytes, 34963)
    for k in range(0, len(pos), 3):
        for d in range(3):
            min_p[d] = min(min_p[d], pos[k + d])
            max_p[d] = max(max_p[d], pos[k + d])
    accessors.append({"bufferView": pv, "componentType": 5126, "count": len(pos) // 3,
                      "type": "VEC3", "min": min_p, "max": max_p})
    accessors.append({"bufferView": nv, "componentType": 5126,
                      "count": len(nrm) // 3, "type": "VEC3"})
    accessors.append({"bufferView": iv, "componentType": 5125,
                      "count": len(idx), "type": "SCALAR"})

    doc = {
        "asset": {"version": "2.0", "generator": "make-occl-scene-glb.py (M6.2)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "OcclScene1M"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                    "indices": 2, "material": 0}],
                    "name": "OcclScene1M"}],
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
    print(f"wrote {dst} (1 primitive: 1.3M sphere + occluder wall, "
          f"{n_tri} triangles, {total} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
