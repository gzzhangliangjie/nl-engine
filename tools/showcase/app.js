/* nl-engine S0 showcase — gallery + live runner. No external dependencies. */
'use strict';

const state = { data: null, versionId: null, backend: 'both', busy: false };
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
  ));
}

function readyCount(v) {
  return ['dx12', 'vulkan'].filter(b => v.status[b].ready).length;
}

function statusClass(v) {
  const n = readyCount(v);
  return n === 2 ? 'ready' : n === 1 ? 'partial' : 'none';
}

/* ---------------- sidebar ---------------- */
function renderVersions() {
  const nav = $('#versionList');
  nav.innerHTML = state.data.versions.map(v => `
    <button class="ver ${v.id === state.versionId ? 'active' : ''}" data-id="${v.id}">
      <div class="ver-top">
        <span class="ver-label">${esc(v.label)}</span>
        <span class="dot ${statusClass(v) === 'ready' ? '' : statusClass(v)}"></span>
      </div>
      <div class="ver-name">${esc(v.name)}</div>
      <div class="ver-tag">${esc(v.tag)} · ${esc(v.date)}</div>
    </button>`).join('');
  $$('.ver', nav).forEach(btn => btn.addEventListener('click', () => selectVersion(btn.dataset.id)));

  const total = state.data.versions.length * 2;
  const ready = state.data.versions.reduce((n, v) => n + readyCount(v), 0);
  const sync = state.data.stageSync;
  let syncLine = '';
  if (sync) {
    syncLine = sync.ok
      ? `<br><span style="color:var(--pass)">✓ 本轮 ${esc(sync.stage)} 已归位 example/（${sync.cases} 用例 / ${sync.baselines} 基线）</span>`
      : `<br><span style="color:var(--warn)" title="${esc(sync.reasons.join('；'))}">⚠ 本轮未归位：${esc(sync.reasons[0])}</span>`;
  }
  $('#serviceInfo').innerHTML = `用例库 <code>example/</code> · 历史快照 exe：<b style="color:var(--text)">${ready}/${total}</b> 就绪` +
    (ready === total ? '，全部版本可现场运行。' : '，缺失的版本只展示基线截图。') + syncLine;
  if (ready < total) {
    $('#rebuildHint').textContent = '有版本缺少历史 exe（build/ 不入库）。在仓库根运行： python tools/showcase/rebuild.py';
  }
}

function selectVersion(id) {
  if (state.busy) return;
  state.versionId = id;
  const v = state.data.versions.find(x => x.id === id);
  $('#headTag').textContent = `TAG ${v.tag}  /  ${v.id.toUpperCase()}`;
  $('#headTitle').textContent = `${v.label} · ${v.name}`;
  $('#headSummary').textContent = v.summary;
  $('#headDate').textContent = v.date;
  const badge = $('#headStatus');
  badge.className = 'badge ' + statusClass(v);
  badge.textContent = readyCount(v) === 2 ? '双后端可运行' : readyCount(v) === 1 ? '部分可运行' : '仅基线截图';
  $('#acceptList').innerHTML = v.acceptance.map(a => `<li>${esc(a)}</li>`).join('');
  renderVersions();
  renderCases(v);
}

/* ---------------- case cards ---------------- */
function paramControl(name, cfg) {
  if (cfg.type === 'select') {
    return `<div class="param" data-name="${esc(name)}">
      <label>${esc(cfg.label)}</label>
      <select data-param="${esc(name)}">${cfg.options.map(o =>
        `<option value="${esc(o)}" ${String(o) === String(cfg.default) ? 'selected' : ''}>${esc(o)}</option>`
      ).join('')}</select></div>`;
  }
  return `<div class="param" data-name="${esc(name)}">
    <label>${esc(cfg.label)} <b data-value="${esc(name)}">${esc(cfg.default)}</b></label>
    <input type="range" data-param="${esc(name)}" min="${cfg.min}" max="${cfg.max}"
           step="${cfg.step}" value="${esc(cfg.default)}">
  </div>`;
}

function shotBlock(v, c, backend) {
  const info = c.baselineInfo[backend];
  const before = (c.beforeInfo || {})[backend] || {};
  const label = backend === 'dx12' ? 'DX12' : 'Vulkan';
  const img = info.exists && info.url
    ? `<img src="${info.url}" alt="${label} 基线">`
    : `<div class="placeholder">该用例没有基线截图<br>点「运行看效果」现场生成</div>`;
  const beforeLink = before.url
    ? ` <a href="${before.url}" target="_blank" title="resize 前的基线截图">前↗</a>` : '';
  return `<div class="shot" data-shot="${backend}">
    <div class="shot-cap"><span>${label} · <span data-cap="${backend}">基线</span></span>
      <span data-hash="${backend}">${info.md5 ? info.md5.slice(0, 8) : '—'}${beforeLink}</span></div>
    <div data-img="${backend}">${img}</div>
  </div>`;
}

