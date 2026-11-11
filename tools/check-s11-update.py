#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S11 M11.1 cache-viz radiance 模式的统计断言（Q11-1 拍板 A：统计口径）。

背景（2026-11-11）：M11.1 csCacheUpdate 的 GI 邻居查询把 RayQuery 命中点量化进
cell key，而 M8.1 TLAS 每帧 rebuild 的 AS build 内部布局 D3D12/Vulkan 规范不保证
bit-exact（M10.1 探针只用命中/未命中二值不受影响）→ radiance 收敛图天生非逐字节
可复现（实测三次运行三份 md5，差异 ~9% 像素）。M11.0 mode 0 伪彩不依赖 radiance
数值，基线逐字节保形不受影响。故 GPU 侧断言改统计口径：
  - 伪彩覆盖：模型区域 cell 亮度像素占比 >= 阈值（cell 更新确实点亮了缓存）
  - 空间结构：直光实例（左/右）亮度均值显著高于背光实例（中）——update 的
    DI 项 NdotL 语义的直接证据
CPU 侧（gi-tools cachesim --update）为单线程仿真，bytes + 收敛数值仍逐字节断言。

用法：
  python tools/check-s11-update.py stats <png> <cover_min> <lit_margin>
      覆盖率(非背景像素占比) >= cover_min；亮区(左/右实例带)均值 - 背光(中带)
      均值 >= lit_margin（0..255 gamma 域 luma）
"""
import sys

import numpy as np
from PIL import Image


def luma(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def stats(path, cover_min, lit_margin):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    h, w, _ = img.shape
    # 背景判定：暗底 (0.006,0.009,0.018) gamma 化 ≈ (25,30,41)，容差 ±12
    bg = (np.abs(img[..., 0] - 25) < 12) & (np.abs(img[..., 1] - 30) < 12) \
        & (np.abs(img[..., 2] - 41) < 12)
    cover = 1.0 - float(bg.mean())
    # 三实例布局（--yaw 12 --pitch 9 --dist 7，960x540）：左/中/右各占横向三带，
    # 实例体在垂直 30%~75%。直光带 = 左右实例列（x 12%~32% / 68%~88%），
    # 背光带 = 中实例列（x 40%~60%）。
    band = lambda x0, x1: (slice(int(h * 0.30), int(h * 0.75)),
                           slice(int(w * x0), int(w * x1)))
    lum = luma(img)
    lit = 0.5 * (lum[band(0.12, 0.32)].mean() + lum[band(0.68, 0.88)].mean())
    back = lum[band(0.40, 0.60)].mean()
    ok = cover >= cover_min and (lit - back) >= lit_margin
    print(f"cover    : {cover:.4f} (min {cover_min})")
    print(f"lit mean : {lit:.2f}  back mean : {back:.2f}  margin {lit - back:.2f} (min {lit_margin})")
    print("stats    : " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == "stats":
        return stats(argv[2], float(argv[3]), float(argv[4]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
