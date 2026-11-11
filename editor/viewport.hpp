#pragma once
// viewport.hpp — 编辑器视口面板（M-E2：SetParent 嵌入引擎窗口）
// 结构：ImGui Viewport 面板的内容区 → 一个 WS_CHILD 容器窗（本进程创建）
//       → 引擎 HWND 经 EmbedWindow::attach 挂入容器（去边框，铺满容器）。
// 每帧 draw() 里把容器同步到面板内容区矩形（ScreenToClient + SetWindowPos），
// 并把引擎窗口 MoveWindow 铺满容器——引擎侧 GLFW 收 WM_SIZE 自动重建 swapchain。
#include <cstdint>
#include <string>

struct ImVec2;   // imgui.h 前置声明（cpp 内 include，头不引 imgui）

namespace nl::editor::ui {

class Viewport {
public:
    // M-E6：每个视口实例用 viewId 生成独立 ImGui 窗口 ID，多视口可同时平铺。
    explicit Viewport(std::string viewId = "main") : viewId_(std::move(viewId)) {}
    ~Viewport();   // M-E6：多视口销毁时必须显式拆 WS_CHILD 容器，否则 HWND 泄漏
    void draw(bool connected);
    const std::string& viewId() const { return viewId_; }
    const char* label() const;   // "Viewport [main]" 等（cpp 内缓存）

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
    std::string viewId_;             // M-E6：多视口路由键
    mutable std::string label_;      // M-E6：ImGui 窗口 ID（const label() 内懒生成）
};

} // namespace nl::editor::ui
