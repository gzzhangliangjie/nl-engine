#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P8 M-J5c evidence renderer: acceptance.png from REAL tool logs.

Reads example/phys-j5c/*.log (captured by run.bat from the real
phys-tools / regression-gate runs) and renders a terminal-style
evidence board. ASCII-only output-safe: logs are pure ASCII (Chinese
Windows GBK console would break the PIL rendering otherwise).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "example", "phys-j5c")

BG = (18, 20, 24)
FG = (222, 226, 232)
CYAN = (110, 200, 235)
GREEN = (120, 220, 130)
RED = (235, 110, 110)
GREY = (130, 136, 146)

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


def ok_line(ln):
    return ("PASS" in ln or ": OK" in ln or "IDENTICAL" in ln)


def make_acceptance():
    logs = [
        ("selftest (p0..p4 + j5c)", read_log("selftest.log")),
        ("check-jobs (adapter matrix, N=4)", read_log("check-jobs.log")),
        ("regression (5 baselines)", read_log("regression.log")),
        ("usage (expect exit 2)", read_log("usage.log")),
        ("negative --jobs -1 (expect exit 2)", read_log("negative.log")),
    ]
    W, H = 1560, 1180
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f14, f16, f22 = font(14), font(16), font(22, True)
    d.text((40, 22), "P8 M-J5c  ACCEPTANCE  /  REAL TOOL LOGS (PoolJobSystem adapter -> ThreadPool)",
           font=f22, fill=FG)
    line_h = 20
    y = 60
    for name, lines in logs:
        d.text((40, y), "== %s ==" % name, font=f16, fill=CYAN)
        y += line_h + 2
        for ln in lines[-14:]:
            color = FG
            if ok_line(ln):
                color = GREEN
            elif "FAIL" in ln or "MISMATCH" in ln:
                color = RED
            elif ln.startswith("#") or ln.startswith("  md5"):
                color = GREY
            d.text((56, y), ln, font=f14, fill=color)
            y += line_h
        y += 6
    d.text((40, H - 34), "generated from real logs by tools/make-phys-j5c-evidence.py",
           font=f14, fill=GREY)
    img.save(os.path.join(OUT, "acceptance.png"))
    print("wrote", os.path.join(OUT, "acceptance.png"))


if __name__ == "__main__":
    sys.exit(make_acceptance())
