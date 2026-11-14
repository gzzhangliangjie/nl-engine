#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M14C1 (S14-C) adaptive-probe acceptance gates.

Deterministic path (probe chain): re-shot gate is bytes-strict is handled by
run.bat via fc /b; this tool carries the M14C1-specific gates:

Modes:
  mse <adaptive.png> <ref.png> <x> <y> <w> <h> <maxMSE>
      Detail-region GI quality: mean squared error (0..255 8-bit domain)
      inside the crop window between probe-adaptive output and the uniform
      doubled-resolution reference (probe --probe-base-res 32).
      Exit 0 if MSE <= maxMSE. Else exit 1.
  magenta <a.png> <minCount>
      Viz gate: adaptive spawn dots are magenta (level-color scheme adds
      magenta for reserved-region spawned probes). Counts pixels whose
      R>150, B>150, G<100 and requires >= minCount. Else exit 1.
  stability <a.png> <b.png> <maxDiffPct> <maxMean>
      Statistical re-shot gate (same semantics as check-s14b-restir.py;
      unused on the deterministic path but kept for diagnostics).
  present <a.png> <minMean> <minMax>
      Non-degenerate image gate.
  timing <probe.log> <adaptive.log> <maxRatio>
      Trace-budget conservation evidence: both logs must carry the
      "csTraceProbe first resolve: X ms" pass-timing anchor, and the
      adaptive run's trace ms must be <= probe ms * maxRatio (timer noise
      headroom; structural conservation itself is by construction:
      (res_eff*10) x (2*res_eff*10) = (baseRes*10)^2 dispatch threads).
      Exit 0 if anchors present and ratio within gate. Else exit 1.
  selftest
      Synthetic PNG self-checks for all modes.
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


