// nl/core TickScheduler 实现 —— 见 tick_groups.hpp 头注释（M-GL5 命名组注册制 /
// 三处参考口径 / 顺序契约）。
#include "nl/core/tick_groups.hpp"

namespace nl::core {

const char* tickGroupName(TickGroup g)
{
    switch (g) {
    case TickGroup::PrePhysics:     return "PrePhysics";
    case TickGroup::Physics:        return "Physics";
    case TickGroup::PostPhysics:    return "PostPhysics";
    case TickGroup::PostUpdateWork: return "PostUpdateWork";
    default:                        return "?";
    }
}

int TickScheduler::registeredCount() const
{
    int n = 0;
    for (const auto& fn : fns_) {
        if (fn) ++n;
    }
    return n;
}

void TickScheduler::runTick(double dt, std::uint64_t tickIndex) const
{
    for (const auto& fn : fns_) {
        if (fn) {
            fn(dt, tickIndex);
        }
    }
}

} // namespace nl::core
