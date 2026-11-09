# -*- coding: utf-8 -*-
"""P8 M-S0 evidence PNG generator.

Iron rule: every pixel comes from REAL tool output.
  - waveform.png  <- example/audio-s0/dump/waveform.tsv (ma_engine_read_pcm_frames output)
  - acceptance.png<- example/audio-s0/*.log (audio-tools real stdout, redirected by run.bat)
No hand-drawn diagrams. All rendered text is ASCII (Consolas has no CJK glyphs).
Reproduce: example/audio-s0/run.bat
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, "example", "audio-s0")
DUMP = os.path.join(DIR, "dump")

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)
GRID = (48, 54, 62)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def read_kv(path):
    out = {}
    for line in read_text(path).splitlines():
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def load_waveform(path):
    left, right = [], []
    for line in read_text(path).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0][0].isdigit():
            left.append(float(parts[1]))
            right.append(float(parts[2]))
    return left, right


def draw_channel(d, box, values, color, label):
    x0, y0, w, h = box
    d.rectangle([x0, y0, x0 + w, y0 + h], outline=GRID)
    mid = y0 + h // 2
    d.line([x0, mid, x0 + w, mid], fill=(64, 72, 82))
    amp = (h / 2.0 - 8) * 0.92
    cols = min(w, len(values))
    for px in range(cols):
        a = px * len(values) // cols
        b = max(a + 1, (px + 1) * len(values) // cols)
        seg = values[a:b]
        if not seg:
            continue
        y1 = mid - max(seg) * amp
        y2 = mid - min(seg) * amp
        if y2 - y1 < 1:
            y2 = y1 + 1
        d.line([x0 + px, y1, x0 + px, y2], fill=color)
    d.text((x0 + 10, y0 + 8), label, font=font(16, True), fill=color)


def make_waveform():
    tsv = os.path.join(DUMP, "waveform.tsv")
    prov = read_kv(os.path.join(DUMP, "provenance.txt"))
    left, right = load_waveform(tsv)
    assert left and len(left) == len(right), "empty or ragged waveform.tsv"

    W, H = 1240, 590
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((28, 22), "M-S0 offline PCM waveform - ma_engine_read_pcm_frames (noDevice)",
           font=font(23, True), fill=CYAN)
    d.text((28, 58),
           "sampleRate=%s  channels=%s  frames=%s  gain=%s  flags=%s" %
           (prov.get("sampleRate", "?"), prov.get("channels", "?"),
            prov.get("frames", "?"), prov.get("gain", "?"), prov.get("flags", "?")),
           font=font(16), fill=FG)
    d.text((28, 84), "waveformHash=%s" % prov.get("waveformHash", "?"),
           font=font(16), fill=DIM)

    draw_channel(d, (28, 120, W - 56, 196), left, CYAN, "LEFT  (f32, %d frames)" % len(left))
    draw_channel(d, (28, 330, W - 56, 196), right, GREEN, "RIGHT (f32, %d frames)" % len(right))

    peak = max(max(abs(v) for v in left), max(abs(v) for v in right))
    d.text((28, 540),
           "peak=%.6f  source=example/audio-s0/dump/waveform.tsv  (real tool output, not a sketch)" % peak,
           font=font(16), fill=AMBER)
    path = os.path.join(DIR, "waveform.png")
    img.save(path)
    print("wrote", path, img.size)


def color_for(line):
    t = line.strip()
    if "PASS" in t:
        return GREEN
    if "FAIL" in t or "unknown subcommand" in t:
        return RED
    if t.startswith("=== ") or t.startswith("usage:"):
        return CYAN
    if t.startswith("dump:") or t.startswith("check:") or t.startswith("wav"):
        return AMBER
    if t.startswith("  ") and t.strip():
        return DIM
    return FG


def wrap_line(line, limit=124):
    if len(line) <= limit:
        return [line]
    out = []
    while len(line) > limit:
        out.append(line[:limit])
        line = "    " + line[limit:]
    out.append(line)
    return out


def make_acceptance():
    sections = [
        ("usage.log", "audio-tools (no args)  -> exit 2 / usage"),
        ("probe.log", "audio-tools probe test-tone.wav  -> exit 0"),
        ("selftest.log", "audio-tools selftest  -> exit 0 (determinism + tamper proof)"),
        ("dump.log", "audio-tools dump  -> exit 0 (real PCM -> waveform.tsv)"),
        ("check.log", "audio-tools check  -> exit 0 (re-render from provenance)"),
        ("negative.log", "negative: missing WAV  -> exit 1 (explicit rejection)"),
        ("negative-check.log", "negative: tampered provenance  -> exit 1 (anti-false-green)"),
    ]
    lines = []
    for log, title in sections:
        p = os.path.join(DIR, log)
        if not os.path.exists(p):
            continue
        if lines:
            lines.append("")
        lines.append("=== " + title + " ===")
        for raw in read_text(p).splitlines():
            lines.extend(wrap_line(raw.rstrip()))

    f = font(16)
    fb = font(16, True)
    ft = font(22, True)
    lh = 25
    pad = 26
    tmp = Image.new("RGB", (4, 4))
    td = ImageDraw.Draw(tmp)
    width = min(1360, max([td.textlength(l, font=f) for l in lines] + [400]) + pad * 2 + 40)
    height = pad * 2 + lh * (len(lines) + 2)
    img = Image.new("RGB", (int(width), height), BG)
    d = ImageDraw.Draw(img)
    d.text((pad, pad), "M-S0 acceptance - audio-tools real output", font=ft, fill=CYAN)
    y = pad + lh * 2
    for line in lines:
        is_title = line.startswith("=== ")
        d.text((pad, y), line, font=fb if is_title else f,
               fill=CYAN if is_title else color_for(line))
        y += lh
    path = os.path.join(DIR, "acceptance.png")
    img.save(path)
    print("wrote", path, img.size)


def main():
    make_waveform()
    make_acceptance()
    print("ALL EVIDENCE DONE")


if __name__ == "__main__":
    main()
