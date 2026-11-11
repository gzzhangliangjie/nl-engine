#pragma once
// viewport.hpp — 编辑器视口面板（M-E2：SetParent 嵌入引擎窗口）
// 结构：ImGui Viewport 面板的内容区 → 一个 WS_CHILD 容器窗（本进程创建）
//       → 引擎 HWND 经 EmbedWindow::attach 挂入容器（去边框，铺满容器）。
// 每帧 draw() 里把容器同步到面板内容区矩形（ScreenToClient + SetWindowPos），
// 并把引擎窗口 MoveWindow 铺满容器——引擎侧 GLFW 收 WM_SIZE 自动重建 swapchain。
#include <cstdint>

struct ImVec2;   // imgui.h 前置声明（cpp 内 include，头不引 imgui）

namespace nl::editor::ui {

class Viewport {
public:
    void draw(bool connected);

    // M-E2：引擎 announce 后由 main 调（editorWindowHwnd = 编辑器 GLFW 顶层窗）。
    // 幂等：重复 announce（引擎重连）先拆旧容器再建新。
    void attachEngine(uint64_t engineHwnd, void* editorWindowHwnd);

    // 引擎掉线（进程已死）时拆容器，面板回落占位提示。
    void detach();

    uint64_t engineHwnd() const { return engineHwnd_; }
    bool attached() const { return attached_; }

private:
    // 每帧：容器贴面板内容区 + 引擎窗铺满容器（pos/size 均为面板内容区屏幕坐标）
    void syncNative(const ImVec2& contentPos, const ImVec2& contentSize);
    uint64_t engineHwnd_   = 0;
    uint64_t containerHwnd_ = 0;   // 本进程 WS_CHILD 容器
    uint64_t editorHwnd_   = 0;    // 编辑器顶层窗（坐标换算用）
    bool     attached_     = false;
    int      lastW_ = 0, lastH_ = 0; // 引擎窗尺寸去抖（同尺寸不动 MoveWindow）
};

} // namespace nl::editor::ui
