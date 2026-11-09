# -*- coding: utf-8 -*-
"""Build M-S2 negative fixtures FROM THE REAL volume dump.

bad-volume/: waveform.tsv tampered (first non-zero sample scaled by 1.5).
             provenance still carries the pristine hash -> verify fails.
bad-hash/  : waveform.tsv untouched, but the stored waveformHash itself is
             edited. File hash no longer matches -> verify fails too.

Both must make verify-dump-hash.py exit 1: the checker never trusts the
stored numbers (anti-false-green, mirrors M-S1's two tamper angles).
"""
import pathlib
import shutil
import sys

root = pathlib.Path(__file__).resolve().parent
src = root / "volume"
for f in ("waveform.tsv", "provenance.txt"):
    if not (src / f).is_file():
        print("missing real dump:", src / f)
        sys.exit(1)

for name in ("bad-volume", "bad-hash"):
    d = root / name
    d.mkdir(exist_ok=True)
    for f in ("waveform.tsv", "provenance.txt"):
        shutil.copyfile(src / f, d / f)

# bad-volume: scale the first non-zero left-channel sample by 1.5
p = root / "bad-volume" / "waveform.tsv"
lines = p.read_text(encoding="utf-8").splitlines()
patched = False
for i, ln in enumerate(lines):
    parts = ln.split()
    if len(parts) == 3 and parts[0][0].isdigit() and float(parts[1]) != 0.0:
        lines[i] = "%s %s %s" % (parts[0], "%.9g" % (float(parts[1]) * 1.5), parts[2])
        patched = True
        break
if not patched:
    print("could not find a non-zero sample to tamper")
    sys.exit(1)
p.write_text("\n".join(lines) + "\n", encoding="utf-8")

# bad-hash: edit the stored hash itself
p = root / "bad-hash" / "provenance.txt"
t = p.read_text(encoding="utf-8")
old = None
for ln in t.splitlines():
    if ln.startswith("waveformHash="):
        old = ln
        break
if not old:
    print("waveformHash field not found in provenance")
    sys.exit(1)
h = old.split("=", 1)[1]
new_h = ("00000000" + h[8:]) if h[0] != "0" else ("1" + h[1:])
p.write_text(t.replace(old, "waveformHash=" + new_h, 1), encoding="utf-8")
print("negative fixtures built from real dump:", src)
