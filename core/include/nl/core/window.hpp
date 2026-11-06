#pragma once

#include <memory>
#include <string>

namespace nl::core {

enum class Key { Unknown = 0, Escape, Space, Enter, W, A, S, D, Count };

struct WindowDesc {
    int         width  = 1280;
    int         height = 720;
    std::string title  = "nl-engine";
    bool        resizable = true;
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

    // 边沿触发：读一次即清除，避免长按重复响应
    bool keyPressed(Key key) const;

    // M0.2 交给 NVRHI 创建 surface 用；当前阶段不调用
    void* nativeHandle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 不创建真窗口的平台层自检（供 --selftest 使用）
bool windowSelfTest();

} // namespace nl::core
