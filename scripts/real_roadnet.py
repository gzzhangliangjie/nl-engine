"""real_roadnet.py - M-CITY-ROAD: real OSM drivable roadnet -> engine roadnet.json.

Data source: E:/GuangzhouBusViewer/assets/_pull-roadnet/tile-*.json (64 tiles,
whole-Guangzhou `way["highway"]` with `out geom tags`, OSM snapshot
2026-07-15, ODbL). Same mirror-fetched corpus the M-CITY-DENS building pull
used, already on disk, so no network gamble.

Why this replaces the M-CG2 derived layer
-----------------------------------------
M-CG2 pulled only the ~2.3 km^2 bbox, which fragmented into 44 weak
components (dead ends / missing links), so the pipeline was forced to keep
just the largest component (199 nodes / 452 edges) and mirror every edge into
a two-way pair. This script instead pulls from the full-city corpus with a
PAD around the bbox, so interior roads connect through the surrounding ring;
the largest weak component then keeps the REAL topology (no fabrication).

Node identity = quantized coordinates (1e-5 deg, same as BusViewer's merge).
Out geom gives no OSM node ids, but ways that share an intersection share the
exact same coordinates, so quantization joins them.

Coordinates: x = A + B*lon, z = C + D*lat, fitted from the cg2 scene's
bld_<wayid> entities against OSM footprints (real_city.calibrate), i.e. the
exact frame the buildings use.

Signal flags: the existing cg2 roadnet.json carries 71 signal nodes derived
from real OSM traffic_signals nodes (M-T4). We invert the calibration to
recover their lon/lat and re-attach each one to the nearest new node, so the
signal set survives the topology swap.

Usage:
  python scripts/real_roadnet.py --stats          # data profile only
  python scripts/real_roadnet.py --out example/cg2-m0/roadnet.real.json
  python scripts/real_roadnet.py selftest
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

BUSVIEWER_TILES = Path("E:/GuangzhouBusViewer/assets/_pull-roadnet")

# bbox of the city area we build; same one the building OSM pull used.
LAT0, LAT1 = 23.108, 23.130
LON0, LON1 = 113.316, 113.342
# Surrounding ring pulled in so interior roads stay connected.
PAD_M = 1200.0

M_PER_DEG_LAT = 110540.0
M_PER_DEG_LON = 111320.0 * math.cos(math.radians(23.119))

# Drivable classes (cars + buses). Foot/cycle/path/track are excluded: the
# traffic sim must never route a car down a staircase.
DRIVE_CLASSES = {
    "motorway", "motorway_link", "trunk", "trunk_link",
    "primary", "primary_link", "secondary", "secondary_link",
    "tertiary", "tertiary_link", "unclassified", "residential",
    "living_street", "service", "road", "busway",
}

# Default speeds (m/s) when maxspeed is missing/unparsable.
DEFAULT_SPEED = {
    "motorway": 27.8, "motorway_link": 16.7,
    "trunk": 22.2, "trunk_link": 13.9,
    "primary": 16.7, "primary_link": 11.1,
    "secondary": 13.9, "secondary_link": 11.1,
    "tertiary": 11.1, "tertiary_link": 8.3,
    "unclassified": 11.1, "residential": 8.3,
    "living_street": 5.6, "service": 5.6, "road": 8.3, "busway": 11.1,
}

# Typical lane count by class when tags.lanes is absent.
DEFAULT_LANES = {
    "motorway": 4, "trunk": 3, "primary": 3, "secondary": 2,
    "tertiary": 2, "unclassified": 1, "residential": 1,
    "living_street": 1, "service": 1, "road": 1, "busway": 1,
}


def in_pad_bbox(lat: float, lon: float, pad_m: float = PAD_M) -> bool:
    dlat = pad_m / M_PER_DEG_LAT
    dlon = pad_m / M_PER_DEG_LON
    return (LAT0 - dlat) <= lat <= (LAT1 + dlat) and \
           (LON0 - dlon) <= lon <= (LON1 + dlon)


def load_ways(tiles_dir: Path, pad_m: float = PAD_M) -> list[dict]:
    """All drivable highway ways touching the padded bbox, deduped by way id.

    A way spanning several tiles is emitted in each tile with full geometry
    (Overpass `way(bbox)` does not clip), so dedupe by id is enough.
    """
    seen: set[int] = set()
    out: list[dict] = []
    for tf in sorted(tiles_dir.glob("tile-*.json")):
        data = json.loads(tf.read_text(encoding="utf-8"))
        for el in data.get("elements", []):
            if el.get("type") != "way":
                continue
            wid = el.get("id")
            if wid in seen:
                continue
            tags = el.get("tags") or {}
            hw = tags.get("highway")
            if hw not in DRIVE_CLASSES:
                continue
            if tags.get("access") in ("private", "no"):
                continue
            if tags.get("motor_vehicle") in ("no", "private"):
                continue
            g = el.get("geometry") or []
            if len(g) < 2:
                continue
            if not any(in_pad_bbox(p["lat"], p["lon"], pad_m) for p in g):
                continue
            seen.add(wid)
            out.append({"id": wid, "tags": tags, "geometry": g})
    out.sort(key=lambda w: w["id"])
    return out


def parse_maxspeed(tags: dict, hw: str) -> float:
    raw = str(tags.get("maxspeed", "")).strip().lower()
    if raw in ("none", "signals", "walk", ""):
        return DEFAULT_SPEED.get(hw, 8.3)
    digits = ""
    for ch in raw:
        if ch.isdigit():
            digits += ch
        elif digits:
            break
    if not digits:
        return DEFAULT_SPEED.get(hw, 8.3)
    kmh = float(digits)
    if "mph" in raw:
        kmh *= 1.609
    kmh = min(max(kmh, 5.0), 120.0)
    return round(kmh / 3.6, 2)


def parse_lanes(tags: dict, hw: str) -> int:
    for key in ("lanes:forward", "lanes"):
        raw = str(tags.get(key, "")).strip()
        digits = ""
        for ch in raw:
            if ch.isdigit():
                digits += ch
            elif digits:
                break
        if digits:
            n = int(digits)
            if key == "lanes":
                n = int(math.ceil(n / 2.0))   # total lanes -> per direction
            return max(1, min(n, 4))
    return DEFAULT_LANES.get(hw, 1)


def build_graph(ways: list[dict], pad_m: float = PAD_M) -> dict:
    """Quantized-node graph; returns raw nodes + edges (pre-component-cut)."""
    key_to_id: dict[tuple[int, int], int] = {}
    nodes: list[dict] = []
    edges: list[dict] = []

    def node_of(lat: float, lon: float) -> int:
        key = (int(round(lat * 1e5)), int(round(lon * 1e5)))
        nid = key_to_id.get(key)
        if nid is None:
            nid = len(nodes)
            key_to_id[key] = nid
            nodes.append({"id": nid, "lat": lat, "lon": lon})
        return nid

    for w in ways:
        tags = w["tags"]
        hw = tags["highway"]
        lanes = parse_lanes(tags, hw)
        speed = parse_maxspeed(tags, hw)
        width = round(min(max(lanes * 2 * 3.5, 4.0), 16.0), 1)
        oneway = tags.get("oneway") in ("yes", "1", "-1")
        prev = None
        for p in w["geometry"]:
            nid = node_of(p["lat"], p["lon"])
            if prev is not None and nid != prev:
                edges.append({"from": prev, "to": nid, "lanes_per_dir": lanes,
                              "speed_limit": speed, "width": width,
                              "oneway": oneway, "way_id": w["id"]})
            prev = nid
    return {"nodes": nodes, "edges": edges}


def largest_component(nodes: list[dict], edges: list[dict]) -> tuple[list, list, dict]:
    """Keep the largest weak component (union-find)."""
    parent = list(range(len(nodes)))

    def find(a: int) -> int:
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for e in edges:
        ra, rb = find(e["from"]), find(e["to"])
        if ra != rb:
            parent[ra] = rb
    comp: dict[int, int] = {}
    for i in range(len(nodes)):
        r = find(i)
        comp[r] = comp.get(r, 0) + 1
    sizes = sorted(comp.values(), reverse=True)
    biggest = max(comp.items(), key=lambda kv: kv[1])[0] if comp else None
    keep = {i for i in range(len(nodes)) if find(i) == biggest}
    kn = [n for n in nodes if n["id"] in keep]
    ke = [e for e in edges if e["from"] in keep and e["to"] in keep]
    return kn, ke, {"components": len(comp), "largest": len(keep),
                    "top_sizes": sizes[:5], "dropped_nodes": len(nodes) - len(keep)}


def compress_chains(nodes: list[dict], edges: list[dict]) -> tuple[list, list, dict]:
    """Collapse chains of degree-2 shape points into polyline edges.

    Engine topology wants junctions (degree != 2) as nodes; the dropped shape
    points become the edge's polyline so the renderer can still lay short
    slabs along the real curvature. Returns (junctions, polyline edges, stats)
    where each edge carries ``path`` = [(lat, lon), ...] including both ends.
    """
    adj: dict[int, list[int]] = {}
    for ei, e in enumerate(edges):
        adj.setdefault(e["from"], []).append(ei)
        adj.setdefault(e["to"], []).append(ei)
    is_junction = {n: (len(adj.get(n, [])) != 2) for n in adj}
    by_id = {n["id"]: n for n in nodes}

    def other(e, n):
        return e["to"] if e["from"] == n else e["from"]

    used = [False] * len(edges)
    out_edges: list[dict] = []
    # 1) start from junctions
    for n in sorted(is_junction):
        if not is_junction[n]:
            continue
        for ei in adj[n]:
            if used[ei]:
                continue
            e = edges[ei]
            used[ei] = True
            path = [n, other(e, n)]
            cur = path[-1]
            props = e
            # walk through degree-2 nodes
            while not is_junction.get(cur, True):
                nxt = None
                for ej in adj.get(cur, []):
                    if not used[ej]:
                        o = other(edges[ej], cur)
                        nxt = (ej, o)
                        break
                if nxt is None:
                    break
                used[nxt[0]] = True
                cur = nxt[1]
                path.append(cur)
            out_edges.append({"from": path[0], "to": path[-1],
                              "lanes_per_dir": props["lanes_per_dir"],
                              "speed_limit": props["speed_limit"],
                              "width": props["width"],
                              "way_id": props.get("way_id", 0),
                              "oneway": props.get("oneway", False),
                              "path": [(by_id[c]["lat"], by_id[c]["lon"])
                                       for c in path]})
    # 2) pure loops with no junction at all
    for ei, e in enumerate(edges):
        if used[ei]:
            continue
        used[ei] = True
        path = [e["from"], e["to"]]
        cur = e["to"]
        while cur != e["from"]:
            nxt = None
            for ej in adj.get(cur, []):
                if not used[ej]:
                    nxt = (ej, other(edges[ej], cur))
                    break
            if nxt is None:
                break
            used[nxt[0]] = True
            cur = nxt[1]
            path.append(cur)
        out_edges.append({"from": path[0], "to": path[-1],
                          "lanes_per_dir": e["lanes_per_dir"],
                          "speed_limit": e["speed_limit"],
                          "width": e["width"],
                          "way_id": e.get("way_id", 0),
                          "oneway": e.get("oneway", False),
                          "path": [(by_id[c]["lat"], by_id[c]["lon"])
                                   for c in path]})
    kept = sorted({e["from"] for e in out_edges} | {e["to"] for e in out_edges})
    out_nodes = [by_id[k] for k in kept]
    return out_nodes, out_edges, {"junctions": len(out_nodes),
                                  "edges": len(out_edges),
                                  "shape_points_folded": len(nodes) - len(out_nodes)}


def to_engine_roadnet(nodes: list[dict], edges: list[dict], A: float, B: float,
                      C: float, D: float,
                      signals: list[tuple[float, float]] | None = None,
                      respect_oneway: bool = False) -> dict:
    """Map lat/lon nodes into scene x/z; attach signal flags by proximity.

    Edges keep their polyline in an extra ``shape`` array (scene x/z pairs,
    ends included). LaneGraph only reads id/from/to/lanes/speed/width, so the
    extra key is inert for the traffic sim but lets the renderer lay slabs
    along the real curvature instead of cutting corners.
    """
    out_nodes = []
    for n in sorted(nodes, key=lambda n: n["id"]):
        out_nodes.append({"id": n["id"],
                          "x": round(A + B * n["lon"], 2),
                          "z": round(C + D * n["lat"], 2),
                          "kind": "intersection", "signal": False})
    if signals:
        by_id = {n["id"]: n for n in out_nodes}
        for (slon, slat) in signals:
            sx = A + B * slon
            sz = C + D * slat
            best = None
            for n in out_nodes:
                d = math.hypot(n["x"] - sx, n["z"] - sz)
                if best is None or d < best[0]:
                    best = (d, n["id"])
            if best and best[0] <= 30.0:
                by_id[best[1]]["signal"] = True

    seen: set[tuple[int, int, int]] = set()
    out_edges = []
    for e in edges:
        if e["from"] == e["to"]:
            continue                      # self loops carry no drivable span
        key = (min(e["from"], e["to"]), max(e["from"], e["to"]),
               int(e.get("way_id", 0)))
        if key in seen:
            continue
        seen.add(key)
        # Engine edges are DIRECTED (LaneGraph: edge = one travel direction, and
        # no-U-turn linking only works on a directed pair). OSM draws two-way
        # roads once, so emit the twin here — same convention as the M-CG2
        # derived roadnet. Without the twin every edge is its own strongly
        # connected component and A* can never leave the start edge
        # (measured: 3931/3931 isolated). `oneway=yes` roads get no twin, and
        # the twin is tagged `reverse` so the scene builder lays one slab only.
        dirs = [(e["from"], e["to"], e.get("path"), False)]
        if not (respect_oneway and e.get("oneway")):
            pth = e.get("path")
            dirs.append((e["to"], e["from"],
                         list(reversed(pth)) if pth else None, True))
        for (fr, to, pth, is_rev) in dirs:
            rec = {"id": len(out_edges), "from": fr, "to": to,
                   "lanes_per_dir": e["lanes_per_dir"],
                   "speed_limit": e["speed_limit"], "width": e["width"]}
            if is_rev:
                rec["reverse"] = True
            if pth and len(pth) > 2:
                rec["shape"] = [[round(A + B * lon, 2), round(C + D * lat, 2)]
                                for (lat, lon) in pth]
            out_edges.append(rec)
    out_edges.sort(key=lambda e: (e["from"], e["to"], e["id"]))
    for i, e in enumerate(out_edges):
        e["id"] = i
    xs = [n["x"] for n in out_nodes] or [0.0]
    zs = [n["z"] for n in out_nodes] or [0.0]
    return {"version": 1, "seed": 0, "units": "meter",
            "blocks": {"x": round(max(xs) - min(xs), 2),
                       "z": round(max(zs) - min(zs), 2)},
            "nodes": out_nodes, "edges": out_edges, "phases": []}


def scene_calibration(scene_path: Path, osm_glob: list[Path]) -> tuple:
    from real_city import calibrate, load_building_ways
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    ways = load_building_ways(osm_glob)
    A, B, C, D, res, pairs = calibrate(scene["entities"], ways)
    return A, B, C, D, res, len(pairs)


def invert_signal_nodes(roadnet_path: Path, A: float, B: float, C: float, D: float):
    """Recover lon/lat of the existing roadnet's signal nodes."""
    rn = json.loads(roadnet_path.read_text(encoding="utf-8"))
    out = []
    for n in rn["nodes"]:
        if not n.get("signal"):
            continue
        lon = (n["x"] - A) / B
        lat = (n["z"] - C) / D
        out.append((lon, lat))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiles", default=str(BUSVIEWER_TILES))
    ap.add_argument("--scene", default=str(REPO_ROOT / "example/cg2-m0/city.scene.engine"))
    ap.add_argument("--signal-source", default=str(REPO_ROOT / "example/cg2-m0/roadnet.json"))
    ap.add_argument("--osm", nargs="*", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--pad", type=float, default=PAD_M)
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--respect-oneway", action="store_true",
                    help="keep OSM oneway roads single-direction (default: "
                         "mirror them; measured SCC 9.4%% vs 86.3%%)")
    a = ap.parse_args()

    tiles = Path(a.tiles)
    ways = load_ways(tiles, a.pad)
    print(f"drivable ways in padded bbox: {len(ways)}")
    g = build_graph(ways, a.pad)
    nodes, edges = g["nodes"], g["edges"]
    print(f"raw nodes={len(nodes)} raw edges={len(edges)}")
    kn, ke, comp = largest_component(nodes, edges)
    print(f"components={comp['components']} largest={comp['largest']} "
          f"top={comp['top_sizes']} dropped={comp['dropped_nodes']}")
    deg: dict[int, int] = {}
    for e in ke:
        deg[e["from"]] = deg.get(e["from"], 0) + 1
        deg[e["to"]] = deg.get(e["to"], 0) + 1
    d_hist: dict[int, int] = {}
    for v in deg.values():
        d_hist[v] = d_hist.get(v, 0) + 1
    print("degree hist:", dict(sorted(d_hist.items())[:8]))
    by_id = {n["id"]: n for n in kn}
    tot_len = sum(math.hypot((by_id[e['from']]['lat'] - by_id[e['to']]['lat']) * M_PER_DEG_LAT,
                             (by_id[e['from']]['lon'] - by_id[e['to']]['lon']) * M_PER_DEG_LON)
                  for e in ke)
    print(f"total length {tot_len/1000:.1f} km")
    cn, ce, cst = compress_chains(kn, ke)
    long_e = [e for e in ce if e['from'] != e['to']]
    avg_pts = sum(len(e["path"]) for e in ce) / max(len(ce), 1)
    print(f"compressed: junctions={cst['junctions']} edges={cst['edges']} "
          f"folded={cst['shape_points_folded']} avg_path_pts={avg_pts:.1f} "
          f"self_loops={len(ce) - len(long_e)}")
    if a.stats or not a.out:
        return 0

    osm_glob = ([Path(p) for p in a.osm] if a.osm else
                sorted((REPO_ROOT / "modules/citygen/test").glob("zjx-bld-q*.json")))
    A, B, C, D, res, npairs = scene_calibration(Path(a.scene), osm_glob)
    print(f"calibration: residual {res:.3f} m on {npairs} pairs "
          f"(B={B:.3f} m/deg lon, D={D:.3f} m/deg lat)")
    sig = invert_signal_nodes(Path(a.signal_source), A, B, C, D)
    rn = to_engine_roadnet(cn, ce, A, B, C, D, sig, a.respect_oneway)
    nsig = sum(1 for n in rn["nodes"] if n["signal"])
    shaped = sum(1 for e in rn["edges"] if e.get("shape"))
    # slab budget: edges whose polyline deviates > 1.5 m from the chord get
    # split into per-segment slabs by the scene builder.
    split = 0
    for e in rn["edges"]:
        sh = e.get("shape")
        if not sh or len(sh) < 3:
            continue
        (x0, z0), (x1, z1) = sh[0], sh[-1]
        cl = math.hypot(x1 - x0, z1 - z0) or 1.0
        dev = max(abs((x1 - x0) * (z0 - p[1]) - (x0 - p[0]) * (z1 - z0)) / cl
                  for p in sh[1:-1])
        if dev > 1.5:
            split += 1
    slabs = len(rn["edges"]) + sum(len(e["shape"]) - 2 for e in rn["edges"]
                                   if e.get("shape"))
    out = Path(a.out)
    out.write_text(json.dumps(rn, ensure_ascii=False, indent=1) + "\n",
                   encoding="utf-8", newline="\n")
    print(f"wrote {out}: nodes={len(rn['nodes'])} edges={len(rn['edges'])} "
          f"signals={nsig} (from {len(sig)} source signals)")
    print(f"shape: {shaped} edges carry polylines, {split} curve >1.5 m "
          f"(worst-case slab count {slabs})")
    xs = [n["x"] for n in rn["nodes"]]
    zs = [n["z"] for n in rn["nodes"]]
    print(f"extent x {min(xs):.0f}..{max(xs):.0f} z {min(zs):.0f}..{max(zs):.0f}")
    return 0


