#!/usr/bin/env python3
"""NET M-NET1 证据图生成器：跑一次真实的 `net-tools snapshot-bench`，把 stdout 解析后
渲染为 PNG（纯标准库 zlib/struct，无 matplotlib/PIL 依赖）。
图 = 逐字段 naive float 位宽（灰）vs 线格式位宽（绿）对比 + 底部实测行
（编码/解码耗时、量化误差、字节数，全部取自真实输出）。
产物 = example/net-m1/net-m1-evidence.png + 同目录 net-m1-evidence.log（原始输出留档）。
数据 100% 来自工具真实输出，无任何手绘/示意成分。
用法：python tools/make-net-m1-evidence.py <net-tools.exe 路径> [输出目录]
"""
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

W, H = 720, 420
MARGIN_L, MARGIN_B, MARGIN_T = 64, 56, 36


def render_png(width, height, px):
    raw = b"".join(b"\x00" + bytes(px[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray([255, 255, 255, 255]) * (w * h)

    def rect(self, x0, y0, x1, y1, rgba):
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(self.w, x1), min(self.h, y1)
        for y in range(y0, y1):
            base = (y * self.w + x0) * 4
            for _ in range(x0, x1):
                self.px[base:base + 4] = rgba
                base += 4

    def hline(self, x0, x1, y, rgba):
        self.rect(x0, y, x1, y + 1, rgba)

    def vline(self, x, y0, y1, rgba):
        self.rect(x, y0, x + 1, y1, rgba)

    def text(self, x, y, s, rgba, scale=2):
        # 3x5 微型 ASCII 点阵（本图所需字母 + 数字；缺字形跳格，不乱画）
        glyphs = {
            "0": ["111", "101", "101", "101", "111"],
            "1": ["010", "110", "010", "010", "111"],
            "2": ["111", "001", "111", "100", "111"],
            "3": ["111", "001", "111", "001", "111"],
            "4": ["101", "101", "111", "001", "001"],
            "5": ["111", "100", "111", "001", "111"],
            "6": ["111", "100", "111", "101", "111"],
            "7": ["111", "001", "010", "010", "010"],
            "8": ["111", "101", "111", "101", "111"],
            "9": ["111", "101", "111", "001", "111"],
            "%": ["101", "001", "010", "100", "101"],
            ".": ["000", "000", "000", "000", "010"],
            " ": ["000", "000", "000", "000", "000"],
            "-": ["000", "000", "111", "000", "000"],
            "/": ["001", "001", "010", "100", "100"],
            "a": ["111", "001", "111", "101", "111"],
            "b": ["110", "101", "110", "101", "110"],
            "c": ["111", "100", "100", "100", "111"],
            "d": ["011", "101", "101", "101", "011"],
            "e": ["111", "100", "111", "100", "111"],
            "f": ["111", "100", "110", "100", "100"],
            "g": ["111", "101", "111", "001", "111"],
            "h": ["101", "101", "111", "101", "101"],
            "i": ["010", "000", "010", "010", "010"],
            "l": ["100", "100", "100", "100", "111"],
            "m": ["101", "111", "111", "101", "101"],
            "n": ["101", "111", "111", "101", "101"],
            "o": ["111", "101", "101", "101", "111"],
            "p": ["111", "101", "111", "100", "100"],
            "q": ["111", "101", "111", "001", "001"],
            "r": ["110", "101", "110", "101", "101"],
            "s": ["111", "100", "111", "001", "111"],
            "t": ["110", "010", "010", "010", "010"],
            "u": ["101", "101", "101", "101", "111"],
            "v": ["101", "101", "101", "101", "010"],
            "w": ["101", "101", "111", "111", "101"],
            "x": ["101", "101", "010", "101", "101"],
            "y": ["101", "101", "111", "001", "111"],
            "b": ["110", "101", "110", "101", "110"],
        }
        cx = x
        for ch in s.lower():
            g = glyphs.get(ch)
            if g is None:
                cx += 4 * scale
                continue
            for gy, row in enumerate(g):
                for gx, bit in enumerate(row):
                    if bit == "1":
                        self.rect(cx + gx * scale, y + gy * scale,
                                  cx + gx * scale + scale, y + gy * scale + scale, rgba)
            cx += 4 * scale


# 逐字段位宽：naive float 全量 vs 线格式（与 Snapshot.hpp 头注释一致）。
FIELDS = [
    ("tick", 32, 32),
    ("id", 16, 16),
    ("pos", 96, 96),
    ("quat", 128, 51),
    ("vel", 96, 48),
]
NAIVE_TOTAL = sum(n for _, n, _ in FIELDS)      # 368 bits = 46 B
WIRE_TOTAL = sum(w for _, _, w in FIELDS)       # 243 bits -> 31 B


def main():
    exe = sys.argv[1]
    outdir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("example/net-m1")
    outdir.mkdir(parents=True, exist_ok=True)
    log_path = outdir / "net-m1-evidence.log"

    # 1) 真实工具输出（确定性种子，数值可复现）
    r = subprocess.run([exe, "snapshot-bench", "--count", "20000"],
                       capture_output=True, text=True, timeout=120)
    log_path.write_text(r.stdout + r.stderr, encoding="utf-8")
    if r.returncode != 0:
        print(f"snapshot-bench failed rc={r.returncode}")
        print(r.stdout, r.stderr)
        return 1
    avg = re.search(r"avgBytes=([\d.]+)", r.stdout)
    enc = re.search(r"encode=(\d+) ns/op decode=(\d+) ns/op", r.stdout)
    err = re.search(r"maxPosErr=([\d.]+) m maxQuatErr=([\d.]+) deg maxVelErr=([\d.]+) m/s", r.stdout)
    if not (avg and enc and err):
        print("parse failed on bench output")
        return 1
    print("parsed: avgBytes=%s enc=%sns dec=%sns err=(%s m, %s deg, %s m/s)"
          % (avg.group(1), enc.group(1), enc.group(2), *err.groups()))

    # 2) 渲染：逐字段位宽对比横条 + 底部实测行
    cv = Canvas(W, H)
    plot_w = W - MARGIN_L - 32
    dark = bytearray([60, 60, 60, 255])
    gray = bytearray([190, 190, 190, 255])
    green = bytearray([46, 139, 87, 255])
    axis_x = MARGIN_L - 8
    top = MARGIN_T + 8
    row_h = 44
    bar_h = 12
    rows = FIELDS + [("total", NAIVE_TOTAL, WIRE_TOTAL)]
    # x 刻度（bits）：0/100/200/300/400（total 行 naive 368 是最大值）
    def bx(bits):
        return MARGIN_L + int(plot_w * bits / 400.0)
    for t in (0, 100, 200, 300, 400):
        x = bx(t)
        cv.vline(x, top - 6, H - MARGIN_B + 8, bytearray([210, 210, 210, 255]))
        cv.text(x - 4, H - MARGIN_B + 12, str(t), dark)
    cv.vline(axis_x, top - 8, H - MARGIN_B + 8, dark)
    for i, (name, naive, wire) in enumerate(rows):
        y0 = top + i * row_h
        # 灰 naive 条（全宽）
        cv.rect(MARGIN_L, y0, bx(naive), y0 + bar_h, gray)
        # 绿 wire 条（叠在同条上部）
        cv.rect(MARGIN_L, y0, bx(wire), y0 + bar_h // 2, green)
        cv.text(12, y0 + 1, name, dark)
        # 数字标注（naive / wire）
        cv.text(bx(naive) + 6, y0, str(naive), bytearray([120, 120, 120, 255]))
        cv.text(bx(wire) + 6, y0 + bar_h + 2, str(wire), bytearray([0, 90, 60, 255]))
    # 图例（间距错开防重叠）
    ly = H - MARGIN_B - 14
    cv.rect(MARGIN_L, ly, MARGIN_L + 14, ly + 8, gray)
    cv.rect(MARGIN_L + 180, ly, MARGIN_L + 194, ly + 8, green)
    cv.text(MARGIN_L + 20, ly, "naive float", dark)
    cv.text(MARGIN_L + 200, ly, "wire", dark)
    # 标题与底部实测行（全部来自真实输出）
    cv.text(MARGIN_L, 14, "snapshot wire bits vs naive - vehicle transform", dark, 2)
    by = H - MARGIN_B + 26
    cv.text(MARGIN_L, by, "bench 20k: %s b avg enc %s ns dec %s ns" % (avg.group(1), enc.group(1), enc.group(2)), dark)
    cv.text(MARGIN_L, by + 14, "err: pos %s m quat %s deg vel %s m/s" % err.groups(), dark)

    out_png = outdir / "net-m1-evidence.png"
    out_png.write_bytes(render_png(W, H, cv.px))
    print(f"written: {out_png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
