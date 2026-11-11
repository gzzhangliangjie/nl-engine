// viewport.cpp — 编辑器视口面板（M-E2：原生容器窗 + 引擎 HWND 嵌入）
// 原理：native child window 永远盖在父窗 GL 内容之上，所以 ImGui 只画面板
// 标题栏/边框（chrome），容器精确贴内容区——引擎像素在容器里直出，不走 RPC。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "viewport.hpp"
#include "embed_window.hpp"   // editor_bridge：SetParent + 去边框
#include <imgui.h>
#include <algorithm>

namespace nl::editor::ui {

namespace {

constexpr wchar_t kHostClass[] = L"NlEditorViewportHost";

LRESULT CALLBACK hostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

HWND ensureContainerClassAndCreate(HWND parent) {
    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = hostProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); // 引擎窗挂入前不闪白
        wc.lpszClassName = kHostClass;
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }
    // M-E6：WS_CLIPSIBLINGS —— 多个平铺容器互不遮挡（后建的默认压在先建的上面）
    return CreateWindowExW(0, kHostClass, L"",
                           WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                           0, 0, 16, 16, parent, nullptr, hInst, nullptr);
}

} // namespace

const char* Viewport::label() const {
    if (label_.empty()) label_ = "Viewport [" + viewId_ + "]";
    return label_.c_str();
}

Viewport::~Viewport() { detach(); }

void Viewport::attachEngine(uint64_t engineHwnd, void* editorWindowHwnd) {
    HWND parent = reinterpret_cast<HWND>(editorWindowHwnd);
    if (!parent || engineHwnd == 0) return;
    if (attached_) detach();   // 引擎重连：拆旧容器重建（幂等）
    HWND container = ensureContainerClassAndCreate(parent);
    if (!container) return;
    editorHwnd_   = reinterpret_cast<uint64_t>(parent);
    containerHwnd_ = reinterpret_cast<uint64_t>(container);
    engineHwnd_   = engineHwnd;
    // SetParent 跨进程 + 去引擎窗边框（编辑器主动挂，EDITOR-PLAN §6 拍板口径）
    nl::editor::EmbedWindow::attach(reinterpret_cast<void*>(engineHwnd), container);
    attached_ = true;
    lastW_ = lastH_ = 0;   // 强制首帧 MoveWindow
}

void Viewport::detach() {
    if (containerHwnd_) DestroyWindow(reinterpret_cast<HWND>(containerHwnd_));
    containerHwnd_ = 0;
    engineHwnd_ = 0;
    attached_ = false;
    lastW_ = lastH_ = 0;
}

void Viewport::syncNative(const ImVec2& contentPos, const ImVec2& contentSize) {
    if (!attached_ || !containerHwnd_ || !editorHwnd_) return;
    // ImGui 的 GetCursorScreenPos 坐标空间原点 = 编辑器窗客户区左上角（DisplaySize
    // 就是客户区大小），而 WS_CHILD 的 SetWindowPos 位置也是相对父窗客户区——
    // 两套坐标天然同系，直接用。绝不 ScreenToClient（那是屏幕坐标专用，多转
    // 一次 = 减两次窗口起点 = 整体偏出客户区，2026-11-11 实锤教训）。
    const int cx = (int)contentPos.x;
    const int cy = (int)contentPos.y;
    const int cw = std::max(1, (int)contentSize.x);
    const int ch = std::max(1, (int)contentSize.y);
    SetWindowPos(reinterpret_cast<HWND>(containerHwnd_), nullptr,
                 cx, cy, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    // 引擎窗铺满容器（同尺寸跳过；WM_SIZE 驱动引擎侧 swapchain 重建）
    if (cw != lastW_ || ch != lastH_) {
        MoveWindow(reinterpret_cast<HWND>(engineHwnd_), 0, 0, cw, ch, TRUE);
        lastW_ = cw; lastH_ = ch;
    }
}

void Viewport::draw(bool connected) {
    if (!ImGui::Begin(label())) { ImGui::End(); return; }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (attached_) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();   // Dummy 之前取（Dummy 会推进光标）
        ImGui::Dummy(avail);   // 占住布局，内容区被原生容器盖住（像素直出）
        syncNative(pos, avail);
    } else {
        if (connected) {
            ImGui::Text("engine connected (pid announced) but not embedded yet.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::TextWrapped("no engine connected.\n"
                "start:  viewer.exe --editor-bridge\n"
                "(viewer without the flag runs standalone; editor crash never kills engine)");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(avail);
    }
    ImGui::End();
}

} // namespace nl::editor::ui
