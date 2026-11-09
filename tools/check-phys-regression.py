# -*- coding: utf-8 -*-
"""M-P2 regression gate: M-P0 drop + M-P1 layer dumps must stay byte-identical.

Reads the REAL archived baselines and freshly regenerated files, emits ASCII-only
witness lines (Chinese Windows `fc /b` prints GBK and breaks the evidence PNG).
Exit 0 = all identical, 1 = any changed.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D2 = os.path.join(ROOT, "example", "phys-p2")

PAIRS = (
    ("M-P0 drop.tsv",
     os.path.join(ROOT, "example", "phys-p0", "drop.tsv"),
     os.path.join(D2, "drop-regen.tsv")),
    ("M-P1 layers.tsv",
     os.path.join(ROOT, "example", "phys-p1", "layers.tsv"),
     os.path.join(D2, "layers-regen.tsv")),
)

ok = True
for label, baseline, regen in PAIRS:
    for path in (baseline, regen):
        if not os.path.isfile(path):
            print("MISSING FILE: %s" % path)
            sys.exit(1)
    with open(baseline, "rb") as f:
        a = f.read()
    with open(regen, "rb") as f:
        b = f.read()
    same = a == b
    ok = ok and same
    print("%s byte-identical: %s" % (label, "OK" if same else "FAIL"))
    print("  md5 archived = %s" % hashlib.md5(a).hexdigest())
    print("  md5 regen    = %s" % hashlib.md5(b).hexdigest())

sys.exit(0 if ok else 1)
