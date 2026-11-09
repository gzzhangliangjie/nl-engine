# -*- coding: utf-8 -*-
"""P8 M-P1 physics evidence generator.

IRON RULE: every pixel's data comes from the REAL tool output --
example/phys-p1/layers.tsv (4 real Jolt 5.3.1 layer-matrix trajectories),
the M-P0 baseline drop.tsv (byte-compare witness) and the *.log files
captured from phys-tools.exe. No hand-drawn / synthetic curves.

Products (both inside example/phys-p1/):
  trajectory.png  4 case curves + blocker/floor reference lines + measured panel
  acceptance.png  terminal-style render of the real logs

Reproduce: example/phys-p1/run.bat (re-runs the tool, rewrites logs+TSV,
regenerates both PNGs, exits 0 only when everything is green).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "example", "phys-p1")
D0 = os.path.join(ROOT, "example", "phys-p0")

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

COLUMNS = ("same_layer", "enabled_pair", "disabled_pair", "ghost")
COLORS = (GREEN, CYAN, AMBER, RED)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_tsv():
    cfg, cases, rows = {}, [], []
    with open(os.path.join(D, "layers.tsv"), encoding="ascii") as f:
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
                p = line.split("\t")
                if len(p) == 6:
                    rows.append((int(p[0]), float(p[1]),
                                 float(p[2]), float(p[3]), float(p[4]), float(p[5])))
    assert rows and len(cases) == 4, "empty or malformed layers.tsv"
    return cfg, cases, rows


def read_log(name):
    path = os.path.join(D, name)
    if not os.path.exists(path):
        return ["(missing %s)" % name]
    with open(path, encoding="ascii", errors="replace") as f:
        return f.read().rstrip("\n").split("\n")


def make_trajectory():
    cfg, cases, rows = read_tsv()
    W, H = 1440, 780
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f12, f14, f16, f18, f22 = font(12), font(14), font(16), font(18), font(22, True)

    d.text((40, 24), "P8 M-P1  COLLISION LAYER MATRIX PROBE", font=f22, fill=FG)
    d.text((40, 56), "real Jolt %s solver output  |  dt=%.6f  g=%.1f  r=%.2f  h=%.1f  blocker_y=%.2f  frames=%d"
           % (cfg.get("jolt", "5.3.1"), cfg.get("dt", 0), cfg.get("gravity", 0),
              cfg.get("radius", 0), cfg.get("start_height", 0),
              cfg.get("blocker_y", 0), int(cfg.get("frames", 0))),
           font=f14, fill=DIM)

    L, R, T, B = 70, 910, 110, 660
    YMIN, YMAX = -2.0, 7.0
    tmax = rows[-1][1]

    def px(t):
        return L + (t / tmax) * (R - L)

    def py(y):
        return B - ((y - YMIN) / (YMAX - YMIN)) * (B - T)

    # grid + y ticks
    for v in range(-2, 8):
        y = py(v)
        d.line((L, y, R, y), fill=GRID, width=1)
        d.text((L - 34, y - 8), "%+d" % v, font=f12, fill=DIM)
    for k in range(0, 7):
        t = k * 0.5
        x = px(t)
        d.line((x, T, x, B), fill=GRID, width=1)
        d.text((x - 12, B + 8), "%.1f" % t, font=f12, fill=DIM)
    d.text((L + (R - L) // 2 - 20, B + 30), "t (s)", font=f14, fill=DIM)
    d.text((L - 46, T - 30), "y (m)", font=f14, fill=DIM)

    blocker_y = float(cfg.get("blocker_y", 2.5))
    radius = float(cfg.get("radius", 0.5))
    contact = blocker_y + 2.0 * radius
    floor_rest = radius

    # reference lines
    for val, label, col, off in (
        (contact, "blocker contact  y=%.3f" % contact, GREEN, -22),
        (blocker_y, "blocker center  y=%.3f" % blocker_y, DIM, -20),
        (floor_rest, "floor rest  y=%.3f" % floor_rest, RED, 8),
        (0.0, "floor top  y=0", RED, 8),
    ):
        y = py(val)
        d.line((L, y, R, y), fill=col, width=1)
        d.text((L + 10, y + off), label, font=f14, fill=col)

    # 4 real curves
    for i, name in enumerate(COLUMNS):
        pts = []
        for row in rows:
            y = row[2 + i]
            if y < YMIN:
                break  # ghost goes off-chart; annotate the real final value instead
            pts.append((px(row[1]), py(y)))
        if len(pts) > 1:
            width = 3 if i == 0 else 2
            if i == 1:  # enabled_pair overlaps same_layer: dashed to stay visible
                for a in range(0, len(pts) - 1, 6):
                    b = min(a + 3, len(pts) - 1)
                    d.line((pts[a], pts[b]), fill=COLORS[i], width=3)
            else:
                d.line(pts, fill=COLORS[i], width=width)

    # off-chart arrow for the ghost case (real measured value from the case row)
    ghost_final = float(cases[3]["final_y"])
    gx = px(rows[-1][1])
    d.line((gx - 40, py(YMIN + 0.25), gx - 40, py(YMIN - 0.25)), fill=RED, width=2)
    d.polygon([(gx - 46, py(YMIN) + 22), (gx - 34, py(YMIN) + 22),
               (gx - 40, py(YMIN) + 38)], fill=RED)
    d.text((L + 250, B - 34), "ghost continues off-chart to y=%.3f (pass-through all layers)"
           % ghost_final, font=f14, fill=RED)

    # legend
    lx, ly = L + 470, T + 8
    for i, name in enumerate(COLUMNS):
        d.line((lx, ly + i * 20 + 7, lx + 26, ly + i * 20 + 7), fill=COLORS[i], width=3)
        d.text((lx + 34, ly + i * 20), name, font=f14, fill=COLORS[i])

    # right-side measured panel (all values read from the real TSV)
    PX = 950
    d.rectangle((PX - 14, T - 10, W - 24, B + 60), outline=GRID, width=1)
    d.text((PX, T + 4), "MEASURED  (from layers.tsv)", font=f18, fill=FG)
    yy = T + 40
    for c in cases:
        d.text((PX, yy), c["name"], font=f16, fill=CYAN)
        yy += 24
        allow = c.get("allow", "?")
        d.text((PX + 12, yy), "pair allow=%s   matrix=%s" % (allow, c.get("matrix", "?")),
               font=f14, fill=DIM)
        yy += 22
        d.text((PX + 12, yy), "final_y=%s   blocked=%s" % (c.get("final_y", "?"), c.get("blocked", "?")),
               font=f14, fill=FG)
        yy += 22
        d.text((PX + 12, yy), "owner_ok=%s   bodies=%s" % (c.get("owner_ok", "?"), c.get("bodies", "?")),
               font=f14, fill=GREEN if c.get("owner_ok") == "1" else RED)
        yy += 34
    d.line((PX, yy, W - 40, yy), fill=GRID, width=1)
    yy += 12
    d.text((PX, yy), "M-P0 baseline regression:", font=f14, fill=DIM)
    yy += 22
    reg = [l for l in read_log("regression.log") if "byte-identical" in l]
    d.text((PX, yy), (reg[0] if reg else "(missing)")[:62], font=f14, fill=GREEN)
    yy += 24
    d.text((PX, yy), "(byte-identical drop.tsv)", font=f12, fill=DIM)

    out = os.path.join(D, "trajectory.png")
    img.save(out)
    print("wrote", out)


def make_acceptance():
    blocks = [
        ("selftest.log  (M-P1 anchors + M-P0 regression)", "selftest.log", GREEN),
        ("check-layers.log  (re-verify archived layers.tsv)", "check-layers.log", CYAN),
        ("layers.tsv  provenance header (real solver output)", None, AMBER),
        ("usage.log  (exit 2 expected)", "usage.log", DIM),
        ("negative.log  (layers --frames 0, exit 2 expected)", "negative.log", RED),
        ("regression.log  (M-P0 byte-identical witness)", "regression.log", PURPLE),
    ]
    loaded = []
    for title, name, col in blocks:
        if name is None:
            with open(os.path.join(D, "layers.tsv"), encoding="ascii") as f:
                lines = [next(f).rstrip("\r\n") for _ in range(7)]
        else:
            lines = read_log(name)
        loaded.append((title, lines, col))

    line_h = 19
    gap = 26
    W = 1280
    f14, f16, f20 = font(14), font(16), font(20, True)
    total = 76 + sum(len(lines) * line_h + gap + 26 for _, lines, _ in loaded) + 40
    H = max(560, total)
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((30, 22), "P8 M-P1 ACCEPTANCE  /  real phys-tools output", font=f20, fill=FG)
    d.text((30, 50), "A-class track: headless, zero graphics API, no kStage, no render regression",
           font=f14, fill=DIM)

    y = 82
    for title, lines, col in loaded:
        d.rectangle((24, y - 6, W - 24, y + 20), fill=(28, 31, 37))
        d.text((34, y - 3), title, font=f16, fill=col)
        y += 30
        for ln in lines:
            d.text((40, y), ln[:150], font=f14, fill=FG if ln.strip() else DIM)
            y += line_h
        y += gap
    out = os.path.join(D, "acceptance.png")
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    make_trajectory()
    make_acceptance()
