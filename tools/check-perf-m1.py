#!/usr/bin/env python3
# M-PERF1 (perf-m1) — per-pass GPU timing log-anchor checker (Q-P4/Q-P5).
#
# Pure stdlib. Verifies viewer logs for --pass-timing runs:
#   anchors <log> <preset>  every pass scheduled by <preset> must emit
#                           "[nl:info|pass-timing] <name> first resolve" and
#                           the run must end with deviceErrors=0.
#   off     <log>           OFF shot: zero pass-timing lines + deviceErrors=0
#                           (Q-P5 default-off = zero baseline disturbance).
#   selftest                mechanical self-check on synthetic logs.
#
# Presets mirror the Scene::draw instrumentation (gpu_timer.hpp PassTimers):
#   hiz    -> csCull / hizBuild / mainDraw
#   filter -> visRaster / matResolve / csPlace / csTraceProbe / csGather /
#             csFilterTemporal / csFilterSpatial / csGatherCompose
#   shadow -> visRaster / csDirect
# (csRefGI is exercised by --mode reference manually; rt/dag-only modes reuse
#  the hiz preset minus hizBuild when hiz assets are absent.)

import sys

PRESETS = {
    "hiz": ["csCull", "hizBuild", "mainDraw"],
    "filter": ["visRaster", "matResolve", "csPlace", "csTraceProbe",
               "csGather", "csFilterTemporal", "csFilterSpatial",
               "csGatherCompose"],
    "shadow": ["visRaster", "csDirect"],
}


def fail(msg):
    print("[perf-m1] FAIL: " + msg)
    return 1


def check_anchor_line(line, name):
    # Anchor produced by Scene::pollPassTimers, e.g.:
    #   [nl:info|pass-timing] csCull first resolve: 0.301696 ms #3
    # ms value is machine-dependent -> only the fixed prefix is matched.
    return ("pass-timing] " + name + " first resolve") in line


def check_device_errors(line):
    return "deviceErrors=0" in line


def cmd_anchors(path, preset):
    if preset not in PRESETS:
        return fail("unknown preset '" + str(preset) + "' (expected hiz|filter|shadow)")
    expected = list(PRESETS[preset])
    found = {n: False for n in expected}
    device_ok = False
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                for n in expected:
                    if not found[n] and check_anchor_line(line, n):
                        found[n] = True
                if check_device_errors(line):
                    device_ok = True
    except OSError as e:
        return fail("cannot read log: " + str(e))
    missing = [n for n in expected if not found[n]]
    if missing:
        return fail(preset + " anchors missing: " + ", ".join(missing))
    if not device_ok:
        return fail("deviceErrors=0 anchor missing")
    print("[perf-m1] PASS: " + preset + " anchors (" + ", ".join(expected) +
          ") + deviceErrors=0")
    return 0


def cmd_off(path):
    saw_timing = False
    device_ok = False
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if "pass-timing]" in line:
                    saw_timing = True
                if check_device_errors(line):
                    device_ok = True
    except OSError as e:
        return fail("cannot read log: " + str(e))
    if saw_timing:
        return fail("OFF log contains pass-timing lines (default-off must be silent)")
    if not device_ok:
        return fail("deviceErrors=0 anchor missing")
    print("[perf-m1] PASS: OFF shot silent (0 pass-timing lines) + deviceErrors=0")
    return 0


def cmd_selftest():
    # Synthetic logs through the same matcher functions used by the commands.
    on_line = "[nl:info|pass-timing] csCull first resolve: 0.301696 ms #3"
    other_line = "[nl:info|pass-timing] hizBuild first resolve: 0.05 ms #3"
    done_line = "[nl:info|viewer] shutdown ok (frames=16 screenshots=1 deviceErrors=0) #16"
    bad_line = "[nl:info|viewer] shutdown ok (frames=16 screenshots=1 deviceErrors=2) #16"
    checks = [
        (check_anchor_line(on_line, "csCull"), True, "anchor prefix match"),
        (check_anchor_line(other_line, "csCull"), False, "anchor name mismatch"),
        (check_anchor_line("[nl:info|scene] csCull first resolve: 1 ms", "csCull"),
         False, "channel must be pass-timing"),
        (check_device_errors(done_line), True, "deviceErrors=0 match"),
        (check_device_errors(bad_line), False, "deviceErrors=2 rejected"),
    ]
    for got, want, label in checks:
        if got != want:
            print("[perf-m1] selftest FAIL: " + label)
            return 1
    print("check-perf-m1 selftest: PASS (5/5)")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "selftest":
        return cmd_selftest()
    if len(argv) == 4 and argv[1] == "anchors":
        return cmd_anchors(argv[2], argv[3])
    if len(argv) == 3 and argv[1] == "off":
        return cmd_off(argv[2])
    print("usage: check-perf-m1.py anchors <log> <hiz|filter|shadow>"
          " | off <log> | selftest")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
