#!/usr/bin/env python3
"""nl-engine 打包脚本（发版用）。

用法（当前只写不跑，跑包需用户明确指令）：
    python tools/package.py                       # 默认：bin + shaders + assets + licenses -> out/package/<name>/
    python tools/package.py --zip                 # 额外产出 zip
    python tools/package.py --with-example        # 附带 example/ 截图基线（约 128M）
    python tools/package.py --no-assets           # 不带 assets（包从 516M 降到几 MB）
    python tools/package.py --backend dx12        # 只带一个后端（默认 both）

包结构：
    <name>/
      bin/DX12/*.exe            # build/DX12 编译产物
      bin/VULKAN/*.exe          # build/VULKAN 编译产物
      shaders/DX12/*.dxil       # build/dx12/shaders
      shaders/VULKAN/*.spv      # build/vulkan/shaders
      assets/                   # 音频/模型/场景等（--no-assets 可关）
      example/                  # 仅 --with-example
      THIRD-PARTY-NOTICES.txt   # 汇总 thirdparty/*/LICENSE*（发版硬要求）
      MANIFEST.json             # 版本/commit/时间/exe sha256 清单
      README-PACKAGE.txt        # 包说明（B1 可重定位布局说明）

已知架构事实（B1 拍板 2026-11-15 落地后）：
  nl/core/paths.cpp 双层解析 —— exe 相对目录优先（本包布局：
  bin/<BE>/exe 两级向上 -> shaders/<BE>、assets/{models,meshlets}），
  探测不到才回退编译期绝对路径。包内 exe 可在无源码树的机器直接运行；
  开发树（build/<BE>/ 布局对不上包内布局）自动走兜底，行为不变。
  bin 里必须带运行时 DLL（nethost.dll 等，M-SC0 约定紧挨 exe）。

仅用标准库（本机 pip 不可靠），Windows 下用纯 pathlib，无 shell 调用。
"""

import argparse
import datetime
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
THIRDPARTY_DIR = REPO_ROOT / "thirdparty"
OUT_ROOT = REPO_ROOT / "out" / "package"

BACKENDS = {
    "dx12":   {"bin": BUILD_DIR / "DX12",   "shaders": BUILD_DIR / "dx12" / "shaders"},
    "vulkan": {"bin": BUILD_DIR / "VULKAN", "shaders": BUILD_DIR / "vulkan" / "shaders"},
}


def git_describe() -> str:
    """取最近 tag 或 commit 短 hash 做包名；git 不可用则退化为日期。"""
    import subprocess
    for args in (["git", "describe", "--tags", "--abbrev=8"],
                 ["git", "rev-parse", "--short", "HEAD"]):
        try:
            r = subprocess.run(args, cwd=REPO_ROOT, capture_output=True,
                               text=True, timeout=15)
            if r.returncode == 0 and r.stdout.strip():
                return r.stdout.strip()
        except Exception:
            pass
    return datetime.date.today().isoformat()


def git_commit() -> str:
    import subprocess
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT,
                           capture_output=True, text=True, timeout=15)
        if r.returncode == 0:
            return r.stdout.strip()
    except Exception:
        pass
    return "unknown"


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_tree(src: Path, dst: Path) -> int:
    """目录拷贝，返回文件数。src 不存在时报错由调用方决定。"""
    n = 0
    for p in sorted(src.rglob("*")):
        if p.is_file():
            rel = p.relative_to(src)
            target = dst / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, target)
            n += 1
    return n


