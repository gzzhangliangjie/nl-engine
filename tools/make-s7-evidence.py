# -*- coding: utf-8 -*-
"""S7 收官证据图（M7.2 收口段，S7-PLAN 验收要求 docs/screenshots/s7.png）。

铁律：所有内容从工具**真实输出**渲染（viewer --screenshot 实拍 + verify.py
实测数字），不许手绘示意。
四格布局：
  上排 = visbuf 调试图（M7.0 簇色档 / 三角形档，Suzanne-materials 711 簇 3 子网格）
  下排 = 一致性对比（vis buffer 材质反解 vs 传统直画 mode 5，DX12；数字来自
         example/s7-done/verify.py 实测）
复现：build/DX12/viewer.exe 拍图（命令见 SOURCES 注释）+ python tools/make-s7-evidence.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "screenshots", "s7.png")

SOURCES = [
    # (path, caption, line2)
    (os.path.join(ROOT, "build", "showcase", "runs", "s72-viz-cluster.png"),
     "visbuf debug: --vis-viz cluster (711 clusters, 3 submeshes, pcg(clusterId))",
     "viewer --mode visbuf --model Suzanne-materials.nmesh (DX12)"),
    (os.path.join(ROOT, "build", "showcase", "runs", "s72-viz-tri.png"),
     "visbuf debug: --vis-viz triangle (11808 tris, edge-welded packing)",
     "viewer --mode visbuf --model Suzanne-materials.nmesh (DX12)"),
    (os.path.join(ROOT, "example", "s7-done", "materials-pulling-dx12.png"),
     "vis buffer path: --mode pulling --shading 5 (indirect material table)",
     "raster u64 visbuf -> full-screen resolve -> clusterMeta[3] materialId -> GPUMaterial[4]"),
    (os.path.join(ROOT, "example", "s7-done", "materials-normalref-dx12.png"),
     "reference path: --mode normal --shading 5 (per-prim direct draw)",
     "model.slang untouched | A/B: sil=0 | 99.991%<=2 | maxDelta=4 | bias=0.0065"),
]

BG = (18, 20, 24)
FG = (222, 226, 232)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
TITLE = "S7 done - visibility buffer + material resolve (M7.0 visbuf / M7.1 pulling / M7.2 multi-material indirect table)"

# 各源图与基线同源（viewer 实拍 960x540），统一切到 CELL 宽度
CELL_W = 620
GAP = 16
PAD = 20

def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)

def main():
    ft = font(19, bold=True)
    fc = font(15, bold=True)
    fl = font(13)
    # 缩放源图并计算布局
    cells = []
    for path, cap, sub in SOURCES:
        im = Image.open(path).convert("RGB")
        h = round(im.height * CELL_W / im.width)
        cells.append((im.resize((CELL_W, h), Image.LANCZOS), cap, sub))
    row_h = max(c[0].height for c in cells)
    cap_h = 24 + 20  # caption + sub line
    title_h = 40
    w = PAD * 2 + CELL_W * 2 + GAP
    h = title_h + PAD + (row_h + cap_h) * 2 + PAD
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    d.text((PAD, 12), TITLE, font=ft, fill=FG)
    for i, (im, cap, sub) in enumerate(cells):
        r, c = divmod(i, 2)
        x = PAD + c * (CELL_W + GAP)
        y = title_h + r * (row_h + cap_h + GAP // 2)
        img.paste(im, (x, y))
        col = CYAN if i < 2 else GREEN
        d.text((x, y + row_h + 4), cap, font=fc, fill=col)
        d.text((x, y + row_h + 24), sub, font=fl, fill=(150, 156, 166))
    img.save(OUT)
    print("written:", OUT, img.size)

if __name__ == "__main__":
    main()
