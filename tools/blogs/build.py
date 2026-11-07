#!/usr/bin/env python3
"""Build docs/blogs/index.html — offline viewer for the blog-study notes.

Reads every docs/blogs/*.md (source-of-truth notes), converts the small
markdown subset used by these documents to HTML (pure stdlib), and emits a
single self-contained index.html with a sidebar (like docs/kb/). Re-run after
editing the .md files:

    python tools/blogs/build.py
"""
from __future__ import annotations

import html
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / "docs" / "blogs"
OUT = SRC / "index.html"


def inline(text: str) -> str:
    """Inline markdown: escape first, then code / bold / links / italics."""
    tokens: list[str] = []

    def stash(escaped: str) -> str:
        tokens.append(escaped)
        return f"\x00{len(tokens) - 1}\x00"

    # inline code first so its content is untouched by the rest
    text = re.sub(r"`([^`]+)`", lambda m: stash(f"<code>{m.group(1)}</code>"), text)
    text = html.escape(text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)",
                  lambda m: stash(f'<a href="{m.group(2)}" target="_blank" rel="noopener">{m.group(1)}</a>'),
                  text)
    text = re.sub(r"<(https?://[^>\s]+)>",
                  lambda m: stash(f'<a href="{m.group(1)}" target="_blank" rel="noopener">{m.group(1)}</a>'),
                  text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<!\w)\*([^*\n]+)\*(?!\w)", r"<em>\1</em>", text)
    for i, t in enumerate(tokens):
        text = text.replace(f"\x00{i}\x00", t)
    return text


def convert(md: str) -> str:
    out: list[str] = []
    lines = md.splitlines()
    i, n = 0, len(lines)
    list_stack: list[str] = []          # open <ul>/<ol> tags
    table_buf: list[list[str]] = []

    def close_lists():
        while list_stack:
            out.append(f"</{list_stack.pop()}>")

    def flush_table():
        nonlocal table_buf
        if not table_buf:
            return
        rows = table_buf
        header: list[str] | None = None
        if len(rows) >= 2 and all(re.fullmatch(r":?-{3,}:?", c.strip()) for c in rows[1]):
            header = rows[0]
            del rows[0:2]
        out.append('<div class="tbl"><table>')
        if header:
            out.append("<thead><tr>" + "".join(f"<th>{inline(c)}</th>" for c in header) + "</tr></thead>")
        out.append("<tbody>")
        for r in rows:
            out.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>")
        out.append("</tbody></table></div>")
        table_buf = []

    while i < n:
        line = lines[i]

        # fenced code block
        if line.startswith("```"):
            flush_table(); close_lists()
            lang = line[3:].strip()
            i += 1
            code: list[str] = []
            while i < n and not lines[i].startswith("```"):
                code.append(lines[i]); i += 1
            i += 1  # closing fence
            cls = f' class="language-{html.escape(lang)}"' if lang else ""
            out.append(f"<pre><code{cls}>{html.escape(chr(10).join(code))}</code></pre>")
            continue

        # table rows
        if line.lstrip().startswith("|") and "|" in line.strip()[1:]:
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            table_buf.append(cells)
            i += 1
            continue
        flush_table()

        stripped = line.strip()
        if not stripped:
            close_lists()
        elif stripped == "---":
            close_lists(); out.append("<hr>")
        elif stripped.startswith("#### "):
            close_lists(); out.append(f"<h4>{inline(stripped[5:])}</h4>")
        elif stripped.startswith("### "):
            close_lists(); out.append(f"<h3>{inline(stripped[4:])}</h3>")
        elif stripped.startswith("## "):
            close_lists(); out.append(f"<h2>{inline(stripped[3:])}</h2>")
        elif stripped.startswith("# "):
            close_lists(); out.append(f"<h1>{inline(stripped[2:])}</h1>")
        elif stripped.startswith("> "):
            close_lists()
            quote: list[str] = []
            while i < n and lines[i].strip().startswith(">"):
                quote.append(lines[i].strip()[1:].strip()); i += 1
            i -= 1
            out.append(f"<blockquote>{inline(' '.join(quote))}</blockquote>")
        elif re.match(r"^\s*[-*] ", line):
            want = "ul"
            if not list_stack or list_stack[-1] != want:
                close_lists(); list_stack.append(want); out.append("<ul>")
            out.append(f"<li>{inline(re.sub(r'^\s*[-*] ', '', line))}</li>")
        elif re.match(r"^\s*\d+\. ", line):
            want = "ol"
            if not list_stack or list_stack[-1] != want:
                close_lists(); list_stack.append(want); out.append("<ol>")
            out.append(f"<li>{inline(re.sub(r'^\s*\d+\. ', '', line))}</li>")
        else:
            close_lists()
            out.append(f"<p>{inline(stripped)}</p>")
        i += 1

    flush_table(); close_lists()
    return "\n".join(out)


