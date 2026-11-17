#!/usr/bin/env python3
"""real_city.py - M-CITY-OSM: real-OSM buildings as the city base.

User decisions (2026-11-17): 441 real OSM buildings fetched into
modules/citygen/test/zjx-bld-q{1..4}.json; first "pure real" (no infill),
then revised after seeing the sparse aerial: "在真实地图的基础上再做生成" —
real buildings occupy the rejection grid FIRST, then procedural infill
(densify_city.py's along-road generator) fills the remaining gaps.

Products:
  city.scene.real.engine = base cg2 scene (150 OSM bld_ entities + landmark
  tower + cars) + NEW real OSM buildings (dedup by way id) + procedural
  infill along the roadnet + road slabs + signal poles.

Coordinate calibration: cg2 scene entities are named bld_<osm_way_id>, so the
way-id overlap between the scene and the OSM files gives exact (lon,lat)->
(local x,z) pairs; a least-squares affine fit (no rotation, uniform meter
scale expected) is derived from those pairs and verified by residual.
The cg2 pipeline used the footprint BBOX CENTER (fit residual 0.005 m; the
vertex-centroid convention drifts ~27 m on complex footprints).

Building conversion:
  - footprint polygon (out geom) -> min-area rotated rect (rotating calipers
    on the convex hull); engine supports yaw (quat_yaw, same convention as
    densify_city.py)
  - height: tag height > tag building:levels * 3.0 > seeded fallback by
    footprint area (tiers match city_box tier split)
  - overlap rejection: 16m hash grid (AabbGrid), conservative world AABB

usage:
  real_city.py [--scene PATH] [--roadnet PATH] [--osm GLOB] [--out PATH]
               [--seed N]
  real_city.py selftest
exit codes: 0 ok / 1 runtime failure / 2 usage
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
from densify_city import height_mix, quat_yaw, tier_for  # noqa: E402

USAGE = """real_city - M-CITY-OSM pure real-OSM buildings
usage:
  real_city.py [--scene PATH] [--roadnet PATH] [--osm GLOB] [--out PATH]
               [--seed N]
  real_city.py selftest
