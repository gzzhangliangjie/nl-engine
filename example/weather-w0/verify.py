#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""WEA M-WEA0 independent python twin: recompute the weather state layer
from (seed, duration, step) WITHOUT touching any C++ code, then compare
against the real nanite-tools `weather` TSV row by row.

Anti-fake-green contract (gl0 precedent): the twin is a from-scratch
reimplementation of the documented model (splitmix64 state machine +
smoothstep blend + fixed-step wetness ODE + analytic sky/sun). Formatted
fields are compared numerically with a tolerance that only absorbs
printf rounding + libm ulp noise (math.sin), NOT model differences.
Exit 0 = all rows match.
"""
import argparse
import math
import sys
import csv

# ---- constants (documented in WEATHER-TOOLS-GUIDE.md / weather.hpp) ----
DAY_LENGTH = 600.0
EPOCH_LENGTH = 120.0
BLEND_WINDOW = 20.0
PERSIST_PROB = 0.55
WET_STEP = 1.0
WET_RATE = 0.08
DRY_RATE = 0.02

PROFILES = {
    0: (0.0, 0.05, 2.0, 0.0005, 30.0),  # clear
    1: (0.0, 0.45, 3.0, 0.001, 40.0),   # partly
    2: (0.0, 0.85, 4.5, 0.002, 50.0),   # overcast
    3: (1.0, 1.0, 6.0, 0.004, 60.0),    # rain
}
KIND_NAMES = {0: "clear", 1: "partly", 2: "overcast", 3: "rain"}

MASK64 = (1 << 64) - 1


def splitmix64(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK64
    return x ^ (x >> 31)


def uniform01(h):
    return (h >> 11) * (1.0 / 9007199254740992.0)


def hash_at(seed, slot):
    return splitmix64(seed ^ splitmix64(slot + 0x2545F4914F6CDD1D))


def clamp01(v):
    return max(0.0, min(1.0, v))


def smoothstep(x):
    t = clamp01(x)
    return t * t * (3.0 - 2.0 * t)


def kind_at_epoch(kinds, seed, e):
    if e <= 0:
        return hash_at(seed, 0) % 4
    prev = kinds[e - 1]
    if uniform01(hash_at(seed, e)) < PERSIST_PROB:
        return prev
    pick = hash_at(seed, e + 0x9E37) % 3
    v = pick
    if v >= prev:
        v += 1
    return v


def blended_profile(kinds, epoch_count, t):
    e = int(t / EPOCH_LENGTH)
    if e >= epoch_count:
        e = epoch_count - 1
    if e < 0:
        e = 0
    u = (t - e * EPOCH_LENGTH) / EPOCH_LENGTH
    if u <= 0.0 and e > 0:
        e -= 1
        u = 1.0
    hw = (BLEND_WINDOW * 0.5) / EPOCH_LENGTH
    rain, cov, turb, fog, fh = PROFILES[kinds[e]]
    if u > 1.0 - hw:
        nr, nc, nt, nf, nh = PROFILES[kinds[e + 1]]
        s = smoothstep((u - (1.0 - hw)) / (2.0 * hw))
        rain += (nr - rain) * s
        cov += (nc - cov) * s
        turb += (nt - turb) * s
        fog += (nf - fog) * s
        fh += (nh - fh) * s
    elif u < hw and e > 0:
        pr, pc, pt, pf, ph = PROFILES[kinds[e - 1]]
        s = smoothstep((u + hw) / (2.0 * hw))
        rain = pr + (rain - pr) * s
        cov = pc + (cov - pc) * s
        turb = pt + (turb - pt) * s
        fog = pf + (fog - pf) * s
        fh = ph + (fh - ph) * s
    return rain, cov, turb, fog, fh


class Timeline:
    def __init__(self, seed, duration, preset=None):
        self.seed = seed
        self.duration = duration if duration > 0 else 1.0
        self.epoch_count = max(1, math.ceil(self.duration / EPOCH_LENGTH))
        self.kinds = [0] * (self.epoch_count + 1)
        for e in range(self.epoch_count + 1):
            self.kinds[e] = preset if preset is not None else kind_at_epoch(self.kinds, seed, e)
        wet_count = int(self.duration / WET_STEP) + 1
        self.wetness = [0.0] * wet_count
        wet = 0.0
        for i in range(wet_count):
            t = i * WET_STEP
            rain, _, _, _, _ = blended_profile(self.kinds, self.epoch_count, t)
            wet += (rain * WET_RATE - (1.0 - rain) * DRY_RATE * wet) * WET_STEP
            wet = clamp01(wet)
            self.wetness[i] = wet

    def sample(self, t):
        if not math.isfinite(t):
            t = 0.0
        t = max(0.0, min(self.duration, t))
        day_phase = math.fmod(t, DAY_LENGTH) / DAY_LENGTH
        TWO_PI = 2.0 * math.pi
        elev = math.sin(TWO_PI * day_phase - 0.5 * math.pi) * (0.5 * math.pi)
        sin_elev = math.sin(elev)
        daylight = clamp01(sin_elev * 8.0)

        rain, cov, turb, fog, fh = blended_profile(self.kinds, self.epoch_count, t)
        trans = clamp01(1.0 - clamp01(cov) * 0.85)
        sun_i = daylight * trans
        temp = 2200.0 + (6500.0 - 2200.0) * clamp01(sin_elev * 2.5)
        disk = clamp01(daylight * trans * trans)

        night = 0.02 + 0.98 * daylight
        warm = clamp01(1.0 - abs(sin_elev) * 4.0)
        gray = clamp01((turb - 2.0) / 4.0) * 0.5
        cloud_mix = clamp01(cov) * 0.7

        def apply(z, h, gain, loss):
            h *= 1.0 + warm * gain
            z *= 1.0 + warm * gain * 0.5
            h *= 1.0 - warm * loss
            z *= 1.0 - warm * loss * 0.5
            z *= night
            h *= night
            avg = (z + h) * 0.5
            z = z + (avg * 0.9 - z) * gray
            h = h + (avg * 0.9 - h) * gray
            z = z + (0.35 - z) * cloud_mix
            h = h + (0.35 - h) * cloud_mix
            return clamp01(z), clamp01(h)

        zr, hr = apply(0.16, 0.62, 0.40, 0.30)
        zg, hg = apply(0.30, 0.71, 0.10, 0.10)
        zb, hb = apply(0.58, 0.84, 0.00, 0.00)

        wpos = t / WET_STEP
        wi = int(wpos)
        wi = max(0, min(wi, len(self.wetness) - 1))
        frac = wpos - wi
        w0 = self.wetness[wi]
        w1 = self.wetness[wi + 1] if wi + 1 < len(self.wetness) else w0
        wet = clamp01(w0 + (w1 - w0) * frac)

        kind_idx, blend = self._dominant_kind(t)
        return {
            "t": t, "kind": KIND_NAMES[kind_idx], "blend": blend,
            "sunAzimuth": TWO_PI * day_phase, "sunElevation": elev,
            "sunIntensity": sun_i, "sunColorTempK": temp,
            "zenithR": zr, "zenithG": zg, "zenithB": zb,
            "horizonR": hr, "horizonG": hg, "horizonB": hb,
            "turbidity": turb, "sunDisk": disk,
            "cloudCoverage": clamp01(cov), "cloudTransmittance": trans,
            "fogDensity": max(0.0, fog), "fogHeight": max(0.0, fh),
            "groundWetness": wet, "rainIntensity": rain,
        }

    def _dominant_kind(self, t):
        # mirror sample()'s kind/blend reporting (dominant weight side)
        e = int(t / EPOCH_LENGTH)
        if e >= self.epoch_count:
            e = self.epoch_count - 1
        if e < 0:
            e = 0
        u = (t - e * EPOCH_LENGTH) / EPOCH_LENGTH
        if u <= 0.0 and e > 0:
            e -= 1
            u = 1.0
        hw = (BLEND_WINDOW * 0.5) / EPOCH_LENGTH
        cur = self.kinds[e]
        if u > 1.0 - hw:
            nxt = self.kinds[e + 1]
            s = smoothstep((u - (1.0 - hw)) / (2.0 * hw))
            if s > 0.5:
                return nxt, s
            return cur, 1.0 - s
        if u < hw and e > 0:
            prev = self.kinds[e - 1]
            s = smoothstep((u + hw) / (2.0 * hw))
            if s > 0.5:
                return cur, s
            return prev, 1.0 - s
        return cur, 1.0


COLUMNS = ["t", "sunAzimuth", "sunElevation", "sunIntensity", "sunColorTempK",
           "zenithR", "zenithG", "zenithB", "horizonR", "horizonG", "horizonB",
           "turbidity", "sunDisk", "cloudCoverage", "cloudTransmittance",
           "fogDensity", "fogHeight", "groundWetness", "rainIntensity"]
# printf formats used by nanite-tools weather dump
FMT = {"t": 3, "sunColorTempK": 3, "fogHeight": 2, "turbidity": 4}
TOL = 1.1e-6  # printf rounding + libm ulp noise


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--preset", default=None,
                    choices=["clear", "partly", "overcast", "rain"])
    ap.add_argument("--tol", type=float, default=TOL)
    args = ap.parse_args()

    preset = {"clear": 0, "partly": 1, "overcast": 2, "rain": 3}.get(args.preset)

    rows = list(csv.DictReader(open(args.tsv, encoding="utf-8"), delimiter="\t"))
    if not rows:
        print("verify: FAIL (empty TSV)")
        return 1
    t0 = float(rows[0]["t"])
    t_last = float(rows[-1]["t"])
    step = float(rows[1]["t"]) - t0 if len(rows) > 1 else 1.0
    duration = t_last

    tl = Timeline(args.seed, duration, preset)
    bad = 0
    max_dev = 0.0
    for r in rows:
        t = float(r["t"])
        s = tl.sample(t)
        if r["kind"] != s["kind"]:
            bad += 1
            print("kind mismatch at t=%.3f: tsv=%s twin=%s" % (t, r["kind"], s["kind"]))
            continue
        for c in COLUMNS[1:]:
            v_tsv = float(r[c])
            v_twin = s[c]
            dev = abs(v_tsv - v_twin)
            max_dev = max(max_dev, dev)
            if dev > args.tol * max(1.0, abs(v_twin)):
                bad += 1
                print("field mismatch at t=%.3f %s: tsv=%.9f twin=%.9f" % (t, c, v_tsv, v_twin))
        # blend: dominant weight, 6dp printf field
        dev = abs(float(r["blend"]) - s["blend"])
        max_dev = max(max_dev, dev)
        if dev > args.tol:
            bad += 1
            print("blend mismatch at t=%.3f: tsv=%.9f twin=%.9f" % (t, float(r["blend"]), s["blend"]))
    if bad == 0:
        print("twin verify: PASS (%d rows, max field deviation %.2e)" % (len(rows), max_dev))
        return 0
    print("twin verify: FAIL (%d mismatches)" % bad)
    return 1


if __name__ == "__main__":
    sys.exit(main())
