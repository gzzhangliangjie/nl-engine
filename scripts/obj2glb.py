#!/usr/bin/env python3
"""Convert a Wavefront OBJ (+ MTL) into one self-contained GLB.

Built for the P9 V1 asset-ingest warm-up (track ``asset-ingest`` / M-P9V1).
Zero third-party dependencies -- stdlib only, following the conventions of
``scripts/pack-suzanne-glb.py`` and ``scripts/gen-dense-mesh.py``.

Why this exists
---------------
nl-engine's only geometry entry point is ``loadGltfScene()``
(``core/src/mesh_io.cpp``): a glTF 2.0 subset read through vendored tinygltf.
Unity / 3ds Max scenes ship FBX / OBJ, so an OBJ -> GLB bridge is the minimal
offline step before ``nanite-tools cluster -> lod -> dag``.

What it does
------------
* parses ``v / vn / vt / f / usemtl / mtllib``; n-gons are fan-triangulated.
* one glTF primitive per ``usemtl`` group => one NSUB submesh in ``.nmesh``,
  material id = submesh index (engine hard limit ``kMaxVisMaterials = 4``).
  The converter refuses >4 groups instead of producing an asset the engine
  cannot load.
* resolves MTL ``map_Kd`` by BASENAME from ``--maps`` (shipped .mtl files
  usually contain dead absolute paths pointing at the author's machine).
  Lookup is case-insensitive; PNG and JPEG are embedded verbatim.
* flips V (OBJ bottom-left origin -> glTF top-left origin); ``--no-flip-v``
  disables it for assets exported with glTF convention already.
* writes a single GLB: geometry bufferViews + embedded image bufferViews.

Usage
-----
  python scripts/obj2glb.py <input.obj> <output.glb> [--maps DIR] [--no-flip-v]
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

# glTF enums we emit.
FLOAT = 5126
UNSIGNED_INT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
TRIANGLES = 4

MAX_MATERIALS = 4  # renderer/src/scene.hpp: kMaxVisMaterials


# ---------------------------------------------------------------------------
# OBJ / MTL parsing
# ---------------------------------------------------------------------------
def resolve_index(token, count):
    """OBJ indices are 1-based; negative values count back from the end."""
    if not token:
        return None
    value = int(token)
    return value - 1 if value > 0 else count + value


def parse_obj(path):
    verts, normals, uvs = [], [], []
    groups = []
    current = None
    mtllib = None

    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        p = line.split()
        tag = p[0]
        if tag == "v" and len(p) >= 4:
            verts.append(tuple(float(x) for x in p[1:4]))
        elif tag == "vn" and len(p) >= 4:
            normals.append(tuple(float(x) for x in p[1:4]))
        elif tag == "vt" and len(p) >= 3:
            uvs.append(tuple(float(x) for x in p[1:3]))
        elif tag == "mtllib":
            mtllib = " ".join(p[1:])
        elif tag == "usemtl":
            current = {"name": " ".join(p[1:]), "faces": []}
            groups.append(current)
        elif tag == "f" and len(p) >= 4:
            if current is None:
                current = {"name": "default", "faces": []}
                groups.append(current)
            current["faces"].append(p[1:])

    if not verts:
        raise ValueError("OBJ has no vertices: %s" % path)
    if not groups:
        raise ValueError("OBJ has no faces: %s" % path)
    return verts, normals, uvs, groups, mtllib


def parse_mtl(path):
    materials = {}
    current = None
    if path is None or not path.exists():
        return materials
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        p = line.split()
        if p[0] == "newmtl":
            current = " ".join(p[1:])
            materials[current] = {}
        elif current is None:
            continue
        elif p[0] == "map_Kd":
            raw_path = " ".join(p[1:]).strip('"')
            materials[current]["map_Kd"] = re.split(r"[\\/]", raw_path)[-1]
        elif p[0] == "Kd" and len(p) >= 4:
            materials[current]["Kd"] = [float(x) for x in p[1:4]]
    return materials


def find_texture(basename, maps_dir):
    """Case-insensitive lookup; returns Path or None."""
    if not basename or not maps_dir.exists():
        return None
    direct = maps_dir / basename
    if direct.exists():
        return direct
    lowered = basename.lower()
    for candidate in maps_dir.iterdir():
        if candidate.is_file() and candidate.name.lower() == lowered:
            return candidate
    return None


def build_primitive(group, verts, normals, uvs, flip_v):
    """Deduplicate (v/vt/vn) tuples into glTF vertices, fan-triangulate faces."""
    cache = {}
    pos, nor, tex, idx = [], [], [], []

    for face in group["faces"]:
        tri = []
        for token in face:
            if token not in cache:
                s = token.split("/")
                vi = resolve_index(s[0], len(verts))
                ti = resolve_index(s[1], len(uvs)) if len(s) > 1 else None
                ni = resolve_index(s[2], len(normals)) if len(s) > 2 else None
                if vi is None or vi < 0 or vi >= len(verts):
                    raise ValueError("bad vertex index %r" % token)
                cache[token] = len(pos) // 3
                pos.extend(verts[vi])
                nor.extend(normals[ni] if ni is not None and 0 <= ni < len(normals)
                           else (0.0, 0.0, 1.0))
                u, v = uvs[ti] if ti is not None and 0 <= ti < len(uvs) else (0.0, 0.0)
                tex.extend((u, 1.0 - v if flip_v else v))
            tri.append(cache[token])
        for k in range(1, len(tri) - 1):
            idx.extend((tri[0], tri[k], tri[k + 1]))

    if not idx:
        raise ValueError("group %r produced no triangles" % group["name"])
    return pos, nor, tex, idx


# ---------------------------------------------------------------------------
# GLB assembly
# ---------------------------------------------------------------------------
class GlbBuilder:
    def __init__(self, generator):
        self.bin = bytearray()
        self.doc = {
            "asset": {"version": "2.0", "generator": generator},
            "scene": 0,
            "scenes": [{"nodes": []}],
            "nodes": [],
            "meshes": [],
            "materials": [],
            "textures": [],
            "samplers": [{"magFilter": 9729, "minFilter": 9729,
                          "wrapS": 10497, "wrapT": 10497}],
            "images": [],
            "accessors": [],
            "bufferViews": [],
            "buffers": [],
        }

    def add_view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        offset = len(self.bin)
        self.bin.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def add_accessor(self, view, component_type, gltf_type, count, bounds=None):
        acc = {"bufferView": view, "componentType": component_type,
               "count": count, "type": gltf_type}
        if bounds is not None:
            acc["min"], acc["max"] = bounds
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    @staticmethod
    def float_bounds(values, components):
        lo = [min(values[i::components]) for i in range(components)]
        hi = [max(values[i::components]) for i in range(components)]
        return lo, hi

    def add_primitive(self, pos, nor, tex, idx, material):
        pv = self.add_view(struct.pack("<%df" % len(pos), *pos), ARRAY_BUFFER)
        nv = self.add_view(struct.pack("<%df" % len(nor), *nor), ARRAY_BUFFER)
        tv = self.add_view(struct.pack("<%df" % len(tex), *tex), ARRAY_BUFFER)
        iv = self.add_view(struct.pack("<%dI" % len(idx), *idx), ELEMENT_ARRAY_BUFFER)
        pa = self.add_accessor(pv, FLOAT, "VEC3", len(pos) // 3,
                               self.float_bounds(pos, 3))
        na = self.add_accessor(nv, FLOAT, "VEC3", len(nor) // 3)
        ta = self.add_accessor(tv, FLOAT, "VEC2", len(tex) // 2)
        ia = self.add_accessor(iv, UNSIGNED_INT, "SCALAR", len(idx))
        return {
            "attributes": {"POSITION": pa, "NORMAL": na, "TEXCOORD_0": ta},
            "indices": ia,
            "material": material,
            "mode": TRIANGLES,
        }

    def add_material(self, name, base_color, texture_path):
        pbr = {
            "baseColorFactor": base_color,
            "metallicFactor": 0.0,
            "roughnessFactor": 0.9,
        }
        if texture_path is not None:
            suffix = texture_path.suffix.lower()
            mime = "image/jpeg" if suffix in (".jpg", ".jpeg") else "image/png"
            view = self.add_view(texture_path.read_bytes())
            image = len(self.doc["images"])
            self.doc["images"].append({"bufferView": view, "mimeType": mime})
            texture = len(self.doc["textures"])
            self.doc["textures"].append({"sampler": 0, "source": image})
            pbr["baseColorTexture"] = {"index": texture}
        self.doc["materials"].append({"name": name, "pbrMetallicRoughness": pbr})
        return len(self.doc["materials"]) - 1

    def write(self, out_path):
        while len(self.bin) % 4:
            self.bin.append(0)
        bin_bytes = bytes(self.bin)
        self.doc["buffers"] = [{"byteLength": len(bin_bytes)}]
        json_bytes = json.dumps(self.doc, separators=(",", ":"),
                                ensure_ascii=False).encode("utf-8")
        json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
        total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with out_path.open("wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))       # 'glTF', v2
            f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))  # 'JSON'
            f.write(json_bytes)
            f.write(struct.pack("<II", len(bin_bytes), 0x004E4942))   # 'BIN\0'
            f.write(bin_bytes)
        return total, len(bin_bytes)


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="OBJ + MTL -> self-contained GLB")
    ap.add_argument("obj", type=Path, help="input .obj")
    ap.add_argument("out", type=Path, help="output .glb")
    ap.add_argument("--maps", type=Path, default=None,
                    help="directory containing map_Kd textures (default: obj dir)")
    ap.add_argument("--no-flip-v", action="store_true",
                    help="keep OBJ V as-is instead of flipping to glTF convention")
    args = ap.parse_args(argv)

    if not args.obj.exists():
        print("ERROR: OBJ not found: %s" % args.obj)
        return 2
    maps_dir = args.maps or args.obj.parent
    mtl_path = args.obj.parent / (args.obj.stem + ".mtl")

    verts, normals, uvs, groups, mtllib = parse_obj(args.obj)
    if len(groups) > MAX_MATERIALS:
        print("ERROR: %d usemtl groups > kMaxVisMaterials(%d): %s"
              % (len(groups), MAX_MATERIALS,
                 ", ".join(g["name"] for g in groups)))
        return 2
    if mtllib and (args.obj.parent / mtllib).exists():
        mtl_path = args.obj.parent / mtllib
    materials = parse_mtl(mtl_path)

    builder = GlbBuilder("nl-engine obj2glb.py (M-P9V1)")
    primitives = []
    total_tris = 0
    for group in groups:
        mat = materials.get(group["name"], {})
        tex_path = find_texture(mat.get("map_Kd"), maps_dir)
        kd = mat.get("Kd", [1.0, 1.0, 1.0])
        material = builder.add_material(group["name"], kd + [1.0], tex_path)
        pos, nor, tex, idx = build_primitive(
            group, verts, normals, uvs, not args.no_flip_v)
        primitives.append(builder.add_primitive(pos, nor, tex, idx, material))
        total_tris += len(idx) // 3
        print("  group %-18s tris=%-6d verts=%-6d tex=%s"
              % (group["name"], len(idx) // 3, len(pos) // 3,
                 tex_path.name if tex_path else "(none)"))

    mesh = len(builder.doc["meshes"])
    builder.doc["meshes"].append({"name": args.obj.stem, "primitives": primitives})
    node = len(builder.doc["nodes"])
    builder.doc["nodes"].append({"mesh": mesh, "name": args.obj.stem})
    builder.doc["scenes"][0]["nodes"] = [node]

    total, bin_len = builder.write(args.out)
    print("wrote %s (%d bytes, BIN %d bytes): %d submeshes, %d triangles, "
          "%d images" % (args.out, total, bin_len, len(primitives), total_tris,
                         len(builder.doc["images"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
