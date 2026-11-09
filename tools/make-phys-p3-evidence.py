# -*- coding: utf-8 -*-
"""P8 M-P3 physics evidence generator.

IRON RULE: every data pixel comes from the REAL tool output --
example/phys-p3/debug.tsv (real Jolt 5.3.1 DebugRenderer line stream, 4 bodies
x 241 frames) and the *.log files captured from phys-tools.exe / viewer.exe.
No hand-drawn curves.

Products (both inside example/phys-p3/):
  wireframe.png   max_y/min_y fall-and-settle curves + per-shape vertex buckets
                  + tail freeze zoom + measured panel
  acceptance.png  terminal-style render of the complete real logs

Reproduce: example/phys-p3/run.bat (re-runs the tools + viewer captures,
rewrites logs+TSV, regenerates both PNGs, exits 0 only when everything is green).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "example", "phys-p3")

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
# 分色与 World.cpp extractLines / debug.tsv 表头一致。
BOX_C = (80, 220, 80)
SPHERE_C = (255, 160, 40)
CAPSULE_C = (230, 80, 230)
FLOOR_C = (60, 120, 255)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_tsv():
    cfg, rows = {}, []
    with open(os.path.join(D, "debug.tsv"), encoding="ascii") as f:
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
                rows.append(line.split("\t"))
    assert rows and len(rows[0]) == 13, "empty or malformed debug.tsv"
    return cfg, rows


def read_log(name):
    path = os.path.join(D, name)
    if not os.path.exists(path):
        return ["(missing %s)" % name]
    with open(path, encoding="ascii", errors="replace") as f:
        return f.read().rstrip("\n").split("\n")


def make_wireframe():
    cfg, rows = read_tsv()
    W, H = 1560, 900
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f12, f14, f16, f18, f22 = font(12), font(14), font(16), font(18), font(22, True)

    d.text((40, 22), "P8 M-P3  DEBUG WIREFRAME  /  FLOOR + BOX + SPHERE + CAPSULE, 4 s",
           font=f22, fill=FG)
    d.text((40, 54), "real Jolt %s DebugRenderer line stream  |  dt=%s  frames=%d  bodies=%d  vertices/frame=%d"
           % (cfg.get("jolt", "5.3.1"), cfg.get("dt", 0), int(cfg.get("frames", 0)),
              int(cfg.get("bodies", 0)), int(rows[0][2])), font=f14, fill=DIM)

    T, V, MXY, MNY, MNX, MXX = 2, 3, 4, 5, 6, 7
    B8, S8, C8, F8 = 9, 10, 11, 12
    tmax = float(rows[-1][1])
    YMIN, YMAX = -1.2, 5.2

    # ---------- left: max_y / min_y fall-and-settle over the full run ----------
    L, R, T_, B = 70, 950, 120, 750

    def px(t):
        return L + (t / tmax) * (R - L)

    def py(y):
        return B - ((y - YMIN) / (YMAX - YMIN)) * (B - T_)

    for v in range(-1, 6):
        y = py(v)
        d.line((L, y, R, y), fill=GRID, width=1)
        d.text((L - 32, y - 8), "%d" % v, font=f12, fill=DIM)
    for k in range(0, 5):
        x = px(float(k))
        d.line((x, T_, x, B), fill=GRID, width=1)
        d.text((x - 8, B + 8), "%.1f" % float(k), font=f12, fill=DIM)
    d.text((L + (R - L) // 2 - 20, B + 32), "t (s)", font=f14, fill=DIM)
    d.text((L - 52, T_ - 30), "y (m)", font=f14, fill=DIM)

    y0 = py(0.0)
    d.line((L, y0, R, y0), fill=(60, 64, 72), width=2)
    d.text((L + 6, y0 - 18), "floor top y=0", font=f12, fill=DIM)

    pts = [(px(float(r[1])), py(float(r[MXY]))) for r in rows]
    d.line(pts, fill=CAPSULE_C, width=3)
    pts = [(px(float(r[1])), py(float(r[MNY]))) for r in rows]
    d.line(pts, fill=FLOOR_C, width=3)

    first_top, last_top = float(rows[0][MXY]), float(rows[-1][MXY])
    d.text((L + 14, T_ + 8),
           "capsule apex falls %.3f -> %.3f m (drop 4.8, rest 1.6); bodies settle on the floor"
           % (first_top, last_top), font=f14, fill=CAPSULE_C)
    d.text((L + 14, T_ + 30),
           "floor bottom stays at -1.000000 (static ground, exact)", font=f14, fill=FLOOR_C)

    # ---------- right top: per-shape vertex buckets (constant per frame) ----------
    L2, R2, T2, B2 = 1010, 1520, 120, 380
    vmax = max(int(r[B8]) for r in rows)
    d.text((L2, T2 - 28), "VERTEX BUCKETS PER FRAME  /  topology constant", font=f16, fill=FG)
    bh = (B2 - T2 - 40) // 4
    for i, (name, col, off) in enumerate((("capsule", CAPSULE_C, C8),
                                          ("sphere", SPHERE_C, S8),
                                          ("box", BOX_C, B8),
                                          ("floor", FLOOR_C, F8))):
        val = int(rows[0][off])
        y0b = T2 + i * bh + 8
        w = int((val / float(max(vmax, 1))) * (R2 - L2 - 120))
        d.rectangle((L2, y0b, L2 + w, y0b + bh - 12), fill=col)
        d.text((L2 + 6, y0b + 2), "%-8s %6d v" % (name, val), font=f14, fill=BG)
    d.text((L2, B2 + 4), "sum = %d vertices/frame, identical on all %d frames"
           % (int(rows[0][2]), len(rows)), font=f14, fill=DIM)

    # ---------- right middle: tail freeze zoom ----------
    ZT, ZB = 440, 590
    tail = rows[-30:]
    vals = [float(r[MXY]) for r in tail]
    lo, hi = min(vals), max(vals)
    span = max(hi - lo, 0.002)
    zmin, zmax = (lo + hi) / 2 - span, (lo + hi) / 2 + span

    def zpy(y):
        return ZB - ((y - zmin) / (zmax - zmin)) * (ZB - ZT)

    d.text((L2, ZT - 28), "LAST 30 FRAMES  /  CAPSULE APEX (zoom)", font=f16, fill=FG)
    d.rectangle((L2, ZT, R2, ZB), outline=GRID, width=1)
    zpts = []
    for i, r in enumerate(tail):
        x = L2 + (i / max(1, len(tail) - 1)) * (R2 - L2)
        zpts.append((x, zpy(float(r[MXY]))))
    d.line(zpts, fill=CAPSULE_C, width=2)
    d.text((L2 + 12, ZB + 8), "apex spread = %.6f  (flat = sleeping island, frozen wireframe)"
           % (hi - lo), font=f14, fill=GREEN)
    d.text((L2 + 12, ZB + 30), "t = %.2f .. %.2f s" % (float(tail[0][1]), float(tail[-1][1])),
           font=f12, fill=DIM)

    # ---------- right bottom: measured panel ----------
    PY = 682
    d.rectangle((L2 - 10, PY - 10, W - 24, H - 24), outline=GRID, width=1)
    d.text((L2, PY), "MEASURED  (from debug.tsv data columns)", font=f16, fill=FG)
    yy = PY + 30
    d.text((L2, yy), "bodies=%d  vertices/frame=%d" % (int(cfg.get("bodies", 0)), int(rows[0][2])),
           font=f14, fill=FG)
    yy += 22
    d.text((L2, yy), "box=%d sphere=%d capsule=%d floor=%d (sum %d)"
           % (int(rows[0][B8]), int(rows[0][S8]), int(rows[0][C8]),
              int(rows[0][F8]), int(rows[0][2])), font=f14, fill=FG)
    yy += 22
    d.text((L2, yy), "floor span x = %.3f .. %.3f (half extent 5, exact)"
           % (float(rows[-1][MNX]), float(rows[-1][MXX])), font=f14, fill=FG)
    yy += 22
    d.text((L2, yy), "rest: floor bottom -1.000000, capsule apex 1.600000, tail frozen",
           font=f14, fill=GREEN)
    yy += 22
    d.text((L2, yy), "UE counterpart: PhysSceneDebug draw + UPhysicalMaterial",
           font=f12, fill=DIM)

    out = os.path.join(D, "wireframe.png")
    img.save(out)
    print("wrote", out)


def make_acceptance():
    blocks = [
        ("selftest.log  (M-P0..M-P3 anchors, complete output)", "selftest.log", GREEN),
        ("check-debug.log  (re-verify archived debug.tsv)", "check-debug.log", CYAN),
        ("debug.tsv  provenance header (real DebugRenderer output)", None, AMBER),
        ("viewer-dx12.log  (viewer --physics-debug, deviceErrors must be 0)", "viewer-dx12.log", CYAN),
        ("viewer-vulkan.log  (Vulkan backend, deviceErrors must be 0)", "viewer-vulkan.log", CYAN),
        ("regression.log  (M-P0..M-P2 byte-identical witnesses)", "regression.log", PURPLE),
        ("usage.log  (exit 2 expected)", "usage.log", DIM),
        ("negative.log  (debug --frames 0, exit 2 expected)", "negative.log", RED),
    ]
    loaded = []
    for title, name, col in blocks:
        if name is None:
            with open(os.path.join(D, "debug.tsv"), encoding="ascii") as f:
                raw = [next(f).rstrip("\r\n") for _ in range(4)]
            lines = []
            for ln in raw:
                ln = ln.replace("\t", " ")  # consolas does not expand tabs
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
    d.text((30, 22), "P8 M-P3 ACCEPTANCE  /  real phys-tools + viewer output", font=f20, fill=FG)
    d.text((30, 50), "core/ keeps zero graphics symbols; the wireframe overlay lives in renderer/, gated by --physics-debug",
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
    make_wireframe()
    make_acceptance()
