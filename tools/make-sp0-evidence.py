# -*- coding: utf-8 -*-
"""M-SP0 evidence renderer: REAL sprite-tools logs / TSV -> PNG.

Iron rule (ROADMAP tool-card convention): every pixel comes from the tool's
real output. No hand-drawn diagrams, no screenshots from other versions.
Output goes to example/sprite2d/. Deterministic: fixed canvas + palette.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

OUT = (sys.argv[1] if len(sys.argv) > 1
       else os.path.join(os.path.dirname(__file__), "..", "example", "sprite2d"))

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    try:
        return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)
    except OSError:
        return ImageFont.load_default()


def read(name):
    path = os.path.join(OUT, name)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().rstrip().splitlines()


def tone(line):
    if "PASS" in line:
        return GREEN
    if "FAIL" in line or "[ERROR]" in line:
        return RED
    if "usage" in line or "exit:" in line:
        return AMBER
    if line.startswith("#") or line.startswith("  "):
        return DIM
    return FG


def render(name, title, lines, width=1240):
    line_h = 25
    pad = 26
    img = Image.new("RGB", (width, pad * 2 + line_h * (len(lines) + 2)), BG)
    d = ImageDraw.Draw(img)
    d.text((pad, pad), title, font=font(21, True), fill=CYAN)
    y = pad + line_h * 2
    for text in lines:
        d.text((pad, y), text[:150], font=font(15), fill=tone(text))
        y += line_h
    path = os.path.join(OUT, name)
    img.save(path)
    print("wrote", path)


def main():
    # 1. usage: real stdout+stderr of `sprite-tools` (exit 2)
    render("usage.png", "sprite-tools / usage (real output, exit 2)", read("usage.log"))

    # 2. selftest: real selftest + three oracle check logs
    lines = []
    for name in ("selftest.log", "atlas-check.log", "flipbook-check.log", "batch-check.log"):
        lines.append("-- " + name + " --")
        lines.extend(read(name))
    render("selftest.png", "sprite-tools / selftest + TSV oracle checks (real output)", lines)

    # 3. batch: real batch-trace.tsv (S = sprite, B = texture bucket)
    lines = ["# source: batch-trace.tsv (real tool output, 17 lines)"]
    for line in read("batch-trace.tsv"):
        parts = line.split("\t")
        if parts and parts[0] == "S":
            color_line = "S  pos=%-3s layer=%-3s textureId=%-12s sequence=%s" % (
                parts[1], parts[4], parts[5], parts[6])
            lines.append(color_line)
        elif parts and parts[0] == "B":
            color_line = "B  begin=%-3s count=%-3s layer=%-3s textureId=%s" % (
                parts[2], parts[3], parts[4], parts[5])
            lines.append(color_line)
        else:
            lines.append(line)
    render("batch.png", "sprite-tools / batch-trace.tsv (stable sort + texture buckets)", lines)

    # 4. negative: real usage of bad args + real missing-file assertion
    lines = ["-- negative.log (atlas-trace --out, exit 2) --"]
    lines.extend(read("negative.log"))
    lines.append("-- negative-check.log (atlas-check missing tsv, exit 1) --")
    lines.extend(read("negative-check.log"))
    render("negative.png", "sprite-tools / negative paths (real output)", lines)

    for name in ("usage.png", "selftest.png", "batch.png", "negative.png"):
        if not os.path.isfile(os.path.join(OUT, name)):
            print("ERROR: missing evidence", name)
            return 1
    print("M-SP0 evidence: 4 PNG OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