CSS = """
:root { color-scheme: dark; }
* { box-sizing: border-box; }
body { margin:0; font:14px/1.75 "Segoe UI",system-ui,sans-serif; background:#0d1117; color:#c9d1d9; }
a { color:#58a6ff; text-decoration:none; } a:hover { text-decoration:underline; }
code { background:#161b22; border:1px solid #30363d; border-radius:4px; padding:1px 5px; font:12.5px/1.5 Consolas,monospace; color:#e6edf3; }
pre { background:#161b22; border:1px solid #30363d; border-radius:8px; padding:14px 16px; overflow-x:auto; }
pre code { border:none; background:none; padding:0; }
.layout { display:flex; min-height:100vh; }
aside { width:280px; flex:0 0 280px; background:#010409; border-right:1px solid #21262d; padding:18px 14px; position:sticky; top:0; height:100vh; overflow-y:auto; }
aside h1 { font-size:15px; color:#e6edf3; margin:2px 6px 2px; }
aside .sub { font-size:11.5px; color:#8b949e; margin:0 6px 14px; }
.doc-link { display:block; padding:9px 12px; border-radius:8px; color:#c9d1d9; margin:2px 0; border:1px solid transparent; }
.doc-link:hover { background:#161b22; text-decoration:none; }
.doc-link.active { background:#1f6feb22; border-color:#1f6feb; color:#58a6ff; }
.toc { margin:16px 6px 0; font-size:12px; }
.toc .toc-title { color:#8b949e; text-transform:uppercase; letter-spacing:.08em; font-size:10.5px; margin-bottom:6px; }
.toc a { display:block; color:#8b949e; padding:3px 0 3px 10px; border-left:2px solid #21262d; }
.toc a:hover { color:#58a6ff; text-decoration:none; border-left-color:#58a6ff; }
main { flex:1; padding:34px 46px 80px; max-width:1060px; }
h1 { font-size:26px; color:#f0f6fc; border-bottom:1px solid #21262d; padding-bottom:12px; }
h2 { font-size:20px; color:#f0f6fc; border-bottom:1px solid #21262d; padding-bottom:8px; margin-top:40px; }
h3 { font-size:16px; color:#e6edf3; margin-top:28px; }
blockquote { margin:14px 0; padding:8px 16px; border-left:3px solid #1f6feb; background:#161b22; border-radius:0 8px 8px 0; color:#8b949e; }
blockquote p { margin:0; }
.tbl { overflow-x:auto; margin:14px 0; }
table { border-collapse:collapse; width:100%; font-size:13px; }
th,td { border:1px solid #30363d; padding:7px 11px; text-align:left; vertical-align:top; }
th { background:#161b22; color:#e6edf3; }
tr:nth-child(even) td { background:#0d1117; }
tr:nth-child(odd) td { background:#10161d; }
hr { border:none; border-top:1px solid #21262d; margin:26px 0; }
li { margin:3px 0; }
"""

JS = """
document.querySelectorAll('main h2[id], main h3[id]').forEach(h => {
  h.addEventListener('click', () => location.hash = h.id);
});
"""


def slug(text: str) -> str:
    s = re.sub(r"<[^>]+>", "", text)
    s = re.sub(r"[^\w\u4e00-\u9fff- ]", "", s).strip().replace(" ", "-")
    return s or "sec"


def build() -> int:
    md_files = sorted(SRC.glob("*.md"))
    if not md_files:
        print(f"no markdown files under {SRC}")
        return 1

    docs = []
    for f in md_files:
        raw = f.read_text(encoding="utf-8")
        title_m = re.match(r"^#\s+(.+)", raw)
        title = title_m.group(1).strip() if title_m else f.stem
        body = convert(raw)
        # 标题已由模板的 doc-title 承担，去掉正文里重复的第一个 h1
        body = re.sub(r"^\s*<h1>.*?</h1>", "", body, count=1)
        docs.append({"id": f.stem, "file": f.name, "title": title, "html": body})

    nav = "\n".join(
        f'<a class="doc-link" data-doc="{d["id"]}" href="#/{d["id"]}">{html.escape(d["title"])}</a>'
        for d in docs)

    sections = "\n".join(
        f'<article class="doc" id="doc-{d["id"]}" hidden><h1 class="doc-title">{html.escape(d["title"])}</h1>{d["html"]}</article>'
        for d in docs)

    page = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>nl-engine · 大佬博客研读笔记</title>
<style>{CSS}</style>
</head>
<body>
<div class="layout">
  <aside>
    <h1>大佬博客研读笔记</h1>
    <div class="sub">博客原文 × 源码实况 × 我们的对照 · 源文件 docs/blogs/*.md（编辑后重跑 tools/blogs/build.py）</div>
    <nav>{nav}</nav>
    <div class="toc" id="toc"></div>
  </aside>
  <main id="content">{sections}</main>
</div>
<script>
const docs = [{", ".join(repr(d["id"]) for d in docs)}];
function show(id) {{
  document.querySelectorAll('.doc').forEach(a => a.hidden = a.id !== 'doc-' + id);
  document.querySelectorAll('.doc-link').forEach(a => a.classList.toggle('active', a.dataset.doc === id));
  const active = document.querySelector('.doc:not([hidden])');
  const toc = document.getElementById('toc');
  toc.innerHTML = '<div class="toc-title">本篇目录</div>';
  if (active) {{
    active.querySelectorAll('h2').forEach((h, i) => {{
      if (!h.id) h.id = 'sec-' + i;
      const a = document.createElement('a');
      a.href = '#' + h.id; a.textContent = h.textContent;
      toc.appendChild(a);
    }});
    window.scrollTo(0, 0);
  }}
  try {{ localStorage.setItem('blogs-doc', id); }} catch (e) {{}}
}}
document.querySelectorAll('.doc-link').forEach(a =>
  a.addEventListener('click', () => show(a.dataset.doc)));
show((() => {{
  try {{ const s = localStorage.getItem('blogs-doc'); if (s && docs.includes(s)) return s; }} catch (e) {{}}
  return docs[docs.length - 1];
}})());
{JS}
</script>
</body>
</html>
"""
    OUT.write_text(page, encoding="utf-8")
    print(f"[blogs] OK: {len(docs)} docs -> {OUT.relative_to(ROOT).as_posix()} "
          f"({OUT.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(build())
