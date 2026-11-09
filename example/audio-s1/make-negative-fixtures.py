# -*- coding: utf-8 -*-
"""Build M-S1 negative fixtures FROM THE REAL spatial dump.

bad/       : attenuation.tsv tampered (a silence row -> non-silence).
             provenance still carries the pristine hash -> hash check fails.
bad-prov/  : only the maxDistance field in provenance is changed. Both tsv
             hashes stay valid, so `check` must fail at the RE-RENDER stage.

Both must make `audio-tools check` exit 1, for two different reasons: this is
the anti-false-green proof (the checker never trusts the stored numbers).
"""
import pathlib
import shutil
import sys

root = pathlib.Path(__file__).resolve().parent
src = root / "spatial"
files = ("attenuation.tsv", "mixing.tsv", "provenance.txt")
for f in files:
    if not (src / f).is_file():
        print("missing real dump:", src / f)
        sys.exit(1)

for name in ("bad", "bad-prov"):
    d = root / name
    d.mkdir(exist_ok=True)
    for f in files:
        shutil.copyfile(src / f, d / f)

prov = (src / "provenance.txt").read_text(encoding="utf-8")
maxd = 20.0
for ln in prov.splitlines():
    if ln.startswith("maxDistance="):
        maxd = float(ln.split("=", 1)[1])
        break

p = root / "bad" / "attenuation.tsv"
lines = p.read_text(encoding="utf-8").splitlines()
patched = False
for i, ln in enumerate(lines):
    parts = ln.split()
    if (len(parts) == 5 and parts[0][0].isdigit() and
            float(parts[0]) > maxd and
            all(float(x) == 0.0 for x in parts[2:])):
        lines[i] = parts[0] + " " + parts[1] + " 0.1 0.1 0.1"
        patched = True
        break
if not patched:
    print("could not find a silence row to tamper")
    sys.exit(1)
p.write_text("\n".join(lines) + "\n", encoding="utf-8")

p = root / "bad-prov" / "provenance.txt"
t = p.read_text(encoding="utf-8")
old = "maxDistance=%g" % maxd
new = "maxDistance=%g" % (maxd / 2.0)
if old not in t:
    print("maxDistance field not found in provenance")
    sys.exit(1)
p.write_text(t.replace(old, new, 1), encoding="utf-8")
print("negative fixtures built from real dump:", src)
