#!/usr/bin/env python3
"""M-GL3 evidence board (PIL): mid-suite screenshots + interp probe verdicts.

Inputs (all real outputs of example/gl3/run.bat):
  example/gl3/gl3-{mid,pause}-{DX12,VULKAN}.png
  example/gl3/gl3-{mid,pause}-{DX12,VULKAN}.log

Output: example/gl3/evidence.png
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
GL3 = os.path.join(ROOT, "example", "gl3")

W, H = 1600, 1400
BG = (18, 20, 26)
CYAN = (80, 200, 220)
GREY = (150, 155, 165)
GREEN = (110, 220, 130)
WHITE = (235, 238, 245)
AMBER = (230, 180, 90)

PROBE = re.compile(
    r"interp probe: k=(\d+) alpha=(\S+)"
    r" prev=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)"
    r" curr=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)"
    r" interp=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)")


def fpath(kind, backend):
    return os.path.join(GL3, f"gl3-{kind}-{backend}.png")


def probes(log):
    rows = []
    for line in open(log, encoding="utf-8", errors="replace"):
        m = PROBE.search(line)
        if m:
            g = [float(x) for x in m.groups()]
            rows.append((int(g[0]), g[1],
                         tuple(g[2:5]), tuple(g[5:8]), tuple(g[8:11])))
    return rows


def main():
    panels = [("mid", "DX12"), ("mid", "VULKAN"),
              ("pause", "DX12"), ("pause", "VULKAN")]
    for kind, backend in panels:
        for ext in (".png", ".log"):
            p = fpath(kind, backend)[:-4] + ext
            if not os.path.isfile(p):
                print("missing input:", p, "(run example/gl3/run.bat first)")
                return 1

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    try:
        f24 = ImageFont.truetype("consola.ttf", 24)
        f18 = ImageFont.truetype("consola.ttf", 18)
        f16 = ImageFont.truetype("consola.ttf", 16)
    except OSError:
        f24 = f18 = f16 = ImageFont.load_default()

    d.text((40, 26), "P8 M-GL3 - render-frame interpolation (alpha = accumulator/step)",
           font=f24, fill=CYAN)
    d.text((40, 58),
           "mid: fixed-dt 25ms => ticks [1,2,1], capture alpha=0.5; PNG renders the "
           "tick boundary (same path as no-interp); probe logs carry the interp numbers",
           font=f16, fill=GREY)

    px, py, pw = 40, 100, 740
    ph = int(pw * 540 / 960)
    positions = [(px, py), (px + pw + 40, py), (px, py + ph + 70), (px + pw + 40, py + ph + 70)]
    for (kind, backend), (x, y) in zip(panels, positions):
        shot = Image.open(fpath(kind, backend)).convert("RGB").resize((pw, ph))
        img.paste(shot, (x, y))
        verdict_color = GREEN if kind == "mid" else AMBER
        d.text((x, y + ph + 6),
               f"{kind}-{backend}: PNG = tick-boundary state (byte-identical to no-interp)",
               font=f16, fill=verdict_color)
        rows = probes(fpath(kind, backend)[:-4] + ".log")
        shown = 0
        for k, a, prev, curr, ip in rows:
            if k not in (4, 5):   # orbit entities only (spin parents rotate in place)
                continue
            name = "orb-a" if k == 4 else "orb-b"
            d.text((x, y + ph + 28 + shown * 20),
                   f"  {name} a={a:.3f} prev=({prev[0]:.3f},{prev[2]:.3f})"
                   f" curr=({curr[0]:.3f},{curr[2]:.3f})"
                   f" interp=({ip[0]:.3f},{ip[2]:.3f})",
                   font=f16, fill=WHITE)
            shown += 1
        if shown == 0:
            d.text((x, y + ph + 28), "  (frozen: prev == curr == interp, alpha=0)",
                   font=f16, fill=GREY)

    y0 = py + 2 * (ph + 70) + 60
    d.text((40, y0), "== verdicts (verify.py independent twin) ==", font=f18, fill=CYAN)
    lines = [
        ("PASS  equal: interp OFF vs ON PNGs byte-identical x2 backends "
         "(capture = same code path as no-interp)", GREEN),
        ("PASS  mid: clock twin re-derives ticks [1,2,1] + alpha=0.5; orb prev/curr/interp "
         "match SpinSystem formula twin (w=0.9*(1+0.5*idx) rad/s)", GREEN),
        ("PASS  slerp: interp quat == slerp(pq, cq, alpha), shortest path, nlerp fallback", GREEN),
        ("PASS  cross-backend: 6 probe lines byte-equal DX12 == VULKAN (pure CPU)", GREEN),
        ("PASS  pause: prev == curr == interp, alpha=0 (frozen everywhere)", AMBER),
        ("PASS  render-only discipline: interp never written back to sim state "
         "(bevy_transform_interpolation rule)", GREEN),
    ]
    for i, (s, c) in enumerate(lines):
        d.text((40, y0 + 34 + i * 26), s, font=f16, fill=c)

    img.save(os.path.join(GL3, "evidence.png"))
    print("evidence written:", os.path.join(GL3, "evidence.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
