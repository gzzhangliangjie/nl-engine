#!/usr/bin/env python3
"""Generate dense test meshes as single-file GLB (M0.7, S1 cluster-cutting prerequisite).

Suzanne has 504 triangles -- useless for demonstrating meshlet/cluster cutting.
This script procedurally generates dense, deterministic meshes (torus knot /
icosphere, 100k~500k tri tiers) and packs them into GLB with an embedded
procedural checkerboard PNG. Pure Python stdlib, zero network, reproducible:
the same arguments always produce a byte-identical GLB (MD5 printed at exit).

Usage:
  python scripts/gen-dense-mesh.py --shape torusknot --tubular 512 --radial 200 \
      --out assets/models/TorusKnotDense.glb     # ~205k tri (M0.7 baseline case)
  python scripts/gen-dense-mesh.py --shape icosphere --subdiv 7 \
      --out assets/models/IcoSphereDense.glb     # ~328k tri

Geometry: POSITION / NORMAL / TEXCOORD_0 (float), uint32 indices (vertices always
exceed 65535; the pipeline already runs R32_UINT). The knot tube frame follows the
common Frenet-approximation used by three.js TorusKnotGeometry; icosphere normals
are the normalized position, UV is equirectangular (the longitudinal seam shows a
texture wrap line -- accepted, documented, and actually useful for M0.8 debug
shading).
"""

import argparse
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path


# ---------------------------------------------------------------- PNG (stdlib)

