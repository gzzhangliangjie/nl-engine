# -*- coding: utf-8 -*-
"""P8 M-P0 physics evidence generator.

IRON RULE: every pixel's data comes from the REAL tool output --
example/phys-p0/drop.tsv (Jolt 5.3.1 solver trajectory) and the *.log files
captured from phys-tools.exe. No hand-drawn / synthetic curves.

Products (both inside example/phys-p0/):
  trajectory.png   y(t) curve + analytic free-fall curve + contact/bounce/rest marks
  acceptance.png   terminal-style render of the real selftest / check / usage / negative logs

Reproduce: example/phys-p0/run.bat (re-runs the tool, rewrites logs+TSV,
regenerates both PNGs, exits 0 only when everything is green).
"""
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "example", "phys-p0")

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)
GRID = (42, 46, 54)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_config_and_samples():
    cfg = {}
    rows = []
    with open(os.path.join(D, "drop.tsv"), encoding="ascii") as f:
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
            elif line and not line.startswith("#") and not line.startswith("frame\t"):
                parts = line.split("\t")
                if len(parts) == 4:
                    rows.append((int(parts[0]), float(parts[1]),
                                 float(parts[2]), float(parts[3])))
    assert rows, "empty trajectory"
    return cfg, rows


def read_log(name):
    path = os.path.join(D, name)
    if not os.path.exists(path):
        return ["(missing %s)" % name]
    with open(path, encoding="ascii", errors="replace") as f:
        return f.read().rstrip("\n").split("\n")


