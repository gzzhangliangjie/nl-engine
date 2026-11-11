#!/usr/bin/env python3
"""M10.1 (S10) probe-viz dots 门禁：探针辐照度点色的机器断言。

用法:
    python tools/check-s10-probe.py dots <png> [min_skyblue] [max_red]

判据（dist=7 基线实测 skyBlue=347/346 双后端一致）:
  - skyBlue 探针点 >= min_skyblue（默认 300）：探针辐照度点存在且成规模
    ——点色 = 探针法线方向八面体 texel，空旷场景表面法线多朝天 → miss
    → skyGradient 蓝亮（Q12-3 解析天穹的确定性输出）。
  - red 级别色残留 <= max_red（默认 0）：dotMode 必须为 1（辐照度采样），
    probe-place 的级别色点（L0 红）混入即失败。
退出码 0 = 全过；1 = 断言失败；2 = 用法错误。
"""
import sys
from PIL import Image


def count_dots(path: str) -> tuple[int, int]:
    im = Image.open(path).convert("RGB")
    w, h = im.size
    p = im.load()
    skyblue = red = 0
    for y in range(h):
        for x in range(w):
            r, g, b = p[x, y]
            # 天空 miss 点（淡蓝亮）：B 通道主导且偏亮——基线实测 347。
            if b > 140 and r < 150 and g < 190 and b > r + 40:
                skyblue += 1
            # probe-place 级别色 L0 红（dotMode 残留检测）。
            if r > 200 and g < 130 and b < 130:
                red += 1
    return skyblue, red


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] != "dots":
        print(__doc__)
        return 2
    path = sys.argv[2]
    min_skyblue = int(sys.argv[3]) if len(sys.argv) > 3 else 300
    max_red = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    skyblue, red = count_dots(path)
    ok = True
    if skyblue < min_skyblue:
        print(f"[FAIL] skyBlue dots {skyblue} < {min_skyblue} "
              f"(probe irradiance dots missing)")
        ok = False
    else:
        print(f"[ok] skyBlue dots {skyblue} >= {min_skyblue}")
    if red > max_red:
        print(f"[FAIL] red level-color residue {red} > {max_red} "
              f"(dotMode leaked to level coloring)")
        ok = False
    else:
        print(f"[ok] red residue {red} <= {max_red}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
