# -*- coding: utf-8 -*-
"""P8 M-S0 evidence PNG generator.

Iron rule: every pixel comes from REAL tool output.
  - waveform.png  <- example/audio-s0/dump/waveform.tsv (ma_engine_read_pcm_frames output)
  - acceptance.png<- example/audio-s0/*.log (audio-tools real stdout, redirected by run.bat)
No hand-drawn diagrams. All rendered text is ASCII (Consolas has no CJK glyphs).
Reproduce: example/audio-s0/run.bat
"""
import os
import sys
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
    # Stretch to the full canvas width. M-S0 has 24000 frames (> w) so its
    # output is unchanged; M-S1's 512 frames must not leave the right half blank.
    cols = w
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


# ---------------- M-S1 evidence (from real audio-tools spatial output) ----------------
S1DIR = os.path.join(ROOT, "example", "audio-s1")
S1DUMP = os.path.join(S1DIR, "spatial")
PURPLE = (186, 148, 224)


def load_s1_attenuation(path):
    xs, series = [], [[], [], [], []]
    for line in read_text(path).splitlines():
        p = line.split()
        if len(p) >= 5 and p[0][0].isdigit():
            xs.append(float(p[0]))
            for i in range(4):
                series[i].append(float(p[1 + i]))
    assert xs and all(len(s) == len(xs) for s in series), "ragged attenuation.tsv"
    return xs, series


def load_s1_mixing(path):
    a, b, mix, sm = [], [], [], []
    for line in read_text(path).splitlines():
        p = line.split()
        if len(p) >= 5 and p[0][0].isdigit():
            a.append(float(p[1]))
            b.append(float(p[2]))
            mix.append(float(p[3]))
            sm.append(float(p[4]))
    assert a and len(a) == len(b) == len(mix) == len(sm), "ragged mixing.tsv"
    return a, b, mix, sm


def make_s1_attenuation():
    xs, series = load_s1_attenuation(os.path.join(S1DUMP, "attenuation.tsv"))
    prov = read_kv(os.path.join(S1DUMP, "provenance.txt"))
    names = ["none", "inverse", "linear", "exponential"]
    colors = [AMBER, CYAN, GREEN, PURPLE]
    W, H = 1240, 660
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((28, 20), "M-S1 distance attenuation - 4 miniaudio models (real offline PCM RMS)",
           font=font(22, True), fill=CYAN)
    d.text((28, 54),
           "minDistance=%s  maxDistance=%s  rolloff=%s  steps=%s  warmup=%s  measure=%s" %
           (prov.get("minDistance", "?"), prov.get("maxDistance", "?"),
            prov.get("rolloff", "?"), prov.get("distanceSteps", "?"),
            prov.get("warmupFrames", "?"), prov.get("measureFrames", "?")),
           font=font(15), fill=FG)
    d.text((28, 78), "attenuationHash=%s  source=example/audio-s1/spatial/attenuation.tsv" %
           prov.get("attenuationHash", "?"), font=font(15), fill=DIM)

    x0, y0, pw, ph = 84, 126, W - 150, 420
    d.rectangle([x0, y0, x0 + pw, y0 + ph], outline=GRID)
    ymax = max(max(s) for s in series) * 1.12
    xmax = max(xs)
    for i in range(5):
        gy = y0 + ph * i / 4.0
        d.line([x0, gy, x0 + pw, gy], fill=GRID)
        d.text((x0 - 76, gy - 9), "%.3f" % (ymax * (4 - i) / 4.0), font=font(13), fill=DIM)
    for i in range(7):
        gx = x0 + pw * i / 6.0
        d.line([gx, y0, gx, y0 + ph], fill=GRID)
        d.text((gx - 12, y0 + ph + 8), "%.0f" % (xmax * i / 6.0), font=font(13), fill=DIM)
    md = float(prov.get("maxDistance", "20"))
    mx = x0 + pw * md / xmax
    d.line([mx, y0, mx, y0 + ph], fill=RED)
    d.text((mx + 6, y0 + 8), "maxDistance -> silence", font=font(13), fill=RED)

    for s, color in zip(series, colors):
        pts = [(x0 + pw * x / xmax, y0 + ph - ph * v / ymax) for x, v in zip(xs, s)]
        d.line(pts, fill=color, width=3)
        for px, py in pts[::4]:
            d.ellipse([px - 2, py - 2, px + 2, py + 2], fill=color)
    ly = y0 + ph + 40
    for ci, (name, color) in enumerate(zip(names, colors)):
        lx = x0 + ci * 272
        d.rectangle([lx, ly + 4, lx + 18, ly + 12], fill=color)
        d.text((lx + 26, ly), name, font=font(16, True), fill=color)
    d.text((28, H - 48),
           "none = miniaudio unspatialized fast path (flat). warmup > gainSmoothTimeInFrames(360).",
           font=font(14), fill=DIM)
    d.text((28, H - 26),
           "Every value = sqrt(mean(L^2,R^2)) over %s measured frames - real tool output, not a sketch." %
           prov.get("measureFrames", "?"), font=font(14), fill=AMBER)
    path = os.path.join(S1DIR, "attenuation.png")
    img.save(path)
    print("wrote", path, img.size)


