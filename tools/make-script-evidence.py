# -*- coding: utf-8 -*-
"""P8-Script 工具版证据图生成器（M-SC0 收口段 + M-SC1 console 绑定段 + M-SC2 ECS 数据面段
+ M-SC3 脚本热更段）。

铁律：所有内容从工具**真实输出**渲染（example/<dir>/*.log，
由 nl-script.exe 实跑重定向落盘），不许手绘示意。产物进 example/<dir>/。
复现：example/<dir>/run.bat（重跑用例 -> 重新生成本图 -> exit 0 校验）。
用法：python make-script-evidence.py [m-sc0|m-sc1|m-sc2|m-sc3]   （缺省 m-sc0）
"""
import os
import sys

DIR_NAME = sys.argv[1] if len(sys.argv) > 1 else "m-sc0"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, "example", DIR_NAME)
OUT = LOGS

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_log(name):
    with open(os.path.join(LOGS, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def color_for(s):
    t = s.strip()
    if "PASS" in t:
        return GREEN
    if "FAIL" in t:
        return RED
    if "run: ret=" in t or "check: PASS" in t or "run: FAIL" in t:
        return GREEN if "run: ret=" in t or "check: PASS" in t else AMBER
    if "exception" in t.lower() or "0xe0434352" in t:
        return AMBER
    if t.startswith("nl-script") or t.startswith("usage:") or t.startswith("$"):
        return CYAN
    if t.startswith("  ") and t.strip():
        return DIM
    if t.endswith(":") and not t.startswith(" "):
        return DIM
    return FG


def wrap_line(l, limit=118):
    # 超长行按字符折行（terminal 风格续行，前缀缩进对齐）
    if len(l) <= limit:
        return [l]
    out = []
    while len(l) > limit:
        out.append(l[:limit])
        l = "    " + l[limit:]  # 续行缩进
    out.append(l)
    return out


def render(log_name, png_name, title, note=None):
    text = read_log(log_name)
    lines = []
    for l in text.splitlines():
        lines.extend(wrap_line(l.rstrip()))
    if note:
        lines.append("")
        lines.extend(wrap_line(note))
    f = font(17)
    ft = font(22, bold=True)
    lh = 28
    pad = 28
    # 最大行宽决定画布（上限 1180）
    tmp = Image.new("RGB", (4, 4))
    td = ImageDraw.Draw(tmp)
    w = min(1180, max(
        [td.textlength(l, font=f) for l in lines] +
        [td.textlength(title, font=ft)]  # 标题参与宽度计算，防截断
    ) + pad * 2 + 40)
    h = pad * 2 + lh * (len(lines) + 2)
    img = Image.new("RGB", (int(w), h), BG)
    d = ImageDraw.Draw(img)
    y = pad
    d.text((pad, y), title, font=ft, fill=CYAN)
    y += lh * 1.5
    for l in lines:
        d.text((pad, y), l, font=f, fill=color_for(l))
        y += lh
    img.save(os.path.join(OUT, png_name))
    print("wrote %s (%dx%d)" % (png_name, img.width, img.height))


CASES_M_SC0 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC0 acceptance (hosting kernel + hot rebuild)",
     "s1-s2 hosting (hostfxr 3-export + default-signature entry) / s3 return anchors / "
     "s4 managed exception at SEH boundary / s5 rebuild -> behavior update 42->43 in-process."),
    ("run-ok.log", "run-ok.png",
     "nl-script run --args ok -- positive (exit 0, ret=42)",
     "dotnet build -> hostfxr init -> load HelloScript.dll -> call Entry(\"ok\") -> ret 42."),
    ("run-throw.log", "run-throw.png",
     "nl-script run --args throw -- negative (exit 1, managed exception captured)",
     "Managed InvalidOperationException surfaces as SEH 0xe0434352 at the interop boundary; "
     "captured -> Result -> exit 1. Process never crashes."),
    ("check.log", "check.png",
     "nl-script check -- compile only (exit 0)",
     "dotnet build only; no runtime init. FDD contract: runtimeconfig.json generated "
     "via GenerateRuntimeConfigurationFiles."),
    ("usage.log", "usage.png",
     "nl-script (no args) -- usage error (exit 2)",
     "Exit-code convention: 0=PASS / 1=fail / 2=usage (showcase expectExit aligned)."),
]

