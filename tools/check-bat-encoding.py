#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bat 编码事前检查（M-GL4 固化，2026-11-10 实测定稿：bat = GBK + CRLF，中文原生）。

实证背景（本机 cmd @ CP936 控制台，2026-11-10 多轮矩阵）：
  - UTF-8 存储的中文 bat：cmd 按代码页 936 解析多字节字节流，实测 rem 行被
    词中截断、截尾被当命令执行（gl4 `rem ...viewer.exe` 截成 `ewer.exe`、
    s3-fallback `；fallback` 实锤）；加 `chcp 65001` 不能稳定救（换页后解析
    偏移错位，多模式复现）。
  - GBK 存储：代码页原生匹配，同一批实炸句子 3/3 全绿、零解析风险——传统
    Windows 中文 bat 的本来形态。
  - 炸点①：LF-only 行尾（必须 CRLF）。
  - 炸点②：UTF-8 BOM（cmd 把 BOM 三字节当首行命令）。

规则（全部 bat，无例外）：
  1. CRLF 行尾；
  2. 纯 ASCII 文件放行（ASCII 是 GBK/UTF-8 公共子集）；
  3. 含非 ASCII：必须是 GBK 可解码，且**不得**是有效 UTF-8（防 UTF-8 中文
     伪装——utf-8 strict 可解码且含中文 = 存错了，转 GBK）。

用法：python tools/check-bat-encoding.py [目录...]     # 缺省扫 example/ scripts/
退出码：0 = 全绿；1 = 有违规（逐文件列原因）。regression.py preflight 每次调用。
"""
import sys
from pathlib import Path

MAX_SCAN_SIZE = 256 * 1024   # 防 huge 文件全量扫描（bat 都很小，纯保险）


def check_file(path: Path):
    """返回违规原因列表（空 = 通过）。"""
    data = path.read_bytes()
    problems = []

    if b"\r\n" not in data:
        problems.append("LF-only 行尾（规范：bat 统一 CRLF）")

    if data.startswith(b"\xef\xbb\xbf"):
        problems.append("UTF-8 BOM present（cmd 会把 BOM 三字节当首行命令）")
        return problems

    if all(b < 128 for b in data):
        return problems  # 纯 ASCII：放行

    is_utf8_chinese = False
    try:
        text = data.decode("utf-8")
        is_utf8_chinese = any(ord(c) > 127 for c in text)
    except UnicodeDecodeError:
        pass

    if is_utf8_chinese:
        problems.append("UTF-8 存储的中文 bat（cmd@CP936 解析多字节必切行，实锤）——请转 GBK 存储")
    else:
        try:
            data.decode("gbk")
        except UnicodeDecodeError as e:
            problems.append(f"不是有效 GBK（byte {e.start}）——中文 bat 必须以 GBK 存储")

    return problems


def main(argv):
    roots = [Path(a) for a in argv[1:]] or [Path("example"), Path("scripts")]
    bad = 0
    checked = 0
    for root in roots:
        if not root.is_dir():
            print(f"[skip] {root} 不存在")
            continue
        for path in sorted(root.rglob("*.bat")):
            if path.stat().st_size > MAX_SCAN_SIZE:
                print(f"[skip] {path} 超大，跳过")
                continue
            checked += 1
            problems = check_file(path)
            if problems:
                bad += 1
                print(f"[FAIL] {path}")
                for p in problems:
                    print(f"       - {p}")
    print(f"bat encoding check: {checked} files, {bad} violations")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
