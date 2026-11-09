#!/usr/bin/env python
"""P8 M-GL3 acceptance twin: independent re-check of the gl3 interp logs.

Suites (see run.bat):
  equal  - interp OFF vs interp ON must produce byte-identical PNGs
           (screenshot render pins alpha=0: capture = tick-boundary state).
           The interp-ON log also carries "interp probe" lines whose alpha
           is the real clock remainder; interp pos must equal lerp(prev,curr,alpha).
  mid    - fixed-dt 25ms => per-frame tick counts [1,2,1] (python re-derivation
           of the GameClock accumulator), capture-frame alpha = 0.5.
           Orbit entities (orb-a / orb-b, E1a children of spin-a / spin-b) move:
           independent twin recomputes prev/curr world positions from the
           SpinSystem formula (w_i = 0.9 * (1 + 0.5*idx) rad/s, idx = entity
           registration order among Transform carriers) and checks interp pos
           == lerp(prev,curr,alpha) AND interp quat == slerp(prevQ,currQ,alpha).
  pause  - game.pause: clock frozen, prev == curr == interp, alpha = 0.

Cross-backend: "interp probe" lines must be byte-equal between DX12/VULKAN.

Usage:
  verify.py <eq-off-dx12.log> <eq-on-dx12.log> <eq-off-vk.log> <eq-on-vk.log>
            <eq-off-dx12.png> <eq-on-dx12.png> <eq-off-vk.png> <eq-on-vk.png>
            <mid-dx12.log> <mid-vk.log> <pause-dx12.log> <pause-vk.log>
Exit 0 = all green; 1 = any mismatch; 2 = bad usage / missing file.
"""
import hashlib
import math
import re
import sys

STEP = 1.0 / 60.0
SPIN_BASE = 0.9
CENTER_A = (-2.2, 0.0, 0.0)   # spin-a world position (scene json)
CENTER_B = (0.0, 0.0, 0.6)    # spin-b world position
OFFSET_A = (1.6, 0.0, 0.0)    # orb-a local offset (parent spin-a, idx 0)
OFFSET_B = (0.0, 0.0, 1.4)    # orb-b local offset (parent spin-b, idx 1)

PROBE = re.compile(
    r"interp probe: k=(\d+) alpha=(\S+)"
    r" prev=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)"
    r" curr=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)"
    r" interp=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)"
    r" pq=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)"
    r" cq=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)"
    r" iq=\((-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)\)")
GAME_STATUS = re.compile(
    r"game: simTime=(\S+)s ticks=(\d+) paused=(\d) scale=(\S+) alpha=(\S+)")


