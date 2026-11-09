# -*- coding: utf-8 -*-
"""P8-ECS 工具版证据图生成器（M-E3 收口段）。

铁律：所有内容从工具**真实输出**渲染（example/ecs-e1a/*.log，
由 nanite-scene.exe 实跑重定向落盘），不许手绘示意。产物进 example/ecs-e1a/。
复现：example/ecs-e1a/run.bat（重跑四用例 -> 重新生成本图 -> exit 0 校验）。
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, "example", "ecs-e1a")
OUT = LOGS

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)

def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)

def read_log(name):
    with open(os.path.join(LOGS, name), encoding="utf-8", errors="replace") as f:
        return f.read()

def color_for(s):
    t = s.strip()
    if "PASS" in t:
        return GREEN
    if t.startswith("nanite-scene") or t.startswith("usage:"):
        return CYAN
    if "rejected" in t or "rejected" in t.lower():
        return AMBER
    if t.startswith("suites:") or t.endswith(":") and not t.startswith(" "):
        return DIM
    if t.startswith("  ") and t.strip():
        return DIM
    return FG

def wrap_line(l, limit=118):
    # 超长行按字符折行（terminal 风格续行，前缀缩进对齐）
    if len(l) <= limit:
        return [l]
    out = []
    while len(l) > limit:
        out.append(l[:limit])
        l = "    " + l[limit:]  # 续行缩进
    out.append(l)
    return out

def render(log_name, png_name, title, note=None):
    text = read_log(log_name)
    lines = []
    for l in text.splitlines():
        lines.extend(wrap_line(l.rstrip()))
    if note:
        lines.append("")
        lines.extend(wrap_line(note))
    f = font(17)
    fb = font(19, bold=True)
    ft = font(22, bold=True)
    lh = 28
    pad = 28
    # 最大行宽决定画布（上限 1180）
    tmp = Image.new("RGB", (4, 4))
    td = ImageDraw.Draw(tmp)
    w = min(1180, max(
        [td.textlength(l, font=f) for l in lines] +
        [td.textlength(title, font=ft)]  # 标题参与宽度计算，防截断
    ) + pad * 2 + 40)
    h = pad * 2 + lh * (len(lines) + 2)
    img = Image.new("RGB", (int(w), h), BG)
    d = ImageDraw.Draw(img)
    y = pad
    d.text((pad, y), title, font=ft, fill=CYAN)
    y += lh * 2
    for l in lines:
        d.text((pad, y), l, font=fb if "PASS" in l and "selftest" in l else f,
               fill=color_for(l))
        y += lh
    path = os.path.join(OUT, png_name)
    img.save(path)
    print("wrote", path, img.size)

def main():
    render("usage.log", "usage.png",
           "nanite-scene usage  (exit 2 = usage error; convention 0/1/2)")
    render("system.log", "system.png",
           "nanite-scene selftest --suite ecs-system  (exit 0)")
    render("serial.log", "serial.png",
           "nanite-scene selftest --suite ecs-serial  (exit 0, roundtrip byte-identical)")
    render("negative.log", "negative-badjson.png",
           "nanite-scene deserialise-check badjson  (exit 2, explicit rejection)",
           note="note: malformed JSON rejected explicitly (err non-empty + exit 2), never silent, never assert.")
    print("ALL EVIDENCE DONE")

if __name__ == "__main__":
    main()
