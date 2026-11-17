#!/usr/bin/env python3
"""max_city.py - M-CITY-MAX: cluster the 3d66 Zhujiang-New-Town .max node
dump into whole buildings and test whether the model can be georeferenced.

VERDICT (2026-11-17): the model is REAL-SCALE (east tower 530 m, west tower
438 m, tower pair separation 304 m vs 297 m real) but its building LAYOUT is
artistic, not surveyed. Exhaustive checks:
  - 8 axis conventions (0/90/180/270 deg x Y-flip) x scales 0.90..1.10:
    best median nearest-OSM residual 298 m;
  - core-only (28 clusters within 1.5 km of the tower pair, merged blobs
    excluded): best 62 m median, p90 613 m;
  - random-point baseline in the same bbox: 62 m median.
=> indistinguishable from random placement. Use the model ONLY for shape /
height reference, NEVER as a positional data source. OSM stays the sole
positional truth for the city.

The clustering + calibration code below is kept as the analysis tool that
produced that verdict (and reused if a properly surveyed max scene appears).

Input: .workbuddy/max-nodes.txt written by dump_max_nodes.ms (3dsmaxbatch):
    name|class|minx,miny,minz|maxx,maxy,maxz|dx,dy,dz
The max scene uses real-world scale, X = east, Y = south (CAD screen frame;
verified from the tower pair sign), Z = up.

Pipeline:
  1. filter: keep real geometry classes, drop Missing_GeomObject placeholders
     (18804 of 21545 nodes have degenerate 60+ km proxy bounds), cameras,
     dummies; keep nodes with 5 m <= horiz <= 400 m and h >= 8 m.
  2. cluster: union-find joins nodes whose XY boxes (grown 2.5 m) overlap and
     whose Z ranges touch/overlap -> one building per cluster (podium+tower
     parts merge).
  3. calibrate: seed translation = centroid difference vs the OSM set;
     two rounds of nearest-neighbour matching (<=120 m) + per-axis scale /
     offset least squares (no rotation; towers verified axis-aligned).
  4. emit: axis-aligned max box corners transformed to (lon, lat) + height,
     one json list for real_city.py stage 1b.

usage:
  max_city.py [--dump PATH] [--osm GLOB] [--out PATH]
  max_city.py selftest
exit codes: 0 ok / 1 runtime failure / 2 usage
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
from real_city import convex_hull, load_building_ways, min_area_rect  # noqa: E402

USAGE = """max_city - cluster .max node dump into buildings, calibrate to WGS84
usage:
  max_city.py [--dump PATH] [--osm GLOB] [--out PATH]
  max_city.py selftest
