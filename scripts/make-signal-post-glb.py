#!/usr/bin/env python3
"""Make assets/models/signal_post.glb — M-T4 traffic signal post (Q-T16=A).

Hand-written minimal GLB (no external deps; same chunk-packing lineage as
make-multi-material-glb.py): two primitives baked into world-space vertices —
  pole : 0.12 x 3.2 x 0.12 box, baseColor dark grey  (tint x grey ~ near black)
  head : 0.42 x 1.05 x 0.28 box, baseColor white     (tint x white = pure lamp
          colour; viewer flips ModelComponent.tint red/yellow/green per phase)

Head panel normal = +/-Z, so the entity yaw faces the +Z panel toward oncoming
traffic (viewer places it right of the entry stop line, facing back along the
lane). Single scene, two nodes, no textures — PBR factor materials only, so
both texturised (mode 0) and PBR-direct (mode 5) renderer paths render it.

Usage:
  python scripts/make-signal-post-glb.py <out.glb>
"""

import json
import struct
import sys


def box(cx, cy, cz, sx, sy, sz):
    """24-vertex cube (per-face normals) + 36 indices, centred at c, size s."""
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    faces = [
        ((1, 0, 0), ((hx, -hy, -hz), (hx, -hy, hz), (hx, hy, hz), (hx, hy, -hz))),
        ((-1, 0, 0), ((-hx, -hy, hz), (-hx, -hy, -hz), (-hx, hy, -hz), (-hx, hy, hz))),
        ((0, 1, 0), ((-hx, hy, -hz), (hx, hy, -hz), (hx, hy, hz), (-hx, hy, hz))),
        ((0, -1, 0), ((-hx, -hy, hz), (hx, -hy, hz), (hx, -hy, -hz), (-hx, -hy, -hz))),
        ((0, 0, 1), ((-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz))),
        ((0, 0, -1), ((hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz))),
    ]
    pos, nrm, idx = [], [], []
    for n, quad in faces:
        base = len(pos)
        for vx, vy, vz in quad:
            pos.append((vx + cx, vy + cy, vz + cz))
            nrm.append(n)
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return pos, nrm, idx


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = sys.argv[1]

    pole_pos, pole_nrm, pole_idx = box(0, 1.6, 0, 0.12, 3.2, 0.12)
    head_pos, head_nrm, head_idx = box(0, 2.75, 0, 0.42, 1.05, 0.28)

    # BIN 布局：每 primitive 三段（pos f32 / nrm f32 / idx u16），4B 对齐。
    bin_data = bytearray()
    layout = []  # (mesh_id, bv_pos, bv_nrm, bv_idx, n_idx)
    for mpos, mnrm, midx in ((pole_pos, pole_nrm, pole_idx),
                             (head_pos, head_nrm, head_idx)):
        def pad():
            while len(bin_data) % 4:
                bin_data.append(0)
        pad()
        bv_pos = len(layout) and None  # placeholder, real index tracked below
        off_p = len(bin_data)
        bin_data += struct.pack(f"<{len(mpos)*3}f",
                                *[v for p in mpos for v in p])
        pad()
        off_n = len(bin_data)
        bin_data += struct.pack(f"<{len(mnrm)*3}f",
                                *[v for n in mnrm for v in n])
        pad()
        off_i = len(bin_data)
        bin_data += struct.pack(f"<{len(midx)}H", *midx)
        layout.append((len(mpos), off_p, off_n, off_i, len(midx)))

    bin_chunk = bytes(bin_data)

    def accessor(bv, comp, count, mn, mx):
        a = {"bufferView": bv, "componentType": 5126, "count": count,
             "type": comp}
        if mn is not None:
            a["min"], a["max"] = mn, mx
        return a

    buffer_views, accessors, meshes, nodes = [], [], [], []
    mats = [
        {"name": "pole", "pbrMetallicRoughness":
            {"baseColorFactor": [0.15, 0.15, 0.17, 1.0],
             "metallicFactor": 0.0, "roughnessFactor": 0.8}},
        {"name": "head", "pbrMetallicRoughness":
            {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
             "metallicFactor": 0.0, "roughnessFactor": 0.5}},
    ]
    for mi, (n_v, off_p, off_n, off_i, n_i) in enumerate(layout):
        bv_p = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": off_p,
                             "byteLength": n_v * 12, "target": 34962})
        bv_n = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": off_n,
                             "byteLength": n_v * 12, "target": 34962})
        bv_i = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": off_i,
                             "byteLength": n_i * 2, "target": 34963})
        xs = [p[0] for p in (pole_pos if mi == 0 else head_pos)]
        ys = [p[1] for p in (pole_pos if mi == 0 else head_pos)]
        zs = [p[2] for p in (pole_pos if mi == 0 else head_pos)]
        a_p = len(accessors)
        accessors.append(accessor(bv_p, "VEC3", n_v,
                                  [min(xs), min(ys), min(zs)],
                                  [max(xs), max(ys), max(zs)]))
        a_n = len(accessors)
        accessors.append(accessor(bv_n, "VEC3", n_v, None, None))
        a_i = len(accessors)
        accessors.append({"bufferView": bv_i, "componentType": 5123,
                          "count": n_i, "type": "SCALAR"})
        meshes.append({"primitives": [{
            "attributes": {"POSITION": a_p, "NORMAL": a_n},
            "indices": a_i, "material": mi}]})
        nodes.append({"mesh": mi, "name": f"signal_{mi}"})

    gltf = {
        "asset": {"version": "2.0", "generator": "nl-engine make-signal-post-glb"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": mats,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(bin_chunk)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    while len(bin_chunk) % 4:
        bin_chunk += b"\x00"
    total = 12 + 8 + len(js) + 8 + len(bin_chunk)
    with open(out, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(bin_chunk), 0x004E4942))
        f.write(bin_chunk)
    print(f"written {out}: {total} bytes, {len(meshes)} meshes, "
          f"{len(mats)} materials")
    return 0


if __name__ == "__main__":
    sys.exit(main())
