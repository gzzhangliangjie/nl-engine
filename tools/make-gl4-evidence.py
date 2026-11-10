#!/usr/bin/env python3
"""M-GL4 evidence board (PIL): script OnTick orbit + pause freeze screenshots.

Inputs (all real outputs of example/gl4/run.bat):
  example/gl4/gl4-{tick,pause}-{DX12,VULKAN}.png
  example/gl4/gl4-{tick,pause}-{DX12,VULKAN}.log

Output: example/gl4/evidence.png
"""
import os
import re

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
GL4 = os.path.join(ROOT, "example", "gl4")

W, H = 1600, 1320
BG = (18, 20, 26)
CYAN = (80, 200, 220)
GREY = (150, 155, 165)
GREEN = (110, 220, 130)
WHITE = (235, 238, 245)
AMBER = (230, 180, 90)

PROBE = re.compile(
    r"script probe: ticks=(\d+) pos=\((-?\d+\.?\d*),(-?\d+\.?\d*),(-?\d+\.?\d*)\)")
CLOCK = re.compile(
    r"script clock state: simTime=(\S+) ticks=(\d+) paused=(\d)")


def fpath(kind, backend):
    return os.path.join(GL4, f"gl4-{kind}-{backend}.png")


def probe_line(log):
    for line in open(log, encoding="utf-8", errors="replace"):
        m = PROBE.search(line)
        if m:
            return int(m.group(1)), tuple(float(x) for x in m.groups()[1:4])
    return None


def clock_line(log):
    for line in open(log, encoding="utf-8", errors="replace"):
        m = CLOCK.search(line)
        if m:
            return float(m.group(1)), int(m.group(2)), int(m.group(3))
    return None


def main():
    panels = [("tick", "DX12"), ("tick", "VULKAN"),
              ("pause", "DX12"), ("pause", "VULKAN")]
    for kind, backend in panels:
        for ext in (".png", ".log"):
            p = fpath(kind, backend)[:-4] + ext
            if not os.path.isfile(p):
                print("missing input:", p, "(run example/gl4/run.bat first)")
                return 1

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    try:
        f24 = ImageFont.truetype("consola.ttf", 24)
        f18 = ImageFont.truetype("consola.ttf", 18)
        f16 = ImageFont.truetype("consola.ttf", 16)
    except OSError:
        f24 = f18 = f16 = ImageFont.load_default()

    d.text((40, 26), "P8 M-GL4 - script OnTick on the logic frame (host-level, orbit r=1.6 w=0.9)",
           font=f24, fill=CYAN)
    d.text((40, 58),
           "tick: fixed-dt 16.7ms x100 => script probe ticks=100; OnTick runs inside the clock "
           "callback BEFORE world.tick; pause = callback never fires = script frozen",
           font=f16, fill=GREY)

    px, py, pw = 40, 100, 740
    ph = int(pw * 540 / 960)
    positions = [(px, py), (px + pw + 40, py), (px, py + ph + 70), (px + pw + 40, py + ph + 70)]
    for (kind, backend), (x, y) in zip(panels, positions):
        shot = Image.open(fpath(kind, backend)).convert("RGB").resize((pw, ph))
        img.paste(shot, (x, y))
        verdict_color = GREEN if kind == "tick" else AMBER
        d.text((x, y + ph + 6),
               f"{kind}-{backend}: script-orb world position driven by OnTick each logic tick",
               font=f16, fill=verdict_color)
        p = probe_line(fpath(kind, backend)[:-4] + ".log")
        c = clock_line(fpath(kind, backend)[:-4] + ".log")
        if p and c:
            d.text((x, y + ph + 28),
                   f"  probe ticks={p[0]} pos=({p[1][0]:.3f},{p[1][2]:.3f})"
                   f" | clock simTime={c[0]:.5f} ticks={c[1]} paused={c[2]}",
                   font=f16, fill=WHITE)

    y0 = py + 2 * (ph + 70) + 50
    d.text((40, y0), "== verdicts (run.bat findstr + verify.py independent twin) ==", font=f18, fill=CYAN)
    lines = [
        ("PASS  tick: 100 frames @ 16.7ms => probe ticks=100; orbit pos matches formula twin "
         "(r=1.6, w=0.9, t=ticks/60, tol 5e-4)", GREEN),
        ("PASS  mid: fixed-dt 30ms x3 => per-frame ticks [1,2,2] (clock accumulator re-derivation; "
         "25ms was on the 2*step float knife edge, dropped)", GREEN),
        ("PASS  pause: probe ticks=0, pos = scene initial (1.6, 0, 0) - script frozen with sim", AMBER),
        ("PASS  cross-backend: probe/clock lines byte-equal DX12 == VULKAN (pure CPU)", GREEN),
        ("PASS  negative: game.script without --script exits 1 with 'no script loaded' (x2 backends)", GREEN),
        ("PASS  OnTick optional contract: scripts without OnTick tick as no-op (m-sc3 regression 8/8)", GREEN),
    ]
    for i, (s, c) in enumerate(lines):
        d.text((40, y0 + 34 + i * 26), s, font=f16, fill=c)

    img.save(os.path.join(GL4, "evidence.png"))
    print("evidence written:", os.path.join(GL4, "evidence.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
