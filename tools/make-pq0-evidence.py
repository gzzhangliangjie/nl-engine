# -*- coding: utf-8 -*-
"""M-PQ0 evidence renderer: REAL phys-tools logs / query TSV -> PNG.

Iron rule (ROADMAP tool-card convention): every pixel comes from the tool's
real output. No hand-drawn diagrams, no screenshots from other versions.
Output goes to example/phys-query/. Deterministic: fixed canvas + palette.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

OUT = (sys.argv[1] if len(sys.argv) > 1
       else os.path.join(os.path.dirname(__file__), "..", "example", "phys-query"))

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


def render(name, title, lines, width=1400):
    line_h = 25
    pad = 26
    img = Image.new("RGB", (width, pad * 2 + line_h * (len(lines) + 2)), BG)
    d = ImageDraw.Draw(img)
    d.text((pad, pad), title, font=font(21, True), fill=CYAN)
    y = pad + line_h * 2
    for text in lines:
        d.text((pad, y), text[:170], font=font(15), fill=tone(text))
        y += line_h
    path = os.path.join(OUT, name)
    img.save(path)
    print("wrote", path)


def main():
    # 1. usage: real stdout of `phys-tools` (exit 2)
    render("usage.png", "phys-tools / usage (real output, exit 2)", read("usage.log"), 1240)

    # 2. query-trace: the real 10-row TSV (six numeric anchors)
    lines = ["# source: query-trace.tsv (real Jolt 5.3.1 NarrowPhaseQuery output)"]
    for line in read("query-trace.tsv"):
        p = line.split("\t")
        if len(p) == 13 and p[0] != "name":
            lines.append("%-18s hit=%-2s frac=%-10s dist=%-10s p=(%s,%s,%s) n=(%s,%s,%s) body=%-9s layer=%s" % (
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]))
        else:
            lines.append(line)
    render("query-trace.png", "phys-tools / query-trace.tsv (raycast-sweep-overlap six anchors)", lines)

    # 3. query-check: real oracle verdict
    lines = ["-- query-trace.log --"] + read("query-trace.log") + ["-- query-check.log --"] + read("query-check.log")
    render("query-check.png", "phys-tools / query-check (real oracle verdict)", lines, 1240)

    # 4. negative: real usage of bad args + real missing-file assertion
    lines = ["-- negative.log (query-trace --out, exit 2) --"] + read("negative.log")
    lines.append("-- negative-check.log (query-check missing tsv, exit 1) --")
    lines.extend(read("negative-check.log"))
    render("negative.png", "phys-tools / negative paths (real output)", lines, 1240)

    for name in ("usage.png", "query-trace.png", "query-check.png", "negative.png"):
        if not os.path.isfile(os.path.join(OUT, name)):
            print("ERROR: missing evidence", name)
            return 1
    print("M-PQ0 evidence: 4 PNG OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
