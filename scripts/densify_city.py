#!/usr/bin/env python3
"""densify_city.py - M-CITY-DENS: OSM road-network infill densification.

Turns the sparse cg2 city (150 OSM buildings) into a dense city by infilling
buildings along BOTH sides of the real OSM road network (roadnet.json), plus
visual road slabs and traffic-signal poles. All deterministic (seeded).

Products:
  1. assets/models/road_slab.glb      (unit box, white untextured material)
  2. city.scene.dense.engine          (original entities + infill + roads + signals)

Infill rules (all seeded, no wall-clock):
  - walk each roadnet edge, both sides, from t=+clearance to L-clearance
  - building depth 10..22m, width 12..22m, setback = width/2 + 3.5m + depth/2
  - height mix: 60% 8..24m / 25% 24..60m / 10% 60..120m / 5% 120..300m
  - tier: h<24 tier1, <80 tier2, else tier3 (matches existing city_box tiers)
  - skip if AABB overlaps an existing/infilled building (16m hash grid)
  - road slab per edge: thin box, y staggered by edge id (anti z-fighting)
  - signal pole + head per signal node (M-T4 signal=true)

usage:
  densify_city.py [--scene PATH] [--roadnet PATH] [--out PATH] [--seed N]
  densify_city.py selftest
exit codes: 0 ok / 1 runtime failure / 2 usage
"""
from __future__ import annotations

import argparse
import json
import math
import random
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
USAGE = """densify_city - M-CITY-DENS road-network infill
usage:
  densify_city.py [--scene PATH] [--roadnet PATH] [--out PATH] [--seed N]
  densify_city.py selftest
"""