def collect_thirdparty_notices(dst: Path) -> list:
    """汇总 thirdparty/<lib>/LICENSE* 到 THIRD-PARTY-NOTICES.txt。
    缺 LICENSE 的库记入 missing，由调用方决定是否放行。"""
    lines = ["nl-engine THIRD-PARTY NOTICES", "=" * 60, ""]
    missing = []
    libs = sorted(d for d in THIRDPARTY_DIR.iterdir()
                  if d.is_dir() and not d.name.startswith("."))
    for lib in libs:
        lines.append("-" * 60)
        lines.append(f"Library: {lib.name}")
        vendor_md = lib / "VENDOR.md"
        if vendor_md.is_file():
            lines.append(f"VENDOR.md: thirdparty/{lib.name}/VENDOR.md (见仓库)")
        lic_files = [p for p in sorted(lib.iterdir())
                     if p.is_file() and p.name.upper().startswith("LICENSE")]
        if not lic_files:
            missing.append(lib.name)
            lines.append("!! LICENSE file NOT FOUND in package source")
        else:
            for lf in lic_files:
                lines.append(f"--- {lib.name}/{lf.name} ---")
                lines.append(lf.read_text(encoding="utf-8", errors="replace"))
        lines.append("")
    out = dst / "THIRD-PARTY-NOTICES.txt"
    out.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return missing


