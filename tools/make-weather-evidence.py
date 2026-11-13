#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""WEA M-WEA0 evidence renderer: acceptance.png from REAL tool outputs.

Reads example/weather-w0/weather-*.tsv + *.log (captured by run.bat from
the real nanite-tools runs) and renders curve strips derived from the
TSV rows (real data, not hand-drawn) plus a log anchor board.
ASCII-only-safe: logs and TSV are pure ASCII.
"""
import csv
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "example", "weather-w0")

BG = (18, 20, 24)
PANEL = (26, 30, 36)
FG = (222, 226, 232)
CYAN = (110, 200, 235)
GREEN = (120, 220, 130)
RED = (235, 110, 110)
GREY = (130, 136, 146)
AMBER = (230, 180, 90)
ORANGE = (240, 150, 80)

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
        return list(csv.DictReader(f, delimiter="\t"))


def curve(draw, box, rows, cols, colors, ymin=0.0, ymax=1.0, label=""):
    """Draw normalized curve strips of `cols` from real TSV rows into box."""
    x0, y0, x1, y1 = box
    draw.rectangle(box, outline=(60, 66, 76))
    if label:
        draw.text((x0 + 6, y0 + 2), label, font=font(12, True), fill=GREY)
    cy0, cy1 = y0 + 20, y1 - 6
    if not rows:
        draw.text((x0 + 8, (y0 + y1) // 2), "<missing TSV>", font=font(12), fill=RED)
        return
    ts = [float(r["t"]) for r in rows]
    tmax = max(ts) or 1.0
    span = ymax - ymin or 1.0
    # gridlines at 0 / 0.5 / 1 (normalized)
    for g in (0.0, 0.5, 1.0):
        gy = cy1 - (g - ymin) / span * (cy1 - cy0)
        draw.line([(x0 + 1, gy), (x1 - 1, gy)], fill=(48, 54, 62))
    for ci, col in enumerate(cols):
        pts = []
        for r in rows:
            v = float(r[col])
            px = x0 + 2 + (float(r["t"]) / tmax) * (x1 - x0 - 4)
            py = cy1 - (v - ymin) / span * (cy1 - cy0)
            pts.append((px, py))
        draw.line(pts, fill=colors[ci], width=2)
    if len(cols) > 1:
        lx = x1 - 160
        for ci, col in enumerate(cols):
            draw.line([(lx, y0 + 8 + ci * 14), (lx + 18, y0 + 8 + ci * 14)],
                      fill=colors[ci], width=2)
            draw.text((lx + 24, y0 + 2 + ci * 14), col, font=font(11), fill=GREY)


def main():
    auto = read_tsv("weather-auto.tsv")
    rain = read_tsv("weather-rain.tsv")

    W, H = 1180, 900
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    d.text((24, 16), "WEA M-WEA0 acceptance - weather state layer (nl/core/weather + nanite-tools weather)",
           font=font(20, True), fill=FG)
    d.text((24, 44), "all curves below are derived from REAL nanite-tools weather TSV dumps (run.bat captured)",
           font=font(13), fill=GREY)

    # --- strip 1: auto timeline, sun + clouds + wetness ---
    curve(d, (24, 70, W - 24, 230), auto,
          ["sunIntensity", "cloudCoverage", "groundWetness"],
          [AMBER, CYAN, GREEN], 0.0, 1.0,
          "auto timeline seed=42 / 1800s / step=2s (day cycle + weather epochs): sunIntensity / cloudCoverage / groundWetness")
    # --- strip 2: auto timeline, rain + transmittance ---
    curve(d, (24, 240, W - 24, 380), auto,
          ["rainIntensity", "cloudTransmittance"],
          [CYAN, (150, 160, 235)], 0.0, 1.0,
          "auto timeline: rainIntensity (0 = dry states) / cloudTransmittance (direct-light transmission)")
    # --- strip 3: auto timeline, sun elevation (day arc) ---
    curve(d, (24, 390, W - 24, 510), auto,
          ["sunElevation"],
          [ORANGE], -1.7, 1.7,
          "auto timeline: sunElevation (rad, +/-pi/2 = midnight/noon, day length 600s)")
    # --- strip 4: preset rain, wetness saturation ---
    curve(d, (24, 520, W - 24, 640), rain,
          ["groundWetness", "rainIntensity"],
          [GREEN, CYAN], 0.0, 1.0,
          "preset rain seed=7 / 300s: wetness saturates under locked rain (cvar/CLI hand-tuning semantics)")

    # --- log anchors ---
    d.text((24, 660), "log anchors (real runs captured by run.bat)", font=font(14, True), fill=FG)
    logs = [
        ("selftest.log", "weather selftest: PASS"),
        ("verify-42.log", "weather verify: PASS"),
        ("twin-auto.log", "twin verify: PASS"),
        ("twin-rain.log", "twin verify: PASS"),
    ]
    y = 688
    for name, anchor in logs:
        lines = read_log(name)
        hit = any(anchor in ln for ln in lines)
        tail = next((ln for ln in reversed(lines) if anchor in ln), lines[-1] if lines else "")
        color = GREEN if hit else RED
        d.text((24, y), "[OK] " if hit else "[XX] ", font=font(13, True), fill=color)
        d.text((70, y), "%s: %s" % (name, tail[:110]), font=font(13), fill=FG)
        y += 24
    d.text((24, y + 8), "determinism contract: same seed + same timeline = byte-identical rows (C++ replay + python twin)",
           font=font(13), fill=GREY)

    out = os.path.join(OUT, "acceptance.png")
    img.save(out)
    print("saved:", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
