#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P8 M-D3 evidence: real viewer runs -> frames.tsv + two deterministic PNGs.

Two modes (mirrors M-D2's "anchors from real output, never hand-written"):

  python tools/make-drive-m3-evidence.py --sweep --exe <viewer.exe>
      Run the M-D3 drive demo at 10 frame counts (30..300) with the SAME
      canonical args as the showcase baseline, parse the real final-state log
      lines, and write example/drive-m3/frames.tsv. No number is invented.

  python tools/make-drive-m3-evidence.py
      Read frames.tsv + the real logs captured by example/drive-m3/run.bat and
      render timeline.png (speed / posZ curves + phase bands + MEASURED panel)
      and acceptance.png (terminal-style board of the real logs).

Deterministic: fixed canvas / fonts / colors, no timestamps, no reading the
script's own stdout log (M-D2 pitfall 6). run.bat fc /b compares regenerations.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT = ROOT / "example" / "drive-m3"
TMP = ROOT / "build" / "drive-m3-evidence"

# Same args as the drive-m3 showcase case. Kept here so the sweep cannot drift
# away from the baseline it is supposed to explain.
BASE_ARGS = [
    "--physics-debug", "--physics-scenario", "2", "--drive-demo", "--drive-hud",
    "--fixed-dt", "16.6667", "--size", "960x540",
    "--dist", "45", "--yaw", "90", "--pitch", "40",
]
FRAME_COUNTS = list(range(30, 301, 30))

SCENE_RE = re.compile(
    r"drive scene: speed=([-\d.eE+]+) grounded=(\d+) wallHits=(\d+) "
    r"contacts=(\d+)/(\d+)/(\d+)")
HOOK_RE = re.compile(
    r"drive hook: chassis valid=(\d+) posZ=([-\d.eE+]+) yawDeg=([-\d.eE+]+)")

BG = (18, 20, 24)
FG = (222, 226, 232)
CYAN = (110, 200, 235)
GREEN = (120, 220, 130)
RED = (235, 110, 110)
YELLOW = (235, 205, 105)
GREY = (130, 136, 146)
PANEL = (28, 32, 38)
GRID = (48, 54, 62)
PHASE_BG = [(26, 40, 34), (26, 34, 48), (46, 30, 30)]

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


# --------------------------------------------------------------------------
# sweep
# --------------------------------------------------------------------------
def sweep(exe: Path) -> int:
    TMP.mkdir(parents=True, exist_ok=True)
    rows = []
    for n in FRAME_COUNTS:
        png = TMP / ("f%d.png" % n)
        proc = subprocess.run(
            [str(exe), *BASE_ARGS, "--frames", str(n), "--screenshot", str(png)],
            cwd=str(ROOT), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120)
        log = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            print("[ERROR] viewer exit %d at frames=%d\n%s" %
                  (proc.returncode, n, log[-1200:]), file=sys.stderr)
            return 1
        m = SCENE_RE.search(log)
        h = HOOK_RE.search(log)
        if not m or not h:
            print("[ERROR] missing drive anchors at frames=%d\n%s" %
                  (n, log[-1200:]), file=sys.stderr)
            return 1
        rows.append((n, float(m.group(1)), int(m.group(2)), int(m.group(3)),
                     int(m.group(4)), int(m.group(5)), int(m.group(6)),
                     float(h.group(2)), float(h.group(3))))
    lines = [
        "# m-d3 drive-demo sweep: REAL viewer final-state logs (no hand-written values)",
        "# args=" + " ".join(BASE_ARGS),
        "# columns: frame speed grounded wallHits added persisted removed posZ yawDeg",
    ]
    for r in rows:
        lines.append("F\t%d\t%.7g\t%d\t%d\t%d\t%d\t%d\t%.6f\t%.6f" % r)
    # .gitattributes pins *.tsv to LF; Path.write_text() would emit CRLF on
    # Windows, so a fresh clone would fail run.bat's byte-stability check the
    # first time it regenerates this file. Match the M-D0/D1/D2 TSV blobs.
    with (OUT / "frames.tsv").open("w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    # Keep the archived log tree/machine independent: an absolute worktree
    # path here made example/drive-m3/sweep.log differ after feat/drive was
    # merged into master (and would differ on every fresh clone / worktree).
    print("sweep: %d real runs -> example/drive-m3/frames.tsv" % len(rows))
    for r in rows:
        print("  F %3d speed=%-10.7g posZ=%-9.4f yaw=%-9.4f wallHits=%d" %
              (r[0], r[1], r[7], r[8], r[3]))
    return 0


# --------------------------------------------------------------------------
# data
# --------------------------------------------------------------------------
def read_frames():
    rows = []
    for raw in (OUT / "frames.tsv").read_text(encoding="ascii").splitlines():
        if not raw.startswith("F\t"):
            continue
        p = raw.split("\t")
        rows.append(dict(frame=int(p[1]), speed=float(p[2]), grounded=int(p[3]),
                         wallHits=int(p[4]), added=int(p[5]), persisted=int(p[6]),
                         removed=int(p[7]), posZ=float(p[8]), yawDeg=float(p[9])))
    return rows


def read_log(name, limit=5):
    p = OUT / name
    if not p.is_file():
        return []
    text = p.read_text(encoding="utf-8", errors="replace")
    lines = [ln.rstrip("\r") for ln in text.splitlines() if ln.strip()]
    return lines[-limit:]


# --------------------------------------------------------------------------
# timeline.png
# --------------------------------------------------------------------------
def draw_timeline(rows):
    W, H = 1000, 620
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    d.text((20, 14), "M-D3  DRIVE DEMO  /  real viewer final-state sweep",
           font=font(19, True), fill=FG)
    d.text((20, 40), "frames.tsv  |  10 independent deterministic runs  |  fixed dt 16.6667 ms",
           font=font(12), fill=GREY)

    # --- chart 1: speed ---------------------------------------------------
    x0, x1, y0, y1 = 70, 660, 86, 300
    fmax = max(r["frame"] for r in rows)
    smax = max(r["speed"] for r in rows) * 1.15 or 1.0
    bands = [(2, 89, PHASE_BG[0], "throttle W"),
             (90, 149, PHASE_BG[1], "W + steer D"),
             (150, 209, PHASE_BG[0], "throttle W"),
             (210, 300, PHASE_BG[2], "handbrake SPACE")]
    for a, b, col, _ in bands:
        xa = x0 + int((a / fmax) * (x1 - x0))
        xb = x0 + int((min(b, fmax) / fmax) * (x1 - x0))
        d.rectangle([xa, y0, xb, y1], fill=col)
    for i in range(5):
        y = y0 + int(i * (y1 - y0) / 4)
        d.line([(x0, y), (x1, y)], fill=GRID)
        d.text((16, y - 8), "%.1f" % (smax * (4 - i) / 4), font=font(11), fill=GREY)
    d.rectangle([x0, y0, x1, y1], outline=GRID)
    d.text((x0, y0 - 22), "speed (m/s)", font=font(13, True), fill=CYAN)
    pts = [(x0 + int((r["frame"] / fmax) * (x1 - x0)),
            y1 - int((r["speed"] / smax) * (y1 - y0))) for r in rows]
    d.line(pts, fill=CYAN, width=2)
    for p in pts:
        d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=CYAN)
    for a, b, _, label in bands:
        xa = x0 + int((a / fmax) * (x1 - x0))
        if xa > x1 - 120:
            continue
        d.text((xa + 4, y1 + 6), label, font=font(10), fill=GREY)
    for f in (0, 90, 150, 210, 300):
        x = x0 + int((f / fmax) * (x1 - x0))
        d.text((x - 12, y1 + 24), "f%d" % f, font=font(11), fill=GREY)

    # --- chart 2: posZ ----------------------------------------------------
    # zmax must include the wall (z=18, World.cpp:1413), not just the curve
    # peak (~15.3): otherwise the wall line lands outside the plot frame and
    # its label collides with this chart's title.
    y2, y3 = 370, 530
    zmin = 0.0
    zmax = max(max(r["posZ"] for r in rows) * 1.15, 20.0)
    for i in range(4):
        y = y2 + int(i * (y3 - y2) / 3)
        d.line([(x0, y), (x1, y)], fill=GRID)
        d.text((16, y - 8), "%.1f" % (zmax * (3 - i) / 3), font=font(11), fill=GREY)
    d.rectangle([x0, y2, x1, y3], outline=GRID)
    d.text((x0, y2 - 22), "chassis Z (m)", font=font(13, True), fill=GREEN)
    pts = [(x0 + int((r["frame"] / fmax) * (x1 - x0)),
            y3 - int(((r["posZ"] - zmin) / (zmax - zmin)) * (y3 - y2))) for r in rows]
    d.line(pts, fill=GREEN, width=2)
    for p in pts:
        d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=GREEN)
    yline = y3 - int((18.0 / zmax) * (y3 - y2))
    d.line([(x0, yline), (x1, yline)], fill=RED, width=1)
    d.text((x1 - 90, yline + 4), "wall z=18", font=font(10), fill=RED)

    # --- measured panel ---------------------------------------------------
    px, py = 690, 86
    d.rectangle([px, py, W - 20, H - 22], fill=PANEL, outline=GRID)
    d.text((px + 14, py + 12), "MEASURED  (parsed from real logs)",
           font=font(14, True), fill=YELLOW)
    last = rows[-1]
    first_hit = next((r["frame"] for r in rows if r["wallHits"] > 0), -1)
    items = [
        ("runs", "%d" % len(rows)),
        ("frame range", "%d .. %d" % (rows[0]["frame"], last["frame"])),
        ("top speed", "%.7g m/s" % max(r["speed"] for r in rows)),
        ("final speed", "%.7g m/s" % last["speed"]),
        ("final posZ", "%.4f m" % last["posZ"]),
        ("final yaw", "%.4f deg" % last["yawDeg"]),
        ("final grounded", "%d / 4" % last["grounded"]),
        ("final contacts", "%d / %d / %d" % (last["added"], last["persisted"], last["removed"])),
        ("wallHits", "%d (first seen f%d)" % (last["wallHits"], first_hit)),
    ]
    yy = py + 44
    for k, v in items:
        d.text((px + 14, yy), k, font=font(12), fill=GREY)
        d.text((px + 152, yy), v, font=font(12, True), fill=FG)
        yy += 26
    yy += 8
    d.text((px + 14, yy), "cvar / gates (verified by real runs)",
           font=font(12, True), fill=CYAN)
    yy += 24
    # The raw cvar log line is ~82 chars and would be cut in half here; the
    # four verdict lines fit in the panel. The full raw line is shown on
    # acceptance.png block [4/5] where the wider board has room for it.
    for line in read_log("cvar.log", 4):
        d.text((px + 14, yy), line[:54], font=font(10), fill=FG)
        yy += 17
    im.save(OUT / "timeline.png")
    return im.size


