#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace nl::core {

// M0.5：游戏热键边沿。M-C1：Grave(`)=控制台呼出、Tab=补全请求（viewer 边沿消费，
// 不进 ImGui 文本流；其余键全量以 KeyEvent 通道喂 ImGui，见 consumeKeyEvents）。
enum class Key { Unknown = 0, Escape, Space, Enter, W, A, S, D, C, R, V, N, F12, Grave, Tab, T, F10, Count };

struct WindowDesc {
    int         width  = 1280;
    int         height = 720;
    std::string title  = "nl-engine";
    bool        resizable = true;
    // 截图回归模式建隐藏窗口：画面走 GPU readback，与窗口可见性无关；
    // 可见窗口首显会抢前台焦点（实测 ~0.85 s/用例），--jobs 并行时打断打字。
    bool        visible = true;
};

struct Extent {
    int width  = 0;
    int height = 0;
};

// M-C1：键盘原始事件（POD）。key 是 GLFW key code（平台无关常量），ImGuiKey 的
// 映射在 renderer 侧 console_ui.cpp 做——core 不许出现 ImGui 符号。
struct KeyEvent {
    int  key = 0;
    bool down = false;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

// p8 M-I0（增量扩展）：鼠标原始按键事件（POD）。button 是 GLFW mouse button
// 常量（GLFW_MOUSE_BUTTON_LEFT 等）。既有 leftButtonDown() 行为不变——
// 本事件队列是**并列新增**的第三条通道（输入层 InputState 消费），不改老语义。
struct MouseButtonEvent {
    int  button = 0;
    bool down = false;
};

// pimpl：GLFW / 平台头文件只出现在 window.cpp 里，公开 API 不泄漏图形或窗口库类型。
class Window {
public:
    explicit Window(const WindowDesc& desc = {});
    ~Window();

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool valid() const;
    bool shouldClose() const;
    void pollEvents();
    void setTitle(const std::string& title);
    void setSize(int width, int height);  // 自动化 resize 回归用

    // 边沿触发：读一次即清除，避免长按重复响应
    bool keyPressed(Key key) const;

    // M-D3 (Q-D3-2 A): READ-ONLY key state for real-time driving input.
    // Pure glfwGetKey query -- it consumes NO event queue and does NOT clear
    // the keyPressed() hotkey edge, so ImGui / console / hotkeys are untouched.
    // glfwKey is a GLFW_KEY_* code (int keeps this header GLFW-free).
    bool rawKeyDown(int glfwKey) const;

    // M-C1：ImGui 键盘输入通路（消费制）。按帧取走本帧累积的事件 / 字符；
    // 超出 cap 的部分丢弃。viewer 把它们塞进 HudFrame 的定长数组，ImGui 映射
    // 在 renderer 侧 console_ui.cpp（core 不许出现 ImGui 符号）。
    std::size_t consumeKeyEvents(KeyEvent* out, std::size_t cap);
    std::size_t consumeTextChars(unsigned int* out, std::size_t cap);

    // P8 M-I0（增量扩展）：鼠标按键事件消费（消费制，与键盘事件同节奏）。
    // 上限 = 调用方容量，溢出丢弃最旧。
    std::size_t consumeMouseButtonEvents(MouseButtonEvent* out, std::size_t cap);

    // M0.4 轨道相机输入：拖动增量 / 滚轮增量按帧消费，左键按下状态用于判定拖动。
    void consumeMouse(float& deltaX, float& deltaY, float& scrollDelta);
    bool leftButtonDown() const;
    // M0.5 HUD 输入：窗口坐标系里的绝对鼠标位置（ImGui IO 需要，非边沿触发）。
    void mousePosition(float& x, float& y) const;

    // M-I3 指针锁定（UE GameViewportClient::SetMouseCaptureMode 三档取一 =
    // CapturePermanently 语义）：on = GLFW_CURSOR_DISABLED 持续捕获（光标隐藏、
    // 光标回调持续吐无界虚拟坐标 → consumeMouse 相对增量照常），off = NORMAL。
    // 释放/重捕获由调用方决定（viewer 约定：F10 切换、Esc 保持退出语义）。
    void setMouseCaptured(bool on);
    bool mouseCaptured() const;

    // M-I5：文件拖拽（glfwSetDropCallback → 消费制队列；路径原样透出，
    // 白名单/加载校验归消费方）。读一次即清。
    std::vector<std::string> consumeDroppedPaths();

    // M-I5：测试注入口——`--drop-sim` 确定性旗标走同一队列（真实 OS 拖拽事件
    // 无法自动化对拍；注入与窗口路径同一消费端）。
    void feedDroppedPath(const std::string& path);

    // M0.2：渲染器拿它创建原生 surface / swapchain
    void* nativeHandle() const;

    // 帧缓冲尺寸（高分屏下可能不等于 WindowDesc 的宽高）
    Extent framebufferSize() const;

    // 边沿触发的 resize：主循环每帧消费一次，交给 renderer 重建 swapchain
    bool consumeResize(Extent& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 不创建真窗口的平台层自检（供 --selftest 使用）
bool windowSelfTest();

} // namespace nl::core
