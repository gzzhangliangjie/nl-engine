#pragma once

// M-W2A Animation 域薄面句柄表（script 内部私有面；**不进 nl/anim 公开头**——
// 波 2 拍板方案① Q2：nl/anim 公开面零改动，AnimationStateMachine::transitionTo
// 保持 private，状态转移走 C++ 侧预置规则 lambda + 脚本标量参数）。
//
// 口径来源（WAVE2-PLAN §5.3 冻结口径，2026-11-15 用户拍板）：
//   - M-SC10 Props 同构纪律：C++ 只存 POD + 引擎类型，**不缓存托管引用**（生命周期
//     硬规则①——托管侧数组经 ptr 传入，调用期拷走，C++ 不持指针）。
//   - 句柄单源计数：clip 与状态机共用一个单调发号器（都 >0），animRelease 双态擦除
//     失效安全（查无 = 拒绝）；跨类型误用（sm 句柄当 clip 用）= 查表失败 = 拒绝。
//   - 规则 lambda：canTake 在 C++ 侧由 (paramIdx, threshold) 构造，纯内部
//     std::function，**不过 blittable 边界**；判据 = params[paramIdx] >= threshold。
//   - 最近 pose：animTick 每次推进后 evaluate 缓存；创建时先评估一次（t=0 初始姿态，
//     animGetPose 未 tick 也有确定值）。
//   - 热更窗口兜底：animReleaseAll()（reload/unload 前置空——与 scriptUnregisterAll
//     同族纪律；宿主热更编排调用）。

#include "nl/anim/anim.hpp"
#include "nl/anim/state_machine.hpp"
#include "nl/script/Bindings.hpp"   // kAnim* 错误码常量（同模块 nl/script 面）

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nl::script {

// 一条已加载 clip 的登记（AnimClip 按值持有；源路径留证据串）。
struct AnimClipEntry {
    nl::anim::AnimClip clip;
    std::string        path;
    int                clipIndex = -1;
};

// 一台状态机实例（AnimationStateMachine 按值持有；params 供 animSetParam 写、
// animTick 读；lastPose = 最近 evaluate 缓存，animGetPose 数据源）。
struct AnimSmEntry {
    nl::anim::AnimationStateMachine sm;
    nl::anim::ParamValues          params;
    std::vector<nl::anim::BonePose> lastPose;
};

// 加载 clip：path = 已解析的绝对/相对文件路径（白名单校验由 Bindings 层做，
// 本层不管路径语义）。返回 rc = kAnimOk（outClipId >0）/ kAnimBadArg（clipIndex
// 越界）/ kAnimLoad（loadGltfAnim 或 validateClip 失败，err 带英文描述）。
int  animClipLoad(const std::string& resolvedPath, int clipIndex,
                  int* outClipId, std::string* err);

// 建状态机：clipIds/clipCount 选 clip 句柄；ruleMeta = int[ruleCount*3]
//（from,to,paramIdx）、ruleVals = float[ruleCount*2]（threshold,blendDuration）。
// 返回 rc = kAnim*（kAnimOk 时 outSmId >0）；失败时 outSmId = 0。失败原因：
// clip 句柄查无（kAnimInvalid，err 带句柄值）、desc 校验失败（kAnimReset，err 带
// validateStateMachine 文本）、非法参数（kAnimBadArg）。
int  animSmCreate(const int* clipIds, int clipCount,
                  const int* ruleMeta, const float* ruleVals, int ruleCount,
                  int* outSmId, std::string* err);

int  animSmSetParam(int smId, int idx, float value);   // kAnimOk / kAnimInvalid / kAnimBadArg
int  animSmTick(int smId, float dt);                   // kAnimOk / kAnimInvalid / kAnimBadArg
int  animSmGetPose(int smId, int boneIndex, float* out7);  // out7 = tx ty tz qx qy qz qw
int  animSmStateInfo(int smId, int* outCurrent, float* outAlpha, float* outDuration);

// 显式回收（clip 或 sm 双态擦除；未知 = kAnimInvalid）。
int  animRelease(int handle);

// 热更窗口兜底：清空两张表 + 重置发号器（reload/unload 前宿主调用）。
void animReleaseAll();

// 观测（selftest / GUIDE 断言用）。
std::size_t animClipCount();
std::size_t animSmCount();

} // namespace nl::script