"""


# ---------------------------------------------------------------------------
# OSM input
# ---------------------------------------------------------------------------
def load_building_ways(paths: list[Path]) -> list[dict]:
    """All building ways from the quadrant json files, sorted by way id."""
    ways: dict[int, dict] = {}
    for p in paths:
        d = json.loads(p.read_text(encoding="utf-8"))
        for e in d.get("elements", []):
            if e.get("type") != "way" or "building" not in e.get("tags", {}):
                continue
            if "geometry" not in e or len(e["geometry"]) < 3:
                continue
            ways[e["id"]] = e
    return [ways[k] for k in sorted(ways)]


def parse_height(tags: dict) -> float | None:
    h = tags.get("height")
    if h:
        try:
            return float(str(h).split()[0])
        except ValueError:
            pass
    lv = tags.get("building:levels")
    if lv:
        try:
            n = float(str(lv).split()[0])
            if n > 0:
                return n * 3.0
        except ValueError:
            pass
    return None


# ---------------------------------------------------------------------------
# geometry
# ---------------------------------------------------------------------------
def convex_hull(pts: list[tuple[float, float]]) -> list[tuple[float, float]]:
    pts = sorted(set(pts))
    if len(pts) < 3:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower: list[tuple[float, float]] = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper: list[tuple[float, float]] = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def min_area_rect(pts: list[tuple[float, float]]):
    """Rotating calipers: smallest-area rectangle covering pts.

    Returns (cx, cz, width, depth, yaw) with yaw the engine quaternion-Y
    convention (local +x maps to (cos t, 0, -sin t)).
    """
    hull = convex_hull(pts)
    if len(hull) < 3:
        xs = [p[0] for p in pts]
        zs = [p[1] for p in pts]
        return ((min(xs) + max(xs)) / 2, (min(zs) + max(zs)) / 2,
                max(max(xs) - min(xs), 1.0), max(max(zs) - min(zs), 1.0), 0.0)
    best = None
    n = len(hull)
    for i in range(n):
        ax, az = hull[i]
        bx, bz = hull[(i + 1) % n]
        dx, dz = bx - ax, bz - az
        length = math.hypot(dx, dz)
        if length < 1e-9:
            continue
        ux, uz = dx / length, dz / length
        us = [p[0] * ux + p[1] * uz for p in hull]
        vs = [-p[0] * uz + p[1] * ux for p in hull]
        w = max(us) - min(us)
        d = max(vs) - min(vs)
        area = w * d
        if best is None or area < best[0]:
            cxu = (max(us) + min(us)) / 2
            czv = (max(vs) + min(vs)) / 2
            cx = cxu * ux - czv * uz
            cz = cxu * uz + czv * ux
            best = (area, cx, cz, w, d, math.atan2(-uz, ux))
    _, cx, cz, w, d, yaw = best
    return cx, cz, w, d, yaw


def fallback_height(rng: random.Random, area_m2: float) -> float:
    if area_m2 < 800.0:
        return rng.uniform(8.0, 24.0)
    if area_m2 < 2000.0:
        return rng.uniform(24.0, 60.0)
    if area_m2 < 5000.0:
        return rng.uniform(60.0, 120.0)
    return rng.uniform(120.0, 300.0)


class RectGrid:
    """16m hash grid of oriented rects; exact SAT rejection.

    Broadata AABB from the grid cell prefilters candidates, then an exact
    separating-axis test on the two oriented rectangles decides. This avoids
    the conservative AABB over-rejection that cost 48 real buildings when
    neighbouring OSM footprints share walls (2026-11-17).
    """

    CELL = 16.0

    def __init__(self) -> None:
        self.cells: dict[tuple[int, int],
                         list[tuple[float, float, float, float, float]]] = {}

    def insert(self, cx: float, cz: float, bw: float, depth: float,
               yaw: float) -> None:
        ca, sa = math.cos(yaw), math.sin(yaw)
        ex = abs(bw * ca) + abs(depth * sa)
        ez = abs(bw * sa) + abs(depth * ca)
        x0 = int(math.floor((cx - ex / 2) / self.CELL))
        x1 = int(math.floor((cx + ex / 2) / self.CELL))
        z0 = int(math.floor((cz - ez / 2) / self.CELL))
        z1 = int(math.floor((cz + ez / 2) / self.CELL))
        rec = (cx, cz, bw, depth, yaw)
        for ix in range(x0, x1 + 1):
            for iz in range(z0, z1 + 1):
                self.cells.setdefault((ix, iz), []).append(rec)

    @staticmethod
    def _overlap(a, b) -> bool:
        """Exact SAT overlap for two oriented rects (touching = no overlap)."""
        EPS = 0.05  # 5cm tolerance: shared walls are not overlaps
        for r in (a, b):
            ca, sa = math.cos(r[4]), math.sin(r[4])
            for ax, az in ((ca, -sa), (sa, ca)):
                pa = [p[0] * ax + p[1] * az for p in
                      ((a[0] + sx * math.cos(a[4]) * a[2] / 2
                        - sz * math.sin(a[4]) * a[3] / 2,
                        a[1] + sx * math.sin(a[4]) * a[2] / 2
                        + sz * math.cos(a[4]) * a[3] / 2)
                       for sx in (-1, 1) for sz in (-1, 1))]
                pb = [p[0] * ax + p[1] * az for p in
                      ((b[0] + sx * math.cos(b[4]) * b[2] / 2
                        - sz * math.sin(b[4]) * b[3] / 2,
                        b[1] + sx * math.sin(b[4]) * b[2] / 2
                        + sz * math.cos(b[4]) * b[3] / 2)
                       for sx in (-1, 1) for sz in (-1, 1))]
                if max(pa) <= min(pb) + EPS or max(pb) <= min(pa) + EPS:
                    return False
        return True

    def hits(self, cx: float, cz: float, bw: float, depth: float,
             yaw: float) -> bool:
        cand = (cx, cz, bw, depth, yaw)
        ca, sa = math.cos(yaw), math.sin(yaw)
        ex = abs(bw * ca) + abs(depth * sa)
        ez = abs(bw * sa) + abs(depth * ca)
        x0 = int(math.floor((cx - ex / 2) / self.CELL))
        x1 = int(math.floor((cx + ex / 2) / self.CELL))
        z0 = int(math.floor((cz - ez / 2) / self.CELL))
        z1 = int(math.floor((cz + ez / 2) / self.CELL))
        seen = set()
        for ix in range(x0, x1 + 1):
            for iz in range(z0, z1 + 1):
                for rec in self.cells.get((ix, iz), ()):
                    if id(rec) in seen:
                        continue
                    seen.add(id(rec))
                    if self._overlap(cand, rec):
                        return True
        return False


# ---------------------------------------------------------------------------
# calibration
# ---------------------------------------------------------------------------
def calibrate(scene_entities: list[dict], ways: list[dict]) -> tuple:
    """Least-squares fit local = origin + k*(lon,lat) from bld_<id> matches.

    Returns (A, B, C, D, residual_max, pairs) with
      x = A + B*lon, z = C + D*lat.
    """
    by_id = {w["id"]: w for w in ways}
    pairs = []
    for ent in scene_entities:
        name = ent.get("name", "")
        if not name.startswith("bld_"):
            continue
        try:
            wid = int(name[4:])
        except ValueError:
            continue
        w = by_id.get(wid)
        if not w:
            continue
        g = w["geometry"]
        # cg2 pipeline used the footprint BBOX CENTER (verified: fit residual
        # 0.00 m with bbox center vs ~27 m outliers with vertex centroid).
        lon = (min(p["lon"] for p in g) + max(p["lon"] for p in g)) / 2
        lat = (min(p["lat"] for p in g) + max(p["lat"] for p in g)) / 2
        tr = ent["components"]["Transform"]
        pairs.append((lon, lat, tr["position"][0], tr["position"][2]))
    if len(pairs) < 3:
        raise RuntimeError(f"calibration needs >=3 bld_<id> matches, got {len(pairs)}")
    # normal equations for x = A + B*lon and z = C + D*lat (independent)
    sl = sum(p[0] for p in pairs)
    sl2 = sum(p[0] * p[0] for p in pairs)
    sx = sum(p[2] for p in pairs)
    slx = sum(p[0] * p[2] for p in pairs)
    n = len(pairs)
    det = n * sl2 - sl * sl
    if abs(det) < 1e-12:
        raise RuntimeError("degenerate lon spread for calibration")
    B = (n * slx - sl * sx) / det
    A = (sx - B * sl) / n
    st = sum(p[1] for p in pairs)
    st2 = sum(p[1] * p[1] for p in pairs)
    sz = sum(p[3] for p in pairs)
    stz = sum(p[1] * p[3] for p in pairs)
    det = n * st2 - st * st
    if abs(det) < 1e-12:
        raise RuntimeError("degenerate lat spread for calibration")
    D = (n * stz - st * sz) / det
    C = (sz - D * st) / n
    res = max(max(abs(A + B * p[0] - p[2]), abs(C + D * p[1] - p[3]))
              for p in pairs)
    return A, B, C, D, res, pairs


# ---------------------------------------------------------------------------
# main build
# ---------------------------------------------------------------------------
def build_city(scene_path: Path, roadnet_path: Path, osm_paths: list[Path],
               out_path: Path, seed: int) -> dict:
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    roadnet = json.loads(roadnet_path.read_text(encoding="utf-8"))
    entities = scene["entities"]
    next_id = int(scene.get("nextID", len(entities) + 1))

    ways = load_building_ways(osm_paths)
    A, B, C, D, res, pairs = calibrate(entities, ways)

    rng = random.Random(seed)
    grid = RectGrid()
    existing_ids = set()
    for ent in entities:
        tr = ent.get("components", {}).get("Transform", {})
        model = ent.get("components", {}).get("Model", {})
        if "city_box_tier" in model.get("mesh", ""):
            p = tr.get("position", [0, 0, 0])
            s = tr.get("scale", [1, 1, 1])
            grid.insert(p[0], p[2], s[0], s[2], 0.0)
        if ent.get("name", "").startswith("bld_"):
            existing_ids.add(ent["name"])

    stats = {"osm_input": len(ways), "real_added": 0, "dup_scene": 0,
             "infill": 0, "skipped_overlap": 0, "skipped_tiny": 0, "roads": 0,
             "signals": 0, "calib_pairs": len(pairs)}

    # --- (1) real OSM buildings (sorted by way id -> deterministic) ---
    for w in ways:
        name = f"bld_{w['id']}"
        if name in existing_ids:
            stats["dup_scene"] += 1
            continue
        g = w["geometry"]
        tags = w.get("tags", {})
        pts = [(A + B * p["lon"], C + D * p["lat"]) for p in g]
        area = abs(sum(pts[i][0] * pts[(i + 1) % len(pts)][1]
                       - pts[(i + 1) % len(pts)][0] * pts[i][1]
                       for i in range(len(pts)))) / 2
        if area < 30.0:
            stats["skipped_tiny"] += 1
            continue
        cx, cz, bw, depth, yaw = min_area_rect(pts)
        h = parse_height(tags)
        if h is None:
            h = fallback_height(rng, area)
        ca, sa = math.cos(yaw), math.sin(yaw)
        if grid.hits(cx, cz, bw, depth, yaw):
            stats["skipped_overlap"] += 1
            continue
        ent = {
            "id": next_id, "name": name, "flags": 0, "parent": 0,
            "components": {
                "Model": {"mesh": tier_for(h), "tint": [1.0, 1.0, 1.0]},
                "Transform": {
                    "position": [round(cx, 2), round(h / 2, 2),
                                 round(cz, 2)],
                    "scale": [round(bw, 2), round(h, 2), round(depth, 2)],
                    "rotation": quat_yaw(yaw),
                },
            },
        }
        if tags.get("name"):
            ent["label"] = tags["name"]
        entities.append(ent)
        next_id += 1
        grid.insert(cx, cz, bw, depth, yaw)
        stats["real_added"] += 1

    # --- (2) procedural infill along both road sides (real map as base,
    #     generate on top: user decision 2026-11-17). Same generator and
    #     conventions as densify_city.py; real buildings already occupy the
    #     rejection grid so infill only fills the gaps. ---
    nm = {n["id"]: n for n in roadnet["nodes"]}
    CLEAR = 14.0        # intersection clearance (both ends)
    SIDEWALK = 3.5      # setback beyond road half-width
    for e in roadnet["edges"]:
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
                if grid.hits(cx, cz, bw, depth, yaw):
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
                grid.insert(cx, cz, bw, depth, yaw)
                stats["infill"] += 1
            t += bw + gap

    # --- (2) road slabs: identical convention to densify_city.py ---
    nm = {n["id"]: n for n in roadnet["nodes"]}
    for e in roadnet["edges"]:
        a = nm[e["from"]]
        b = nm[e["to"]]
        dx = b["x"] - a["x"]
        dz = b["z"] - a["z"]
        length = math.hypot(dx, dz)
        if length < 1.0:
            continue
        w = float(e.get("width", 8.0))
        yaw = math.atan2(-dz, dx)
        slab_y = 0.05 + (e["id"] % 7) * 0.006
        entities.append({
            "id": next_id, "name": f"road_{e['id']}", "flags": 0, "parent": 0,
            "components": {
                "Model": {"mesh": "road_slab.glb",
                          "tint": [0.22, 0.22, 0.24]},
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

    # --- (3) traffic signal poles: identical to densify_city.py ---
    for n in roadnet["nodes"]:
        if not n.get("signal"):
            continue
        px = n["x"] + 5.5
        pz = n["z"] + 5.5
        entities.append({
            "id": next_id, "name": f"sigpole_{n['id']}", "flags": 0,
            "parent": 0,
            "components": {
                "Model": {"mesh": "road_slab.glb",
                          "tint": [0.15, 0.15, 0.16]},
                "Transform": {
                    "position": [round(px, 2), 3.5, round(pz, 2)],
                    "scale": [0.35, 7.0, 0.35],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                },
            },
        })
        next_id += 1
        head_tint = ([0.9, 0.12, 0.10] if n["id"] % 2 == 0
                     else [0.15, 0.85, 0.20])
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
    stats["calib_residual_m"] = round(res, 3)
    return stats


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------
def selftest() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        # Synthetic frame: 1 deg lon = 102000 m, 1 deg lat = 110540 m.
        # Scene bld_101 at local (1020, 1105.4) built from (lon0+0.01,
        # lat0+0.01); origin lon0=113.0, lat0=23.0 -> fit must recover
        # A=0 (+102000/deg), D=110540/deg with tiny residual.
        def mkway(wid, lon, lat, dlon, dlat, height=""):
            tags = {"building": "yes"}
            if height:
                tags["height"] = height
            hlon, hlat = dlon / 2, dlat / 2
            return {"type": "way", "id": wid, "tags": tags,
                    "geometry": [
                        {"lon": lon - hlon, "lat": lat - hlat},
                        {"lon": lon + hlon, "lat": lat - hlat},
                        {"lon": lon + hlon, "lat": lat + hlat},
                        {"lon": lon - hlon, "lat": lat + hlat},
                    ]}

        LON, LAT = 113.0, 23.0
        scene = {
            "version": 1, "nextID": 4,
            "entities": [
                {"id": 1, "name": "bld_101", "flags": 0, "parent": 0,
                 "components": {
                     "Model": {"mesh": "city_box_tier1.glb",
                               "tint": [1.0, 1.0, 1.0]},
                     "Transform": {
                         "position": [1020.0, 11.0, 1105.4],
                         "scale": [40.0, 22.0, 40.0],
                         "rotation": [0.0, 0.0, 0.0, 1.0]}}},
                {"id": 2, "name": "bld_102", "flags": 0, "parent": 0,
                 "components": {
                     "Model": {"mesh": "city_box_tier1.glb",
                               "tint": [1.0, 1.0, 1.0]},
                     "Transform": {
                         "position": [-1020.0, 8.0, -1105.4],
                         "scale": [30.0, 16.0, 30.0],
                         "rotation": [0.0, 0.0, 0.0, 1.0]}}},
                {"id": 3, "name": "bld_105", "flags": 0, "parent": 0,
                 "components": {
                     "Model": {"mesh": "city_box_tier2.glb",
                               "tint": [1.0, 1.0, 1.0]},
                     "Transform": {
                         "position": [1020.0, 16.0, -1105.4],
                         "scale": [24.0, 32.0, 24.0],
                         "rotation": [0.0, 0.0, 0.0, 1.0]}}},
            ],
        }
        roadnet = {
            "nodes": [{"id": 0, "x": -50.0, "z": 0.0, "kind": "intersection",
                       "signal": True},
                      {"id": 1, "x": 50.0, "z": 0.0, "kind": "intersection",
                       "signal": False}],
            "edges": [{"id": 0, "from": 0, "to": 1, "lanes_per_dir": 1,
                       "speed_limit": 13.9, "width": 8.0}],
        }
        q = {"elements": [
            mkway(101, LON + 0.01, LAT + 0.01, 0.0004, 0.0004),      # dup
            mkway(102, LON - 0.01, LAT - 0.01, 0.0003, 0.0003),      # dup
            mkway(105, LON + 0.01, LAT - 0.01, 0.0002, 0.0002),      # dup
            mkway(103, LON, LAT, 0.0005, 0.0005, height="48.5"),     # new
            mkway(104, LON + 0.02, LAT + 0.02, 0.00005, 0.00005),    # tiny
        ]}
        sp = td / "s.scene"
        sp.write_text(json.dumps(scene), encoding="utf-8")
        rp = td / "r.json"
        rp.write_text(json.dumps(roadnet), encoding="utf-8")
        qp = td / "q.json"
        qp.write_text(json.dumps(q), encoding="utf-8")
        o1 = td / "o1.scene"
        o2 = td / "o2.scene"
        s1 = build_city(sp, rp, [qp], o1, 20260717)
        s2 = build_city(sp, rp, [qp], o2, 20260717)
        b1 = o1.read_bytes()
        assert b1 == o2.read_bytes(), "determinism violated"
        assert s1["calib_residual_m"] < 2.0, s1["calib_residual_m"]
        assert s1["dup_scene"] == 3 and s1["real_added"] == 1, s1
        assert s1["skipped_tiny"] == 1, s1
        d = json.loads(b1.decode("utf-8"))
        new = [e for e in d["entities"] if e["name"] == "bld_103"]
        assert len(new) == 1
        tr = new[0]["components"]["Transform"]
        assert abs(tr["scale"][1] - 48.5) < 1e-6, tr
        assert abs(tr["position"][1] - 24.25) < 1e-6, tr
        # rotated-rect yaw of an axis-aligned quad is ~0 or ~pi/2
        q4 = new[0]["components"]["Transform"]["rotation"]
        ang = 2.0 * math.atan2(q4[1], q4[3])
        assert min(abs(ang), abs(abs(ang) - math.pi)) < math.radians(2), q4
        kinds = {}
        for e in d["entities"]:
            k = e["name"].split("_")[0]
            kinds[k] = kinds.get(k, 0) + 1
        assert kinds.get("road") == 1 and kinds.get("sigpole") == 1, kinds
        assert kinds.get("infill", 0) > 0, kinds
        print(f"real_city selftest: PASS (stats={s1})")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) >= 1 and argv[0] == "selftest":
        return selftest()
    if len(argv) >= 1 and argv[0] in ("-h", "--help"):
        print(USAGE)
        return 0
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene",
                    default=str(REPO_ROOT / "example/cg2-m0/city.scene.engine"))
    ap.add_argument("--roadnet",
                    default=str(REPO_ROOT / "example/cg2-m0/roadnet.json"))
    ap.add_argument("--osm",
                    default=str(REPO_ROOT / "modules/citygen/test/zjx-bld-q*.json"))
    ap.add_argument("--out",
                    default=str(REPO_ROOT / "example/cg2-m0/city.scene.real.engine"))
    ap.add_argument("--seed", type=int, default=20260717)
    a = ap.parse_args(argv)
    from glob import glob
    paths = sorted(Path(p) for p in glob(a.osm))
    if not paths:
        print(f"real_city: no OSM files match {a.osm}", file=sys.stderr)
        return 2
    stats = build_city(Path(a.scene), Path(a.roadnet), paths, Path(a.out),
                       a.seed)
    print(f"real_city: osm_input={stats['osm_input']} "
          f"real_added={stats['real_added']} dup_scene={stats['dup_scene']} "
          f"infill={stats['infill']} "
          f"skipped_overlap={stats['skipped_overlap']} "
          f"skipped_tiny={stats['skipped_tiny']} "
          f"roads={stats['roads']} signals={stats['signals']} "
          f"calib_pairs={stats['calib_pairs']} "
          f"calib_residual={stats['calib_residual_m']}m")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
