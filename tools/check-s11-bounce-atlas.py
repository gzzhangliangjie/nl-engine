#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S11 M11.2 --probe-atlas-dump 的 ON vs OFF 统计断言（atlas 级验收）。

为什么需要它（2026-11-12 实测结论）：
  M11.2 让 probe_trace 命中点查世界缓存（命中→albedo*(C/pi+ambient) 的二级弹射光），
  真正改写的对象是 g_irrad 的【命中方向 texel】。而 --mode probe 的 csGather 每像素
  只采样 atlas 的【外法线方向 texel】，对纯天空场景的凸体（Suzanne）该方向恒 miss→
  天空 texel，故合成图天然 ON≡OFF（结构性，非缓存 bug）。因此 M11.2 的验收对象必须是
  probe atlas 本身——用 --probe-atlas-dump 把 irradAtlas_[0] 直出，再跑本脚本自比。

断言（统计口径，Q11-2d「同管线开关开/关自比统计增量」）：
  python tools/check-s11-bounce-atlas.py onoff <on.png> <off.png> [margin=0.2] [eps=0.3]

  - not byte-identical（修好后应「不同」；字节相同反而说明 gate 没生效 / 查询没命中）
  - mean abs diff（RGB）>= margin（系统性增量；atlas 天空 texel 占多数会稀释均值，
    故 margin 远低于合成图的 0.5）
  - 变化像素占比（单通道差 > 2/255）>= eps（结构化增量，排除浮点噪声）

全部满足才 PASS。margin/eps 首跑裸机定：DX12 mean≈0.47、变化像素≈1.1%、质心落在
画面中央物体区（非散点噪声）→ 确认缓存查询路径真实改写 atlas。
"""
import sys

import numpy as np
from PIL import Image


def onoff(on_path, off_path, margin, eps):
    a = np.asarray(Image.open(on_path).convert("RGB"), dtype=np.float64)
    b = np.asarray(Image.open(off_path).convert("RGB"), dtype=np.float64)
    ha, wa = a.shape[:2]
    hb, wb = b.shape[:2]
    if (ha, wa) != (hb, wb):
        print(f"size mismatch: on {wa}x{ha} off {wb}x{hb}")
        return 1

    import hashlib
    h_on = hashlib.md5(open(on_path, "rb").read()).hexdigest()
    h_off = hashlib.md5(open(off_path, "rb").read()).hexdigest()
    byte_identical = (h_on == h_off)

    diff = np.abs(a - b)
    mean_abs = float(diff.mean())
    max_abs = float(diff.max())
    changed = (diff > 2.0).any(axis=2)
    changed_frac = float(changed.mean())

    # 变化像素质心（确认落在物体区而非散点噪声）
    if changed.sum() > 0:
        ys, xs = np.where(changed)
        cy, cx = int(ys.mean()), int(xs.mean())
    else:
        cy, cx = -1, -1

    print(f"ON  = {on_path}")
    print(f"OFF = {off_path}")
    print(f"  byte-identical = {byte_identical}")
    print(f"  mean abs diff (RGB) = {mean_abs:.3f}   (margin >= {margin})")
    print(f"  max  abs diff       = {max_abs:.1f}")
    print(f"  changed pixels      = {changed_frac*100:.2f}%   (eps >= {eps}%)")
    print(f"  changed centroid    = ({cy}, {cx})  of {ha}x{wa}")

    ok = (not byte_identical) and (mean_abs >= margin) and (changed_frac >= eps / 100.0)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    if len(sys.argv) < 3 or sys.argv[1] != "onoff":
        print("usage: check-s11-bounce-atlas.py onoff <on.png> <off.png> [margin=0.2] [eps=0.3]")
        return 2
    margin = float(sys.argv[4]) if len(sys.argv) > 4 else 0.2
    eps = float(sys.argv[5]) if len(sys.argv) > 5 else 0.3
    return onoff(sys.argv[2], sys.argv[3], margin, eps)


if __name__ == "__main__":
    sys.exit(main())
