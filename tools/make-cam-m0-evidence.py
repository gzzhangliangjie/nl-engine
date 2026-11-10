#!/usr/bin/env python3
# P8 M-CAM0 evidence renderer: cam-trace.tsv + free-trace.tsv (REAL cam-tools
# output) -> acceptance.png.
# Tool-card convention (ROADMAP「工具用例约定」): the evidence image must come from
# the tool's real output — no hand-drawn diagrams, no other version's screenshots.
# Deterministic output: fixed canvas, fixed palette, no timestamps.
import sys, os, math
from PIL import Image, ImageDraw, ImageFont

def load_tsv(path, ncols):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if parts[0] == "frame":
                continue
            if len(parts) != ncols:
                print("ERROR: bad row in", path, ":", line); sys.exit(1)
            rows.append([float(x) if i else int(x) for i, x in enumerate(parts)])
    if not rows:
        print("ERROR: no data rows in", path); sys.exit(1)
    return rows

def main():
    base = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "example", "cam-m0")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(base, "acceptance.png")
    cam = load_tsv(os.path.join(base, "cam-trace.tsv"), 7)    # f ex ey ez off pitch mode
    free = load_tsv(os.path.join(base, "free-trace.tsv"), 6)  # f ex ey ez yaw pitch

    W, H = 1180, 620
    img = Image.new("RGB", (W, H), (24, 26, 32))
    d = ImageDraw.Draw(img)
    f_title = ImageFont.truetype("consola.ttf", 18)
    f_lab   = ImageFont.truetype("consola.ttf", 13)
    f_small = ImageFont.truetype("consola.ttf", 11)

    d.text((16, 10), "cam-tools M-CAM0 acceptance (real cam-trace/free-trace output)", font=f_title, fill=(230, 230, 235))

    def panel(x0, y0, w, h, title):
        d.rectangle([x0, y0, x0 + w, y0 + h], outline=(90, 96, 110))
        d.text((x0 + 8, y0 + 6), title, font=f_lab, fill=(180, 186, 200))
        return x0 + 12, y0 + 34, w - 24, h - 48

    # ---- p1: chase eye top-down (x-z) ----
    px, py, pw, ph = panel(16, 44, 560, 260, "chase: eye top-down (x-z), straight f0-89 / turn f90-149")
    exs = [r[1] for r in cam]; ezs = [r[3] for r in cam]
    xmin, xmax = min(exs) - 1, max(exs) + 1
    zmin, zmax = min(ezs) - 1, max(ezs) + 1
    def mapx(v, lo, hi, a, b): return a + (v - lo) / (hi - lo) * (b - a)
    # draw target path (script geometry, dashed reference) + eye path (real TSV)
    tgt = [(6.0 / 60.0 * min(f, 90), 6.0 / 60.0 * max(0, f - 90)) for f in range(150)]
    for i in range(len(tgt) - 1):
        x1 = mapx(tgt[i][0], xmin, xmax, px, px + pw); y1 = mapx(tgt[i][1], zmin, zmax, py + ph, py)
        x2 = mapx(tgt[i + 1][0], xmin, xmax, px, px + pw); y2 = mapx(tgt[i + 1][1], zmin, zmax, py + ph, py)
        d.line([x1, y1, x2, y2], fill=(120, 120, 130))
    for i in range(len(cam) - 1):
        x1 = mapx(exs[i], xmin, xmax, px, px + pw); y1 = mapx(ezs[i], zmin, zmax, py + ph, py)
        x2 = mapx(exs[i + 1], xmin, xmax, px, px + pw); y2 = mapx(ezs[i + 1], zmin, zmax, py + ph, py)
        d.line([x1, y1, x2, y2], fill=(90, 200, 250))
    d.text((px + pw - 170, py + 4), "blue=eye(Real TSV) gray=target(script)", font=f_small, fill=(160, 166, 180))

    # ---- p2: chase boom_off_deg curve + orbit band ----
    px, py, pw, ph = panel(600, 44, 564, 260, "chase: boomYawOffset deg (kick f10, auto-return, orbit freeze f120-139)")
    offs = [r[4] for r in cam]
    omax = 13.0
    # orbit band
    bx1 = mapx(120, 0, 149, px, px + pw); bx2 = mapx(139, 0, 149, px, px + pw)
    d.rectangle([bx1, py, bx2, py + ph], fill=(60, 55, 30))
    for i in range(len(cam) - 1):
        x1 = mapx(i, 0, 149, px, px + pw); y1 = py + ph - offs[i] / omax * ph
        x2 = mapx(i + 1, 0, 149, px, px + pw); y2 = py + ph - offs[i + 1] / omax * ph
        d.line([x1, y1, x2, y2], fill=(250, 190, 60), width=2)
    d.text((bx1 - 158, py + 6), "Orbit(no auto-return)", font=f_small, fill=(230, 210, 140))
    d.text((px + 6, py + ph - 18), "0 deg", font=f_small, fill=(160, 166, 180))
    d.text((px + pw - 60, py + 4), "12 deg kick", font=f_small, fill=(160, 166, 180))

    # ---- p3: free eye top-down ----
    px, py, pw, ph = panel(16, 330, 560, 260, "free: eye top-down (x-z), boost f10-29 / yaw+90 f30-49 / dive f50-59")
    fex = [r[1] for r in free]; fez = [r[3] for r in free]
    fxmin, fxmax = min(fex) - 1, max(fex) + 1
    fzmin, fzmax = min(fez) - 1, max(fez) + 1
    for i in range(len(free) - 1):
        x1 = mapx(fex[i], fxmin, fxmax, px, px + pw); y1 = mapx(fez[i], fzmin, fzmax, py + ph, py)
        x2 = mapx(fex[i + 1], fxmin, fxmax, px, px + pw); y2 = mapx(fez[i + 1], fzmin, fzmax, py + ph, py)
        d.line([x1, y1, x2, y2], fill=(140, 240, 160))
    d.text((px + 6, py + 4), "start (0,0)", font=f_small, fill=(160, 166, 180))

    # ---- p4: free yaw/pitch curves ----
    px, py, pw, ph = panel(600, 330, 564, 260, "free: yaw deg (0->90) / pitch deg (0->-11.46, clamped)")
    yaws = [r[4] for r in free]; pits = [r[5] for r in free]
    for i in range(len(free) - 1):
        x1 = mapx(i, 0, 59, px, px + pw); y1 = py + ph - yaws[i] / 100.0 * ph
        x2 = mapx(i + 1, 0, 59, px, px + pw); y2 = py + ph - yaws[i + 1] / 100.0 * ph
        d.line([x1, y1, x2, y2], fill=(90, 200, 250), width=2)
        y1 = py + ph - (pits[i] + 20.0) / 40.0 * ph
        y2 = py + ph - (pits[i + 1] + 20.0) / 40.0 * ph
        d.line([x1, y1, x2, y2], fill=(250, 130, 130), width=2)
    d.text((px + 6, py + 4), "blue=yaw (0..90)", font=f_small, fill=(140, 200, 240))
    d.text((px + 6, py + 20), "red=pitch (-11.46..0, offset -20..+20 scale)", font=f_small, fill=(240, 170, 170))

    d.text((16, H - 26), "source: example/cam-m0/cam-trace.tsv + free-trace.tsv (cam-tools real output, deterministic replay)",
           font=f_small, fill=(140, 146, 160))
    img.save(out)
    print("wrote", out)

if __name__ == "__main__":
    main()