CASES_M_SC1 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC1 console binding (s6a-s6f, on top of M-SC0 s0-s5)",
     "s6a C# cvar.set fires C++ onChanged hook / s6b C#-registered command driven by console exec / "
     "s6c ExecCode 0/1/2/3 aligned across the binding (mask 31) / "
     "s6d managed exception in command -> SEH at boundary -> CommandFailed, console alive / "
     "s6e C++-set float read back from C# + cvar.list full view / s6f unregister -> UnknownCommand."),
    ("run-api.log", "run-api.png",
     "nl-script run --api -- demo (exit 0)",
     "Entry receives ApiTable* (6 native fn pointers): C# registers script.echo, sets script.count, "
     "execs it back, drives builtin cvar.set via exec, reads the value from C#, dumps cvar.list."),
    ("run-badtype.log", "run-badtype.png",
     "nl-script run --api without --type -- negative (exit 1, clean Result error)",
     "Default type name does not exist in ConsoleScript assembly -> getEntryPoint returns "
     "Result{code=5} surfaced as 'run: FAIL getEntryPoint'; no crash, exit 1."),
    ("check.log", "check.png",
     "nl-script check console example -- compile only (exit 0)",
     "dotnet build only; binding entry points require [UnmanagedCallersOnly] default signature."),
]

CASES_M_SC2 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC2 ECS binding (s7a-s7g, on top of M-SC0 s0-s5 + M-SC1 s6)",
     "s7a C# spawn+Transform+fields -> C++ reads back exact / s7b C++ sets anchors via table, "
     "C# verifies / s7c invalid entity ops all rejected NoEntity, no crash / "
     "s7d deferred removal + flush asymmetry (Carrot) + table TransformGet safe reject / "
     "s7e parent roundtrip via table + getChildIDs view / s7f JSON roundtrip bytes equal / "
     "s7g unbound World -> all exports NoWorld (M-SC3 hot-reload window fallback)."),
    ("run-api.log", "run-api.png",
     "nl-script run --api on ecs example -- demo (exit 0)",
     "Entry receives ApiTable* (v2, 16 fn pointers): spawn root+leaf, setParent/getParent, "
     "addTransform, transformSet/Get, flush, remove root (children cascade), flush again."),
    ("run-badtype.log", "run-badtype.png",
     "nl-script run --api with wrong --type -- negative (exit 1, clean Result error)",
     "HelloScript type does not exist in EcsScript assembly -> getEntryPoint returns "
     "Result{code=5}; no crash, exit 1."),
    ("check.log", "check.png",
     "nl-script check ecs example -- compile only (exit 0)",
     "dotnet build only; binding entry points require [UnmanagedCallersOnly] default signature."),
]

CASES_M_SC3 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC3 hot reload (s8a-s8h, on top of s0-s5 + s6 + s7)",
     "s8a ScriptHost::init (bootstrap 3 fn pointers) + console.reload-script registered / "
     "s8b first load via collectible ALC (LoadFromStream, file not locked) + Entry rebinding / "
     "s8c edit script source -> dotnet build overwrites same DLL name -> ScriptHost::reloadScript / "
     "s8d behavior updated (echo prefix v1 -> [v2]) / s8e C++ state intact "
     "(cvar values, entity tree, builtin commands, command ledger re-registered) / "
     "s8f consecutive reload x3, handle drift 0 / s8g unloadScript -> weakref dead "
     "(ALC fully collected) / s8h script commands gone, C++ exports still live."),
    ("reload.log", "reload.png",
     "s8 hot-reload section close-up (filtered from selftest.log)",
     "reload-script lifecycle: unbind old ALC thunks (scriptUnregisterAll) -> Unload "
     "(weakref dead after GC drain) -> reload same DLL -> Entry re-registers commands. "
     "Key pitfall fixed: unload and GC drain must live in separate (NoInlining) methods -- "
     "same-frame 'alc = null' is a JIT dead store, stack keeps rooting the ALC forever."),
    ("run-api.log", "run-api.png",
     "nl-script run --api on console example -- demo (exit 0)",
     "Entry (managed, collectible-reflectable) behind [UnmanagedCallersOnly] EntryNative: "
     "registers script.echo, sets script.count, execs back, drives builtin cvar.set, "
     "dumps cvar.list."),
    ("check.log", "check.png",
     "nl-script check bootstrap -- compile only (exit 0)",
     "Bootstrap is the non-collectible relay assembly (BootLoad/BootCall/BootUnload); "
     "FDD contract: runtimeconfig.json via GenerateRuntimeConfigurationFiles."),
]

CASES = {"m-sc0": CASES_M_SC0, "m-sc1": CASES_M_SC1, "m-sc2": CASES_M_SC2,
         "m-sc3": CASES_M_SC3}[DIR_NAME]


def main():
    os.makedirs(OUT, exist_ok=True)
    for log_name, png_name, title, note in CASES:
        render(log_name, png_name, title, note)
    print("%s evidence: %d PNGs -> %s" % (DIR_NAME, len(CASES), OUT))


if __name__ == "__main__":
    main()