def selftest() -> int:
    import tempfile
    nodes = [
        {"id": 0, "lat": 23.100, "lon": 113.300},
        {"id": 1, "lat": 23.100, "lon": 113.301},
        {"id": 2, "lat": 23.101, "lon": 113.301},
        {"id": 3, "lat": 23.105, "lon": 113.305},   # isolated island
    ]
    edges = [
        {"from": 0, "to": 1, "lanes_per_dir": 2, "speed_limit": 13.9, "width": 8.0},
        {"from": 1, "to": 2, "lanes_per_dir": 1, "speed_limit": 8.3, "width": 4.0},
        {"from": 1, "to": 2, "lanes_per_dir": 1, "speed_limit": 8.3, "width": 4.0},
    ]
    kn, ke, comp = largest_component(nodes, edges)
    assert len(kn) == 3 and comp["components"] == 2 and comp["dropped_nodes"] == 1, comp
    rn = to_engine_roadnet(kn, ke, A=0.0, B=111320.0, C=0.0, D=110540.0)
    assert len(rn["nodes"]) == 3, rn["nodes"]
    # duplicate physical edge deduped, then each road -> two directed edges
    assert len(rn["edges"]) == 4, rn["edges"]
    assert rn["edges"][0]["from"] == 0 and rn["edges"][0]["to"] == 1
    assert sum(1 for e in rn["edges"] if e.get("reverse")) == 2, rn["edges"]
    # oneway: mirrored by default (engine needs a directed pair), single only
    # when respect_oneway is requested
    ow = [{"from": 0, "to": 1, "lanes_per_dir": 1, "speed_limit": 8.3,
           "width": 4.0, "way_id": 9, "oneway": True}]
    assert len(to_engine_roadnet(kn, ow, 0.0, 111320.0, 0.0, 110540.0)["edges"]) == 2
    assert len(to_engine_roadnet(kn, ow, 0.0, 111320.0, 0.0, 110540.0,
                                 respect_oneway=True)["edges"]) == 1
    # lat/lon -> meters relative to (0,0)
    n1 = [n for n in rn["nodes"] if n["id"] == 1][0]
    assert abs(n1["x"] - 113.301 * 111320.0) < 1.0, n1
    # signal attach: one point 10 m from node 1
    sig_lon = 113.301
    sig_lat = 23.100 + 10.0 / 110540.0
    rn2 = to_engine_roadnet(kn, ke, 0.0, 111320.0, 0.0, 110540.0,
                            signals=[(sig_lon, sig_lat)])
    assert sum(1 for n in rn2["nodes"] if n["signal"]) == 1, rn2["nodes"]
    # attribute parsing
    assert parse_lanes({"lanes": "6"}, "primary") == 3
    assert parse_lanes({}, "residential") == 1
    assert parse_maxspeed({"maxspeed": "50"}, "primary") == 13.89
    assert parse_maxspeed({}, "motorway") == 27.8
    assert parse_maxspeed({"maxspeed": "30 mph"}, "primary") == round(48.28 / 3.6, 2)
    # chain compression: 0-1-2 straight chain collapses into one edge 0->2
    pts = [(23.100, 113.300), (23.100, 113.3005), (23.100, 113.301)]
    cn = [{"id": i, "lat": la, "lon": lo} for i, (la, lo) in enumerate(pts)]
    ce = [{"from": 0, "to": 1, "lanes_per_dir": 1, "speed_limit": 8.3,
           "width": 4.0, "way_id": 7},
          {"from": 1, "to": 2, "lanes_per_dir": 1, "speed_limit": 8.3,
           "width": 4.0, "way_id": 7}]
    jn, je, jst = compress_chains(cn, ce)
    assert len(jn) == 2 and len(je) == 1, (jn, je)
    assert je[0]["from"] == 0 and je[0]["to"] == 2, je[0]
    assert len(je[0]["path"]) == 3, je[0]["path"]
    # self loops dropped, shape emitted into the engine record
    rn3 = to_engine_roadnet(jn, je + [{"from": 0, "to": 0, "lanes_per_dir": 1,
                                       "speed_limit": 8.3, "width": 4.0}],
                            0.0, 111320.0, 0.0, 110540.0)
    assert len(rn3["edges"]) == 2, rn3["edges"]   # self loop dropped, twin added
    assert len(rn3["edges"][0]["shape"]) == 3, rn3["edges"][0]
    assert rn3["blocks"]["x"] > 0 and rn3["blocks"]["z"] == 0.0, rn3["blocks"]
    print("selftest OK")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        sys.exit(selftest())
    sys.exit(main())
