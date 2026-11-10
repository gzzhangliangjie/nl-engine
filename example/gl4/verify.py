#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M-GL4 verify：宿主级脚本 OnTick 机器孪生（python 独立复算，防假绿）。

三重孪生：
  1. GameClock accumulator 复推：kStep=1/60 s（16.6667ms）、kMaxDelta=250ms、
     --fixed-dt 钉值喂入 → 每帧 tick 数与总 ticks（dt=16.7×100 → 100；
     dt=30×3 → [1,2,2] 共 5。注：25ms 落在 2×step=33.333ms 的浮点刀锋上，
     舍入方向不确定，弃用——用例必须离边界远）。
  2. 轨道公式孪生：脚本 OrbitScript 约定 r=1.6、ω=0.9 rad/s、t=ticks/60 →
     pos=(r·cos ωt, 0, r·sin ωt)；probe 读回逐位容差对拍（float 边界 5e-4）。
  3. 相等性锚：宿主 tickCount == GameClock ticks（probe 行 vs clock state 行）；
     暂停组 ticks=0 且位置停在场景初始态（脚本从未跑过）。

用法：python verify.py <tick-dx.log> <tick-vk.log> <tick-dx.png> <tick-vk.png>
         <mid-dx.log> <mid-vk.log> <pause-dx.log> <pause-vk.log> <neg.log>
