#pragma once
// P8 M-GL5：命名 TickGroup 注册制 —— 逻辑帧内执行顺序的显式契约（A 类并行轨道，
// 纯 CPU 零图形符号，计划 docs/p8/P8-GAMELOOP-PLAN.md）。
//
// 解决的问题：M-GL0~GL4 期间逻辑帧内的执行顺序一直硬编码在 viewer/main.cpp 的
// 匿名 LogicTickFn lambda 里（runScene/runNormal 两份副本）——「脚本必须先于 ECS、
// 音频必须读最终位置」这类依赖没有任何地方写成条文，新消费方（粒子/载具/交通）
// 只能继续塞 lambda。本类把顺序升级为显式契约：消费方按组注册，组序即执行序。
//
// 三处参考口径（2026-11-10 实读，M-GL5 开工按铁律核对）：
//   - UE Runtime/Engine/Classes/Engine/EngineBaseTypes.h:83-110 —— ETickingGroup
//     组序（PrePhysics→StartPhysics→DuringPhysics→EndPhysics→PostPhysics→
//     PostUpdateWork）+ FTickPrerequisite 跨组依赖图 + TickInterval 逐函数调速；
//     工业终态是「排序组 + 依赖图 + 并行组」三机制叠加。
//   - bevy crates/bevy_app/src/main_schedule.rs:94-148 —— FixedMain = FixedFirst
//     →FixedPreUpdate→FixedUpdate→FixedPostUpdate→FixedLast，RunFixedMainLoop
//     包裹（每渲染帧 0..N 次）；纯逻辑帧流派 = 命名阶段 + 单一固定速率，无 UE 式组。
//   - Carrot —— 全 engine 目录 grep TickGroup = 0 命中（无 tick 组概念，反向印证
//     小引擎顺序硬编码也能活，但扩展性差）。
//
// 本类取 bevy 流派（逻辑帧内命名阶段 + 单一 60Hz 固定速率），组名借 UE 语义；
// 多速率（UE TickInterval）与跨组依赖图（UE FTickPrerequisite）不在本轮范围
// （PLAN §4 候选，出现真实需求再立项，2026-11-10 用户拍板 A 方案）。
//
// 顺序契约（runScene 实况，M-GL2 既有顺序原样固化，基线零重拍）：
//   PrePhysics（script OnTick：可能 spawn/删实体）→ Physics（Jolt step）
//   → PostPhysics（ECS world.tick：transform 消费物理结果）
//   → PostUpdateWork（audio 声源同步：读最终状态）。
//
// 确定性纪律：组间顺序恒为枚举序（编译期固定）；组内单回调（同组多消费方由注册方
// 在一个 fn 内串联，保持调用序可审计）；同一注册序列产出逐位一致的执行序列。

#include <cstdint>
#include <functional>

namespace nl::core {

// 逻辑帧内的命名执行阶段（枚举序 = 执行序，编译期固定契约）。
enum class TickGroup : int {
    PrePhysics = 0,     // 物理/模拟前：脚本 OnTick、动作、输入意图消费
    Physics,            // 物理步进（Jolt step）
    PostPhysics,        // 物理后：ECS world.tick（transform 同步等）
    PostUpdateWork,     // 收尾：audio 声源同步等「读最终状态」消费方
    Count
};

// 组名（诊断/日志/game.tickgroups 输出用；返回静态字符串，不转移所有权）。
const char* tickGroupName(TickGroup g);

// 组回调注册表：组间顺序固定（枚举序）、组内单回调。runTick 按枚举序执行已注册
// 组，空组跳过——注册与执行解耦（M-GL1 语义：viewer 在帧循环前一次性注册）。
class TickScheduler {
public:
    using TickFn = std::function<void(double dt, std::uint64_t tickIndex)>;

    // 注册/替换组回调。组序与注册先后无关——枚举序是唯一顺序源。
    void set(TickGroup g, TickFn fn) { fns_[static_cast<int>(g)] = std::move(fn); }
    void clear(TickGroup g) { fns_[static_cast<int>(g)] = nullptr; }
    bool has(TickGroup g) const { return static_cast<bool>(fns_[static_cast<int>(g)]); }
    int registeredCount() const;

    // 执行一个逻辑 tick 的全部已注册组（按枚举序）。dt/tickIndex 透传
    // （GameClock 语境下恒 kStep / 单调递增）。
    void runTick(double dt, std::uint64_t tickIndex) const;
    // 注意：注册是结构配置不是时间线状态，本类不提供 reset（测试需要就重新构造）。
private:
    TickFn fns_[static_cast<int>(TickGroup::Count)] = {};
};

} // namespace nl::core
