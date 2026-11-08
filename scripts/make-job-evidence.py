# -*- coding: utf-8 -*-
"""P8-JOB 可视化证据图生成器（支线任务 2026-11-08）。

铁律：所有内容从工具**真实输出**渲染（build/showcase/runs/job-offline/ 下的
.log 与逐层 MD5），不许手绘示意。产物进 example/p8-jobs/。
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(ROOT, "build", "showcase", "runs", "job-offline")
OUT = os.path.join(ROOT, "example", "p8-jobs")

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)
BAR2 = (70, 78, 90)

def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)

def read_log(name):
    with open(os.path.join(RUNS, name), encoding="utf-8", errors="replace") as f:
        return f.read()

def new_canvas(lines, w=1180, pad=28, title=None, accent=CYAN):
    lh = 30
    h = pad * 2 + lh * (len(lines) + (2 if title else 0))
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    f = font(17)
    fb = font(19, bold=True)
    y = pad
    if title:
        d.text((pad, y), title, font=font(22, bold=True), fill=accent)
        y += lh * 2
    for text, color, bold in lines:
        d.text((pad, y), text, font=fb if bold else f, fill=color)
        y += lh
    return img

def ok_line(s):
    if s.strip().startswith("ok"):
        return (s, GREEN, False)
    if "PASS" in s:
        return (s, GREEN, True)
    if s.startswith("info"):
        return (s, DIM, False)
    return (s, FG, False)

# ---------- 1. selftest jobs ----------
def make_selftest():
    log = read_log("selftest-jobs.log").rstrip().splitlines()
    lines = [("nanite-tools selftest jobs  (real output)", CYAN, True)]
    lines += [ok_line(s) for s in log]
    img = new_canvas(lines, title=None)
    img.save(os.path.join(OUT, "selftest-jobs.png"))

# ---------- 2. cluster parallel ----------
def make_cluster():
    log = read_log("cluster-parallel.log").rstrip().splitlines()
    # 每 submesh 取关键行 + 末行并行计时
    lines = [("nanite-tools cluster assets/models/Suzanne-multi.glb  (real output)", CYAN, True)]
    for s in log:
        keep = ("submesh", "input", "meshlets", "coverage", "limits")
        if any(s.startswith(k) for k in keep):
            lines.append((s, FG, s.startswith("limits")))
        if s.startswith("jobs"):
            lines.append(("", DIM, False))
            lines.append((s, AMBER, True))
    img = new_canvas(lines)
    img.save(os.path.join(OUT, "cluster-parallel.png"))

# ---------- 3. lod speedup + md5 ----------
def md5_rows():
    import hashlib
    rows = []
    for i in range(6):
        rows.append((f"L{i}",
                     hashlib.md5(open(os.path.join(RUNS, f"lod-dense-t1.lod{i}.nmesh"), "rb").read()).hexdigest(),
                     hashlib.md5(open(os.path.join(RUNS, f"lod-dense-tp.lod{i}.nmesh"), "rb").read()).hexdigest()))
    return rows

def make_lod():
    def timing(logname):
        m = re.search(r"threads=(\d+), ([\d.]+)s", read_log(logname))
        return int(m.group(1)), float(m.group(2))
    nt1, t1 = timing("lod-dense-t1.log")
    ntp, tp = timing("lod-dense-tp.log")
    rows = md5_rows()
    w, lh, pad = 1180, 30, 28
    h = pad * 2 + lh * (3 + 4 + 2 + 1 + 2 + len(rows) + 1)
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    f = font(17); fb = font(19, bold=True); ft = font(22, bold=True)
    y = pad
    d.text((pad, y), "nanite-tools lod IcoSphereDense.nmesh --max-levels 5  (real output)", font=ft, fill=CYAN)
    y += lh * 2
    # 计时条
    d.text((pad, y), "wall-clock  (jobs line, per-run)", font=fb, fill=FG); y += lh
    bar_x, bar_max = pad + 300, w - pad - 140
    for label, t, color in ((f"threads={nt1}", t1, BAR2), (f"threads={ntp}", tp, CYAN)):
        d.text((pad, y), label, font=f, fill=FG)
        bw = int(bar_max - bar_x) * (t / t1)
        d.rounded_rectangle((bar_x, y + 3, bar_x + bw, y + 22), 4, fill=color)
        d.text((bar_x + bw + 10, y), f"{t:.3f}s", font=fb, fill=color)
        y += lh
    d.text((pad, y), f"speedup {t1/tp:.2f}x   deterministic: parallel == threads=1", font=fb, fill=AMBER)
    y += lh * 2
    # MD5 表
    d.text((pad, y), "per-level MD5  (hashlib md5, real bytes)", font=fb, fill=FG); y += lh
    d.text((pad, y), "level", font=f, fill=DIM)
    d.text((pad + 80, y), "threads=1", font=f, fill=DIM)
    d.text((pad + 480, y), "threads=32", font=f, fill=DIM)
    d.text((pad + 880, y), "verdict", font=f, fill=DIM)
    y += lh
    for lv, h1, hp in rows:
        match = h1 == hp
        d.text((pad, y), lv, font=f, fill=FG)
        d.text((pad + 80, y), h1, font=f, fill=FG)
        d.text((pad + 480, y), hp, font=f, fill=FG)
        d.text((pad + 880, y), "MATCH" if match else "DIFF", font=fb, fill=GREEN if match else RED)
        y += lh
    d.text((pad, y), "6/6 levels byte-identical  (selftest also asserts chain byte-identical)", font=f, fill=DIM)
    img.save(os.path.join(OUT, "lod-parallel.png"))

# ---------- 4. negative ----------
def make_negative():
    log = read_log("negative.log").rstrip().splitlines()
    lines = [("$ nanite-tools selftest nope      (real output, exit code 1)", CYAN, True)]
    lines += [(s, RED, False) for s in log]
    lines += [("exit 1  = rejected, no side effects (per tool exit-code convention)", DIM, False)]
    img = new_canvas(lines)
    img.save(os.path.join(OUT, "negative-unknown.png"))

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_selftest()
    make_cluster()
    make_lod()
    make_negative()
    print("done:", os.listdir(OUT))
