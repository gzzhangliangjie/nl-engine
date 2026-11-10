#!/usr/bin/env python3
"""M-GL1 acceptance cross-checks: pause freeze + timescale trajectory, MD5 level.

Run from example/gl1/run.bat after the 8 PNGs are produced into
build/showcase/runs/. Exits 0 on all-green, 1 on any mismatch.

Checks (per backend):
  1. pause120 == pause60      -> paused run renders byte-identical frames at
     frame 60 and 120: simTime frozen, HUD alive (full HUD panel present in
     both) and deterministic fixed samples.
  2. pause120 == anim0        -> console `game.pause` bind pose equals the
     legacy --anim-time 0 pin: two different code paths, one pose.
  3. scale2 == scale2ref      -> `game.timeScale 2` with --fixed-dt 16.7
     accumulates the same per-frame 33.4ms as --fixed-dt 33.4 at scale 1:
     trajectory equality at the pixel level.
  4. every run == committed baseline in example/gl1 -> reproducibility.
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.normpath(os.path.join(HERE, "..", "..", "build", "showcase", "runs"))
BACKENDS = ("DX12", "VULKAN")

fails = 0


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def main():
    global fails
    # fails is module-level (mutated from nested expect helpers via `global`).
    cases = ("pause120", "pause60", "anim0", "scale2", "scale2ref")
    digests = {}
    for backend in BACKENDS:
        for case in cases:
            p = os.path.join(RUNS, "gl1-%s-%s.png" % (case, backend))
            if not os.path.isfile(p):
                print("FAIL missing %s" % p)
                fails += 1
                continue
            digests[(case, backend)] = md5(p)

    def expect(a, b, backend, why):
        global fails
        da, db = digests.get((a, backend)), digests.get((b, backend))
        if da is None or db is None:
            return  # missing already counted
        if da == db:
            print("PASS [%s] %s == %s  (%s)" % (backend, a, b, why))
        else:
            print("FAIL [%s] %s != %s  (%s)" % (backend, a, b, why))
            fails += 1

    def expect_baseline(case, backend):
        global fails
        d = digests.get((case, backend))
        base = os.path.join(HERE, "gl1-%s-%s.png" % (case, backend))
        if d is None:
            return
        if not os.path.isfile(base):
            print("FAIL missing baseline %s" % base)
            fails += 1
            return
        if d == md5(base):
            print("PASS [%s] %s == committed baseline" % (backend, case))
        else:
            print("FAIL [%s] %s != committed baseline" % (backend, case))
            fails += 1

    for backend in BACKENDS:
        expect("pause120", "pause60", backend, "frozen across render frames")
        expect("pause120", "anim0", backend, "paused pose == anim-time 0 pin")
        expect("scale2", "scale2ref", backend, "scaled feed == direct 33.4ms feed")
        expect_baseline("pause120", backend)
        expect_baseline("scale2", backend)

    print("gl1 check: %s (%d mismatch)" % ("PASS" if fails == 0 else "FAIL", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
