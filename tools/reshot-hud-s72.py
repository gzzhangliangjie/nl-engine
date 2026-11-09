#!/usr/bin/env python3
"""kStage S7.1->S7.2 bump 后的 HUD 基线重拍（2026-11-10，M7.2 收口）。

背景：M7.2 把 stage 字符串 bump 成 "S7.2 visibility buffer multi-material
indirect table"，m0.9 / pbr-direct 的 2 张 --hud 基线（exe=current）首行文本
随之过期。回归实测签名：527 px / maxDelta 240 = HUD 首行一行文本（与 M7.0
bump 的 512 px 同族）。

安全闸（沿 reshot-hud-s71.py 范式）：逐用例先跑出新图、与旧基线做像素 diff
——只有 diff 落在「HUD 一行文本」签名内（diffPixels <= 800 且 sameSize）才
覆盖基线；超签名的只报告不动，留人工判断（防止把真渲染回归刷进基线）。
"""
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE / "showcase"))

# Cross-tree gate (2026-11-10): this tool OVERWRITES example/ baselines —
# running the main tree's copy from a wt cwd (or vice versa) is exactly the
# contamination class wt_pool scope was built for. See tools/tree_gate.py.
sys.path.insert(0, str(HERE))
import tree_gate as _tree_gate  # noqa: E402
_tree_gate.gate(__file__)

import server as show  # noqa: E402

# 本轮 bump 实测的 2 个 HUD 失败用例（S7.1 文本基线，exe=current）
CASES = [
    ("m0.9", "gate-hud"),
    ("pbr-direct", "hud"),
]

# HUD 首行文本一行的像素签名（实测 527 px / maxDelta 240；留裕量防字体微差）
HUD_LINE_MAX_PIXELS = 800


def main() -> int:
    import json
    data = json.loads((ROOT / "example" / "cases.json").read_text(encoding="utf-8"))
    versions = {v["id"]: v for v in data["versions"]}

    copied, skipped, failed = 0, 0, 0
    for vid, cid in CASES:
        v = versions[vid]
        case = next(c for c in v["cases"] if c["id"] == cid)
        for backend in ("dx12", "vulkan"):
            r = show.run_one(v, case, backend, {})
            if not r.get("ok") and "退出码 0" not in (r.get("error") or ""):
                # run_one 只在退出码/expectLog 不符时 ok=False；渲染类退出码 0
                # 但像素 FAIL 时 ok 仍可能是 True（像素比对在 regression 层）。
                # 这里只关心：跑出来没有 PNG。
                print(f"[SKIP] {vid}/{cid} {backend}: 运行失败 {r.get('error')}")
                failed += 1
                continue
            out = r.get("_path")
            if not out or not Path(out).is_file():
                print(f"[SKIP] {vid}/{cid} {backend}: 无产物 PNG")
                failed += 1
                continue
            baseline = show.baseline_for(case, backend)
            if baseline is None or not baseline.is_file():
                print(f"[SKIP] {vid}/{cid} {backend}: 无旧基线可比对")
                failed += 1
                continue
            cmp = show.compare_pixels(Path(out), baseline)
            dp, md = cmp["diffPixels"], cmp["maxDelta"]
            if not cmp["sameSize"]:
                print(f"[SKIP] {vid}/{cid} {backend}: 尺寸不同（{cmp}），人工检查")
                skipped += 1
                continue
            if dp > HUD_LINE_MAX_PIXELS:
                print(f"[SKIP] {vid}/{cid} {backend}: diff {dp} px / maxDelta {md}"
                      f" 超出 HUD 行签名，人工检查")
                skipped += 1
                continue
            shutil.copyfile(out, baseline)
            copied += 1
            print(f"[OK]   {vid}/{cid} {backend}: diff {dp} px / maxDelta {md}"
                  f" -> 基线已更新 {baseline.name}")
    print(f"\n[residual] 更新 {copied} 张 | 跳过(人工) {skipped} | 运行失败 {failed}")
    return 0 if failed == 0 and skipped == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
