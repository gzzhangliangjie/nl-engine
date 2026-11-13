#!/usr/bin/env python3
"""wt_gate.py -- build-scope gate + wt locator (2026-11-13, tree_gate companion).

Problem (2026-11-13 incident): `scripts\\build-m0.2.bat` derives its build
root from the SCRIPT LOCATION (`%~dp0..`), not from cwd. A relative call
(`scripts\\build-m0.2.bat DX12`) from a main-tree cwd silently built the MAIN
tree while the session meant to build its claimed worktree. tree_gate.py
cannot catch this: there script-tree == cwd-tree (both main) and it passes.

Rule this gate enforces (build tools only):

  The tree the script LIVES IN must be a registered ACTIVE worktree
  (.wt_pool_busy.json at the main tree root, state == "active").
  Building the MAIN tree is refused by default -- mainline builds need the
  explicit, deliberate override:  set NL_BUILD_MAINREPO=1

Output anchor on pass (repo HUD convention, ASCII only):
  [nl:wt-gate] root=<tree> track=<track> branch=<branch> milestone=<mid>

Subcommands:
  check-build <script_path>   gate for .bat build scripts (call at bat entry)
  locate <track>              print the active wt path of <track> (stdout only)

Exit codes: 0 pass, 3 refused, 4 track not found / not active.
"""
import json
import os
import subprocess
import sys


def _norm(p):
    return os.path.normcase(os.path.normpath(p)) if p else None


def tree_root_of(start):
    """Walk up from `start` to the nearest .git (dir on main, file on wt)."""
    p = os.path.abspath(start)
    while True:
        if os.path.exists(os.path.join(p, ".git")):
            return p
        parent = os.path.dirname(p)
        if parent == p:
            return None
        p = parent


def main_root_of(root):
    """Main worktree root = parent dir of git-common-dir (shared .git)."""
    try:
        out = subprocess.run(
            ["git", "-C", root, "rev-parse", "--git-common-dir"],
            capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return None
    if not os.path.isabs(out):
        out = os.path.join(root, out)
    return os.path.normpath(os.path.dirname(os.path.abspath(out)))


def _busy(main_root):
    path = os.path.join(main_root, ".wt_pool_busy.json")
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _branch_of(root):
    try:
        return subprocess.run(
            ["git", "-C", root, "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return None


def cmd_check_build(script_path):
    root = tree_root_of(os.path.dirname(os.path.abspath(script_path)))
    if not root:
        print("WT-GATE FAIL: script is not inside any git tree: %s" % script_path)
        return 3
    main_root = main_root_of(root)
    if not main_root:
        print("WT-GATE FAIL: cannot resolve main tree for %s" % root)
        return 3
    if _norm(root) == _norm(main_root):
        if os.environ.get("NL_BUILD_MAINREPO") == "1":
            print("WARN: main-repo build allowed via NL_BUILD_MAINREPO=1 "
                  "(deliberate mainline override)")
            return 0
        print("WT-GATE FAIL: refusing to build the MAIN repo by default.")
        print("  script tree: %s" % root)
        print("  This is the #1 silent-wrong-tree failure mode (2026-11-13).")
        print("  Fix: run the copy inside your claimed worktree, e.g.")
        print("    scripts\\wt.bat <track> build DX12")
        print("  Mainline (master) builds are deliberate only:")
        print("    set NL_BUILD_MAINREPO=1")
        return 3
    busy = _busy(main_root)
    for track, entry in sorted(busy.items()):
        if entry.get("state") != "active":
            continue
        if _norm(entry.get("wt", "")) == _norm(root):
            branch = _branch_of(root)
            if branch and entry.get("branch") and branch != entry["branch"]:
                print("WT-GATE FAIL: branch mismatch in claimed wt.")
                print("  registry: %s   actual: %s" % (entry["branch"], branch))
                return 3
            print("[nl:wt-gate] root=%s track=%s branch=%s milestone=%s"
                  % (root, track, branch or "?", entry.get("milestone", "?")))
            return 0
    print("WT-GATE FAIL: tree is NOT a registered ACTIVE worktree.")
    print("  tree: %s" % root)
    print("  active tracks: %s" % (", ".join(sorted(
        t for t, e in busy.items() if e.get("state") == "active")) or "(none)"))
    return 3


def cmd_locate(track):
    here = tree_root_of(os.path.dirname(os.path.abspath(__file__)))
    main_root = main_root_of(here) or here
    busy = _busy(main_root)
    entry = busy.get(track)
    if not entry or entry.get("state") != "active":
        print("locate: track '%s' is not active. active: %s"
              % (track, ", ".join(sorted(
                  t for t, e in busy.items()
                  if e.get("state") == "active")) or "(none)"),
              file=sys.stderr)
        return 4
    print(entry["wt"])
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 4
    if sys.argv[1] == "check-build":
        if len(sys.argv) != 3:
            print("usage: wt_gate.py check-build <script_path>", file=sys.stderr)
            return 4
        return cmd_check_build(sys.argv[2])
    if sys.argv[1] == "locate":
        if len(sys.argv) != 3:
            print("usage: wt_gate.py locate <track>", file=sys.stderr)
            return 4
        return cmd_locate(sys.argv[2])
    print("unknown subcommand: %s" % sys.argv[1], file=sys.stderr)
    return 4


if __name__ == "__main__":
    sys.exit(main())
