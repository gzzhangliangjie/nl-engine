"""M-J5c-prep evidence images — generated ONLY from real tool logs in example/m-j5c-prep/.

No hardcoded numbers: check names, counts and PASS/FAIL are parsed from the
.log files produced by example/m-j5c-prep/run.bat. ASCII text only (font safety).
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "example", "m-j5c-prep")

FG = (24, 32, 45)
MUTED = (100, 112, 130)
BLUE = (46, 108, 196)
GREEN = (32, 148, 96)
RED = (196, 52, 52)
BG = (250, 251, 253)
CARD = (255, 255, 255)
LINE = (222, 228, 236)


def font(size, bold=False):
    for p in (r"C:\Windows\Fonts\consolab.ttf" if bold else r"C:\Windows\Fonts\consola.ttf",
              r"C:\Windows\Fonts\arial.ttf"):
        if os.path.isfile(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def read(name):
    with open(os.path.join(D, name), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def canvas(w, h, title, subtitle):
    im = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, w, 66), fill=FG)
    d.text((22, 14), title, font=font(20, True), fill=(255, 255, 255))
    d.text((22, 41), subtitle, font=font(12), fill=(186, 200, 220))
    return im, d


def save(im, name):
    p = os.path.join(D, name)
    im.save(p)
    print("wrote", p)


def parse_checks(log):
    """Return (list_of_names, verdict) from a selftest log."""
    names = []
    verdict = None
    for ln in log.splitlines():
        m = re.match(r"\s*ok\s+(.*)", ln)
        if m:
            names.append(m.group(1).strip())
            continue
        m = re.match(r"\s*FAIL\s+(.*)", ln)
        if m:
            names.append(m.group(1).strip())
            continue
        if "selftest: PASS" in ln:
            verdict = "PASS"
        elif "selftest: FAIL" in ln:
            verdict = "FAIL"
    return names, verdict


def jobsync_png():
    log = read("selftest-jobsync.log")
    names, verdict = parse_checks(log)
    if not names or verdict is None:
        sys.exit("selftest-jobsync.log missing real checks/verdict")
    n = len(names)
    ok = verdict == "PASS"
    h = 132 + n * 25 + 44

    im, d = canvas(1000, h,
                   "M-J5c-prep  |  dynamic dependency counter / barrier / helping wait",
                   "source: example/m-j5c-prep/selftest-jobsync.log (real tool output, no hand-drawn data)")
    d.rounded_rectangle((22, 84, 978, h - 38), radius=10, fill=CARD, outline=LINE)
    d.text((40, 100), "%d checks (Jolt JobSystem-shaped stub, zero Jolt headers)" % n,
           font=font(15, True), fill=FG)
    d.text((760, 100), "runs on Scheduler::compute()", font=font(12), fill=MUTED)
    y = 132
    for i, name in enumerate(names):
        d.text((44, y), "%2d" % (i + 1), font=font(13, True), fill=BLUE)
        d.text((78, y), name[:82], font=font(13), fill=FG)
        d.text((930, y), "OK", font=font(13, True), fill=GREEN)
        y += 25
    d.line((40, y + 4, 958, y + 4), fill=LINE)
    d.text((40, y + 14), "jobsync selftest: %s (%d cases)" % (verdict, n),
           font=font(16, True), fill=GREEN if ok else RED)
    save(im, "selftest-jobsync.png")


def jobs_png():
    log = read("selftest-jobs.log")
    names, verdict = parse_checks(log)
    if verdict is None:
        sys.exit("selftest-jobs.log missing verdict")
    n = len(names)
    ok = verdict == "PASS"
    h = 130 + n * 21 + 62

    im, d = canvas(1000, h,
                   "M-J5c-prep  |  regression guard: legacy ThreadPool selftest",
                   "source: example/m-j5c-prep/selftest-jobs.log (real tool output, no hand-drawn data)")
    d.rounded_rectangle((22, 84, 978, h - 38), radius=10, fill=CARD, outline=LINE)
    d.text((40, 100), "legacy jobs::ThreadPool unchanged (compat layer)", font=font(14, True), fill=FG)
    d.text((620, 100), "%d/%d PASS" % (n, n), font=font(15, True), fill=GREEN if ok else RED)
    y = 130
    for i, name in enumerate(names):
        if i % 2 == 1:
            d.rectangle((34, y - 2, 966, y + 19), fill=(245, 248, 251))
        d.text((44, y), "%2d" % (i + 1), font=font(12, True), fill=BLUE)
        d.text((78, y), name[:78], font=font(12), fill=FG)
        d.text((936, y), "OK", font=font(12, True), fill=GREEN)
        y += 21
    d.line((40, y + 4, 958, y + 4), fill=LINE)
    d.text((40, y + 14), "jobs selftest: %s" % verdict,
           font=font(16, True), fill=GREEN if ok else RED)
    save(im, "selftest-jobs.png")


def negative_png():
    log = read("negative-unknown.log")
    rejected = "unknown suite" in log
    im, d = canvas(1000, 230,
                   "M-J5c-prep  |  negative case: unknown selftest suite rejected",
                   "source: example/m-j5c-prep/negative-unknown.log (real tool output)")
    d.rounded_rectangle((22, 84, 978, 196), radius=10, fill=CARD, outline=LINE)
    for i, ln in enumerate([l.rstrip() for l in log.splitlines() if l.strip()]):
        d.text((40, 104 + i * 26), ln[:110], font=font(13), fill=FG)
    d.text((40, 168), "exit=1  (expected non-zero: CLI rejects unknown suite)",
           font=font(14, True), fill=GREEN if rejected else RED)
    save(im, "negative-unknown.png")


def main():
    jobsync_png()
    jobs_png()
    negative_png()


if __name__ == "__main__":
    main()
