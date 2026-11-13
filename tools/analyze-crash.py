#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
自动分析 nl-engine 崩溃 minidump —— 无需打开 VS。

定位崩溃引擎（cdb.exe 优先，其次 windbg.exe），自动配置符号路径
（dump 同级 build 目录的 *.pdb + Microsoft 公共符号服务器），执行
  .ecxr ; !analyze -v ; k 30 ; lm
把完整分析写入 <dump>.analysis.txt 并打印到终端。

用法:
  python tools/analyze-crash.py                 # 分析最新的 .dmp
  python tools/analyze-crash.py 2               # 分析第 2 新的 .dmp（序号见 --list，1 = 最新）
  python tools/analyze-crash.py path/to/x.dmp   # 分析指定 dump
  python tools/analyze-crash.py --list          # 列出所有 dump（带序号，新→旧）
  python tools/analyze-crash.py --json x.dmp    # 只打印元数据 .json（零工具）

依赖: 需本机存在 cdb.exe 或 windbg.exe。
  - 推荐: VS 安装器 -> 修改 -> 单个组件 -> 勾选 "Windows Debugging Tools"
    （会把 cdb.exe + 经典 windbg.exe 装到 C:/Program Files (x86)/Windows Kits/10/Debuggers/x64）
  - 或: 安装 Windows SDK 26100 并勾选 "Debugging Tools for Windows"
