#!/usr/bin/env python
"""P8 M-GL2 acceptance twin: independent re-check of the four gl2 logs.

Checks (per backend, then cross-backend byte-exact):
  move  - two readback lines with equal=1; source position sits ON the orbit
          circle of its parent (radius invariant 1.6 / 1.4); position differs
          from the bind-time value (moved with the spinning parent);
          clock state simTime=1.66667 ticks=100 paused=0 scale=1.
  pause - clock frozen (simTime=0 ticks=0 paused=1); positions equal the
          bind-time world positions (E1a offset composition, unrotated).
Cross-backend: readback positions must match exactly (CPU-side logic, no GPU).

Usage: verify.py <move-DX12.log> <move-VULKAN.log> <pause-DX12.log> <pause-VULKAN.log>
Exit 0 = all green; 1 = any mismatch; 2 = bad usage / missing file.
"""
import math
import re
import sys

CENTER_A = (-2.2, 0.0, 0.0)   # spin-a parent world position (scene json)
CENTER_B = (0.0, 0.0, 0.6)    # spin-b parent world position
RADIUS_A = 1.6                # orb-a local offset from spin-a
RADIUS_B = 1.4                # orb-b local offset from spin-b

READBACK = re.compile(
    r"audio readback: src=(\d+) entity=(\S+) "
    r"entityPos=\((-?\d+\.\d+), (-?\d+\.\d+), (-?\d+\.\d+)\) "
    r"srcPos=\((-?\d+\.\d+), (-?\d+\.\d+), (-?\d+\.\d+)\) equal=(\d)")
BIND = re.compile(
    r"audio\.play: src (\d+) <-> (\S+) "
    r"@\((-?\d+\.\d+), (-?\d+\.\d+), (-?\d+\.\d+)\)")
CLOCK = re.compile(
    r"audio clock state: simTime=(\S+) ticks=(\d+) paused=(\d) scale=(\S+)")


def dist3(p, q):
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(p, q)))


def parse(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    binds, reads, clocks = {}, {}, CLOCK.findall(text)
    for m in BIND.finditer(text):
        binds[m.group(2)] = (m.group(1), tuple(float(m.group(k)) for k in (3, 4, 5)))
    for m in READBACK.finditer(text):
        reads[m.group(2)] = {
            "src": m.group(1),
            "entity": tuple(float(m.group(k)) for k in (3, 4, 5)),
            "source": tuple(float(m.group(k)) for k in (6, 7, 8)),
            "equal": m.group(9) == "1",
        }
    return binds, reads, clocks


def check_move(tag, path, fails):
    binds, reads, clocks = parse(path)
    if len(binds) != 2 or set(binds) != {"orb-a", "orb-b"}:
        fails.append(f"{tag}: expected binds for orb-a+orb-b, got {sorted(binds)}")
        return
    if set(reads) != {"orb-a", "orb-b"}:
        fails.append(f"{tag}: expected readbacks for orb-a+orb-b, got {sorted(reads)}")
        return
    for name, center, radius in (("orb-a", CENTER_A, RADIUS_A),
                                 ("orb-b", CENTER_B, RADIUS_B)):
        r = reads[name]
        if not r["equal"]:
            fails.append(f"{tag}: {name} readback equal=0")
        if r["src"] != binds[name][0]:
            fails.append(f"{tag}: {name} src id mismatch bind vs readback")
        d = dist3(r["entity"], center)
        if abs(d - radius) > 1e-3:
            fails.append(f"{tag}: {name} off orbit circle (dist={d:.6f} want {radius})")
        if dist3(r["entity"], r["source"]) > 1e-6:
            fails.append(f"{tag}: {name} srcPos != entityPos")
        if dist3(r["entity"], binds[name][1]) < 1e-3:
            fails.append(f"{tag}: {name} did not move since bind (position frozen?)")
    if len(clocks) != 1 or clocks[0] != ("1.66667", "100", "0", "1"):
        fails.append(f"{tag}: clock state wrong: {clocks}")


def check_pause(tag, path, fails):
    binds, reads, clocks = parse(path)
    if set(binds) != {"orb-a", "orb-b"} or set(reads) != {"orb-a", "orb-b"}:
        fails.append(f"{tag}: expected orb-a+orb-b, got binds={sorted(binds)}")
        return
    for name in ("orb-a", "orb-b"):
        r = reads[name]
        if not r["equal"]:
            fails.append(f"{tag}: {name} readback equal=0")
        if dist3(r["entity"], binds[name][1]) > 1e-6:
            fails.append(f"{tag}: {name} moved while paused "
                         f"({r['entity']} vs bind {binds[name][1]})")
        if dist3(r["entity"], r["source"]) > 1e-6:
            fails.append(f"{tag}: {name} srcPos != entityPos while paused")
    if len(clocks) != 1 or clocks[0][1:4] != ("0", "1", "1"):
        fails.append(f"{tag}: clock not frozen: {clocks}")
    if clocks and float(clocks[0][0]) != 0.0:
        fails.append(f"{tag}: simTime advanced while paused: {clocks[0][0]}")


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    paths = sys.argv[1:5]
    for p in paths:
        import os
        if not os.path.exists(p):
            print(f"missing log: {p}")
            return 2
    fails = []
    check_move("move-DX12", paths[0], fails)
    check_move("move-VULKAN", paths[1], fails)
    check_pause("pause-DX12", paths[2], fails)
    check_pause("pause-VULKAN", paths[3], fails)
    # Cross-backend: readback positions must match exactly (pure CPU logic).
    for kind, i, j in (("move", 0, 1), ("pause", 2, 3)):
        _, ra, _ = parse(paths[i])
        _, rb, _ = parse(paths[j])
        for name in ("orb-a", "orb-b"):
            if name in ra and name in rb and \
               (ra[name]["entity"] != rb[name]["entity"] or
                ra[name]["source"] != rb[name]["source"]):
                fails.append(f"{kind} cross-backend: {name} positions differ "
                             f"{ra[name]['entity']} vs {rb[name]['entity']}")
    if fails:
        for f in fails:
            print("FAIL", f)
        print(f"gl2 verify: FAIL ({len(fails)} mismatch)")
        return 1
    print("gl2 verify: PASS (orbit radius invariants, movement, freeze, "
          "cross-backend byte-exact - 12 checks x 2 backends)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
