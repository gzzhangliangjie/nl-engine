# -*- coding: utf-8 -*-
"""M-E1b.5 工具版证据图生成器。

铁律：所有内容从工具**真实输出**渲染（example/ecs-parallel/*.log，
由 nanite-scene.exe 实跑重定向落盘），不许手绘示意。产物进 example/ecs-parallel/。
复现：example/ecs-parallel/run-parallel.bat（重跑四用例 -> 重新生成本图 -> exit 0 校验）。
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, "example", "ecs-parallel")
OUT = LOGS

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_log(name):
    with open(os.path.join(LOGS, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def text(dr, xy, s, size=14, color=FG, bold=False):
    dr.text(xy, s, font=font(size, bold), fill=color)


# ---------------------------------------------------------------------------
# 图 1：selftest 逐条断言（真实 verbose 日志）+ 用例分组
# ---------------------------------------------------------------------------
def make_selftest_png():
    log = read_log("selftest-ecs-parallel.log")
    oks = [ln.strip() for ln in log.splitlines() if ln.strip().startswith("ok ")]
    verdict = "ecs-parallel selftest: PASS" in log

    W, H = 1180, 220 + 26 * len(oks) + 120
    im = Image.new("RGB", (W, H), BG)
    dr = ImageDraw.Draw(im)
    text(dr, (24, 20), "M-E1b.5  ecs-parallel selftest  (nanite-scene.exe, DX12 build)", 20, FG, True)
    text(dr, (24, 52), "read/write-set solver + jobgraph DAG + structural-change guard  --  real log: selftest-ecs-parallel.log", 13, DIM)

    groups = {
        "1. dependency order": ["2 edges", "post-write", "deterministic"],
        "2. true parallel overlap": ["saw spin"],
        "3. serial parity": ["parallel == serial", "parallel frame"],
        "4. bare-type conservative": ["bare-type"],
        "5. structural guard": ["violation", "forbidden", "offending API", "usable after"],
        "6. firstTick once": ["firstTick", "tick once"],
    }

    y = 92
    for ln in oks:
        color = GREEN
        for g, keys in groups.items():
            if any(k in ln for k in keys):
                color = CYAN if g != "5. structural guard" else AMBER
                break
        text(dr, (32, y), ln, 13, color)
        y += 26

    y += 8
    dr.rectangle([24, y, 24 + 8, y + 8], fill=GREEN)
    text(dr, (44, y - 4), "ecs-parallel selftest: PASS" if verdict else "ecs-parallel selftest: FAIL", 18, GREEN if verdict else RED, True)
    y += 44
    text(dr, (32, y), "contract: structural changes forbidden during tickParallel (violation -> exit false + located err)", 13, AMBER)
    text(dr, (32, y + 24), "determinism: conflict edge = registration order, acyclic by construction; parallel == serial bitwise", 13, DIM)

    im.save(os.path.join(OUT, "selftest-ecs-parallel.png"))
    return len(oks), verdict


# ---------------------------------------------------------------------------
# 图 2：求解器依赖图（parallel-dot 真实 DOT 输出解析，不手绘拓扑）
# ---------------------------------------------------------------------------
def make_solver_png():
    dot = read_log("parallel-dot.log")
    nodes = re.findall(r't(\d+) \[label="([^"]+)"\]', dot)
    edges = re.findall(r't(\d+) -> t(\d+);', dot)
    assert nodes, "parallel-dot.log missing nodes"

    labels = {int(i): lb for i, lb in nodes}
    n = len(labels)
    W, H = 1180, 520
    im = Image.new("RGB", (W, H), BG)
    dr = ImageDraw.Draw(im)
    text(dr, (24, 20), "M-E1b.5  read/write-set dependency solver  (real DOT from parallel-dot)", 20, FG, True)
    text(dr, (24, 52), "conflict = writer vs reader/writer on same component; edge direction = registration order (acyclic)", 13, DIM)

    # 节点布局（扇形）：有出边的源（写者）col0 居左，有入边者（读者）col1
    # 竖排扇出，孤立系统 col2 居右——边上不过第三节点，不误导成链。
    pos = {}
    has_out = {int(a) for a, _ in edges}
    has_in = {int(b) for _, b in edges}
    col0 = sorted(i for i in labels if i in has_out and i not in has_in)
    col1 = sorted(has_in - has_out)
    col2 = sorted(i for i in labels if i not in has_out and i not in has_in)

    def stack(col_ids, x, y_center, gap=120):
        span = gap * (len(col_ids) - 1)
        y0 = y_center - span / 2.0
        for k, i in enumerate(col_ids):
            pos[i] = (x, int(y0 + gap * k))

    mid_y = 240
    stack(col0, 240, mid_y)
    stack(col1, 600, mid_y)
    stack(col2, 950, mid_y)

    for a, b in edges:
        ax, ay = pos[int(a)]
        bx, by = pos[int(b)]
        ar, br = 92, 30
        dr.line([ax + ar, ay, bx - ar - 12, by], fill=AMBER, width=3)
        dr.polygon([(bx - ar, by), (bx - ar - 14, by - 8), (bx - ar - 14, by + 8)], fill=AMBER)

    kinds = {}
    for i, lb in labels.items():
        writes = ("Writer" in lb) or ("Spin" in lb) or ("Chain" in lb) or ("Bare" in lb)
        kinds[i] = AMBER if writes else CYAN

    for i, (x, y) in pos.items():
        lb = labels[i]
        dr.rounded_rectangle([x - 92, y - 30, x + 92, y + 30], 10,
                             outline=kinds[i], width=3, fill=(30, 33, 40))
        text(dr, (x - 84, y - 12), lb, 15, FG, True)

    text(dr, (150, H - 78), "amber = write access (exclusive)      cyan = read-only (parallel-friendly)", 14, DIM)
    text(dr, (150, H - 50), "edges: %d    nodes: %d    independent systems run concurrently (ParLightC)" % (len(edges), n), 14, DIM)

    im.save(os.path.join(OUT, "solver-graph.png"))
    return len(edges), n


if __name__ == "__main__":
    oks, verdict = make_selftest_png()
    edges, nodes = make_solver_png()
    if not verdict:
        raise SystemExit("selftest log says FAIL -- refusing to render PASS evidence")
    print("evidence: selftest-ecs-parallel.png (%d assertions) + solver-graph.png (%d edges / %d nodes)"
          % (oks, edges, nodes))
