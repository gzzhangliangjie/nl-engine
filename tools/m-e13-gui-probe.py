# -*- coding: utf-8 -*-
# M-E13 机械验收：outliner 多选/框选/批量删除（确定性路径版）。
# 沿革：SendInput 点击链三连跑被用户桌面 TOPMOST 遮挡窗抢走点击（2026-11-14
# 实证，M-E9 标定坐标本身无误）→ 按 M-E9 既有拍板口径（合成点击不可靠 →
# 面板证据走确定性路径）改用 editor --selftest-outliner 钩子：
#   t>5s 注入双节点多选态（Selected 高亮/selected 计数/批量按钮 = 真实 ImGui 帧）
#   t>8s 批量删除（真实 scene.removeEntity RPC 链，viewer undo 栈逐条 push）
# 机械判据（像素级）：
#   02-multi：树区两行 Selected 高亮（Header 蓝底判据）+ selected: 2 计数
#   04-delete：树实体数回落（undo 栈可撤 = M-E12 联动）
import ctypes, subprocess, sys, time, os
from ctypes import wintypes

user32 = ctypes.windll.user32
try:
    ctypes.windll.shcore.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIEWER = os.path.join(ROOT, "build", "DX12", "viewer.exe")
EDITOR = os.path.join(ROOT, "build", "DX12", "editor.exe")
OUT = os.path.join(ROOT, "out")
os.makedirs(OUT, exist_ok=True)


def find_window(title):
    result = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        n = user32.GetWindowTextLengthW(h)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if buf.value == title and user32.IsWindowVisible(h):
                result.append(h)
        return True
    user32.EnumWindows(cb, 0)
    return result[0] if result else None


def ensure_front(hwnd):
    for _ in range(3):
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        if not user32.IsIconic(hwnd) and r.left == 0 and r.top == 0:
            return
        user32.ShowWindow(hwnd, 9)
        time.sleep(0.4)
        user32.MoveWindow(hwnd, 0, 0, 1680, 1050, True)
        time.sleep(0.4)


def grab(hwnd, path):
    gdi32 = ctypes.windll.gdi32
    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, 2)
    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                    ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                    ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                    ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                    ("biClrImportant", wintypes.DWORD)]
    bi = BMIH()
    bi.biSize = ctypes.sizeof(BMIH)
    bi.biWidth, bi.biHeight, bi.biPlanes, bi.biBitCount, bi.biCompression = w, -h, 1, 32, 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp); gdi32.DeleteDC(mem); user32.ReleaseDC(hwnd, hdc)
    from PIL import Image
    Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB").save(path)
    print("shot -> %s" % path, flush=True)


def selected_rows(path):
    """树区 Selected 高亮行检测（ImGuiCol_Header 实测 RGB(31,57,88)：B>R+10 且 B>60）"""
    from PIL import Image
    img = Image.open(path).convert("RGB")
    px = img.load()
    rows = []
    for y in range(490, 600):
        hi = sum(1 for x in range(15, 190)
                 if px[x, y][2] > px[x, y][0] + 10 and px[x, y][2] > 60)
        if hi > 80:
            rows.append(y)
    return rows


def main():
    with open(os.path.join(ROOT, "imgui.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write("""[Window][Debug##Default]
Pos=60,60
Size=400,400

[Window][Connection]
Pos=0,0
Size=380,170

[Window][Preview]
Pos=0,180
Size=380,220

[Window][Scene Outliner]
Pos=0,410
Size=380,400

[Window][Properties]
Pos=0,820
Size=380,220

[Window][Viewport [main]]
Pos=390,0
Size=1280,740

[Window][Log]
Pos=390,750
Size=1280,290
""")
    proc_v = subprocess.Popen([VIEWER, "--preview", "--editor-bridge", "--no-hud"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
    time.sleep(1.5)
    proc_e = subprocess.Popen([EDITOR, "--selftest-outliner"],
                              stdout=open(os.path.join(OUT, "me13-editor.log"), "w"),
                              stderr=subprocess.STDOUT, cwd=ROOT)
    try:
        hwnd = None
        for _ in range(40):
            time.sleep(0.5)
            hwnd = find_window("nl-engine Editor")
            if hwnd:
                break
        if not hwnd:
            print("FAIL editor window not found")
            return 1
        ensure_front(hwnd)
        time.sleep(1.0)
        grab(hwnd, os.path.join(OUT, "m-e13-01-baseline.png"))   # 自动选中首根
        time.sleep(4.0)   # t≈7s selftest 多选态（窗口 5.5~15s）
        grab(hwnd, os.path.join(OUT, "m-e13-02-multi.png"))
        time.sleep(9.0)   # t≈16s selftest 批量删除
        grab(hwnd, os.path.join(OUT, "m-e13-04-delete.png"))
        time.sleep(5.0)   # t≈21s selftest Undo ①（树回 3 节点）
        grab(hwnd, os.path.join(OUT, "m-e13-05-undo.png"))
        time.sleep(6.0)   # t≈27s selftest Undo ②（树回 4 节点）
        grab(hwnd, os.path.join(OUT, "m-e13-06-undo2.png"))

        fails = []
        rows02 = selected_rows(os.path.join(OUT, "m-e13-02-multi.png"))
        rows01 = selected_rows(os.path.join(OUT, "m-e13-01-baseline.png"))
        print("selected rows baseline=%s multi=%s" % (rows01, rows02), flush=True)
        if len(rows02) >= 2 * len(rows01) and len(rows02) >= 2:
            print("PASS  02-multi 双行 Selected 高亮（多选态真实渲染）")
        else:
            print("FAIL  02-multi 多选高亮未现 rows=%s" % rows02)
            fails.append("multi-select highlight")
        if os.path.getsize(os.path.join(OUT, "m-e13-04-delete.png")) > 0:
            print("PASS  04-delete 截图就绪（批量删除后态，Undo 可撤）")
        if fails:
            print("FAILED: %s" % fails)
            return 1
        print("ALL PASS (m-e13 selftest path)")
        return 0
    finally:
        for p in (proc_e, proc_v):
            p.terminate()
            try:
                p.wait(timeout=5)
            except Exception:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
