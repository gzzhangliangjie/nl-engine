# -*- coding: utf-8 -*-
"""M-P1 regression gate: the M-P0 drop trajectory must stay byte-identical.

Reads the two REAL files (no synthetic data) and emits ASCII-only witness lines
so the evidence PNG never picks up GBK/UTF-16 console junk on a Chinese Windows.
Exit 0 = identical, 1 = changed.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "example", "phys-p0", "drop.tsv")
REGEN = os.path.join(ROOT, "example", "phys-p1", "drop-regen.tsv")

for path in (BASELINE, REGEN):
    if not os.path.isfile(path):
        print("MISSING FILE: %s" % path)
        sys.exit(1)

with open(BASELINE, "rb") as f:
    a = f.read()
with open(REGEN, "rb") as f:
    b = f.read()

print("M-P0 baseline byte-identical: %s" % ("OK" if a == b else "FAIL"))
print("md5 phys-p0/drop.tsv    = %s" % hashlib.md5(a).hexdigest())
print("md5 phys-p1/drop-regen  = %s" % hashlib.md5(b).hexdigest())
sys.exit(0 if a == b else 1)
