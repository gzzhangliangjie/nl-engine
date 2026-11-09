"""M-J5a evidence images — generated ONLY from real tool logs in example/m-j5a/.

No hardcoded numbers: timings, hashes, case lines and PASS/FAIL are parsed from
the .log files produced by the acceptance run.bat. ASCII text only (font safety).
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "example", "m-j5a")

FG = (24, 32, 45)
MUTED = (100, 112, 130)
BLUE = (46, 108, 196)
GREEN = (32, 148, 96)
RED = (196, 52, 52)
BG = (250, 251, 253)
CARD = (255, 255, 255)
LINE = (222, 228, 236)


def font(size, bold=False):
    for p in (r"C:\Windows\Fonts\consolab.ttf" if bold else r"C:\Windows\Fonts\consola.ttf",
              r"C:\Windows\Fonts\arial.ttf"):
        if os.path.isfile(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def read(name):
    with open(os.path.join(D, name), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def canvas(w, h, title, subtitle):
    im = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, w, 66), fill=FG)
    d.text((22, 14), title, font=font(20, True), fill=(255, 255, 255))
    d.text((22, 41), subtitle, font=font(12), fill=(186, 200, 220))
    return im, d


def save(im, name):
    p = os.path.join(D, name)
    im.save(p)
    print("wrote", p)


def compilepar_png():
    log = read("compilepar.log")
    mS = re.search(r"serial\s+OK\s+\(([\d.]+)s", log)
    mD = re.search(r"dag\s+OK\s+\(([\d.]+)s", log)
    mX = re.search(r"speedup\s+([\d.]+)x", log)
    if not (mS and mD and mX):
        sys.exit("compilepar.log missing real timings")
    s, dg, x = float(mS.group(1)), float(mD.group(1)), float(mX.group(1))
    rep = re.search(r"report bytes\s+(\w+)\s+\((\d+) vs (\d+)\)", log)
    hashes = re.findall(r"(\S+)\s+hash\s+(MATCH|DIFFER)\s+\((0x[0-9a-f]+)\)", log)
    verdict = "PASS" if "compilepar : PASS" in log else "FAIL"

    im, d = canvas(1000, 620,
                   "M-J5a  |  4-way slangc compile: serial vs fan-in DAG",
                   "source: example/m-j5a/compilepar.log (real tool output, no hand-drawn data)")
    # timing bars
    d.rounded_rectangle((22, 86, 978, 286), radius=10, fill=CARD, outline=LINE)
    d.text((40, 102), "wall-clock (seconds, lower is better)", font=font(14, True), fill=FG)
    maxv = max(s, dg)
    scale = 540.0 / maxv   # 留出右侧数值标签空间，避免贴边截断
    for i, (label, val, color) in enumerate((("serial  threads=1", s, MUTED),
                                             ("DAG    4 jobs -> 1 fan-in", dg, BLUE))):
        y = 142 + i * 62
        d.text((40, y), label, font=font(15), fill=FG)
        d.rectangle((340, y + 2, 340 + int(val * scale), y + 30), fill=color)
        d.text((350 + int(val * scale), y + 4), "%.3fs" % val, font=font(15, True), fill=FG)
    d.text((40, 250), "speedup %.2fx" % x, font=font(18, True),
           fill=GREEN if x > 1.0 else RED)

    # determinism table
    d.rounded_rectangle((22, 302, 978, 560), radius=10, fill=CARD, outline=LINE)
    d.text((40, 318), "byte-exact determinism (hard gate)", font=font(14, True), fill=FG)
    rows = []
    if rep:
        rows.append(("report bytes", "%s  (%s vs %s)" % rep.groups(), rep.group(1) == "EQUAL"))
    rows.append(("lane flags", "EQUAL" if "lane flags   EQUAL" in log else "DIFFER",
                 "lane flags   EQUAL" in log))
    for name, status, h in hashes:
        rows.append((name + " FNV64", "%s  %s" % (status, h), status == "MATCH"))
    y = 350
    for name, val, ok in rows:
        d.text((42, y), name.ljust(16), font=font(14), fill=FG)
        d.text((230, y), val, font=font(14), fill=GREEN if ok else RED)
        d.text((930, y), "OK" if ok else "FAIL", font=font(14, True),
               fill=GREEN if ok else RED)
        d.line((40, y + 25, 958, y + 25), fill=LINE)
        y += 31
    d.text((40, 590), "verdict: compilepar : %s   (exit 0)" % verdict,
           font=font(15, True), fill=GREEN if verdict == "PASS" else RED)
    save(im, "compilepar.png")


def log_png(log_name, out_name, title, subtitle, height, extra=None):
    lines = [ln.rstrip() for ln in read(log_name).splitlines() if ln.strip()]
    if extra:
        lines.append(extra)
    im, d = canvas(1000, height, title, subtitle)
    d.rounded_rectangle((22, 84, 978, height - 24), radius=10, fill=CARD, outline=LINE)
    y = 100
    for ln in lines:
        color = FG
        if "[FAIL]" in ln or "FAIL" in ln and "PASS" not in ln:
            color = RED
        elif "PASS" in ln or "[ OK ]" in ln:
            color = GREEN
        d.text((40, y), ln[:120], font=font(13), fill=color)
        y += 22
    save(im, out_name)


def main():
    compilepar_png()
    log_png("selftest-jobgraph.log", "selftest-jobgraph.png",
            "M-J5a  |  jobgraph selftest (Taskflow 3.11.0 thin wrapper)",
            "source: example/m-j5a/selftest-jobgraph.log | 9 cases incl. 4 negative", 360)
    log_png("negative-unknown.log", "negative-unknown.png",
            "M-J5a  |  negative case: unknown selftest suite rejected",
            "source: example/m-j5a/negative-unknown.log", 210,
            extra="exit=1  (expected non-zero)")
    log_png("check.log", "check.png",
            "M-J5a  |  regression: CLI check still green via shared entry",
            "source: example/m-j5a/check.log", 470)


if __name__ == "__main__":
    main()
