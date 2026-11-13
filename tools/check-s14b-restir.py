#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M14B2 (S14-B) restir acceptance gates.

Why statistical (not bytes): restir mode consults the S11 world_cache
indirect term (Q-B1 semantic). The cache chain in restir mode is run-to-run
nondeterministic (S11 hashmap CAS insert races under warp scheduling;
measured 0.16% pixels / max 23/255 between fresh-process reruns of the same
exe, DX12). Per M14B2 acceptance record (option A), the re-shot gate is
statistical: diff-pixel ratio and mean-abs-diff thresholds calibrated from
the measured rerun spread. Same-backend determinism was proven when the
cache chain is disabled (diagnostic bisect, two fresh runs byte-identical).

Modes:
  stability <a.png> <b.png> <maxDiffPct> <maxMean>
      Exit 0 if pixel-diff ratio (%) <= maxDiffPct and mean abs diff
      (0..255) <= maxMean. Else exit 1.
  present <a.png> <minMean> <minMax>
      Exit 0 if mean > minMean and max > minMax (image not degenerate/black).
      Else exit 1.
  selftest
      Synthetic PNG self-checks for both modes.
"""
import sys
import struct
import zlib
import tempfile
import os


def read_png_rgb(path):
    """Minimal stdlib PNG reader: 8-bit RGB/RGBA, non-interlaced."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos = 8
    width = height = None
    bitdepth = colortype = None
    idat = b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(
                ">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if bitdepth != 8 or colortype not in (2, 6):
        raise ValueError("unsupported PNG (depth %s type %s)" %
                         (bitdepth, colortype))
    raw = zlib.decompress(idat)
    nch = 3 if colortype == 2 else 4
    stride = width * nch
    out = bytearray(height * width * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (
                    b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        prev = line
        for x in range(width):
            o = (y * width + x) * 3
            s = x * nch
            out[o] = line[s]
            out[o + 1] = line[s + 1]
            out[o + 2] = line[s + 2]
    return width, height, bytes(out)


def diff_stats(wa, ha, a, wb, hb, b):
    if (wa, ha) != (wb, hb):
        return None
    n = wa * ha
    diffpix = 0
    total = 0
    mx = 0
    for i in range(0, n * 3, 3):
        dr = abs(a[i] - b[i])
        dg = abs(a[i + 1] - b[i + 1])
        db = abs(a[i + 2] - b[i + 2])
        m = dr if dr > dg else dg
        if db > m:
            m = db
        if m:
            diffpix += 1
            mx = m if m > mx else mx
        total += dr + dg + db
    return diffpix, total / (n * 3.0), mx


def write_png(path, w, h, rgb):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    stride = w * 3
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw))))
        f.write(chunk(b"IEND", b""))


def cmd_stability(pa, pb, maxpct, maxmean):
    wa, ha, a = read_png_rgb(pa)
    wb, hb, b = read_png_rgb(pb)
    st = diff_stats(wa, ha, a, wb, hb, b)
    if st is None:
        print("[FAIL] size mismatch %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 1
    diffpix, meandiff, maxd = st
    pct = diffpix * 100.0 / (wa * ha)
    ok = pct <= maxpct and meandiff <= maxmean
    print("[{}] diffPixels={} ({:.3f}%) meanDiff={:.4f}/255 maxDelta={}/255 "
          "(gate: pct<={}% mean<={})".format(
              "OK" if ok else "FAIL", diffpix, pct, meandiff, maxd,
              maxpct, maxmean))
    return 0 if ok else 1


def cmd_present(pa, minmean, minmax):
    w, h, a = read_png_rgb(pa)
    n = w * h
    total = 0
    mx = 0
    for v in a:
        if v > mx:
            mx = v
        total += v
    mean = total / (n * 3.0)
    ok = mean > minmean and mx > minmax
    print("[{}] mean={:.2f}/255 max={}/255 (gate: mean>{} max>{})".format(
        "OK" if ok else "FAIL", mean, mx, minmean, minmax))
    return 0 if ok else 1


def cmd_selftest():
    tmp = tempfile.mkdtemp()
    pa = os.path.join(tmp, "a.png")
    pb = os.path.join(tmp, "b.png")
    pc = os.path.join(tmp, "c.png")
    # a: flat 100 gray; b: 2 pixels off by 1 -> stability ok at 0.5%/0.5
    w, h = 64, 64
    a = bytes([100]) * (w * h * 3)
    b = bytearray(a)
    b[0] = 101
    b[3] = 99
    c = bytes([0]) * (w * h * 3)  # black -> present must fail at minmean 2
    d = bytes([60]) * (w * h * 3)  # visible -> present ok at minmean 2
    write_png(pa, w, h, a)
    write_png(pb, w, h, bytes(b))
    pc1 = os.path.join(tmp, "c1.png")
    pc2 = os.path.join(tmp, "c2.png")
    write_png(pc1, w, h, c)
    write_png(pc2, w, h, d)
    ok = True
    # stability: identical
    if cmd_stability(pa, pa, 0.5, 0.5) != 0:
        print("[FAIL] selftest: identical images must pass")
        ok = False
    # stability: 2 pixels off (0.049%) -> pass at 0.5 gate, fail at 0.0 gate
    if cmd_stability(pa, pb, 0.5, 0.5) != 0:
        print("[FAIL] selftest: tiny diff must pass loose gate")
        ok = False
    if cmd_stability(pa, pb, 0.0, 0.0) != 1:
        print("[FAIL] selftest: tiny diff must fail zero gate")
        ok = False
    # present: black fails, gray passes
    if cmd_present(pc1, 2.0, 10) != 1:
        print("[FAIL] selftest: black must fail present")
        ok = False
    if cmd_present(pc2, 2.0, 10) != 0:
        print("[FAIL] selftest: gray must pass present")
        ok = False
    print("[OK] selftest" if ok else "[FAIL] selftest")
    return 0 if ok else 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        return cmd_selftest()
    if len(sys.argv) >= 6 and sys.argv[1] == "stability":
        return cmd_stability(sys.argv[2], sys.argv[3],
                             float(sys.argv[4]), float(sys.argv[5]))
    if len(sys.argv) >= 5 and sys.argv[1] == "present":
        return cmd_present(sys.argv[2], float(sys.argv[3]),
                           int(sys.argv[4]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
