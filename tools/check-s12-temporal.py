#!/usr/bin/env python3
"""S12 M12.0 temporal-accumulation acceptance gates (pure stdlib, no PIL).

Why a custom PNG reader: this workstation has two Python installs and PIL is
not guaranteed for the `python` on PATH. viewer.exe --screenshot emits 8-bit
non-interlaced PNG (RGB/RGBA/grey), which is all we need here.

Background (Q12-E): M10.1 traces 64 fixed octahedron directions per probe, so
a static scene has zero frame-to-frame GI variance and lerp(hist,cur,alpha) is
mathematically identical to cur. M12.0 therefore re-samples the probe
directions per frame in --mode filter only, and the acceptance object is the
GI component itself (--filter-gi-dump), not the composite image.

Subcommands:
  onoff <off.png> <on.png> [hfRatio] [minDiff]
      OFF = single-frame jittered GI (--temporal-off), ON = accumulated GI.
      Gate: ON differs from OFF, and ON high-frequency energy <= hfRatio*OFF.

  onoffref <ref.png> <off.png> <on.png> [hfRatio] [maeRatio] [minDiff]
      Same as onoff plus MAE(ON,ref) <= maeRatio*MAE(OFF,ref), where ref is
      the no-jitter GI dump (--mode probe --filter-gi-dump). Accumulation must
      move the noisy frame toward the deterministic reference.

  selftest
      Negative + positive gate checks on synthetic buffers (no files needed).

Exit 0 = pass, 1 = fail, 2 = usage.
"""
import struct
import sys
import zlib


# ---------------------------------------------------------------- PNG reader
def read_png_lum(path):
    """Return (w, h, [luminance per pixel]) for an 8-bit non-interlaced PNG."""
    b = open(path, "rb").read()
    if b[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos, idat = 8, b""
    w = h = bd = ct = None
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
    take = min(3, ch)
    lum = [sum(out[i:i + take]) / take for i in range(0, len(out), ch)]
    return w, h, lum


# ---------------------------------------------------------------- metrics
def highfreq(lum, w, h):
    """Mean |dI/dx| + |dI/dy| over a half-resolution lattice (matches the
    M12.0 measurement used when the gate thresholds were chosen)."""
    s, n = 0.0, 0
    for y in range(0, h - 1, 2):
        row = y * w
        for x in range(0, w - 1, 2):
            v = lum[row + x]
            s += abs(v - lum[row + x + 1]) + abs(v - lum[row + w + x])
            n += 2
    return s / max(n, 1)


def mae(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / max(len(a), 1)


def diff_pixels(a, b, threshold=2.0):
    return sum(1 for x, y in zip(a, b) if abs(x - y) > threshold)


# ---------------------------------------------------------------- gates
def gate(off, on, ref=None, hf_ratio=0.88, mae_ratio=0.90, min_diff=1000,
         quiet=False):
    """off/on/ref are (w, h, lum-list) tuples. Return True when all gates pass."""
    ow, oh, o = off
    nw, nh, n = on
    if (ow, oh) != (nw, nh):
        if not quiet:
            print("[s12-temporal] FAIL: size mismatch %dx%d vs %dx%d"
                  % (ow, oh, nw, nh))
        return False
    ok = True
    d = diff_pixels(o, n)
    hf_o, hf_n = highfreq(o, ow, oh), highfreq(n, nw, nh)
    ratio = hf_n / max(hf_o, 1e-6)
    if not quiet:
        print("[s12-temporal] hf off=%.3f on=%.3f ratio=%.3f (limit %.3f)"
              % (hf_o, hf_n, ratio, hf_ratio))
        print("[s12-temporal] diffPixels=%d (min %d)" % (d, min_diff))
    if d < min_diff:
        if not quiet:
            print("[s12-temporal] FAIL: ON and OFF are structurally identical "
                  "(static ON==OFF trap, M11.2/M12.0 family)")
        ok = False
    if ratio > hf_ratio:
        if not quiet:
            print("[s12-temporal] FAIL: high-frequency energy did not converge")
        ok = False
    if ref is not None:
        rw, rh, r = ref
        if (rw, rh) != (ow, oh):
            if not quiet:
                print("[s12-temporal] FAIL: ref size mismatch")
            return False
        mo, mn = mae(o, r), mae(n, r)
        mr = mn / max(mo, 1e-6)
        if not quiet:
            print("[s12-temporal] mae off=%.3f on=%.3f ratio=%.3f (limit %.3f)"
                  % (mo, mn, mr, mae_ratio))
        if mr > mae_ratio:
            if not quiet:
                print("[s12-temporal] FAIL: ON not closer to no-jitter reference")
            ok = False
    if ok and not quiet:
        print("[s12-temporal] OK: temporal accumulation converges "
              "(noise down, GI preserved)")
    return ok


def selftest():
    w = h = 64
    # off = strong checkerboard noise, on = flat, ref = flat deterministic.
    off = (w, h, [128.0 + (40.0 if (i // w + i % w) % 2 else -40.0)
                  for i in range(w * h)])
    on = (w, h, [128.0] * (w * h))
    ref = (w, h, [128.0] * (w * h))
    positive = gate(off, on, ref, quiet=True)
    negative = gate(off, off, ref, quiet=True)
    neg_ref = gate(off, off, quiet=True)
    print("[s12-temporal] selftest positive=%s negative-onoff=%s negative-onoffref=%s"
          % (positive, not negative, not neg_ref))
    if positive and not negative and not neg_ref:
        print("[s12-temporal] selftest OK")
        return 0
    print("[s12-temporal] selftest FAIL")
    return 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        return selftest()
    if cmd == "onoff" and len(argv) >= 4:
        hf = float(argv[4]) if len(argv) > 4 else 0.88
        md = int(argv[5]) if len(argv) > 5 else 1000
        off, on = read_png_lum(argv[2]), read_png_lum(argv[3])
        return 0 if gate(off, on, None, hf, 0.90, md) else 1
    if cmd == "onoffref" and len(argv) >= 5:
        hf = float(argv[5]) if len(argv) > 5 else 0.88
        mr = float(argv[6]) if len(argv) > 6 else 0.90
        md = int(argv[7]) if len(argv) > 7 else 1000
        ref, off, on = (read_png_lum(argv[2]), read_png_lum(argv[3]),
                        read_png_lum(argv[4]))
        return 0 if gate(off, on, ref, hf, mr, md) else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
