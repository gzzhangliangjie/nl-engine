#!/usr/bin/env python3
"""S12 M12.2 acceptance gates (pure stdlib, no PIL).

Implements the two decisions taken at the M12.2 start gate (S12-PLAN.md 0.8):

  Q12-D = D-B "N-frame mean with filtering off": a S12-owned temporal
  reference produced by averaging N independent single-frame GI dumps
  (--mode filter --temporal-off --filter-gi-dump --screenshot-seq).
  It is ISOMORPHIC to the object under test (same csGather weights, same
  jitter distribution). Hard limit N <= 64: the jitter frame index is
  s12Frame_ % 64, so sample 65 repeats sample 1 (scene.cpp:3802).
  Motion sequences are NEVER averaged (that would blur motion); the
  same-frame no-history single shot is used as the ghosting baseline.

  Q12-B = B1 "no previous GBuffer on disk": the rejection layer is exercised
  by motion sequences (static camera has velocity == 0, so no rejection can
  fire). The gate compares
      ON   = --filter-reject                (history rejected where invalid)
      OFF  = no reject layer               (history always accumulated)
      SINGLE = --temporal-off              (same-frame, no-history baseline)
      REJALL = --filter-reject --reject-threshold <tiny> (negative control:
               an over-strict threshold must reject MORE history than ON)

Gates (Q11-1: GPU numerical images are NOT byte-exact across backends, so
every assertion is a statistical invariant, never bytes):
  avg      : N-frame per-pixel RGB mean -> D-B reference PNG
  refcheck : ON does not drift from the D-B reference (MAE / high-freq)
  ghost    : mean MAE(ON,SINGLE) <= ratioLimit * mean MAE(OFF,SINGLE),
             i.e. rejection makes motion frames track the no-history
             baseline more closely (ghosting proxy); plus layer-response
             negative control diff(REJALL,OFF) > diff(ON,OFF)
  disocc   : the DISOCCLUSION-LIMITED edge-preservation gate. Derives a
             disocclusion mask per pixel from |OFF - SINGLE| > disTol
             (history disagreed with the current no-history truth = a
             newly revealed / occluded region). Within that mask it asserts
             ON tracks SINGLE better than OFF (ratio <= ratioLimit), OFF
             actually ghosts there (meanMAE(off,single) > minGhost), and
             the mask is non-trivial (maskFrac > minFrac). This is the gate
             the single convex + pure-sky s12-done scene COULD NOT exercise
             (S12-PLAN 0.8.x): real geometric edges / disocclusion under
             motion. The over-strict-threshold layer-response negative
             control is covered by the `ghost` gate (pixel-count invariant,
             proven there) and is intentionally not duplicated here.

Subcommands:
  avg      <out.png> <seqDir> [n=32]
  refcheck <ref.png> <off.png> <on.png> [maeRatio] [maxMae] [hfRatio]
  ghost    <onDir> <offDir> <singleDir> <rejectAllDir> [n=16] [ratioLimit]
  disocc   <onDir> <offDir> <singleDir> <rejectAllDir> [n=16] [ratioLimit]
           [disTol=8.0] [minFrac=0.0005] [minGhost=6.0]
  selftest

Exit 0 = pass, 1 = fail, 2 = usage.
"""
import glob
import os
import re
import struct
import sys
import tempfile
import zlib

MAX_SAMPLES = 64  # s12Frame_ % 64 -> independent jitter samples are capped


