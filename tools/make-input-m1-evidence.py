#!/usr/bin/env python3
# P8 M-I1 evidence renderer: act-trace TSV (REAL input-tools act-trace output)
# -> acceptance.png. Tool-card convention: the evidence image must come from the
# tool's real output — no hand-drawn diagrams. Deterministic output.
import sys, os
from PIL import Image, ImageDraw, ImageFont

def main():
    tsv = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "example", "input-m1", "act-trace.tsv")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "example", "input-m1", "acceptance.png")
    rows = []
    with open(tsv, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            p = line.split("\t")
            if p[0] == "frame":
                continue
            rows.append({"frame": int(p[0]), "th": float(p[1]), "st": float(p[2]),
                         "lx": float(p[3]), "ly": float(p[4]),
                         "hb": int(p[5]), "hbP": int(p[6]), "pause": int(p[7]), "pauseP": int(p[8])})
    if not rows:
        print("ERROR: no data rows in", tsv); return 1

    CELL_W, ROW_H, LEFT, TOP = 44, 34, 170, 96
    n = len(rows)
    W = max(LEFT + CELL_W * n + 24, 1000)   # 标题/副标题完整放下（实测 722px 会裁）
    H = TOP + ROW_H * 7 + 70
    img = Image.new("RGB", (W, H), (24, 26, 32))
    d = ImageDraw.Draw(img)
    try:
        f_title = ImageFont.truetype("consola.ttf", 20)
        f_lab   = ImageFont.truetype("consola.ttf", 15)
        f_small = ImageFont.truetype("consola.ttf", 13)
    except OSError:
        f_title = f_lab = f_small = ImageFont.load_default()

    d.text((16, 14), "input-m1 acceptance — real input-tools act-trace TSV (driving action map)",
           font=f_title, fill=(230, 232, 238))
    d.text((16, 42), "Float = signed max-abs (W/S throttle, A/D steer, tie -> first-defined)   Bool = OR   Set off = zeroed",
           font=f_small, fill=(150, 155, 165))

    # 数值条：throttle/steer 用双色（正=青、负=橙），中点竖线。
    def value_track(idx, label, vals):
        y = TOP + idx * ROW_H
        d.text((10, y + 8), label, font=f_lab, fill=(190, 195, 205))
        vmax = max(max(abs(v) for v in vals), 1.0)
        for j, v in enumerate(vals):
            x = LEFT + j * CELL_W
            d.rectangle([x, y, x + CELL_W - 3, y + ROW_H - 6], fill=(38, 42, 52))
            d.line([(x + CELL_W // 2 - 1, y + 4), (x + CELL_W // 2 - 1, y + ROW_H - 10)], fill=(70, 76, 88))
            if v:
                w = int((CELL_W / 2 - 6) * abs(v) / vmax)
                color = (90, 200, 230) if v > 0 else (235, 150, 70)
                if v > 0: d.rectangle([x + CELL_W // 2, y + 4, x + CELL_W // 2 + w, y + ROW_H - 10], fill=color)
                else:     d.rectangle([x + CELL_W // 2 - w, y + 4, x + CELL_W // 2, y + ROW_H - 10], fill=color)

    value_track(0, "throttle",  [r["th"] for r in rows])   # f4 tie -> W(+1) 锚
    value_track(1, "steer",     [r["st"] for r in rows])
    # look 向量：x/y 分量小条。
    value_track(2, "look x",    [r["lx"] for r in rows])
    value_track(3, "look y",    [r["ly"] for r in rows])
    # Bool 轨：handbrake / pause（黄 = value，亮黄 = justPressed 边沿）。
    bool_tracks = [
        (4, "handbrake",  "hb",  "hbP"),
        (5, "pause(ui)",  "pause", "pauseP"),
    ]
    for idx, label, vk, vp in bool_tracks:
        y = TOP + idx * ROW_H
        d.text((10, y + 8), label, font=f_lab, fill=(190, 195, 205))
        for j, r in enumerate(rows):
            x = LEFT + j * CELL_W
            d.rectangle([x, y, x + CELL_W - 3, y + ROW_H - 6], fill=(38, 42, 52))
            if r[vk]:
                d.rectangle([x + 2, y + 2, x + CELL_W - 5, y + ROW_H - 8], fill=(230, 210, 90))
            if r[vp]:
                d.rectangle([x + CELL_W - 10, y + 2, x + CELL_W - 5, y + ROW_H - 8], fill=(255, 240, 150))
    # 事件标记行：组开关（f8 ui on / f9 ui off）与关键事件注释。
    y = TOP + 6 * ROW_H
    d.text((10, y + 8), "events", font=f_lab, fill=(190, 195, 205))
    marks = {0: "W dn", 2: "D+scr", 4: "S dn", 6: "SP dn", 7: "Esc", 8: "ui ON", 9: "ui OFF", 10: "all up"}
    for j, r in enumerate(rows):
        if r["frame"] in marks:
            x = LEFT + j * CELL_W
            # 奇偶行错位绘制，防相邻单元格长标签水平重叠（44px/格 @13px 字号）。
            dy = 4 if r["frame"] % 2 == 0 else 20
            d.text((x + 2, y + dy), marks[r["frame"]], font=f_small, fill=(150, 120, 220))
    for j, r in enumerate(rows):
        x = LEFT + j * CELL_W
        d.text((x + 12, TOP - 22), str(r["frame"]), font=f_lab, fill=(160, 165, 175))
    d.text((10, H - 26), f"source: {os.path.basename(tsv)}  rows={n}  "
           "anchors: f0 th=+1 / f2 st=+1 / f4 tie->W / f6 hb edge / f8 pause edge / f9 zeroed / f10 zeros",
           font=f_small, fill=(120, 125, 135))
    img.save(out)
    print("evidence written:", os.path.normpath(out), f"({os.path.getsize(out)} bytes)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