def build_manifest(pkg_dir: Path, version: str, backends: list) -> dict:
    manifest = {
        "name": "nl-engine",
        "version": version,
        "git_commit": git_commit(),
        "packaged_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "backends": backends,
        "bin": {},       # backend -> {exe名: sha256}
        "shaders": {},   # backend -> 文件数
        "notes": [
            "路径双层解析（B1 拍板 2026-11-15）：包内 exe 相对目录优先",
            "（bin/<后端>/exe -> ../shaders/<后端>、../assets/...），",
            "本包可在无源码树的机器直接运行；开发树才走编译期绝对路径兜底。",
        ],
    }
    for be in backends:
        bin_dir = pkg_dir / "bin" / be.upper()
        manifest["bin"][be] = {
            p.name: sha256_of(p) for p in sorted(bin_dir.glob("*.exe"))
        }
        manifest["shaders"][be] = len(list((pkg_dir / "shaders" / be.upper()).glob("*")))
    (pkg_dir / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=1, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n")
    return manifest


def write_readme(pkg_dir: Path, backends: list, with_assets: bool,
                 with_example: bool, missing_licenses: list) -> None:
    lines = [
        "nl-engine package",
        "=================",
        "",
        "bin/DX12/viewer.exe    -- DX12 后端 viewer（无参 = 开窗渲染不退出）",
        "bin/VULKAN/viewer.exe  -- Vulkan 后端 viewer",
        "bin/*/ *-tools.exe     -- 各里程碑工具（usage 见仓库 docs/）",
        "bin/*/*.dll            -- 运行时 DLL（nethost.dll 等必须紧挨 exe，M-SC0 约定）",
        "shaders/<BE>/          -- 编译期字节码（dxil / spv）",
        "",
        "路径解析（B1 拍板 2026-11-15）：exe 相对目录优先 ——",
        "  bin/<BE>/xxx.exe  ->  ../../shaders/<BE>/   ../../assets/models|meshlets",
        "本包可在无源码树的机器直接运行；开发树才走编译期绝对路径兜底。",
        "",
        f"assets:     {'包含' if with_assets else '未包含（--no-assets）'}",
        f"example:    {'包含' if with_example else '未包含（默认关）'}",
        f"backends:   {', '.join(backends)}",
    ]
    if missing_licenses:
        lines.append("")
        lines.append("!! 以下 thirdparty 库缺 LICENSE，发版前必须补齐：")
        lines.extend(f"  - {n}" for n in missing_licenses)
    (pkg_dir / "README-PACKAGE.txt").write_text(
        "\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser(description="nl-engine 打包脚本（快照归档）")
    ap.add_argument("--version", default=None, help="包名后缀，默认 git describe")
    ap.add_argument("--out", default=None, help="输出根目录，默认 out/package/")
    ap.add_argument("--backend", choices=["both", "dx12", "vulkan"],
                    default="both", help="带哪些后端（默认 both）")
    ap.add_argument("--no-assets", action="store_true", help="不拷 assets/")
    ap.add_argument("--with-example", action="store_true",
                    help="附带 example/ 截图基线（约 128M）")
    ap.add_argument("--zip", action="store_true", help="额外产出 zip")
    args = ap.parse_args()

    version = args.version or git_describe()
    out_root = Path(args.out) if args.out else OUT_ROOT
    pkg_dir = out_root / f"nl-engine-{version}"
    if pkg_dir.exists():
        print(f"[package] 目标已存在，先清理：{pkg_dir}")
        shutil.rmtree(pkg_dir)

    backends = list(BACKENDS) if args.backend == "both" else [args.backend]

    # 前置检查：bin 与 shaders 必须齐，缺了就不该出包
    errors = []
    for be in backends:
        if not (BACKENDS[be]["bin"] / "viewer.exe").is_file():
            errors.append(f"缺 {BACKENDS[be]['bin'] / 'viewer.exe'}（未构建？）")
        if not BACKENDS[be]["shaders"].is_dir():
            errors.append(f"缺 shader 目录 {BACKENDS[be]['shaders']}")
    if errors:
        for e in errors:
            print(f"[package] ERROR: {e}", file=sys.stderr)
        return 2

    print(f"[package] version = {version}")
    print(f"[package] 输出   = {pkg_dir}")

    # 1. bin（exe + 运行时 DLL；.pdb 不进包）
    for be in backends:
        dst = pkg_dir / "bin" / be.upper()
        dst.mkdir(parents=True, exist_ok=True)
        n = 0
        for p in sorted(BACKENDS[be]["bin"].iterdir()):
            # clang_rt.asan*: 编译器运行时遗留（实测 viewer/nav-tools/imgui dll 均不依赖），
            # 带进包既增重又引入 LLVM 分发义务，过滤。
            if p.name.startswith("clang_rt.asan"):
                continue
            if p.is_file() and p.suffix.lower() in (".exe", ".dll"):
                shutil.copy2(p, dst / p.name)
                n += 1
        print(f"[package] bin/{be.upper()}: {n} 个 exe/dll（.pdb 已跳过）")

    # 2. shaders（按后端分目录）
    for be in backends:
        n = copy_tree(BACKENDS[be]["shaders"], pkg_dir / "shaders" / be.upper())
        print(f"[package] shaders/{be.upper()}: {n} 个文件")

    # 3. assets（默认带，--no-assets 关）
    if not args.no_assets:
        n = copy_tree(REPO_ROOT / "assets", pkg_dir / "assets")
        print(f"[package] assets: {n} 个文件")
    else:
        print("[package] assets: 跳过（--no-assets）")

    # 4. example（默认不带）
    if args.with_example:
        n = copy_tree(REPO_ROOT / "example", pkg_dir / "example")
        print(f"[package] example: {n} 个文件")
    else:
        print("[package] example: 跳过（默认关，--with-example 开）")

    # 5. 第三方许可汇总（发版硬要求；缺 LICENSE 只记录不拦截，README 显著提示）
    missing = collect_thirdparty_notices(pkg_dir)
    if missing:
        print(f"[package] 警告：{len(missing)} 个 thirdparty 库缺 LICENSE："
              + ", ".join(missing))
    else:
        print("[package] THIRD-PARTY-NOTICES.txt: 全库 LICENSE 齐")

    # 6. 根 LICENSE（仓库自述 MIT）
    root_lic = REPO_ROOT / "LICENSE"
    if root_lic.is_file():
        shutil.copy2(root_lic, pkg_dir / "LICENSE")

    # 7. manifest + readme
    manifest = build_manifest(pkg_dir, version, backends)
    write_readme(pkg_dir, backends, not args.no_assets,
                 args.with_example, missing)
    print(f"[package] MANIFEST.json: {len(manifest['bin'])} 后端 "
          f"{sum(len(v) for v in manifest['bin'].values())} exe 入账")

    # 8. 可选 zip
    if args.zip:
        zip_path = out_root / f"{pkg_dir.name}.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for p in sorted(pkg_dir.rglob("*")):
                if p.is_file():
                    zf.write(p, p.relative_to(out_root))
        print(f"[package] zip: {zip_path} ({zip_path.stat().st_size >> 20} MB)")

    print("[package] 完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