退出码：0 = 全绿；1 = 断言失败清单非空。
"""
import math
import re
import sys

STEP_MS = 1000.0 / 60.0
MAX_DELTA_MS = 250.0
RADIUS = 1.6
OMEGA = 0.9

PROBE = re.compile(r"script probe: ticks=(\d+) pos=\((-?\d+\.?\d*),(-?\d+\.?\d*),(-?\d+\.?\d*)\)")
CLOCK = re.compile(r"script clock state: simTime=(\S+) ticks=(\d+) paused=(\d)")
LOADED = re.compile(r"script: loaded (\S+) type=(\S+)")


def clock_ticks_total(fixed_dt_ms, frames):
    """GameClock 复推：返回 (总 ticks, 每帧 tick 序列)。"""
    acc = 0.0
    per_frame = []
    for _ in range(frames):
        acc += min(fixed_dt_ms, MAX_DELTA_MS)
        n = int(acc // STEP_MS)
        acc -= n * STEP_MS
        per_frame.append(n)
    return sum(per_frame), per_frame


def orbit_pos(t_seconds):
    a = OMEGA * t_seconds
    return (RADIUS * math.cos(a), 0.0, RADIUS * math.sin(a))


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def parse(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    probes = [dict(ticks=int(m[0]), pos=(float(m[1]), float(m[2]), float(m[3])))
              for m in PROBE.findall(text)]
    clocks = [dict(ticks=int(m[1]), paused=int(m[2])) for m in CLOCK.findall(text)]
    loads = LOADED.findall(text)
    return text, probes, clocks, loads


def main(argv):
    if len(argv) != 10:
        print("usage: verify.py <tick-dx.log> <tick-vk.log> <tick-dx.png> <tick-vk.png> "
              "<mid-dx.log> <mid-vk.log> <pause-dx.log> <pause-vk.log> <neg.log>")
        return 2
    tick_dx, tick_vk, tick_dx_png, tick_vk_png, mid_dx, mid_vk, pause_dx, pause_vk, neg = argv[1:]
    fails = []

    # ---- tick 组：dt=16.7 × 100 帧 ----
    for path, tag in ((tick_dx, "DX12"), (tick_vk, "VULKAN")):
        text, probes, clocks, loads = parse(path)
        if not loads or "OrbitScript" not in loads[0][1]:
            fails.append(f"tick {tag}: script: loaded line missing/type wrong")
        total, per = clock_ticks_total(16.7, 100)
        if not probes:
            fails.append(f"tick {tag}: no script probe line")
        else:
            p = probes[-1]
            if p["ticks"] != total:
                fails.append(f"tick {tag}: probe ticks {p['ticks']} != twin {total}")
            expect = orbit_pos(total / 60.0)
            if not close(p["pos"], expect, 5e-4):
                fails.append(f"tick {tag}: pos {p['pos']} != orbit twin {expect}")
        if not clocks or clocks[-1]["ticks"] != total or clocks[-1]["paused"] != 0:
            fails.append(f"tick {tag}: clock state ticks/paused wrong (expect ticks={total} paused=0)")
        if probes and clocks and probes[-1]["ticks"] != clocks[-1]["ticks"]:
            fails.append(f"tick {tag}: host tickCount != GameClock ticks")
        if "shutdown ok" not in text:
            fails.append(f"tick {tag}: shutdown ok missing")

    # tick 组双后端 PNG 逐字节（确定性：--fixed-dt + 截图模式）。
    if open(tick_dx_png, "rb").read() != open(tick_vk_png, "rb").read():
        fails.append("tick: PNG DX12 != VULKAN (determinism broken)")

    # ---- mid 组：dt=30 × 3 帧 → 每帧 [1,2,2]，总 5（25ms 是 2×step 刀锋，弃用）----
    _, per_mid = clock_ticks_total(30.0, 3)
    if per_mid != [1, 2, 2]:
        fails.append(f"mid: twin per-frame ticks {per_mid} != [1,2,2] (twin self-check)")
    total_mid, _ = clock_ticks_total(30.0, 3)
    mtexts = {}
    for path, tag in ((mid_dx, "DX12"), (mid_vk, "VULKAN")):
        text, probes, clocks, _ = parse(path)
        mtexts[tag] = text
        if not probes:
            fails.append(f"mid {tag}: no script probe line")
        else:
            p = probes[-1]
            if p["ticks"] != total_mid:
                fails.append(f"mid {tag}: probe ticks {p['ticks']} != twin {total_mid}")
            expect = orbit_pos(total_mid / 60.0)
            if not close(p["pos"], expect, 5e-4):
                fails.append(f"mid {tag}: pos {p['pos']} != orbit twin {expect}")

    # mid 组跨后端逐位（probe + clock 行纯 CPU）。
    dx_probe = PROBE.findall(mtexts.get("DX12", ""))
    vk_probe = PROBE.findall(mtexts.get("VULKAN", ""))
    if dx_probe != vk_probe or not dx_probe:
        fails.append("mid: probe lines not byte-equal across backends")

    # ---- pause 组：先 game.pause 再跑 → ticks=0、位置停场景初始态 ----
    for path, tag in ((pause_dx, "DX12"), (pause_vk, "VULKAN")):
        text, probes, clocks, _ = parse(path)
        if not probes:
            fails.append(f"pause {tag}: no script probe line")
        else:
            p = probes[-1]
            if p["ticks"] != 0:
                fails.append(f"pause {tag}: probe ticks {p['ticks']} != 0")
            if not close(p["pos"], (RADIUS, 0.0, 0.0), 5e-4):
                fails.append(f"pause {tag}: pos {p['pos']} != scene initial (1.6,0,0)")
        if not clocks or clocks[-1]["paused"] != 1 or clocks[-1]["ticks"] != 0:
            fails.append(f"pause {tag}: clock not frozen (expect paused=1 ticks=0)")

    # ---- 负向：无 --script 时 game.script 必失败（viewer exit 1）----
    ntext = open(neg, encoding="utf-8", errors="replace").read()
    if "no script loaded" not in ntext:
        fails.append("neg: expected game.script failure message missing")
    if "acceptance failed" not in ntext:
        fails.append("neg: acceptance-failed exit path missing")

    if fails:
        print("VERIFY FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("VERIFY OK: clock twin (100/[1,2,2]) + orbit twin + pause freeze + "
          "cross-backend byte-equal + negative path — all green")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
