# -*- coding: utf-8 -*-
"""P8-Script 工具版证据图生成器（M-SC0 收口段）。

铁律：所有内容从工具**真实输出**渲染（example/m-sc0/*.log，
由 nl-script.exe 实跑重定向落盘），不许手绘示意。产物进 example/m-sc0/。
复现：example/m-sc0/run.bat（重跑五用例 -> 重新生成本图 -> exit 0 校验）。
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, "example", "m-sc0")
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
    if "run: ret=" in t or "check: PASS" in t:
        return GREEN
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


CASES = [
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


def main():
    os.makedirs(OUT, exist_ok=True)
    for log_name, png_name, title, note in CASES:
        render(log_name, png_name, title, note)
    print("M-SC0 evidence: %d PNGs -> %s" % (len(CASES), OUT))


if __name__ == "__main__":
    main()
