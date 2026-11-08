// M-A3：动作状态机实现（口径见 state_machine.hpp 文件头）。

#include "nl/anim/state_machine.hpp"

#include <cmath>
#include <set>
#include <utility>

namespace nl::anim {

std::string validateStateMachine(const StateMachineDesc& desc) {
    if (desc.states.empty()) return "statemachine: no states";
    std::set<std::string> names;
    for (const StateDef& state : desc.states) {
        if (state.name.empty()) return "statemachine: empty state name";
        if (!names.insert(state.name).second) return "statemachine: duplicate state name";
        const std::string clipError = validateClip(state.clip);
        if (!clipError.empty()) return "statemachine: invalid clip in state " + state.name;
    }
    if (desc.initialState >= desc.states.size())
        return "statemachine: initialState out of range";
    for (const TransitionRule& rule : desc.rules) {
        if (rule.from >= desc.states.size() || rule.to >= desc.states.size())
            return "statemachine: transition rule references out-of-range state";
        if (rule.from == rule.to) return "statemachine: transition self-loop";
        if (!std::isfinite(rule.blendDuration) || rule.blendDuration < 0.0f)
            return "statemachine: blendDuration must be finite >= 0";
        if (!rule.canTake) return "statemachine: transition without condition";
    }
    return {};
}

bool AnimationStateMachine::reset(const StateMachineDesc& desc) {
    if (!validateStateMachine(desc).empty()) {
        valid_ = false;
        return false;
    }
    desc_ = desc;
    valid_ = true;
    current_ = desc.initialState;
    stateTime_ = 0.0f;
    transitioning_ = false;
    fromState_ = desc.initialState;
    fromTime_ = 0.0f;
    alpha_ = 1.0f;
    blendDuration_ = 0.0f;
    return true;
}

void AnimationStateMachine::transitionTo(const TransitionRule& rule, float dt) {
    (void)dt;   // 转移当帧 alpha 从 0 起（文件头「简化声明」），dt 暂不参与
    if (rule.to == current_) return;
    if (transitioning_ && rule.to == fromState_) {
        // 回退转移：source/target 互换 + alpha 取反 = 姿态连续（lerp 对称性）
        // UE 防跳变：生效时长按目标态既有权重缩放（线性：effective = D * (1 - wNext)）
        std::swap(current_, fromState_);
        std::swap(stateTime_, fromTime_);
        alpha_ = 1.0f - alpha_;
        blendDuration_ = rule.blendDuration * (1.0f - alpha_);
        if (blendDuration_ <= 0.0f) transitioning_ = false;   // 即刻完成
        return;
    }
    fromState_ = current_;
    fromTime_ = stateTime_;
    current_ = rule.to;
    stateTime_ = 0.0f;
    alpha_ = 0.0f;
    blendDuration_ = rule.blendDuration;
    transitioning_ = blendDuration_ > 0.0f;
}

void AnimationStateMachine::update(float dt, const ParamValues& params) {
    if (!valid_) return;
    if (!std::isfinite(dt) || dt <= 0.0f) dt = 0.0f;
    stateTime_ += dt;
    if (transitioning_) {
        fromTime_ += dt;
        alpha_ += dt / blendDuration_;
        if (alpha_ >= 1.0f) {
            alpha_ = 1.0f;
            transitioning_ = false;
        }
    }
    // FindValidTransition：按声明优先级取首个满足者（UE 同款）
    for (const TransitionRule& rule : desc_.rules) {
        if (rule.from != current_) continue;
        if (rule.canTake && rule.canTake(params)) {
            transitionTo(rule, dt);
            break;
        }
    }
}

bool AnimationStateMachine::evaluate(std::vector<BonePose>& out) const {
    if (!valid_) return false;
    const AnimClip& targetClip = desc_.states[current_].clip;
    const float targetTime = wrapTime(stateTime_, targetClip.duration);
    if (!sampleClip(targetClip, targetTime, out)) return false;
    if (!transitioning_) return true;
    std::vector<BonePose> from;
    const AnimClip& fromClip = desc_.states[fromState_].clip;
    if (!sampleClip(fromClip, wrapTime(fromTime_, fromClip.duration), from)) return false;
    std::vector<BonePose> blended;   // 输入输出禁同容器（blendPoses 先 clear）
    if (!blendPoses(from, out, alpha_, blended)) return false;
    out = std::move(blended);
    return true;
}

} // namespace nl::anim
