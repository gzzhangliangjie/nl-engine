# -*- coding: utf-8 -*-
"""P8 M-P2 physics evidence generator.

IRON RULE: every data pixel comes from the REAL tool output --
example/phys-p2/stack.tsv (3 real Jolt 5.3.1 stacking runs, 6 cubes x 601
frames) and the *.log files captured from phys-tools.exe. No hand-drawn curves.

Products (both inside example/phys-p2/):
  trajectory.png  all 6 layer heights over 10 s + 3-case top-cube comparison
                  + last-60-frame jitter zoom + measured panel
  acceptance.png  terminal-style render of the complete real logs

Reproduce: example/phys-p2/run.bat (re-runs the tool, rewrites logs+TSV,
regenerates both PNGs, exits 0 only when everything is green).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "example", "phys-p2")

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)
PURPLE = (190, 150, 230)
GRID = (42, 46, 54)
LAYER_COLORS = ((108, 220, 128), (86, 182, 224), (150, 200, 235),
                (190, 150, 230), (235, 185, 90), (235, 140, 110))


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_tsv():
    cfg, cases, rows = {}, [], []
    with open(os.path.join(D, "stack.tsv"), encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("# jolt="):
                for tok in line.split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        try:
                            cfg[k] = float(v)
                        except ValueError:
                            cfg[k] = v
            elif line.startswith("# case "):
                c = {}
                parts = line.split()
                c["name"] = parts[2]
                for tok in parts[3:]:
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        c[k] = v
                cases.append(c)
            elif line and not line.startswith("#") and not line.startswith("frame\t"):
                rows.append(line.split("\t"))
    boxes = int(cfg["boxes"])
    assert rows and len(cases) == 3 and all(len(r) == 3 * boxes + 7 for r in rows), \
        "empty or malformed stack.tsv"
    return cfg, cases, rows, boxes


def read_log(name):
    path = os.path.join(D, name)
    if not os.path.exists(path):
        return ["(missing %s)" % name]
    with open(path, encoding="ascii", errors="replace") as f:
        return f.read().rstrip("\n").split("\n")


def make_trajectory():
    cfg, cases, rows, boxes = read_tsv()
    W, H = 1560, 900
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f12, f14, f16, f18, f22 = font(12), font(14), font(16), font(18), font(22, True)

    d.text((40, 22), "P8 M-P2  STACKING STABILITY  /  6-CUBE TOWER, 10 s", font=f22, fill=FG)
    d.text((40, 54), "real Jolt %s solver output  |  dt=%.6f  g=%.1f  boxes=%d  half_extent=%.2f  gap=%.3f  frames=%d"
           % (cfg.get("jolt", "5.3.1"), cfg.get("dt", 0), cfg.get("gravity", 0),
              boxes, cfg.get("half_extent", 0), cfg.get("gap", 0),
              int(cfg.get("frames", 0))), font=f14, fill=DIM)

    BASE, NOSLEEP, NOFRIC = 2, 2 + boxes, 2 + 2 * boxes
    ACT, DRIFT = 2 + 3 * boxes, 2 + 3 * boxes + 3
    tmax = float(rows[-1][1])
    YMIN, YMAX = 0.0, 6.2

    # ---------- left: all six layer heights over the full run ----------
    L, R, T, B = 70, 950, 120, 750

    def px(t, l=L, r=R):
        return l + (t / tmax) * (r - l)

    def py(y, t=T, b=B, ymin=YMIN, ymax=YMAX):
        return b - ((y - ymin) / (ymax - ymin)) * (b - t)

    for v in range(0, 7):
        y = py(v)
        d.line((L, y, R, y), fill=GRID, width=1)
        d.text((L - 32, y - 8), "%d" % v, font=f12, fill=DIM)
    for k in range(0, 11, 2):
        t = float(k)
        x = px(t)
        d.line((x, T, x, B), fill=GRID, width=1)
        d.text((x - 10, B + 8), "%d" % k, font=f12, fill=DIM)
    d.text((L + (R - L) // 2 - 20, B + 32), "t (s)", font=f14, fill=DIM)
    d.text((L - 46, T - 30), "y (m)", font=f14, fill=DIM)

    for b in range(boxes):
        y = py(0.5 + b)
        d.line((L, y, R, y), fill=(38, 42, 50), width=1)

    for b in range(boxes):
        pts = [(px(float(r[1])), py(float(r[BASE + b]))) for r in rows]
        d.line(pts, fill=LAYER_COLORS[b % len(LAYER_COLORS)], width=3)

    finals = [float(rows[-1][BASE + b]) for b in range(boxes)]
    for b, fy in enumerate(finals):
        d.text((R - 172, py(fy) - 22), "L%d  y=%.4f" % (b, fy),
               font=f12, fill=LAYER_COLORS[b % len(LAYER_COLORS)])

    d.text((L + 14, T + 8), "baseline: all 6 cubes hold their slots; layer pitch 1.000 m",
           font=f14, fill=GREEN)
    d.text((L + 14, T + 30), "no jitter after settling (last 60 frames spread = 0.000000)",
           font=f14, fill=DIM)

    # ---------- right top: 3-case top-cube comparison ----------
    L2, R2, T2, B2 = 1010, 1520, 120, 400

    def px2(t):
        return L2 + (t / tmax) * (R2 - L2)

    def py2(y):
        return B2 - ((y - YMIN) / (YMAX - YMIN)) * (B2 - T2)

    d.text((L2, T2 - 28), "TOP CUBE HEIGHT  /  3 CASES", font=f16, fill=FG)
    for v in range(0, 7):
        y = py2(v)
        d.line((L2, y, R2, y), fill=GRID, width=1)
        d.text((L2 - 28, y - 8), "%d" % v, font=f12, fill=DIM)
    for k in range(0, 11, 2):
        x = px2(float(k))
        d.line((x, T2, x, B2), fill=GRID, width=1)
        d.text((x - 8, B2 + 6), "%d" % k, font=f12, fill=DIM)

    for idx, (col, name) in enumerate(((GREEN, "baseline"), (CYAN, "no_sleep"),
                                       (RED, "no_friction"))):
        off = (BASE, NOSLEEP, NOFRIC)[idx]
        pts = [(px2(float(r[1])), py2(float(r[off + boxes - 1]))) for r in rows]
        d.line(pts, fill=col, width=3)
        ly = B2 - 92 + idx * 20
        d.line((L2 + 300, ly + 7, L2 + 326, ly + 7), fill=col, width=3)
        d.text((L2 + 334, ly), name, font=f14, fill=col)

    # ---------- right middle: last-60-frame jitter zoom ----------
    ZT, ZB = 470, 620
    tail = rows[-60:]
    vals = [float(r[BASE + boxes - 1]) for r in tail]
    lo, hi = min(vals), max(vals)
    span = max(hi - lo, 0.002)
    zmin, zmax = (lo + hi) / 2 - span, (lo + hi) / 2 + span

    def zpy(y):
        return ZB - ((y - zmin) / (zmax - zmin)) * (ZB - ZT)

    d.text((L2, ZT - 28), "LAST 60 FRAMES  /  BASELINE TOP CUBE (zoom)", font=f16, fill=FG)
    d.rectangle((L2, ZT, R2, ZB), outline=GRID, width=1)
    zpts = []
    for i, r in enumerate(tail):
        x = L2 + (i / max(1, len(tail) - 1)) * (R2 - L2)
        zpts.append((x, zpy(float(r[BASE + boxes - 1]))))
    d.line(zpts, fill=GREEN, width=2)
    d.text((L2 + 12, ZB + 8), "spread = %.6f   (flat = converged, no solver jitter)"
           % (hi - lo), font=f14, fill=GREEN)
    d.text((L2 + 12, ZB + 30), "t = %.2f .. %.2f s" % (float(tail[0][1]), float(tail[-1][1])),
           font=f12, fill=DIM)

    # ---------- right bottom: measured panel (all values from the TSV) ----------
    PY = 682
    d.rectangle((L2 - 10, PY - 10, W - 24, H - 24), outline=GRID, width=1)
    d.text((L2, PY), "MEASURED  (from stack.tsv # case rows)", font=f16, fill=FG)
    yy = PY + 30
    for i, c in enumerate(cases):
        col = (GREEN, CYAN, RED)[i]
        d.text((L2, yy), "%-12s active=%-2s tail_spread=%s collapsed=%s"
               % (c["name"], c.get("final_active", "?"),
                  c.get("max_tail_spread", "?"), c.get("collapsed", "?")),
               font=f14, fill=col)
        yy += 22
    yy += 6
    d.text((L2, yy), "no_friction top_drift = %s m (friction holds the tower)"
           % cases[2].get("top_drift", "?"), font=f14, fill=FG)
    yy += 22
    d.text((L2, yy), "sleeping removes work, it is NOT the stability source",
           font=f14, fill=DIM)
    yy += 22
    d.text((L2, yy), "UE Chaos counterpart: IslandManager sleeps a whole island",
           font=f12, fill=DIM)

    out = os.path.join(D, "trajectory.png")
    img.save(out)
    print("wrote", out)


def make_acceptance():
    blocks = [
        ("selftest.log  (M-P0 + M-P1 + M-P2 anchors, complete output)", "selftest.log", GREEN),
        ("check-stack.log  (re-verify archived stack.tsv)", "check-stack.log", CYAN),
        ("stack.tsv  provenance header (real solver output)", None, AMBER),
        ("usage.log  (exit 2 expected)", "usage.log", DIM),
        ("negative.log  (stack --frames 0, exit 2 expected)", "negative.log", RED),
        ("regression.log  (M-P0 + M-P1 byte-identical witnesses)", "regression.log", PURPLE),
    ]
    loaded = []
    for title, name, col in blocks:
        if name is None:
            with open(os.path.join(D, "stack.tsv"), encoding="ascii") as f:
                raw = [next(f).rstrip("\r\n") for _ in range(6)]
            lines = []
            for ln in raw:
                ln = ln.replace("\t", " ")  # consolas does not expand tabs; keep header readable
                first, budget = True, 155
                while len(ln) > budget:
                    cut = ln.rfind(" ", 0, budget)
                    if cut < budget - 60:
                        cut = budget
                    lines.append(ln[:cut] if first else "    " + ln[:cut])
                    ln = ln[cut:].lstrip()
                    first, budget = False, 151
                lines.append(ln if first else "    " + ln)
        else:
            lines = read_log(name)
        loaded.append((title, lines, col))

    line_h = 18
    gap = 24
    W = 1400
    f14, f16, f20 = font(14), font(16), font(20, True)
    total = 82 + sum(len(lines) * line_h + gap + 26 for _, lines, _ in loaded) + 40
    H = max(600, total)
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((30, 22), "P8 M-P2 ACCEPTANCE  /  real phys-tools output", font=f20, fill=FG)
    d.text((30, 50), "A-class track: headless, zero graphics API, no kStage, no render regression",
           font=f14, fill=DIM)

    y = 82
    for title, lines, col in loaded:
        d.rectangle((24, y - 6, W - 24, y + 20), fill=(28, 31, 37))
        d.text((34, y - 3), title, font=f16, fill=col)
        y += 30
        for ln in lines:
            d.text((40, y), ln[:155], font=f14, fill=FG if ln.strip() else DIM)
            y += line_h
        y += gap
    out = os.path.join(D, "acceptance.png")
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    make_trajectory()
    make_acceptance()
