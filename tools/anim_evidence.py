#!/usr/bin/env python3
"""anim-tools sample -> evidence PNG (example/ma-track/evidence.png).

纯标准库（zlib + struct）：跑 `anim-tools sample` 拿确定性输出，画双面板折线图。
工具真实输出 -> 图，不手绘示意（M2.1 固化约定）。重复运行逐位一致。

用法: python tools/anim_evidence.py [--exe PATH]
默认 exe: build/nmake/anim-tools.exe（相对仓库根，脚本内部自行定位）。
"""

import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ---- 5x7 位图字体（本图所需字符的紧凑集） ------------------------------------
# 每字符 7 行、每行 5 bit（高位在左）。'?' 兜底。
_GLYPHS = {
    "0": [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
    "1": [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
    "2": [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F],
    "3": [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E],
    "4": [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02],
    "5": [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E],
    "6": [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E],
    "7": [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08],
    "8": [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E],
    "9": [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C],
    ".": [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C],
    ":": [0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00],
    "a": [0x00, 0x0E, 0x01, 0x0D, 0x13, 0x13, 0x0D],
    "b": [0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E],
    "c": [0x00, 0x0E, 0x11, 0x10, 0x10, 0x11, 0x0E],
    "d": [0x01, 0x01, 0x0D, 0x13, 0x13, 0x13, 0x0D],
    "e": [0x00, 0x0E, 0x11, 0x1F, 0x10, 0x11, 0x0E],
    "f": [0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08],
    "g": [0x0F, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E],
    "h": [0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11],
    "i": [0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E],
    "k": [0x10, 0x10, 0x11, 0x12, 0x14, 0x12, 0x11],
    "l": [0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E],
    "m": [0x00, 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11],
    "n": [0x00, 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11],
    "o": [0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E],
    "p": [0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10],
    "r": [0x00, 0x1E, 0x11, 0x10, 0x10, 0x10, 0x10],
    "s": [0x00, 0x0F, 0x10, 0x0E, 0x01, 0x11, 0x0E],
    "t": [0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06],
    "u": [0x00, 0x11, 0x11, 0x11, 0x11, 0x13, 0x0D],
    "v": [0x00, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04],
    "w": [0x00, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A],
    "x": [0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00],
    "y": [0x11, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E],
    "z": [0x00, 0x1F, 0x02, 0x04, 0x08, 0x10, 0x1F],
    " ": [0x00] * 7,
    "/": [0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10],
    "-": [0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00],
    "(": [0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02],
    ")": [0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08],
    "+": [0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00],
    "=": [0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00],
    ">": [0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08],
}

WHITE = (255, 255, 255)
BLACK = (20, 20, 20)
BLUE = (30, 80, 200)
RED = (200, 40, 40)
GREEN = (30, 140, 60)
GRAY = (190, 190, 190)


def draw_text(px, x, y, text, color=BLACK, scale=2):
    for ch in text:
        glyph = _GLYPHS.get(ch.lower(), _GLYPHS["?"] if "?" in _GLYPHS else [0x1F] * 7)
        for row in range(7):
            bits = glyph[row]
            for col in range(5):
                if bits & (0x10 >> col):
                    for dy in range(scale):
                        for dx in range(scale):
                            px.put(x + col * scale + dx, y + row * scale + dy, color)
        x += 6 * scale


def draw_line(px, x0, y0, x1, y1, color):
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    while True:
        px.put(x0, y0, color)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.buf = bytearray(b"\xff" * (w * h * 3))

    def put(self, x, y, color):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 3
            self.buf[i:i + 3] = bytes(color)

    def rect_fill(self, x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.put(x, y, color)

    def write_png(self, path):
        raw = b"".join(b"\x00" + bytes(self.buf[y * self.w * 3:(y + 1) * self.w * 3])
                       for y in range(self.h))
        def chunk(tag, data):
            c = tag + data
            return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
        ihdr = struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
               chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
        path.write_bytes(png)


def parse_sample(text):
    sweep, scenario = [], []
    section = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("# section 1"):
            section = "sweep"
            continue
        if line.startswith("# section 2"):
            section = "scenario"
            continue
        if not line or line.startswith("#") or line.startswith("anim-tools") or line == "param z":
            continue
        parts = line.split()
        if section == "sweep":
            sweep.append((float(parts[0]), float(parts[1])))
        elif section == "scenario":
            scenario.append((int(parts[0]), float(parts[1]), parts[2],
                             float(parts[3]), float(parts[4])))
    return sweep, scenario


def main():
    exe = ROOT / "build" / "nmake" / "anim-tools.exe"
    if "--exe" in sys.argv:
        exe = Path(sys.argv[sys.argv.index("--exe") + 1])
    if not exe.is_file():
        print(f"[ERROR] anim-tools not built: {exe}")
        return 1
    proc = subprocess.run([str(exe), "sample"], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        print(f"[ERROR] anim-tools sample exit {proc.returncode}: {proc.stderr}")
        return 1
    sweep, scenario = parse_sample(proc.stdout)
    if not sweep or not scenario:
        print("[ERROR] empty parse")
        return 1

    W, H = 960, 540
    px = Canvas(W, H)
    px.rect_fill(0, 0, W, 28, (235, 235, 245))
    draw_text(px, 12, 7, "anim-tools sample: blendspace + statemachine (deterministic)")

    # ---- 面板 1：blend space z(param) ---------------------------------------
    p1x0, p1x1, p1y0, p1y1 = 70, 460, 60, 250       # 绘图区（y0 顶 y1 底）
    px.rect_fill(p1x0, p1y0, p1x1, p1y1, (248, 248, 252))
    for gx in range(p1x0, p1x1 + 1, 65):
        draw_line(px, gx, p1y0, gx, p1y1, GRAY)
    for gy in range(p1y0, p1y1 + 1, 48):
        draw_line(px, p1x0, gy, p1x1, gy, GRAY)
    draw_line(px, p1x0, p1y1, p1x1, p1y1, BLACK)
    draw_line(px, p1x0, p1y0, p1x0, p1y1, BLACK)
    draw_text(px, p1x0, p1y1 + 10, "0", scale=1)
    draw_text(px, p1x1 - 20, p1y1 + 10, "2", scale=1)
    draw_text(px, p1x0, p1y1 + 26, "param (m/s) -> hips z at t=0.5", scale=1)
    zmax = 2.0
    prev = None
    for param, z in sweep:
        sx = p1x0 + int((param / 2.0) * (p1x1 - p1x0))
        sy = p1y1 - int((z / zmax) * (p1y1 - p1y0))
        if prev:
            draw_line(px, prev[0], prev[1], sx, sy, BLUE)
        prev = (sx, sy)
    draw_text(px, p1x0 + 8, p1y0 + 6, "z (m)", scale=1)

    # ---- 面板 2：状态机场景 z(step) + 状态色带 + alpha ------------------------
    p2x0, p2x1, p2y0, p2y1 = 70, 900, 310, 480
    px.rect_fill(p2x0, p2y0, p2x1, p2y1, (248, 248, 252))
    state_colors = {"idle": (200, 200, 210), "walk": (215, 230, 200), "run": (240, 215, 200)}
    step_w = (p2x1 - p2x0) / 10.0
    for i, (_, _, state, _, _) in enumerate(scenario):
        px.rect_fill(p2x0 + int(i * step_w), p2y0,
                     p2x0 + int((i + 1) * step_w), p2y1, state_colors[state])
    for gx in range(p2x0, p2x1 + 1, int(step_w)):
        draw_line(px, gx, p2y0, gx, p2y1, GRAY)
    draw_line(px, p2x0, p2y1, p2x1, p2y1, BLACK)
    draw_line(px, p2x0, p2y0, p2x0, p2y1, BLACK)
    prev = None
    for i, (_, _, _, alpha, z) in enumerate(scenario):
        cx = p2x0 + int((i + 0.5) * step_w)
        cy = p2y1 - int((z / 4.0) * (p2y1 - p2y0))
        if prev:
            draw_line(px, prev[0], prev[1], cx, cy, RED)
        prev = (cx, cy)
        ay = p2y1 - int(alpha * (p2y1 - p2y0))
        px.put(cx, ay, GREEN)
        px.put(cx, ay + 1, GREEN)
        px.put(cx, ay - 1, GREEN)
        draw_text(px, cx - 6, p2y1 + 8, str(i), scale=1)
    draw_text(px, p2x0, p2y1 + 24, "step (dt=0.1s, speed input) -> hips z (red),", scale=1)
    draw_text(px, p2x0, p2y1 + 38, "alpha (green dots); bands = idle/walk/run", scale=1)
    draw_text(px, p2x0, p2y0 + 6, "step6 revert continuity: z stays 0.7", scale=1)

    # 图例
    draw_line(px, 700, 66, 730, 66, BLUE)
    draw_text(px, 736, 60, "blendspace z", scale=1)
    draw_line(px, 700, 82, 730, 82, RED)
    draw_text(px, 736, 76, "statemachine z", scale=1)
    px.put(715, 98, GREEN); px.put(715, 97, GREEN)
    draw_text(px, 736, 92, "transition alpha", scale=1)

    out = ROOT / "example" / "ma-track" / "evidence.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    px.write_png(out)
    print(f"written: {out.relative_to(ROOT)} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