"""
import os
import sys
import glob
import json
import shutil
import subprocess
from datetime import datetime

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_ROOT = os.path.join(REPO, "build")
MS_SYMSRV = "srv*https://msdl.microsoft.com/download/symbols"
SDK_DEBUGGERS = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64"
# WinDbgX 商店包自带的 cdb 别名（包族目录名跨版本稳定，零安装）
WINDBGX_ALIAS_DIR = os.path.join(os.environ.get("LOCALAPPDATA", ""),
                                 "Microsoft", "WindowsApps",
                                 "Microsoft.WinDbg_8wekyb3d8bbwe")


def list_dumps():
    dumps = []
    for root, _dirs, files in os.walk(BUILD_ROOT):
        if os.path.basename(root) == "crashdumps":
            for f in files:
                if f.endswith(".dmp"):
                    dumps.append(os.path.join(root, f))
    dumps.sort(key=os.path.getmtime, reverse=True)
    return dumps


def find_dump(arg):
    if arg and arg != "--list":
        return arg
    dumps = list_dumps()
    return dumps[0] if dumps else None


def pdb_dirs(dump):
    dirs = set()
    d = os.path.dirname(dump)
    dirs.add(os.path.dirname(d))          # crashdumps -> build/<backend>
    for cand in ("DX12", "VULKAN"):
        p = os.path.join(BUILD_ROOT, cand)
        if os.path.isdir(p):
            dirs.add(p)
    for p in glob.glob(os.path.join(BUILD_ROOT, "*", "*.pdb")):
        dirs.add(os.path.dirname(p))
    return [d for d in dirs if os.path.isdir(d)]


def find_engine():
    """返回 (exe 路径, 类型)。类型: cdb(纯控制台) / windbg(经典 GUI) / windbgx(WinDbg Preview GUI)。
    优先纯控制台引擎（零窗口闪烁，适合无人值守/CI）：
      1. cdb.exe    经典 Debugging Tools（VS 组件，可选）
      2. cdbX64.exe WinDbgX 商店包自带（零安装，已实测可用）
      3. windbg.exe 经典 GUI / WinDbgX.exe Preview GUI（-c "...;q" 自动退出）"""
    # ① cdb 家族（控制台，同 CLI：-sins -y -z -c -logo）
    for p in (shutil.which("cdb.exe"),
              os.path.join(SDK_DEBUGGERS, "cdb.exe"),
              os.path.join(WINDBGX_ALIAS_DIR, "cdbX64.exe")):
        if p and os.path.isfile(p):
            return p, "cdb"
    # ② 经典 windbg GUI
    for p in (shutil.which("windbg.exe"),
              os.path.join(SDK_DEBUGGERS, "windbg.exe")):
        if p and os.path.isfile(p):
            return p, "windbg"
    # ③ WinDbg Preview GUI（支持 -z / -y / -c / -logo 无头模式）
    p = os.path.join(os.environ.get("LOCALAPPDATA", ""),
                     "Microsoft", "WindowsApps", "WinDbgX.exe")
    if os.path.isfile(p):
        return p, "windbgx"
    return None, None


def print_metadata(dump):
    js = dump[:-4] + ".json" if dump.endswith(".dmp") else dump + ".json"
    if not os.path.isfile(js):
        return
    try:
        meta = json.load(open(js, encoding="utf-8"))
    except Exception as e:
        print(f"[meta] 读取 {js} 失败: {e}")
        return
    print("==== 崩溃元数据 (nl crash .json) ====")
    for k in ("exe_name", "exception_code", "exception_code_name", "timestamp",
              "git_sha", "build_time", "exception_address", "thread_id",
              "command_line"):
        if k in meta:
            print(f"  {k:20}: {meta[k]}")
    print("=====================================")


def main():
    args = sys.argv[1:]
    show_list = "--list" in args
    show_json = "--json" in args
    positional = [a for a in args if not a.startswith("--")]
    dump_arg = positional[0] if positional else None

    if show_list:
        dumps = list_dumps()
        if not dumps:
            print("未找到任何 .dmp (build/**/crashdumps)")
            return
        for i, d in enumerate(dumps, 1):
            ts = datetime.fromtimestamp(os.path.getmtime(d)).strftime("%Y-%m-%d %H:%M:%S")
            print(f"[{i}] {ts}  {d}")
        return

    if dump_arg and dump_arg.isdigit():
        dumps = list_dumps()
        n = int(dump_arg)
        if not dumps or n < 1 or n > len(dumps):
            print(f"[错误] 序号 {n} 超出范围（当前共 {len(dumps)} 个 dump；"
                  f"先 --list 查看序号，1 = 最新）")
            sys.exit(1)
        dump = dumps[n - 1]
    else:
        dump = find_dump(dump_arg)
    if not dump or not os.path.isfile(dump):
        print(f"[错误] 未找到 dump: {dump}\n"
              f"       用法: python tools/analyze-crash.py [path/to/x.dmp | 序号 | --list | --json]")
        sys.exit(1)

    print(f"[dump] {dump}")
    print_metadata(dump)

    if show_json:
        return

    engine, ename = find_engine()
    if not engine:
        print("\n[错误] 未找到调试器引擎 (cdb.exe / windbg.exe / WinDbgX.exe)。\n"
              "       WinDbg Preview 商店包安装后本脚本即可自动找到 WinDbgX.exe；\n"
              "       或装经典 cdb: VS 安装器 -> 修改 -> 单个组件 -> 勾选 "
              "'Windows Debugging Tools'\n"
              f"       (cdb.exe 会装到 {SDK_DEBUGGERS})。")
        sys.exit(2)

    symdirs = pdb_dirs(dump)
    sym = ";".join(symdirs + [MS_SYMSRV])
    report = dump + ".analysis.txt"
    print(f"[engine] {engine} ({ename})")
    print(f"[symbol] {sym}")
    print(f"[report] {report}\n")

    if ename == "cdb":
        cmd = [engine, "-sins", "-y", sym, "-z", dump,
               "-c", ".ecxr;!analyze -v;k 30;lm;q", "-logo", report]
    else:  # windbg 经典 / windbgx(WinDbg Preview) 均支持 -z/-y/-c/-logo 无头模式
        cmd = [engine, "-y", sym, "-z", dump,
               "-c", ".ecxr;!analyze -v;k 30;q", "-logo", report]

    try:
        subprocess.run(cmd, check=False)
    except Exception as e:
        print(f"[错误] 运行调试器失败: {e}")
        sys.exit(3)

    if os.path.isfile(report):
        print("==== !analyze -v 输出 ====")
        txt = open(report, encoding="utf-8", errors="replace").read()
        print(txt)
        print(f"\n[完成] 完整报告已写入: {report}")
    else:
        print("[警告] 未生成报告文件；若用的是新版 WinDbg 且无头模式不支持，"
              "请直接双击 .dmp 用 WinDbg 打开。")


if __name__ == "__main__":
    main()
