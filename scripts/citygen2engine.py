#!/usr/bin/env python3
"""citygen2engine.py - M-CG2 engine delivery driver (citygen track).

Read-only consumer of citygen products (example/cg1-m0/ four-piece bundle):
  city.scene (E1 World JSON, frozen)  ->  city.scene.engine
    - box mesh "buildings/box.obj" -> flat whitelist glb (height tier)
    - landmark mesh "canton_tower.glb" kept verbatim
    - N car entities appended (viewer --traffic-city requires >=1 car_*)
  roadnet.json / ODbL / whitelist / top.png copied byte-faithful.
  city_delivery.png = evidence map driven by real engine-scene transforms.

Per Q-CG2A=A: viewer/renderer/core zero source change; frozen cg0-m0/cg1-m0
products consumed read-only. Per Q-CG2B=1 this lives on the citygen track.

usage:
  citygen2engine.py
  citygen2engine.py selftest [--verbose]
  citygen2engine.py drive --src DIR --out DIR [--models-out DIR] [--stats]
exit codes: 0 ok / 1 runtime failure / 2 usage or invalid arguments
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import sys
import tempfile
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = Path(__file__).resolve().parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import obj2glb  # noqa: E402  (sibling script, W1 asset-ingest chain)

USAGE = """citygen2engine - nl-engine citygen tools (M-CG2 engine delivery driver)
usage:
  citygen2engine.py
  citygen2engine.py selftest [--verbose]
  citygen2engine.py drive --src DIR --out DIR [--models-out DIR] [--stats]
examples:
  citygen2engine.py drive --src example/cg1-m0 --out example/cg2-m0
  citygen2engine.py selftest --verbose
