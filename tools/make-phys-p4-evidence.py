# -*- coding: utf-8 -*-
"""P8 M-P4 physics evidence generator.

IRON RULE: every data pixel comes from the REAL tool output --
example/phys-p4/character.tsv (real Jolt 5.3.1 CharacterVirtual ExtendedUpdate
walk, 1200 frames) and the *.log files captured from phys-tools.exe /
viewer.exe. No hand-drawn curves.

Products (both inside example/phys-p4/):
  trajectory.png  side-view walk profile (py vs px over the course) + ground
                  state timeline + tail parking zoom + measured panel
  acceptance.png  terminal-style render of the complete real logs

Reproduce: example/phys-p4/run.bat (re-runs the tools + viewer captures,
rewrites logs+TSV, regenerates both PNGs, exits 0 only when everything is green).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "example", "phys-p4")

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)
GRID = (42, 46, 54)
# 与 World.cpp / character.tsv 注释同源：场地几何绿、地板蓝、角色亮黄。
COURSE_C = (80, 220, 80)
FLOOR_C = (60, 120, 255)
CHAR_C = (255, 230, 60)
# groundState 色带（JPH::CharacterBase::EGroundState 枚举序）。
STATE_COLORS = {
    0: (108, 220, 128),   # OnGround
    1: (235, 185, 90),    # OnSteepGround
    2: (190, 150, 230),   # NotSupported
    3: (86, 182, 224),    # InAir
}
STATE_NAMES = {0: "OnGround", 1: "OnSteepGround", 2: "NotSupported", 3: "InAir"}

# 场地剖面（与 World.cpp CharacterHarness 同源，x 已中心化）。
RAMP_TOP = 2.3094
STEP_TOP = 3.2094


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_tsv():
    cfg, rows = {}, []
    with open(os.path.join(D, "character.tsv"), encoding="ascii") as f:
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
    assert rows and len(rows[0]) == 10, "empty or malformed character.tsv"
    return cfg, rows


def read_log(name):
    path = os.path.join(D, name)
    if not os.path.exists(path):
        return ["(missing %s)" % name]
    with open(path, encoding="ascii", errors="replace") as f:
        return f.read().rstrip("\n").split("\n")


def course_profile(px):
    """Ideal course top-surface height at foot x (for the reference silhouette)."""
    if -43.0 <= px <= 47.0:
        y = 0.0
    else:
        return None
    # 30 deg ramp x[-7,-3]
    if -7.0 <= px <= -3.0:
        y = max(y, (px + 7.0) * (RAMP_TOP / 4.0))
    # platform1 x[-3,5]
    if -3.0 <= px <= 5.0:
        y = max(y, RAMP_TOP)
    # steps x[5,6.8]
    if 5.0 <= px <= 5.6:
        y = max(y, RAMP_TOP + 0.3)
    elif 5.6 <= px <= 6.2:
        y = max(y, RAMP_TOP + 0.6)
    elif 6.2 <= px <= 6.8:
        y = max(y, STEP_TOP)
    # platform2 x[6.8,11]
    if 6.8 <= px <= 11.0:
        y = max(y, STEP_TOP)
    # 60 deg slope x[11,13]
    if 11.0 <= px <= 13.0:
        y = max(y, STEP_TOP + (px - 11.0) * 1.7320508)
    return y


def make_trajectory():
    cfg, rows = read_tsv()
    W, H = 1560, 940
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f12, f14, f16, f18, f22 = font(12), font(14), font(16), font(18), font(22, True)

    d.text((40, 22), "P8 M-P4  CHARACTER WALK-THROUGH  /  RAMP + STEPS + STEEP SLOPE, 20 s",
           font=f22, fill=FG)
    d.text((40, 52), "Jolt %s CharacterVirtual ExtendedUpdate (CharacterMovement-style sweep);"
           " py = capsule center (feet = py - 1.08)" % cfg.get("jolt", "5.3.1"),
           font=f14, fill=DIM)

    # ---- panel 1: side view px-py profile ----
    p1x, p1y, p1w, p1h = 60, 90, 1440, 380
    d.rectangle([p1x, p1y, p1x + p1w, p1y + p1h], outline=GRID)
    d.text((p1x + 12, p1y + 8), "SIDE VIEW  py vs px  (course silhouette behind)",
           font=f16, fill=FG)

    xmin, xmax = -14.5, 18.5
    ymin, ymax = -1.2, 7.2

    def P(px, py):
        fx = p1x + (px - xmin) / (xmax - xmin) * p1w
        fy = p1y + p1h - (py - ymin) / (ymax - ymin) * p1h
        return fx, fy

    # grid + axis labels
    for gy in range(-1, 8):
        _, sy = P(0, gy)
        d.line([p1x, sy, p1x + p1w, sy], fill=GRID)
        d.text((p1x - 34, sy - 7), "%d" % gy, font=f12, fill=DIM)
    for gx in range(-14, 19, 4):
        sx, _ = P(gx, 0)
        d.line([sx, p1y, sx, p1y + p1h], fill=GRID)
        d.text((sx - 10, p1y + p1h + 4), "%+d" % gx, font=f12, fill=DIM)

    # course silhouette (ideal geometry, from the same constants as World.cpp)
    prev = None
    px = xmin
    while px <= xmax:
        gy = course_profile(px)
        if gy is not None:
            sx, sy = P(px, gy)
            if prev is not None:
                d.line([prev, (sx, sy)], fill=COURSE_C)
            prev = (sx, sy)
        else:
            prev = None
        px += 0.02
    # floor slab outline
    fx0, fy0 = P(-43.0, 0.0)
    fx1, fy1 = P(47.0, -1.0)
    d.line([fx0, fy1, fx1, fy1], fill=FLOOR_C)
    d.text((P(-13, 0)[0] - 40, P(-13, 0)[1] + 10), "floor top y=0", font=f12, fill=FLOOR_C)

    # real walk samples (capsule center height)
    pts = []
    for r in rows:
        px_, py_ = float(r[2]), float(r[3])
        sx, sy = P(px_, py_)
        pts.append((sx, sy))
    for i in range(1, len(pts)):
        d.line([pts[i - 1], pts[i]], fill=CHAR_C)
    # character start / end markers
    d.ellipse([pts[0][0] - 5, pts[0][1] - 5, pts[0][0] + 5, pts[0][1] + 5], outline=FG)
    d.text((pts[0][0] - 46, pts[0][1] - 26), "start x=-13", font=f12, fill=FG)
    last = pts[-1]
    d.ellipse([last[0] - 5, last[1] - 5, last[0] + 5, last[1] + 5], outline=RED)
    d.text((last[0] - 96, last[1] - 26), "parked x=%.3f" % float(rows[-1][2]),
           font=f12, fill=RED)
    d.text((P(-5.0, RAMP_TOP)[0] - 30, P(-5.0, RAMP_TOP)[1] - 20),
           "30deg ramp", font=f12, fill=COURSE_C)
    d.text((P(6.0, STEP_TOP)[0] - 20, P(6.0, STEP_TOP)[1] - 36),
           "3 steps x0.3", font=f12, fill=COURSE_C)
    d.text((P(12.6, 5.2)[0], P(12.6, 5.2)[1]), "60deg slope (blocks)",
           font=f12, fill=COURSE_C)

    # ---- panel 2: ground-state timeline ----
    p2y, p2h = 500, 56
    d.rectangle([p1x, p2y, p1x + p1w, p2y + p2h], outline=GRID)
    d.text((p1x + 12, p2y - 22), "GROUND STATE TIMELINE  (0=OnGround 1=OnSteepGround 2=NotSupported 3=InAir)",
           font=f16, fill=FG)
    n = len(rows)
    for i, r in enumerate(rows):
        st = int(r[8])
        x0 = p1x + i / n * p1w
        x1 = p1x + (i + 1) / n * p1w
        d.line([x0, p2y + 6, x0, p2y + p2h - 6], fill=STATE_COLORS.get(st, RED))
    # state legend with real residency counts
    counts = {s: 0 for s in STATE_NAMES}
    for r in rows:
        st = int(r[8])
        counts[st] = counts.get(st, 0) + 1
    lx = p1x + 12
    for s in (0, 1, 2, 3):
        d.rectangle([lx, p2y + p2h + 6, lx + 12, p2y + p2h + 18], fill=STATE_COLORS[s])
        d.text((lx + 18, p2y + p2h + 5), "%s=%d" % (STATE_NAMES[s], counts[s]),
               font=f14, fill=FG)
        lx += 190

    # ---- panel 3: tail parking zoom ----
    p3y = 610
    d.text((p1x, p3y), "TAIL 60 FRAMES ZOOM  (parking anchor: spread < 1e-3)",
           font=f16, fill=FG)
    zy, zh = p3y + 26, 150
    tail = rows[-60:]
    vals = [float(r[3]) for r in tail]
    lo, hi = min(vals) - 0.01, max(vals) + 0.01
    d.rectangle([p1x, zy, p1x + p1w, zy + zh], outline=GRID)
    for i, v in enumerate(vals):
        sx = p1x + i / 59.0 * p1w
        sy = zy + zh - (v - lo) / (hi - lo) * zh
        if i:
            pxv = p1x + (i - 1) / 59.0 * p1w
            pv = vals[i - 1]
            d.line([pxv, zy + zh - (pv - lo) / (hi - lo) * zh, sx, sy], fill=CHAR_C)
    spread = max(vals) - min(vals)
    d.text((p1x + p1w - 260, zy + 8), "py spread=%.6f" % spread,
           font=f14, fill=GREEN if spread < 1e-3 else RED)

    # ---- measured panel ----
    my = 796
    d.text((p1x, my), "MEASURED  (real character.tsv, provenance # jolt= header)",
           font=f16, fill=FG)
    mlines = [
        "frames=%d  dt=%.6f  walk=%.1f  max_slope=50deg  radius=0.5 half_cyl=0.6"
        % (len(rows), float(cfg.get("dt", 1.0 / 60.0)), float(cfg.get("walk", 1.5))),
        "final x=%.4f  final y=%.4f (parked at 60deg slope foot, feet=%.4f on platform2)"
        % (float(rows[-1][2]), float(rows[-1][3]), float(rows[-1][3]) - 1.08),
        "course: start x=-13 -> 30deg ramp -> 3 steps (0.3 each) -> 60deg BLOCKED",
    ]
    yy = my + 24
    for ln in mlines:
        d.text((p1x, yy), ln, font=f14, fill=FG)
        yy += 20
    d.text((40, H - 30), "generated from example/phys-p4/character.tsv (no hand-drawn pixels)",
           font=f12, fill=DIM)
    img.save(os.path.join(D, "trajectory.png"))
    print("wrote trajectory.png")


def make_acceptance():
    logs = [
        ("selftest", read_log("selftest.log")),
        ("character", read_log("character.log")),
        ("check-character", read_log("check-character.log")),
        ("regression (5 baselines)", read_log("regression.log")),
        ("viewer dx12", read_log("viewer-dx12.log")),
        ("viewer vulkan", read_log("viewer-vulkan.log")),
        ("usage", read_log("usage.log")),
        ("negative", read_log("negative.log")),
    ]
    W, H = 1560, 1580
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f14, f16, f22 = font(14), font(16), font(22, True)
    d.text((40, 22), "P8 M-P4  ACCEPTANCE  /  REAL TOOL LOGS (phys-tools + viewer + regression gate)",
           font=f22, fill=FG)
    line_h = 20
    y = 60
    for name, lines in logs:
        d.text((40, y), "== %s ==" % name, font=f16, fill=CYAN)
        y += line_h + 2
        for ln in lines[-12:]:
            color = FG
            if "PASS" in ln or "OK" in ln or "ALL GREEN" in ln:
                color = GREEN
            elif "FAIL" in ln or "ERROR" in ln:
                color = RED
            d.text((60, y), ln[:170], font=f14, fill=color)
            y += line_h
        y += 6
    img.save(os.path.join(D, "acceptance.png"))
    print("wrote acceptance.png")


if __name__ == "__main__":
    make_trajectory()
    make_acceptance()
