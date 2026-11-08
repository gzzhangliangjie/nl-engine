#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""skin-demo: 读取 anim-tools skindump 的 TSV，生成单文件 HTML 蒙皮动画演示页。

数据 100% 来自 anim-tools skindump 真实输出（CPU LBS 逐帧蒙皮顶点，与 viewer
--mode skin 同口径：palette=model*IBM、t=0 蒙皮输出包围盒中心平移），页面只做
投影 / 着色 / 播放，不伪造任何一帧。输出 example/ma-track/skin-demo.html。
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "nmake", "anim-tools.exe")
OUT = os.path.join(ROOT, "example", "ma-track", "skin-demo.html")

# (资产, clip, 标签, 帧数)
CLIPS = [
    ("assets/models/rigged/CesiumMan.glb", 0, "CesiumMan — Walk（CPU LBS 蒙皮，22 骨）", 36),
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
            t, rest = vals[0], vals[1:]
            frames_data.append(rest)  # 扁平 [x,y,z, x,y,z, ...]
    assert idx and dom and frames_data, "skindump output incomplete"
    nbones = max(dom) + 1
    return {"duration": duration, "idx": idx, "dom": dom,
            "nbones": nbones, "frames": frames_data}


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
  .stage { background:#16162288; border:1px solid #2a2a3c; border-radius:10px;
           padding:12px; max-width:960px; }
  canvas { width:100%; border-radius:6px; background:#0c0c14; display:block;
           cursor:grab; touch-action:none; }
  canvas.drag { cursor:grabbing; }
  .ctrl { display:flex; gap:10px; align-items:center; flex-wrap:wrap; margin-top:10px; }
  button { background:#2a2a44; color:#e8e8ff; border:1px solid #444466; border-radius:6px;
           padding:5px 14px; cursor:pointer; font-size:13px; }
  button:hover { background:#3a3a5c; }
  button.on { background:#5a5aa8; }
  input[type=range] { flex:1; min-width:160px; accent-color:#7a7ad0; }
  .info { font-size:12px; color:#8a8aa8; margin-top:8px; }
  .legend { font-size:11px; color:#8a8aa8; margin-top:2px; }
  .sw { display:inline-block; width:10px; height:10px; border-radius:2px; margin:0 3px 0 10px;
        vertical-align:-1px; }
</style>
</head>
<body>
<h1>nl-engine 动作系统 — 蒙皮动画演示（M-A5）</h1>
<div class="sub">数据来自 anim-tools skindump（CPU LBS：palette = model × IBM，逐帧蒙皮顶点）——页面只做投影与播放，不伪造任何一帧</div>
<div class="stage">
  <canvas id="cv" width="920" height="600"></canvas>
  <div class="ctrl">
    <button id="play">⏸ 暂停</button>
    <button data-sp="0.5">0.5×</button>
    <button data-sp="1" class="on">1×</button>
    <button data-sp="2">2×</button>
    <span style="width:14px"></span>
    <button data-view="0" class="on">45°</button>
    <button data-view="1">正面</button>
    <button data-view="2">侧面</button>
    <input id="scrub" type="range" min="0" max="1000" value="0">
  </div>
  <div class="info" id="info"></div>
  <div class="legend">着色 = 主导关节槽位（关节 0 → <span id="hues"></span> → 关节 <span id="nbn"></span>），明暗 = 面法线兰伯特</div>
</div>
<script>
const DATA = __DATA__;
const cv = document.getElementById('cv'), ctx = cv.getContext('2d');
const W = cv.width, H = cv.height, NF = DATA.frames.length;

// 相机：yaw/pitch 轨道 + 正交投影；数据已居中（skindump 输出口径）
let yaw = 0.6, pitch = 0.18, zoom = 1.0, tPlay = 0, playing = true, speed = 1;

// 调试入口：#t=1.2&pause=1 —— 固定时刻截图验证
{
  const m = Object.fromEntries(location.hash.slice(1).split('&').map(s => s.split('=')));
  if (m.t) tPlay = parseFloat(m.t) % DATA.duration;
  if (m.pause === '1') playing = false;
}

// 包围半径：取全部帧全局最大范围，保证任何姿态不出画
let R = 0.1;
for (const f of DATA.frames) {
  for (let i = 0; i < f.length; i += 3) {
    R = Math.max(R, Math.abs(f[i]), Math.abs(f[i+1]), Math.abs(f[i+2]));
  }
}

function lerpFrame(fA, fB, u) {
  const out = new Float32Array(fA.length);
  for (let i = 0; i < fA.length; ++i) out[i] = fA[i] + (fB[i] - fA[i]) * u;
  return out;
}

// 关节槽位 → 色相（hue 环形展开，相邻骨分色明显）
function jointHue(slot) {
  return (slot * 360 / DATA.nbones + 20) % 360;
}
// 预生成 DOM 每顶点基色字符串（hsl 亮度 1.0，绘制时乘兰伯特）
const DOMC = DATA.dom.map(s => jointHue(Math.max(0, s)));

let hueSwatches = '';
for (let k = 0; k < 5; ++k) {
  hueSwatches += `<span class="sw" style="background:hsl(${(k*360/DATA.nbones+20)%360},55%,55%)"></span>`;
}
document.getElementById('hues').outerHTML = hueSwatches;
document.getElementById('nbn').textContent = DATA.nbones - 1;
document.getElementById('info').textContent =
  `${DATA.label} · ${DATA.verts} 顶点 / ${DATA.idx.length/3} 三角 / ${NF} 帧 · 时长 ${DATA.duration.toFixed(2)}s · 拖拽旋转 · 滚轮缩放`;

// 视角预设
const VIEWS = [[0.6, 0.18], [0.0, 0.06], [Math.PI/2, 0.06]];
document.querySelectorAll('[data-view]').forEach(b => {
  b.onclick = () => {
    document.querySelectorAll('[data-view]').forEach(x => x.classList.remove('on'));
    b.classList.add('on');
    const v = VIEWS[+b.dataset.view]; yaw = v[0]; pitch = v[1];
  };
});

// 播放控制
const playBtn = document.getElementById('play'), scrub = document.getElementById('scrub');
playBtn.onclick = () => { playing = !playing; playBtn.textContent = playing ? '⏸ 暂停' : '▶ 播放'; };
document.querySelectorAll('[data-sp]').forEach(b => {
  b.onclick = () => {
    document.querySelectorAll('[data-sp]').forEach(x => x.classList.remove('on'));
    b.classList.add('on'); speed = +b.dataset.sp;
  };
});
let scrubbing = false;
scrub.oninput = () => { tPlay = (+scrub.value / 1000) * DATA.duration; };
scrub.onmousedown = () => scrubbing = true;
window.addEventListener('mouseup', () => scrubbing = false);

// 轨道拖拽
let dragging = false, lx = 0, ly = 0;
cv.addEventListener('mousedown', e => { dragging = true; lx = e.clientX; ly = e.clientY; cv.classList.add('drag'); });
window.addEventListener('mouseup', () => { dragging = false; cv.classList.remove('drag'); });
window.addEventListener('mousemove', e => {
  if (!dragging) return;
  yaw -= (e.clientX - lx) * 0.008; pitch += (e.clientY - ly) * 0.006;
  pitch = Math.max(-1.2, Math.min(1.2, pitch)); lx = e.clientX; ly = e.clientY;
});
cv.addEventListener('wheel', e => {
  e.preventDefault();
  zoom = Math.max(0.4, Math.min(3.0, zoom * (e.deltaY < 0 ? 1.12 : 0.89)));
}, {passive: false});

function draw() {
  requestAnimationFrame(draw);
  const dt = 1/60;
  if (playing && !scrubbing) {
    tPlay = (tPlay + dt * speed) % DATA.duration;
  }
  scrub.value = Math.round(tPlay / DATA.duration * 1000);

  // 帧插值（相邻 skindump 帧线性 lerp，播放平滑）
  const fu = tPlay / DATA.duration * NF;
  const i0 = Math.floor(fu) % NF, i1 = (i0 + 1) % NF, u = fu - Math.floor(fu);
  const P = lerpFrame(DATA.frames[i0], DATA.frames[i1], u);

  // 相机基向量（正交）
  const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
  const fwd = [sy*cp, sp, -cy*cp];            // 眼睛看向 -fwd
  const right = [cy, 0, sy];
  const up = [-sy*sp, cp, cy*sp];
  const scale = Math.min(W, H) * 0.40 / R * zoom;
  const cx0 = W/2, cy0 = H/2 + H*0.06;

  // 投影全部顶点
  const n = P.length / 3;
  const sx = new Float32Array(n), syv = new Float32Array(n), dz = new Float32Array(n);
  for (let i = 0; i < n; ++i) {
    const x = P[i*3], y = P[i*3+1], z = P[i*3+2];
    sx[i]   = cx0 + (x*right[0] + y*right[1] + z*right[2]) * scale;
    syv[i]  = cy0 - (x*up[0] + y*up[1] + z*up[2]) * scale;
    dz[i]   = x*fwd[0] + y*fwd[1] + z*fwd[2];
  }

  // 三角形按深度排序（painter's algorithm，远→近）
  const T = DATA.idx, NT = T.length / 3;
  const order = new Array(NT), depth = new Float32Array(NT);
  for (let k = 0; k < NT; ++k) {
    order[k] = k;
    depth[k] = (dz[T[k*3]] + dz[T[k*3+1]] + dz[T[k*3+2]]) / 3;
  }
  order.sort((a, b) => depth[b] - depth[a]);

  // 光照方向（世界系）
  const L = [0.45, 0.78, 0.42];
  ctx.fillStyle = '#0c0c14';
  ctx.fillRect(0, 0, W, H);

  for (const k of order) {
    const a = T[k*3], b = T[k*3+1], c = T[k*3+2];
    const ax = P[a*3], ay = P[a*3+1], az = P[a*3+2];
    const bx = P[b*3], by = P[b*3+1], bz = P[b*3+2];
    const gx = P[c*3], gy = P[c*3+1], gz = P[c*3+2];
    // 面法线
    const e1x = bx-ax, e1y = by-ay, e1z = bz-az;
    const e2x = gx-ax, e2y = gy-ay, e2z = gz-az;
    let nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
    const nl = Math.hypot(nx, ny, nz) || 1;
    nx/=nl; ny/=nl; nz/=nl;
    // 双面兰伯特（朝向光面亮、背光面 ambient）
    let lam = nx*L[0] + ny*L[1] + nz*L[2];
    lam = 0.32 + 0.62 * Math.abs(lam);
    const hue = DOMC[a];
    ctx.fillStyle = `hsl(${hue},52%,${(lam*62)|0}%)`;
    ctx.beginPath();
    ctx.moveTo(sx[a], syv[a]);
    ctx.lineTo(sx[b], syv[b]);
    ctx.lineTo(sx[c], syv[c]);
    ctx.closePath();
    ctx.fill();
  }

  // 地面参考圈（近似：脚底平面）
  ctx.strokeStyle = '#2a2a44';
  ctx.lineWidth = 1;
  ctx.beginPath();
  const yFloor = cy0 + R * scale;
  ctx.ellipse(cx0, yFloor, R*scale*Math.abs(cy), R*scale*0.22, 0, 0, Math.PI*2);
  ctx.stroke();
}
draw();
</script>
</body>
</html>
"""

def main() -> int:
    data = skindump(*CLIPS[0][:2], CLIPS[0][3])
    data["label"] = CLIPS[0][2]
    data["verts"] = len(data["dom"])
    payload = json.dumps(data, separators=(",", ":"))
    html = HTML.replace("__DATA__", payload)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(html)
    print(f"skin-demo: {OUT} ({len(html)/1e6:.2f} MB, {len(data['frames'])} frames, "
          f"{len(data['dom'])} verts, {len(data['idx'])//3} tris)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
