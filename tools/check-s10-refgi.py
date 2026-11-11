#!/usr/bin/env python3
"""M10.3 (S10) reference-gi 趋势门禁：探针 gather 基线 vs GI 参考图的亮暗分布对齐。

用法:
    python tools/check-s10-refgi.py trend <gather_png> <refgi_png> [min_corr] [max_mad]
    python tools/check-s10-refgi.py righthalf <shot_png> <ref_png> [min_corr] [max_mad]

trend 判据（Q3 勘误口径：参考图不是逐像素裁判——S9 参考图只算直光一跳，
S10 自立 GI 参考图同样只做趋势尺）:
  - 参考图缩放到 gather 基线同尺寸，两图按 TILE x TILE 块求均值亮度
    （Rec.709 灰度），得到两条 tile 亮度序列。
  - Pearson 相关系数 >= min_corr（默认 0.60）：亮暗分布趋势一致
    ——猴头亮面在两图都亮、暗面都暗，一次弹射/天光差异只允许噪声级扰动。
  - 平均绝对 tile 亮度差 <= max_mad（默认 30/255）：整体明暗量级同档
    ——gather 是滤波低分辨率估计，refgi 是 1024 spp 收敛真值，逐 tile
    有噪声带，但量级漂移（整体过亮/过暗）即失败。

righthalf 判据（--compare 贴片冒烟）: 两图各裁右半屏后再走同一 tile 逻辑
  ——左半是 live 画面（本就不同于参考图），必须只对比右半贴片区域。
退出码 0 = 全过；1 = 断言失败；2 = 用法错误。
"""
import sys
from PIL import Image

TILE = 16


def tile_luma_img(im: Image.Image) -> list[float]:
    w, h = im.size
    tw, th = w // TILE, h // TILE
    im = im.resize((tw * TILE, th * TILE))
    p = im.load()
    tiles = []
    for ty in range(th):
        for tx in range(tw):
            s = 0.0
            for y in range(ty * TILE, (ty + 1) * TILE):
                for x in range(tx * TILE, (tx + 1) * TILE):
                    r, g, b = p[x, y]
                    s += 0.2126 * r + 0.7152 * g + 0.0722 * b
            tiles.append(s / (TILE * TILE))
    return tiles


def tile_luma(path: str, right_half: bool = False,
              match_size_of: Image.Image | None = None) -> list[float]:
    im = Image.open(path).convert("RGB")
    if right_half:
        w, h = im.size
        im = im.crop((w // 2, 0, w, h))
    return tile_luma_img(im)


def pearson(a: list[float], b: list[float]) -> float:
    n = len(a)
    ma = sum(a) / n
    mb = sum(b) / n
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    va = sum((x - ma) ** 2 for x in a) ** 0.5
    vb = sum((y - mb) ** 2 for y in b) ** 0.5
    if va == 0.0 or vb == 0.0:
        return 0.0
    return cov / (va * vb)


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[1] not in ("trend", "righthalf"):
        print(__doc__)
        return 2
    mode = sys.argv[1]
    gather_path, refgi_path = sys.argv[2], sys.argv[3]
    min_corr = float(sys.argv[4]) if len(sys.argv) > 4 else 0.60
    max_mad = float(sys.argv[5]) if len(sys.argv) > 5 else 30.0

    if mode == "righthalf":
        shot = Image.open(gather_path).convert("RGB")
        w, h = shot.size
        half = shot.crop((w // 2, 0, w, h))          # 贴片区域 = 参考图全幅缩放
        ref_full = Image.open(refgi_path).convert("RGB").resize(half.size)
        g = tile_luma_img(half)
        r = tile_luma_img(ref_full)
    else:
        g = tile_luma(gather_path)
        r = tile_luma(refgi_path)
    if len(g) != len(r):
        print(f"[FAIL] tile count mismatch: gather {len(g)} vs refgi {len(r)}")
        return 1
    corr = pearson(g, r)
    mad = sum(abs(x - y) for x, y in zip(g, r)) / len(g)

    ok = True
    if corr < min_corr:
        print(f"[FAIL] tile-luma pearson {corr:.3f} < {min_corr} "
              f"(bright/dark trend diverges)")
        ok = False
    else:
        print(f"[ok] tile-luma pearson {corr:.3f} >= {min_corr} "
              f"({len(g)} tiles)")
    if mad > max_mad:
        print(f"[FAIL] tile-luma MAD {mad:.1f} > {max_mad} "
              f"(overall brightness drift)")
        ok = False
    else:
        print(f"[ok] tile-luma MAD {mad:.1f} <= {max_mad}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
