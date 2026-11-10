#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M-GL5 verify: tick-group registry contract re-checked independently (twin, anti-false-green).

The run.bat findstr anchors already pin the order lines; this script re-derives the
same contract from the logs a second time and cross-checks both backends:
  1. scene mode: order must be PrePhysics(on) -> Physics(off) -> PostPhysics(on)
     -> PostUpdateWork(on), and the startup line must report registered=3 with the
     active chain PrePhysics->PostPhysics->PostUpdateWork.
  2. phys mode (--physics-debug): order must be PrePhysics(off) -> Physics(on)
     -> PostPhysics(off) -> PostUpdateWork(off) (only the Physics group registered).
  3. cross-backend: the game.tickgroups lines of DX12 and Vulkan must be identical
     (registry view is deterministic).
  4. negative: "unknown command: game.tickgroup" present.

Usage:
  python verify.py <scene-dx.log> <scene-vk.log> <phys-dx.log> <phys-vk.log> <neg-dx.log> <neg-vk.log>
Exit: 0 = all green; 1 = assertion failures listed.
"""
import re
import sys

SCENE_ORDER = ("game.tickgroups: order PrePhysics(on) -> Physics(off) -> "
               "PostPhysics(on) -> PostUpdateWork(on)")
PHYS_ORDER = ("game.tickgroups: order PrePhysics(off) -> Physics(on) -> "
              "PostPhysics(off) -> PostUpdateWork(off)")
SCENE_STARTUP = "tick groups: PrePhysics->PostPhysics->PostUpdateWork (registered=3)"

def fails():
    out = []

    def check(cond, what):
        if not cond:
            out.append(what)

    scene_dx, scene_vk, phys_dx, phys_vk, neg_dx, neg_vk = (
        open(p, encoding="utf-8", errors="replace").read()
        for p in sys.argv[1:7])

    # 1. scene mode order + startup chain
    for tag, log in (("dx", scene_dx), ("vk", scene_vk)):
        check(SCENE_ORDER in log, f"scene/{tag}: expected order line missing")
        check(SCENE_STARTUP in log, f"scene/{tag}: startup registered=3 line missing")

    # 2. phys mode order (only Physics group)
    for tag, log in (("dx", phys_dx), ("vk", phys_vk)):
        check(PHYS_ORDER in log, f"phys/{tag}: expected order line missing")

    # 3. cross-backend determinism of the registry view
    def grab(log):
        m = re.search(r"game\.tickgroups: order .*", log)
        return m.group(0) if m else None
    check(grab(scene_dx) == grab(scene_vk) and grab(scene_dx) is not None,
          "scene: DX12/Vulkan tickgroups lines differ")
    check(grab(phys_dx) == grab(phys_vk) and grab(phys_dx) is not None,
          "phys: DX12/Vulkan tickgroups lines differ")

    # 4. negative
    for tag, log in (("dx", neg_dx), ("vk", neg_vk)):
        check("unknown command: game.tickgroup" in log, f"neg/{tag}: unknown-command line missing")

    return out

def main():
    bad = fails()
    for f in bad:
        print("FAIL", f)
    print(f"gl-m5 verify: {'FAIL' if bad else 'PASS'} (order/startup/cross-backend/negative)")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
