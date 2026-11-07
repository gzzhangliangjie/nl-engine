#pragma once

// M-A3：动作状态机（单层状态 + 按优先级转移 + crossfade）。
// 坐在 M-A1 采样 / M-A2 混合之上；蒙皮（2.3）就绪后 evaluate 输出直接刷骨骼。
//
// 口径来源（ACTION-PLAN §2.2/§2.5）：
//   - UE FAnimNode_StateMachine：转移规则按优先级排序、FindValidTransition 取
//     **首个**满足者（Private/Animation/AnimNode_StateMachine.cpp:741）；
//     TransitionToState 记录目标态既有权重（cpp:1335）。
//   - ⭐ UE 防跳变细节：crossfade 生效时长按目标态既有权重反向缩放——
//     effective = blendDuration * (1 - wNext)（StandardBlend 线性
//     CalculateInverseAlpha，cpp:56-70）。回退转移（A→B 半途 B→A）据此缩时，
//     姿态连续性由 lerp 对称性保证（swap source/target + alpha 取反）。
//   - bevy AnimationTransitions 的 fade-out（weight_decline_per_sec = 1/duration）
//     只作权重衰减节奏参照；其文档明说该临时 API 将被状态机取代，组织方式不抄。
//
// 简化声明（留痕，不做静默偏离）：
//   - 单活跃转移（UE 有 ActiveTransitionArray 栈支持同帧多次切换）：新转移
//     若目标是当前 fromState 则走回退缩时路径，否则丢弃旧源态贡献重新起混
//     （极端连续切换会有轻微权重跳变，逻辑层暂不需要栈式语义）。
//   - clip 循环采样（wrapTime）：one-shot 结束事件/自动规则留给蒙皮接入期。
//   - 转移当帧 alpha 从 0 起（下一帧 update 才开始推进）——确定性时序简单。
// 纪律：纯 CPU 确定性；零图形 API；条件为 std::function（给定 params 结果确定）。

#include "nl/anim/anim.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nl::anim {

using StateIndex = std::uint32_t;
using ParamValues = std::vector<float>;   // gameplay 输入按下标引用（如 [0]=speed）

struct StateDef {
    std::string name;
    AnimClip clip;
};

struct TransitionRule {
    StateIndex from = 0;
    StateIndex to = 0;
    std::function<bool(const ParamValues&)> canTake;   // 条件；空函数恒 false
    float blendDuration = 0.2f;                        // 秒（crossfade 线性）
};

struct StateMachineDesc {
    std::vector<StateDef> states;
    std::vector<TransitionRule> rules;   // 声明顺序 = 优先级（从高到低）
    StateIndex initialState = 0;
};

// 校验：空串合法；否则返回第一处错误（ASCII）。规则：
// states 非空、名字唯一非空、clip 全部过 validateClip、initialState 越界拒绝、
// 规则 from/to 越界或自环拒绝、blendDuration 有限且 >= 0、canTake 必须可调用。
std::string validateStateMachine(const StateMachineDesc& desc);

class AnimationStateMachine {
public:
    // 校验失败返回 false（内部保持空置，evaluate 返回 false）。
    bool reset(const StateMachineDesc& desc);

    // 推进 dt 秒：时间前进 -> 活跃 crossfade 衰减 -> 按优先级查当前态转移规则。
    // dt 非有限时按 0 处理（护栏）。
    void update(float dt, const ParamValues& params);

    // 当前混合姿态；未 reset 或骨架非法返回 false。
    bool evaluate(std::vector<BonePose>& out) const;

    StateIndex currentState() const { return current_; }
    const std::string& currentStateName() const { return desc_.states[current_].name; }

    // 测试/调试观测：crossfade 中目标态权重（未在转移中 = 1）。
    float transitionAlpha() const { return transitioning_ ? alpha_ : 1.0f; }
    // 当前 crossfade 生效时长（含 UE 缩时；未在转移中 = 0）。
    float transitionDuration() const { return transitioning_ ? blendDuration_ : 0.0f; }

private:
    void transitionTo(const TransitionRule& rule, float dt);

    StateMachineDesc desc_;
    bool valid_ = false;
    StateIndex current_ = 0;
    float stateTime_ = 0.0f;          // 目标态已播时间（秒）
    bool transitioning_ = false;
    StateIndex fromState_ = 0;
    float fromTime_ = 0.0f;           // 源态已播时间（crossfade 期间同步推进）
    float alpha_ = 1.0f;              // 目标态权重（0..1，线性）
    float blendDuration_ = 0.0f;      // 生效时长（UE 缩时后）
};

} // namespace nl::anim
