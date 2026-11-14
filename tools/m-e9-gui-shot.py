# -*- coding: utf-8 -*-
# M-E9/M-E10 GUI 目击证据：真 editor + 真 viewer（--preview），SendInput 点击
# outliner 首个树节点（选中实体）→ Properties 泛化面板自动拉 getProps →
# PrintWindow 免焦点抓 editor 窗口 → out/m-e9-editor-editui.png。
# 布局与点击坐标强依赖预置 imgui.ini（M-E8 工具同款手法，坐标为实测标定）。
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

gdi32 = ctypes.windll.gdi32

def find_window(title):
    result = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        n = user32.GetWindowTextLengthW(h)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if buf.value == title:
                if user32.IsWindowVisible(h):
                    result.append(h)
        return True
    user32.EnumWindows(cb, 0)
    return result[0] if result else None

def restore_if_min(h):
    if user32.IsIconic(h):
        user32.ShowWindow(h, 9)
        time.sleep(0.3)

def window_origin(hwnd):
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.left, rect.top

def click_at(x, y):
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.1)
    user32.mouse_event(0x0002, 0, 0, 0, 0)   # LEFTDOWN
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)   # LEFTUP
    time.sleep(0.3)

def main():
    ini = os.path.join(ROOT, "imgui.ini")
    with open(ini, "w", encoding="utf-8", newline="\n") as f:
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

[Window][Camera]
Pos=1560,760
Size=380,140

[Window][Material Inspector]
Pos=780,760
Size=380,280

[Window][Viewport [main]]
Pos=390,0
Size=1280,740

[Window][Log]
Pos=390,750
Size=1280,290

""")
    proc_v = subprocess.Popen([VIEWER, "--preview", "--editor-bridge", "--no-hud",
                               "--editor-endpoint", "tcp://127.0.0.1:29110"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
    time.sleep(1.5)
    proc_e = subprocess.Popen([EDITOR], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
    try:
        hwnd = None
        for _ in range(40):
            time.sleep(0.5)
            hwnd = find_window("nl-engine Editor")
            if hwnd: break
        if not hwnd:
            print("FAIL editor window not found"); return 1
        # 恢复并复位窗口（沙箱会话里 GLFW 窗常以最小化态出现：GetWindowRect =
        # 经典 -32000 系——不恢复则后续点击全打到屏幕外，PrintWindow 却仍能
        # 抓到完整内容，极具迷惑性；恢复后必须复核 rect）。
        ok_restore = False
        for _ in range(5):
            if user32.IsIconic(hwnd):
                user32.ShowWindow(hwnd, 9)   # SW_RESTORE
                time.sleep(0.5)
            user32.MoveWindow(hwnd, 0, 0, 1680, 1050, True)
            time.sleep(0.5)
            rect0 = wintypes.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect0))
            if not user32.IsIconic(hwnd) and rect0.left == 0 and rect0.top == 0:
                ok_restore = True
                break
        rect0 = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect0))
        print("restore ok=%s rect=(%d,%d)" % (ok_restore, rect0.left, rect0.top))
        time.sleep(3.5)   # viewer 连入 + outliner 首拉

        wx, wy = window_origin(hwnd)
        # 点选 Scene Outliner 首个树节点 preview-model：PNG(PrintWindow)=窗口
        # 原点坐标系，首行中心 PNG(50,536)——点击基点用窗口原点（客户区原点
        # 会多加 7px 边框 + 30px 标题栏）；窗口可能中途再最小化（实测 restore
        # 后截图时 rect 缩成 158x26），每次点击前复核 rect 并重恢复。
        for _ in range(3):
            rect0 = wintypes.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect0))
            if user32.IsIconic(hwnd) or rect0.left != 0 or rect0.top != 0:
                user32.ShowWindow(hwnd, 9)
                time.sleep(0.4)
                user32.MoveWindow(hwnd, 0, 0, 1680, 1050, True)
                time.sleep(0.4)
            click_at(wx + 50, wy + 536)
            time.sleep(0.8)
        rect1 = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect1))
        print("post-click rect=(%d,%d,%d,%d) iconic=%d" %
              (rect1.left, rect1.top, rect1.right, rect1.bottom, user32.IsIconic(hwnd)))
        time.sleep(1.0)   # Properties 面板 getProps 拉取 + 渲染

        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        hdc = user32.GetWindowDC(hwnd)
        mem = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        gdi32.SelectObject(mem, bmp)
        ok = user32.PrintWindow(hwnd, mem, 2)
        class BMIH(ctypes.Structure):
            _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                        ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                        ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                        ("biClrImportant", wintypes.DWORD)]
        bmpinfo = BMIH()
        bmpinfo.biSize = ctypes.sizeof(BMIH)
        bmpinfo.biWidth = w
        bmpinfo.biHeight = -h
        bmpinfo.biPlanes = 1
        bmpinfo.biBitCount = 32
        bmpinfo.biCompression = 0
        buf = ctypes.create_string_buffer(w * h * 4)
        gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmpinfo), 0)
        from PIL import Image
        img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
        out_path = os.path.join(OUT, "m-e9-editor-editui.png")
        img.save(out_path)
        print("OK PrintWindow=%s size=%dx%d -> %s" % (ok, w, h, out_path))
        gdi32.DeleteObject(bmp); gdi32.DeleteDC(mem); user32.ReleaseDC(hwnd, hdc)
        return 0
    finally:
        for p in (proc_e, proc_v):
            p.terminate()
            try: p.wait(timeout=5)
            except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
