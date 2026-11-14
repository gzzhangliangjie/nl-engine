# -*- coding: utf-8 -*-
# M-E11 嵌入视口截图基建（BitBlt 合成）。
# 根因背景（M-E6/M-E8 两次留账）：PrintWindow 抓 editor 主窗时嵌入视口区域全黑
# ——viewer 的 GLFW 窗是跨进程 SetParent 挂入的 DX12 子窗口，PrintWindow 只能
# 拿到目标进程自己的 GDI 表面，跨进程 flip-model 子窗不在其中。
# 方案 = 双源合成：
#   ① PrintWindow 抓 editor 主窗（ImGui UI 层完整，视口区黑）；
#   ② 视口容器窗（NlEditorViewportHost，viewer 子窗铺满其上）屏幕矩形
#      BitBlt(GetDC(NULL)) —— DWM 已合成像素，全屏抓包可靠（M-E2 验收同路径）；
#   ③ UI 图的容器对应区域替换为 BitBlt 帧 -> 单张 PNG。
# 用法：python tools/editor-viewport-shot.py [out.png]
# 沉淀为 M-E12/M-E13 GUI 验收共用基建（全流程自含：起 viewer/editor/恢复窗口）。
import ctypes, subprocess, sys, time, os
from ctypes import wintypes

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
try:
    ctypes.windll.shcore.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIEWER = os.path.join(ROOT, "build", "DX12", "viewer.exe")
EDITOR = os.path.join(ROOT, "build", "DX12", "editor.exe")
OUT = os.path.join(ROOT, "out")
os.makedirs(OUT, exist_ok=True)

CONTAINER_CLASS = "NlEditorViewportHost"   # M-E2：WS_CHILD 容器（viewer 子窗铺满）


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


def find_child_by_class(parent, cls):
    result = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, buf, 256)
        if buf.value == cls:
            result.append(h)
        return True
    user32.EnumChildWindows(parent, cb, 0)
    return result[0] if result else None


def get_rect(h):
    r = wintypes.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    return r


def grab_screen(x, y, w, h):
    """BitBlt 全屏 DC 抓物理坐标矩形 -> PIL RGBA（DWM 合成帧，跨进程 DX12 可见）"""
    hdc = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h, hdc, x, y, 0x00CC0020)   # SRCCOPY
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
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(None, hdc)
    from PIL import Image
    return Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)


def grab_window(hwnd):
    """PrintWindow 抓窗口自身 GDI 表面（UI 层；跨进程 DX12 子窗 = 黑）"""
    r = get_rect(hwnd)
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
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    from PIL import Image
    return Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1), (w, h)


def grab_window_flag(hwnd, flag=2):
    """PrintWindow 带旗标抓窗口（2 = PW_RENDERFULLCONTENT：走 DWM 截取，
    Win10+ 对跨进程 DX12 子窗也有效）"""
    r = get_rect(hwnd)
    w, h = r.right - r.left, r.bottom - r.top
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, flag)
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
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    from PIL import Image
    return Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)


def image_stats(img):
    g = img.convert("L")
    data = list(g.getdata())
    mx = max(data)
    mean = sum(data) / len(data)
    var = sum((v - mean) ** 2 for v in data) / len(data)
    return mx, var ** 0.5


