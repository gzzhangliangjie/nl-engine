# -*- coding: utf-8 -*-
# M-E8 GUI 目击证据：起真 editor（nl-engine Editor）+ 真 viewer（--preview），
# 等 outliner 层级树渲染后 PrintWindow 免焦点抓 editor 窗口 → out/m-e8-outliner.png。
# DPI-aware（M-E6 踩坑①：SetProcessDpiAwarenessContext PER_MONITOR_AWARE_V2）。
import ctypes, subprocess, sys, time, os
from ctypes import wintypes

user32 = ctypes.windll.user32
try:  # DPI-aware 统一物理坐标（M-E6 踩坑①）
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
    # 枚举顶层窗口，按标题前缀精确匹配（禁 GetWindowText 模糊误中资源管理器）
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
        user32.ShowWindow(h, 9)  # SW_RESTORE
        time.sleep(0.3)

def main():
    # 预置 ImGui 布局（editor 缺省把所有插件面板叠在 (60,60) 互相遮挡；
    # imgui.ini 工作目录 = 启动 cwd = ROOT）。左列自上而下 Connection/
    # Preview/Scene Outliner，右侧嵌入视口，Log 底部。
    ini = os.path.join(ROOT, "imgui.ini")
    with open(ini, "w", encoding="utf-8", newline="\n") as f:
        f.write("""[Window][Debug##Default]
Pos=60,60
Size=400,400

[Window][Connection]
Pos=0,0
Size=380,180

[Window][Preview]
Pos=0,190
Size=380,250

[Window][Scene Outliner]
Pos=0,450
Size=380,580

[Window][Camera]
Pos=0,1040
Size=380,140

[Window][Material Inspector]
Pos=390,760
Size=380,280

[Window][Hello (DLL)]
Pos=390,1040
Size=380,140

[Window][Viewport [main]]
Pos=390,0
Size=1280,750

[Window][Log]
Pos=390,760
Size=1280,270

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
        restore_if_min(hwnd)
        # 放大窗口保证多插件面板全可见（ImGui 缺省平铺，868x571 会把 outliner 挤出视野）
        user32.MoveWindow(hwnd, 0, 0, 1680, 1050, True)
        # 等 viewer 连入 + outliner 首帧自动拉取（连接后 500ms 内）
        time.sleep(4.0)

        # 客户区尺寸
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        hdc = user32.GetWindowDC(hwnd)
        mem = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        gdi32.SelectObject(mem, bmp)
        # PrintWindow 免焦点（PW_RENDERFULLCONTENT=2 抓 DirectComposition 内容）
        ok = user32.PrintWindow(hwnd, mem, 2)
        # 保存 PNG（BGRA → PIL；wintypes 无 BITMAPINFO，手工定义 header）
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
        out_path = os.path.join(OUT, "m-e8-outliner.png")
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