# ---------------------------------------------------------------- PNG reader
def read_png(path):
    """Return (w, h, ch, raw unfiltered bytes) for an 8-bit non-interlaced PNG."""
    b = open(path, "rb").read()
    if b[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos, idat = 8, b""
    w = h = bd = ct = None
    inter = 1
    while pos < len(b):
        ln = struct.unpack(">I", b[pos:pos + 4])[0]
        typ = b[pos + 4:pos + 8]
        data = b[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bd, ct, _comp, _filt, inter = struct.unpack(">IIBBBBB", data)
        elif typ == b"IDAT":
            idat += data
        elif typ == b"IEND":
            break
    if bd != 8 or inter != 0:
        raise ValueError("only 8-bit non-interlaced PNG supported: %s" % path)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ct)
    if ch is None:
        raise ValueError("unsupported PNG colour type %s: %s" % (ct, path))
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            bb = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + bb) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + bb) // 2)) & 255
            elif f == 4:
                pp = a + bb - c
                pa, pb, pc = abs(pp - a), abs(pp - bb), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (bb if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
            elif f != 0:
                raise ValueError("unsupported PNG filter %d" % f)
        out += line
        prev = line
    return w, h, ch, bytes(out)


def write_png_rgb(path, w, h, rgb):
    """Write an 8-bit RGB PNG with filter 0 (pure stdlib)."""
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as fp:
        fp.write(png)


def lum(t):
    """Mean of the available colour channels per pixel (M12.0/12.1 metric)."""
    w, h, ch, raw = t
    if ch == 1:
        return list(raw)
    take = min(3, ch)
    return [sum(raw[i:i + take]) / take for i in range(0, len(raw), ch)]


# ---------------------------------------------------------------- metrics
def highfreq(v, w, h):
    """Mean |dI/dx| + |dI/dy| over a half-resolution lattice (M12.0 metric)."""
    s, n = 0.0, 0
    for y in range(0, h - 1, 2):
        row = y * w
        for x in range(0, w - 1, 2):
            s += abs(v[row + x] - v[row + x + 1]) + abs(v[row + x] - v[row + w + x])
            n += 2
    return s / max(n, 1)


def mae(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / max(len(a), 1)


def diff_pixels(a, b, threshold=2.0):
    return sum(1 for x, y in zip(a, b) if abs(x - y) > threshold)


# ---------------------------------------------------------------- D-B mean
def seq_files(seqdir, n):
    out = []
    for p in glob.glob(os.path.join(seqdir, "f*.png")):
        m = re.match(r"f(\d+)\.png$", os.path.basename(p))
        if m:
            out.append((int(m.group(1)), p))
    out.sort()
    return [p for _k, p in out[:n]]


def cmd_avg(out_path, seqdir, n):
    if n < 1 or n > MAX_SAMPLES:
        print("[s12-done] FAIL: n=%d outside 1..%d (jitter index s12Frame_ %% %d)"
              % (n, MAX_SAMPLES, MAX_SAMPLES))
        return 1
    files = seq_files(seqdir, n)
    if len(files) < n:
        print("[s12-done] FAIL: only %d/%d frames in %s" % (len(files), n, seqdir))
        return 1
    w = h = None
    sums = None
    for p in files:
        ww, hh, ch, raw = read_png(p)
        if w is None:
            w, h = ww, hh
            sums = [0] * (w * h * 3)
        elif (ww, hh) != (w, h):
            print("[s12-done] FAIL: size mismatch in %s" % p)
            return 1
        rgb = bytearray(w * h * 3)
        if ch >= 3:
            rgb[0::3] = raw[0::ch]
            rgb[1::3] = raw[1::ch]
            rgb[2::3] = raw[2::ch]
        else:
            c = raw[0::ch]
            rgb[0::3] = c
            rgb[1::3] = c
            rgb[2::3] = c
        sums = [s + v for s, v in zip(sums, rgb)]
    pixels = bytes(max(0, min(255, int(s / n + 0.5))) for s in sums)
    write_png_rgb(out_path, w, h, pixels)
    print("[s12-done] avg: %d frames -> %s (%dx%d, D-B S12-owned reference)"
          % (n, out_path, w, h))
    return 0


# ---------------------------------------------------------------- static gate
def refcheck_gate(off, on, ref, mae_ratio, max_mae, hf_ratio, quiet=False):
    ow, oh, o = off
    nw, nh, n = on
    rw, rh, r = ref
    if (ow, oh) != (nw, nh) or (ow, oh) != (rw, rh):
        if not quiet:
            print("[s12-done] FAIL: size mismatch")
        return False
    mo, mn = mae(o, r), mae(n, r)
    ho, hn = highfreq(o, ow, oh), highfreq(n, nw, nh)
    d = diff_pixels(o, n)
    if not quiet:
        print("[s12-done] mae off=%.3f on=%.3f (limit %.3f x off, abs %.3f)"
              % (mo, mn, mae_ratio, max_mae))
        print("[s12-done] hf  off=%.3f on=%.3f (limit %.3f x off)"
              % (ho, hn, hf_ratio))
        print("[s12-done] diffPixels(on,off)=%d [static: 0 expected, "
              "rejection cannot fire with velocity=0]" % d)
    ok = True
    if mn > mae_ratio * mo + 1e-6:
        if not quiet:
            print("[s12-done] FAIL: ON drifted from the D-B reference")
        ok = False
    if mn > max_mae + 1e-6:
        if not quiet:
            print("[s12-done] FAIL: ON too far from the D-B reference")
        ok = False
    if hn > hf_ratio * ho + 1e-6:
        if not quiet:
            print("[s12-done] FAIL: ON is noisier than OFF")
        ok = False
    if ok and not quiet:
        print("[s12-done] OK: history rejection preserves D-B convergence")
    return ok


def cmd_refcheck(ref_p, off_p, on_p, mae_ratio, max_mae, hf_ratio):
    def load(p):
        t = read_png(p)
        return (t[0], t[1], lum(t))
    return 0 if refcheck_gate(load(off_p), load(on_p), load(ref_p),
                              mae_ratio, max_mae, hf_ratio) else 1


# ---------------------------------------------------------------- motion gate
def ghost_gate(rows, ratio_limit, quiet=False):
    """rows = [(mae(ON,SINGLE), mae(OFF,SINGLE), diff(ON,OFF), diff(REJALL,OFF))]."""
    if not rows:
        if not quiet:
            print("[s12-done] FAIL: empty motion sequence")
        return False
    k = float(len(rows))
    m_on = sum(r[0] for r in rows) / k
    m_off = sum(r[1] for r in rows) / k
    d_on = sum(r[2] for r in rows) / k
    d_rej = sum(r[3] for r in rows) / k
    ratio = m_on / max(m_off, 1e-6)
    if not quiet:
        print("[s12-done] ghost frames=%d meanMAE(on,single)=%.3f "
              "meanMAE(off,single)=%.3f ratio=%.3f (limit %.3f)"
              % (len(rows), m_on, m_off, ratio, ratio_limit))
        print("[s12-done] layer response meanDiff(on,off)=%.1f "
              "meanDiff(rejectAll,off)=%.1f (latter must be greater)"
              % (d_on, d_rej))
    ok = True
    if m_off <= 1e-6:
        if not quiet:
            print("[s12-done] FAIL: OFF == SINGLE on every frame "
                  "(no history accumulated / sequence not moving)")
        ok = False
    if ratio > ratio_limit + 1e-6:
        if not quiet:
            print("[s12-done] FAIL: rejection did not bring ON closer to the "
                  "same-frame no-history baseline (ghosting)")
        ok = False
    if d_on <= 0:
        if not quiet:
            print("[s12-done] FAIL: ON == OFF on every frame "
                  "(rejection layer did not run)")
        ok = False
    if d_rej <= d_on:
        if not quiet:
            print("[s12-done] FAIL: threshold response missing "
                  "(over-strict threshold must reject more history)")
        ok = False
    if ok and not quiet:
        print("[s12-done] OK: motion sequence tracks the no-history baseline, "
              "rejection layer responds to its threshold")
    return ok


def cmd_ghost(dirs, n, ratio_limit):
    on_d, off_d, single_d, rejall_d = dirs
    rows = []
    for k in range(1, n + 1):
        name = "f%05d.png" % k
        paths = [os.path.join(d, name) for d in dirs]
        missing = [p for p in paths if not os.path.isfile(p)]
        if missing:
            print("[s12-done] FAIL: missing frame %s" % missing[0])
            return 1
        vals = [lum(read_png(p)) for p in paths]
        on, off, single, rejall = vals
        rows.append((mae(on, single), mae(off, single),
                     diff_pixels(on, off), diff_pixels(rejall, off)))
    return 0 if ghost_gate(rows, ratio_limit) else 1


# ----------------------------------------------------- disocclusion-limited gate
def disocc_gate(agg, ratio_limit, min_frac, min_ghost, quiet=False):
    """agg = {maskCount, nPix, sumOn, sumOff} pooled across frames.

    mask(p) = |off(p) - single(p)| > disTol  (history disagreed with the
    current no-history truth = disocclusion / occluded-boundary region).
    sumOn   = sum |on(p) - single(p)|     over masked pixels
    sumOff  = sum |off(p) - single(p)|    over masked pixels  (= ghost)
    """
    frac = agg["maskCount"] / max(agg["nPix"], 1)
    mOn = agg["sumOn"] / max(agg["maskCount"], 1)
    mOff = agg["sumOff"] / max(agg["maskCount"], 1)
    ratio = mOn / max(mOff, 1e-6)
    if not quiet:
        print("[s12-multiscene] disocc masked frac=%.4f (%d/%d px)"
              % (frac, agg["maskCount"], agg["nPix"]))
        print("[s12-multiscene] masked meanMAE(on,single)=%.3f "
              "meanMAE(off,single)=%.3f ratio=%.3f (limit %.3f)"
              % (mOn, mOff, ratio, ratio_limit))
    ok = True
    if frac < min_frac:
        if not quiet:
            print("[s12-multiscene] FAIL: disocclusion mask too small "
                  "(%.4f < %.4f) - scene/motion did not expose disocclusion"
                  % (frac, min_frac))
        ok = False
    if mOff < min_ghost:
        if not quiet:
            print("[s12-multiscene] FAIL: OFF does not ghost in the mask "
                  "(%.3f < %.3f) - mask is not a real ghost region"
                  % (mOff, min_ghost))
        ok = False
    if ratio > ratio_limit + 1e-6:
        if not quiet:
            print("[s12-multiscene] FAIL: ON did not track the no-history "
                  "baseline better than OFF in disocclusion regions")
        ok = False
    if ok and not quiet:
        print("[s12-multiscene] OK: rejection layer preserves edges in "
              "disocclusion regions (ON tracks no-history baseline)")
    return ok


def cmd_disocc(dirs, n, ratio_limit, dis_tol, min_frac, min_ghost):
    on_d, off_d, single_d, rejall_d = dirs
    agg = {"maskCount": 0, "nPix": 0, "sumOn": 0.0, "sumOff": 0.0}
    for k in range(1, n + 1):
        name = "f%05d.png" % k
        # rejall is accepted for interface symmetry but unused by the gate.
        paths = [os.path.join(dirs[0], name), os.path.join(dirs[1], name),
                 os.path.join(dirs[2], name), os.path.join(dirs[3], name)]
        if not (os.path.isfile(paths[0]) and os.path.isfile(paths[1])
                and os.path.isfile(paths[2]) and os.path.isfile(paths[3])):
            print("[s12-multiscene] FAIL: missing frame %s" % name)
            return 1
        on = lum(read_png(paths[0]))
        off = lum(read_png(paths[1]))
        single = lum(read_png(paths[2]))
        w = len(on)
        for i in range(w):
            d_os = abs(off[i] - single[i])
            if d_os > dis_tol:
                agg["maskCount"] += 1
                agg["sumOn"] += abs(on[i] - single[i])
                agg["sumOff"] += d_os
        agg["nPix"] += w
    return 0 if disocc_gate(agg, ratio_limit, min_frac, min_ghost) else 1


# ---------------------------------------------------------------- selftest
def selftest():
    ok = True
    # (1) avg writes a readable PNG whose value is the arithmetic mean.
    with tempfile.TemporaryDirectory() as td:
        for i, v in enumerate((0, 128, 255), start=1):
            write_png_rgb(os.path.join(td, "f%05d.png" % i), 2, 2,
                          bytes([v]) * (2 * 2 * 3))
        out = os.path.join(td, "avg.png")
        rc = cmd_avg(out, td, 3)
        got = lum(read_png(out))
        expect = 127.0  # (0+128+255)/3 = 127.67 -> rounds to 128; allow 1
        avg_ok = rc == 0 and all(abs(x - 128.0) <= 1.0 for x in got)
        print("[s12-done] selftest avg=%s (mean %.1f -> %s)"
              % (avg_ok, expect, got[0]))
        ok = ok and avg_ok
        over = cmd_avg(os.path.join(td, "over.png"), td, 65)
        print("[s12-done] selftest avg-N>64 rejected=%s" % (over == 1))
        ok = ok and over == 1
    # (2) refcheck: positive / drift negative / noise negative.
    w = h = 32
    ref = (w, h, [100.0] * (w * h))
    off = (w, h, [100.0 + (1.0 if i % 2 else -1.0) for i in range(w * h)])
    pos = refcheck_gate(off, off, ref, 1.05, 5.0, 1.05, quiet=True)
    drift = refcheck_gate(off, (w, h, [200.0] * (w * h)), ref,
                          1.05, 5.0, 1.05, quiet=True)
    noisy = (w, h, [100.0 + (30.0 if i % 2 else -30.0) for i in range(w * h)])
    noise = refcheck_gate(off, noisy, ref, 1.05, 5.0, 1.05, quiet=True)
    print("[s12-done] selftest refcheck positive=%s negative-drift=%s "
          "negative-noise=%s" % (pos, not drift, not noise))
    ok = ok and pos and not drift and not noise
    # (3) ghost: positive / ratio negative / layer-response negative.
    good = [(1.0, 2.0, 100, 500)] * 4
    bad_ratio = [(3.0, 2.0, 100, 500)] * 4
    bad_layer = [(1.0, 2.0, 100, 100)] * 4
    g1 = ghost_gate(good, 0.98, quiet=True)
    g2 = ghost_gate(bad_ratio, 0.98, quiet=True)
    g3 = ghost_gate(bad_layer, 0.98, quiet=True)
    print("[s12-done] selftest ghost positive=%s negative-ratio=%s "
          "negative-layer=%s" % (g1, not g2, not g3))
    ok = ok and g1 and not g2 and not g3
    # (4) disocc: positive / ratio negative / mask-too-small negative.
    N = 64
    single_v = [100.0] * N
    off_v = [200.0 if i < 32 else 100.0 for i in range(N)]   # ghosts region
    on_v = [120.0 if i < 32 else 100.0 for i in range(N)]    # closer to truth
    def masked_sums(vec):
        return sum(abs(vec[i] - single_v[i])
                   for i in range(N) if abs(off_v[i] - single_v[i]) > 8.0)
    pos_agg = {"maskCount": 32, "nPix": N,
               "sumOn": masked_sums(on_v), "sumOff": masked_sums(off_v)}
    d1 = disocc_gate(pos_agg, 0.95, 0.001, 6.0, quiet=True)
    on_bad = [200.0 if i < 32 else 100.0 for i in range(N)]   # == off -> ratio 1
    neg_agg = {"maskCount": 32, "nPix": N,
               "sumOn": masked_sums(on_bad), "sumOff": pos_agg["sumOff"]}
    d2 = disocc_gate(neg_agg, 0.95, 0.001, 6.0, quiet=True)
    tiny_agg = {"maskCount": 0, "nPix": N * 100,
                "sumOn": 0.0, "sumOff": 0.0}
    d3 = disocc_gate(tiny_agg, 0.95, 0.001, 6.0, quiet=True)
    print("[s12-done] selftest disocc positive=%s negative-ratio=%s "
          "negative-mask=%s" % (d1, not d2, not d3))
    ok = ok and d1 and not d2 and not d3
    print("[s12-done] selftest %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------- main
def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        return selftest()
    if cmd == "avg" and len(argv) >= 4:
        n = int(argv[4]) if len(argv) > 4 else 32
        return cmd_avg(argv[2], argv[3], n)
    if cmd == "refcheck" and len(argv) >= 5:
        mr = float(argv[5]) if len(argv) > 5 else 1.05
        mm = float(argv[6]) if len(argv) > 6 else 10.0
        hr = float(argv[7]) if len(argv) > 7 else 1.05
        return cmd_refcheck(argv[2], argv[3], argv[4], mr, mm, hr)
    if cmd == "ghost" and len(argv) >= 6:
        n = int(argv[6]) if len(argv) > 6 else 16
        rl = float(argv[7]) if len(argv) > 7 else 0.98
        return cmd_ghost(argv[2:6], n, rl)
    if cmd == "disocc" and len(argv) >= 6:
        n = int(argv[6]) if len(argv) > 6 else 16
        rl = float(argv[7]) if len(argv) > 7 else 0.95
        tol = float(argv[8]) if len(argv) > 8 else 8.0
        mf = float(argv[9]) if len(argv) > 9 else 0.0005
        mg = float(argv[10]) if len(argv) > 10 else 6.0
        return cmd_disocc(argv[2:6], n, rl, tol, mf, mg)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
