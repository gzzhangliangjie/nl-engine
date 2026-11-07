#include "nl/core/window.hpp"
#include "nl/core/log.hpp"

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#endif

#include <array>

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

    void onKey(int glfwKey, int action) {
        if (action != GLFW_PRESS) return;
        const Key key = mapKey(glfwKey);
        if (key != Key::Unknown) keyDown[static_cast<size_t>(key)] = true;
    }

    static void keyCallback(GLFWwindow* handle, int key, int /*scancode*/, int action, int /*mods*/) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            impl->onKey(key, action);
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
        }
    }

    static void scrollCallback(GLFWwindow* handle, double /*x*/, double y) {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(handle))) {
            impl->scrollY += y;
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

    impl_->handle = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
    if (!impl_->handle) {
        log::error("glfwCreateWindow failed (", desc.width, "x", desc.height, ")");
        glfwTerminate();
        return;
    }

    glfwSetWindowUserPointer(impl_->handle, impl_.get());
    glfwSetKeyCallback(impl_->handle, &Impl::keyCallback);
    glfwSetFramebufferSizeCallback(impl_->handle, &Impl::framebufferCallback);
    glfwSetCursorPosCallback(impl_->handle, &Impl::cursorCallback);
    glfwSetMouseButtonCallback(impl_->handle, &Impl::buttonCallback);
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