exit codes: 0 ok / 1 runtime failure / 2 usage or invalid arguments
"""

# Mirror of nl::scene::validAssetFileName (flat ASCII, no separators, no '..')
GLB_NAME_RE = re.compile(r"^[A-Za-z0-9_-]+\.glb$")
BOX_MESH_JSON = '"mesh": "buildings/box.obj"'
BOX_MESH_RE = re.compile(re.escape(BOX_MESH_JSON))
NEXTID_RE = re.compile(r'"nextID":\s*(\d+)')
ENTITIES_END = '\n  ]\n}'

# Height tier thresholds (metres). W1 used density tiers from city-tools
# buildingTier; real OSM data has no density field, so derive from height.
TIER_LIMITS = (30.0, 80.0)          # <30 -> tier1, 30..80 -> tier2, >=80 -> tier3
TIER_NAMES = ("tier1", "tier2", "tier3")
CAR_COUNT_DEFAULT = 8
CAR_MESHES = ("sedan.glb", "taxi.glb", "van.glb", "police.glb")
TEXTURE_DIR = REPO_ROOT / "assets" / "thirdparty" / "ambientcg-facades"
ASSETS_MODELS = REPO_ROOT / "assets" / "models"


def whitelist_ok(name: str) -> bool:
    return bool(GLB_NAME_RE.match(name))


def tier_of(height_m: float) -> int:
    """1/2/3 by building height; deterministic at boundaries (>= lower)."""
    if height_m < TIER_LIMITS[0]:
        return 1
    if height_m < TIER_LIMITS[1]:
        return 2
    return 3


def box_glb_name(tier: int) -> str:
    return "city_box_tier%d.glb" % tier


# --- byte-preserving scene rewrite -----------------------------------------

def split_heights(scene_text: str):
    """Return the ordered heights of every box entity (mesh == box.obj)."""
    doc = json.loads(scene_text)
    out = []
    for e in doc.get("entities", []):
        comp = e.get("components", {})
        model = comp.get("Model", {})
        if model.get("mesh", "") != "buildings/box.obj":
            continue
        tr = comp.get("Transform", {})
        scale = tr.get("scale", [1.0, 1.0, 1.0])
        out.append(float(scale[1]))
    return out


def rewrite_scene(scene_text: str, heights, car_nodes):
    """Rewrite ONLY mesh values + nextID + append car entities; all other
    bytes (including UTF-8 labels, lm_* entities, spacing) preserved.
    car_nodes are sorted by numeric id for deterministic placement.
    Returns (engine_text, box_refs, car_count, new_next_id)."""
    box_refs = [0]

    def sub_mesh(_m):
        tier = tier_of(heights[box_refs[0]])
        box_refs[0] += 1
        return '"mesh": "%s"' % box_glb_name(tier)

    text = BOX_MESH_RE.sub(sub_mesh, scene_text)
    if box_refs[0] != len(heights):
        raise ValueError("box mesh ref count %d != heights %d"
                         % (box_refs[0], len(heights)))
    m = NEXTID_RE.search(text)
    if not m:
        raise ValueError("city.scene missing nextID")
    old_next = int(m.group(1))
    # E1 nextID = next free entity id (scene.cpp emits n+1 for n entities).
    # Cars take [old_next, old_next+count); nextID advances by count.
    first_car_id = old_next
    new_next_id = old_next + len(car_nodes)
    text = text[:m.start(1)] + str(new_next_id) + text[m.end(1):]

    car_lines = []
    for idx, node in enumerate(
            sorted(car_nodes, key=lambda n: int(n.get("id", 0)))):
        eid = first_car_id + idx
        mesh = CAR_MESHES[idx % len(CAR_MESHES)]
        car_lines.append(
            '    { "id": %d, "name": "car_%02d", "flags": 0, "parent": 0,\n'
            '      "components": {\n'
            '      "Model": { "mesh": "%s", "tint": [1.0, 1.0, 1.0] },\n'
            '      "Transform": { "position": [%.2f, 0.00, %.2f], '
            '"scale": [1.00, 1.00, 1.00], '
            '"rotation": [0.0, 0.0, 0.0, 1.0] }\n'
            '    } }%s\n'
            % (eid, idx, mesh, float(node["x"]), float(node["z"]),
               "," if idx < len(car_nodes) - 1 else ""))
    pos = text.rfind(ENTITIES_END)
    if pos < 0:
        raise ValueError("city.scene missing entities array terminator")
    # Insert cars after the last entity (which has no trailing comma).
    last_comma = text.rfind("\n    } }")
    if last_comma < 0 or last_comma > pos:
        raise ValueError("city.scene last entity not found")
    insert_at = last_comma + len("\n    } }")
    text = text[:insert_at] + ",\n" + "".join(car_lines).rstrip("\n") + text[insert_at:]
    return text, box_refs[0], len(car_nodes), new_next_id


# --- textured unit box OBJ/MTL (UV per face; W1.1 ambientCG maps) -----------

# 24 vertices (4 per face) so each face carries full 0..1 UV. Y is up.
_BOX_VERTS = [
    (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5),
    (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (0.5, 0.5, -0.5),
    (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (0.5, 0.5, 0.5),
    (-0.5, -0.5, 0.5), (-0.5, -0.5, -0.5), (-0.5, 0.5, -0.5), (-0.5, 0.5, 0.5),
    (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5),
    (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, -0.5, -0.5), (-0.5, -0.5, -0.5),
]
_BOX_NORMALS = [
    (0, 0, -1), (1, 0, 0), (0, 0, 1), (-1, 0, 0), (0, 1, 0), (0, -1, 0),
]
_BOX_UV = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]


def write_tier_obj(obj_path: Path, tier: int):
    """Write a deterministic textured unit-box OBJ + sibling MTL."""
    mat = TIER_NAMES[tier - 1]
    mtl_path = obj_path.with_suffix(".mtl")
    lines = ["# citygen2engine tier box (M-CG2)", "mtllib %s" % mtl_path.name]
    for v in _BOX_VERTS:
        lines.append("v %.4f %.4f %.4f" % v)
    for n in _BOX_NORMALS:
        lines.append("vn %d %d %d" % n)
    for uv in _BOX_UV:
        lines.append("vt %.4f %.4f" % uv)
    lines.append("usemtl %s" % mat)
    for f in range(6):
        base = f * 4 + 1
        lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d %d/%d/%d"
                     % (base, f * 4 + 1, f + 1,
                        base + 1, f * 4 + 2, f + 1,
                        base + 2, f * 4 + 3, f + 1,
                        base + 3, f * 4 + 4, f + 1))
    obj_path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="")
    mtl_path.write_text(
        "# citygen2engine tier material\nnewmtl %s\nKd 1.0 1.0 1.0\nmap_Kd %s.jpg\n"
        % (mat, mat), encoding="ascii", newline="")


# --- evidence PNG (stdlib, real transforms) --------------------------------

def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path: Path, w: int, h: int, px) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            r, g, b = px(x, y)
            raw += bytes((r & 0xFF, g & 0xFF, b & 0xFF))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr)
                     + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
                     + _png_chunk(b"IEND", b""))


def render_delivery_map(engine_text: str, out_png: Path, cell: int = 6) -> None:
    """Real-transform-driven map: x/z from engine scene, height -> luminance,
    landmark mesh -> gold, cars -> white."""
    doc = json.loads(engine_text)
    pts = []
    for e in doc.get("entities", []):
        comp = e.get("components", {})
        model = comp.get("Model", {})
        tr = comp.get("Transform", {})
        if not model or not tr:
            continue
        x, _y, z = tr.get("position", [0.0, 0.0, 0.0])
        h = float(tr.get("scale", [1.0, 1.0, 1.0])[1])
        mesh = model.get("mesh", "")
        kind = "tower" if mesh == "canton_tower.glb" else (
            "car" if e.get("name", "").startswith("car_") else "bld")
        pts.append((float(x), float(z), h, kind))
    if not pts:
        raise ValueError("no entities with Transform in engine scene")
    minx, maxx = min(p[0] for p in pts), max(p[0] for p in pts)
    minz, maxz = min(p[1] for p in pts), max(p[1] for p in pts)
    spanx, spanz = max(1.0, maxx - minx), max(1.0, maxz - minz)
    W = min(1024, max(256, int(spanx / 3.0) + 16))
    H = min(1024, max(256, int(spanz / 3.0) + 16))
    # Pre-index entities into a coarse grid: O(N) build + O(1) lookup instead
    # of O(pixels*N) (~6.6e8 Python iterations at 2048^2 x 159 entities).
    grid_w = cell * 8
    buckets = {}
    for wxp, wzp, hp, kind in pts:
        bx = int((wxp - minx) / spanx * grid_w)
        bz = int((wzp - minz) / spanz * grid_w)
        key = (max(0, min(grid_w - 1, bx)), max(0, min(grid_w - 1, bz)))
        cur = buckets.get(key)
        if cur is None or hp > cur[1]:
            buckets[key] = (kind, hp)

    def px(x, y):
        bx = int(x / max(1, W - 1) * grid_w)
        bz = int(y / max(1, H - 1) * grid_w)
        best = None
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                c = buckets.get((bx + dx, bz + dz))
                if c is not None and (best is None or c[1] > best[1]):
                    best = c
        if best is None:
            return (28, 30, 36)
        if best[0] == "tower":
            return (255, 215, 0)
        if best[0] == "car":
            return (240, 240, 240)
        t = min(1.0, max(0.0, best[1] / 140.0))
        g = int(88 + 150 * t)
        return (g, g, g)
    write_png(out_png, W, H, px)


# --- drive ------------------------------------------------------------------

# --- derived roadnet adapter (Q-CG2C=双向化最大分量) ------------------------
# Real OSM roadnets fragment into many weakly/strongly connected pieces; the
# traffic system picks route start/goal from the WHOLE lane pool via fnv1a64,
# so lanes in isolated components make init fail ("goal lane not reachable").
# Source roadnet.json is frozen; the DERIVED one keeps only the largest weak
# component and adds the reverse of every edge (one-way semantics preserved
# in the source, explicitly adapted here for closed-loop route generation).

def adapt_roadnet(roadnet: dict):
    """Return (derived_roadnet, stats). Source dict is not mutated."""
    nodes = list(roadnet.get("nodes", []))
    edges = list(roadnet.get("edges", []))
    if not nodes or not edges:
        raise ValueError("empty roadnet")
    und = {}
    for e in edges:
        a, b = int(e["from"]), int(e["to"])
        und.setdefault(a, set()).add(b)
        und.setdefault(b, set()).add(a)
    node_ids = {int(n["id"]) for n in nodes}
    seen = set()
    components = []
    for start in sorted(node_ids):
        if start in seen:
            continue
        stack, comp = [start], []
        seen.add(start)
        while stack:
            u = stack.pop()
            comp.append(u)
            for v in und.get(u, ()):
                if v not in seen and v in node_ids:
                    seen.add(v)
                    stack.append(v)
        components.append(set(comp))
    largest = max(components, key=len)
    kept_nodes = [n for n in nodes if int(n["id"]) in largest]
    kept_edges = [e for e in edges if int(e["from"]) in largest
                  and int(e["to"]) in largest]
    if not kept_edges:
        raise ValueError("largest roadnet component has no edges")
    derived_edges = []
    seen_pairs = set()
    for e in kept_edges:
        a, b = int(e["from"]), int(e["to"])
        for src, dst in ((a, b), (b, a)):
            if (src, dst) in seen_pairs:
                continue
            seen_pairs.add((src, dst))
            ne = dict(e)
            ne["from"], ne["to"] = src, dst
            derived_edges.append(ne)
    derived_edges.sort(key=lambda e: (int(e["from"]), int(e["to"])))
    for idx, e in enumerate(derived_edges):
        e["id"] = idx
    out = dict(roadnet)
    out["nodes"] = kept_nodes
    out["edges"] = derived_edges
    stats = {
        "source_nodes": len(nodes), "source_edges": len(edges),
        "components": len(components),
        "kept_nodes": len(kept_nodes), "kept_edges": len(kept_edges),
        "derived_edges": len(derived_edges),
    }
    return out, stats


def pick_car_nodes(roadnet: dict, count: int):
    nodes = sorted(roadnet.get("nodes", []), key=lambda n: int(n.get("id", 0)))
    return nodes[:count]


POSITION_RE = re.compile(r'"position":\s*\[\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)\s*\]')


def compute_center(roadnet: dict):
    """Median x/z of retained roadnet nodes = stable view center (viewer camera
    is fixed on world origin; km-scale OSM local metres would fall off-screen).
    """
    xs = sorted(float(n["x"]) for n in roadnet.get("nodes", []))
    zs = sorted(float(n["z"]) for n in roadnet.get("nodes", []))
    if not xs:
        return 0.0, 0.0
    mid = len(xs) // 2
    cx = xs[mid] if len(xs) % 2 else (xs[mid - 1] + xs[mid]) * 0.5
    mz = len(zs) // 2
    cz = zs[mz] if len(zs) % 2 else (zs[mz - 1] + zs[mz]) * 0.5
    return cx, cz


def translate_scene_text(scene_text: str, cx: float, cz: float):
    """Shift every entity position x/z so view centre lands on world origin."""
    def sub(m):
        return '"position": [%.2f, %s, %.2f]' % (
            float(m.group(1)) - cx, m.group(2), float(m.group(3)) - cz)
    return POSITION_RE.sub(sub, scene_text)


def translate_roadnet(roadnet: dict, cx: float, cz: float) -> dict:
    out = dict(roadnet)
    nodes = []
    for n in roadnet.get("nodes", []):
        nn = dict(n)
        nn["x"] = round(float(n["x"]) - cx, 2)
        nn["z"] = round(float(n["z"]) - cz, 2)
        nodes.append(nn)
    out["nodes"] = nodes
    return out


def validate_src(src: Path):
    if not src.is_dir():
        return "--src not a directory: %s" % src
    for name in ("city.scene", "roadnet.json", "buildings/box.obj",
                 "landmark_whitelist.json", "ODbL-attribution.txt", "top.png"):
        if not (src / name).is_file():
            return "--src missing %s" % name
    if not TEXTURE_DIR.is_dir():
        return "ambientCG texture dir missing: %s" % TEXTURE_DIR
    return None


def cmd_drive(args) -> int:
    src, out = Path(args.src), Path(args.out)
    err = validate_src(src)
    if err:
        sys.stderr.write("citygen2engine: " + err + "\n")
        return 2
    models_out = Path(args.models_out) if args.models_out else ASSETS_MODELS
    out.mkdir(parents=True, exist_ok=True)
    models_out.mkdir(parents=True, exist_ok=True)

    with open(src / "city.scene", "r", encoding="utf-8", newline="") as f:
        scene_text = f.read()
    roadnet = json.loads((src / "roadnet.json").read_text(encoding="utf-8"))
    roadnet, net_stats = adapt_roadnet(roadnet)
    cx, cz = compute_center(roadnet)
    roadnet = translate_roadnet(roadnet, cx, cz)
    scene_text = translate_scene_text(scene_text, cx, cz)
    heights = split_heights(scene_text)
    if not heights:
        sys.stderr.write("citygen2engine: no box entities in city.scene\n")
        return 1
    car_nodes = pick_car_nodes(roadnet, args.cars)
    if len(car_nodes) < 1:
        sys.stderr.write("citygen2engine: roadnet has no nodes for car injection\n")
        return 1
    engine_text, ref_count, car_count, new_next_id = rewrite_scene(
        scene_text, heights, car_nodes)

    # verify engine text is valid JSON with expected entity shape
    eng = json.loads(engine_text)
    ents = eng.get("entities", [])
    cars = [e for e in ents if e.get("name", "").startswith("car_")]
    if len(cars) != car_count:
        sys.stderr.write("citygen2engine: car entity count mismatch\n")
        return 1
    mesh_names = []
    for e in ents:
        mesh = e.get("components", {}).get("Model", {}).get("mesh", "")
        if mesh and not whitelist_ok(mesh):
            sys.stderr.write("citygen2engine: mesh fails whitelist: %s\n" % mesh)
            return 1
        mesh_names.append(mesh)
    if "canton_tower.glb" not in mesh_names:
        sys.stderr.write("citygen2engine: landmark mesh missing\n")
        return 1

    # tier glbs (shared per tier, textured with ambientCG)
    tiers = sorted(set(tier_of(h) for h in heights))
    with tempfile.TemporaryDirectory(prefix="cg2eng_") as td:
        tdpath = Path(td)
        for tier in tiers:
            obj = tdpath / ("city_box_tier%d.obj" % tier)
            write_tier_obj(obj, tier)
            glb = models_out / box_glb_name(tier)
            rc = obj2glb.main([str(obj), str(glb), "--maps", str(TEXTURE_DIR)])
            if rc != 0:
                sys.stderr.write("citygen2engine: obj2glb failed for tier %d\n" % tier)
                return 1

    # copy vendored/landmark glbs into --out for self-contained delivery
    needed = {"canton_tower.glb"} | set(CAR_MESHES)
    copied = []
    for name in sorted(needed):
        src_glb = ASSETS_MODELS / name
        if not src_glb.is_file():
            sys.stderr.write("citygen2engine: missing asset glb %s\n" % name)
            return 1
        dst = out / name
        if not dst.is_file() or dst.read_bytes() != src_glb.read_bytes():
            shutil.copyfile(src_glb, dst)
        copied.append(name)

    with open(out / "city.scene.engine", "w", encoding="utf-8", newline="") as f:
        f.write(engine_text)
    (out / "roadnet.json").write_text(
        json.dumps(roadnet, ensure_ascii=False, indent=1) + "\n",
        encoding="utf-8", newline="\n")
    (out / "ROADNET-ADAPT.txt").write_text(
        "citygen2engine M-CG2 derived roadnet adapter (Q-CG2C=bidirectional largest component)\n"
        "source: example/cg1-m0/roadnet.json (frozen, ODbL, unchanged)\n"
        "source nodes=%d edges=%d weak_components=%d\n"
        "kept largest weak component nodes=%d edges=%d\n"
        "derived bidirectional edges=%d (reverse of each kept edge; oneway semantics\n"
        "  preserved in source roadnet, adapted here only for closed-loop route init)\n"
        % (net_stats["source_nodes"], net_stats["source_edges"],
           net_stats["components"], net_stats["kept_nodes"],
           net_stats["kept_edges"], net_stats["derived_edges"]),
        encoding="utf-8", newline="\n")
    for name in ("ODbL-attribution.txt", "landmark_whitelist.json"):
        shutil.copyfile(src / name, out / name)
    shutil.copyfile(src / "top.png", out / "citygen_top.png")
    render_delivery_map(engine_text, out / "city_delivery.png")

    tier_counts = {t: sum(1 for h in heights if tier_of(h) == t) for t in tiers}
    print("citygen2engine drive OK: src=%s out=%s boxes=%d tiers=%s cars=%d "
          "nextID=%d models=%d copied=%d roads=%d->%d/%d comps=%d"
          % (src, out, ref_count,
             ",".join("%d:%d" % (t, tier_counts[t]) for t in tiers),
             car_count, new_next_id, len(tiers), len(copied),
             net_stats["source_edges"], net_stats["kept_edges"],
             net_stats["derived_edges"], net_stats["components"]))
    return 0


# --- selftest ---------------------------------------------------------------

def selftest(verbose: bool) -> int:
    fails = []
    n = [0]

    def assert_(name, cond):
        n[0] += 1
        if verbose:
            print("  ok   %s" % name if cond else "  FAIL %s" % name)
        if not cond:
            fails.append(name)

    # whitelist
    assert_("whitelist accept tier", whitelist_ok("city_box_tier1.glb"))
    assert_("whitelist accept tower", whitelist_ok("canton_tower.glb"))
    assert_("whitelist reject obj", not whitelist_ok("buildings/box.obj"))
    assert_("whitelist reject subdir", not whitelist_ok("assets/car.glb"))
    assert_("whitelist reject traversal", not whitelist_ok("../x.glb"))
    assert_("whitelist reject nonascii", not whitelist_ok("\u5e7f.glb"))

    # tier boundaries
    assert_("tier 9.0 -> 1", tier_of(9.0) == 1)
    assert_("tier 29.99 -> 1", tier_of(29.99) == 1)
    assert_("tier 30.0 -> 2", tier_of(30.0) == 2)
    assert_("tier 79.99 -> 2", tier_of(79.99) == 2)
    assert_("tier 80.0 -> 3", tier_of(80.0) == 3)
    assert_("tier 440.2 -> 3", tier_of(440.2) == 3)
    assert_("glb name distinct", box_glb_name(1) != box_glb_name(2))

    # rewrite: preserve bytes except mesh/nextID/cars
    mini = ('{\n  "version": 1,\n  "nextID": 4,\n  "entities": [\n'
            '    { "id": 1, "name": "bld_1", "flags": 0, "parent": 0,\n'
            '      "label": "\u897f\u5854",\n'
            '      "components": {\n'
            '      "Model": { "mesh": "buildings/box.obj", "tint": [1.0, 1.0, 1.0] },\n'
            '      "Transform": { "position": [1.00, 5.00, 2.00], '
            '"scale": [2.00, 10.00, 3.00], "rotation": [0.0, 0.0, 0.0, 1.0] }\n'
            '    } },\n'
            '    { "id": 2, "name": "bld_2", "flags": 0, "parent": 0,\n'
            '      "components": {\n'
            '      "Model": { "mesh": "buildings/box.obj", "tint": [1.0, 1.0, 1.0] },\n'
            '      "Transform": { "position": [5.00, 40.00, 2.00], '
            '"scale": [2.00, 80.00, 3.00], "rotation": [0.0, 0.0, 0.0, 1.0] }\n'
            '    } },\n'
            '    { "id": 3, "name": "lm_0", "flags": 0, "parent": 0,\n'
            '      "label": "\u5e7f\u5dde\u5854",\n'
            '      "components": {\n'
            '      "Model": { "mesh": "canton_tower.glb", "tint": [1.0, 1.0, 1.0] },\n'
            '      "Transform": { "position": [-1.00, 0.00, -2.00], '
            '"scale": [1.00, 1.00, 1.00], "rotation": [0.0, 0.0, 0.0, 1.0] }\n'
            '    } }\n  ]\n}\n')
    heights = split_heights(mini)
    assert_("heights parsed", heights == [10.0, 80.0])
    nodes = [{"id": 7, "x": 12.5, "z": -3.0}, {"id": 2, "x": 4.0, "z": 9.0}]
    engine, refs, cars, new_next = rewrite_scene(mini, heights, nodes)
    assert_("box refs", refs == 2 and cars == 2)
    assert_("tier rewrite", '"mesh": "city_box_tier1.glb"' in engine
            and '"mesh": "city_box_tier3.glb"' in engine)   # 80.0 >= tier3 下限
    assert_("tower preserved", '"mesh": "canton_tower.glb"' in engine)
    assert_("label preserved UTF-8", "\u897f\u5854" in engine and "\u5e7f\u5dde\u5854" in engine)
    assert_("nextID bumped", '"nextID": 6' in engine and new_next == 6)
    edoc = json.loads(engine)
    assert_("engine valid json", edoc["version"] == 1 and len(edoc["entities"]) == 5)
    assert_("car names", [e["name"] for e in edoc["entities"]][-2:] == ["car_00", "car_01"])
    car0 = edoc["entities"][-2]
    assert_("car from smallest node id",
            car0["components"]["Transform"]["position"][0] == 4.0
            and car0["components"]["Transform"]["position"][2] == 9.0)
    assert_("car mesh flat", car0["components"]["Model"]["mesh"] == "sedan.glb")
    assert_("car id range", [e["id"] for e in edoc["entities"]][-2:] == [4, 5]
            and new_next == 6)
    # byte preservation: strip mesh values / nextID / the two appended car
    # entity blocks, then the remainder must equal the source verbatim.
    def strip_cars(s):
        # cars are the last two entities, each starting with '    { "id": 4/5'
        idx = s.find('    { "id": 4, "name": "car_00"')
        s = s[:idx].rstrip("\n")
        if s.endswith(","):
            s = s[:-1]
        return s + "\n  ]\n}\n"
    strip = lambda s: re.sub(r'"nextID":\s*\d+', '"nextID": X',
                             re.sub(r'"mesh": "[^"]*"', '"mesh": X', s))
    assert_("rewrite preserves other bytes",
            strip(strip_cars(engine)) == strip(mini))

    # car node ordering deterministic
    assert_("car node order", [n["id"] for n in pick_car_nodes(
        {"nodes": [{"id": 9}, {"id": 1}, {"id": 4}]}, 2)] == [1, 4])

    # roadnet adapter: fragmented graph -> largest component, bidirectional
    frag = {
        "version": 1, "nodes": [{"id": i} for i in range(6)],
        "edges": [
            {"id": 0, "from": 0, "to": 1}, {"id": 1, "from": 1, "to": 2},
            {"id": 2, "from": 2, "to": 0},
            {"id": 3, "from": 3, "to": 4}, {"id": 4, "from": 4, "to": 5},
        ],
    }
    adapted, ast = adapt_roadnet(frag)
    assert_("adapter keeps largest component",
            {n["id"] for n in adapted["nodes"]} == {0, 1, 2})
    assert_("adapter bidirectional", len(adapted["edges"]) == 6)
    assert_("adapter both directions",
            {(e["from"], e["to"]) for e in adapted["edges"]} ==
            {(0, 1), (1, 0), (1, 2), (2, 1), (2, 0), (0, 2)})
    assert_("adapter edge ids reindexed",
            [e["id"] for e in adapted["edges"]] == list(range(6)))
    assert_("adapter stats", ast["components"] == 2 and ast["source_edges"] == 5)
    adapted2, _ = adapt_roadnet(frag)
    assert_("adapter deterministic", adapted == adapted2)
    assert_("adapter source unmutated", len(frag["edges"]) == 5
            and frag["edges"][0]["from"] == 0)

    # view-centre translation (viewer camera fixed on world origin)
    tr = {"nodes": [{"id": 0, "x": 100.0, "z": -50.0},
                    {"id": 1, "x": 102.0, "z": -48.0},
                    {"id": 2, "x": 98.0, "z": -52.0}]}
    tcx, tcz = compute_center(tr)
    assert_("view centre median", tcx == 100.0 and tcz == -50.0)
    tr2 = translate_roadnet(tr, tcx, tcz)
    assert_("roadnet translated to origin",
            [(n["x"], n["z"]) for n in tr2["nodes"]] ==
            [(0.0, 0.0), (2.0, 2.0), (-2.0, -2.0)])
    assert_("translate source unmutated", tr["nodes"][0]["x"] == 100.0)
    stext = '{"entities": [{"components": {"Transform": {"position": [100.00, 5.00, -50.00]}}}]}'
    assert_("scene translated to origin",
            '"position": [0.00, 5.00, 0.00]' in translate_scene_text(stext, tcx, tcz))

    # OBJ/MTL + obj2glb determinism
    with tempfile.TemporaryDirectory(prefix="cg2eng_selftest_") as td:
        tdp = Path(td)
        obj = tdp / "city_box_tier1.obj"
        write_tier_obj(obj, 1)
        obj_txt = obj.read_text(encoding="ascii")
        assert_("obj 24 verts", obj_txt.count("\nv ") + obj_txt.startswith("v ") == 24)
        assert_("obj 6 normals", obj_txt.count("\nvn ") == 6)
        assert_("obj 4 uvs", obj_txt.count("\nvt ") == 4)
        assert_("obj 6 quads", obj_txt.count("\nf ") == 6)
        assert_("mtl map_Kd", "map_Kd tier1.jpg" in (tdp / "city_box_tier1.mtl").read_text())
        g1, g2 = tdp / "a.glb", tdp / "b.glb"
        assert_("obj2glb rc tier1",
                obj2glb.main([str(obj), str(g1), "--maps", str(TEXTURE_DIR)]) == 0)
        assert_("obj2glb rc tier1 rerun",
                obj2glb.main([str(obj), str(g2), "--maps", str(TEXTURE_DIR)]) == 0)
        assert_("tier glb deterministic", g1.read_bytes() == g2.read_bytes())
        js = g1.read_bytes()
        assert_("glb magic", js[:4] == b"glTF")

    # PNG writer
    with tempfile.TemporaryDirectory(prefix="cg2eng_png_") as td:
        p1, p2 = Path(td) / "a.png", Path(td) / "b.png"
        def px(x, y): return (x % 256, y % 256, 128)
        write_png(p1, 32, 24, px)
        write_png(p2, 32, 24, px)
        assert_("png signature", p1.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n")
        assert_("png deterministic", p1.read_bytes() == p2.read_bytes())
        w = struct.unpack(">I", p1.read_bytes()[16:20])[0]
        assert_("png ihdr width", w == 32)

    # real fixture end-to-end (single pass; dual-run byte determinism is
    # gated by example/cg2-m0/run.bat seg2 and regression --only cg2-m0)
    fixture = REPO_ROOT / "example" / "cg1-m0"
    if fixture.is_dir():
        with tempfile.TemporaryDirectory(prefix="cg2eng_e2e_") as td:
            tdp = Path(td)
            a_args = argparse.Namespace(src=str(fixture), out=str(tdp / "a"),
                                        models_out=str(tdp / "models_a"),
                                        cars=CAR_COUNT_DEFAULT, stats=False)
            ra = cmd_drive(a_args)
            assert_("e2e drive rc", ra == 0)
            for name in ("city.scene.engine", "roadnet.json", "city_delivery.png",
                         "ODbL-attribution.txt", "landmark_whitelist.json", "citygen_top.png"):
                assert_("e2e wrote " + name, (tdp / "a" / name).is_file())
            e = json.loads((tdp / "a" / "city.scene.engine").read_text(encoding="utf-8"))
            cars_e = [x for x in e["entities"] if x.get("name", "").startswith("car_")]
            assert_("e2e car count", len(cars_e) == CAR_COUNT_DEFAULT)
            assert_("e2e 150 boxes + tower + cars",
                    len(e["entities"]) == 150 + 1 + CAR_COUNT_DEFAULT)
            assert_("e2e roadnet adapted", len(json.loads(
                (tdp / "a" / "roadnet.json").read_text(encoding="utf-8"))["edges"]) > 417)

    if fails:
        print("citygen2engine selftest: FAIL (%d/%d)" % (len(fails), n[0]))
        for f in fails:
            print("  failed: " + f)
        return 1
    print("citygen2engine selftest: PASS (%d assertions)" % n[0])
    return 0


def main(argv=None) -> int:
    if argv is None:
        argv = sys.argv[1:]
    if not argv:
        print(USAGE, end="")
        return 2
    head = argv[0]
    if head in ("-h", "--help"):
        print(USAGE, end="")
        return 0
    if head == "selftest":
        ap = argparse.ArgumentParser(add_help=False)
        ap.add_argument("--verbose", action="store_true")
        a = ap.parse_args(argv[1:])
        return selftest(a.verbose)
    if head == "drive":
        ap = argparse.ArgumentParser(add_help=False)
        ap.add_argument("--src", required=True)
        ap.add_argument("--out", required=True)
        ap.add_argument("--models-out", default=None)
        ap.add_argument("--cars", type=int, default=CAR_COUNT_DEFAULT)
        ap.add_argument("--stats", action="store_true")
        a = ap.parse_args(argv[1:])
        if a.cars < 1:
            sys.stderr.write("citygen2engine: --cars must be >= 1\n")
            return 2
        return cmd_drive(a)
    sys.stderr.write(USAGE)
    return 2


if __name__ == "__main__":
    sys.exit(main())
