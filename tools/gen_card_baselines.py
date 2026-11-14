#!/usr/bin/env python3
"""M-WEA2: 按卡重生成基线（逐 case × 后端跑 viewer --screenshot 基线路径）。

用法: python tools/gen_card_baselines.py <version>
读 example/cases.json 该版本的 cases，逐条以 args 跑 exe（{out} = baseline 路径），
直接覆写 example/ 基线 PNG。重拍后必须接 regression --only <version> bytes 复验。
"""
import json
import io
import os
import subprocess
import sys

EXE = {
    "dx12": "build/DX12/viewer.exe",
    "vulkan": "build/VULKAN/viewer.exe",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gen_card_baselines.py <version>")
        return 2
    version = sys.argv[1]
    data = json.load(io.open("example/cases.json", encoding="utf-8"))
    ver = next((v for v in data["versions"] if v["id"] == version), None)
    if ver is None:
        print(f"version not found: {version}")
        return 2
    failed = 0
    for case in ver["cases"]:
        for backend, exe in EXE.items():
            out = case["baselines"][backend]
            os.makedirs(os.path.dirname(out), exist_ok=True)
            args = [a.replace("{out}", out) for a in case["args"]]
            cmd = [exe] + args
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
                ok = r.returncode == 0 and os.path.isfile(out)
            except subprocess.TimeoutExpired:
                ok = False
                r = None
            status = "OK" if ok else "FAIL"
            print(f"[{status}] {case['id']} {backend} -> {out}")
            if not ok and r is not None:
                print("  stderr:", (r.stderr or r.stdout or "")[-300:])
            if not ok:
                failed += 1
    print(f"done: {len(ver['cases']) * 2 - failed}/{len(ver['cases']) * 2} OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