def checker_png(size: int = 256, cells: int = 8) -> bytes:
    """RGBA8 checkerboard PNG, written by hand with zlib+struct (no PIL)."""
    cell = size // cells
    light = (208, 210, 216, 255)
    dark = (58, 64, 92, 255)
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # filter type 0 (None) per scanline
        cy = (y // cell) % 2
        for x in range(size):
            rows.extend(dark if (cx := (x // cell) % 2) ^ cy else light)
    raw = bytes(rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ------------------------------------------------------------------ geometry

def normalize(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / l, v[1] / l, v[2] / l)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def gen_torus_knot(tubular: int, radial: int, p: int = 2, q: int = 3,
                   scale: float = 0.4):
    """(p,q) torus knot tube. Returns (positions, normals, uvs, indices)."""
    def curve(t):
        cu, su = math.cos(p * t), math.sin(p * t)
        qu_over_p = q / p * t
        cs = math.cos(qu_over_p)
        # knot on a torus (R=2, r=1) around Y, then remapped to Z-up glTF style
        return ((2.0 + cs) * cu, cs * qu_over_p * 0.0 + math.sin(qu_over_p), (2.0 + cs) * su)

    eps = 1e-4
    positions, normals, uvs, indices = [], [], [], []
    for i in range(tubular + 1):
        t = i / tubular * 2.0 * math.pi
        P1, P2, P = curve(t + eps), curve(t - eps), curve(t)
        T = normalize((P1[0] - P[0], P1[1] - P[1], P1[2] - P[2]))
        Nn = (P1[0] + P2[0], P1[1] + P2[1], P1[2] + P2[2])
        B = cross(T, Nn)
        Nn = cross(B, T)
        Nn, B = normalize(Nn), normalize(B)
        for j in range(radial + 1):
            v = j / radial * 2.0 * math.pi
            cx, cy = -math.cos(v), math.sin(v)
            r = 0.42  # tube radius
            positions.extend(((P[0] + r * (cx * Nn[0] + cy * B[0])) * scale,
                              (P[1] + r * (cx * Nn[1] + cy * B[1])) * scale,
                              (P[2] + r * (cx * Nn[2] + cy * B[2])) * scale))
            # tube surface normal: radial direction in the (N,B) plane
            normals.extend(normalize((cx * Nn[0] + cy * B[0],
                                      cx * Nn[1] + cy * B[1],
                                      cx * Nn[2] + cy * B[2])))
            uvs.append((i / tubular, j / radial))
    stride = radial + 1
    for i in range(tubular):
        for j in range(radial):
            a = i * stride + j
            b = (i + 1) * stride + j
            c = (i + 1) * stride + j + 1
            d = i * stride + j + 1
            indices.extend((a, b, d, b, c, d))
    return positions, normals, uvs, indices


def gen_icosphere(subdiv: int):
    """Subdivided icosphere via midpoint cache. 20 * 4**subdiv triangles."""
    t = (1.0 + math.sqrt(5.0)) / 2.0
    base_verts = [normalize(v) for v in
                  ((-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0),
                   (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
                   (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1))]
    base_faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
                  (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
                  (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
                  (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]

    verts = list(base_verts)
    faces = list(base_faces)
    cache = {}

    def midpoint(a: int, b: int) -> int:
        key = (a, b) if a < b else (b, a)
        idx = cache.get(key)
        if idx is None:
            va, vb = verts[a], verts[b]
            m = normalize(((va[0] + vb[0]) / 2, (va[1] + vb[1]) / 2,
                           (va[2] + vb[2]) / 2))
            idx = len(verts)
            verts.append(m)
            cache[key] = idx
        return idx

    for _ in range(subdiv):
        new_faces = []
        for (a, b, c) in faces:
            ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
            new_faces.extend(((a, ab, ca), (b, bc, ab), (c, ca, bc),
                              (ab, bc, ca)))
        faces = new_faces

    positions, normals, uvs = [], [], []
    for v in verts:
        positions.extend(v)
        normals.extend(v)  # sphere: normal == normalized position
        u = 0.5 + math.atan2(v[2], v[0]) / (2.0 * math.pi)
        vv = 0.5 - math.asin(max(-1.0, min(1.0, v[1]))) / math.pi
        uvs.append((u, vv))
    indices = []
    for (a, b, c) in faces:
        indices.extend((a, b, c))
    return positions, normals, uvs, indices


# ---------------------------------------------------------------- GLB packing

def pack_glb(positions, normals, uvs, indices, png: bytes) -> bytes:
    pos_b = struct.pack("<%df" % len(positions), *positions)
    nrm_b = struct.pack("<%df" % len(normals), *normals)
    flat_uv = [c for pair in uvs for c in pair]
    uv_b = struct.pack("<%df" % len(flat_uv), *flat_uv)
    idx_b = struct.pack("<%dI" % len(indices), *indices)

    binary = bytearray()
    views = []

    def add(data: bytes, align: int = 4, target: int = 0) -> int:
        while len(binary) % align:
            binary.append(0)
        offset = len(binary)
        binary.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target:
            view["target"] = target
        views.append(view)
        return len(views) - 1

    idx_view = add(idx_b, target=34963)
    pos_view = add(pos_b)
    nrm_view = add(nrm_b)
    uv_view = add(uv_b)
    img_view = add(png)

    vmin = [min(positions[0::3]), min(positions[1::3]), min(positions[2::3])]
    vmax = [max(positions[0::3]), max(positions[1::3]), max(positions[2::3])]
    doc = {
        "asset": {"version": "2.0",
                  "generator": "nl-engine gen-dense-mesh.py (procedural, deterministic)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 1, "NORMAL": 2, "TEXCOORD_0": 3},
            "indices": 0, "material": 0, "mode": 4}]}],
        "materials": [{
            "name": "checker",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0, "roughnessFactor": 0.9}}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 10497, "wrapT": 10497}],
        "images": [{"bufferView": img_view, "mimeType": "image/png"}],
        "buffers": [{"byteLength": 0}],  # filled below
        "bufferViews": views,
        "accessors": [
            {"bufferView": idx_view, "componentType": 5125,
             "count": len(indices), "type": "SCALAR"},
            {"bufferView": pos_view, "componentType": 5126,
             "count": len(positions) // 3, "type": "VEC3",
             "min": vmin, "max": vmax},
            {"bufferView": nrm_view, "componentType": 5126,
             "count": len(normals) // 3, "type": "VEC3"},
            {"bufferView": uv_view, "componentType": 5126,
             "count": len(flat_uv) // 2, "type": "VEC2"},
        ],
    }

    binary = bytes(binary)
    doc["buffers"][0]["byteLength"] = len(binary)

    def pad4(data: bytes, fill: bytes) -> bytes:
        return data + fill * ((4 - len(data) % 4) % 4)

    json_bytes = pad4(
        json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8"),
        b" ")
    bin_bytes = pad4(binary, b"\x00")
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)        # 'glTF', v2
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A)  # 'JSON'
    out += json_bytes
    out += struct.pack("<II", len(bin_bytes), 0x004E4942)   # 'BIN\0'
    out += bin_bytes
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shape", required=True, choices=["torusknot", "icosphere"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--tubular", type=int, default=512, help="torus knot tubular segments")
    ap.add_argument("--radial", type=int, default=200, help="torus knot radial segments")
    ap.add_argument("--subdiv", type=int, default=7, help="icosphere subdivision level")
    ap.add_argument("--scale", type=float, default=0.4, help="torus knot scale to fit view")
    ap.add_argument("--checker-size", type=int, default=256)
    args = ap.parse_args()

    if args.shape == "torusknot":
        if args.tubular < 8 or args.radial < 4:
            print("error: --tubular >= 8, --radial >= 4", file=sys.stderr)
            return 2
        pos, nrm, uv, idx = gen_torus_knot(args.tubular, args.radial, scale=args.scale)
    else:
        if not (1 <= args.subdiv <= 8):
            print("error: --subdiv in [1,8]", file=sys.stderr)
            return 2
        pos, nrm, uv, idx = gen_icosphere(args.subdiv)

    glb = pack_glb(pos, nrm, uv, idx, checker_png(args.checker_size))

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(glb)

    md5 = hashlib.md5(glb).hexdigest()
    tri = len(idx) // 3
    print(f"wrote {out} ({len(glb)} bytes)")
    print(f"  vertices={len(pos)//3} triangles={tri} indices={len(idx)} "
          f"checker={args.checker_size}px")
    print(f"  md5={md5}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
