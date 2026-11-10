// P8 M-I0：输入状态层实现（口径与契约见 input.hpp 文件头）。
// 关键不变量：注入路径与窗口路径必须走同一 applyKey/applyMouseButton 内核——
// selftest 的数值锚只对内核背书，窗口路径接线（游戏壳里程碑）后自动继承。

#include "nl/core/input.hpp"

#include <algorithm>

namespace nl::core {

namespace {
// 越界判定统一走这一处（防御性丢弃：selftest 锚验证不崩不误伤合法键）。
inline bool keyInTable(int key)  { return key >= 0 && key < kGlfwKeyTableSize; }
inline bool mouseInTable(int b)  { return b >= 0 && b < kGlfwMouseButtonTableSize; }
} // namespace

// ---- 注入路径 ----

void InputState::feedKey(int glfwKey, bool down) {
    if (!keyInTable(glfwKey)) return;
    applyKey(glfwKey, down);
}

void InputState::feedMouseButton(int button, bool down) {
    if (!mouseInTable(button)) return;
    applyMouseButton(button, down);
}

void InputState::feedMouseDelta(float dx, float dy) {
    mouseDelta_.x += dx;
    mouseDelta_.y += dy;
}

void InputState::feedScroll(float dx, float dy) {
    scrollDelta_.x += dx;
    scrollDelta_.y += dy;
}

void InputState::feedCursor(float x, float y, bool hasCursor) {
    cursor_ = {x, y};
    hasCursor_ = hasCursor;
}

void InputState::feedMods(bool ctrl, bool shift, bool alt) {
    ctrl_ = ctrl;
    shift_ = shift;
    alt_ = alt;
}

// ---- 窗口路径（游戏壳接线前禁用，单消费者铁律见 input.hpp 文件头）----

void InputState::updateFromWindow(Window& win) {
    // 键盘事件：consumeKeyEvents 是消费制队列（viewer 的 ImGui 通道同一数据源），
    // 调用方成为唯一消费者——见 input.hpp 铁律注释。
    KeyEvent keyEvents[64];
    if (const std::size_t n = win.consumeKeyEvents(keyEvents, 64)) {
        for (std::size_t i = 0; i < n; ++i) {
            applyKey(keyEvents[i].key, keyEvents[i].down);
            ctrl_  = keyEvents[i].ctrl;
            shift_ = keyEvents[i].shift;
            alt_   = keyEvents[i].alt;
        }
    }
    // 鼠标按键事件（M-I0 增量扩展的 consumeMouseButtonEvents）。
    MouseButtonEvent mouseEvents[16];
    if (const std::size_t n = win.consumeMouseButtonEvents(mouseEvents, 16)) {
        for (std::size_t i = 0; i < n; ++i) applyMouseButton(mouseEvents[i].button, mouseEvents[i].down);
    }
    // 拖动增量 / 滚轮（window 消费制，读一次即清）。
    float dx = 0, dy = 0, scroll = 0;
    win.consumeMouse(dx, dy, scroll);
    feedMouseDelta(dx, dy);
    feedScroll(0.0f, scroll);   // M-I0 范围：window 底座只累积 Y（见 input.hpp 文件头）
    // 光标绝对坐标（HUD 口径：无事件帧也要有稳定值）。
    float cx = 0, cy = 0;
    win.mousePosition(cx, cy);
    feedCursor(cx, cy, true);
}

// ---- 更新内核（唯一写入点；注入与窗口两路共用）----

void InputState::applyKey(int glfwKey, bool down) {
    if (!keyInTable(glfwKey)) return;
    const auto i = static_cast<std::size_t>(glfwKey);
    if (down) {
        // bevy ButtonInput 口径：REPEAT 事件不重触发 justPressed（down 已真则只续持续态）。
        if (!keyDown_[i]) keyPressed_[i] = true;
        keyDown_[i] = true;
        keyReleased_[i] = false;    // 同帧先 release 后 press：release 边沿被后续 press 抹掉
    } else {
        if (keyDown_[i]) keyReleased_[i] = true;
        keyDown_[i] = false;
        keyPressed_[i] = false;     // 同帧先 press 后 release：press 边沿被后续 release 抹掉
    }
}

void InputState::applyMouseButton(int button, bool down) {
    if (!mouseInTable(button)) return;
    const auto i = static_cast<std::size_t>(button);
    if (down) {
        if (!mouseDown_[i]) mousePressed_[i] = true;
        mouseDown_[i] = true;
        mouseReleased_[i] = false;
    } else {
        if (mouseDown_[i]) mouseReleased_[i] = true;
        mouseDown_[i] = false;
        mousePressed_[i] = false;
    }
}

// ---- 查询 ----

bool InputState::isDown(int glfwKey) const {
    return keyInTable(glfwKey) && keyDown_[static_cast<std::size_t>(glfwKey)];
}
bool InputState::justPressed(int glfwKey) const {
    return keyInTable(glfwKey) && keyPressed_[static_cast<std::size_t>(glfwKey)];
}
bool InputState::justReleased(int glfwKey) const {
    return keyInTable(glfwKey) && keyReleased_[static_cast<std::size_t>(glfwKey)];
}
bool InputState::mouseIsDown(int button) const {
    return mouseInTable(button) && mouseDown_[static_cast<std::size_t>(button)];
}
bool InputState::mouseJustPressed(int button) const {
    return mouseInTable(button) && mousePressed_[static_cast<std::size_t>(button)];
}
bool InputState::mouseJustReleased(int button) const {
    return mouseInTable(button) && mouseReleased_[static_cast<std::size_t>(button)];
}

// ---- 帧末 ----

void InputState::endFrame() {
    std::fill(std::begin(keyPressed_), std::end(keyPressed_), false);
    std::fill(std::begin(keyReleased_), std::end(keyReleased_), false);
    std::fill(std::begin(mousePressed_), std::end(mousePressed_), false);
    std::fill(std::begin(mouseReleased_), std::end(mouseReleased_), false);
    mouseDelta_ = {};
    scrollDelta_ = {};
    // 持续态（keyDown_/mouseDown_/cursor_/mods）跨帧保留——bevy 口径。
}

} // namespace nl::core