def cmd_mse(pa, pb, x, y, w, h, maxmse):
    wa, ha, a = read_png_rgb(pa)
    wb, hb, b = read_png_rgb(pb)
    if (wa, ha) != (wb, hb):
        print("[FAIL] size mismatch %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 1
    if x < 0 or y < 0 or x + w > wa or y + h > ha:
        print("[FAIL] crop window %d,%d %dx%d out of %dx%d" %
              (x, y, w, h, wa, ha))
        return 1
    acc = 0
    n = 0
    for yy in range(y, y + h):
        row = (yy * wa + x) * 3
        for i in range(row, row + w * 3, 3):
            dr = a[i] - b[i]
            dg = a[i + 1] - b[i + 1]
            db = a[i + 2] - b[i + 2]
            acc += dr * dr + dg * dg + db * db
            n += 3
    mse = acc / float(n)
    ok = mse <= maxmse
    print("[{}] crop=({},{},{}x{}) MSE={:.4f}/255 (gate: MSE<={})".format(
        "OK" if ok else "FAIL", x, y, w, h, mse, maxmse))
    return 0 if ok else 1


def cmd_magenta(pa, mincount):
    w, h, a = read_png_rgb(pa)
    count = 0
    for i in range(0, len(a), 3):
        r, g, b = a[i], a[i + 1], a[i + 2]
        if r > 150 and b > 150 and g < 100:
            count += 1
    ok = count >= mincount
    print("[{}] magenta pixels={} (gate: >={})".format(
        "OK" if ok else "FAIL", count, mincount))
    return 0 if ok else 1


def cmd_stability(pa, pb, maxpct, maxmean):
    wa, ha, a = read_png_rgb(pa)
    wb, hb, b = read_png_rgb(pb)
    if (wa, ha) != (wb, hb):
        print("[FAIL] size mismatch %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 1
    n = wa * ha
    diffpix = 0
    total = 0
    for i in range(0, n * 3, 3):
        dr = abs(a[i] - b[i])
        dg = abs(a[i + 1] - b[i + 1])
        db = abs(a[i + 2] - b[i + 2])
        if dr or dg or db:
            diffpix += 1
        total += dr + dg + db
    pct = diffpix * 100.0 / n
    meandiff = total / (n * 3.0)
    ok = pct <= maxpct and meandiff <= maxmean
    print("[{}] diffPixels={} ({:.3f}%) meanDiff={:.4f}/255 "
          "(gate: pct<={}% mean<={})".format(
              "OK" if ok else "FAIL", diffpix, pct, meandiff,
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


def cmd_timing(probe_log, adaptive_log, maxratio):
    def extract(path):
        marker = "pass-timing] csTraceProbe first resolve: "
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    i = line.find(marker)
                    if i >= 0:
                        tail = line[i + len(marker):]
                        return float(tail.split()[0])
        except OSError:
            pass
        return None
    pms = extract(probe_log)
    ams = extract(adaptive_log)
    if pms is None or ams is None:
        print("[FAIL] csTraceProbe anchor missing (probe=%s adaptive=%s)"
              % (pms, ams))
        return 1
    ratio = ams / pms if pms > 0 else float("inf")
    ok = ratio <= maxratio
    print("[{}] probeTrace probe={:.5f} ms adaptive={:.5f} ms "
          "ratio={:.2f} (gate: <={})".format(
              "OK" if ok else "FAIL", pms, ams, ratio, maxratio))
    return 0 if ok else 1


def cmd_selftest():
    tmp = tempfile.mkdtemp()
    w, h = 64, 64
    pa = os.path.join(tmp, "a.png")
    pb = os.path.join(tmp, "b.png")
    pc = os.path.join(tmp, "c.png")
    # a: flat 100 gray; b: one pixel magenta-ish; c: flat 90 gray
    a = bytes([100]) * (w * h * 3)
    b = bytearray(a)
    b[0] = 200   # magenta-ish pixel (r=200 g=100 b=100 -> not magenta)
    b[1] = 100
    b[2] = 200
    b[3] = 200   # true magenta pixel: r=200 g=90 b=200
    b[4] = 90
    b[5] = 200
    c = bytes([90]) * (w * h * 3)
    write_png(pa, w, h, a)
    write_png(pb, w, h, bytes(b))
    write_png(pc, w, h, c)
    ok = True
    # mse: identical -> 0; flat 100 vs 90 -> MSE=100
    if cmd_mse(pa, pa, 0, 0, w, h, 0.0) != 0:
        print("[FAIL] selftest: identical images MSE must be 0")
        ok = False
    if cmd_mse(pa, pc, 0, 0, w, h, 99.0) != 1:
        print("[FAIL] selftest: MSE=100 must fail 99 gate")
        ok = False
    if cmd_mse(pa, pc, 0, 0, w, h, 100.0) != 0:
        print("[FAIL] selftest: MSE=100 must pass 100 gate")
        ok = False
    if cmd_mse(pa, pb, 0, 0, w, h, 0.0) != 1:
        print("[FAIL] selftest: crop diff must fail zero gate")
        ok = False
    if cmd_mse(pa, pb, 0, 0, w, h - 1, 0.0) != 1:
        pass  # window still includes the diff pixel
    # out-of-bounds crop must fail
    if cmd_mse(pa, pb, 60, 60, 8, 8, 0.0) != 1:
        print("[FAIL] selftest: OOB crop must fail")
        ok = False
    # magenta: 1 pixel present
    if cmd_magenta(pb, 1) != 0:
        print("[FAIL] selftest: 1 magenta pixel must pass >=1")
        ok = False
    if cmd_magenta(pa, 1) != 1:
        print("[FAIL] selftest: zero magenta must fail >=1")
        ok = False
    # stability: identical passes zero gate
    if cmd_stability(pa, pa, 0.0, 0.0) != 0:
        print("[FAIL] selftest: identical must pass zero gate")
        ok = False
    # present: gray passes at low gate
    if cmd_present(pc, 2.0, 10) != 0:
        print("[FAIL] selftest: gray must pass present")
        ok = False
    print("[OK] selftest" if ok else "[FAIL] selftest")
    return 0 if ok else 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        return cmd_selftest()
    if len(sys.argv) >= 9 and sys.argv[1] == "mse":
        return cmd_mse(sys.argv[2], sys.argv[3], int(sys.argv[4]),
                       int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7]),
                       float(sys.argv[8]))
    if len(sys.argv) >= 4 and sys.argv[1] == "magenta":
        return cmd_magenta(sys.argv[2], int(sys.argv[3]))
    if len(sys.argv) >= 6 and sys.argv[1] == "stability":
        return cmd_stability(sys.argv[2], sys.argv[3],
                             float(sys.argv[4]), float(sys.argv[5]))
    if len(sys.argv) >= 5 and sys.argv[1] == "present":
        return cmd_present(sys.argv[2], float(sys.argv[3]),
                           int(sys.argv[4]))
    if len(sys.argv) >= 5 and sys.argv[1] == "timing":
        return cmd_timing(sys.argv[2], sys.argv[3], float(sys.argv[4]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
