#include "nl/core/window.hpp"
#include "nl/core/log.hpp"

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#  include <imm.h>   // ImmAssociateContext：解除 IME 关联（M-C1）
#endif

#include <array>
#include <utility>
#include <vector>

namespace nl::core {
namespace {

Key mapKey(int glfwKey) {
    switch (glfwKey) {
        case GLFW_KEY_ESCAPE: return Key::Escape;
        case GLFW_KEY_SPACE:  return Key::Space;
        case GLFW_KEY_ENTER:  return Key::Enter;
        case GLFW_KEY_W:      return Key::W;
        case GLFW_KEY_A:      return Key::A;
        case GLFW_KEY_S:      return Key::S;
        case GLFW_KEY_D:      return Key::D;
        case GLFW_KEY_C:      return Key::C;
        case GLFW_KEY_R:      return Key::R;
        case GLFW_KEY_V:      return Key::V;   // M0.8：线框开关
        case GLFW_KEY_N:      return Key::N;   // M0.8：循环切着色模式
        case GLFW_KEY_F12:    return Key::F12;
        case GLFW_KEY_GRAVE_ACCENT: return Key::Grave;  // M-C1：控制台呼出
        case GLFW_KEY_TAB:    return Key::Tab;   // M-C1：补全请求（viewer 边沿）
        case GLFW_KEY_T:      return Key::T;     // P8 M-CAM4：Chase↔Free 切换边沿（C 已被 cull 占用）
        case GLFW_KEY_F10:    return Key::F10;   // M-I3：指针锁定切换边沿（Esc=退出约定让位）
        default:              return Key::Unknown;
    }
}

void glfwErrorCallback(int code, const char* description) {
    log::deviceError("GLFW", std::string("error ") + std::to_string(code) + ": " +
                            (description ? description : "<null>"));
}

} // namespace

struct Window::Impl {
    GLFWwindow* handle = nullptr;
    std::array<bool, static_cast<size_t>(Key::Count)> keyDown{};
    int  pendingWidth  = 0;
    int  pendingHeight = 0;
    double mouseDX = 0.0, mouseDY = 0.0, scrollY = 0.0;
    double lastCursorX = 0.0, lastCursorY = 0.0;
    bool   haveCursor = false;    // 是否收到过光标位置（HUD 的绝对坐标）
    bool   haveDragBase = false;  // 拖动增量是否有基准点（按下左键时重置）
    bool   leftDown = false;

    // M-C1：键盘原始事件 / 文本字符队列（ImGui 输入通路，消费制，见 consumeKeyEvents）。
    std::vector<KeyEvent>     keyEvents;
    std::vector<unsigned int> textChars;

    // P8 M-I0（增量扩展）：鼠标按键事件队列（InputState 消费通道，见 window.hpp）。
    std::vector<MouseButtonEvent> mouseButtonEvents;

    // M-I5：文件拖拽队列（consumeDroppedPaths 消费制）。
    std::vector<std::string> droppedPaths;

    void onKey(int glfwKey, int action, int mods) {
        // 游戏热键边沿（PRESS 才记，读一次即清）
        if (action == GLFW_PRESS) {
            const Key key = mapKey(glfwKey);
            if (key != Key::Unknown) keyDown[static_cast<size_t>(key)] = true;
        }
        // ImGui 通道：PRESS / RELEASE / REPEAT 全记（ImGui 自己消化重复输入）
        KeyEvent ev;
        ev.key = glfwKey;
        ev.down = (action != GLFW_RELEASE);
        ev.ctrl  = (mods & GLFW_MOD_CONTROL) != 0;
        ev.shift = (mods & GLFW_MOD_SHIFT) != 0;
        ev.alt   = (mods & GLFW_MOD_ALT) != 0;
        keyEvents.push_back(ev);
    }

