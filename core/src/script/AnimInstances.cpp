// M-W2A Animation 域薄面句柄表实现（口径见 AnimInstances.hpp 头注；冻结口径
// WAVE2-PLAN §5.3）。nl/anim 公开面零改动：状态转移的 canTake lambda 在本文件
// 内部构造（std::function 不跨绑定边界）。
#include "AnimInstances.hpp"

#include "nl/anim/gltf_anim.hpp"

#include <algorithm>
#include <cmath>

namespace nl::script {

namespace {

// 单源发号器：clip 与 sm 共用（都 >0；0 = 无效句柄哨兵）。
int g_nextAnimHandle = 1;
std::unordered_map<int, AnimClipEntry> g_clips;
std::unordered_map<int, AnimSmEntry>   g_sms;

} // namespace

int animClipLoad(const std::string& resolvedPath, int clipIndex,
                 int* outClipId, std::string* err) {
    if (outClipId) *outClipId = 0;
    // 纯逻辑层文件入口（M-A4b）；失败 error 带英文描述（ASCII）。
    nl::anim::Skeleton skeleton;   // clip 域不持骨架（pose 尺寸 = clip 轨道数）
    std::vector<nl::anim::AnimClip> clips;
    std::string loadErr;
    if (!nl::anim::loadGltfAnim(resolvedPath, skeleton, clips, loadErr)) {
        if (err) *err = "loadGltfAnim failed: " + loadErr;
        return kAnimLoad;
    }
    if (clipIndex < 0 || static_cast<std::size_t>(clipIndex) >= clips.size()) {
        if (err) *err = "clipIndex out of range (0.." + std::to_string(clips.size() - 1) + ")";
        return kAnimBadArg;
    }
    std::string clipErr = nl::anim::validateClip(clips[static_cast<std::size_t>(clipIndex)]);
    if (!clipErr.empty()) {
        if (err) *err = "validateClip: " + clipErr;
        return kAnimLoad;
    }
    const int id = g_nextAnimHandle++;
    AnimClipEntry e;
    e.clip = std::move(clips[static_cast<std::size_t>(clipIndex)]);
    e.path = resolvedPath;
    e.clipIndex = clipIndex;
    g_clips.emplace(id, std::move(e));
    if (outClipId) *outClipId = id;
    return kAnimOk;
}

int animSmCreate(const int* clipIds, int clipCount,
                 const int* ruleMeta, const float* ruleVals, int ruleCount,
                 int* outSmId, std::string* err) {
    if (outSmId) *outSmId = 0;
    if (clipCount <= 0 || clipIds == nullptr || ruleCount < 0 ||
        (ruleCount > 0 && (ruleMeta == nullptr || ruleVals == nullptr))) {
        return kAnimBadArg;
    }
    nl::anim::StateMachineDesc desc;
    desc.states.reserve(static_cast<std::size_t>(clipCount));
    for (int i = 0; i < clipCount; ++i) {
        auto it = g_clips.find(clipIds[i]);
        if (it == g_clips.end()) {
            if (err) *err = "clip handle not found: " + std::to_string(clipIds[i]);
            return kAnimInvalid;
        }
        // 状态名自动补（validateStateMachine 要求非空唯一；脚本语义只认下标）。
        nl::anim::StateDef s;
        s.name = "state" + std::to_string(i);
        s.clip = it->second.clip;
        desc.states.push_back(std::move(s));
    }
    desc.rules.reserve(static_cast<std::size_t>(ruleCount));
    for (int r = 0; r < ruleCount; ++r) {
        nl::anim::TransitionRule rule;
        rule.from          = static_cast<nl::anim::StateIndex>(ruleMeta[static_cast<std::size_t>(r) * 3 + 0]);
        rule.to            = static_cast<nl::anim::StateIndex>(ruleMeta[static_cast<std::size_t>(r) * 3 + 1]);
        const int paramIdx = ruleMeta[static_cast<std::size_t>(r) * 3 + 2];
        const float threshold = ruleVals[static_cast<std::size_t>(r) * 2 + 0];
        rule.blendDuration  = ruleVals[static_cast<std::size_t>(r) * 2 + 1];
        // C++ 侧预置规则 lambda（值捕获，拷进 std::function；不过绑定边界）。
        // 判据 = params[paramIdx] >= threshold；paramIdx 越界 = 恒 false（护栏）。
        rule.canTake = [paramIdx, threshold](const nl::anim::ParamValues& p) {
            return paramIdx >= 0 &&
                   static_cast<std::size_t>(paramIdx) < p.size() &&
                   p[static_cast<std::size_t>(paramIdx)] >= threshold;
        };
        desc.rules.push_back(std::move(rule));
    }
    // 参数表容量 = 规则引用的最大 paramIdx + 1（脚本可写 0..n-1；越界写 = rc4）。
    int maxParamIdx = -1;
    for (int r = 0; r < ruleCount; ++r)
        maxParamIdx = std::max(maxParamIdx, ruleMeta[static_cast<std::size_t>(r) * 3 + 2]);
    AnimSmEntry entry;
    entry.params.assign(static_cast<std::size_t>(std::max(maxParamIdx + 1, 0)), 0.0f);
    if (!entry.sm.reset(desc)) {
        if (err) *err = "validateStateMachine: desc invalid";
        return kAnimReset;
    }
    // 创建即评估一次（t=0 初始姿态入缓存：animGetPose 未 tick 也有确定值）。
    entry.sm.evaluate(entry.lastPose);
    const int id = g_nextAnimHandle++;
    g_sms.emplace(id, std::move(entry));
    if (outSmId) *outSmId = id;
    return kAnimOk;
}

int animSmSetParam(int smId, int idx, float value) {
    auto it = g_sms.find(smId);
    if (it == g_sms.end()) return kAnimInvalid;
    if (idx < 0 || static_cast<std::size_t>(idx) >= it->second.params.size())
        return kAnimBadArg;
    if (!std::isfinite(value)) return kAnimBadArg;
    it->second.params[static_cast<std::size_t>(idx)] = value;
    return kAnimOk;
}

int animSmTick(int smId, float dt) {
    auto it = g_sms.find(smId);
    if (it == g_sms.end()) return kAnimInvalid;
    if (!std::isfinite(dt)) return kAnimBadArg;
    it->second.sm.update(dt, it->second.params);
    it->second.sm.evaluate(it->second.lastPose);
    return kAnimOk;
}

int animSmGetPose(int smId, int boneIndex, float* out7) {
    auto it = g_sms.find(smId);
    if (it == g_sms.end()) return kAnimInvalid;
    if (out7 == nullptr || boneIndex < 0 ||
        static_cast<std::size_t>(boneIndex) >= it->second.lastPose.size())
        return kAnimBadArg;
    const auto& p = it->second.lastPose[static_cast<std::size_t>(boneIndex)];
    out7[0] = p.translation.x;
    out7[1] = p.translation.y;
    out7[2] = p.translation.z;
    out7[3] = p.rotation.x;
    out7[4] = p.rotation.y;
    out7[5] = p.rotation.z;
    out7[6] = p.rotation.w;
    return kAnimOk;
}

int animSmStateInfo(int smId, int* outCurrent, float* outAlpha, float* outDuration) {
    auto it = g_sms.find(smId);
    if (it == g_sms.end()) return kAnimInvalid;
    if (outCurrent)  *outCurrent  = static_cast<int>(it->second.sm.currentState());
    if (outAlpha)    *outAlpha    = it->second.sm.transitionAlpha();
    if (outDuration) *outDuration = it->second.sm.transitionDuration();
    return kAnimOk;
}

int animRelease(int handle) {
    if (handle <= 0) return kAnimBadArg;
    if (g_clips.erase(handle) > 0) return kAnimOk;
    if (g_sms.erase(handle) > 0) return kAnimOk;
    return kAnimInvalid;
}

void animReleaseAll() {
    g_clips.clear();
    g_sms.clear();
    g_nextAnimHandle = 1;
}

std::size_t animClipCount() { return g_clips.size(); }
std::size_t animSmCount()   { return g_sms.size(); }

} // namespace nl::script
