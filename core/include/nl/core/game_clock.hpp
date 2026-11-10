#pragma once
// P8 游戏循环 / 双帧模型（A 类并行轨道，纯 CPU，零图形符号，计划 docs/p8/P8-GAMELOOP-PLAN.md）。
//
// 双帧模型：渲染帧（vsync/变步长）vs 逻辑帧（固定 60Hz accumulator）。
// 三处参考口径（2026-11-09/10 实读，M-GL0 开工按铁律复核）：
//   - Carrot engine/physics/PhysicsSystem.cpp:130-138 —— accumulator 固定步长 while 循环
//     （TimeStep = 1.0/60.0）；它只在物理侧用，本类推广为引擎级 sim clock。
//   - bevy crates/bevy_time/src/virt.rs —— Time<Fixed> 与 Time<Virtual> 分离；
//     DEFAULT_MAX_DELTA = 250ms 双重用途（大 delta 截断 + 长卡顿追帧上限）；
//     paused = 时钟停摆（delta 归零，时间不烧不丢）。本类取 bevy 的暂停语义
//     （非 Carrot 的「跳过 tick 但照烧 accumulator」——语义是「时钟停摆」而非「丢帧」）。
//   - UE Runtime/Engine/Private/UnrealEngine.cpp FEngineLoop::Tick —— 渲染帧为主
//     （真时变 dt + 可选 bUseFixedFrameRate 帧率门 + 物理 substepping），与本项目
//     「逻辑帧为主」是两条流派；拍板逻辑帧为主（对齐确定性回放契约，PLAN §2）。
//
// 确定性纪律：纯 double 运算、无隐藏状态、同一 realDt 序列产出逐位一致的 tick 序列
// （与 viewer「确定性回放 = 固定样本 + 固定 dt」契约同向，见 PLAN §2）。
// 边界：单线程使用起步（M-GL2 再评估并行）；不插值（用户拍板「起步不插值」，
// alpha() 仅作输出预留，渲染帧直接取最新逻辑帧状态）。

#include <cstdint>
#include <functional>

#include "nl/core/tick_groups.hpp"

namespace nl::core {

class GameClock {
public:
    // 逻辑帧步长：固定 60Hz（对齐 Carrot TimeStep）。
    static constexpr double kStep = 1.0 / 60.0;
    // 单渲染帧喂给逻辑时钟的 delta 上限（秒），对齐 bevy DEFAULT_MAX_DELTA=250ms。
    // 双重用途：① 防死亡螺旋（单帧大 delta 截断，如断点/拖窗口后最多补 15 tick）；
    // ② 长卡顿追赶上限（时间丢失而非集中爆发）。
    static constexpr double kMaxDelta = 0.250;
    // timeScale 合法区间 [0, kMaxTimeScale]；0 = 冻结（显式零速，与 paused 叠加）。
    static constexpr double kMaxTimeScale = 1024.0;

    // 逻辑帧回调：dt 恒为 kStep，tickIndex 单调递增（从 0 起）。
    using LogicTickFn = std::function<void(double dt, std::uint64_t tickIndex)>;

    GameClock() = default;

    // 渲染帧驱动入口。realDtMs = 本渲染帧实际耗时（毫秒）。
    // 内部顺序：paused/scale 处理 → 截断 maxDelta → accumulator 累加 → while 跑 logicTick(kStep)。
    // 暂停语义：paused_ 为真时本帧什么都不动（accumulator/simTime/frameDt 全冻结）。
    // 非法输入（NaN/负值）按 0 处理（防御性钳制，UE 对负 dt 报错、本项目静默钳 0 并由
    // 调用方自查——工具链在 selftest 里锚定该行为）。
    void tick(double realDtMs, const LogicTickFn& logicTick);

    // M-GL5：按组调度入口（TickScheduler 组序契约见 tick_groups.hpp）。
    // 与旧 LogicTickFn 重载并存：传 lambda 走旧路径（gl-tools/selftest 契约不变），
    // 不传则每逻辑帧按枚举序跑成员 scheduler_ 的已注册组。
    void tick(double realDtMs) { tick(realDtMs, {}); }
    TickScheduler& scheduler() { return scheduler_; }

    // ---- 状态查询（全部只读，渲染帧取值即「最新逻辑帧状态」，不插值）----
    double simTime() const { return simTime_; }          // 逻辑时间（秒），每 tick += kStep
    double frameDt() const { return frameDt_; }          // 本渲染帧实际喂入的缩放+截断后 delta（秒）；暂停帧恒 0
    double accumulator() const { return accumulator_; }  // 未消费余量（秒），[0, kStep)
    double alpha() const { return accumulator_ / kStep; }// 帧间插值预留输出，恒 [0,1)（起步不插值，仅暴露）
    double timeScale() const { return timeScale_; }
    bool   paused() const { return paused_; }
    std::uint64_t tickCount() const { return tickCount_; } // 已执行逻辑帧总数（单调）

    // ---- 控制 ----
    void setPaused(bool p) { paused_ = p; }
    // 越界钳制到 [0, kMaxTimeScale]（NaN 归 0）。
    void setTimeScale(double s);
    // 全量归零（simTime/accumulator/frameDt/tickCount；timeScale/paused 保留——
    // reset 语义是「重开一局时间线」，不是「恢复出厂」）。
    void reset();

private:
    double simTime_ = 0.0;
    double accumulator_ = 0.0;
    double frameDt_ = 0.0;
    double timeScale_ = 1.0;
    bool   paused_ = false;
    std::uint64_t tickCount_ = 0;
    TickScheduler scheduler_;   // M-GL5：tick(realDtMs) 无回调路径的按组调度
};

} // namespace nl::core
