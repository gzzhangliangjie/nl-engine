#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""skin-demo: 读取 anim-tools skindump 的 TSV，生成单文件 HTML 蒙皮动画演示页。

数据 100% 来自 anim-tools skindump 真实输出（CPU LBS 逐帧蒙皮顶点，与 viewer
--mode skin 同口径：palette=model*IBM、t=0 蒙皮输出包围盒中心平移），页面只做
投影 / 着色 / 播放，不伪造任何一帧。输出 example/ma-track/skin-demo.html。
四资产四面板（与 anim-demo.html 的 FK 面板对齐）：CesiumMan Walk / Fox Walk /
Fox Run / RiggedFigure。
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "nmake", "anim-tools.exe")
OUT = os.path.join(ROOT, "example", "ma-track", "skin-demo.html")

# (资产, clip, 标签, 帧数) —— 与 anim-demo 的 FK 面板同资产同 clip
CLIPS = [
    ("assets/models/rigged/CesiumMan.glb", 0, "CesiumMan — Walk（22 骨）", 28),
    ("assets/models/rigged/Fox.glb", 1, "Fox — Walk（26 骨，四足）", 28),
    ("assets/models/rigged/Fox.glb", 2, "Fox — Run（26 骨，四足）", 28),
    ("assets/models/rigged/RiggedFigure.glb", 0, "RiggedFigure — animation（22 骨）", 28),
]