# ---------------------------------------------------------------------------
# road_slab.glb: unit box (+-0.5), white untextured pbr material.
# Engine contract (core/src/mesh_io.cpp): missing texture -> has* false ->
# neutral fallback texture x tint; baseColorFactor parsed into prim.
# ---------------------------------------------------------------------------
def make_road_slab_glb(path: Path) -> None:
    # 24 verts: 6 faces x 4 corners, CCW outward.
    faces = [
        # (normal, 4 verts)
        ((0, 0, 1), [(-.5, -.5, .5), (.5, -.5, .5), (.5, .5, .5), (-.5, .5, .5)]),
        ((0, 0, -1), [(.5, -.5, -.5), (-.5, -.5, -.5), (-.5, .5, -.5), (.5, .5, -.5)]),
        ((1, 0, 0), [(.5, -.5, .5), (.5, -.5, -.5), (.5, .5, -.5), (.5, .5, .5)]),
        ((-1, 0, 0), [(-.5, -.5, -.5), (-.5, -.5, .5), (-.5, .5, .5), (-.5, .5, -.5)]),
        ((0, 1, 0), [(-.5, .5, .5), (.5, .5, .5), (.5, .5, -.5), (-.5, .5, -.5)]),
        ((0, -1, 0), [(-.5, -.5, -.5), (.5, -.5, -.5), (.5, -.5, .5), (-.5, -.5, .5)]),
    ]
    pos, nor, uv, idx = [], [], [], []
    for fi, (n, verts) in enumerate(faces):
        for vi, v in enumerate(verts):
            pos.extend(v)
            nor.extend(n)
            uv.extend([0.0 if vi in (0, 3) else 1.0, 0.0 if vi in (0, 1) else 1.0])
        base = fi * 4
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def acc(view, ctype, gtype, count, bmin, bmax):
        return {"bufferView": view, "componentType": ctype, "count": count,
                "type": gtype, "min": bmin, "max": bmax}

    def bounds2(flat, comps):
        xs = flat[0::comps]; ys = flat[1::comps]
        zs = flat[2::comps] if comps == 3 else None
        if zs is None:
            return [min(xs), min(ys)], [max(xs), max(ys)]
        return [min(xs), min(ys), min(zs)], [max(xs), max(ys), max(zs)]

    # bufferViews: 0 pos, 1 nor, 2 uv, 3 idx
    pos_bytes = struct.pack(f"<{len(pos)}f", *pos)
    nor_bytes = struct.pack(f"<{len(nor)}f", *nor)
    uv_bytes = struct.pack(f"<{len(uv)}f", *uv)
    idx_bytes = struct.pack(f"<{len(idx)}H", *idx)

    def pad(b):
        return b + b"\x00" * ((4 - len(b) % 4) % 4)

    bv_pos = {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes)}
    bv_nor = {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nor_bytes)}
    bv_uv = {"buffer": 0, "byteOffset": len(pos_bytes) + len(nor_bytes),
             "byteLength": len(uv_bytes)}
    off3 = len(pos_bytes) + len(nor_bytes) + len(uv_bytes)
    bv_idx = {"buffer": 0, "byteOffset": off3, "byteLength": len(idx_bytes)}
    bin_chunk = pad(pos_bytes + nor_bytes + uv_bytes + idx_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "densify_city road_slab"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1,
                                                   "TEXCOORD_0": 2},
                                    "indices": 3, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 0.0, "roughnessFactor": 0.9}}],
        "accessors": [
            acc(0, 5126, "VEC3", 24, *bounds2(pos, 3)),
            acc(1, 5126, "VEC3", 24, *bounds2(nor, 3)),
            acc(2, 5126, "VEC2", 24, *bounds2(uv, 2)),
            acc(3, 5123, "SCALAR", 36, [0], [35]),
        ],
        "bufferViews": [bv_pos, bv_nor, bv_uv, bv_idx],
        "buffers": [{"byteLength": len(bin_chunk)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js = js + b" " * ((4 - len(js) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_chunk)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(bin_chunk), 0x004E4942))
        f.write(bin_chunk)


# ---------------------------------------------------------------------------
# infill
# ---------------------------------------------------------------------------
def quat_yaw(yaw: float) -> list[float]:
    return [0.0, math.sin(yaw / 2.0), 0.0, math.cos(yaw / 2.0)]


def tier_for(h: float) -> str:
    if h < 24.0:
        return "city_box_tier1.glb"
    if h < 80.0:
        return "city_box_tier2.glb"
    return "city_box_tier3.glb"


def height_mix(rng: random.Random) -> float:
    r = rng.random()
    if r < 0.60:
        return rng.uniform(8.0, 24.0)
    if r < 0.85:
        return rng.uniform(24.0, 60.0)
    if r < 0.95:
        return rng.uniform(60.0, 120.0)
    return rng.uniform(120.0, 300.0)


class AabbGrid:
    """16m hash grid of axis-aligned boxes for overlap rejection."""

    CELL = 16.0

    def __init__(self) -> None:
        self.cells: dict[tuple[int, int], list[tuple[float, float, float, float]]] = {}

    @staticmethod
    def _aabb(px: float, pz: float, sx: float, sz: float):
        return (px - sx / 2, pz - sz / 2, px + sx / 2, pz + sz / 2)

    def insert(self, px: float, pz: float, sx: float, sz: float) -> None:
        box = self._aabb(px, pz, sx, sz)
        x0 = int(math.floor(box[0] / self.CELL))
        x1 = int(math.floor(box[2] / self.CELL))
        z0 = int(math.floor(box[1] / self.CELL))
        z1 = int(math.floor(box[3] / self.CELL))
        for cx in range(x0, x1 + 1):
            for cz in range(z0, z1 + 1):
                self.cells.setdefault((cx, cz), []).append(box)

    def hits(self, px: float, pz: float, sx: float, sz: float) -> bool:
        box = self._aabb(px, pz, sx, sz)
        x0 = int(math.floor(box[0] / self.CELL))
        x1 = int(math.floor(box[2] / self.CELL))
        z0 = int(math.floor(box[1] / self.CELL))
        z1 = int(math.floor(box[3] / self.CELL))
        for cx in range(x0, x1 + 1):
            for cz in range(z0, z1 + 1):
                for b in self.cells.get((cx, cz), ()):
                    if not (box[2] <= b[0] or box[0] >= b[2]
                            or box[3] <= b[1] or box[1] >= b[3]):
                        return True
        return False


def densify(scene_path: Path, roadnet_path: Path, out_path: Path,
            seed: int) -> dict:
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    roadnet = json.loads(roadnet_path.read_text(encoding="utf-8"))
    entities = scene["entities"]
    next_id = int(scene.get("nextID", len(entities) + 1))

    rng = random.Random(seed)
    grid = AabbGrid()

    # --- register existing building AABBs (rotation-free boxes in cg2) ---
    for ent in entities:
        tr = ent.get("components", {}).get("Transform", {})
        model = ent.get("components", {}).get("Model", {})
        if "city_box_tier" in model.get("mesh", ""):
            p = tr.get("position", [0, 0, 0])
            s = tr.get("scale", [1, 1, 1])
            grid.insert(p[0], p[2], s[0], s[2])

    nm = {n["id"]: n for n in roadnet["nodes"]}
    edges = roadnet["edges"]
    stats = {"infill": 0, "roads": 0, "signals": 0, "skipped_overlap": 0}

    # --- (1) road slabs: one thin box per edge, yaw-aligned ---
    for e in edges:
        a = nm[e["from"]]
        b = nm[e["to"]]
        dx = b["x"] - a["x"]
        dz = b["z"] - a["z"]
        length = math.hypot(dx, dz)
        if length < 1.0:
            continue
        w = float(e.get("width", 8.0))
        yaw = math.atan2(-dz, dx)   # box local +x aligned to edge direction
        # rotation maps local +x to world dir: yaw = atan2(-dz, dx)? Engine
        # convention: quaternion Y-rotation by theta maps +x to (cos t, 0,
        # -sin t). Solve cos t = dx/L, -sin t = dz/L -> t = atan2(-dz, dx).
        slab_y = 0.05 + (e["id"] % 7) * 0.006
        entities.append({
            "id": next_id, "name": f"road_{e['id']}", "flags": 0, "parent": 0,
            "components": {
                "Model": {"mesh": "road_slab.glb", "tint": [0.22, 0.22, 0.24]},
                "Transform": {
                    "position": [round((a["x"] + b["x"]) / 2, 2),
                                 round(slab_y, 3),
                                 round((a["z"] + b["z"]) / 2, 2)],
                    "scale": [round(length + w, 2), 0.12, round(w, 2)],
                    "rotation": quat_yaw(yaw),
                },
            },
        })
        next_id += 1
        stats["roads"] += 1

    # --- (2) infill buildings along both sides ---
    CLEAR = 14.0        # intersection clearance (both ends)
    SIDEWALK = 3.5      # setback beyond road half-width
    for e in edges:
        a = nm[e["from"]]
        b = nm[e["to"]]
        dx = b["x"] - a["x"]
        dz = b["z"] - a["z"]
        length = math.hypot(dx, dz)
        if length < 2 * CLEAR + 8.0:
            continue
        ux, uz = dx / length, dz / length
        w = float(e.get("width", 8.0))
        yaw = math.atan2(-dz, dx)
        t = CLEAR
        while t < length - CLEAR:
            for side in (1.0, -1.0):
                depth = rng.uniform(10.0, 22.0)
                bw = rng.uniform(12.0, 22.0)
                gap = rng.uniform(4.0, 10.0)
                h = height_mix(rng)
                off = w / 2 + SIDEWALK + depth / 2
                cx = a["x"] + ux * (t + bw / 2) + (-uz) * side * off
                cz = a["z"] + uz * (t + bw / 2) + (ux) * side * off
                # rotated box -> conservative world AABB for rejection
                ca = math.cos(yaw)
                sa = math.sin(yaw)
                ex = abs(bw * ca) + abs(depth * sa)
                ez = abs(bw * sa) + abs(depth * ca)
                if grid.hits(cx, cz, ex, ez):
                    stats["skipped_overlap"] += 1
                    continue
                entities.append({
                    "id": next_id, "name": f"infill_{next_id}", "flags": 0,
                    "parent": 0,
                    "components": {
                        "Model": {"mesh": tier_for(h),
                                  "tint": [1.0, 1.0, 1.0]},
                        "Transform": {
                            "position": [round(cx, 2), round(h / 2, 2),
                                         round(cz, 2)],
                            "scale": [round(bw, 2), round(h, 2),
                                      round(depth, 2)],
                            "rotation": quat_yaw(yaw),
                        },
                    },
                })
                next_id += 1
                grid.insert(cx, cz, ex, ez)
                stats["infill"] += 1
            t += bw + gap

    # --- (3) traffic signal poles at M-T4 signal nodes ---
    for n in roadnet["nodes"]:
        if not n.get("signal"):
            continue
        # pole slightly off-road: shift toward +x+z corner by 5.5m
        px = n["x"] + 5.5
        pz = n["z"] + 5.5
        entities.append({
            "id": next_id, "name": f"sigpole_{n['id']}", "flags": 0,
            "parent": 0,
            "components": {
                "Model": {"mesh": "road_slab.glb", "tint": [0.15, 0.15, 0.16]},
                "Transform": {
                    "position": [round(px, 2), 3.5, round(pz, 2)],
                    "scale": [0.35, 7.0, 0.35],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                },
            },
        })
        next_id += 1
        head_tint = [0.9, 0.12, 0.10] if n["id"] % 2 == 0 else [0.15, 0.85, 0.20]
        entities.append({
            "id": next_id, "name": f"sighead_{n['id']}", "flags": 0,
            "parent": 0,
            "components": {
                "Model": {"mesh": "road_slab.glb", "tint": head_tint},
                "Transform": {
                    "position": [round(px, 2), 7.4, round(pz, 2)],
                    "scale": [1.0, 0.8, 0.5],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                },
            },
        })
        next_id += 1
        stats["signals"] += 1

    scene["nextID"] = next_id
    out_path.write_text(json.dumps(scene, ensure_ascii=False, indent=1) + "\n",
                        encoding="utf-8", newline="\n")
    return stats


def selftest() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        # minimal fixture: 1 building + 2-node roadnet with 1 signal
        scene = {
            "version": 1, "nextID": 2,
            "entities": [
                {"id": 1, "name": "bld_x", "flags": 0, "parent": 0,
                 "components": {
                     "Model": {"mesh": "city_box_tier1.glb",
                               "tint": [1.0, 1.0, 1.0]},
                     "Transform": {"position": [0.0, 5.0, 0.0],
                                   "scale": [20.0, 10.0, 20.0],
                                   "rotation": [0.0, 0.0, 0.0, 1.0]}}}],
        }
        roadnet = {
            "nodes": [{"id": 0, "x": -50.0, "z": 0.0, "kind": "intersection",
                       "signal": True},
                      {"id": 1, "x": 50.0, "z": 0.0, "kind": "intersection",
                       "signal": False}],
            "edges": [{"id": 0, "from": 0, "to": 1, "lanes_per_dir": 1,
                       "speed_limit": 13.9, "width": 8.0}],
        }
        sp = td / "s.scene"; sp.write_text(json.dumps(scene), encoding="utf-8")
        rp = td / "r.json"; rp.write_text(json.dumps(roadnet), encoding="utf-8")
        o1 = td / "o1.scene"; o2 = td / "o2.scene"
        s1 = densify(sp, rp, o1, 20260717)
        s2 = densify(sp, rp, o2, 20260717)
        b1 = o1.read_bytes(); b2 = o2.read_bytes()
        assert b1 == b2, "determinism violated"
        d = json.loads(b1.decode("utf-8"))
        kinds = {}
        for e in d["entities"]:
            k = e["name"].split("_")[0]
            kinds[k] = kinds.get(k, 0) + 1
        assert kinds.get("road") == 1, kinds
        assert kinds.get("sigpole") == 1 and kinds.get("sighead") == 1, kinds
        assert kinds.get("infill", 0) > 0, kinds
        # slab/box glb selftest: header + chunk structure parseback
        gp = td / "road_slab.glb"
        make_road_slab_glb(gp)
        raw = gp.read_bytes()
        magic, ver, total = struct.unpack("<III", raw[:12])
        assert magic == 0x46546C67 and total == len(raw), "glb header broken"
        jlen, jtype = struct.unpack("<II", raw[12:20])
        assert jtype == 0x4E4F534A, "JSON chunk type broken"
        j = json.loads(raw[20:20 + jlen].decode("utf-8"))
        assert len(j["accessors"]) == 4 and j["accessors"][3]["count"] == 36
        blen, btype = struct.unpack("<II", raw[20 + jlen:28 + jlen])
        assert btype == 0x004E4942, "BIN chunk type broken"
        print(f"densify selftest: PASS (kinds={kinds}, stats={s1})")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) >= 1 and argv[0] == "selftest":
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default=str(REPO_ROOT / "example/cg2-m0/city.scene.engine"))
    ap.add_argument("--roadnet", default=str(REPO_ROOT / "example/cg2-m0/roadnet.json"))
    ap.add_argument("--out", default=str(REPO_ROOT / "example/cg2-m0/city.scene.dense.engine"))
    ap.add_argument("--seed", type=int, default=20260717)
    ap.add_argument("--slab-glb", default=str(REPO_ROOT / "assets/models/road_slab.glb"))
    a = ap.parse_args(argv)
    make_road_slab_glb(Path(a.slab_glb))
    stats = densify(Path(a.scene), Path(a.roadnet), Path(a.out), a.seed)
    print(f"densify: infill={stats['infill']} roads={stats['roads']} "
          f"signals={stats['signals']} skipped={stats['skipped_overlap']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
