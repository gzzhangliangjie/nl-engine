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
    log::info("window created ", desc.width, "x", desc.height,
              " | no GL context | ESC to quit, SPACE color, F12 screenshot");
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
