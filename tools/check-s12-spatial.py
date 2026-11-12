#!/usr/bin/env python3
"""S12 M12.1 spatial joint-bilateral acceptance gates (pure stdlib, no PIL).

Object under test: the GI component dumped by --filter-gi-dump (Q12-E / M11.2
methodology). M12.0 already accumulated GI temporally; M12.1 adds one 5x5
joint-bilateral pass AFTER temporal (Q12-C = A). The gate therefore compares

    REF = --mode probe --filter-gi-dump          (no jitter, deterministic)
    OFF = --mode filter --filter-gi-dump         (temporal only, spatial OFF)
    ON  = --mode filter --spatial --filter-gi-dump (temporal + spatial)

All three must use --dist 4: at --dist 7 the gather weights underflow and
gatherLinear_ is exactly zero (M12.0 root cause #3).

Gates (Q11-1: GPU numerical images are NOT byte-exact across backends, so all
assertions are statistical invariants, never bytes):
  * ON != OFF                       -> spatial layer really ran
  * hf(ON) <= hfRatio * hf(OFF)     -> single-frame noise drops further
  * MAE(ON,ref) <= maeRatio*MAE(OFF,ref) -> no severe structural drift.
    Joint bilateral is a BIASED estimator (neighbour averaging trades variance
    for bias), so a modest MAE increase vs the no-jitter reference is EXPECTED;
    the limit is calibrated from measurement (M12.0 precedent), not guessed.
  * edgeGradRatio >= edgeMin -> high-gradient pixels keep their gradient,
    i.e. the joint weights did not blur edges flat (S12-PLAN M12.1 self-check).
    Measured 0.493 on both backends; gate calibrated at 0.30.

Subcommands:
  onoff     <off.png> <on.png> [hfRatio] [minDiff]
  onoffref  <ref.png> <off.png> <on.png> [hfRatio] [maeRatio] [minDiff]
  selftest

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
    """Mean |dI/dx| + |dI/dy| over a half-resolution lattice (same M12.0
    measurement the thresholds were calibrated against)."""
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


def edge_grad_ratio(off, on, w, h):
    """Hard assertion: mean gradient of ON vs OFF on the high-gradient
    subset of OFF (S12-PLAN M12.1 self-check "edges not blurred through").
    If the joint weights blur across edges this drops sharply.
    Uses a fixed gradient cut (4.0) instead of a percentile sort so the tool
    stays fast on 960x540 images with pure stdlib. Gate calibrated at 0.30
    from the measured 0.493 on both backends."""
    cut, go, gn, cnt = 4.0, 0.0, 0.0, 0
    for y in range(0, h - 1, 2):
        row = y * w
        for x in range(0, w - 1, 2):
            i = row + x
            g = abs(off[i] - off[i + 1]) + abs(off[i] - off[i + w])
            if g > cut:
                go += g
                gn += abs(on[i] - on[i + 1]) + abs(on[i] - on[i + w])
                cnt += 1
    if cnt == 0:
        return 1.0, 0
    return gn / max(go, 1e-6), cnt


# ---------------------------------------------------------------- gates
def gate(off, on, ref=None, hf_ratio=0.90, mae_ratio=1.15, min_diff=1000,
         quiet=False, edge_min=0.30):
    """off/on/ref are (w, h, lum-list) tuples. Return True when all gates pass."""
    ow, oh, o = off
    nw, nh, n = on
    if (ow, oh) != (nw, nh):
        if not quiet:
            print("[s12-spatial] FAIL: size mismatch %dx%d vs %dx%d"
                  % (ow, oh, nw, nh))
        return False
    ok = True
    d = diff_pixels(o, n)
    hf_o, hf_n = highfreq(o, ow, oh), highfreq(n, nw, nh)
    ratio = hf_n / max(hf_o, 1e-6)
    er, ec = edge_grad_ratio(o, n, ow, oh)
    if not quiet:
        print("[s12-spatial] hf off=%.3f on=%.3f ratio=%.3f (limit %.3f)"
              % (hf_o, hf_n, ratio, hf_ratio))
        print("[s12-spatial] diffPixels=%d (min %d)" % (d, min_diff))
        print("[s12-spatial] edgeGradRatio=%.3f (min %.3f, edgePx=%d)"
              % (er, edge_min, ec))
    if d < min_diff:
        if not quiet:
            print("[s12-spatial] FAIL: ON and OFF are structurally identical "
                  "(spatial layer did not run / flag gated off)")
        ok = False
    if ratio > hf_ratio:
        if not quiet:
            print("[s12-spatial] FAIL: spatial layer did not reduce noise")
        ok = False
    if er < edge_min:
        if not quiet:
            print("[s12-spatial] FAIL: high-gradient pixels were blurred flat "
                  "(joint weights not preserving edges)")
        ok = False
    if ref is not None:
        rw, rh, r = ref
        if (rw, rh) != (ow, oh):
            if not quiet:
                print("[s12-spatial] FAIL: ref size mismatch")
            return False
        mo, mn = mae(o, r), mae(n, r)
        mr = mn / max(mo, 1e-6)
        if not quiet:
            print("[s12-spatial] mae off=%.3f on=%.3f ratio=%.3f (limit %.3f)"
                  % (mo, mn, mr, mae_ratio))
        if mr > mae_ratio:
            if not quiet:
                print("[s12-spatial] FAIL: spatial layer drifted from the "
                      "no-jitter reference (edges blurred through)")
            ok = False
    if ok and not quiet:
        print("[s12-spatial] OK: 5x5 joint bilateral reduces noise while "
              "preserving GI structure")
    return ok


def selftest():
    w = h = 64
    # off = accumulated GI with residual checkerboard noise; on = spatially
    # smoothed; ref = flat deterministic reference.
    off = (w, h, [128.0 + (18.0 if (i // w + i % w) % 2 else -18.0)
                  for i in range(w * h)])
    on = (w, h, [128.0 + (9.0 if (i // w + i % w) % 2 else -9.0)
                 for i in range(w * h)])
    ref = (w, h, [128.0] * (w * h))
    positive = gate(off, on, ref, quiet=True)
    negative = gate(off, off, ref, quiet=True)
    blurred = (w, h, [40.0] * (w * h))   # drifted away from ref
    neg_mae = gate(off, blurred, ref, quiet=True)
    print("[s12-spatial] selftest positive=%s negative-onoff=%s negative-mae=%s"
          % (positive, not negative, not neg_mae))
    if positive and not negative and not neg_mae:
        print("[s12-spatial] selftest OK")
        return 0
    print("[s12-spatial] selftest FAIL")
    return 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        return selftest()
    if cmd == "onoff" and len(argv) >= 4:
        hf = float(argv[4]) if len(argv) > 4 else 0.90
        md = int(argv[5]) if len(argv) > 5 else 1000
        off, on = read_png_lum(argv[2]), read_png_lum(argv[3])
        return 0 if gate(off, on, None, hf, 1.15, md) else 1
    if cmd == "onoffref" and len(argv) >= 5:
        hf = float(argv[5]) if len(argv) > 5 else 0.90
        mr = float(argv[6]) if len(argv) > 6 else 1.15
        md = int(argv[7]) if len(argv) > 7 else 1000
        ref = read_png_lum(argv[2])
        off, on = read_png_lum(argv[3]), read_png_lum(argv[4])
        return 0 if gate(off, on, ref, hf, mr, md) else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