def make_s1_mixing():
    a, b, mix, sm = load_s1_mixing(os.path.join(S1DUMP, "mixing.tsv"))
    prov = read_kv(os.path.join(S1DUMP, "provenance.txt"))
    residual = [m - s for m, s in zip(mix, sm)]
    W, H = 1240, 780
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((28, 18), "M-S1 concurrent sources - linear mixing proof (real PCM, left channel)",
           font=font(22, True), fill=CYAN)
    d.text((28, 50),
           "mixingHash=%s  maxAbsDiff=%s  exactSamples=%s  clips=%s misses=%s hits=%s sources=%s" %
           (prov.get("mixingHash", "?"), prov.get("mixingMaxAbsDiff", "?"),
            prov.get("mixingExactSamples", "?"), prov.get("clips", "?"),
            prov.get("misses", "?"), prov.get("hits", "?"), prov.get("sources", "?")),
           font=font(14), fill=DIM)
    draw_channel(d, (28, 92, W - 56, 142), a, CYAN, "SOURCE A  440 Hz / amp 0.6  (solo)")
    draw_channel(d, (28, 250, W - 56, 142), b, GREEN, "SOURCE B  660 Hz / amp 0.4  (solo)")
    draw_channel(d, (28, 408, W - 56, 142), mix, AMBER, "MIX  A + B in one ma_engine (concurrent)")
    draw_channel(d, (28, 566, W - 56, 122), residual, PURPLE,
                 "RESIDUAL  mix - (a+b)   maxAbsDiff=%s" % prov.get("mixingMaxAbsDiff", "?"))
    d.text((28, H - 58),
           "Two ma_sound nodes share one engine; PCM data de-duplicated by ma_resource_manager.",
           font=font(14), fill=DIM)
    d.text((28, H - 32),
           "source=example/audio-s1/spatial/mixing.tsv - real tool output, not a sketch.",
           font=font(14), fill=AMBER)
    path = os.path.join(S1DIR, "mixing.png")
    img.save(path)
    print("wrote", path, img.size)


def make_s1_acceptance():
    sections = [
        ("usage.log", "audio-tools (no args)  -> exit 2 / usage"),
        ("selftest.log", "audio-tools selftest  -> exit 0 (33/33: M-S0 + M-S1)"),
        ("spatial.log", "audio-tools spatial  -> exit 0 (4 curves + mixing + cache)"),
        ("check.log", "audio-tools check  -> exit 0 (re-render ALL from provenance)"),
        ("probe-b.log", "audio-tools probe test-tone-b.wav  -> exit 0"),
        ("negative-tsv.log", "negative: tampered attenuation.tsv  -> exit 1 (hash)"),
        ("negative-prov.log", "negative: tampered maxDistance  -> exit 1 (re-render)"),
    ]
    lines = []
    for log, title in sections:
        p = os.path.join(S1DIR, log)
        if not os.path.exists(p):
            continue
        if lines:
            lines.append("")
        lines.append("=== " + title + " ===")
        for raw in read_text(p).splitlines():
            lines.extend(wrap_line(raw.rstrip()))
    f, fb, ft = font(16), font(16, True), font(22, True)
    lh, pad = 25, 26
    td = ImageDraw.Draw(Image.new("RGB", (4, 4)))
    width = min(1360, max([td.textlength(l, font=f) for l in lines] + [400]) + pad * 2 + 40)
    height = pad * 2 + lh * (len(lines) + 2)
    img = Image.new("RGB", (int(width), height), BG)
    d = ImageDraw.Draw(img)
    d.text((pad, pad), "M-S1 acceptance - audio-tools real output", font=ft, fill=CYAN)
    y = pad + lh * 2
    for line in lines:
        is_title = line.startswith("=== ")
        d.text((pad, y), line, font=fb if is_title else f,
               fill=CYAN if is_title else color_for(line))
        y += lh
    path = os.path.join(S1DIR, "acceptance.png")
    img.save(path)
    print("wrote", path, img.size)


def main():
    if "--s1" in sys.argv[1:]:
        make_s1_attenuation()
        make_s1_mixing()
        make_s1_acceptance()
    else:
        make_waveform()
        make_acceptance()
    print("ALL EVIDENCE DONE")


if __name__ == "__main__":
    main()