function renderCases(v) {
  $('#cases').innerHTML = v.cases.map(c => {
    const hasImage = c.kind === 'image' || c.kind === 'pair';
    const params = c.params
      ? `<div class="params">${Object.entries(c.params).map(([n, cfg]) => paramControl(n, cfg)).join('')}</div>`
      : '';
    const shots = hasImage
      ? `<div class="shots">${shotBlock(v, c, 'dx12')}${shotBlock(v, c, 'vulkan')}</div>`
      : '';
    const btnLabel = c.kind === 'selftest' ? '▶ 运行自检'
      : c.kind === 'manual' ? '交互用例 · 手动运行' : '▶ 运行看效果';
    return `<article class="card" data-case="${esc(c.id)}">
      <div class="card-head">
        <div class="card-title">${esc(c.name)}</div>
        <span class="kind ${esc(c.kind)}">${esc(c.kind)}</span>
      </div>
      <div class="card-note">${esc(c.note)}</div>
      ${shots}
      ${params}
      <div class="cmd"><span>${esc(c.command)}</span><button data-copy="${esc(c.command)}">复制</button></div>
      ${c.script ? `<div class="cmd"><span>${esc(c.script)}</span><button data-copy="${esc(c.script)}">复制</button></div>` : ''}
      <div class="card-foot">
        <button class="btn-run ${c.kind === 'manual' ? 'manual' : ''}"
                ${c.kind === 'manual' ? 'disabled' : ''}>${btnLabel}</button>
        <div class="result" data-result></div>
      </div>
    </article>`;
  }).join('');

  $$('#cases .card').forEach(card => {
    const c = v.cases.find(x => x.id === card.dataset.case);
    $$('input[type=range]', card).forEach(inp =>
      inp.addEventListener('input', () => {
        $(`[data-value="${inp.dataset.param}"]`, card).textContent = inp.value;
      }));
    $$('[data-copy]', card).forEach(b => b.addEventListener('click', e => {
      const btn = e.currentTarget;
      const text = btn.dataset.copy;
      navigator.clipboard?.writeText(text).then(
        () => { btn.textContent = '已复制'; setTimeout(() => btn.textContent = '复制', 1200); },
        () => { btn.textContent = '复制失败'; }
      );
    }));
    if (c.kind !== 'manual') {
      $('.btn-run', card).addEventListener('click', () => runCase(card, v, c));
    }
  });
}

function collectParams(card) {
  const out = {};
  $$('[data-param]', card).forEach(el => { out[el.dataset.param] = el.value; });
  return out;
}

function logBlock(results) {
  return `<details class="log"><summary>运行日志（${results.length} 个进程）</summary>` +
    results.map(r => `<pre>${esc(r.backend.toUpperCase())}$ ${esc(r.command || '')}\n${esc(r.log || '(无输出)')}</pre>`).join('') +
    `</details>`;
}