def skindump(asset: str, clip: int, frames: int) -> dict:
    r = subprocess.run([EXE, "skindump", asset, str(clip), str(frames)],
                       capture_output=True, text=True, check=True)
    idx, dom, frames_data = None, None, []
    duration = 0
    for line in r.stdout.splitlines():
        if line.startswith("# skindump"):
            duration = float(line.split("duration")[1].split()[0])
        elif line.startswith("IDX "):
            idx = [int(v) for v in line.split()[1:]]
        elif line.startswith("DOM "):
            dom = [int(v) for v in line.split()[1:]]
        elif line.startswith("F "):
            vals = [float(v) for v in line.split()[1:]]
            frames_data.append(vals[1:])  # 扁平 [x,y,z, x,y,z, ...]
    assert idx and dom and frames_data, "skindump output incomplete"
    return {"duration": duration, "idx": idx, "dom": dom,
            "nbones": max(dom) + 1, "verts": len(dom),
            "tris": len(idx) // 3, "frames": frames_data}


HTML = r"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>nl-engine 动作系统 — 蒙皮动画演示（M-A5）</title>
<style>
  :root { color-scheme: dark; }
  body { background:#101018; color:#d8d8e8; font:14px/1.5 "Segoe UI",system-ui,sans-serif;
         margin:0; padding:24px; }
  h1 { font-size:18px; margin:0 0 4px; }
  .sub { color:#8a8aa8; font-size:12px; margin-bottom:14px; }
  .grid { display:grid; grid-template-columns:1fr 1fr; gap:16px; max-width:1240px; }
  .panel { background:#16162288; border:1px solid #2a2a3c; border-radius:10px; padding:10px; }
  canvas { width:100%; border-radius:6px; background:#0c0c14; display:block;
           cursor:grab; touch-action:none; }
  canvas.drag { cursor:grabbing; }
  .label { font-size:13px; color:#c8c8e0; margin:8px 2px 2px; }
  .meta { font-size:11px; color:#8a8aa8; margin:0 2px; }
  .ctrl { display:flex; gap:10px; align-items:center; margin:0 0 16px; max-width:1240px; }
  button { background:#2a2a44; color:#e8e8ff; border:1px solid #444466; border-radius:6px;
           padding:5px 14px; cursor:pointer; font-size:13px; }
  button:hover { background:#3a3a5c; }
  button.on { background:#5a5aa8; }
</style>
</head>
<body>
<h1>nl-engine 动作系统 — 蒙皮动画演示（M-A5）</h1>
<div class="sub">数据来自 anim-tools skindump（CPU LBS：palette = model × IBM，逐帧蒙皮顶点）——页面只做投影与播放，不伪造任何一帧；拖拽画布旋转视角，滚轮缩放</div>
<div class="ctrl">
  <button id="play">⏸ 暂停</button>
  <button data-sp="0.5">0.5×</button>
  <button data-sp="1" class="on">1×</button>
  <button data-sp="2">2×</button>
  <span style="color:#8a8aa8;font-size:12px">着色 = 主导关节槽位，明暗 = 面法线兰伯特</span>
</div>
<div class="grid" id="grid"></div>
<script>
const CLIPS = __DATA__;
let tGlobal = 0, playing = true, speed = 1;
{
  const m = Object.fromEntries(location.hash.slice(1).split('&').map(s => s.split('=')));
  if (m.t) tGlobal = parseFloat(m.t);
  if (m.pause === '1') playing = false;
}

const grid = document.getElementById('grid');
const panels = CLIPS.map(c => {
  const div = document.createElement('div'); div.className = 'panel';
  const cv = document.createElement('canvas'); cv.width = 600; cv.height = 440;
  const lb = document.createElement('div'); lb.className = 'label'; lb.textContent = c.label;
  const mt = document.createElement('div'); mt.className = 'meta';
  mt.textContent = `${c.verts} 顶点 / ${c.tris} 三角 / ${c.nbones} 骨 / ${c.frames.length} 帧 · 时长 ${c.duration.toFixed(2)}s`;
  div.append(cv, lb, mt); grid.append(div);
  // 包围半径：全帧全局最大分量
  let R = 0.1;
  for (const f of c.frames)
    for (let i = 0; i < f.length; i += 3)
      R = Math.max(R, Math.abs(f[i]), Math.abs(f[i+1]), Math.abs(f[i+2]));
  // 主导关节 → 色相
  const hue = c.dom.map(s => (Math.max(0, s) * 360 / c.nbones + 20) % 360);
  const p = { c, ctx: cv.getContext('2d'), cv, yaw: 0.6, pitch: 0.18, zoom: 1.0, R, hue };
  // 每画布独立轨道拖拽 + 滚轮缩放
  let drag = false, lx = 0, ly = 0;
  cv.addEventListener('mousedown', e => { drag = true; lx = e.clientX; ly = e.clientY; cv.classList.add('drag'); });
  window.addEventListener('mouseup', () => { drag = false; cv.classList.remove('drag'); });
  window.addEventListener('mousemove', e => {
    if (!drag) return;
    p.yaw -= (e.clientX - lx) * 0.008; p.pitch += (e.clientY - ly) * 0.006;
    p.pitch = Math.max(-1.2, Math.min(1.2, p.pitch)); lx = e.clientX; ly = e.clientY;
  });
  cv.addEventListener('wheel', e => {
    e.preventDefault();
    p.zoom = Math.max(0.4, Math.min(3.0, p.zoom * (e.deltaY < 0 ? 1.12 : 0.89)));
  }, {passive: false});
  return p;
});

const playBtn = document.getElementById('play');
playBtn.onclick = () => { playing = !playing; playBtn.textContent = playing ? '⏸ 暂停' : '▶ 播放'; };
document.querySelectorAll('[data-sp]').forEach(b => {
  b.onclick = () => {
    document.querySelectorAll('[data-sp]').forEach(x => x.classList.remove('on'));
    b.classList.add('on'); speed = +b.dataset.sp;
  };
});

function lerpFrame(fA, fB, u, out) {
  for (let i = 0; i < fA.length; ++i) out[i] = fA[i] + (fB[i] - fA[i]) * u;
}

function drawPanel(p, W, H) {
  const c = p.c, ctx = p.ctx;
  const t = tGlobal % c.duration;
  const fu = t / c.duration * c.frames.length;
  const i0 = Math.floor(fu) % c.frames.length, i1 = (i0 + 1) % c.frames.length;
  const u = fu - Math.floor(fu);
  const P = new Float32Array(c.frames[0].length);
  lerpFrame(c.frames[i0], c.frames[i1], u, P);

  const cy = Math.cos(p.yaw), sy = Math.sin(p.yaw), cp = Math.cos(p.pitch), sp = Math.sin(p.pitch);
  const fwd = [sy*cp, sp, -cy*cp], right = [cy, 0, sy], up = [-sy*sp, cp, cy*sp];
  const scale = Math.min(W, H) * 0.40 / p.R * p.zoom;
  const cx0 = W/2, cy0 = H/2 + H*0.05;

  const n = P.length / 3;
  const sx = new Float32Array(n), syv = new Float32Array(n), dz = new Float32Array(n);
  for (let i = 0; i < n; ++i) {
    const x = P[i*3], y = P[i*3+1], z = P[i*3+2];
    sx[i]  = cx0 + (x*right[0] + y*right[1] + z*right[2]) * scale;
    syv[i] = cy0 - (x*up[0] + y*up[1] + z*up[2]) * scale;
    dz[i]  = x*fwd[0] + y*fwd[1] + z*fwd[2];
  }
  const T = c.idx, NT = T.length / 3;
  const order = new Array(NT), depth = new Float32Array(NT);
  for (let k = 0; k < NT; ++k) {
    order[k] = k;
    depth[k] = (dz[T[k*3]] + dz[T[k*3+1]] + dz[T[k*3+2]]) / 3;
  }
  order.sort((a, b) => depth[b] - depth[a]);

  const L = [0.45, 0.78, 0.42];
  ctx.fillStyle = '#0c0c14';
  ctx.fillRect(0, 0, W, H);
  const hue = p.hue;
  for (const k of order) {
    const a = T[k*3], b = T[k*3+1], g = T[k*3+2];
    const ax = P[a*3], ay = P[a*3+1], az = P[a*3+2];
    const bx = P[b*3], by = P[b*3+1], bz = P[b*3+2];
    const gx = P[g*3], gy = P[g*3+1], gz = P[g*3+2];
    const e1x = bx-ax, e1y = by-ay, e1z = bz-az;
    const e2x = gx-ax, e2y = gy-ay, e2z = gz-az;
    let nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
    const nl = Math.hypot(nx, ny, nz) || 1;
    let lam = (nx*L[0] + ny*L[1] + nz*L[2]) / nl;
    lam = 0.32 + 0.62 * Math.abs(lam);
    ctx.fillStyle = `hsl(${hue[a]},52%,${(lam*62)|0}%)`;
    ctx.beginPath();
    ctx.moveTo(sx[a], syv[a]); ctx.lineTo(sx[b], syv[b]); ctx.lineTo(sx[g], syv[g]);
    ctx.closePath(); ctx.fill();
  }
  // 地面参考圈
  ctx.strokeStyle = '#2a2a44'; ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.ellipse(cx0, cy0 + p.R*scale, p.R*scale*Math.abs(cy), p.R*scale*0.22, 0, 0, Math.PI*2);
  ctx.stroke();
}

function frame() {
  requestAnimationFrame(frame);
  if (playing) tGlobal += (1/60) * speed;
  for (const p of panels) drawPanel(p, p.cv.width, p.cv.height);
}
frame();
</script>
</body>
</html>
"""

def main() -> int:
    clips = []
    for asset, ci, label, frames in CLIPS:
        c = skindump(asset, ci, frames)
        c["label"] = label
        clips.append(c)
    payload = json.dumps(clips, separators=(",", ":"))
    html = HTML.replace("__DATA__", payload)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(html)
    total = sum(len(c["frames"]) for c in clips)
    print(f"skin-demo: {OUT} ({len(html)/1e6:.2f} MB, {len(clips)} panels, {total} frames)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