    static void keyCallback(GLFWwindow* handle, int key, int /*scancode*/, int action, int mods) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            impl->onKey(key, action, mods);
        }
    }

    // M-C1：文本字符（ImGui InputText 的字符流）。反引号全局过滤：` 只做控制台
    // 呼出边沿，呼出瞬间不该往输入行打一个 `。
    static void charCallback(GLFWwindow* handle, unsigned int codepoint) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            if (codepoint == '`') return;
            impl->textChars.push_back(codepoint);
        }
    }

    static void framebufferCallback(GLFWwindow* handle, int width, int height) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            impl->pendingWidth  = width;
            impl->pendingHeight = height;
        }
    }

    static void cursorCallback(GLFWwindow* handle, double x, double y) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            if (impl->haveDragBase) {
                impl->mouseDX += x - impl->lastCursorX;
                impl->mouseDY += y - impl->lastCursorY;
            }
            impl->lastCursorX = x;
            impl->lastCursorY = y;
            impl->haveCursor = true;
            impl->haveDragBase = true;
        }
    }

    static void buttonCallback(GLFWwindow* handle, int button, int action, int /*mods*/) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            if (button == GLFW_MOUSE_BUTTON_LEFT) {
                impl->leftDown = (action == GLFW_PRESS);
                // 按下瞬间只重置「拖动增量」的基准点，不动绝对坐标：
                // HUD 需要每帧稳定的 MousePos，否则点击面板会丢一帧输入。
                if (action == GLFW_PRESS) impl->haveDragBase = false;
            }
            // P8 M-I0（增量扩展）：全部按键进事件队列（老 leftDown 语义不动）。
            impl->mouseButtonEvents.push_back({button, action != GLFW_RELEASE});
        }
    }

    static void scrollCallback(GLFWwindow* handle, double /*x*/, double y) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            impl->scrollY += y;
        }
    }

    // M-I5：文件拖拽（消费制队列；路径原样存 UTF-8/本地多字节，校验归消费方）。
    static void dropCallback(GLFWwindow* handle, int count, const char** paths) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            for (int i = 0; i < count; ++i)
                if (paths[i]) impl->droppedPaths.emplace_back(paths[i]);
        }
    }
};

Window::Window(const WindowDesc& desc) : impl_(std::make_unique<Impl>()) {
    glfwSetErrorCallback(glfwErrorCallback);

    if (glfwInit() != GLFW_TRUE) {
        log::error("glfwInit failed");
        return;
    }

    // 底座是 NVRHI，不创建 OpenGL context（与 Carrot Init 同款设置）
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);

    impl_->handle = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
    if (!impl_->handle) {
        log::error("glfwCreateWindow failed (", desc.width, "x", desc.height, ")");
        glfwTerminate();
        return;
    }

    glfwSetWindowUserPointer(impl_->handle, impl_.get());
    glfwSetKeyCallback(impl_->handle, &Impl::keyCallback);
    glfwSetCharCallback(impl_->handle, &Impl::charCallback);
    glfwSetFramebufferSizeCallback(impl_->handle, &Impl::framebufferCallback);
    glfwSetCursorPosCallback(impl_->handle, &Impl::cursorCallback);
    glfwSetMouseButtonCallback(impl_->handle, &Impl::buttonCallback);
    glfwSetDropCallback(impl_->handle, &Impl::dropCallback);   // M-I5 文件拖拽
#if defined(_WIN32)
    // M-C1：中文 IME 会把反引号（VK_OEM_3）当编辑键抢走，WM_KEYDOWN 根本到不了
    // GLFW（插桩实证：ESC/TAB 有事件、grave 无）——viewer 全窗口解除 IME 关联。
    // 控制台输入本就 ASCII（ImGui 默认字体无 CJK 字形），游戏视口也不该被 IME 拦热键。
    ImmAssociateContext(glfwGetWin32Window(impl_->handle), nullptr);
#endif
    glfwSetScrollCallback(impl_->handle, &Impl::scrollCallback);
    log::info("window created ", desc.width, "x", desc.height,
              " | no GL context | drag=orbit, wheel=zoom, C=cull, R=reset, ESC=quit");
}

Window::~Window() {
    if (impl_ && impl_->handle) {
        glfwDestroyWindow(impl_->handle);
        glfwTerminate();
        log::info("window destroyed");
    }
}

Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;

bool Window::valid() const { return impl_ && impl_->handle != nullptr; }
bool Window::shouldClose() const { return !valid() || glfwWindowShouldClose(impl_->handle) == GLFW_TRUE; }
void Window::pollEvents() { if (valid()) glfwPollEvents(); }
void Window::setTitle(const std::string& title) {
    if (valid()) glfwSetWindowTitle(impl_->handle, title.c_str());
}

void Window::setSize(int width, int height) {
    if (valid() && width > 0 && height > 0) glfwSetWindowSize(impl_->handle, width, height);
}

bool Window::keyPressed(Key key) const {
    if (!valid() || key == Key::Unknown) return false;
    const size_t i = static_cast<size_t>(key);
    const bool pressed = impl_->keyDown[i];
    impl_->keyDown[i] = false;
    return pressed;
}

bool Window::rawKeyDown(int glfwKey) const {
    if (!valid()) return false;
    return glfwGetKey(impl_->handle, glfwKey) == GLFW_PRESS;
}