async function runCase(card, v, c) {
  if (state.busy) return;
  const btn = $('.btn-run', card);
  const resultBox = $('[data-result]', card);
  btn.disabled = true;
  btn.textContent = '运行中…';
  resultBox.innerHTML = `<span class="pill neutral">调用 ${esc(state.backend === 'both' ? 'DX12 + Vulkan' : state.backend)} 的 viewer.exe…</span>`;
  state.busy = true;

  let payload;
  try {
    const resp = await fetch('/api/run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ version: v.id, case: c.id, backend: state.backend, params: collectParams(card) })
    });
    payload = await resp.json();
  } catch (e) {
    payload = { ok: false, error: '无法连接展示服务：' + e.message, results: [] };
  }

  const results = payload.results || [];
  const pills = [];
  if (payload.error) pills.push(`<span class="pill bad">${esc(payload.error)}</span>`);
  results.forEach(r => {
    if (!r.ok) {
      pills.push(`<span class="pill bad">${r.backend.toUpperCase()} 失败 ${r.error ? '· ' + esc(r.error) : ''}</span>`);
      return;
    }
    pills.push(`<span class="pill">${r.backend.toUpperCase()} PASS · ${r.ms}ms · exit ${r.exit ?? 0}</span>`);
    if (r.deviceErrors !== null && r.deviceErrors !== undefined) {
      pills.push(`<span class="pill ${r.deviceErrors === 0 ? '' : 'bad'}">deviceErrors=${r.deviceErrors}</span>`);
    }
    if (r.baselineMatch === true) pills.push(`<span class="pill">与基线 PNG 相同</span>`);
    else if (r.baselineMatch === false) pills.push(`<span class="pill warn">与基线不同（参数已改动？）</span>`);
    if (r.image) {
      const wrap = $(`[data-img="${r.backend}"]`, card);
      if (wrap) wrap.innerHTML = `<img src="${r.image}" alt="${r.backend} 实时渲染">`;
      const cap = $(`[data-cap="${r.backend}"]`, card);
      if (cap) cap.textContent = '实时运行';
      const hash = $(`[data-hash="${r.backend}"]`, card);
      if (hash && r.md5) hash.textContent = r.md5.slice(0, 8);
    }
    if (r.beforeImage) {
      pills.push(`<a class="pill neutral" href="${r.beforeImage}" target="_blank">这次 resize 前 ↗</a>`);
    }
    if (r.beforeBaselineImage) {
      pills.push(`<a class="pill neutral" href="${r.beforeBaselineImage}" target="_blank">基线 resize 前 ↗</a>`);
    }
  });
  if (state.backend === 'both' && results.length === 2 &&
      payload.identical !== null && payload.identical !== undefined) {
    const pd = payload.pixelDiff || {};
    if (payload.identical === 'bytes') {
      pills.push(`<span class="pill">双后端 PNG 逐字节一致 ✓</span>`);
    } else if (payload.identical === 'pixels') {
      pills.push(`<span class="pill">像素完全一致（PNG 编码字节不同）</span>`);
    } else if (payload.identical === 'near') {
      pills.push(`<span class="pill warn">像素差 ≤1/255 · ${pd.diffPixels ?? '?'} 个像素（GPU 取整噪声）</span>`);
    } else {
      pills.push(`<span class="pill bad">双后端画面不同（最大通道差 ${pd.maxDelta ?? '?'}）</span>`);
    }
  }
  resultBox.innerHTML = pills.join('') + (results.length ? logBlock(results) : '');
  btn.disabled = false;
  btn.textContent = c.kind === 'selftest' ? '▶ 运行自检' : '▶ 运行看效果';
  state.busy = false;
  return payload.ok;
}

/* ---------------- global controls ---------------- */
async function runAll() {
  if (state.busy) return;
  const v = state.data.versions.find(x => x.id === state.versionId);
  const runnable = $$('#cases .card').filter(card =>
    !$('.btn-run', card).disabled && !$('.btn-run', card).classList.contains('manual'));
  const btn = $('#runAll');
  btn.disabled = true;
  let pass = 0;
  for (let i = 0; i < runnable.length; i++) {
    $('#runAllStatus').textContent = `运行中 ${i + 1}/${runnable.length}…`;
    if (await runCase(runnable[i], v, v.cases.find(c => c.id === runnable[i].dataset.case))) pass++;
  }
  $('#runAllStatus').textContent = `完成：${pass}/${runnable.length} 个用例通过`;
  btn.disabled = false;
}

function bindBackend() {
  $$('#backendSwitch button').forEach(b => b.addEventListener('click', () => {
    if (state.busy) return;
    state.backend = b.dataset.backend;
    $$('#backendSwitch button').forEach(x => x.classList.toggle('active', x === b));
  }));
}

// 开交互窗口：不截图、不比对基线，窗口保持打开直到手动关闭。
async function openInteractive() {
  const btn = $('#interactiveBtn');
  const status = $('#interactiveStatus');
  const v = state.data.versions.find(x => x.id === state.versionId);
  if (!v || state.busy) return;
  const label = state.backend === 'both' ? 'DX12 + Vulkan' : state.backend.toUpperCase();
  btn.disabled = true;
  status.textContent = `正在启动 ${esc(v.id)} 的交互窗口（${label}）…`;
  try {
    const resp = await fetch('/api/interactive', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ version: v.id, backend: state.backend })
    });
    const payload = await resp.json();
    if (payload.ok) {
      const pids = payload.spawned.map(s => `${s.backend.toUpperCase()}(pid ${s.pid})`).join('、');
      status.textContent = `已启动 ${pids}。窗口保持打开，ESC 或点 X 关闭；拖动旋转、滚轮缩放、HUD 控件可直接点。`;
    } else {
      status.textContent = esc(payload.error || '启动失败');
    }
  } catch (e) {
    status.textContent = '无法连接展示服务：' + esc(e.message);
  }
  btn.disabled = false;
}

async function init() {
  bindBackend();
  $('#runAll').addEventListener('click', runAll);
  $('#interactiveBtn').addEventListener('click', openInteractive);
  try {
    const resp = await fetch('/api/cases');
    state.data = await resp.json();
  } catch (e) {
    document.body.innerHTML = `<pre style="padding:30px;color:#f85149">无法读取 /api/cases：${esc(e.message)}<br>请通过 python tools/showcase/server.py 启动服务后访问。</pre>`;
    return;
  }
  // Default to the newest milestone: it is the one with the most to look at.
  selectVersion(state.data.versions[state.data.versions.length - 1].id);
}

init();
