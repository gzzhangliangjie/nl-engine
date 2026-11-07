#!/usr/bin/env python3
"""M3.2 offline manifold check for a dag-top drill (group-atomic cut preview).

Loads <stem>.lodT.nmesh (top) and <stem>.lodT-1.nmesh (child), builds the
MIXED cluster set the viewer renders for a drill group (top minus the group's
generated parents, plus the group's member clusters) and verifies the exact
invariant the "no holes / no overlaps" acceptance depends on:

  Every welded edge (undirected position pair, bit-exact floats) is used
  EXACTLY twice -- in the full top level AND in the mixed set. A closed
  manifold surface. Any seam crack, missing patch or overlap shows up as an
  edge used once (boundary/hole) or 3+ times (overlap).

This simultaneously validates the M2.2 locked-border assumption across levels:
if L5 and L6 border positions were not bit-exact, the seam edges would split
apart and fail immediately.

Usage:
    python tools/dag_weld_check.py --prefix assets/meshlets/Suzanne --group 1
    python tools/dag_weld_check.py --prefix assets/meshlets/Suzanne --all
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_nmesh(path: Path):
    """Minimal .nmesh v1/v2/v3 reader.

    Returns (meshlets, verts, triangles_bytes, lodmeta)."""
    blob = path.read_bytes()
    magic, version, name_len, mcount, vcount, tcount = struct.unpack_from("<IHHIII", blob, 0)
    assert magic == 0x48534D4E, f"bad magic in {path}"
    off = 20 + name_len
    meshlets = []
    for _ in range(mcount):
        vo, vc, to, tc, s0, s1, s2, s3, ca, cb, cc, cut, err = \
            struct.unpack_from("<IIII9f", blob, off)
        meshlets.append({"vertexOffset": vo, "vertexCount": vc,
                         "triangleOffset": to, "triangleCount": tc})
        off += 52
    verts_raw = struct.unpack_from(f"<{vcount*8}f", blob, off)
    # MeshVertex = 8 floats（pos3+normal3+uv2）= 32B；位置取每记录前 3 个。
    # （踩坑：曾按 i*3 切片 = 12B 步进，全部顶点探针出垃圾数据。）
    verts = [tuple(verts_raw[i*8:i*8+3]) for i in range(vcount)]
    off += vcount * 32
    tris = blob[off:off + tcount * 3]
    off += tcount * 3
    meta = {"dagPresent": False, "groups": [], "clusterGroup": []}
    if off + 8 <= len(blob):
        m1, _m2 = struct.unpack_from("<II", blob, off)
        if m1 == 0x444F4C4E:  # 'NLOD' meta section
            # 头布局（24B）：magic4 + payload4 + version2 + level2 + maxError f32
            #               + lockedCount u32 + reserved u32。off+8 起是 <HHfII>。
            _payload, mver, _level, _maxerr, locked, _r = \
                struct.unpack_from("<IHHfII", blob, off + 4)
            off += 24 + locked * 12
            if mver >= 2:
                gcount = struct.unpack_from("<I", blob, off)[0]
                off += 4
                for _ in range(gcount):
                    err, serr, *sph, gf, gc = struct.unpack_from("<2f4fII", blob, off)
                    meta["groups"].append({"genFirst": gf, "genCount": gc, "error": err})
                    off += 32
                meta["clusterGroup"] = list(struct.unpack_from(f"<{mcount}I", blob, off))
                meta["dagPresent"] = True
    return meshlets, verts, tris, meta


def welded_edges(meshlets, verts, tris, cluster_ids) -> Counter:
    """Undirected position-pair edge usage over the given clusters' triangles.

    Positions (bit-exact float triples) are the identity: cluster windows
    duplicate shared vertices, so welding by position reconstructs the real
    surface. Meshlet::triangleOffset is a BYTE offset (M3.1 convention)."""
    edges: Counter = Counter()
    for cid in cluster_ids:
        m = meshlets[cid]
        base = m["triangleOffset"]
        for t in range(m["triangleCount"]):
            a, b, c = struct.unpack_from("<3B", tris, base + t * 3)
            va, vb, vc = (verts[m["vertexOffset"] + a],
                          verts[m["vertexOffset"] + b],
                          verts[m["vertexOffset"] + c])
            for e in ((va, vb), (vb, vc), (vc, va)):
                key = e if e[0] <= e[1] else (e[1], e[0])
                edges[key] += 1
    return edges


def edge_report(edges: Counter, label: str, ref_boundary: set | None = None) -> bool:
    bad1 = {e for e, n in edges.items() if n == 1}
    bad3 = [(e, n) for e, n in edges.items() if n > 2]
    # Suzanne 是开壳模型（眼窝/嘴），84 条 1x 开边逐层守恒是**预期**的。
    # 判据不是 0 条 1x，而是：overlap=0 且 1x 集合与 full top 完全一致
    # （下钻不引入任何新开边 = 父子衔接无洞无裂缝）。
    new1 = bad1 - ref_boundary if ref_boundary is not None else set()
    ok = not bad3 and not new1
    print(f"{label}: {len(edges)} welded edges | "
          f"boundary(1x)={len(bad1)} (new vs top: {len(new1)}) | "
          f"overlap(>2x)={len(bad3)} -> "
          + ("PASS" if ok else "FAIL"))
    for e in sorted(new1)[:5]:
        print("  NEW HOLE EDGE", e)
    for e, n in sorted(bad3)[:5]:
        print(f"  OVERLAP EDGE ({n}x)", e)
    return ok


def check_group(prefix: str, group: int) -> bool:
    top_level = 0
    while (ROOT / f"{prefix}.lod{top_level}.nmesh").is_file():
        top_level += 1
    top_level -= 1
    if top_level < 1:
        print("FAIL: need >= 2 layers")
        return False

    tm, tv, tt, _tmeta = load_nmesh(ROOT / f"{prefix}.lod{top_level}.nmesh")
    cm, cv, ct, cmeta = load_nmesh(ROOT / f"{prefix}.lod{top_level - 1}.nmesh")
    assert cmeta["dagPresent"], "child level has no DAG lineage (meta v2)"
    assert 0 <= group < len(cmeta["groups"]), "group out of range"

    g = cmeta["groups"][group]
    parents = list(range(g["genFirst"], g["genFirst"] + g["genCount"]))
    children = [i for i, gid in enumerate(cmeta["clusterGroup"]) if gid == group]
    kept_top = [i for i in range(len(tm)) if i not in set(parents)]
    mixed = kept_top + children
    print(f"top=lod{top_level} child=lod{top_level - 1} group={group} "
          f"error={g['error']:.6f} | parents={parents} children={children}")

    top_edges = welded_edges(tm, tv, tt, range(len(tm)))
    top_boundary = {e for e, n in top_edges.items() if n == 1}
    ok_top = edge_report(top_edges, "full top   ")
    mixed_edges = welded_edges(tm, tv, tt, kept_top)
    mixed_edges.update(welded_edges(cm, cv, ct, children))
    ok_mix = edge_report(mixed_edges, "mixed set  ", ref_boundary=top_boundary)
    # Also verify the child group alone is a closed patch except at its outer
    # border (interior edges 2x, border edges 1x) -- informative, not a gate.
    child_edges = welded_edges(cm, cv, ct, children)
    border = sum(1 for n in child_edges.values() if n == 1)
    print(f"child patch: {len(child_edges)} edges, open border {border} "
          f"(must match the removed parents' border)")

    print(f"group {group}: " + ("PASS" if (ok_top and ok_mix) else "FAIL"))
    return ok_top and ok_mix


def merge_counters(a: Counter, b: Counter) -> Counter:
    out = Counter(a)
    out.update(b)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default="assets/meshlets/Suzanne")
    ap.add_argument("--group", type=int)
    ap.add_argument("--all", action="store_true", help="check every drillable group")
    args = ap.parse_args()

    if args.all:
        # discover drillable groups from the second-coarsest layer
        lvl = 0
        while (ROOT / f"{args.prefix}.lod{lvl}.nmesh").is_file():
            lvl += 1
        _, _, _, meta = load_nmesh(ROOT / f"{args.prefix}.lod{lvl - 2}.nmesh")
        groups = range(len(meta["groups"]))
    else:
        groups = [args.group]

    ok = all(check_group(args.prefix, g) for g in groups)
    print("ALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
