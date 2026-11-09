# -*- coding: utf-8 -*-
"""M-S2 anti-false-green: recompute FNV-1a64 over each dump's waveform.tsv
(bytes exactly as written) and compare against provenance.waveformHash.

Same algorithm as audio-tools main.cpp fnv1a64 (standard FNV-1a 64-bit
offset basis + prime). The dump writes the file content == hdr + body,
which is exactly what was hashed, so a tampered sample OR a tampered
stored hash cannot pass. debug/ has no waveform.tsv (snapshot only) and
is skipped.

Usage: python verify-dump-hash.py <dump-dir> [<dir> ...]
Exit 0 = every dir with a waveform.tsv matches. Exit 1 = any mismatch.
"""
import os
import sys

BASIS = 14695981039346656037
PRIME = 1099511628211
MASK = (1 << 64) - 1


def fnv1a64(data):
    h = BASIS
    for b in data:
        h = ((h ^ b) * PRIME) & MASK
    return h


def check_dir(d):
    tsv = os.path.join(d, "waveform.tsv")
    prov = os.path.join(d, "provenance.txt")
    if not os.path.isfile(tsv):
        return None  # snapshot-only dir (debug): nothing to verify
    if not os.path.isfile(prov):
        print("FAIL: missing provenance:", d)
        return False
    stored = None
    with open(prov, encoding="utf-8") as f:
        for ln in f:
            if ln.startswith("waveformHash="):
                stored = ln.split("=", 1)[1].strip()
    if not stored:
        print("FAIL: no waveformHash in provenance:", d)
        return False
    with open(tsv, "rb") as f:
        data = f.read()
    calc = "%016x" % fnv1a64(data)
    if calc != stored:
        print("hash MISMATCH: %s  computed=%s  stored=%s" % (d, calc, stored))
        return False
    print("hash ok: %s  waveformHash=%s" % (d, calc))
    return True


def main():
    dirs = sys.argv[1:]
    if not dirs:
        print("usage: python verify-dump-hash.py <dump-dir> [...]")
        return 2
    bad = 0
    verified = 0
    for d in dirs:
        r = check_dir(d)
        if r is None:
            print("skip (no waveform.tsv):", d)
        elif r:
            verified += 1
        else:
            bad += 1
    print("verify-dump-hash: %s (%d verified, %d mismatch)" %
          ("PASS" if bad == 0 else "FAIL", verified, bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
