#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S11 M11.2 --probe-cache-bounce 的 ON vs OFF 统计断言（Q11-1 拍板 A：统计口径）。

背景（2026-11-12）：M11.2 让 probe_trace 命中点查世界缓存（命中→albedo*(C/pi+ambient)
的二级弹射光）。验证不是「on/off 逐字节相同即 bug」——恰恰相反，修好后 ON 应比 OFF
在间接光区系统性更亮；而 M8.1 TLAS 每帧 rebuild 非 bit-exact + hitP 量化进 cell key
（Q11-1），两份 radiance 图天生非逐字节可复现。故断言 = 统计口径：

  python tools/check-s11-bounce.py onoff <on.png> <off.png> <margin>

  - 整帧（或间接光带）luma 均值：ON - OFF >= margin（bounce 系统性加亮间接项）
  - 同时报告两图是否字节相同（修好后应「不同」；字节相同反而说明 gate 没生效）

margin 首次裸机跑定（CPU 仿真 late 0.35212 vs ctrl 0.31920 ≈ 间接项 +10%，全局均值
位移约 1~3 / 255；保守默认 0.5）。TLAS 噪声随机、均值趋零，系统性位移可分辨。
"""
import sys

import numpy as np
from PIL import Image


def luma(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def onoff(on_path, off_path, margin):
    a = np.asarray(Image.open(on_path).convert("RGB"), dtype=np.float64)
    b = np.asarray(Image.open(off_path).convert("RGB"), dtype=np.float64)
    ha, wa = a.shape[:2]
    hb, wb = b.shape[:2]
    if (ha, wa) != (hb, wb):
        print(f"size mismatch: on {wa}x{ha} off {wb}x{hb}")
        return 1
    la, lb = luma(a), luma(b)
    mean_on, mean_off = float(la.mean()), float(lb.mean())
    delta = mean_on - mean_off
    # 间接光带：场景地面（y=-1 轴对齐面，bounce 效应最强）占画面下 55%~95%
    band = (slice(int(ha * 0.55), int(ha * 0.95)), slice(0, wa))
    band_on, band_off = float(la[band].mean()), float(lb[band].mean())
    band_delta = band_on - band_off
    ok = delta >= margin
    print(f"mean on  : {mean_on:.2f}")
    print(f"mean off : {mean_off:.2f}")
    print(f"delta    : {delta:.2f} (min {margin})  -> {'PASS' if ok else 'FAIL'}")
    print(f"floorband on/off delta : {band_delta:.2f}  (bounce 应使地面更亮)")
    # 字节相同反而说明 gate 没生效（修好前 ON/OFF 逐字节相同 = 旧 bug）
    same = bool(np.array_equal(a, b))
    print(f"identical : {same}  (修好后应 False；True = gate 未生效)")
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == "onoff":
        return onoff(argv[2], argv[3], float(argv[4]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
