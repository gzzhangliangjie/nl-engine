#pragma once

// P8 M-I1：ActionMap / ActionSet 映射层（A 类轨道，docs/p8/P8-INPUT-PLAN.md §M-I1）。
//
// 口径来源（三处对照，2026-11-10 本轮重读留痕）：
//   - Carrot io/actions（本轮主证据）：README 明文解析规则「Bool = OR of all
//     inputs / Float = Result with the largest absolute value」；ActionSet.cpp
//     实证 Bool←Float 阈值 >= 0.5f、Float←Bool 1.0/0.0、非激活组回调直接
//     return、updatePrePoll 先 previous=value 再清零（poll 后冻结的 pull 模型
//     对应本类 update() 一次搬移+重解析）。
//   - bevy bevy_input：无映射层（ButtonInput 纯状态表）——映射层自研口径由
//     Carrot 提供，bevy 只贡献了 M-I0 的三态节奏。
//   - UE EnhancedInput：InputMappingContext 的 modifier/trigger 装饰器链、
//     上下文优先级叠加不做（P8-INPUT-PLAN §3 最小必要方案留痕）。
//
// 本层设计（对照 Carrot 的落点）：
//   - Action 三型 Bool/Float/Vec2；value + previousValue 双值（Carrot
//     InputState union 口径），Bool 边沿 justPressed/justReleased =
//     Carrot wasJustPressed/wasJustReleased 语义。
//   - 解析规则：Bool = 绑定 OR；Float = 各绑定带符号值取最大绝对值
//     （平局取先定义的绑定——严格大于比较，selftest 锚钉死）；键→Bool =
//     isDown；键→Float = 1.0/0.0 乘 scale；Float 型源（滚轮/鼠标增量轴）
//     →Bool 走 |v| >= 0.5f 阈值（Carrot gamepad 轴口径）。
//   - Vec2 = 各绑定按轴叠加（Carrot changeVec2Input additive 口径）。
//   - ActionSet = 位掩码组开关（kMaxSets=32）：动作可属多组，任一组激活即
//     参与 update；未挂组的动作恒参与。**组关停 = 清零不保旧值**（与 Carrot
//     「跳过不写」不同—— Carrot 是回调侧不写入、旧值冻结；我们选清零，
//     防止模式切换后旧动作值漏进新模式的玩法逻辑。此处是有意偏离，
//     selftest 锚钉死，见 update() 注释）。
//
// 帧节奏（与 InputState 对齐，游戏壳接线时执行）：
//   feed/updateFromWindow -> actions.update(inputState) -> 游戏读动作值
//   -> inputState.endFrame()。update() 必须在 endFrame 之前调用（滚轮/
//   鼠标增量轴是 endFrame 清零的源）；update 后动作值冻结到下一次 update。
//
// 本头文件不含 GLFW（纯逻辑零 GLFW 调用；绑定里的键码只是 int，常量由
// 使用方传入——默认驾驶映射表 buildDrivingActionMap 在 input.hpp，那里
// 已有 GLFW include 面）。include input.hpp 只为复用 Vec2f。

#include <cstdint>
#include <vector>

#include "nl/core/input.hpp"

namespace nl::core {

class ActionMap {
public:
    using ActionId = std::uint32_t;
    using SetId    = std::uint32_t;
    static constexpr ActionId kInvalidAction = 0xFFFFFFFFu;
    static constexpr SetId    kInvalidSet    = 0xFFFFFFFFu;
    static constexpr int      kMaxSets       = 32;

    enum class Type : std::uint8_t { Bool, Float, Vec2 };

    // 绑定源：键 / 鼠标按键 = 布尔型源；滚轮 / 鼠标增量轴 = 浮点型源
    //（Carrot gamepad axis 的 Float 口径在我们这里只有这四个来源）。
    enum class Source : std::uint8_t { Key, MouseButton, ScrollX, ScrollY, DeltaX, DeltaY };

