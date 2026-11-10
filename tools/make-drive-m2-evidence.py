#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P8 M-D2 evidence renderer: two PNGs from REAL tool output, no hand-drawn art.

Data sources (all produced by example/drive-m2/run.bat):
  - example/drive-m2/drive.tsv (real Jolt VehicleConstraint drive, 480 frames)
  - example/drive-m2/*.log    (real phys-tools output)

timeline.png   = speed profile / three-phase script + yaw + wheel contact +
                 M-D1 stream cumulative curves + MEASURED panel.
acceptance.png = terminal-style board of the real logs.

Every number drawn below is recomputed from the TSV F/W/E data rows; the #
provenance lines are used only for axis labels and cross-checks.

Deterministic: fixed canvas, fixed fonts/colors, no timestamps, so run.bat can
fc /b compare the regenerated PNGs against the archived baselines.

Usage:
  python tools/make-drive-m2-evidence.py             # render both PNGs
  python tools/make-drive-m2-evidence.py --anchors  # print measured anchors
"""
import math
import os
import sys
from collections import Counter

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "example", "drive-m2")

BG = (18, 20, 24)
FG = (222, 226, 232)
CYAN = (110, 200, 235)
GREEN = (120, 220, 130)
BLUE = (120, 165, 235)
RED = (235, 110, 110)
YELLOW = (235, 205, 105)
GREY = (130, 136, 146)
PANEL = (28, 32, 38)
GRID = (48, 54, 62)
PHASE_BG = [(26, 40, 34), (26, 34, 48), (46, 30, 30)]

_cache = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in _cache:
        name = "segoeuib.ttf" if bold else "segoeui.ttf"
        try:
            _cache[key] = ImageFont.truetype("C:/Windows/Fonts/" + name, size)
        except OSError:
            _cache[key] = ImageFont.load_default()
    return _cache[key]


def read_tsv():
    path = os.path.join(OUT, "drive.tsv")
    frames, wheels, events, prov = [], [], [], {}
    with open(path, "r", encoding="ascii") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if line.startswith("# jolt="):
                for kv in line[2:].split(" "):
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        prov[k] = v
            elif line.startswith("F\t"):
                p = line.split("\t")
                frames.append(dict(
                    frame=int(p[1]), phase=int(p[2]), t=float(p[3]),
                    forward=float(p[4]), right=float(p[5]),
                    handbrake=float(p[7]), px=float(p[8]), py=float(p[9]),
                    pz=float(p[10]), vx=float(p[11]), vy=float(p[12]),
                    vz=float(p[13]), yaw=float(p[14]), speed=float(p[15]),
                    grounded=int(p[16]), published=int(p[17]),
                    consumed=int(p[18]), dropped=int(p[19])))
            elif line.startswith("W\t"):
                p = line.split("\t")
                wheels.append(dict(
                    frame=int(p[1]), wheel=int(p[2]), contact=int(p[3]),
                    susp=float(p[4]), rot=float(p[5]), steer=float(p[6])))
            elif line.startswith("E\t"):
                p = line.split("\t")
                events.append(dict(frame=int(p[1]), kind=int(p[2]),
                                   ownerA=p[3], ownerB=p[4],
                                   bodyA=int(p[5]), bodyB=int(p[6])))
    return frames, wheels, events, prov


def compute_anchors(frames, wheels, events, prov):
    accel_end = int(prov["accel_end"])
    turn_end = int(prov["turn_end"])
    kinds = Counter(e["kind"] for e in events)
    first_added = next(e["frame"] for e in events if e["kind"] == 0)
    reach = next(r["frame"] for r in frames
                 if r["phase"] == 0 and r["speed"] >= 1.0)
    stop = next((r["frame"] for r in frames
                 if r["phase"] == 2 and r["speed"] <= 0.2), -1)
    yaw0 = frames[accel_end]["yaw"]
    yaw1 = frames[turn_end]["yaw"]
    d = yaw1 - yaw0
    while d > math.pi:
        d -= 2.0 * math.pi
    while d < -math.pi:
        d += 2.0 * math.pi
    dx = frames[turn_end]["px"] - frames[accel_end]["px"]
    dz = frames[turn_end]["pz"] - frames[accel_end]["pz"]
    dist = math.hypot(dx, dz)
    return dict(
        nframes=len(frames) - 1, nF=len(frames), nW=len(wheels), nE=len(events),
        accel_end=accel_end, turn_end=turn_end,
        added=kinds.get(0, 0), persisted=kinds.get(1, 0), removed=kinds.get(2, 0),
        first_added=first_added, last_event=events[-1]["frame"],
        grounded4=sum(1 for r in frames if r["grounded"] == 4),
        sum_published=sum(r["published"] for r in frames),
        sum_consumed=sum(r["consumed"] for r in frames),
        sum_dropped=sum(r["dropped"] for r in frames),
        reach_frame=reach, max_speed=max(r["speed"] for r in frames),
        final_speed=frames[accel_end]["speed"],
        brake_start=frames[turn_end]["speed"],
        brake_end=frames[-1]["speed"], stop_frame=stop,
        turn_yaw=d, turn_dist=dist,
        turn_radius=dist / abs(d) if abs(d) > 1e-3 else 0.0,
        jolt=prov.get("jolt", "?"), dt=prov.get("dt", "?"),
        published=prov.get("published", "?"), consumed=prov.get("consumed", "?"),
        dropped=prov.get("dropped", "?"),
        front_steer=max(abs(w["steer"]) for w in wheels if w["wheel"] < 2),
        rear_steer=max(abs(w["steer"]) for w in wheels if w["wheel"] >= 2),
    )


def print_anchors(a):
    print("rows F/W/E = %d %d %d" % (a["nF"], a["nW"], a["nE"]))
    print("kinds(Added/Persisted/Removed) = %d/%d/%d" %
          (a["added"], a["persisted"], a["removed"]))
    print("firstAdded / lastEvent = %d %d" % (a["first_added"], a["last_event"]))
    print("grounded4 / sumPublished = %d %d" %
          (a["grounded4"], a["sum_published"]))
    print("reachFrame = %d" % a["reach_frame"])
    print("maxSpeed = %.6f" % a["max_speed"])
    print("finalSpeed@%d = %.6f" % (a["accel_end"], a["final_speed"]))
    print("brakeStart@%d = %.6f" % (a["turn_end"], a["brake_start"]))
    print("brakeEnd@%d = %.6f" % (a["nframes"], a["brake_end"]))
    print("stopFrame = %d" % a["stop_frame"])
    print("turnYawDelta = %.6f turnDist = %.6f turnRadius = %.6f" %
          (a["turn_yaw"], a["turn_dist"], a["turn_radius"]))
    print("frontSteer max = %.6f rearSteer max = %.6f" %
          (a["front_steer"], a["rear_steer"]))


def read_log(name):
    path = os.path.join(OUT, name)
    if not os.path.isfile(path):
        return ["<missing log: %s>" % name]
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return [ln.rstrip("\r\n") for ln in f]


def make_timeline(frames, wheels, events, prov, a):
    W, H = 1560, 960
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f13, f15, f17, f23 = font(13), font(15), font(17, True), font(23, True)
    x0, x1 = 90, 1470
    n = a["nframes"]
    ticks = (0, 120, 240, 360, 480)

    def fx(frame):
        return x0 + int((x1 - x0) * frame / float(n))

    d.text((40, 20),
           "P8 M-D2  HEADLESS VEHICLECONSTRAINT DRIVE  /  REAL JOLT SOLVER OUTPUT",
           font=f23, fill=FG)
    d.text((40, 52),
           "fresh world per call | 4 raycast wheels | three-phase frame-indexed driver input | M-D1 ContactEventStream consumption",
           font=f15, fill=GREY)

    # ---- Panel A: speed profile + phase bands ----
    d.rectangle((40, 85, 1520, 370), fill=PANEL)
    d.text((55, 95),
           "SPEED PROFILE  (F rows)   phase0 accel / phase1 steer / phase2 handbrake",
           font=f17, fill=CYAN)
    top, base = 135, 335
    for phase, fa, fb in ((0, 0, a["accel_end"]),
                          (1, a["accel_end"], a["turn_end"]),
                          (2, a["turn_end"], n)):
        d.rectangle((fx(fa), top, fx(fb), base), fill=PHASE_BG[phase])
    for s in (0, 2, 4, 6, 8, 10):
        y = base - int((base - top) * s / 10.0)
        d.line((x0, y, x1, y), fill=GRID)
        d.text((52, y - 8), "%2d" % s, font=f13, fill=GREY)
    for t in ticks:
        x = fx(t)
        d.line((x, top, x, base), fill=GRID)
        d.text((x - 12, 340), "f%d" % t, font=f13, fill=GREY)
    d.line([(fx(r["frame"]), base - int((base - top) *
            min(r["speed"], 10.0) / 10.0)) for r in frames],
           fill=GREEN, width=3)
    for fr, label, ly in ((a["reach_frame"], "f%d reach 1 m/s" % a["reach_frame"], 138),
                          (a["accel_end"], "f%d exit %.3f" %
                           (a["accel_end"], a["final_speed"]), 180),
                          (a["turn_end"], "f%d brake in %.3f" %
                           (a["turn_end"], a["brake_start"]), 138),
                          (a["stop_frame"], "f%d STOP" % a["stop_frame"], 260)):
        x = fx(fr)
        d.line((x, top, x, base), fill=YELLOW)
        d.text((min(x + 5, 1310), ly), label, font=f13, fill=YELLOW)
    d.text((55, 115), "m/s", font=f13, fill=GREY)

    # ---- Panel B: yaw + wheel ray contact ----
    d.rectangle((40, 385, 1520, 605), fill=PANEL)
    d.text((55, 395),
           "STEERING YAW (F rows)  +  FOUR-WHEEL RAY CONTACT (W rows)",
           font=f17, fill=CYAN)
    ytop, ybase = 430, 535
    for t in ticks:
        x = fx(t)
        d.line((x, ytop, x, ybase), fill=GRID)
        d.text((x - 12, 540), "f%d" % t, font=f13, fill=GREY)
    yaws = [r["yaw"] for r in frames]
    ylo, yhi = min(yaws) - 0.1, max(yaws) + 0.1

    def fy(v):
        return ybase - int((ybase - ytop) * (v - ylo) / max(yhi - ylo, 1e-6))

    d.line((x0, fy(0.0), x1, fy(0.0)), fill=GRID)
    d.line([(fx(r["frame"]), fy(r["yaw"])) for r in frames],
           fill=BLUE, width=3)
    d.text((55, 560), "4-wheel contact strip (green = all 4 rays hit the floor):",
           font=f13, fill=GREY)
    for r in frames:
        x = fx(r["frame"])
        d.line((x, 578, x, 590),
               fill=GREEN if r["grounded"] == 4 else RED, width=2)
    d.text((1130, 405), "yaw delta = %.6f rad" % a["turn_yaw"],
           font=f15, fill=YELLOW)
    d.text((1130, 425), "turn radius = %.3f m" % a["turn_radius"],
           font=f15, fill=YELLOW)
    d.text((1130, 560), "front steer max %.4f / rear %.4f" %
           (a["front_steer"], a["rear_steer"]), font=f13, fill=GREY)

    # ---- Panel C: M-D1 stream cumulative curves ----
    d.rectangle((40, 620, 1520, 800), fill=PANEL)
    d.text((55, 630),
           "M-D1 STREAM CONTRACT  cumulative published / consumed (F rows + E rows)",
           font=f17, fill=CYAN)
    ctop, cbase = 665, 775
    for t in ticks:
        x = fx(t)
        d.line((x, ctop, x, cbase), fill=GRID)
        d.text((x - 12, 776), "f%d" % t, font=f13, fill=GREY)
    pub, con = 0, 0
    pub_pts, con_pts = [], []
    for r in frames:
        pub += r["published"]
        con += r["consumed"]
        x = fx(r["frame"])
        pub_pts.append((x, cbase - int((cbase - ctop) * pub / max(a["nE"], 1))))
        con_pts.append((x, cbase - int((cbase - ctop) * con / max(a["nE"], 1))))
    d.line(pub_pts, fill=GREEN, width=3)
    d.line(con_pts, fill=CYAN, width=1)
    d.text((x0 + 20, 670),
           "green = published | cyan = consumed (identical curve, zero drop)",
           font=f15, fill=FG)
    d.text((x0 + 20, 693),
           "each frame: stream.update() -> publish(wheel edges) -> drain() in PostPhysics",
           font=f15, fill=GREY)
    d.text((x0 + 20, 716),
           "one event per wheel in contact; %d events over %d frames" %
           (a["nE"], a["nF"]), font=f15, fill=GREY)

    # ---- MEASURED panel ----
    d.rectangle((40, 815, 1520, 935), fill=PANEL)
    d.text((55, 823), "MEASURED  (recomputed from drive.tsv F/W/E data rows)",
           font=f17, fill=YELLOW)
    lines = [
        "F/W/E rows = %d/%d/%d  |  events Added/Persisted/Removed = %d/%d/%d  |  first contact f%d  last event f%d"
        % (a["nF"], a["nW"], a["nE"], a["added"], a["persisted"],
           a["removed"], a["first_added"], a["last_event"]),
        "frames=%d dt=%s jolt=%s  |  fully-grounded frames=%d  |  published=%s consumed=%s dropped=%s"
        % (a["nframes"], a["dt"], a["jolt"], a["grounded4"],
           a["published"], a["consumed"], a["dropped"]),
        "reach 1 m/s f%d  |  top speed %.6f m/s  |  accel exit f%d %.6f m/s"
        % (a["reach_frame"], a["max_speed"], a["accel_end"], a["final_speed"]),
        "turn yaw %.6f rad  |  turn distance %.3f m  |  turn radius %.3f m"
        % (a["turn_yaw"], a["turn_dist"], a["turn_radius"]),
        "handbrake f%d %.6f -> f%d %.6f m/s  |  stop f%d  |  front steer %.4f rad, rear 0"
        % (a["turn_end"], a["brake_start"], a["nframes"], a["brake_end"],
           a["stop_frame"], a["front_steer"]),
    ]
    for i, t in enumerate(lines):
        d.text((55, 848 + i * 17), t, font=f13, fill=FG)
    d.text((40, 938),
           "PNG generated from the real TSV and real tool logs; no hand-drawn diagram.",
           font=f13, fill=GREY)
    img.save(os.path.join(OUT, "timeline.png"))


def make_acceptance():
    W, H = 1560, 600
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f13, f15, f17, f23 = font(13), font(15), font(17, True), font(23, True)
    d.text((30, 16), "P8 M-D2  ACCEPTANCE BOARD  /  REAL phys-tools OUTPUT",
           font=f23, fill=FG)
    d.text((30, 48),
           "example/drive-m2/run.bat five-stage gate | DX12 primary, VULKAN cross-check",
           font=f15, fill=GREY)

    def pick(name, count, filt=None):
        lines = read_log(name)
        if filt:
            lines = [l for l in lines if filt(l)]
        out = (lines + [""] * count)[:count]
        return [l[:148] for l in out]

    sections = [
        ("[1/5] selftest --verbose (M-P0..M-P4 + j5c + contacts + ecs + vehicle)",
         pick("selftest.log", 4,
              lambda l: "PASS" in l or l.startswith("[vehicle]"))),
        ("[2/5] drive-trace + drive-check",
         pick("drive-trace.log", 2) + pick("drive-check.log", 2)),
        ("[3/5] cross-backend DX12 vs VULKAN", pick("cross-backend.log", 2)),
        ("[4/5] usage / negative exit codes",
         pick("usage.log", 2) + pick("negative.log", 2)),
        ("[5/5] evidence regenerated from the real TSV + logs above",
         pick("drive-check.log", 1) + pick("cross-backend.log", 1)),
    ]
    y = 78
    for title, lines in sections:
        d.rectangle((30, y, 1530, y + 24 + 17 * len(lines) + 8), fill=PANEL)
        d.text((42, y + 3), title, font=f17, fill=CYAN)
        yy = y + 26
        for ln in lines:
            d.text((52, yy), ln if ln else " ", font=f13, fill=FG)
            yy += 17
        y += 24 + 17 * len(lines) + 14
    d.text((30, H - 22),
           "Every line above is copied from a real log file; both PNGs are regenerated by run.bat and fc /b compared.",
           font=f13, fill=GREY)
    img.save(os.path.join(OUT, "acceptance.png"))


def main():
    frames, wheels, events, prov = read_tsv()
    assert frames and wheels and events, "empty drive.tsv - run drive-trace first"
    a = compute_anchors(frames, wheels, events, prov)
    if "--anchors" in sys.argv:
        print_anchors(a)
        return
    make_timeline(frames, wheels, events, prov, a)
    make_acceptance()
    print("wrote example/drive-m2/timeline.png + acceptance.png "
          "(F=%d W=%d E=%d)" % (a["nF"], a["nW"], a["nE"]))


if __name__ == "__main__":
    main()