def restore_window(hwnd, w=1680, h=1050):
    """恢复最小化 + 复位到 (0,0)（沙箱会话窗常以最小化态出现，rect=-32000 系）"""
    for _ in range(5):
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 9)   # SW_RESTORE
            time.sleep(0.4)
        user32.MoveWindow(hwnd, 0, 0, w, h, True)
        time.sleep(0.4)
        r = get_rect(hwnd)
        if not user32.IsIconic(hwnd) and r.left == 0 and r.top == 0:
            return True
    return False


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(OUT, "editor-viewport.png")
    # 预置布局（同 m-e9 标定：Viewport 面板独立位置，容器窗会贴其内容区）
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
            if hwnd:
                break
        if not hwnd:
            print("FAIL editor window not found")
            return 1
        restore_window(hwnd)
        time.sleep(3.5)   # viewer 连入 + announce + SetParent 挂容器
        # 找视口容器（SetParent 实挂证据；跨进程子窗 GLFW30 铺满其上）
        container = find_child_by_class(hwnd, CONTAINER_CLASS)
        print("container %s hwnd=%x" % (CONTAINER_CLASS, container or 0))
        if not container:
            print("FAIL viewport container not found (viewer not attached?)")
            return 1
        time.sleep(1.0)   # 渲染帧稳定

        # 等容器被 syncNative 铺满（时序抖动：容器可能短暂 1x1；后台节流慢启动）
        cw = ch = 0
        for i in range(60):
            cr = get_rect(container)
            cw, ch = cr.right - cr.left, cr.bottom - cr.top
            if cw > 200 and ch > 200:
                break
            if i % 10 == 0:
                print("  wait container rect: %dx%d (t=%.1fs)" % (cw, ch, i * 0.5), flush=True)
            time.sleep(0.5)
        ui_img, (uw, uh) = grab_window(hwnd)
        wr = get_rect(hwnd)
        cr = get_rect(container)
        # 容器屏幕矩形（物理坐标，DPI-aware 一致系）-> UI 图内偏移
        ox, oy = cr.left - wr.left, cr.top - wr.top
        print("viewport @ui(%d,%d) size %dx%d" % (ox, oy, cw, ch))
        if ox < 0 or oy < 0 or ox + cw > uw or oy + ch > uh:
            print("FAIL container rect outside editor window")
            return 1
        # 视口帧三源择优（无干扰优先；纯色不算命中——std 判据）：
        #   a) PrintWindow(viewer 子窗 GLFW30, PW_RENDERFULLCONTENT)——3D 画面
        #      所在窗口（DWM 截取路径，跨进程 DX12 Win10+ 有效）
        #   b) PrintWindow(container)——容器自身表面（纯背景，兜底）
        #   c) BitBlt(GetDC(NULL)) 屏幕抓帧（TOPMOST 置顶瞬间）——被遮挡时
        #      抓到遮挡物（2026-11-14 两次实证），最后兜底
        vp_img = None
        src = "?"
        for name, fn in (("pw-viewer-child",
                          lambda: grab_window_flag(find_child_by_class(container, "GLFW30"))),
                         ("pw-container", lambda: grab_window_flag(container))):
            try:
                cand = fn()
            except Exception:
                continue
            if cand is None:
                continue
            if cand.size != (cw, ch):
                cand = cand.resize((cw, ch))
            mx, sd = image_stats(cand)
            print("  try %-14s max=%d std=%.1f" % (name, mx, sd), flush=True)
            if mx > 50 and sd > 10.0:   # 非全黑且非纯色 = 真画面
                vp_img = cand.convert("RGB")
                src = name
                break
        if vp_img is None:
            # BitBlt 兜底：双次置顶（TOPMOST Z 序内后来者居前，两次插入提高
            # 胜率）+ 遮挡自检（与容器纯色背景比对：遮挡物 diff 率近 1.0，
            # 出 FAIL 不落假图——2026-11-14 用户桌面 TOPMOST 会话窗实证）
            for _ in range(2):
                user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
                time.sleep(0.25)
            time.sleep(0.4)
            bit_img = grab_screen(cr.left, cr.top, cw, ch).convert("L")
            user32.SetWindowPos(hwnd, -2, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
            bg_img = grab_window_flag(container).convert("L").resize((cw, ch))
            bl, gl = list(bit_img.getdata()), list(bg_img.getdata())
            diff = sum(1 for a, b in zip(bl, gl) if abs(a - b) > 16) / len(bl)
            print("bitblt vs container-bg diff-rate = %.3f" % diff, flush=True)
            if diff > 0.95:
                print("FAIL bitblt source occluded by other window (diff≈1.0)")
                return 1
            vp_img = grab_screen(cr.left, cr.top, cw, ch).convert("RGB")
            src = "bitblt-screen"
        print("viewport frame source = %s" % src)
        # 合成：UI 图容器区域替换为视口帧
        from PIL import Image
        ui_img = ui_img.convert("RGB")
        ui_img.paste(vp_img, (ox, oy))
        ui_img.save(out_path)
        print("OK composited %dx%d (viewport %dx%d pasted at %d,%d) -> %s" %
              (uw, uh, cw, ch, ox, oy, out_path))
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
