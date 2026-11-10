#pragma once

// P8 M-I0：输入状态层（A 类轨道，docs/p8/P8-INPUT-PLAN.md）。
//
// 口径来源（三处对照，P8-INPUT-PLAN.md §2 实读留痕）：
//   - bevy bevy_input（ButtonInput 节奏锚）：pressed / just_pressed / just_released
//     三态；事件批量进、边沿帧末统一清；状态表=定长数组，不做查询缓存。
//   - Carrot io/actions（映射层锚，M-I1 用）：绑定解析 Bool=OR / Float=max abs；
//     ActionSet 预声明、poll 后冻结。本文件只做状态层，映射层见 M-I1。
//   - UE EnhancedInput（只取思想）：映射上下文可注册/移除/分层；Modifier/Trigger
//     装饰器链不做（最小必要方案）。
//
// 键位口径：直接用 GLFW 键码（平台无关物理键位，GLFW_KEY_LAST 定表长）——
// 不学 bevy 自造 W3C 枚举（再造一层只增加映射表维护成本，见 P8-INPUT-PLAN §3）。
//
// 主循环节奏（bevy 口径，游戏壳接线时执行）：
//   window.pollEvents() -> input.updateFromWindow(window)
//   -> 游戏读状态 -> input.endFrame()
// 边沿（justPressed/justReleased）与增量（鼠标 delta / 滚轮）只在 endFrame 清。
//
// ⚠️ 单消费者铁律：updateFromWindow 会消费 Window 的键盘事件队列——
// viewer 现有 ImGui 通道（consumeKeyEvents）是同一个队列，**在游戏壳接线前
// viewer 绝不允许调本接口**（否则抢走 ImGui 的输入）。M-I0 只走注入路径验收。
//
// 确定性契约（selftest 锚）：注入路径（feedKey/feedMouseButton/...）与窗口路径
// （updateFromWindow）走同一更新内核 _applyKey/_applyMouseButton——合成事件序列
// 数值锚逐位可复现（phys/audio 先例口径）。core 零图形 API 纪律不变。
//
// M-I0 有意不做（P8-INPUT-PLAN §4 留痕）：手柄/触摸、滚动 X 轴（GLFW 有但
// window 底座只累积 Y，M-I0 保持）、Modifier/Trigger 链、运行时重绑定 UI。

#include <cstddef>
#include <cstdint>

#include "nl/core/window.hpp"

// GLFW 键位常量：经 window.hpp 的 pimpl 约束，GLFW 头允许在 core 内出现
// （window.cpp / console_ui.cpp 先例），但不得泄漏进公开 API 类型。
#include <GLFW/glfw3.h>

namespace nl::core {

// M-I0 键位表长锚：GLFW_KEY_LAST 是 GLFW 版本ABI的一部分，selftest 钉死
// 具体键码数值（GLFW_KEY_W==87 等），上游 bump 撞表长会在锚上炸出来。
inline constexpr int kGlfwKeyTableSize = GLFW_KEY_LAST + 1;
inline constexpr int kGlfwMouseButtonTableSize = GLFW_MOUSE_BUTTON_LAST + 1;

// 轻量 2D 向量（core 未引 GLM——M0.4 起手写数学库口径沿用）。
struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

// 输入状态表：三态（isDown / justPressed / justReleased）+ 鼠标全量。
// 帧节奏见文件头；所有 feed* / updateFromWindow 只在帧首调用，endFrame 帧末调用。
class InputState {
public:
    InputState() = default;

    // ---- 窗口路径（游戏壳专用；单消费者铁律见文件头）----
    // 消费 Window 的键盘/鼠标事件队列 + 鼠标按键事件 + 光标/滚轮/拖动增量。
    void updateFromWindow(Window& win);

    // ---- 注入路径（selftest / 无窗口逻辑测试；与窗口路径同内核）----
    // 越界键码（<0 或 >GLFW_KEY_LAST）显式忽略——防御性丢弃，不崩不告警
    //（selftest 有专门锚验证此行为）。
    void feedKey(int glfwKey, bool down);
    void feedMouseButton(int button, bool down);
    void feedMouseDelta(float dx, float dy);
    void feedScroll(float dx, float dy);
    void feedCursor(float x, float y, bool hasCursor = true);
    // 修饰键注入（selftest 对称入口；窗口路径由 KeyEvent.ctrl/shift/alt 维护）。
    void feedMods(bool ctrl, bool shift, bool alt);

    // ---- 查询（键码用 GLFW 键码常量；越界 = false）----
    bool isDown(int glfwKey) const;
    bool justPressed(int glfwKey) const;
    bool justReleased(int glfwKey) const;

    bool mouseIsDown(int button) const;
    bool mouseJustPressed(int button) const;
    bool mouseJustReleased(int button) const;

    Vec2f mouseDelta() const { return mouseDelta_; }
    Vec2f scrollDelta() const { return scrollDelta_; }
    Vec2f cursorPosition() const { return cursor_; }
    bool  hasCursor() const { return hasCursor_; }

    // 修饰键：取最近一次带 mods 的事件状态（feedMods / 窗口事件共同维护）。
    bool ctrlDown() const { return ctrl_; }
    bool shiftDown() const { return shift_; }
    bool altDown() const { return alt_; }

    // ---- 帧末：清边沿 + 清增量（持续态 isDown 不动，bevy ButtonInput 口径）----
    void endFrame();

private:
    void applyKey(int glfwKey, bool down);
    void applyMouseButton(int button, bool down);

    // 定长状态表（bevy ButtonInput 口径：数组查询 O(1)，50 物体规模不做缓存）。
    // 三个数组三态：down 持续态跨帧保留；pressed/released 是本帧边沿，endFrame 清。
    bool keyDown_[kGlfwKeyTableSize]{};
    bool keyPressed_[kGlfwKeyTableSize]{};
    bool keyReleased_[kGlfwKeyTableSize]{};
    bool mouseDown_[kGlfwMouseButtonTableSize]{};
    bool mousePressed_[kGlfwMouseButtonTableSize]{};
    bool mouseReleased_[kGlfwMouseButtonTableSize]{};

    Vec2f mouseDelta_{};
    Vec2f scrollDelta_{};
    Vec2f cursor_{};
    bool  hasCursor_ = false;
    bool  ctrl_ = false;
    bool  shift_ = false;
    bool  alt_ = false;
};

} // namespace nl::core
