#!/usr/bin/env python3
"""city2engine.py - M-W1.0 engine delivery driver (worldgen track).

Turns frozen city-tools products (blocks/*.obj + city.scene) into engine-ready
assets, per W1-PLAN.md (docs/worldgen/W1-PLAN.md):

  1. per-block OBJ  -> flat .glb            via scripts/obj2glb.py (in-process)
     assets/models/city_block_<seed>_<size>_NN.glb
  2. per-block glb  -> nanite-tools suite   via nanite-tools cluster -> lod -> dag
     assets/meshlets/city_block_<seed>_<size>_NN{.nmesh,.lodN.nmesh,.dag.json}
  3. city.scene     -> city.scene.engine    meshFile rewritten to flat .glb names,
     every other byte preserved (Q-W1A dual-track: source scene untouched)

Determinism contract: same seed + same inputs -> byte-identical outputs.
- obj2glb builder name is static, no timestamps.
- nanite-tools invocations always run with cwd = repo root and repo-RELATIVE
  paths, so .dag.json's embedded "prefix" field is stable across machines/cwd.
- city.scene.engine is a pure regex rewrite of the frozen scene text.

usage:
  city2engine.py
  city2engine.py selftest [--verbose]
  city2engine.py drive --src DIR --seed N --size N [--nanite EXE]
                       [--models-out DIR] [--meshlets-out DIR]
                       [--scene-out FILE] [--stats]

exit codes: 0 ok / 1 runtime failure / 2 usage or invalid arguments
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = Path(__file__).resolve().parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import obj2glb  # noqa: E402  (vendored-style sibling script, asset-ingest chain)

USAGE = """city2engine - nl-engine worldgen tools (M-W1.0 engine delivery driver)
usage:
  city2engine.py
  city2engine.py selftest [--verbose]
  city2engine.py drive --src DIR --seed N --size N [--nanite EXE]
                       [--models-out DIR] [--meshlets-out DIR]
                       [--scene-out FILE] [--stats]
examples:
  city2engine.py drive --src example/w0-m3 --seed 12345 --size 6
  city2engine.py selftest --verbose
