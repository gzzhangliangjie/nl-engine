#!/usr/bin/env python3
"""tree_gate.py -- cross-tree execution gate (2026-11-10, wt_pool companion).

Problem (2026-11-10 user report): sessions that claimed a worktree slot
(`wt_pool.py claim`) kept drifting into the MAIN tree or OTHER worktrees --
running the main tree's regression.py, rewriting main-tree example/ baselines,
etc. This module makes the boundary mechanically checkable:

    Rule: a script must run in the SAME tree it lives in.
      script's repo root  ==  cwd's repo root   -> pass
      otherwise                             -> exit 2 (refuse)

Zero false positives by construction:
  - mainline session: main-tree script + main-tree cwd  -> pass
  - wt session:      wt's own script  + wt cwd          -> pass
  - anything else (wt script from main cwd, main script from wt cwd,
    script invoked with an unrelated cwd)              -> refuse

Escape hatch for deliberate, user-approved exceptions (CI sandbox with an
unrelated cwd, one-off cross-tree diagnosis):
    set NL_TREE_GATE_ALLOW=1   -> warn instead of refusing

NOTE: wt_pool.py itself is intentionally NOT gated -- it is the cross-tree
occupancy tool by design (the lock lives in the shared .git dir).
"""
import os
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


def gate(script_file):
    """Refuse (exit 2) when script tree != cwd tree. Call at script entry."""
    script_root = tree_root_of(os.path.dirname(os.path.abspath(script_file)))
    cwd_root = tree_root_of(os.getcwd())
    if script_root and cwd_root and _norm(script_root) == _norm(cwd_root):
        return
    where = cwd_root or "(cwd is not inside any git tree)"
    if os.environ.get("NL_TREE_GATE_ALLOW") == "1":
        print("WARN: tree gate bypassed via NL_TREE_GATE_ALLOW=1 "
              "(script tree %s, cwd %s)" % (script_root, where))
        return
    print("TREE-GATE FAIL: script tree and cwd tree disagree.")
    print("  script tree: %s" % script_root)
    print("  cwd tree:    %s" % where)
    print("Rule (2026-11-10): run the copy of this tool INSIDE your own tree "
          "(the wt you claimed, or the main tree for mainline work). "
          "Deliberate exception: NL_TREE_GATE_ALLOW=1.")
    sys.exit(2)