"""

KEEP_CLASSES = {"Editable_mesh", "Editable_Poly", "PolyMeshObject", "Box"}
MIN_H = 8.0
HORIZ_MAX = 400.0
CLUSTER_GAP = 2.5       # XY grow before overlap test
MATCH_RADIUS = 120.0    # nearest-neighbour pairing radius (m)
LAT0 = 23.119
M_PER_DEG_LAT = 110540.0
M_PER_DEG_LON = 111320.0 * math.cos(math.radians(LAT0))


# ---------------------------------------------------------------------------
# parse + filter
# ---------------------------------------------------------------------------
def load_max_nodes(dump_path: Path) -> list[dict]:
    """Parse the pipe dump; keep real-geometry nodes only."""
    out = []
    with dump_path.open(encoding="utf-8", errors="replace") as f:
        f.readline()  # sceneObjects= header
        for line in f:
            p = line.rstrip("\n").split("|")
            if len(p) != 5 or p[1] not in KEEP_CLASSES:
                continue
            try:
                bmin = [float(x) for x in p[2].split(",")]
                bmax = [float(x) for x in p[3].split(",")]
            except ValueError:
                continue
            out.append({"name": p[0], "cls": p[1],
                        "bmin": bmin, "bmax": bmax})
    return out


def filter_building_nodes(nodes: list[dict]) -> list[dict]:
    keep = []
    for n in nodes:
        dx = n["bmax"][0] - n["bmin"][0]
        dy = n["bmax"][1] - n["bmin"][1]
        dz = n["bmax"][2] - n["bmin"][2]
        if dz < MIN_H:
            continue
        if not (5.0 <= max(dx, dy) <= HORIZ_MAX):
            continue
        keep.append(n)
    return keep


# ---------------------------------------------------------------------------
# clustering
# ---------------------------------------------------------------------------
def _xy_overlap(a: dict, b: dict, grow: float) -> bool:
    return (a["bmin"][0] - grow < b["bmax"][0]
            and b["bmin"][0] - grow < a["bmax"][0]
            and a["bmin"][1] - grow < b["bmax"][1]
            and b["bmin"][1] - grow < a["bmax"][1])


def _z_touch(a: dict, b: dict, gap: float = 3.0) -> bool:
    return (a["bmin"][2] - gap < b["bmax"][2]
            and b["bmin"][2] - gap < a["bmax"][2])


def cluster_buildings(nodes: list[dict]) -> list[dict]:
    """Union-find on XY proximity + Z contact; returns merged buildings."""
    n = len(nodes)
    parent = list(range(n))

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    # sweep by x-min to keep the pair scan near-linear
    order = sorted(range(n), key=lambda i: nodes[i]["bmin"][0])
    for ai, i in enumerate(order):
        a = nodes[i]
        for bj in range(ai + 1, n):
            j = order[bj]
            b = nodes[j]
            if b["bmin"][0] - a["bmax"][0] > CLUSTER_GAP:
                break
            if _xy_overlap(a, b, CLUSTER_GAP) and _z_touch(a, b):
                ri, rj = find(i), find(j)
                if ri != rj:
                    parent[ri] = rj

    groups: dict[int, list[dict]] = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(nodes[i])

    blds = []
    for members in groups.values():
        x0 = min(m["bmin"][0] for m in members)
        y0 = min(m["bmin"][1] for m in members)
        z0 = min(m["bmin"][2] for m in members)
        x1 = max(m["bmax"][0] for m in members)
        y1 = max(m["bmax"][1] for m in members)
        z1 = max(m["bmax"][2] for m in members)
        blds.append({"x0": x0, "y0": y0, "x1": x1, "y1": y1,
                     "h": round(z1 - z0, 2), "base": round(z0, 2),
                     "parts": len(members)})
    blds.sort(key=lambda b: (b["x0"], b["y0"]))
    return blds


# ---------------------------------------------------------------------------
# calibration: max XY (m) -> (lon, lat)
# ---------------------------------------------------------------------------
def fit_max_to_geo(blds: list[dict], ways: list[dict]) -> tuple:
    """Offset-search calibration: brute-force the translation, then LSQ refine.

    Neither source has reliable per-building identity, so the seed is found by
    scoring candidate offsets (each = one tall max building vs one OSM
    centroid) with a spatial hash, then two rounds of NN matching + per-axis
    least squares. No landmark ids hardcoded.

    Returns (ex, ey, sx, sy, residual_max, pairs) with
      lon = lon_c + (ex + sx * mx) / M_PER_DEG_LON
      lat = lat_c + (ey + sy * my) / M_PER_DEG_LAT
    where (mx, my) are max coords relative to the max centroid.
    """
    lon_c, lat_c = 113.0, 23.0
    osm_pts = []
    for w in ways:
        g = w["geometry"]
        osm_pts.append(((min(p["lon"] for p in g) + max(p["lon"] for p in g)) / 2,
                        (min(p["lat"] for p in g) + max(p["lat"] for p in g)) / 2))
    # OSM points in meter frame, spatial hash
    osm_m = [((lon - lon_c) * M_PER_DEG_LON, (lat - lat_c) * M_PER_DEG_LAT)
             for lon, lat in osm_pts]
    CELL = 120.0
    grid: dict[tuple[int, int], list[tuple[float, float]]] = {}
    for (e, n) in osm_m:
        grid.setdefault((int(math.floor(e / CELL)), int(math.floor(n / CELL))),
                        []).append((e, n))

    def nn(e: float, n: float, radius: float):
        cx = int(math.floor(e / CELL))
        cz = int(math.floor(n / CELL))
        r = int(math.ceil(radius / CELL))
        best = None
        for ix in range(cx - r, cx + r + 1):
            for iz in range(cz - r, cz + r + 1):
                for (oe, on) in grid.get((ix, iz), ()):
                    d = math.hypot(oe - e, on - n)
                    if d <= radius and (best is None or d < best[0]):
                        best = (d, oe, on)
        return best

    mcx = sum((b["x0"] + b["x1"]) / 2 for b in blds) / len(blds)
    mcy = sum((b["y0"] + b["y1"]) / 2 for b in blds) / len(blds)
    pts = [((b["x0"] + b["x1"]) / 2 - mcx, (b["y0"] + b["y1"]) / 2 - mcy,
            b["h"]) for b in blds]
    tall = sorted(pts, key=lambda p: -p[2])[:5]
    solid = [p for p in pts if p[2] >= 30.0]

    # --- seed: for each (tall max bld, OSM point) candidate offset, score ---
    best_off = None
    for (ax, ay, _) in tall:
        for (oe, on) in osm_m:
            ox, oy = oe - ax, on - ay
            score = 0
            for (px, py, _) in solid:
                if nn(px + ox, py + oy, 35.0):
                    score += 1
            if best_off is None or score > best_off[0]:
                best_off = (score, ox, oy)
    _, ex, ey = best_off if best_off else (0, 0.0, 0.0)

    sx = sy = 1.0
    pairs = []
    res_max = 1e9
    for _ in range(3):
        pairs = []
        for (px, py, _) in pts:
            e = ex + sx * px
            nn_ = ey + sy * py
            hit = nn(e, nn_, MATCH_RADIUS)
            if hit:
                pairs.append((px, py, hit[1], hit[2]))
        if len(pairs) < 2:
            break
        for axis in (0, 1):
            src = [p[axis] for p in pairs]
            dst = [p[2 + axis] for p in pairs]
            n = len(pairs)
            ss = sum(src)
            ss2 = sum(v * v for v in src)
            sd = sum(dst)
            ssd = sum(a * b for a, b in zip(src, dst))
            det = n * ss2 - ss * ss
            if abs(det) < 1e-9:
                continue
            k = (n * ssd - ss * sd) / det
            c = (sd - k * ss) / n
            if axis == 0:
                sx, ex = k, c
            else:
                sy, ey = k, c
        res_max = max(math.hypot(ex + sx * p[0] - p[2], ey + sy * p[1] - p[3])
                      for p in pairs)
    return ex, ey, sx, sy, res_max, pairs


def to_geo(blds: list[dict], fit: tuple) -> list[dict]:
    ex, ey, sx, sy, _, _ = fit
    out = []
    for i, b in enumerate(blds):
        corners = []
        for (mx, my) in ((b["x0"], b["y0"]), (b["x1"], b["y0"]),
                         (b["x1"], b["y1"]), (b["x0"], b["y1"])):
            e = ex + sx * (mx - _mcx(blds))
            nn = ey + sy * (my - _mcy(blds))
            corners.append([113.0 + e / M_PER_DEG_LON,
                            23.0 + nn / M_PER_DEG_LAT])
        out.append({"id": i + 1, "h": b["h"], "corners": corners})
    return out


def _mcx(blds: list[dict]) -> float:
    return sum((b["x0"] + b["x1"]) / 2 for b in blds) / len(blds)


def _mcy(blds: list[dict]) -> float:
    return sum((b["y0"] + b["y1"]) / 2 for b in blds) / len(blds)


# ---------------------------------------------------------------------------
# build + CLI
# ---------------------------------------------------------------------------
def build_max_bld(dump_path: Path, osm_paths: list[Path],
                  out_path: Path) -> dict:
    nodes = load_max_nodes(dump_path)
    bnodes = filter_building_nodes(nodes)
    blds = cluster_buildings(bnodes)
    ways = load_building_ways(osm_paths)
    fit = fit_max_to_geo(blds, ways)
    geo = to_geo(blds, fit)
    out_path.write_text(json.dumps(geo, ensure_ascii=False, indent=1) + "\n",
                        encoding="utf-8", newline="\n")
    stats = {"nodes_total": sum(1 for _ in _iter_lines(dump_path)),
             "geom_nodes": len(nodes), "bld_nodes": len(bnodes),
             "clusters": len(blds), "fit_pairs": len(fit[5]),
             "fit_residual_m": round(fit[4], 1), "emitted": len(geo)}
    return stats


def _iter_lines(dump_path: Path):
    with dump_path.open(encoding="utf-8", errors="replace") as f:
        for _ in f:
            yield None


def selftest() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        # synthetic max scene: east tower (530m) at max(-115,-104),
        # west tower (438m) at (-418,-77); true transform is identity
        # (max meters == geo meters offset by (113.32076, 23.12052) towers).
        lines = ["sceneObjects=5\n"]
        # east tower: 63x60 footprint
        lines.append("Obj-1|Editable_mesh|-146,-134,0|-84,-74,530|62,60,530\n")
        # west tower split into two stacked parts (podium + shaft)
        lines.append("Obj-2a|Editable_mesh|-458,-117,0|-378,-37,40|80,80,40\n")
        lines.append("Obj-2b|Editable_mesh|-458,-117,40|-378,-37,438|80,80,398\n")
        # noise: small prop + missing placeholder
        lines.append("Obj-3|Editable_mesh|0,500,0|10,510,12|10,10,12\n")
        lines.append("Obj-4|Missing_GeomObject|-100,-100,0|70000,70000,60000|70100,70100,60000\n")
        dp = td / "dump.txt"
        dp.write_text("".join(lines), encoding="utf-8")

        # OSM: towers at their real positions (delta east=296m, north=30m
        # matches max delta (303, 27) within tolerance)
        def mkway(wid, lon, lat, dlon_m, dlat_m, height=""):
            tags = {"building": "yes"}
            if height:
                tags["height"] = height
            hl = dlon_m / 2 / 102040.0
            ha = dlat_m / 2 / 110540.0
            return {"type": "way", "id": wid, "tags": tags,
                    "geometry": [
                        {"lon": lon - hl, "lat": lat - ha},
                        {"lon": lon + hl, "lat": lat - ha},
                        {"lon": lon + hl, "lat": lat + ha},
                        {"lon": lon - hl, "lat": lat + ha},
                    ]}
        LON_E, LAT_E = 113.32076, 23.12052
        LON_W = LON_E - 296.0 / 102040.0
        LAT_W = LAT_E + 30.0 / 110540.0
        q = {"elements": [
            mkway(1, LON_E, LAT_E, 60, 60, height="530"),
            mkway(2, LON_W, LAT_W, 80, 80, height="438"),
        ]}
        qp = td / "q.json"
        qp.write_text(json.dumps(q), encoding="utf-8")

        stats = build_max_bld(dp, [qp], td / "max-bld.json")
        assert stats["geom_nodes"] == 4, stats      # placeholder dropped
        assert stats["bld_nodes"] == 4, stats
        assert stats["clusters"] == 3, stats        # west tower parts merged
        assert stats["fit_residual_m"] < 15.0, stats
        geo = json.loads((td / "max-bld.json").read_text(encoding="utf-8"))
        assert len(geo) == 3
        # east tower (tallest) lands within 30 m of its OSM center
        e0 = max(geo, key=lambda b: b["h"])
        cx = sum(c[0] for c in e0["corners"]) / 4
        cy = sum(c[1] for c in e0["corners"]) / 4
        assert abs(cx - LON_E) < 30 / 102040.0, (cx, LON_E)
        assert abs(cy - LAT_E) < 30 / 110540.0, (cy, LAT_E)
        assert abs(e0["h"] - 530.0) < 1e-6, e0
        print(f"max_city selftest: PASS (stats={stats})")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) >= 1 and argv[0] == "selftest":
        return selftest()
    if len(argv) >= 1 and argv[0] in ("-h", "--help"):
        print(USAGE)
        return 0
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump",
                    default=str(REPO_ROOT / ".workbuddy/max-nodes.txt"))
    ap.add_argument("--osm",
                    default=str(REPO_ROOT / "modules/citygen/test/zjx-bld-q*.json"))
    ap.add_argument("--out",
                    default=str(REPO_ROOT / "modules/citygen/test/max-bld.json"))
    a = ap.parse_args(argv)
    from glob import glob
    paths = sorted(Path(p) for p in glob(a.osm))
    if not paths:
        print(f"max_city: no OSM files match {a.osm}", file=sys.stderr)
        return 2
    stats = build_max_bld(Path(a.dump), paths, Path(a.out))
    print(f"max_city: nodes_total={stats['nodes_total']} "
          f"geom_nodes={stats['geom_nodes']} bld_nodes={stats['bld_nodes']} "
          f"clusters={stats['clusters']} fit_pairs={stats['fit_pairs']} "
          f"fit_residual={stats['fit_residual_m']}m "
          f"emitted={stats['emitted']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
