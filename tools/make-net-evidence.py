#!/usr/bin/env python3
"""NET M-NET0 证据图生成器：跑一次真实的 `net-tools loss-sweep`（10 级丢包注入实测），
把 stdout 解析后渲染为 PNG 柱状图（纯标准库 zlib/struct，无 matplotlib/PIL 依赖）。
产物 = example/net-m0/net-m0-evidence.png + 同目录 net-m0-evidence.log（原始输出留档）。
数据 100% 来自工具真实输出，无任何手绘/示意成分。
用法：python tools/make-net-evidence.py <net-tools.exe 路径> [输出目录]
"""
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

W, H = 720, 400
MARGIN_L, MARGIN_B, MARGIN_T = 64, 48, 36


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
        # 3x5 微型数字/字符点阵（.: %/recvlost 0-9 与少量字母）
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
            "l": ["100", "100", "100", "100", "111"],
            "o": ["111", "101", "101", "101", "111"],
            "s": ["111", "100", "111", "001", "111"],
            "s2": ["011", "100", "010", "001", "110"],
            "r": ["110", "101", "100", "100", "100"],
            "e": ["111", "100", "111", "100", "111"],
            "c": ["111", "100", "100", "100", "111"],
            "v": ["101", "101", "101", "101", "010"],
            "n": ["101", "111", "111", "101", "101"],
            "t": ["110", "010", "010", "010", "010"],
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


def main():
    exe = sys.argv[1]
    outdir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("example/net-m0")
    outdir.mkdir(parents=True, exist_ok=True)
    log_path = outdir / "net-m0-evidence.log"

    # 1) 真实工具输出
    r = subprocess.run([exe, "loss-sweep"], capture_output=True, text=True, timeout=120)
    log_path.write_text(r.stdout + r.stderr, encoding="utf-8")
    if r.returncode != 0:
        print(f"loss-sweep failed rc={r.returncode}")
        print(r.stdout, r.stderr)
        return 1
    levels = [(int(m[0]), int(m[1]), int(m[2]))
              for m in re.findall(r"loss\s+(\d+)%\s+: recv (\d+)/(\d+)", r.stdout)]
    if len(levels) != 10:
        print(f"parse failed: {len(levels)} levels in output")
        return 1
    print("parsed:", levels)

    # 2) 渲染柱状图
    cv = Canvas(W, H)
    plot_w = W - MARGIN_L - 24
    plot_h = H - MARGIN_B - MARGIN_T
    baseline = H - MARGIN_B
    dark = bytearray([60, 60, 60, 255])
    gray = bytearray([190, 190, 190, 255])
    green = bytearray([46, 139, 87, 255])
    # 坐标轴
    cv.vline(MARGIN_L - 8, MARGIN_T - 8, baseline + 8, dark)
    cv.hline(MARGIN_L - 8, W - 20, baseline + 8, dark)
    # sent 基准线（满 32）与 1/2、0 参考刻度
    for frac, label in ((1.0, "32"), (0.5, "16"), (0.0, "0")):
        y = baseline - int(plot_h * frac)
        cv.hline(MARGIN_L - 12, W - 20, y, bytearray([200, 200, 200, 255]))
        cv.text(12, y - 5, label, dark)
    bar_w = plot_w // len(levels)
    for i, (loss, recv, total) in enumerate(levels):
        x0 = MARGIN_L + i * bar_w + 6
        x1 = MARGIN_L + (i + 1) * bar_w - 6
        # 灰色 sent 全高
        cv.rect(x0, baseline - plot_h, x1, baseline, gray)
        # 绿色 recv 实测
        h_recv = int(plot_h * recv / total)
        cv.rect(x0, baseline - h_recv, x1, baseline, green)
        cv.text(x0 + 2, baseline + 10, f"{loss}%", dark)
        cv.text(x0 + 2, baseline - h_recv - 16, str(recv),
                bytearray([0, 90, 60, 255]))
    # 标题与图例（ASCII 点阵，避免任何字体依赖；标题只用点阵里有的字形）
    cv.text(MARGIN_L, 14, "net tools recv vs loss 0..90", dark, 2)
    cv.rect(W - 220, baseline - plot_h + 4, W - 204, baseline - plot_h + 20, gray)
    cv.rect(W - 140, baseline - plot_h + 4, W - 124, baseline - plot_h + 20, green)
    cv.text(W - 200, baseline - plot_h + 6, "sent", dark)
    cv.text(W - 120, baseline - plot_h + 6, "recv", dark)

    out_png = outdir / "net-m0-evidence.png"
    out_png.write_bytes(render_png(W, H, cv.px))
    print(f"written: {out_png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
