#!/usr/bin/env python3
"""wt_pool.py -- nl-engine worktree occupancy gate (minimal, rea_potato-inspired).

Task ID = milestone ID (M-C1, M-A5.2, S4.2, ...). Milestone IDs natively map to
tracks/worktrees, so no separate task numbering system is introduced.

Why this exists (2026-11-09 incident): a session working the action-system
track deleted *other* tracks' worktrees. Rule fixed into AGENTS.md discipline
#8; this tool makes the rule mechanically checkable:

  - claim   : a conversation MUST claim its track+milestone before touching a wt
  - check   : before touching a wt (esp. delete/cleanup), check occupancy
  - guard   : hard gate for edit/merge/delete ops (exit code driven)
  - status  : registry x lock x git worktree three-way cross-check
  - release : free the claim; prints documentation-gate checklist

Lock file lives at the MAIN worktree root (shared across all worktrees via
`git rev-parse --git-common-dir`), so any session from any wt sees one truth.
All output is ASCII English (repo HUD convention).

Usage examples:
  python tools/wt_pool.py claim --track console --milestone M-C1 --note "session X"
  python tools/wt_pool.py check --track material-graph --op delete
  python tools/wt_pool.py status
  python tools/wt_pool.py release --track console --milestone M-C1
  python tools/wt_pool.py selftest
"""

import argparse
import ctypes
import datetime
import json
import os
import re
import socket
import subprocess
import sys

# ---------------------------------------------------------------------------
# Track registry -- single source of truth mirrors docs/PARALLEL-TRACKS.md 7.1/7.1b
# state: active   = wt exists, track in progress
#        closed   = merged back to master, wt removed, branch kept (do NOT touch)
#        planned  = planned, wt not created yet (claim allowed for pre-booking)
# ---------------------------------------------------------------------------
TRACKS = {
    "material-graph": {
        "wt": "E:/UE_STUDY_2026/nl-engine-material-graph",
        "branch": "feat/material-graph", "state": "closed",
    },
    "job-system": {
        "wt": "E:/UE_STUDY_2026/nl-engine-job-system",
        "branch": "feat/job-system", "state": "closed",
    },
    "action-system": {
        "wt": "E:/UE_STUDY_2026/nl-engine-action-system",
        "branch": "feat/action-system", "state": "closed",
    },
    "console": {
        "wt": "E:/UE_STUDY_2026/nl-engine-console",
        "branch": "feat/console", "state": "active",
    },
    "ecs": {
        "wt": "E:/UE_STUDY_2026/nl-engine-ecs",
        "branch": "feat/ecs", "state": "active",
    },
    "pathfinding": {
        "wt": "E:/UE_STUDY_2026/nl-engine-pathfinding",
        "branch": "feat/pathfinding", "state": "planned",
    },
    "physics": {
        "wt": "E:/UE_STUDY_2026/nl-engine-physics",
        "branch": "feat/physics", "state": "planned",
    },
    "audio": {
        "wt": "E:/UE_STUDY_2026/nl-engine-audio",
        "branch": "feat/audio", "state": "planned",
    },
}

LOCK_NAME = ".wt_pool_busy.json"
# Loose milestone-ID check: M-x[.y], S-x[.y], P9, C1 ... warn only, never block.
MILESTONE_RE = re.compile(r"^(M|S|P|E|V|J|A|C|N|D)[-.\w]*\d", re.IGNORECASE)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def fail(msg, code=1):
    print("ERROR: " + msg)
    sys.exit(code)


def warn(msg):
    print("WARN: " + msg)


def now_iso():
    return datetime.datetime.now().isoformat(timespec="seconds")


def pid_alive(pid):
    """Windows-aware pid liveness check (standard library only)."""
    if not pid or pid <= 0:
        return False
    try:
        if os.name == "nt":
            k32 = ctypes.windll.kernel32
            SYNCHRONIZE = 0x00100000
            h = k32.OpenProcess(SYNCHRONIZE, False, int(pid))
            if h:
                k32.CloseHandle(h)
                return True
            return False
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False
    except Exception:
        return False


