#!/usr/bin/env python3
"""kStage bump 后的 HUD 基线通用重拍（2026-11-10，M7.2 收口沉淀）。

与 reshot-hud-s71.py / reshot-hud-s72.py 的区别：不再手抄用例清单——自动
枚举 cases.json 里 args 含 `--hud`、exe=current（非 showcase tag exe）且带
基线的全部用例。kStage bump 每次都撞同一批（HUD 首行打印 stage 字符串），
清单抄写是纯重复劳动且漏抄就是回归假失败。

安全闸（沿 s71/s72 范式，不变）：逐用例先跑出新图、与旧基线做像素 diff
——只有 diff 落在「HUD 一行文本」签名内（diffPixels <= 800 且 sameSize）
才覆盖基线；超签名的只报告不动，留人工判断（防止把真渲染回归刷进基线）。
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

# HUD 首行文本一行的像素签名（实测 527 px / maxDelta 240；留裕量防字体微差）
HUD_LINE_MAX_PIXELS = 800

BACKENDS = ("dx12", "vulkan")


def select_cases() -> list[tuple[dict, dict]]:
    """args 含 --hud 且 exe=current（双后端都非 showcase tag exe）且有基线。"""
    import json
    data = json.loads((ROOT / "example" / "cases.json").read_text(encoding="utf-8"))
    out = []
    for v in data["versions"]:
        exes = v.get("exe", {})
        exe_current = all("showcase" not in (exes.get(b) or "") for b in BACKENDS)
        if not exe_current:
            continue
        for c in v.get("cases", []):
            if not any(a == "--hud" or a.startswith("--hud") for a in c.get("args", [])):
                continue
            if not all((c.get("baselines") or {}).get(b) for b in BACKENDS):
                continue
            out.append((v, c))
    return out


def main() -> int:
    # --force（2026-11-10 M8.0 沉淀）：人工追因核对通过后放行超签名重拍的合规
    # 通道——先抽 1~2 张代表用例（非 dag 887px 档 + dag/hiz 位移档）逐像素确认
    # diff 全部落在 HUD 面板区域内、模型区零差异，才允许带此参数批量重拍。
    # 不带参数时行为不变（超签名只报告不动），防呆语义不弱化。
    force = "--force" in sys.argv
    cases = select_cases()
    print(f"[enumerate] --hud 且 exe=current 且有基线：{len(cases)} 用例"
          f"{'（--force 人工确认已过）' if force else ''}")

    copied, skipped, failed = 0, 0, 0
    for v, case in cases:
        vid, cid = v["id"], case["id"]
        for backend in BACKENDS:
            r = show.run_one(v, case, backend, {})
            if not r.get("ok") and "退出码 0" not in (r.get("error") or ""):
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
                if force:
                    print(f"[FORCE] {vid}/{cid} {backend}: diff {dp} px / maxDelta {md}"
                          f" 超签名但人工确认通过 -> 覆盖 {baseline.name}")
                else:
                    print(f"[SKIP] {vid}/{cid} {backend}: diff {dp} px / maxDelta {md}"
                          f" 超出 HUD 行签名，人工检查")
                    skipped += 1
                    continue
            if dp == 0:
                print(f"[SAME] {vid}/{cid} {backend}: diff 0，基线无需更新")
                continue
            shutil.copyfile(out, baseline)
            copied += 1
            print(f"[OK]   {vid}/{cid} {backend}: diff {dp} px / maxDelta {md}"
                  f" -> 基线已更新 {baseline.name}")
    print(f"\n[residual] 更新 {copied} 张 | 相同 {len(cases) * 2 - copied - skipped - failed}"
          f" | 跳过(人工) {skipped} | 运行失败 {failed}")
    return 0 if failed == 0 and skipped == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