exit codes: 0 ok / 1 runtime failure / 2 usage or invalid arguments
"""

# flat whitelist re-implementation of viewer/renderer scene gate (Q-W1B: the
# gate accepts bare ASCII .glb file names only; no separators, no traversal)
GLB_NAME_RE = re.compile(r"^[A-Za-z0-9_]+\.glb$")
MESH_REF_RE = re.compile(r"blocks/block_(\d+)\.obj$")
MESH_JSON_RE = re.compile(r'"mesh": "blocks/block_(\d+)\.obj"')
ENTITY_NAME_RE = re.compile(r"^block_(\d+)$")


def glb_name(seed: int, size: int, block_id: int) -> str:
    """Flat, whitelist-compliant asset name carrying seed/size dimensions."""
    return "city_block_%d_%d_%02d.glb" % (seed, size, block_id)


def whitelist_ok(name: str) -> bool:
    return bool(GLB_NAME_RE.match(name))


def parse_mesh_ref(mesh: str):
    m = MESH_REF_RE.match(mesh)
    return int(m.group(1)) if m else None


def rewrite_scene(scene_text: str, seed: int, size: int):
    """Rewrite meshFile values only; every other byte of the frozen scene is
    preserved verbatim. Returns (new_text, ref_count)."""
    count = [0]

    def sub(m):
        count[0] += 1
        return '"mesh": "%s"' % glb_name(seed, size, int(m.group(1)))

    return MESH_JSON_RE.sub(sub, scene_text), count[0]


def rel(repo_rel: Path) -> str:
    """Repo-relative POSIX path for a path already inside the repo (or given
    repo-relative); used to keep nanite .dag.json prefix byte-stable."""
    p = repo_rel if repo_rel.is_absolute() else (REPO_ROOT / repo_rel)
    return os.path.relpath(str(p), str(REPO_ROOT)).replace("\\", "/")


def rel_to_repo(p: Path) -> str:
    return os.path.relpath(str(p if p.is_absolute() else (REPO_ROOT / p)),
                           str(REPO_ROOT)).replace("\\", "/")


def run_checked(argv, expect_log=None, what="") -> str:
    proc = subprocess.run(argv, cwd=str(REPO_ROOT), capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        sys.stderr.write("city2engine: %s failed (exit %d)\n%s\n"
                         % (what, proc.returncode, out[-2000:]))
        raise SystemExit(1)
    if expect_log and expect_log not in out:
        sys.stderr.write("city2engine: %s missing anchor %r\n%s\n"
                         % (what, expect_log, out[-2000:]))
        raise SystemExit(1)
    return out


def nanite_suite(nanite: Path, glb_rel: str, prefix_rel: str, max_levels: int):
    """cluster -> lod -> dag, all with cwd=REPO_ROOT and relative paths.
    Returns (cluster_count, dag_json_dict)."""
    run_checked([str(nanite), "cluster", glb_rel, "-o", prefix_rel + ".nmesh"],
                expect_log="PASS", what="nanite cluster %s" % glb_rel)
    run_checked([str(nanite), "lod", prefix_rel + ".nmesh", "-o", prefix_rel,
                 "--max-levels", str(max_levels)],
                what="nanite lod %s" % prefix_rel)
    dag_out = run_checked(
        [str(nanite), "dag", prefix_rel, "--export", prefix_rel + ".dag.json"],
        expect_log="dag         : OK", what="nanite dag %s" % prefix_rel)
    dag = json.loads(Path(REPO_ROOT / (prefix_rel + ".dag.json")).read_text(
        encoding="utf-8"))
    clusters = sum(lv["clusters"] for lv in dag["levels"])
    return clusters, dag


# ---------------------------------------------------------------------------
# evidence map: 6x6-style block grid, cell luminance = glb byte size (real
# output driven, stdlib PNG; no text glyphs needed)
# ---------------------------------------------------------------------------

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


def render_delivery_map(scene: dict, sizes: dict, out_png: Path,
                        cell: int = 96, header: int = 72) -> None:
    """Block-grid delivery map from real scene Transform positions + real glb
    byte sizes (min..max normalized luminance)."""
    ents = []
    for e in scene["entities"]:
        tr = e["components"]["Transform"]
        # engine 场景的 mesh 已换成 glb 名；实体名 block_NN 才是稳定主键
        m = ENTITY_NAME_RE.match(e.get("name", ""))
        if m is None:
            continue
        ents.append((int(m.group(1)), tr["position"][0], tr["position"][2]))
    if not ents:
        raise SystemExit("city2engine: no block entities in scene")
    xs = sorted(set(x for _, x, _ in ents))
    zs = sorted(set(z for _, _, z in ents))
    pitch_x = xs[1] - xs[0] if len(xs) > 1 else 1.0
    pitch_z = zs[1] - zs[0] if len(zs) > 1 else 1.0
    cols = len(xs)
    rows = len(zs)
    lo, hi = min(sizes.values()), max(sizes.values())
    span = max(1, hi - lo)

    def cell_of(ref):
        _, x, z = next(e for e in ents if e[0] == ref)
        c = int(round((x - xs[0]) / pitch_x)) if pitch_x else 0
        r = int(round((z - zs[0]) / pitch_z)) if pitch_z else 0
        return c, r

    grid = {}
    for ref in sizes:
        c, r = cell_of(ref)
        grid[(c, r)] = sizes[ref]
    W, H = header + cols * cell + 16, header + rows * cell + 16
    border = (60, 60, 60)
    header_bg = (24, 26, 30)

    def px(x, y):
        if y < header:
            return header_bg
        if x < 8 or x >= 8 + cols * cell or y >= header + rows * cell:
            return header_bg
        c = (x - 8) // cell
        r = (y - header) // cell
        u = (x - 8) % cell
        v = (y - header) % cell
        if u < 2 or v < 2:
            return border
        size = grid.get((c, r))
        if size is None:
            return (20, 20, 20)
        t = (size - lo) / span
        g = int(90 + 150 * t)
        return (g, g, min(255, g + 12))

    write_png(out_png, W, H, px)


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------

def selftest(verbose: bool) -> int:
    fails = []
    n = [0]

    def assert_(name, cond):
        n[0] += 1
        if verbose:
            print("  ok   %s" % name if cond else "  FAIL %s" % name)
        if not cond:
            fails.append(name)

    # -- naming / whitelist (Q-W1B gate mirror)
    assert_("glb_name format+pad", glb_name(12345, 6, 0) == "city_block_12345_6_00.glb")
    assert_("glb_name id pad", glb_name(12345, 6, 7) == "city_block_12345_6_07.glb")
    assert_("glb_name distinct seeds", glb_name(1, 6, 0) != glb_name(12, 6, 0))
    assert_("whitelist accept", whitelist_ok(glb_name(12345, 6, 35)))
    assert_("whitelist reject traversal", not whitelist_ok("../x.glb"))
    assert_("whitelist reject subdir", not whitelist_ok("sub/x.glb"))
    assert_("whitelist reject ext", not whitelist_ok("x.obj") and not whitelist_ok("x.gltf"))
    assert_("whitelist reject non-ascii", not whitelist_ok("\u57ce\u5e02.glb"))

    # -- mesh ref parsing
    assert_("mesh ref parse", parse_mesh_ref("blocks/block_07.obj") == 7)
    assert_("mesh ref reject foreign", parse_mesh_ref("models/x.glb") is None)

    # -- scene rewrite: mesh values only, other bytes preserved
    sample = ('{\n  "entities": [\n    { "id": 1, "components": { "Model": '
              '{ "mesh": "blocks/block_00.obj", "tint": [0.8, 0.8, 0.8] } },\n'
              '    { "id": 2, "components": { "Model": '
              '{ "mesh": "blocks/block_11.obj", "tint": [0.8, 0.8, 0.8] } } ] }\n')
    new, cnt = rewrite_scene(sample, 12345, 6)
    assert_("rewrite count", cnt == 2)
    assert_("rewrite values",
            '"mesh": "city_block_12345_6_00.glb"' in new
            and '"mesh": "city_block_12345_6_11.glb"' in new)
    strip = lambda s: re.sub(r'"mesh": "[^"]*"', "", s)
    assert_("rewrite preserves other bytes", strip(new) == strip(sample))
    _, cnt0 = rewrite_scene('{"a": 1}', 12345, 6)
    assert_("rewrite no-ref count 0", cnt0 == 0)

    # -- obj2glb determinism on a minimal box (no MTL, no usemtl)
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        obj = tdp / "tiny.obj"
        obj.write_text(
            "o tiny\nv -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\n"
            "v -1 2 -1\nv 1 2 -1\nv 1 2 1\nv -1 2 1\n"
            "f 1 3 4\nf 1 2 3\nf 5 6 7\nf 5 7 8\nf 1 5 8\nf 1 8 4\n"
            "f 2 3 7\nf 2 7 6\nf 1 2 6\nf 1 6 5\nf 4 8 7\nf 4 7 3\n",
            encoding="ascii")
        g1, g2 = tdp / "a.glb", tdp / "b.glb"
        rc1 = obj2glb.main([str(obj), str(g1)])
        rc2 = obj2glb.main([str(obj), str(g2)])
        assert_("obj2glb rc 0", rc1 == 0 and rc2 == 0)
        assert_("obj2glb byte-determinism", g1.read_bytes() == g2.read_bytes())
        obj2 = tdp / "tiny2.obj"
        obj2.write_text(obj.read_text(encoding="ascii").replace("-1 2 -1", "-1 3 -1"),
                        encoding="ascii")
        g3 = tdp / "c.glb"
        obj2glb.main([str(obj2), str(g3)])
        assert_("obj2glb differing geom differs", g1.read_bytes() != g3.read_bytes())

        # -- png writer + delivery map on synthetic scene
        scene = {"entities": [
            {"id": 1, "name": "block_00",
             "components": {"Model": {"mesh": "blocks/block_00.obj"},
                            "Transform": {"position": [0.0, 0.0, 0.0]}}},
            {"id": 2, "name": "block_01",
             "components": {"Model": {"mesh": "blocks/block_01.obj"},
                            "Transform": {"position": [5.5, 0.0, 0.0]}}},
            {"id": 3, "name": "block_02",
             "components": {"Model": {"mesh": "blocks/block_02.obj"},
                            "Transform": {"position": [11.0, 0.0, 0.0]}}},
        ]}
        p1, p2 = tdp / "m1.png", tdp / "m2.png"
        render_delivery_map(scene, {0: 100, 1: 300, 2: 200}, p1)
        render_delivery_map(scene, {0: 100, 1: 300, 2: 200}, p2)
        head = p1.read_bytes()[:8]
        assert_("png signature", head == b"\x89PNG\r\n\x1a\n")
        assert_("png deterministic", p1.read_bytes() == p2.read_bytes())
        w = struct.unpack(">I", p1.read_bytes()[16:20])[0]
        h = struct.unpack(">I", p1.read_bytes()[20:24])[0]
        assert_("png ihdr dims", w > 0 and h > 0)
        p3 = tdp / "m3.png"
        render_delivery_map(scene, {0: 100, 1: 300, 2: 250}, p3)
        assert_("png size-sensitive", p1.read_bytes() != p3.read_bytes())

    if fails:
        print("city2engine selftest: FAIL (%d/%d)" % (len(fails), n[0]))
        for f in fails:
            print("  failed: " + f)
        return 1
    print("city2engine selftest: PASS (%d assertions)" % n[0])
    return 0


# ---------------------------------------------------------------------------
# drive
# ---------------------------------------------------------------------------

def validate_args(seed: int, size: int, src: Path, nanite: Path):
    if seed < 0:
        return "invalid arguments: --seed must be >= 0"
    if not (1 <= size <= 64):
        return "invalid arguments: --size out of range (1..64)"
    if not src.is_dir():
        return "invalid arguments: --src not a directory: %s" % src
    if not (src / "city.scene").is_file():
        return "invalid arguments: --src missing city.scene"
    if not (src / "blocks").is_dir():
        return "invalid arguments: --src missing blocks/ directory"
    if not nanite.is_file():
        return "invalid arguments: --nanite exe not found: %s" % nanite
    return None


def cmd_drive(args) -> int:
    src = Path(args.src)
    nanite = Path(args.nanite)
    err = validate_args(args.seed, args.size, src, nanite)
    if err:
        sys.stderr.write(err + "\n")
        return 2

    # newline="" both ways: preserve the frozen scene's bytes verbatim except
    # the mesh values (read_text would translate CRLF->LF and break the
    # "every other byte preserved" contract)
    with open(src / "city.scene", "r", encoding="utf-8", newline="") as f:
        scene_text = f.read()
    refs = MESH_JSON_RE.findall(scene_text)
    if not refs:
        sys.stderr.write("city2engine: no blocks/block_NN.obj mesh refs in city.scene\n")
        return 1
    block_ids = [int(r) for r in refs]
    if len(set(block_ids)) != len(block_ids):
        sys.stderr.write("city2engine: duplicate mesh refs in city.scene\n")
        return 1

    models_out = Path(args.models_out) if args.models_out else REPO_ROOT / "assets" / "models"
    meshlets_out = Path(args.meshlets_out) if args.meshlets_out else REPO_ROOT / "assets" / "meshlets"
    scene_out = Path(args.scene_out) if args.scene_out else REPO_ROOT / "example" / "w1-m0" / "city.scene.engine"
    for d in (models_out, meshlets_out, scene_out.parent):
        d.mkdir(parents=True, exist_ok=True)

    engine_text, ref_count = rewrite_scene(scene_text, args.seed, args.size)
    if ref_count != len(refs):
        sys.stderr.write("city2engine: rewrite count mismatch (%d vs %d)\n"
                         % (ref_count, len(refs)))
        return 1

    sizes = {}
    for bid in sorted(block_ids):
        obj = src / "blocks" / ("block_%02d.obj" % bid)
        if not obj.is_file():
            sys.stderr.write("city2engine: missing OBJ for block %d: %s\n" % (bid, obj))
            return 1
        name = glb_name(args.seed, args.size, bid)
        if not whitelist_ok(name):
            sys.stderr.write("city2engine: generated name fails whitelist: %s\n" % name)
            return 1
        glb = models_out / name
        stem = glb.stem
        rc = obj2glb.main([str(obj), str(glb)])
        if rc != 0:
            sys.stderr.write("city2engine: obj2glb failed for %s (rc=%d)\n" % (obj, rc))
            return 1
        clusters, _ = nanite_suite(nanite, rel_to_repo(glb), rel_to_repo(meshlets_out / stem),
                                   args.max_levels)
        sizes[bid] = glb.stat().st_size
        print("  block %02d -> %s (%d B, %d clusters, dag OK)"
              % (bid, name, sizes[bid], clusters))

    scene_out.write_text(engine_text, encoding="utf-8", newline="")

    if not args.no_evidence:
        scene = json.loads(engine_text)
        render_delivery_map(scene, sizes, scene_out.parent / "city_delivery.png")

    print("city2engine drive OK: blocks=%d glb=%d nmesh_suites=%d dag=%d "
          "scene_refs=%d scene=%s"
          % (len(block_ids), len(sizes), len(sizes), len(sizes), ref_count,
             rel_to_repo(scene_out)))
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
        ap.add_argument("--seed", type=int, required=True)
        ap.add_argument("--size", type=int, required=True)
        ap.add_argument("--nanite", default=str(REPO_ROOT / "build" / "DX12" / "nanite-tools.exe"))
        ap.add_argument("--models-out", default=None)
        ap.add_argument("--meshlets-out", default=None)
        ap.add_argument("--scene-out", default=None)
        ap.add_argument("--max-levels", type=int, default=4)
        ap.add_argument("--stats", action="store_true")
        ap.add_argument("--no-evidence", action="store_true")
        a = ap.parse_args(argv[1:])
        return cmd_drive(a)
    sys.stderr.write(USAGE)
    return 2


if __name__ == "__main__":
    sys.exit(main())