def main_root():
    """Repo root of the MAIN worktree (lock lives there, shared by all wts)."""
    env_lock = os.environ.get("NL_WT_POOL_LOCK")  # override for selftest
    if env_lock:
        return env_lock
    try:
        gitdir = subprocess.run(
            ["git", "rev-parse", "--git-common-dir"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        # git-common-dir is "<main-root>/.git" (absolute when run from another wt)
        return os.path.dirname(os.path.abspath(gitdir))
    except Exception:
        # fallback: repo root inferred from this script's location
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lock_path():
    return os.path.join(main_root(), LOCK_NAME)


def load_locks():
    p = lock_path()
    if not os.path.exists(p):
        return {}
    try:
        with open(p, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (json.JSONDecodeError, OSError) as e:
        fail("lock file unreadable: %s (%s)" % (p, e))


def save_locks(locks):
    p = lock_path()
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(locks, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, p)


def fmt_entry(track, e):
    dead = "" if pid_alive(e.get("pid")) else "  [pid-dead, stale?]"
    return ("  %-16s <- %-10s wt=%s pid=%s host=%s since=%s%s%s"
            % (track, e.get("milestone", "?"), e.get("wt", "?"),
               e.get("pid", "?"), e.get("host", "?"),
               e.get("started_at", "?"), dead,
               ("  note=" + e["note"]) if e.get("note") else ""))


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------
def cmd_claim(args):
    track, ms = args.track, args.milestone
    info = TRACKS.get(track)
    if info is None:
        fail("track '%s' not in registry (docs/PARALLEL-TRACKS.md 7.1/7.1b). "
             "Register it there first, then claim." % track)
    if not MILESTONE_RE.match(ms):
        warn("milestone id '%s' looks non-standard; expected like M-C1 / S4.2. "
             "Task ID = milestone ID (user decision 2026-11-09)." % ms)
    if info["state"] == "closed" and not args.force:
        fail("track '%s' is CLOSED (merged to master, wt removed, branch kept). "
             "Never touch it. To reopen: rebuild wt per PARALLEL-TRACKS 7.1b, "
             "update registry, then claim with --force." % track)

    locks = load_locks()
    cur = locks.get(track)
    if cur and cur.get("milestone") != ms and not args.force:
        print("CONFLICT: '%s' already claimed by another task/conversation:"
              % track)
        print(fmt_entry(track, cur))
        fail("refusing to steal the claim. If the occupant is stale/dead, "
             "confirm with the user, then use --force.", code=2)

    locks[track] = {
        "milestone": ms,
        "wt": info["wt"], "branch": info["branch"], "state": info["state"],
        "pid": os.getpid(), "host": socket.gethostname(),
        "started_at": now_iso(),
        "note": args.note or ("conversation ownership"
                              if not cur else cur.get("note", "") + " [force-taken]"),
    }
    save_locks(locks)
    print("CLAIMED: track='%s' milestone='%s'" % (track, ms))
    print(fmt_entry(track, locks[track]))
    if args.force and cur:
        print("NOTE: previous claim force-taken: %s" % cur.get("milestone"))


def cmd_release(args):
    track, ms = args.track, args.milestone
    if track not in TRACKS:
        fail("track '%s' not in registry." % track)
    locks = load_locks()
    cur = locks.get(track)
    if cur is None:
        fail("track '%s' has no active claim (nothing to release)." % track, code=1)
    if cur.get("milestone") != ms and not args.force:
        print(fmt_entry(track, cur))
        fail("milestone mismatch: you said '%s', claim says '%s'. "
             "Refusing to release someone else's claim." % (ms, cur["milestone"]), code=2)

    # documentation gate reminder (soft, per PARALLEL-TRACKS 7.2/7.5)
    milestones_doc = os.path.join(os.getcwd(), "docs", "MILESTONES.md")
    if os.path.exists(milestones_doc):
        with open(milestones_doc, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
        if ms not in text:
            warn("'%s' not found in this tree's docs/MILESTONES.md. Before "
                 "merge: write the acceptance notes (7.2) and sync master-side "
                 "registries (7.5)." % ms)
            if args.strict:
                fail("strict mode: documentation gate not satisfied.")
    else:
        warn("docs/MILESTONES.md not found under cwd; verify doc gate manually.")

    del locks[track]
    save_locks(locks)
    print("RELEASED: track='%s' milestone='%s'" % (track, ms))


def cmd_check(args):
    track = args.track
    info = TRACKS.get(track)
    if info is None:
        print("UNKNOWN: track '%s' not in registry." % track)
        sys.exit(1)
    if info["state"] == "closed" and not args.force:
        print("CLOSED: '%s' merged to master, wt removed (branch kept). "
              "Do not touch. Rebuild per PARALLEL-TRACKS 7.1b to reopen." % track)
        sys.exit(1)
    locks = load_locks()
    cur = locks.get(track)
    if cur is None:
        if info["state"] == "planned":
            print("PLANNED: wt not created yet (%s)." % info["wt"])
            sys.exit(1)
        print("FREE: '%s' has no active claim." % track)
        if not args.milestone:
            warn("working without a claim; run claim first "
                 "(conversation ownership rule, AGENTS.md).")
        sys.exit(0)
    mine = args.milestone and cur.get("milestone") == args.milestone
    print(("MINE: " if mine else "BUSY: ") + "claimed as:")
    print(fmt_entry(track, cur))
    sys.exit(0 if mine else 1)


def cmd_guard(args):
    """Hard gate before touching a wt. Exit 0=allowed, 1=blocked, 2=warn."""
    track, op = args.track, args.op
    info = TRACKS.get(track)
    if info is None:
        fail("guard: unknown track '%s'." % track)
    if info["state"] == "closed":
        if op == "delete":
            fail("guard: '%s' is closed; its wt should already be removed. "
                 "If a leftover directory exists, list it to the user and wait "
                 "for explicit approval (AGENTS.md #8)." % track)
        fail("guard: '%s' is closed (merged to master). Do not edit/merge." % track)
    locks = load_locks()
    cur = locks.get(track)
    if op == "delete":
        if cur is None:
            warn("guard: '%s' unclaimed; wt removal is a close-out op that "
                 "requires merge + registry sync first. Use --force only with "
                 "explicit user approval." % track)
            sys.exit(2)
        fail("guard: '%s' is claimed; release it first.\n%s"
             % (track, fmt_entry(track, cur)))
    # edit / merge
    if cur is None:
        warn("guard: '%s' unclaimed -- claim first before working "
             "(conversation ownership rule)." % track)
        sys.exit(2)
    if args.milestone and cur.get("milestone") != args.milestone:
        print(fmt_entry(track, cur))
        fail("guard: '%s' claimed by a DIFFERENT milestone ('%s'). This "
             "conversation belongs to '%s'. Do not touch."
             % (track, cur["milestone"], args.milestone), code=1)
    if not pid_alive(cur.get("pid")):
        warn("guard: claimant pid %s looks dead (stale claim?); verify with "
             "the user before proceeding." % cur.get("pid"))
        sys.exit(2)
    print("guard: OK '%s' op=%s claimed by '%s'."
          % (track, op, cur["milestone"]))
    sys.exit(0)


def cmd_status(args):
    locks = load_locks()
    print("wt_pool status  (lock=%s)" % lock_path())
    print("registry entries: %d, active claims: %d" % (len(TRACKS), len(locks)))
    for track, info in sorted(TRACKS.items()):
        wt_exists = os.path.isdir(info["wt"])
        cur = locks.get(track)
        tag = {"active": "ACTIVE  ", "closed": "CLOSED  ",
               "planned": "PLANNED "}[info["state"]]
        line = "%s %-16s wt=%s [%s]" % (
            tag, track, "exists" if wt_exists else "absent", info["branch"])
        if info["state"] == "closed" and wt_exists:
            line += "  <== LEFTOVER (should be removed!)"
        print(line)
        if cur:
            print(fmt_entry(track, cur))
    orphan = set(locks) - set(TRACKS)
    if orphan:
        warn("orphan claims not in registry: %s" % ", ".join(sorted(orphan)))


# ---------------------------------------------------------------------------
# selftest (in-process, temp lock file, fake tracks; never touches real lock)
# ---------------------------------------------------------------------------
def cmd_selftest(args):
    import tempfile
    n = [0]

    def ok(name):
        n[0] += 1
        print("  ok %2d  %s" % (n[0], name))

    def expect(fn, cond, name):
        if cond != True:
            fail("selftest FAILED at: %s" % name)
        ok(name)

    with tempfile.TemporaryDirectory() as tmp:
        os.environ["NL_WT_POOL_LOCK"] = tmp
        fake = {
            "console": {"wt": os.path.join(tmp, "wt-console"),
                        "branch": "feat/console", "state": "active"},
            "mg": {"wt": os.path.join(tmp, "wt-mg"),
                   "branch": "feat/mg", "state": "closed"},
        }

        def run(argv):
            """Run CLI in-process; returns (exit_code, stdout_text)."""
            import io, contextlib
            code_buf = {"c": None}

            def fake_exit(c=0):
                code_buf["c"] = c
                raise SystemExit(c)
            real_exit = sys.exit
            sys.exit = fake_exit
            buf = io.StringIO()
            old_argv = sys.argv
            sys.argv = ["wt_pool"] + argv
            saved_tracks = globals()["TRACKS"]
            globals()["TRACKS"] = fake
            try:
                with contextlib.redirect_stdout(buf):
                    main()
            except SystemExit:
                pass
            finally:
                sys.exit = real_exit
                sys.argv = old_argv
                globals()["TRACKS"] = saved_tracks
            return code_buf["c"] or 0, buf.getvalue()

        # 1. claim + lock file written
        c, out = run(["claim", "--track", "console", "--milestone", "M-C1"])
        expect("claim ok", c == 0 and "CLAIMED" in out, "claim console M-C1")
        expect("lock file exists", os.path.exists(os.path.join(tmp, LOCK_NAME)),
               "lock file written to main root")

        # 2. idempotent re-claim (same milestone)
        c, out = run(["claim", "--track", "console", "--milestone", "M-C1"])
        expect("re-claim same milestone ok", c == 0 and "CLAIMED" in out,
               "idempotent re-claim")

        # 3. conflict from another milestone
        c, out = run(["claim", "--track", "console", "--milestone", "M-C2"])
        expect("conflict refused", c == 2 and "CONFLICT" in out,
               "conflict refused without --force")

        # 4. --force steals
        c, out = run(["claim", "--track", "console", "--milestone", "M-C2",
                      "--force"])
        expect("force steal ok", c == 0 and "force-taken" in out, "force steal")

        # 5. closed track refused
        c, out = run(["claim", "--track", "mg", "--milestone", "M-B5"])
        expect("closed track refused", c == 1 and "CLOSED" in out,
               "closed track claim refused")

        # 6. check: MINE vs BUSY
        c, _ = run(["check", "--track", "console", "--milestone", "M-C2"])
        expect("check mine exit0", c == 0, "check reports MINE")
        c, _ = run(["check", "--track", "console", "--milestone", "M-C1"])
        expect("check other exit1", c == 1, "check reports BUSY")

        # 7. guard: wrong milestone blocked, delete blocked while claimed
        c, out = run(["guard", "--track", "console", "--op", "edit",
                      "--milestone", "M-C9"])
        expect("guard other milestone blocked", c == 1, "guard blocks foreign milestone")
        c, out = run(["guard", "--track", "console", "--op", "delete"])
        expect("guard delete blocked while claimed", c == 1, "guard blocks delete")
        c, out = run(["guard", "--track", "console", "--op", "edit",
                      "--milestone", "M-C2"])
        expect("guard own edit ok", c == 0, "guard allows own edit")

        # 8. release: mismatch refused, then proper release
        c, out = run(["release", "--track", "console", "--milestone", "M-C1"])
        expect("release mismatch refused", c == 2, "release mismatch refused")
        c, out = run(["release", "--track", "console", "--milestone", "M-C2"])
        expect("release ok", c == 0 and "RELEASED" in out, "release ok")
        c, out = run(["check", "--track", "console"])
        expect("check free after release", c == 0 and "FREE" in out,
               "check FREE after release")

        # 9. release with no claim -> error
        c, out = run(["release", "--track", "console", "--milestone", "M-C2"])
        expect("release empty refused", c == 1, "release without claim refused")

        # 10. pid-dead flag rendered
        locks = load_locks()
        locks["console"] = dict(locks.get("console") or {
            "milestone": "M-C1", "wt": fake["console"]["wt"],
            "branch": "x", "state": "active", "pid": 1,
            "host": "t", "started_at": now_iso()})
        locks["console"]["pid"] = 2147483000  # surely non-existent
        save_locks(locks)
        c, out = run(["status"])
        expect("stale pid flagged", "pid-dead" in out, "status flags dead pid")

        os.environ.pop("NL_WT_POOL_LOCK", None)
    print("selftest: %d/%d ALL GREEN" % (n[0], n[0]))
    sys.exit(0)


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="nl-engine worktree occupancy gate")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("claim", help="claim a track for a milestone (conversation ownership)")
    p.add_argument("--track", required=True)
    p.add_argument("--milestone", required=True, help="task ID = milestone ID, e.g. M-C1")
    p.add_argument("--note", default="")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_claim)

    p = sub.add_parser("release", help="release a claim")
    p.add_argument("--track", required=True)
    p.add_argument("--milestone", required=True)
    p.add_argument("--force", action="store_true")
    p.add_argument("--strict", action="store_true", help="block release if doc gate unsatisfied")
    p.set_defaults(fn=cmd_release)

    p = sub.add_parser("check", help="is a track touchable? exit 0=yes")
    p.add_argument("--track", required=True)
    p.add_argument("--milestone", default="")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("guard", help="hard gate before edit/merge/delete on a wt")
    p.add_argument("--track", required=True)
    p.add_argument("--op", required=True, choices=["edit", "merge", "delete"])
    p.add_argument("--milestone", default="")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_guard)

    sub.add_parser("status", help="registry x lock cross-check").set_defaults(fn=cmd_status)
    sub.add_parser("selftest", help="in-process self test").set_defaults(fn=cmd_selftest)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
