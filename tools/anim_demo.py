#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""anim-demo: 读取 anim-tools fkdump 的 TSV，生成单文件 HTML 骨骼动画演示页。

数据 100% 来自 anim-tools fkdump 真实输出（FK 逐帧世界坐标），页面只做投影与播放，
不伪造任何一帧。输出 example/ma-track/anim-demo.html。
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "nmake", "anim-tools.exe")
OUT = os.path.join(ROOT, "example", "ma-track", "anim-demo.html")

# (资产, clip, 标签, 帧数)
CLIPS = [
    ("assets/models/rigged/Fox.glb", 1, "Fox — Walk (26 bones)", 64),
    ("assets/models/rigged/Fox.glb", 2, "Fox — Run (26 bones)", 64),
    ("assets/models/rigged/CesiumMan.glb", 0, "CesiumMan — Walk (22 bones)", 64),
    ("assets/models/rigged/RiggedFigure.glb", 0, "RiggedFigure — animation (22 bones)", 64),
]


def fkdump(asset: str, clip: int, frames: int) -> dict:
    r = subprocess.run([EXE, "fkdump", asset, str(clip), str(frames)],
                       capture_output=True, text=True, check=True)
    parents, clips_frames = None, []
    duration = 0.0
    for line in r.stdout.splitlines():
        if line.startswith("# fkdump"):
            duration = float(line.split("duration")[1].split()[0])
        elif line.startswith("P "):
            parents = [int(v) for v in line.split()[1:]]
        elif line.startswith("F "):
            vals = [float(v) for v in line.split()[1:]]
            t, rest = vals[0], vals[1:]
            pts = [(rest[i * 3], rest[i * 3 + 1], rest[i * 3 + 2]) for i in range(len(rest) // 3)]
            clips_frames.append({"t": t, "p": pts})
    return {"duration": duration, "parents": parents, "frames": clips_frames}


HTML = """<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>nl-engine 动作系统 — FK 骨骼动画演示（M-A4c）</title>
<style>
  :root { color-scheme: dark; }
  body { background:#101018; color:#d8d8e8; font:14px/1.5 "Segoe UI",system-ui,sans-serif;
         margin:0; padding:24px; }
  h1 { font-size:18px; margin:0 0 4px; }
  .sub { color:#8a8aa8; font-size:12px; margin-bottom:18px; }
  .grid { display:grid; grid-template-columns:1fr 1fr; gap:16px; max-width:1100px; }
  .panel { background:#16162288; border:1px solid #2a2a3c; border-radius:10px; padding:10px; }
  canvas { width:100%; border-radius:6px; background:#0c0c14; display:block; }
  .label { font-size:13px; color:#c8c8e0; margin:8px 2px 2px; }
  .ctrl { margin:0 0 16px; display:flex; gap:14px; align-items:center; max-width:1100px; }
  button { background:#2a2a44; color:#e8e8ff; border:1px solid #444466; border-radius:6px;
           padding:5px 16px; cursor:pointer; font-size:13px; }
  button:hover { background:#3a3a5c; }
  input[type=range] { width:180px; accent-color:#5a7aff; }
  .speed { color:#9a9abc; font-size:12px; min-width:70px; }
  .foot { color:#6a6a88; font-size:11px; margin-top:20px; max-width:1100px; }
</style>
</head>
<body>
<h1>nl-engine 动作系统 — FK 骨骼动画演示</h1>
<div class="sub">M-A1 采样 + M-A4b Skeleton localToModel 前向运动学 · Khronos glTF-Sample-Assets 标准资产 ·
数据由 anim-tools fkdump 真实导出（确定性，两次运行逐位一致），页面仅做投影与播放。</div>
<div class="ctrl">
  <button id="play">暂停</button>
  <button id="view">视角：正视 XY</button>
  <span>速度 <input id="spd" type="range" min="0.25" max="3" step="0.25" value="1"></span>
  <span class="speed" id="spdtxt">1.00×</span>
</div>
<div class="grid" id="grid"></div>
<div class="foot">线段 = 骨骼（父→子），亮点 = 关节，白色点 = 末端骨骼，淡色轨迹 = 末端骨骼整周期世界路径。
循环播放一个 clip 周期。资产：Khronos glTF-Sample-Assets（Fox / CesiumMan / RiggedFigure）。</div>
<script>
const DATA = __DATA__;
const grid = document.getElementById('grid');
let playing = true, speed = 1.0, phase = 0.0, lastTs = null;

DATA.forEach((clip, i) => {
  const d = document.createElement('div'); d.className = 'panel';
  d.innerHTML = `<canvas width="520" height="380"></canvas><div class="label">${clip.label}
     · 周期 ${clip.duration.toFixed(3)}s</div>`;
  grid.appendChild(d);
  clip.canvas = d.querySelector('canvas');
});

let viewAxis = 0;  // 0 = XY（正视）, 1 = ZY（侧视）
function proj(p) { return viewAxis === 0 ? [p[0], p[1]] : [p[2], p[1]]; }

// 等比包围盒：内容以同一像素比例缩放并居中（防 x/y 各自拉伸变形）。
function refit() {
  const pad = 0.08;
  DATA.forEach(clip => {
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    clip.frames.forEach(f => f.p.forEach(p => {
      const [x, y] = proj(p);
      if (x < x0) x0 = x; if (x > x1) x1 = x;
      if (y < y0) y0 = y; if (y > y1) y1 = y;
    }));
    const w = Math.max(x1 - x0, 1e-4), h = Math.max(y1 - y0, 1e-4);
    clip.fit = { x0: x0 - w * pad, y0: y0 - h * pad, w: w * (1 + 2 * pad), h: h * (1 + 2 * pad) };
    clip.traj = clip.frames.map(f => proj(f.p[f.p.length - 1]));
  });
}
refit();

function draw() {
  DATA.forEach(clip => {
    const cv = clip.canvas, ctx = cv.getContext('2d');
    const W = cv.width, H = cv.height;
    ctx.clearRect(0, 0, W, H);
    const f = clip.fit;
    const k = Math.min(W / f.w, H / f.h);
    const ox = (W - f.w * k) * 0.5, oy = (H - f.h * k) * 0.5;
    const px = x => (x - f.x0) * k + ox;
    const py = y => H - ((y - f.y0) * k + oy);
    // 地面参考线
    ctx.strokeStyle = '#26263a'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, py(f.y0)); ctx.lineTo(W, py(f.y0)); ctx.stroke();
    // 末端轨迹
    ctx.strokeStyle = 'rgba(120,150,255,0.25)'; ctx.lineWidth = 1.5;
    ctx.beginPath();
    clip.traj.forEach(([x, y], k) => k ? ctx.lineTo(px(x), py(y)) : ctx.moveTo(px(x), py(y)));
    ctx.stroke();
    // 当前帧骨架
    const fr = clip.frames[Math.min(clip.frames.length - 1,
        Math.floor(phase * clip.frames.length))];
    ctx.strokeStyle = '#5a8aff'; ctx.lineWidth = 2; ctx.lineCap = 'round';
    fr.p.forEach((p, b) => {
      const par = clip.parents[b];
      if (par >= 0) {
        const [x, y] = proj(p), [qx, qy] = proj(fr.p[par]);
        ctx.beginPath();
        ctx.moveTo(px(qx), py(qy));
        ctx.lineTo(px(x), py(y));
        ctx.stroke();
      }
    });
    ctx.fillStyle = '#8ab0ff';
    fr.p.forEach(p => {
      const [x, y] = proj(p);
      ctx.beginPath(); ctx.arc(px(x), py(y), 2.2, 0, 6.284); ctx.fill();
    });
    const [tx, ty] = proj(fr.p[fr.p.length - 1]);
    ctx.fillStyle = '#ffffff';
    ctx.beginPath(); ctx.arc(px(tx), py(ty), 3.6, 0, 6.284); ctx.fill();
  });
}

function tick(ts) {
  if (lastTs != null && playing) {
    // 相位推进：以最长 clip 周期为节拍，各 panel 用自己的 duration 缩放，
    // 简化处理——统一按 4s 循环一格，各 panel 按自身帧数取帧。
    phase = (phase + (ts - lastTs) / 4000 * speed) % 1;
  }
  lastTs = ts;
  draw();
  requestAnimationFrame(tick);
}
document.getElementById('play').onclick = e => {
  playing = !playing; e.target.textContent = playing ? '暂停' : '播放';
};
document.getElementById('spd').oninput = e => {
  speed = parseFloat(e.target.value);
  document.getElementById('spdtxt').textContent = speed.toFixed(2) + '×';
};
document.getElementById('view').onclick = e => {
  viewAxis = 1 - viewAxis;
  e.target.textContent = viewAxis === 0 ? '视角：正视 XY' : '视角：侧视 ZY';
  refit();
};
requestAnimationFrame(tick);
</script>
</body>
</html>
"""


def main() -> int:
    data = []
    for asset, clip, label, frames in CLIPS:
        path = os.path.join(ROOT, asset)
        d = fkdump(path, clip, frames)
        d["label"] = label
        data.append(d)
        print(f"  dumped {label}: {len(d['frames'])} frames, "
              f"{len(d['parents'])} bones, duration {d['duration']:.3f}s")
    html = HTML.replace("__DATA__", json.dumps(data, separators=(",", ":")))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(html)
    print(f"written {OUT} ({os.path.getsize(OUT)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