    struct Binding {
        Source source;
        int    index   = 0;      // Key/MouseButton：键码或鼠标键 id；轴源忽略
        float  scale   = 1.0f;   // 带符号倍率（W=+1 / S=-1 这类轴绑定的符号面）
        int    vecAxis = -1;     // Vec2 动作专用：0=x 1=y；Bool/Float 动作忽略
    };

    // ---- 声明（Carrot「预声明」口径：动作/组先定义后绑定，运行期不增删）----
    ActionId define(const char* name, Type type);
    ActionId find(const char* name) const;        // 未命中 = kInvalidAction
    const char* name(ActionId id) const;          // 非法 id = nullptr
    Type type(ActionId id) const;

    SetId defineSet(const char* name);
    SetId findSet(const char* name) const;

    // ---- 绑定（同名动作重复绑定 = 追加，OR/max-abs 语义天然支持）----
    void bindKey(ActionId id, int glfwKey, float scale = 1.0f);
    void bindMouseButton(ActionId id, int button, float scale = 1.0f);
    void bindScroll(ActionId id, bool axisY, float scale = 1.0f);
    void bindMouseDelta(ActionId id, int axis, float scale = 1.0f);   // axis 0=x 1=y
    // Vec2 专用：布尔源按轴叠加（WASD→移动向量这类）。
    void bindAxisKey(ActionId id, int glfwKey, int axis, float scale = 1.0f);

    // ---- 组开关（ActionSet）----
    void setActive(SetId id, bool active);
    bool isActive(SetId id) const;
    // 挂组（可挂多组，位掩码累积）；不挂组的动作恒参与 update。
    void setMembership(ActionId id, SetId set);

    // ---- 每帧冻结（见文件头帧节奏）----
    void update(const InputState& in);

    // ---- 查询（冻结值；非法 id 返回零值）----
    bool  boolValue(ActionId id) const;
    bool  boolJustPressed(ActionId id) const;
    bool  boolJustReleased(ActionId id) const;
    float floatValue(ActionId id) const;
    float floatDelta(ActionId id) const;
    Vec2f vec2Value(ActionId id) const;
    Vec2f vec2Delta(ActionId id) const;

private:
    struct ActionDef {
        const char* name = nullptr;   // 指向调用方字符串字面量（预声明口径）
        Type type = Type::Bool;
        std::uint32_t setMask = 0;    // 0 = 未挂组恒参与
        std::vector<Binding> bindings;
        // 冻结态（update() 维护）
        bool  bValue = false, bPrev = false;
        float fValue = 0.0f, fPrev = 0.0f;
        Vec2f vValue{}, vPrev{};
    };

    float sourceFloat(const InputState& in, const Binding& b) const;
    void  resolveAction(ActionDef& a, const InputState& in);

    std::vector<ActionDef> actions_;
    std::vector<const char*> setNames_;
    std::uint32_t setActiveMask_ = 0;
};

// 默认驾驶映射表（P8-INPUT-PLAN §M-I1：C++ 常量落库，游戏壳直接用）。
// 定义在 input.hpp（本文件零 GLFW；键码常量由 GLFW 提供处使用）。
struct DrivingActions {
    ActionMap::SetId    drivingSet;   // 驾驶组：throttle/steer/handbrake/camToggle/look
    ActionMap::SetId    uiSet;        // UI 组：pause（演示组开关；UI 期驾驶组可关停）
    ActionMap::ActionId throttle;     // Float：W=+1 / S=-1（平局取先定义的 W）
    ActionMap::ActionId steer;        // Float：A=-1 / D=+1
    ActionMap::ActionId handbrake;    // Bool：Space
    ActionMap::ActionId camToggle;    // Bool：C（游戏侧用 justPressed 边沿）
    ActionMap::ActionId look;         // Vec2：鼠标增量轴（additive）
    ActionMap::ActionId pause;        // Bool：Escape（uiSet）
};

DrivingActions buildDrivingActionMap(ActionMap& map);

} // namespace nl::core
