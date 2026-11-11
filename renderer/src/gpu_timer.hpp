// M0.6 — NVRHI timestamp query：两后端共享的 GPU 帧内计时器。
//
// 纪律：只用 NVRHI 抽象（ITimerQuery / beginTimerQuery / endTimerQuery），不出现任何
// D3D12 / Vulkan 符号，构建链依旧零厂商 SDK。DX12 侧映射到 EndQuery + ResolveQueryData，
// Vulkan 侧映射到 vkCmdResetQueryPool + vkCmdWriteTimestamp，换算由 NVRHI 统一做。
//
// 用法（两后端对称）：
//   renderFrame 开头（hud != nullptr 时）：gpuTimer_.poll(ms) -> 写回 HudFrame
//   cmd->open() 之后：gpuTimer_.begin(cmd)
//   cmd->close() 之前：gpuTimer_.end(cmd)
// --no-hud 路径一次都不调用，M0.1~M0.5 的回放基线保持逐字节不变。
#pragma once

#include <nvrhi/nvrhi.h>

#include <vector>

#include "nl/core/log.hpp"

namespace nl::renderer::detail {

class GpuTimer {
public:
    static constexpr int kSlots = 4;   // 比 frame-in-flight=2 多留两个，避免等 resolve 时没得用

    // NVRHI getTimerQueryTime() 返回秒；HUD 一律以 ms 显示。
    static float toMilliseconds(float seconds) { return seconds * 1000.0f; }

    static int nextCursor(int c) { return (c + 1) % kSlots; }

    // M4.2：resolve 日志带实例标签（frame / cull）——同帧两个实例各 resolve 一次，
    // 日志里区分得开，验收才能逐行对照。只影响日志文本，不影响任何截图基线。
    void setLabel(const char* label) { label_ = label; }

    // 延迟到第一帧真正需要 HUD 时才建 query：--no-hud 完全不碰。
    bool ensure(nvrhi::IDevice* device) {
        if (ready_) return true;
        if (!device) return false;
        slots_.clear();
        for (int i = 0; i < kSlots; ++i) {
            nvrhi::TimerQueryHandle q = device->createTimerQuery();
            if (!q) { slots_.clear(); return false; }
            slots_.push_back(Slot{q, false});
        }
        device_ = device;
        ready_ = true;
        return true;
    }

    // device 必须还活着时调用（shutdown 早期）。
    void shutdown() {
        slots_.clear();
        device_ = nullptr;
        ready_ = false;
        active_ = -1;
        writeCursor_ = 0;
        pollCursor_ = 0;
        resolvedOnce_ = false;
    }

    // 帧首轮询：只取已经 resolve 的结果，绝不阻塞；拿到一个就返回。
    // 结果与具体帧的对应关系由 HUD 曲线体现（异步计时天然有 1~2 帧延迟）。
    bool poll(float& outMilliseconds) {
        if (!ready_) return false;
        for (int n = 0; n < kSlots; ++n) {
            const int i = (pollCursor_ + n) % kSlots;
            Slot& s = slots_[i];
            if (!s.pending) continue;
            if (!device_->pollTimerQuery(s.query)) continue;
            outMilliseconds = toMilliseconds(device_->getTimerQueryTime(s.query));
            device_->resetTimerQuery(s.query);
            s.pending = false;
            pollCursor_ = nextCursor(i);
            // M0.6 验收证据：首次真实 resolve 落一条日志（交互模式跑几秒即可核对）。
            if (!resolvedOnce_) {
                nl::core::log::infoCh("renderer", "gpu timer resolved [", label_, "]: ", outMilliseconds,
                                    " ms (first successful NVRHI timestamp query)");
                resolvedOnce_ = true;
            }
            return true;
        }
        return false;
    }

    // cmd->open() 之后调用。NVRHI 规定：未 resolve 的 query 不能复用；
    // 找不到空闲 slot 时本帧干脆不计时，不报错、不阻塞、不复用。
    bool begin(nvrhi::ICommandList* cmd) {
        if (!ready_ || active_ >= 0) return false;
        for (int n = 0; n < kSlots; ++n) {
            const int i = (writeCursor_ + n) % kSlots;
            if (slots_[i].pending) continue;
            cmd->beginTimerQuery(slots_[i].query);
            active_ = i;
            writeCursor_ = nextCursor(i);
            return true;
        }
        return false;
    }

    void end(nvrhi::ICommandList* cmd) {
        if (!ready_ || active_ < 0) return;
        cmd->endTimerQuery(slots_[active_].query);
        slots_[active_].pending = true;
        active_ = -1;
    }

    bool available() const { return ready_; }

    // 不依赖真实 GPU 的机械自检：秒→毫秒换算 + 环形游标回绕。
    static bool selfTest() {
        volatile float seconds = 0.0165f;   // volatile：避免 /W4 下 C4127 常量折叠
        const float ms = toMilliseconds(seconds);
        if (ms < 16.49f || ms > 16.51f) return false;
        if (nextCursor(kSlots - 1) != 0) return false;
        if (nextCursor(0) != 1) return false;
        return true;
    }

private:
    struct Slot {
        nvrhi::TimerQueryHandle query;
        bool pending = false;
    };

    nvrhi::IDevice* device_ = nullptr;
    std::vector<Slot> slots_;
    const char* label_ = "frame";
    bool ready_ = false;
    int active_ = -1;
    int writeCursor_ = 0;
    int pollCursor_ = 0;
    bool resolvedOnce_ = false;
};

} // namespace nl::renderer::detail
