// P8 M-I1：ActionMap 实现（解析规则与冻结语义见 action_map.hpp 文件头）。
// 纯逻辑零 GLFW 调用；一切键码/鼠标键 id 由绑定数据携带。

#include "nl/core/action_map.hpp"

#include <cmath>
#include <cstring>

namespace nl::core {

ActionMap::ActionId ActionMap::define(const char* name, Type type) {
    ActionDef a;
    a.name = name;
    a.type = type;
    actions_.push_back(a);
    return static_cast<ActionId>(actions_.size() - 1);
}

ActionMap::ActionId ActionMap::find(const char* name) const {
    for (std::size_t i = 0; i < actions_.size(); ++i)
        if (std::strcmp(actions_[i].name, name) == 0) return static_cast<ActionId>(i);
    return kInvalidAction;
}

const char* ActionMap::name(ActionId id) const {
    return id < actions_.size() ? actions_[id].name : nullptr;
}

ActionMap::Type ActionMap::type(ActionId id) const {
    return id < actions_.size() ? actions_[id].type : Type::Bool;
}

ActionMap::SetId ActionMap::defineSet(const char* name) {
    setNames_.push_back(name);
    return static_cast<SetId>(setNames_.size() - 1);
}

ActionMap::SetId ActionMap::findSet(const char* name) const {
    for (std::size_t i = 0; i < setNames_.size(); ++i)
        if (std::strcmp(setNames_[i], name) == 0) return static_cast<SetId>(i);
    return kInvalidSet;
}

void ActionMap::bindKey(ActionId id, int glfwKey, float scale) {
    if (id >= actions_.size()) return;
    Binding b;
    b.source = Source::Key;
    b.index = glfwKey;
    b.scale = scale;
    actions_[id].bindings.push_back(b);
}

void ActionMap::bindMouseButton(ActionId id, int button, float scale) {
    if (id >= actions_.size()) return;
    Binding b;
    b.source = Source::MouseButton;
    b.index = button;
    b.scale = scale;
    actions_[id].bindings.push_back(b);
}

void ActionMap::bindScroll(ActionId id, bool axisY, float scale) {
    if (id >= actions_.size()) return;
    Binding b;
    b.source = axisY ? Source::ScrollY : Source::ScrollX;
    b.scale = scale;
    actions_[id].bindings.push_back(b);
}

void ActionMap::bindMouseDelta(ActionId id, int axis, float scale) {
    if (id >= actions_.size()) return;
    Binding b;
    b.source = (axis == 0) ? Source::DeltaX : Source::DeltaY;
    b.scale = scale;
    b.vecAxis = axis;
    actions_[id].bindings.push_back(b);
}

void ActionMap::bindAxisKey(ActionId id, int glfwKey, int axis, float scale) {
    if (id >= actions_.size()) return;
    Binding b;
    b.source = Source::Key;
    b.index = glfwKey;
    b.scale = scale;
    b.vecAxis = axis;
    actions_[id].bindings.push_back(b);
}

void ActionMap::setActive(SetId id, bool active) {
    if (id >= setNames_.size()) return;
    const std::uint32_t bit = 1u << id;
    if (active) setActiveMask_ |= bit;
    else        setActiveMask_ &= ~bit;
}

bool ActionMap::isActive(SetId id) const {
    if (id >= setNames_.size()) return false;
    return (setActiveMask_ & (1u << id)) != 0;
}

void ActionMap::setMembership(ActionId id, SetId set) {
    if (id >= actions_.size() || set >= setNames_.size()) return;
    actions_[id].setMask |= (1u << set);
}

// 绑定源的浮点值（含 scale）：布尔源 = down ? 1 : 0；浮点源 = 增量/滚轮。
// 输入状态表越界查询返回 false/0（InputState 已防御），这里不再重复判界。
float ActionMap::sourceFloat(const InputState& in, const Binding& b) const {
    float v = 0.0f;
    switch (b.source) {
        case Source::Key:        v = in.isDown(b.index) ? 1.0f : 0.0f; break;
        case Source::MouseButton:v = in.mouseIsDown(b.index) ? 1.0f : 0.0f; break;
        case Source::ScrollX:    v = in.scrollDelta().x; break;
        case Source::ScrollY:    v = in.scrollDelta().y; break;
        case Source::DeltaX:     v = in.mouseDelta().x; break;
        case Source::DeltaY:     v = in.mouseDelta().y; break;
    }
    return v * b.scale;
}

void ActionMap::resolveAction(ActionDef& a, const InputState& in) {
    // Bool：绑定 OR（Carrot README 口径）；布尔源不吃 scale（恒 down ? 1 : 0，
    // 防「负 scale 键绑定恒 false」误用）；浮点源走 |v| >= 0.5f 阈值
    //（Carrot ActionSet.cpp gamepad 轴口径）。
    if (a.type == Type::Bool) {
        bool v = false;
        for (const Binding& b : a.bindings) {
            if (b.source == Source::Key)
                v = v || in.isDown(b.index);
            else if (b.source == Source::MouseButton)
                v = v || in.mouseIsDown(b.index);
            else
                v = v || (std::fabs(sourceFloat(in, b)) >= 0.5f);
        }
        a.bPrev  = a.bValue;
        a.bValue = v;
        return;
    }
    // Float：各绑定带符号值取最大绝对值（Carrot README 口径）；
    // 平局取先定义的绑定（严格大于比较，selftest 锚钉死）。
    if (a.type == Type::Float) {
        float best = 0.0f;
        bool  have = false;
        for (const Binding& b : a.bindings) {
            const float v = sourceFloat(in, b);
            if (!have || std::fabs(v) > std::fabs(best)) { best = v; have = true; }
        }
        a.fPrev  = a.fValue;
        a.fValue = best;
        return;
    }
    // Vec2：绑定按轴叠加（Carrot changeVec2Input additive 口径）；
    // vecAxis < 0 的绑定对本型无意义，防御性跳过。
    Vec2f sum{};
    for (const Binding& b : a.bindings) {
        if (b.vecAxis < 0) continue;
        const float v = sourceFloat(in, b);
        if (b.vecAxis == 0) sum.x += v;
        else                sum.y += v;
    }
    a.vPrev  = a.vValue;
    a.vValue = sum;
}

void ActionMap::update(const InputState& in) {
    for (ActionDef& a : actions_) {
        // 组关停 = 清零不保旧值（与 Carrot「跳过不写、旧值冻结」的有意偏离，
        // 防止模式切换后旧动作值漏进新模式；action_map.hpp 文件头留痕）。
        const bool participates = (a.setMask == 0) || (a.setMask & setActiveMask_) != 0;
        if (!participates) {
            a.bPrev = a.bValue = false;
            a.fPrev = a.fValue = 0.0f;
            a.vPrev = a.vValue = Vec2f{};
            continue;
        }
        resolveAction(a, in);
    }
}

bool ActionMap::boolValue(ActionId id) const {
    return id < actions_.size() ? actions_[id].bValue : false;
}

bool ActionMap::boolJustPressed(ActionId id) const {
    return id < actions_.size() ? (actions_[id].bValue && !actions_[id].bPrev) : false;
}

bool ActionMap::boolJustReleased(ActionId id) const {
    return id < actions_.size() ? (!actions_[id].bValue && actions_[id].bPrev) : false;
}

float ActionMap::floatValue(ActionId id) const {
    return id < actions_.size() ? actions_[id].fValue : 0.0f;
}

float ActionMap::floatDelta(ActionId id) const {
    return id < actions_.size() ? (actions_[id].fValue - actions_[id].fPrev) : 0.0f;
}

Vec2f ActionMap::vec2Value(ActionId id) const {
    return id < actions_.size() ? actions_[id].vValue : Vec2f{};
}

Vec2f ActionMap::vec2Delta(ActionId id) const {
    if (id >= actions_.size()) return Vec2f{};
    const ActionDef& a = actions_[id];
    return Vec2f{a.vValue.x - a.vPrev.x, a.vValue.y - a.vPrev.y};
}

} // namespace nl::core
