// P8 M-I1：ActionMap 实现（解析规则与冻结语义见 action_map.hpp 文件头）。
// 纯逻辑零 GLFW 调用；一切键码/鼠标键 id 由绑定数据携带。

#include "nl/core/action_map.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <json.hpp>

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

// ---- M-I5 手柄绑定 ----

void ActionMap::bindGamepadButton(ActionId id, int button, float scale) {
    if (id >= actions_.size()) return;
    Binding b;
    b.source = Source::GamepadButton;
    b.index = button;
    b.scale = scale;
    actions_[id].bindings.push_back(b);
}

void ActionMap::bindGamepadAxis(ActionId id, int axis, float scale) {
    if (id >= actions_.size()) return;
    Binding b;
    b.source = Source::GamepadAxis;
    b.index = axis;
    b.scale = scale;
    actions_[id].bindings.push_back(b);
}

// ---- M-I5 运行时重绑定（有意偏离 Carrot「poll 后冻结」，P8-INPUT-PLAN §7.3）----

bool ActionMap::rebind(ActionId id, const std::vector<Binding>& bindings) {
    if (id >= actions_.size()) return false;
    actions_[id].bindings = bindings;
    return true;   // 生效点 = 下一次 update()
}

// ---- M-I5 映射文件（vendored nlohmann json；scene 序列化同款依赖，零新依赖）----
// 全量校验后一次性应用：任何解析/校验错误返回 false + outError 原因，map 不动。
bool ActionMap::loadFromFile(const char* path, std::string& outError) {
    outError.clear();
    if (!path || !*path) { outError = "path empty"; return false; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { outError = std::string("open failed: ") + path; return false; }
    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e) { outError = std::string("parse: ") + e.what(); return false; }
    if (!j.contains("actions") || !j["actions"].is_array()) {
        outError = "missing \"actions\" array"; return false;
    }

    // 先全量解析到临时结构（Pending），全部合法才提交——错误中途不污染 map。
    struct Pending {
        std::string name;
        Type type = Type::Bool;
        std::uint32_t setMask = 0;
        std::vector<Binding> bindings;
    };
    std::vector<std::string> pendingSets;
    if (j.contains("sets")) {
        if (!j["sets"].is_array()) { outError = "\"sets\" must be array"; return false; }
        for (const auto& s : j["sets"]) {
            if (!s.is_string()) { outError = "\"sets\" entry must be string"; return false; }
            pendingSets.push_back(s.get<std::string>());
        }
        if (pendingSets.size() > static_cast<std::size_t>(kMaxSets)) {
            outError = "sets exceed kMaxSets(32)"; return false;
        }
    }

    auto parseBinding = [&](const nlohmann::json& b, Binding& out, std::string& err) -> bool {
        if (!b.contains("src") || !b["src"].is_string()) { err = "bind missing \"src\""; return false; }
        const std::string src = b["src"].get<std::string>();
        const int id = b.value("id", 0);
        const float scale = static_cast<float>(b.value("scale", 1.0));
        if (src == "key")          { out.source = Source::Key; out.index = id; }
        else if (src == "mouse")   { out.source = Source::MouseButton; out.index = id; }
        else if (src == "scrollX") { out.source = Source::ScrollX; }
        else if (src == "scrollY") { out.source = Source::ScrollY; }
        else if (src == "deltaX")  { out.source = Source::DeltaX; }
        else if (src == "deltaY")  { out.source = Source::DeltaY; }
        else if (src == "pad")     { out.source = Source::GamepadButton; out.index = id; }
        else if (src == "padAxis") { out.source = Source::GamepadAxis; out.index = id; }
        else { err = "unknown src: " + src; return false; }
        out.scale = scale;
        out.vecAxis = b.value("axis", -1);
        return true;
    };

    std::vector<Pending> pending;
    for (const auto& a : j["actions"]) {
        Pending p;
        if (!a.contains("name") || !a["name"].is_string()) { outError = "action missing \"name\""; return false; }
        p.name = a["name"].get<std::string>();
        if (p.name.empty()) { outError = "action name empty"; return false; }
        const std::string ty = a.value("type", "bool");
        if      (ty == "bool")  p.type = Type::Bool;
        else if (ty == "float") p.type = Type::Float;
        else if (ty == "vec2")  p.type = Type::Vec2;
        else { outError = "unknown type: " + ty; return false; }
        if (a.contains("sets")) {
            if (!a["sets"].is_array()) { outError = "action \"sets\" must be array"; return false; }
            for (const auto& s : a["sets"]) {
                if (!s.is_string()) { outError = "action \"sets\" entry must be string"; return false; }
                const std::string sn = s.get<std::string>();
                std::uint32_t bit = 0;
                bool found = false;
                for (std::size_t i = 0; i < pendingSets.size(); ++i)
                    if (pendingSets[i] == sn) { bit = 1u << i; found = true; break; }
                if (!found) { outError = "action references unknown set: " + sn; return false; }
                p.setMask |= bit;
            }
        }
        if (!a.contains("binds") || !a["binds"].is_array()) { outError = "action missing \"binds\""; return false; }
        for (const auto& b : a["binds"]) {
            Binding bind;
            std::string err;
            if (!parseBinding(b, bind, err)) { outError = "action \"" + p.name + "\": " + err; return false; }
            p.bindings.push_back(bind);
        }
        pending.push_back(std::move(p));
    }

    // 提交：先组后动作（set 引用已在校验期闭合）。名字进 ownedNames_ 持有
    //（define 的 name 是调用方字面量裸指针口径——文件名必须活过 selftest 进程）。
    for (const auto& s : pendingSets) {
        ownedSetNames_.push_back(s);
        setNames_.push_back(ownedSetNames_.back().c_str());
    }
    for (auto& p : pending) {
        ownedNames_.push_back(std::move(p.name));
        const char* namePtr = ownedNames_.back().c_str();
        ActionId id = define(namePtr, p.type);
        actions_[id].setMask = p.setMask;
        actions_[id].bindings = std::move(p.bindings);
    }
    return true;
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
        case Source::GamepadButton: v = in.gamepadIsDown(b.index) ? 1.0f : 0.0f; break;  // M-I5
        case Source::GamepadAxis:   v = in.gamepadAxis(b.index); break;                  // M-I5（扳机 0..1 其余 -1..1）
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