# --------------------------------------------------------------------------
# acceptance.png
# --------------------------------------------------------------------------
def draw_acceptance(rows):
    W, H = 1000, 700
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W, 34], fill=(30, 36, 44))
    d.ellipse([12, 12, 22, 22], fill=RED)
    d.ellipse([30, 12, 40, 22], fill=YELLOW)
    d.ellipse([48, 12, 58, 22], fill=GREEN)
    d.text((74, 8), "DRIVE-M3 ACCEPTANCE  /  real tool + viewer output",
           font=font(15, True), fill=FG)

    blocks = [
        ("[1/5] phys-tools selftest --verbose", "selftest.log", 5),
        ("[2/5] M-D0/D1/D2 archived TSV zero-regression", "zero-regression.log", 4),
        ("[3/5] DX12 / VULKAN drive-demo screenshot", "cross-backend.log", 4),
        ("[4/5] console cvar + default-off gate + negative", "cvar.log", 5),
        ("[5/5] real frame sweep (10 viewer runs)", "sweep.log", 4),
    ]
    y = 50
    for title, name, limit in blocks:
        d.text((18, y), title, font=font(13, True), fill=CYAN)
        y += 22
        lines = read_log(name, limit)
        if not lines:
            d.text((32, y), "(log missing)", font=font(11), fill=RED)
            y += 18
        for ln in lines:
            d.text((32, y), ln[:118], font=font(11), fill=FG)
            y += 16
        y += 12

    # final-state strip from frames.tsv
    last = rows[-1]
    d.rectangle([18, H - 78, W - 18, H - 18], fill=PANEL, outline=GRID)
    d.text((30, H - 68), "FINAL STATE  f%d" % last["frame"], font=font(12, True), fill=YELLOW)
    strip = ("speed=%.7g m/s   posZ=%.4f m   yaw=%.4f deg   grounded=%d/4   "
             "contacts=%d/%d/%d   wallHits=%d" %
             (last["speed"], last["posZ"], last["yawDeg"], last["grounded"],
              last["added"], last["persisted"], last["removed"], last["wallHits"]))
    d.text((30, H - 46), strip, font=font(12), fill=FG)
    im.save(OUT / "acceptance.png")
    return im.size


def main() -> int:
    ap = argparse.ArgumentParser(description="M-D3 evidence renderer")
    ap.add_argument("--sweep", action="store_true", help="run viewer and rebuild frames.tsv")
    ap.add_argument("--exe", default=None, help="viewer.exe path for --sweep")
    args = ap.parse_args()
    if args.sweep:
        exe = Path(args.exe) if args.exe else ROOT / "build" / "DX12" / "viewer.exe"
        if not exe.is_file():
            print("[ERROR] viewer not found: %s" % exe, file=sys.stderr)
            return 1
        return sweep(exe)
    if not (OUT / "frames.tsv").is_file():
        print("[ERROR] frames.tsv missing; run --sweep first", file=sys.stderr)
        return 1
    rows = read_frames()
    if not rows:
        print("[ERROR] frames.tsv has no F rows", file=sys.stderr)
        return 1
    print("timeline.png  %s" % (draw_timeline(rows),))
    print("acceptance.png %s" % (draw_acceptance(rows),))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
