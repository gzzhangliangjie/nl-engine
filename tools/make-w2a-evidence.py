# -*- coding: utf-8 -*-
"""M-W2A evidence renderer: REAL nl-script selftest stdout -> PNG.

Iron rule (ROADMAP tool-card convention): every pixel comes from the tool's
real output. No hand-drawn diagrams, no screenshots from other versions.
Output goes to example/m-w2a-{audio,query,anim}/. Deterministic: fixed
canvas + palette (make-pq0-evidence.py same shape).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(__file__)
DEFAULT_ROOT = os.path.abspath(os.path.join(HERE, "..", "example"))

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


def read(out_dir, name):
    path = os.path.join(out_dir, name)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().rstrip().splitlines()


def tone(line):
    if "PASS" in line:
        return GREEN
    if "FAIL" in line or "[ERROR]" in line or "detail:" in line:
        return RED
    if "usage" in line or "exit" in line:
        return AMBER
    if line.startswith("#") or line.startswith("  "):
        return DIM
    return FG


def render(out_dir, name, title, lines, width=1400):
    line_h = 25
    pad = 26
    img = Image.new("RGB", (width, pad * 2 + line_h * (len(lines) + 2)), BG)
    d = ImageDraw.Draw(img)
    d.text((pad, pad), title, font=font(21, True), fill=CYAN)
    y = pad + line_h * 2
    for text in lines:
        d.text((pad, y), text[:170], font=font(15), fill=tone(text))
        y += line_h
    path = os.path.join(out_dir, name)
    img.save(path)
    print("wrote", path)


def main():
    # 参数：[root] [audio|query|anim] 或仅 [audio|query|anim]（run.bat 用法——
    # 单个裸域名 = 只渲染该域，root 取默认 example/）。
    only = None
    root = DEFAULT_ROOT
    for a in sys.argv[1:]:
        if a in ("audio", "query", "anim"):
            only = a
        else:
            root = a
    domains = [
        ("audio", "s16", "nl-script selftest / s16 Audio (real output)"),
        ("query", "s17", "nl-script selftest / s17 Physics query (real output)"),
        ("anim",  "s18", "nl-script selftest / s18 Animation (real output)"),
    ]
    for domain, section, title in domains:
        if only and domain != only:
            continue
        out_dir = os.path.join(root, "m-w2a-%s" % domain)
        if not os.path.isfile(os.path.join(out_dir, "%s.log" % domain)):
            print("skip", domain, "(log not present yet)")
            continue
        render(out_dir, "usage.png", "nl-script / usage (real output, exit 2)",
               read(out_dir, "usage.log"), 1240)
        lines = ["# source: %s.log (real nl-script selftest stdout)" % domain]
        for line in read(out_dir, "%s.log" % domain):
            # keep section anchor lines: assertion results ("ok  s16a ..." /
            # FAIL variants), pre/detail lines, and the final verdict
            stripped = line.lstrip()
            hit = (stripped.startswith(("s16", "s17", "s18"))
                   or line.startswith("  s16") or line.startswith("  s17")
                   or line.startswith("  s18")
                   or (stripped.startswith(("ok", "FAIL"))
                       and any(s in line for s in ("s16", "s17", "s18")))
                   or line.startswith("nl-script selftest:")
                   or "PASS" in line or "FAIL" in line)
            if hit:
                lines.append(line)
        render(out_dir, "%s.png" % domain, title, lines)


if __name__ == "__main__":
    main()
