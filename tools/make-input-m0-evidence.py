#!/usr/bin/env python3
# P8 M-I0 evidence renderer: input-trace TSV (REAL input-tools output) -> acceptance.png.
# Tool-card convention (ROADMAP「工具用例约定」): the evidence image must come from the
# tool's real output — no hand-drawn diagrams, no other version's screenshots.
# Deterministic output: fixed canvas, fixed palette, no timestamps.
import sys, os
from PIL import Image, ImageDraw, ImageFont

def main():
    tsv = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "example", "input-m0", "input-trace.tsv")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "example", "input-m0", "acceptance.png")
    rows = []
    with open(tsv, encoding="utf-8") as f:
        header = None
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if header is None and parts[0] == "frame":
                header = parts
                continue
            rows.append({
                "frame": int(parts[0]),
                "w_down": int(parts[1]), "w_pressed": int(parts[2]), "w_released": int(parts[3]),
                "space_down": int(parts[4]), "space_pressed": int(parts[5]),
                "lmb_down": int(parts[6]), "lmb_pressed": int(parts[7]), "lmb_released": int(parts[8]),
                "dx": float(parts[9]), "dy": float(parts[10]), "scroll": float(parts[11]),
            })
    if not rows:
        print("ERROR: no data rows in", tsv); return 1

    CELL_W, ROW_H, LEFT, TOP = 44, 34, 150, 88
    n = len(rows)
    W, H = LEFT + CELL_W * n + 24, TOP + ROW_H * 8 + 96
    img = Image.new("RGB", (W, H), (24, 26, 32))
    d = ImageDraw.Draw(img)
    try:
        f_title = ImageFont.truetype("consola.ttf", 20)
        f_lab   = ImageFont.truetype("consola.ttf", 15)
        f_small = ImageFont.truetype("consola.ttf", 13)
    except OSError:
        f_title = f_lab = f_small = ImageFont.load_default()

    d.text((16, 14), "input-m0 acceptance — real input-tools trace TSV",
           font=f_title, fill=(230, 232, 238))
    d.text((16, 42), "green = isDown   yellow = justPressed   orange = justReleased (per frame; endFrame clears edges)",
           font=f_small, fill=(150, 155, 165))

    tracks = [
        ("W down",     [r["w_down"] for r in rows],     (70, 200, 120)),
        ("W pressed",  [r["w_pressed"] for r in rows],  (230, 210, 90)),
        ("W released", [r["w_released"] for r in rows], (235, 150, 70)),
        ("Space down", [r["space_down"] for r in rows], (70, 200, 120)),
        ("Space pres", [r["space_pressed"] for r in rows], (230, 210, 90)),
        ("LMB down",   [r["lmb_down"] for r in rows],   (70, 200, 120)),
        ("LMB press",  [r["lmb_pressed"] for r in rows], (230, 210, 90)),
        ("LMB release",[r["lmb_released"] for r in rows], (235, 150, 70)),
    ]
    for i, (label, vals, color) in enumerate(tracks):
        y = TOP + i * ROW_H
        d.text((10, y + 8), label, font=f_lab, fill=(190, 195, 205))
        for j, v in enumerate(vals):
            x = LEFT + j * CELL_W
            d.rectangle([x, y, x + CELL_W - 3, y + ROW_H - 6], fill=(38, 42, 52))
            if v:
                d.rectangle([x + 2, y + 2, x + CELL_W - 5, y + ROW_H - 8], fill=color)
    # frame headers
    for j, r in enumerate(rows):
        x = LEFT + j * CELL_W
        d.text((x + 12, TOP - 22), str(r["frame"]), font=f_lab, fill=(160, 165, 175))
    # delta curve (mouse dx per frame, scaled) + scroll markers
    base = TOP + 8 * ROW_H + 16
    max_dx = max(max(abs(r["dx"]) for r in rows), 0.001)
    d.text((10, base - 4), "mouse dx/frame (scaled)", font=f_lab, fill=(190, 195, 205))
    pts = [(LEFT + j * CELL_W + CELL_W // 2 - 1, base + 34 - int(30 * r["dx"] / max_dx)) for j, r in enumerate(rows)]
    d.line(pts, fill=(90, 160, 230), width=2)
    for j, r in enumerate(rows):
        if r["scroll"]:
            x = LEFT + j * CELL_W
            d.polygon([(x + 4, TOP - 8), (x + 18, TOP - 8), (x + 11, TOP - 20)], fill=(150, 120, 220))
    d.text((10, H - 26), f"source: {os.path.basename(tsv)}  rows={n}  scroll frames: "
           + ",".join(str(r['frame']) for r in rows if r['scroll']),
           font=f_small, fill=(120, 125, 135))
    img.save(out)
    print("evidence written:", os.path.normpath(out), f"({os.path.getsize(out)} bytes)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
