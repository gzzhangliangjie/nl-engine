# -*- coding: utf-8 -*-
# 重组 m-e12-probe.py：m-e9 已验证工具段（1..136）+ M-E12 断言段
import os

with open("tools/m-e9-probe.py", "r", encoding="utf-8") as f:
    e9 = f.readlines()
with open("tools/m-e12-assert.py", "r", encoding="utf-8") as f:
    tail = f.readlines()

head = e9[:136]   # 至 wait_announce 定义结束
out = head + ["\n", "# ===== M-E12: undo-redo 断言段（工具段 = M-E9 32/32 原版）=====\n"] + tail
tmp = "tools/m-e12-probe.py.tmp"
with open(tmp, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(out)
os.replace(tmp, "tools/m-e12-probe.py")
print("rewrote", len(out), "lines")
