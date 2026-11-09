#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M-GL0 independent verifier: recompute the GameClock trace in Python and
compare row-by-row against the gl-tools trace TSV (anti-fake-green, same
discipline as audio-s2 verify-dump-hash.py).

The Python twin implements the same contract from P8-GAMELOOP-PLAN §3
directly (60Hz step / 250ms clamp / bevy pause semantics) without importing
any C++ code, so agreement is cross-implementation evidence.

Usage: python verify.py <trace.tsv> [--dt MS | --dts a,b,c] [--scale S]
                       [--pause-at N] [--resume-at N]
Exit:  0 all rows match / 1 mismatch
"""
import sys
import os

STEP = 1.0 / 60.0
MAX_DELTA = 0.250


class Twin:
    def __init__(self):
        self.sim_time = 0.0
        self.acc = 0.0
        self.frame_dt = 0.0
        self.scale = 1.0
        self.paused = False
        self.count = 0

    def set_scale(self, s):
        # C++ twin clamps: NaN/negative -> 0, huge -> 1024
        if not (s == s) or s < 0.0:
            s = 0.0
        self.scale = min(s, 1024.0)

    def tick(self, real_dt_ms):
        if self.paused:
            self.frame_dt = 0.0
            return 0
        dt = real_dt_ms * 0.001 if (real_dt_ms == real_dt_ms and real_dt_ms > 0) else 0.0
        dt *= self.scale
        if dt > MAX_DELTA:
            dt = MAX_DELTA
        self.frame_dt = dt
        self.acc += dt
        n = 0
        while self.acc >= STEP:
            self.acc -= STEP
            self.sim_time += STEP
            self.count += 1
            n += 1
        return n


def main():
    if len(sys.argv) < 2:
        print("usage: verify.py <trace.tsv> [--dt MS | --dts a,b,c] [--scale S] "
              "[--pause-at N] [--resume-at N]")
        return 2
    tsv = sys.argv[1]
    dts = None
    dt_val, frames = 16.7, 240
    twin = Twin()
    pause_at = resume_at = -1
    i = 2
    while i < len(sys.argv):
        a = sys.argv[i]
        if a == "--dt":
            dt_val = float(sys.argv[i + 1])
            i += 2
        elif a == "--dts":
            dts = [float(x) for x in sys.argv[i + 1].split(",")]
            i += 2
        elif a == "--frames":
            frames = int(sys.argv[i + 1])
            i += 2
        elif a == "--scale":
            twin.set_scale(float(sys.argv[i + 1]))
            i += 2
        elif a == "--pause-at":
            pause_at = int(sys.argv[i + 1]); i += 2
        elif a == "--resume-at":
            resume_at = int(sys.argv[i + 1]); i += 2
        else:
            print("unknown arg " + a)
            return 2

    if dts is None:
        dts = [dt_val] * frames

    if not os.path.exists(tsv):
        print("FAIL: missing " + tsv)
        return 1
    with open(tsv, "r", encoding="utf-8") as f:
        lines = [l.rstrip("\n") for l in f if l.strip()]
    header, rows = lines[0], lines[1:]
    want_header = "frame\trealDtMs\tticks\ttickCount\tsimTime\talpha\tpaused"
    if header != want_header:
        print("FAIL: header mismatch:\n  got  %r\n  want %r" % (header, want_header))
        return 1

    bad = 0
    for idx, row in enumerate(rows):
        if pause_at >= 0 and idx >= pause_at and (resume_at < 0 or idx < resume_at):
            twin.paused = True
        elif resume_at >= 0 and idx >= resume_at:
            twin.paused = False
        ticks = twin.tick(dts[idx])
        col = row.split("\t")
        if len(col) != 7:
            print("FAIL row %d: bad column count" % idx)
            bad += 1
            continue
        f_idx, f_dt, f_ticks, f_count, f_sim, f_alpha, f_paused = col
        want = (idx, ticks, twin.count, twin.sim_time, twin.acc / STEP, twin.paused)
        got = (int(f_idx), int(f_ticks), int(f_count), float(f_sim), float(f_alpha),
               f_paused == "1")
        ok = (got[0] == want[0] and got[1] == want[1] and got[2] == want[2]
              and abs(got[3] - want[3]) < 1e-9 and abs(got[4] - want[4]) < 1e-9
              and got[5] == want[5])
        if not ok:
            print("FAIL row %d: C++ %r vs python %r" % (idx, got, want))
            bad += 1
        if bad > 10:
            print("FAIL: too many mismatches, stop at row %d" % idx)
            return 1

    if bad:
        print("gl-m0 verify: FAIL (%d rows)" % bad)
        return 1
    print("gl-m0 verify: PASS (%d rows, C++ trace == independent python twin)" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