// M-C1：键盘 / 字符事件消费。队列上限 = 调用方容量，溢出丢弃最旧（打字速率下
// 一帧 16 键 / 32 字符绰绰有余；真溢出说明主循环卡了 N 帧，丢输入好过卡死）。
std::size_t Window::consumeKeyEvents(KeyEvent* out, std::size_t cap) {
    if (!valid() || cap == 0) { if (valid()) impl_->keyEvents.clear(); return 0; }
    std::size_t n = 0;
    for (std::size_t i = 0; i < impl_->keyEvents.size() && n < cap; ++i) out[n++] = impl_->keyEvents[i];
    impl_->keyEvents.clear();
    return n;
}

std::size_t Window::consumeTextChars(unsigned int* out, std::size_t cap) {
    if (!valid() || cap == 0) { if (valid()) impl_->textChars.clear(); return 0; }
    std::size_t n = 0;
    for (std::size_t i = 0; i < impl_->textChars.size() && n < cap; ++i) out[n++] = impl_->textChars[i];
    impl_->textChars.clear();
    return n;
}

// P8 M-I0（增量扩展）：鼠标按键事件消费（消费制，节奏同 consumeKeyEvents）。
std::size_t Window::consumeMouseButtonEvents(MouseButtonEvent* out, std::size_t cap) {
    if (!valid() || cap == 0) { if (valid()) impl_->mouseButtonEvents.clear(); return 0; }
    std::size_t n = 0;
    for (std::size_t i = 0; i < impl_->mouseButtonEvents.size() && n < cap; ++i) out[n++] = impl_->mouseButtonEvents[i];
    impl_->mouseButtonEvents.clear();
    return n;
}

void Window::consumeMouse(float& deltaX, float& deltaY, float& scrollDelta) {
    deltaX = deltaY = scrollDelta = 0.0f;
    if (!valid()) return;
    deltaX = static_cast<float>(impl_->mouseDX);
    deltaY = static_cast<float>(impl_->mouseDY);
    scrollDelta = static_cast<float>(impl_->scrollY);
    impl_->mouseDX = impl_->mouseDY = impl_->scrollY = 0.0;
}

bool Window::leftButtonDown() const {
    return valid() && impl_->leftDown;
}

void Window::mousePosition(float& x, float& y) const {
    x = y = 0.0f;
    if (!valid() || !impl_->haveCursor) return;
    x = static_cast<float>(impl_->lastCursorX);
    y = static_cast<float>(impl_->lastCursorY);
}

// M-I3 指针锁定：GLFW_CURSOR_DISABLED = 光标隐藏 + 原生捕获，光标回调持续吐
// 无界虚拟坐标（consumeMouse 相对增量语义不变）；NORMAL = 恢复。CapturePermanently
// 语义（UE GameViewportClient::SetMouseCaptureMode 口径，P8-INPUT-PLAN §7.1 实读留痕）。
void Window::setMouseCaptured(bool on) {
    if (!valid()) return;
    glfwSetInputMode(impl_->handle, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

bool Window::mouseCaptured() const {
    if (!valid()) return false;
    return glfwGetInputMode(impl_->handle, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
}

// M-I5：文件拖拽消费（读一次即清）+ 测试注入口（--drop-sim 与真实拖拽同一队列）。
std::vector<std::string> Window::consumeDroppedPaths() {
    if (!valid()) return {};
    return std::exchange(impl_->droppedPaths, {});
}

void Window::feedDroppedPath(const std::string& path) {
    if (valid()) impl_->droppedPaths.push_back(path);
}

void* Window::nativeHandle() const {
#if defined(_WIN32)
    if (valid()) return reinterpret_cast<void*>(glfwGetWin32Window(impl_->handle));
#endif
    return nullptr;
}

Extent Window::framebufferSize() const {
    Extent out{};
    if (valid()) glfwGetFramebufferSize(impl_->handle, &out.width, &out.height);
    return out;
}

bool Window::consumeResize(Extent& out) {
    if (!valid() || impl_->pendingWidth <= 0 || impl_->pendingHeight <= 0) return false;
    out = { impl_->pendingWidth, impl_->pendingHeight };
    impl_->pendingWidth = impl_->pendingHeight = 0;
    return true;
}

bool windowSelfTest() {
    WindowDesc desc;
    if (desc.width <= 0 || desc.height <= 0) return false;
    if (desc.title.empty()) return false;
    if (!desc.resizable) return false; // resize 是每阶段回归项，默认必须可缩放
    return true;
}

} // namespace nl::core
