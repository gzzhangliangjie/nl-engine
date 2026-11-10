#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P8 M-GL0 evidence renderer: acceptance.png from REAL tool logs.

Reads example/gl0/*.log + *.tsv (captured by run.bat from the real
gl-tools runs) and renders a terminal-style evidence board plus a
logic-tick cadence strip derived from the trace TSV (real data, not
hand-drawn). ASCII-only-safe: logs are pure ASCII.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "example", "gl0")

BG = (18, 20, 24)
FG = (222, 226, 232)
CYAN = (110, 200, 235)
GREEN = (120, 220, 130)
RED = (235, 110, 110)
GREY = (130, 136, 146)
AMBER = (230, 180, 90)

_cache = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in _cache:
        name = "segoeuib.ttf" if bold else "segoeui.ttf"
        try:
            _cache[key] = ImageFont.truetype("C:/Windows/Fonts/" + name, size)
        except OSError:
            _cache[key] = ImageFont.load_default()
    return _cache[key]


def read_log(name):
    path = os.path.join(OUT, name)
    if not os.path.exists(path):
        return ["<missing log: %s>" % name]
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return [ln.rstrip("\n\r") for ln in f]


def read_tsv(name):
    path = os.path.join(OUT, name)
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        rows = [ln.rstrip("\n\r").split("\t") for ln in f if ln.strip()]
    return rows[1:]  # drop header


def ok_line(ln):
    return ("PASS" in ln or ": OK" in ln)


def make_acceptance():
    logs = [
        ("selftest (GameClock anchors)", read_log("selftest.log")),
        ("trace --frames 240 --dt 16.7", read_log("trace-const.log")),
        ("trace burst/pause/scale mix", read_log("trace-burst.log")),
        ("verify.py (independent python twin)", read_log("verify-const.log")),
        ("verify.py burst/pause/scale mix", read_log("verify-burst.log")),
        ("usage (expect exit 2)", read_log("usage.log")),
    ]
    W, H = 1560, 800
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f14, f16, f22 = font(14), font(16), font(22, True)
    d.text((40, 22), "P8 M-GL0  ACCEPTANCE  /  REAL TOOL LOGS (GameClock dual-frame model)",
           font=f22, fill=FG)
    line_h = 20
    y = 60
    half = W // 2 - 40
    # left column: first three logs; right column: last three
    for col, group in ((0, logs[:3]), (1, logs[3:])):
        y = 60
        x = 40 + col * (half + 40)
        for name, lines in group:
            d.text((x, y), "== %s ==" % name, font=f16, fill=CYAN)
            y += line_h + 2
            for ln in lines[-16:]:
                color = FG
                if ok_line(ln):
                    color = GREEN
                elif "FAIL" in ln:
                    color = RED
                d.text((x + 16, y), ln, font=f14, fill=color)
                y += line_h
            y += 8

    # cadence strip: ticks per frame from trace-const.tsv (real data)
    rows = read_tsv("trace-const.tsv")
    strip_y = H - 190
    d.text((40, strip_y - 26), "== logic-tick cadence (trace-const.tsv, first 140 frames, "
                                "16.7ms real -> 60Hz logic) ==", font=f16, fill=CYAN)
    x0, y0, bw = 40, strip_y + 4, 9
    for i, row in enumerate(rows[:140]):
        try:
            ticks = int(row[2])
        except (ValueError, IndexError):
            ticks = -1
        color = GREY
        h = 4
        if ticks == 1:
            color = GREEN
            h = 22
        elif ticks == 2:
            color = AMBER
            h = 34
        elif ticks > 2:
            color = RED
            h = 46
        elif ticks == 0:
            color = (70, 76, 86)
            h = 4
        d.rectangle([x0 + i * (bw + 1), y0 + 50 - h, x0 + i * (bw + 1) + bw, y0 + 50], fill=color)
    d.text((40, y0 + 60), "grey=0 tick  green=1 tick  amber=2 ticks  "
                          "(burst frames beyond 140 omitted here)", font=f14, fill=GREY)

    d.text((40, H - 34), "generated from real logs by tools/make-gl-evidence.py",
           font=f14, fill=GREY)
    img.save(os.path.join(OUT, "acceptance.png"))
    print("wrote", os.path.join(OUT, "acceptance.png"))


if __name__ == "__main__":
    sys.exit(make_acceptance())
