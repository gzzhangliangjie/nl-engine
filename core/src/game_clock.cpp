// nl/core GameClock 实现 —— 见 game_clock.hpp 头注释（双帧模型 / 三处参考口径 / 确定性纪律）。
#include "nl/core/game_clock.hpp"

#include <cmath>

namespace nl::core {

void GameClock::setTimeScale(double s)
{
    if (!std::isfinite(s) || s < 0.0) {
        timeScale_ = 0.0; // NaN / 负值 → 冻结
        return;
    }
    timeScale_ = (s > kMaxTimeScale) ? kMaxTimeScale : s;
}

void GameClock::tick(double realDtMs, const LogicTickFn& logicTick)
{
    // 暂停 = 逻辑帧时钟停摆（bevy 语义）：simTime / accumulator / frameDt 全冻结，
    // 渲染帧照常（本函数返回后调用方继续 present/HUD）。
    if (paused_) {
        frameDt_ = 0.0;
        return;
    }

    // 1) realDt(ms) → 秒；防御性钳制非法输入（NaN/负值 → 0）。
    double dt = (std::isfinite(realDtMs) && realDtMs > 0.0) ? (realDtMs * 0.001) : 0.0;

    // 2) timeScale 缩放（bevy relative_speed 语义）。
    dt *= timeScale_;

    // 3) 截断 maxDelta（bevy DEFAULT_MAX_DELTA 双重用途：大 delta 截断 + 追帧上限）。
    if (dt > kMaxDelta) {
        dt = kMaxDelta;
    }
    frameDt_ = dt;

    // 4) accumulator 累加 + 固定步长 while 消费（Carrot PhysicsSystem 同构，推广为引擎级）。
    accumulator_ += dt;
    while (accumulator_ >= kStep) {
        accumulator_ -= kStep;
        simTime_ += kStep;
        if (logicTick) {
            logicTick(kStep, tickCount_);
        }
        ++tickCount_;
    }
}

void GameClock::reset()
{
    simTime_ = 0.0;
    accumulator_ = 0.0;
    frameDt_ = 0.0;
    tickCount_ = 0;
}

} // namespace nl::core
