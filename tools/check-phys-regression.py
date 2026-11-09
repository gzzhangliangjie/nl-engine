# -*- coding: utf-8 -*-
"""Physics-track regression gate: archived dumps must stay byte-identical.

Covers M-P0 drop + M-P1 layers (M-P2 origin), the M-P2 stack dump, the M-P3
debug wireframe dump and, since M-P4, the M-P4 character walk-through dump.
Reads the REAL archived baselines and freshly regenerated files, emits
ASCII-only witness lines (Chinese Windows `fc /b` prints GBK and breaks the
evidence PNG). Exit 0 = all identical, 1 = any changed.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D2 = os.path.join(ROOT, "example", "phys-p2")
D3 = os.path.join(ROOT, "example", "phys-p3")
D4 = os.path.join(ROOT, "example", "phys-p4")

PAIRS = (
    ("M-P0 drop.tsv",
     os.path.join(ROOT, "example", "phys-p0", "drop.tsv"),
     os.path.join(D2, "drop-regen.tsv")),
    ("M-P1 layers.tsv",
     os.path.join(ROOT, "example", "phys-p1", "layers.tsv"),
     os.path.join(D2, "layers-regen.tsv")),
    ("M-P2 stack.tsv",
     os.path.join(D2, "stack.tsv"),
     os.path.join(D3, "stack-regen.tsv")),
    ("M-P3 debug.tsv",
     os.path.join(D3, "debug.tsv"),
     os.path.join(D3, "debug-regen.tsv")),
    ("M-P4 character.tsv",
     os.path.join(D4, "character.tsv"),
     os.path.join(D4, "character-regen.tsv")),
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
