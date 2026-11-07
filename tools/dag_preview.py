#!/usr/bin/env python3
"""M3.0 visual evidence: layered cluster-DAG structure drawn from the *real*
`nanite-tools dag --export` JSON. One column per LOD level; every DAG group is
a box sized by its member-cluster count and colored by its ParentLODError
(blue = fine, red = coarse); edges connect a group to the coarser groups that
own the clusters it generated (group-atomic cut neighbourhood, S5 semantics).
No mockups: every node/edge comes from the binary assets via the dag tool.

Usage: python tools/dag_preview.py <dag-export.json> <out.png>
"""

import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SS = 2                       # supersampling factor
COL_W = 190                  # column width
NODE_H = 34                  # node slot height
GAP = 24
TOP, BOTTOM = 96, 56
BG = (18, 22, 28)
PANEL_BG = (24, 29, 36)
EDGE = (46, 56, 68)
RED = (235, 72, 72)
TEXT = (225, 232, 240)
MUTED = (140, 155, 170)
LINE = (58, 72, 88)


def font(size: int):
    for name in ("C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/arial.ttf"):
        if Path(name).exists():
            return ImageFont.truetype(name, size)
    return ImageFont.load_default()


def error_color(t: float) -> tuple[int, int, int]:
    """blue (fine) -> green -> yellow -> red (coarse). t in [0,1]."""
    stops = [(70, 130, 230), (80, 200, 140), (230, 200, 80), (235, 90, 70)]
    x = min(max(t, 0.0), 1.0) * (len(stops) - 1)
    i = min(int(x), len(stops) - 2)
    f = x - i
    a, b = stops[i], stops[i + 1]
    return tuple(int(a[k] + (b[k] - a[k]) * f) for k in range(3))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dag_json")
    ap.add_argument("out")
    args = ap.parse_args()

    doc = json.loads(Path(args.dag_json).read_text(encoding="utf-8"))
    levels = doc["levels"]

    # cluster id -> owning group id, per level (from children membership lists)
    cluster2group = []
    for lv in levels:
        m = [0] * lv["clusters"]
        for g in lv["groups"]:
            for c in g["children"]:
                m[c] = g["id"]
        cluster2group.append(m)

    # DAG edges between group nodes: g(L) -> h(L+1) when a generated cluster
    # of g belongs to h (aggregated from the per-cluster parents lists).
    edges = []
    for L in range(len(levels) - 1):
        seen = set()
        for g in levels[L]["groups"]:
            for p in g["parents"]:
                h = cluster2group[L + 1][p]
                if (g["id"], h) not in seen:
                    seen.add((g["id"], h))
                    edges.append((L, g["id"], h))

    max_groups = max(len(lv["groups"]) for lv in levels)
    max_err = max(g["error"] for lv in levels for g in lv["groups"]) or 1.0
    W = TOP * 0 + GAP + len(levels) * (COL_W + GAP)
    H = TOP + max_groups * NODE_H + BOTTOM

    img = Image.new("RGB", (W * SS, H * SS), BG)
    d = ImageDraw.Draw(img)
    f_big, f_mid, f_small = font(24 * SS), font(15 * SS), font(12 * SS)

    d.text((GAP * SS, 14 * SS), "M3.0 / s3-dag   CLUSTER DAG (group = parent node)", font=f_big, fill=TEXT)
    n_groups = sum(len(lv["groups"]) for lv in levels)
    d.text((GAP * SS, 48 * SS),
           f"{doc['prefix']}   {len(levels)} levels   {n_groups} groups   "
           f"edges {len(edges)}   maxError {max_err:.4f} (world)",
           font=f_mid, fill=MUTED)

    # column layout: groups stacked in id order, box height by member count
    node_rect = []   # node_rect[L][gid] = (x0,y0,x1,y1)
    centers = {}
    for L, lv in enumerate(levels):
        x0 = GAP + L * (COL_W + GAP)
        cy = TOP
        node_rect.append({})
        n = len(lv["groups"])
        # compress slots when few groups so columns don't look sparse
        slot = NODE_H if n * NODE_H > max_groups * NODE_H * 0.55 else max(NODE_H, (max_groups * NODE_H) // max(n, 1))
        for g in lv["groups"]:
            members = len(g["children"])
            h = slot - 8
            w = COL_W - 36
            box = (x0, cy, x0 + w, cy + h)
            node_rect[L][g["id"]] = box
            centers[(L, g["id"])] = ((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
            cy += slot
        d.text((x0 * SS, (TOP - 26) * SS),
               f"L{lv['level']}  {lv['triangles']} tri  {n} groups",
               font=f_mid, fill=TEXT)

    # edges first (under nodes)
    for L, gi, hi in edges:
        x0, y0 = centers[(L, gi)]
        x1, y1 = centers[(L + 1, hi)]
        mx = (x0 + x1) // 2
        d.line([(x0 * SS, y0 * SS), (mx * SS, y0 * SS), (mx * SS, y1 * SS),
                (x1 * SS, y1 * SS)], fill=LINE, width=SS)

    # nodes
    for L, lv in enumerate(levels):
        for g in lv["groups"]:
            x0, y0, x1, y1 = node_rect[L][g["id"]]
            t = g["error"] / max_err
            col = error_color(t)
            d.rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS],
                        fill=tuple(int(c * 0.35 + PANEL_BG[k] * 0.65)
                                   for k, c in enumerate(col)),
                        outline=col, width=SS)
            d.text(((x0 + 6) * SS, (y0 + 3) * SS),
                   f"g{g['id']}  {len(g['children'])}clu", font=f_small, fill=TEXT)
            d.text(((x0 + 6) * SS, (y0 + 16) * SS),
                   f"err {g['error']:.3f}", font=f_small, fill=MUTED)

    d.text((GAP * SS, (H - BOTTOM + 10) * SS),
           "box size slot = group id order, color = ParentLODError (blue fine -> red coarse), "
           "label = member clusters; edge = generated clusters owned by coarser group",
           font=f_small, fill=MUTED)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    img = img.resize((W, H), Image.LANCZOS)
    img.save(out)
    print(f"wrote {out}  {W}x{H}  levels={len(levels)} groups={n_groups} edges={len(edges)}")


if __name__ == "__main__":
    main()
