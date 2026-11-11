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
  - close   : milestone CLOSE gate (2026-11-09) - mechanical checks (tag /
              MILESTONES entry / example deliverable) then writes a checklist
              report; release --done refuses while cells are unfilled
  - status  : registry x lock x git worktree three-way cross-check
  - release : free the claim; --done = milestone closed, close report required

Lock file lives at the MAIN worktree root (shared across all worktrees via
`git rev-parse --git-common-dir`), so any session from any wt sees one truth.
All output is ASCII English (repo HUD convention).

Usage examples:
  python tools/wt_pool.py claim --track console --milestone M-C1 --note "session X"
  python tools/wt_pool.py scope  [--milestone M-C1]   # cwd boundary self-check (2026-11-10)
  python tools/wt_pool.py check --track material-graph --op delete
  python tools/wt_pool.py status
  python tools/wt_pool.py release --track console --milestone M-C1
  python tools/wt_pool.py selftest
"""

import argparse
import ctypes
import datetime
import glob
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
        "branch": "feat/job-system", "state": "active",
        "note": "M-J5a reopened 2026-11-09 by user instruction (reuse expansion); "
                "wt rebuilt from master 6a718fb per PARALLEL-TRACKS 7.1b",
    },
    "action-system": {
        "wt": "E:/UE_STUDY_2026/nl-engine-action-system",
        "branch": "feat/action-system", "state": "closed",
    },
    "console": {
        "wt": "E:/UE_STUDY_2026/nl-engine-console",
        "branch": "feat/console", "state": "closed",
    },
    "ecs": {
        "wt": "E:/UE_STUDY_2026/nl-engine-ecs",
        "branch": "feat/ecs", "state": "active",
    },
    "pathfinding": {
        "wt": "E:/UE_STUDY_2026/nl-engine-pathfinding",
        "branch": "feat/pathfinding", "state": "closed",
        "note": "M-N0~M-N6 merged to master 2026-11-11 (merge 5356c80 + "
                "acceptance d3d6127; tag nav-done, branch milestone/nav-done); "
                "wt removed 2026-11-11, branch kept per PARALLEL-TRACKS 7.6; "
                "state CLOSED by user instruction 2026-11-11",
    },
    "physics": {
        "wt": "E:/UE_STUDY_2026/nl-engine-physics",
        "branch": "feat/physics", "state": "active",
        "note": "M-P0 opened 2026-11-09 by user instruction (schedule gate lifted); "
                "M-P5 still gated on P8 E3",
    },
    "audio": {
        "wt": "E:/UE_STUDY_2026/nl-engine-audio",
        "branch": "feat/audio", "state": "planned",
    },
    "gameloop": {
        "wt": "E:/UE_STUDY_2026/nl-engine-gameloop",
        "branch": "feat/gameloop", "state": "active",
        "note": "M-GL0 opened 2026-11-10 by user instruction (plan "
                "docs/p8/P8-GAMELOOP-PLAN.md); M-GL2 merges with M-S3",
    },
    "pbr-direct": {
        "wt": "E:/UE_STUDY_2026/nl-engine-pbr-direct",
        "branch": "feat/pbr-direct", "state": "closed",
        "note": "merged to master e57173d (tag pbr-direct); wt removed 2026-11-09, branch kept",
    },
    "script": {
        "wt": "E:/UE_STUDY_2026/nl-engine-script",
        "branch": "feat/script", "state": "closed",
        "note": "merged to master 1b49b5f (tags m-sc0~m-sc3); wt removed 2026-11-09, branch kept",
    },
    "particles": {
        "wt": "E:/UE_STUDY_2026/nl-engine-particles",
        "branch": "feat/particles", "state": "active",
        "note": "B-class rendering track opened 2026-11-10 (M-PT0 starter: CPU emitter "
                "+ GPU instanced billboard; wt built at 50574e3, junctions "
                ".workbuddy + slang dist, skip-worktree set); E-prefix rejected "
                "to avoid ECS M-E0~E3 collision",
    },
    "camera": {
        "wt": "E:/UE_STUDY_2026/nl-engine-camera",
        "branch": "feat/camera", "state": "closed",
        "note": "A-class track (camera family, game-dev step 4; plan "
                "docs/p8/P8-CAMERA-PLAN.md, M-CAM0~CAM4); a段(M-CAM0/CAM1a/CAM3a) "
                "merged 7b0f24a (tags cam-m0/cam-1a/cam-3a) + b段(M-CAM1b/CAM2b/CAM3b + "
                "M-CAM4) merged 745ff83 (tag cam-b); wt removed 2026-11-11, branch kept; "
                "free/chase/track/smooth/shake 视觉卡待带显示会话补",
    },
    "input": {
        "wt": "E:/UE_STUDY_2026/nl-engine-input",
        "branch": "feat/input", "state": "active",
        "note": "A-class track opened 2026-11-10 by user instruction (city-driving game "
                "direction; plan docs/p8/P8-INPUT-PLAN.md; M-I0 core/input state layer "
                "first; viewer wiring deferred to merge after M8.1 TLAS closes)",
    },
    "drive": {
        "wt": "E:/UE_STUDY_2026/nl-engine-drive",
        "branch": "feat/drive", "state": "closed",
        "note": "A-class track opened 2026-11-10 by user instruction "
                "(city-driving game-dev steps 2+3 merged; plan "
                "docs/p8/P8-DRIVE-PLAN.md, M-D0~D3; M-D3 is the only "
                "milestone touching the viewer wiring seam); "
                "merged to master a2e3cff (tags drive-m0~drive-m3); "
                "wt removed 2026-11-10, branch kept",
    },
    "editor": {
        "wt": "E:/UE_STUDY_2026/nl-engine-editor",
        "branch": "feat/editor", "state": "closed",
        "note": "外置编辑器轨道（C++/ImGui 宿主 + 引擎侧 editor_bridge），独立于渲染主线；"
                "plan docs/editor/EDITOR-PLAN.md + RPC-SPEC.md；M-E0~E6 全量 merge 回 "
                "master ac50c10 + 集成修复 fc34176（tags e-m0e2-done~e-m6-done）；"
                "wt removed 2026-11-11, branch kept per PARALLEL-TRACKS 7.6; "
                "state CLOSED by user instruction 2026-11-11 17:1x",
    },
    "logging": {
        "wt": "E:/UE_STUDY_2026/nl-engine-logging",
        "branch": "feat/logging", "state": "closed",
        "note": "A-class track opened 2026-11-11 by user instruction (Q-L1; plan "
                "docs/p8/P8-LOG-PLAN.md); M-LOG0+M-LOG1+M-LOG2 closed (tags log-m0/log-m2) "
                "and merged back to master aaf43b8 with post-merge acceptance green; "
                "wt removed 2026-11-11, branch kept per PARALLEL-TRACKS 7.6; "
                "state CLOSED by user instruction 2026-11-11 22:0x",
    },
    "profiling": {
        "wt": "E:/UE_STUDY_2026/nl-engine-profiling",
        "branch": "feat/profiling", "state": "active",
        "note": "A-class track opened 2026-11-11 by user instruction (Q1~Q6 "
                "confirmed; plan docs/p8/P8-TRACY-PLAN.md): Tracy v0.15.0 "
                "(BSD-3-Clause) performance + memory profiling, RenderDoc "
                "in-app capture as M-TR2; M-TR0~M-TR3, tags tracy-m0~tracy-m3",
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

    # HARD GATE (2026-11-09): releasing a CLOSED milestone requires a passed
    # close gate with zero unfilled cells. Plain release (no --done) stays
    # available for mid-work ends-of-conversation.
    if args.done:
        rp = close_report_path(track, ms)
        if not os.path.exists(rp):
            fail("release --done refused: no close report for %s/%s. "
                 "Run first:\n  python tools/wt_pool.py close --track %s "
                 "--milestone %s --tag <tag> [--version <example-ver>]"
                 % (track, ms, track, ms), code=2)
        with open(rp, "r", encoding="utf-8") as f:
            rtext = f.read()
        # only table rows count: the header prose legitimately mentions 待填
        unfilled = [ln for ln in rtext.split("\n")
                    if ln.startswith("| ") and ("待填" in ln or "待执行" in ln)]
        if unfilled:
            fail("release --done refused: close report has %d unfilled cell(s):%s%s. "
                 "Fill the report first, paste it as the FIRST screen of the "
                 "closing reply, then release."
                 % (len(unfilled), os.linesep, os.linesep.join(unfilled)), code=2)

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


def close_report_path(track, ms):
    """Close report lives next to the lock file (main root, shared by all wts)."""
    return os.path.join(main_root(), ".wt_pool_close",
                        "%s-%s.md" % (track, ms))


def _git(args_git, cwd=None):
    return subprocess.run(["git"] + args_git, capture_output=True, text=True,
                          cwd=cwd)


def cmd_close(args):
    """Milestone CLOSE gate (2026-11-09): mechanical verification of the
    closing evidence + machine-generated checklist. The checklist file is the
    single source of truth: release --done refuses while it has unfilled
    cells. Replaces 'write the closing reply from memory'."""
    track, ms, tag = args.track, args.milestone, args.tag
    if track not in TRACKS:
        fail("track '%s' not in registry." % track)
    errors = []

    # -- 1. tag: exists, annotated, points at --commit (default: its own target)
    commit = args.commit
    r = _git(["tag", "-l", tag])
    if not r.stdout.strip():
        errors.append("tag '%s' does not exist in this tree (git tag -l)." % tag)
    else:
        rd = _git(["rev-parse", tag + "^{}"])
        deref = rd.stdout.strip()
        if rd.returncode != 0 or not deref:
            errors.append("tag '%s' is lightweight; annotated tag required "
                          "(git tag -a)." % tag)
        else:
            short = deref[:12]
            if commit:
                rc = _git(["rev-parse", commit])
                if rc.returncode != 0 or rc.stdout.strip()[:12] != short:
                    errors.append("tag '%s' points at %s, not --commit '%s'."
                                  % (tag, short, commit))
            else:
                commit = short

    # -- 2. per-commit modified-file list (machine-generated, never hand-typed)
    stat = ""
    if not errors and commit:
        rs = _git(["show", "--stat", "--format=short", commit])
        stat = rs.stdout
        if rs.returncode != 0 or not stat.strip():
            errors.append("git show --stat %s failed." % commit)

    # -- 3. MILESTONES.md four-column entry contains the tag
    msdoc = os.path.join(os.getcwd(), "docs", "MILESTONES.md")
    if os.path.exists(msdoc):
        with open(msdoc, "r", encoding="utf-8", errors="ignore") as f:
            mstext = f.read()
        if tag not in mstext:
            errors.append("tag '%s' not found in docs/MILESTONES.md; write the "
                          "four-column entry (动作/提交版本/完成情况/踩坑) first." % tag)
    else:
        errors.append("docs/MILESTONES.md not found under cwd (run close from "
                      "the tree that holds the milestone entry).")

    # -- 4. example deliverable: baselines + run.bat + showcase --check
    evidence = []
    if args.version:
        edir = os.path.join(os.getcwd(), "example", args.version)
        pngs = sorted(glob.glob(os.path.join(edir, "*.png")))
        if not os.path.isdir(edir):
            errors.append("example/%s/ does not exist." % args.version)
        elif not pngs or not os.path.exists(os.path.join(edir, "run.bat")):
            errors.append("example/%s/ missing run.bat or baseline PNGs."
                          % args.version)
        else:
            rck = subprocess.run(
                [sys.executable, os.path.join(os.getcwd(), "tools", "showcase",
                                              "server.py"), "--check"],
                capture_output=True, text=True)
            if rck.returncode != 0:
                errors.append("showcase --check FAILED (exit %d):"
                              % rck.returncode)
                errors.append(rck.stdout[-800:] or rck.stderr[-800:])
            evidence = [os.path.relpath(p, os.getcwd()).replace("\\", "/")
                        for p in pngs]

    if errors:
        for e in errors:
            print("CLOSE-GATE FAIL: %s" % e)
        fail("close gate NOT satisfied; NO report written. "
             "release --done will refuse. Fix the items above and re-run.",
             code=1)

    # -- report: checklist with mechanical facts pre-filled
    def row(ok, item, content):
        return "| %s | %s | %s |" % (item, "OK" if ok else "待填", content)

    lines = []
    lines.append("# close report: track='%s' milestone='%s' "
                 "(generated by wt_pool close, %s)"
                 % (track, ms, datetime.date.today().isoformat()))
    lines.append("")
    lines.append("> 本文件由 close 闸机械校验后生成（全过才有此文件）。收口流程："
                 "①补齐所有「待填」格；②本表原样作为收口回复的第一屏；"
                 "③证据图用 present_files 打开后再把该格改为「已打开」；"
                 "④`release --done` 会校验本文件无「待填/待执行」才放行。")
    lines.append("")
    lines.append("| 项 | 状态 | 内容 |")
    lines.append("|---|---|---|")
    lines.append(row(True, "提交版本",
                     "tag `%s` -> `%s`" % (tag, commit)))
    lines.append(row(True, "修改文件清单",
                     "见下方 `git show --stat` 原文（机器生成，禁止手抄省略）"))
    lines.append(row(False, "固化内容",
                     "待填：本轮写进了哪些 canonical 文档/台账/注册表，各补/改了什么"))
    if args.version:
        lines.append(row(True, "① 渲染交代物",
                         "example/%s/（%d 张基线 PNG + run.bat）+ "
                         "`server.py --check` exit 0" % (args.version, len(evidence))))
    else:
        lines.append(row(False, "① 渲染交代物",
                         "待填：纯研究轮次写「无渲染交代物」及原因"))
    lines.append(row(False, "② 工具交代物",
                     "待填：展示台 tool 卡 + TOOLS-GUIDE 归档；没有也要写「无工具交代物」"))
    lines.append(row(True, "③ MILESTONES 四栏台账",
                     "docs/MILESTONES.md 含 tag `%s`" % tag))
    lines.append(row(False, "④ 固化文档",
                     "待填：新约定/铁律写回的 canonical 文档清单"))
    if evidence:
        lines.append(row(False, "证据图 present_files",
                         "待执行：打开以下 %d 张 -> %s"
                         % (len(evidence), ", ".join(evidence[:4]))
                         + (" ..." if len(evidence) > 4 else "")))
    else:
        lines.append(row(False, "证据图 present_files",
                         "待执行：无基线 PNG；收口回复写「本轮无新证据图」后改此格"))
    lines.append("")
    lines.append("## git show --stat 原文（%s）" % commit)
    lines.append("")
    lines.append("```")
    lines.extend(stat.rstrip("\n").split("\n"))
    lines.append("```")
    lines.append("")

    rp = close_report_path(track, ms)
    os.makedirs(os.path.dirname(rp), exist_ok=True)
    with open(rp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("CLOSE-GATE PASS: report written -> %s" % rp)
    print("NEXT: fill the 待填 cells, paste the table as the FIRST screen of "
          "the closing reply, present evidence images, then "
          "`release --done --track %s --milestone %s`." % (track, ms))
    sys.exit(0)


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


def _norm(p):
    return os.path.normcase(os.path.normpath(p)) if p else None


def _inside(root, cwd):
    r, c = _norm(root), _norm(cwd)
    return bool(r and c and (c == r or c.startswith(r + os.sep)))


def cmd_scope(args):
    """cwd boundary self-check (2026-11-10): a claimed conversation must work
    inside ITS OWN wt only. Running in the main tree (or another track's wt)
    while claims are active is the cross-contamination class this gate kills.

    Exit codes: 0 = OK, 1 = blocked, 2 = warning (main tree with active claims).
    Convention: run `scope` right after `claim`, and again after any cd.
    """
    cwd = os.getcwd()
    # 1. inside a registered track wt?
    for track, info in sorted(TRACKS.items()):
        if _inside(info["wt"], cwd):
            locks = load_locks()
            cur = locks.get(track)
            if info["state"] == "closed":
                fail("scope: cwd is inside '%s' but that track is CLOSED "
                     "(merged to master, wt removed). Do not work here." % track)
            if cur is None:
                fail("scope: cwd is inside '%s' wt but the track has NO active "
                     "claim. `claim` first (conversation ownership, AGENTS.md)."
                     % track, code=1)
            if args.milestone and cur.get("milestone") != args.milestone:
                print(fmt_entry(track, cur))
                fail("scope: '%s' is claimed by a DIFFERENT milestone ('%s' vs "
                     "yours '%s'). This wt is not yours -- do not touch."
                     % (track, cur["milestone"], args.milestone), code=1)
            print("scope: OK cwd is inside '%s' wt, claimed by '%s'."
                  % (track, cur["milestone"]))
            return
    # 2. main tree?
    if _inside(main_root(), cwd):
        locks = load_locks()
        if locks:
            print("scope: WARN cwd is the MAIN tree while %d track(s) are "
                  "claimed:" % len(locks))
            for t in sorted(locks):
                print(fmt_entry(t, locks[t]))
            print("If this conversation holds one of those claims, go back to "
                  "your own wt -- main-tree edits from a claimed session are "
                  "the contamination class (2026-11-10). A mainline session "
                  "(no claim) may ignore this warning after verifying.")
            sys.exit(2)
        print("scope: OK cwd is the main tree, no active claims (mainline "
              "session).")
        return
    fail("scope: cwd is not inside any registered tree (main root or a track "
         "wt): %s" % cwd, code=1)


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

        # 11. scope: inside own claimed wt -> OK
        wt_dir = fake["console"]["wt"]
        os.makedirs(wt_dir, exist_ok=True)
        open(os.path.join(wt_dir, ".git"), "w").close()  # wt .git is a file
        old_cwd = os.getcwd()
        os.chdir(wt_dir)
        try:
            c, out = run(["scope"])
            expect("scope own wt ok", c == 0 and "scope: OK" in out,
                   "scope OK inside own claimed wt")
        finally:
            os.chdir(old_cwd)

        # 12. scope: inside wt with foreign milestone -> blocked
        os.chdir(wt_dir)
        try:
            c, out = run(["scope", "--milestone", "M-C9"])
            expect("scope foreign milestone blocked", c == 1,
                   "scope blocks foreign milestone in claimed wt")
        finally:
            os.chdir(old_cwd)

        # 13. scope: main tree with active claims -> warn exit 2
        os.chdir(tmp)  # tmp IS the main root via NL_WT_POOL_LOCK
        try:
            c, out = run(["scope"])
            expect("scope main tree warns", c == 2 and "WARN" in out,
                   "scope warns in main tree with active claims")
        finally:
            os.chdir(old_cwd)

        # 14. scope: main tree without claims -> OK
        locks = load_locks()
        del locks["console"]
        save_locks(locks)
        os.chdir(tmp)
        try:
            c, out = run(["scope"])
            expect("scope main tree clean ok", c == 0 and "mainline" in out,
                   "scope OK in main tree without claims")
        finally:
            os.chdir(old_cwd)

        # 15. scope: inside unclaimed wt -> blocked
        # (claim already removed by test 14)
        os.chdir(wt_dir)
        try:
            c, out = run(["scope"])
            expect("scope unclaimed wt blocked", c == 1 and "NO active claim" in out,
                   "scope blocks wt without claim")
        finally:
            os.chdir(old_cwd)

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
    p.add_argument("--done", action="store_true",
                   help="milestone is CLOSED: requires a passed close gate "
                        "(wt_pool close) with zero unfilled cells")
    p.set_defaults(fn=cmd_release)

    p = sub.add_parser("close", help="milestone CLOSE gate: mechanical checks "
                                     "+ machine-generated checklist; required "
                                     "before release --done")
    p.add_argument("--track", required=True)
    p.add_argument("--milestone", required=True)
    p.add_argument("--tag", required=True,
                   help="annotated tag of the closing commit (must exist, must "
                        "be mentioned in docs/MILESTONES.md)")
    p.add_argument("--commit", help="commit the tag must point at (default: "
                                    "the tag's own target)")
    p.add_argument("--version", help="example/<ver> deliverable dir: checks "
                                     "run.bat + baseline PNGs + server --check")
    p.set_defaults(fn=cmd_close)

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

    p = sub.add_parser("scope", help="cwd boundary self-check: claimed "
                                     "sessions must work inside their own wt "
                                     "(exit 0=ok, 1=blocked, 2=main-tree warn)")
    p.add_argument("--milestone", default="",
                   help="your milestone id; verifies the claim matches")
    p.set_defaults(fn=cmd_scope)

    sub.add_parser("status", help="registry x lock cross-check").set_defaults(fn=cmd_status)
    sub.add_parser("selftest", help="in-process self test").set_defaults(fn=cmd_selftest)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