def lerp(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def slerp(qa, qb, t):
    """Quat slerp, shortest path, nlerp fallback near-parallel (math.hpp 口径)."""
    d = sum(x * y for x, y in zip(qa, qb))
    if d < 0.0:
        qb = tuple(-x for x in qb)
        d = -d
    if d > 1.0 - 1e-4:  # near-parallel: nlerp (ozz Lerp threshold style)
        r = lerp(qa, qb, t)
        n = math.sqrt(sum(x * x for x in r)) or 1.0
        return tuple(x / n for x in r)
    theta0 = math.acos(max(-1.0, min(1.0, d)))
    s = math.sin(theta0)
    w0 = math.sin((1.0 - t) * theta0) / s
    w1 = math.sin(t * theta0) / s
    return tuple(x * w0 + y * w1 for x, y in zip(qa, qb))


def ry(theta):
    c, s = math.cos(theta), math.sin(theta)
    return [[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]


def mat_vec(m, v):
    return tuple(sum(m[i][j] * v[j] for j in range(3)) for i in range(3))


def clock_alpha_frames(fixed_dt_ms, frames):
    """Re-derive per-frame (ticks, alpha) from GameClock semantics (python twin)."""
    acc = 0.0
    out = []
    for _ in range(frames):
        acc = min(acc + fixed_dt_ms / 1000.0, 0.250)  # kMaxDelta
        ticks = 0
        while acc >= STEP:
            acc -= STEP
            ticks += 1
        out.append((ticks, acc / STEP))
    return out


def orb_world_pos(parent_idx, center, offset, n_ticks):
    """SpinSystem twin: rotation accumulates w_i*dt per tick about +Y."""
    theta = n_ticks * STEP * SPIN_BASE * (1.0 + 0.5 * parent_idx)
    return tuple(c + r for c, r in zip(center, mat_vec(ry(theta), offset)))


def parse_probes(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    probes = []
    for m in PROBE.finditer(text):
        g = [float(x) for x in m.groups()]
        probes.append({
            "k": int(m.group(1)),
            "alpha": g[1],
            "prev": tuple(g[2:5]),
            "curr": tuple(g[5:8]),
            "interp": tuple(g[8:11]),
            "pq": tuple(g[11:15]),
            "cq": tuple(g[15:19]),
            "iq": tuple(g[19:23]),
        })
    return probes, text


def probe_line_bytes(text):
    return [ln for ln in text.splitlines() if "interp probe:" in ln]


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def check_interp_row(tag, p, fails, tol=1e-4):
    if not close(p["interp"], lerp(p["prev"], p["curr"], p["alpha"]), tol):
        fails.append(f"{tag} k={p['k']}: interp pos != lerp(prev,curr,alpha)")
    if not close(p["iq"], slerp(p["pq"], p["cq"], p["alpha"]), tol):
        fails.append(f"{tag} k={p['k']}: interp quat != slerp(pq,cq,alpha)")


def main(argv):
    if len(argv) != 13:
        print(__doc__)
        return 2
    paths = argv[1:]
    for p in paths:
        try:
            open(p, "rb").close()
        except OSError as e:
            print(f"[FAIL] missing file: {p} ({e})")
            return 2
    fails = []

    # ---- equal suite: PNG byte identity off vs on, per backend ----
    for backend, off_png, on_png in (("dx12", paths[4], paths[5]),
                                     ("vulkan", paths[6], paths[7])):
        h_off = hashlib.md5(open(off_png, "rb").read()).hexdigest()
        h_on = hashlib.md5(open(on_png, "rb").read()).hexdigest()
        if h_off != h_on:
            fails.append(f"equal {backend}: interp-off/on PNG bytes differ "
                         f"({h_off} vs {h_on})")
        else:
            print(f"[ok] equal {backend}: interp off==on PNG md5 {h_off[:12]}")

    # ---- equal suite: interp-on probe self-consistency (real alpha) ----
    for backend, on_log in (("dx12", paths[1]), ("vulkan", paths[3])):
        probes, _ = parse_probes(on_log)
        if len(probes) < 6:
            fails.append(f"equal {backend}: expected >=6 probe lines, got {len(probes)}")
            continue
        for p in probes:
            check_interp_row(f"equal {backend}", p, fails)
        print(f"[ok] equal {backend}: {len(probes)} probes lerp/slerp self-consistent")

    # ---- mid suite: alpha twin + orb position twin + cross-backend ----
    mid_probes = []
    for backend, mid_log in (("dx12", paths[8]), ("vulkan", paths[9])):
        probes, text = parse_probes(mid_log)
        mid_probes.append(probe_line_bytes(text))
        if not probes:
            fails.append(f"mid {backend}: no probe lines")
            continue
        frames = clock_alpha_frames(25.0, 3)
        n_ticks_total = sum(t for t, _ in frames)
        alpha_final = frames[-1][1]
        if abs(alpha_final - 0.5) > 1e-3:
            fails.append(f"mid {backend}: twin alpha {alpha_final:.6f} != 0.5")
        # prev = state after (n_ticks_total - 1) ticks, curr = after n_ticks_total
        for name, pidx, center, offset in (
                ("orb-a", 0, CENTER_A, OFFSET_A), ("orb-b", 1, CENTER_B, OFFSET_B)):
            prev_t = orb_world_pos(pidx, center, offset, n_ticks_total - 1)
            curr_t = orb_world_pos(pidx, center, offset, n_ticks_total)
            cand = [p for p in probes if close(p["curr"], curr_t, 2e-3)]
            if not cand:
                fails.append(f"mid {backend}: no probe matches {name} curr twin "
                             f"{curr_t}")
                continue
            p = cand[0]
            if not close(p["prev"], prev_t, 2e-3):
                fails.append(f"mid {backend} {name}: prev {p['prev']} != twin {prev_t}")
            if not (close(p["interp"], lerp(prev_t, curr_t, alpha_final), 2e-3)):
                fails.append(f"mid {backend} {name}: interp != twin lerp @0.5")
            check_interp_row(f"mid {backend} {name}", p, fails)
            print(f"[ok] mid {backend} {name}: twin prev/curr/interp @alpha=0.5")
    if mid_probes[0] != mid_probes[1]:
        fails.append("mid: probe lines differ between backends (CPU-determinism)")
    else:
        print(f"[ok] mid: {len(mid_probes[0])} probe lines byte-equal across backends")

    # ---- pause suite: frozen everywhere ----
    for backend, pause_log in (("dx12", paths[10]), ("vulkan", paths[11])):
        probes, text = parse_probes(pause_log)
        if not probes:
            fails.append(f"pause {backend}: no probe lines")
            continue
        for p in probes:
            if not (close(p["prev"], p["curr"], 1e-6) and
                    close(p["interp"], p["curr"], 1e-6)):
                fails.append(f"pause {backend} k={p['k']}: state not frozen")
            if abs(p["alpha"]) > 1e-9:
                fails.append(f"pause {backend} k={p['k']}: alpha {p['alpha']} != 0")
        status = GAME_STATUS.findall(text)
        if not status or status[-1][2] != "1":
            fails.append(f"pause {backend}: game.pause not registered (paused != 1)")
        print(f"[ok] pause {backend}: {len(probes)} probes frozen, alpha=0, paused=1")

    if fails:
        for f in fails:
            print("[FAIL]", f)
        return 1
    print("[PASS] M-GL3 verify: PNG byte-identity + clock alpha twin + "
          "spin-system position twin + slerp self-consistency all green")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