def make_trajectory():
    cfg, rows = read_config_and_samples()
    W, H = 1100, 660
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f16, f18, f22 = font(16), font(18), font(22, True)

    d.text((28, 20), "P8 M-P0  Jolt 5.3.1 drop trajectory  (REAL solver output)",
           font=f22, fill=CYAN)
    d.text((28, 52), "source: example/phys-p0/drop.tsv  |  phys-tools simulate --frames %d"
           % int(cfg.get("frames", len(rows) - 1)), font=f16, fill=DIM)

    L, R, T, B = 90, 800, 100, 560
    t_max = rows[-1][1]
    y_max = max(r[2] for r in rows) * 1.08

    def px(t, y):
        return (L + (t / t_max) * (R - L), B - (y / y_max) * (B - T))

    # grid + axes
    for i in range(0, 7):
        y = i * 1.0
        if y > y_max:
            break
        _, gy = px(0, y)
        d.line([(L, gy), (R, gy)], fill=GRID, width=1)
        d.text((L - 42, gy - 9), "%.1f" % y, font=f16, fill=DIM)
    for i in range(0, 5):
        t = i * 1.0
        if t > t_max:
            break
        gx, _ = px(t, 0)
        d.line([(gx, T), (gx, B)], fill=GRID, width=1)
        d.text((gx - 10, B + 10), "%.0fs" % t, font=f16, fill=DIM)
    d.text((L - 62, T - 26), "y [m]", font=f16, fill=FG)
    d.text((R - 50, B + 34), "t [s]", font=f16, fill=FG)

    # analytic free fall (semi-implicit Euler closed form) up to first contact
    g = cfg.get("gravity", 9.8)
    dt = cfg.get("dt", 1.0 / 60.0)
    h = cfg.get("height", 5.0)
    radius = cfg.get("radius", 0.5)
    rest = cfg.get("restitution", 0.6)
    analytic_fall = math.sqrt(2.0 * (h - radius) / g)
    analytic_pts = []
    n = 0
    while n * dt <= analytic_fall:
        y = h - 0.5 * g * dt * dt * n * (n + 1)
        analytic_pts.append(px(n * dt, y))
        n += 1
    if len(analytic_pts) > 1:
        d.line(analytic_pts, fill=AMBER, width=2)

    # floor / rest line
    _, fy = px(0, radius)
    d.line([(L, fy), (R, fy)], fill=RED, width=2)
    d.text((L + 8, fy + 8), "floor top + radius (y=%.2f)" % radius, font=f16, fill=RED)

    # real trajectory
    pts = [px(r[1], r[2]) for r in rows]
    d.line(pts, fill=CYAN, width=3)

    # first contact + bounce apex
    fall_frame = next((r[0] for r in rows[1:] if r[2] <= radius + 0.005), None)
    if fall_frame is not None:
        ft = fall_frame * dt
        cx, cy = px(ft, radius)
        d.ellipse((cx - 5, cy - 5, cx + 5, cy + 5), fill=AMBER)
        d.text((cx + 10, cy - 30), "contact  t=%.3fs (frame %d)" % (ft, fall_frame),
               font=f16, fill=AMBER)
    bounce_peak = max(rows[fall_frame:], key=lambda r: r[2]) if fall_frame else rows[0]
    bx, by = px(bounce_peak[1], bounce_peak[2])
    d.ellipse((bx - 5, by - 5, bx + 5, by + 5), fill=GREEN)
    d.text((bx + 12, by - 32), "bounce apex  y=%.3f" % bounce_peak[2], font=f16, fill=GREEN)
    predicted = radius + rest * rest * (h - radius)
    d.text((bx + 12, by - 12), "e^2 law predicts %.3f" % predicted, font=f16, fill=DIM)

    # legend + real metrics panel
    lx, ly = 830, 130
    d.text((lx, ly), "LEGEND", font=f18, fill=FG)
    d.line([(lx, ly + 32), (lx + 40, ly + 32)], fill=CYAN, width=3)
    d.text((lx + 50, ly + 24), "Jolt 5.3.1 y(t)", font=f16, fill=FG)
    d.line([(lx, ly + 60), (lx + 40, ly + 60)], fill=AMBER, width=2)
    d.text((lx + 50, ly + 52), "analytic free fall", font=f16, fill=FG)
    d.line([(lx, ly + 88), (lx + 40, ly + 88)], fill=RED, width=2)
    d.text((lx + 50, ly + 80), "rest height", font=f16, fill=FG)

    d.text((lx, ly + 140), "MEASURED (from TSV)", font=f18, fill=CYAN)
    lines = [
        "jolt        %s" % cfg.get("jolt", "5.3.1"),
        "job system  single_threaded",
        "dt          %.6f s" % dt,
        "gravity     %.1f m/s^2" % g,
        "height      %.2f m" % h,
        "radius      %.2f m" % radius,
        "restitution %.2f" % rest,
        "frames      %d" % int(cfg.get("frames", len(rows) - 1)),
        "contact     %.3f s" % (fall_frame * dt if fall_frame else -1),
        "sqrt(2h/g)  %.3f s" % analytic_fall,
        "bounce apex %.3f m" % bounce_peak[2],
        "e^2 law     %.3f m" % predicted,
        "rest y      %.3f m" % rows[-1][2],
    ]
    for i, s in enumerate(lines):
        d.text((lx, ly + 172 + i * 24), s, font=f16, fill=FG if i < 8 else GREEN)

    d.text((28, H - 34), "determinism: same DropConfig -> bit-identical samples (phys-tools selftest)",
           font=f16, fill=DIM)
    out = os.path.join(D, "trajectory.png")
    img.save(out)
    print("wrote", out)


def make_acceptance():
    blocks = [
        ("phys-tools selftest --verbose", "selftest.log", GREEN),
        ("phys-tools check drop.tsv", "check.log", GREEN),
        ("phys-tools   [usage, exit 2]", "usage.log", CYAN),
        ("phys-tools simulate --restitution 1.5   [negative, exit 2]", "negative.log", AMBER),
    ]
    lines_out = []
    for title, name, color in blocks:
        lines_out.append(("=== " + title + " ===", CYAN))
        for line in read_log(name):
            c = color if ("PASS" in line or "ok " in line) else FG
            if "FAIL" in line:
                c = RED
            if line.startswith("#") or line.startswith("phys-p0") or "invalid" in line:
                c = AMBER if "invalid" in line else c
            lines_out.append((line, c))
        lines_out.append(("", FG))

    W = 1160
    lh = 21
    H = 80 + len(lines_out) * lh + 40
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f15, f20 = font(15), font(20, True)
    d.text((24, 20), "P8 M-P0 acceptance - real phys-tools output (Jolt 5.3.1)",
           font=f20, fill=CYAN)
    d.text((24, 50), "reproduce: example/phys-p0/run.bat  |  logs + drop.tsv captured from the exe",
           font=f15, fill=DIM)
    y = 80
    for text, color in lines_out:
        d.text((24, y), text[:150], font=f15, fill=color)
        y += lh
    out = os.path.join(D, "acceptance.png")
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    make_trajectory()
    make_acceptance()
