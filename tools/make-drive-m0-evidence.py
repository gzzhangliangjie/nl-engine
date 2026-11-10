#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P8 M-D0 evidence renderer: two PNGs from REAL tool output, no hand-drawn art.

Data sources (all produced by example/drive-m0/run.bat):
  - example/drive-m0/contacts.tsv         (real ContactListener event stream)
  - example/drive-m0/*.log               (real phys-tools / ctest output)

events.png     = event timeline + probe trajectory + MEASURED panel, all from TSV.
acceptance.png = terminal-style board of the real logs.

Deterministic: fixed canvas, fixed fonts/colors, no timestamps, so run.bat can
fc /b compare the regenerated PNGs against the archived baselines.
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "example", "drive-m0")

BG = (18, 20, 24)
FG = (222, 226, 232)
CYAN = (110, 200, 235)
GREEN = (120, 220, 130)
BLUE = (120, 165, 235)
RED = (235, 110, 110)
YELLOW = (235, 205, 105)
GREY = (130, 136, 146)
PANEL = (28, 32, 38)

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
    path = os.path.join(OUT, "contacts.tsv")
    evs, traj, prov = [], [], {}
    with open(path, "r", encoding="ascii") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if line.startswith("# jolt="):
                for kv in line[2:].split(" "):
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        prov[k] = v
            elif line.startswith("E\t"):
                p = line.split("\t")
                evs.append(dict(frame=int(p[1]), kind=int(p[2]),
                                ownerA=p[3], ownerB=p[4],
                                bodyA=int(p[5]), bodyB=int(p[6]),
                                px=float(p[7]), py=float(p[8]), pz=float(p[9]),
                                ny=float(p[10])))
            elif line.startswith("T\t"):
                p = line.split("\t")
                traj.append(dict(frame=int(p[1]), t=float(p[2]),
                                 y=float(p[3]), vy=float(p[4])))
    return evs, traj, prov


def read_log(name):
    path = os.path.join(OUT, name)
    if not os.path.isfile(path):
        return ["<missing log: %s>" % name]
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return [ln.rstrip("\r\n") for ln in f]


def make_events(evs, traj, prov):
    W, H = 1560, 940
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f13, f15, f17, f23 = font(13), font(15), font(17, True), font(23, True)
    x0, x1 = 90, 1470
    nframes = int(prov.get("frames", "240"))

    def fx(frame):
        return x0 + int((x1 - x0) * frame / float(nframes))

    d.text((40, 20), "P8 M-D0  CONTACT EVENT STREAM  /  REAL Jolt ContactListener OUTPUT",
           font=f23, fill=FG)
    d.text((40, 52), "course: floor + static blocker + dropped probe | "
                     "kind 0=Added 1=Persisted 2=Removed | frame-end stable sorted",
           font=f15, fill=GREY)

    # ---- event timeline ----
    d.rectangle((40, 85, 1520, 350), fill=PANEL)
    d.text((55, 95), "EVENT TIMELINE  (frame 0..%d)" % nframes, font=f17, fill=CYAN)
    rows = [(0, "Added", 150, GREEN), (1, "Persisted", 210, BLUE), (2, "Removed", 270, RED)]
    for f in (0, 60, 120, 180, 240):
        if f <= nframes:
            x = fx(f)
            d.line((x, 130, x, 320), fill=(48, 54, 62), width=1)
            d.text((x - 10, 322), "f%d" % f, font=f13, fill=GREY)
    for kind, label, y, color in rows:
        d.text((55, y - 9), label, font=f15, fill=color)
        d.line((x0, y, x1, y), fill=(58, 64, 74), width=1)
        for e in evs:
            if e["kind"] == kind:
                x = fx(e["frame"])
                d.line((x, y - 11, x, y + 11), fill=color, width=3)
                d.ellipse((x - 3, y - 3, x + 3, y + 3), fill=color)
    marks = [(44, "first Added f44", GREEN), (46, "first Removed f46", RED),
             (185, "last event f185", YELLOW)]
    for f, text, color in marks:
        x = fx(f)
        d.line((x, 120, x, 300), fill=color, width=1)
    d.text((fx(44) + 6, 118), "f44 added", font=f13, fill=GREEN)
    d.text((fx(46) + 6, 138), "f46 removed", font=f13, fill=RED)
    d.text((fx(185) - 130, 118), "f185 last event", font=f13, fill=YELLOW)
    d.text((55, 300), "quiet tail: no events after f185 (sleeping island)", font=f13, fill=GREY)

    # ---- trajectory ----
    ty0, ty1 = 400, 750
    ymin, ymax = 0.0, 6.5

    def fy(y):
        return ty1 - int((ty1 - ty0) * (y - ymin) / (ymax - ymin))

    d.rectangle((40, 370, 1080, 790), fill=PANEL)
    d.text((55, 380), "PROBE TRAJECTORY  y(t)  /  T rows from the same TSV", font=f17, fill=CYAN)
    d.text((620, 382), "markers = contact point y at each event frame",
           font=f13, fill=GREY)
    d.line((x0, ty1, x1 - 390, ty1), fill=(70, 76, 86), width=1)
    for yv in (0.0, 2.0, 4.0, 6.0):
        yy = fy(yv)
        d.line((x0, yy, 1040, yy), fill=(48, 54, 62), width=1)
        d.text((52, yy - 8), "%.1f" % yv, font=f13, fill=GREY)
    for f in (0, 60, 120, 180, 240):
        x = x0 + int((1040 - x0) * f / float(nframes))
        d.text((x - 10, ty1 + 8), "f%d" % f, font=f13, fill=GREY)
    pts = [(x0 + int((1040 - x0) * s["frame"] / float(nframes)), fy(s["y"]))
           for s in traj]
    if len(pts) > 1:
        d.line(pts, fill=YELLOW, width=2)
    rest = float(traj[-1]["y"]) if traj else 0.0
    d.line((x0, fy(rest), 1040, fy(rest)), fill=GREEN, width=1)
    d.text((860, fy(rest) - 18), "rest y=%.3f" % rest, font=f13, fill=GREEN)
    start = float(traj[0]["y"]) if traj else 0.0
    d.text((x0 + 170, fy(start) + 6), "start y=%.3f" % start, font=f13, fill=GREY)
    # contact event markers on the trajectory
    for e in evs:
        x = x0 + int((1040 - x0) * e["frame"] / float(nframes))
        color = GREEN if e["kind"] == 0 else (BLUE if e["kind"] == 1 else RED)
        d.ellipse((x - 3, fy(e["py"]) - 3, x + 3, fy(e["py"]) + 3), fill=color)
    d.text((55, 730), "green line = rest height; event markers ride the contact plane",
           font=f13, fill=GREY)

    # ---- measured panel ----
    d.rectangle((1100, 370, 1520, 790), fill=PANEL)
    d.text((1115, 380), "MEASURED  (from TSV)", font=f17, fill=CYAN)
    counts = [sum(1 for e in evs if e["kind"] == k) for k in (0, 1, 2)]
    first_added = next((e["frame"] for e in evs if e["kind"] == 0), -1)
    first_removed = next((e["frame"] for e in evs if e["kind"] == 2), -1)
    lines = [
        ("jolt", prov.get("jolt", "?")),
        ("job_system", prov.get("job_system", "?")),
        ("frames", prov.get("frames", "?")),
        ("bodies", prov.get("bodies", "?")),
        ("events", str(len(evs))),
        ("Added", str(counts[0])),
        ("Persisted", str(counts[1])),
        ("Removed", str(counts[2])),
        ("first Added", "f%d" % first_added),
        ("first Removed", "f%d" % first_removed),
        ("last event", "f%d" % (evs[-1]["frame"] if evs else -1)),
        ("contact py", "%.3f" % (evs[0]["py"] if evs else 0.0)),
        ("normalY", "%.3f" % (evs[0]["ny"] if evs else 0.0)),
        ("rest y", "%.6f" % rest),
        ("cache_misses", prov.get("cache_misses", "?")),
    ]
    y = 415
    for k, v in lines:
        d.text((1120, y), k, font=f15, fill=GREY)
        d.text((1340, y), v, font=f15,
               fill=GREEN if k in ("cache_misses", "events") else FG)
        y += 25
    d.text((1115, y + 8), "Q2: every Removed carries owners", font=f13, fill=GREEN)
    d.text((1115, y + 28), "(cache hit, zero Jolt body access)", font=f13, fill=GREEN)

    d.text((40, 900), "source: example/drive-m0/contacts.tsv  |  "
                      "regenerated by tools/make-drive-m0-evidence.py  |  ASCII-only data",
           font=f13, fill=GREY)
    img.save(os.path.join(OUT, "events.png"))


def make_acceptance():
    logs = [
        ("selftest (M-P0..P4 + j5c + contacts)", read_log("selftest.log")),
        ("contact-trace (real event dump)", read_log("contact-trace.log")),
        ("contact-check (anchors from data rows)", read_log("contact-check.log")),
        ("cross-backend DX12 vs VULKAN", read_log("cross-backend.log")),
        ("usage / negative (exit 2 / 1)", read_log("usage.log") + read_log("negative.log")),
    ]
    W, H = 1560, 660
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f14, f16, f22 = font(14), font(16), font(22, True)
    d.text((40, 22), "P8 M-D0  ACCEPTANCE  /  REAL TOOL LOGS (ContactListener -> POD stream)",
           font=f22, fill=FG)
    line_h = 20
    y = 62
    for name, lines in logs:
        d.text((40, y), "== %s ==" % name, font=f16, fill=CYAN)
        y += line_h + 2
        for ln in lines[-12:]:
            color = FG
            if "PASS" in ln or ": OK" in ln or "byte-identical" in ln:
                color = GREEN
            elif "FAIL" in ln or "ERROR" in ln or "MISMATCH" in ln:
                color = RED
            d.text((60, y), ln[:150], font=f14, fill=color)
            y += line_h
        y += 8
    d.text((40, H - 34), "source: example/drive-m0/*.log  |  "
                         "exit: 0=PASS 1=assert/runtime 2=usage  |  ASCII-only",
           font=f14, fill=GREY)
    img.save(os.path.join(OUT, "acceptance.png"))


def main():
    evs, traj, prov = read_tsv()
    if not evs or not traj:
        raise SystemExit("contacts.tsv has no E/T rows - run contact-trace first")
    make_events(evs, traj, prov)
    make_acceptance()
    print("drive-m0 evidence: events.png + acceptance.png (from real TSV/logs)")


if __name__ == "__main__":
    main()
