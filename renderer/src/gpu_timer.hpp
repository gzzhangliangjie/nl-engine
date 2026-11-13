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

// M-PERF1：per-pass GPU 计时（Q-P4/Q-P5 拍板，docs/perf/PERF-PLAN.md）。
// 与 GpuTimer 同语义的多实例版：每个命名 pass 一对 begin/end，各自 4-slot 环
// （懒建 / poll 不阻塞 / 无空闲槽本帧跳过）。ensure 由 Scene::pollPassTimers 在
// --pass-timing 打开后首次轮询时才调 —— 开关关 = 一个 query 都不建 = 渲染路径
// 零变化（Q-P5 零基线扰动）。
// pass 清单（Q-P4，按实码对位；id 即 HudFrame.passTimingMs 槽位）：
//   csCull          视锥剔除 compute（scene.cpp frustumCull_ 段，与 hizBuild 拆开）
//   visRaster       vis buffer 簇光栅（visbuf 三段式 (4)）
//   matResolve      material 反解（visbuf pulling 全屏三角形）
//   hizBuild        Hi-Z 深度预通道 + 金字塔 + top（scene.cpp hiz_ 段）
//   csDirect        直光 compute（shadow 模式 dispatch）
//   csRefGI         参考图累积 compute（reference 模式 dispatch）
//   csPlace         探针 cascade 布点（drawProbeFrame (2)）
//   csTraceProbe    探针追踪（drawProbeFrame (3)）
//   csGather        final gather（drawProbeGather (1)）
//   csFilterTemporal  时域累积（S12 M12.0）
//   csFilterSpatial   空间联合双边（S12 M12.1）
//   csGatherCompose   全屏合成（drawProbeGather (2)；--filter-gi-dump 分支不计时）
//   mainDraw        主绘制 drawIndexedIndirect（非 visbuf cull/dag/hiz 路径）
class PassTimers {
public:
    static constexpr int kMaxPasses = 16;
    static constexpr int kSlots = 4;   // 同 GpuTimer：比 frame-in-flight=2 多留两个

    enum PassId {
        kPassCull = 0, kPassVisRaster, kPassMaterial, kPassHizBuild,
        kPassDirect, kPassRefGI, kPassPlace, kPassTrace, kPassGather,
        kPassFilterTemporal, kPassFilterSpatial, kPassGatherCompose, kPassMainDraw,
        kPassCount
    };
    static_assert(kPassCount <= kMaxPasses, "pass count exceeds HudFrame table");

    // ASCII 名：HUD 逐 pass 行 + resolve 日志锚共用（SHADER-CONVENTIONS 字符串纪律）。
    static const char* name(int id) {
        static const char* kNames[kPassCount] = {
            "csCull", "visRaster", "matResolve", "hizBuild",
            "csDirect", "csRefGI", "csPlace", "csTraceProbe", "csGather",
            "csFilterTemporal", "csFilterSpatial", "csGatherCompose", "mainDraw"};
        return kNames[id];
    }

    bool ensure(nvrhi::IDevice* device) {
        if (ready_) return true;
        if (!device) return false;
        for (int p = 0; p < kPassCount; ++p) {
            for (int s = 0; s < kSlots; ++s) {
                nvrhi::TimerQueryHandle q = device->createTimerQuery();
                if (!q) return false;
                slots_[p][s] = Slot{q, false};
            }
            active_[p] = -1;
            writeCursor_[p] = 0;
            pollCursor_[p] = 0;
        }
        device_ = device;
        ready_ = true;
        return true;
    }

    // device 必须还活着时调用（shutdown 早期）。
    void shutdown() {
        for (int p = 0; p < kPassCount; ++p) {
            for (int s = 0; s < kSlots; ++s) slots_[p][s] = Slot{};
            active_[p] = -1;
            writeCursor_[p] = 0;
            pollCursor_[p] = 0;
        }
        device_ = nullptr;
        ready_ = false;
    }

    bool available() const { return ready_; }

    // cmd->open() 之后调用。未 resolve 的 query 不能复用（NVRHI 规定）；
    // 找不到空闲 slot 时本帧不计时，不报错、不阻塞（与 GpuTimer 同款静默降级）。
    bool begin(nvrhi::ICommandList* cmd, int pass) {
        if (!ready_ || pass < 0 || pass >= kPassCount || active_[pass] >= 0) return false;
        for (int n = 0; n < kSlots; ++n) {
            const int i = (writeCursor_[pass] + n) % kSlots;
            if (slots_[pass][i].pending) continue;
            cmd->beginTimerQuery(slots_[pass][i].query);
            active_[pass] = i;
            writeCursor_[pass] = (i + 1) % kSlots;
            return true;
        }
        return false;
    }

    void end(nvrhi::ICommandList* cmd, int pass) {
        if (!ready_ || pass < 0 || pass >= kPassCount || active_[pass] < 0) return;
        cmd->endTimerQuery(slots_[pass][active_[pass]].query);
        slots_[pass][active_[pass]].pending = true;
        active_[pass] = -1;
    }

    // 帧首轮询全部 pass：只取已 resolve 的结果，绝不阻塞。outMs/outValid 为
    // kPassCount 长度数组；本轮有任一新 resolve 返回 true（调用方据此刷日志锚）。
    bool poll(float* outMs, bool* outValid) {
        bool any = false;
        for (int p = 0; p < kPassCount; ++p) {
            for (int n = 0; n < kSlots; ++n) {
                const int i = (pollCursor_[p] + n) % kSlots;
                Slot& s = slots_[p][i];
                if (!s.pending) continue;
                if (!device_->pollTimerQuery(s.query)) continue;
                outMs[p] = GpuTimer::toMilliseconds(device_->getTimerQueryTime(s.query));
                device_->resetTimerQuery(s.query);
                s.pending = false;
                pollCursor_[p] = (i + 1) % kSlots;
                outValid[p] = true;
                any = true;
                break;   // 每 pass 每轮只消费一个最新结果
            }
        }
        return any;
    }

private:
    struct Slot {
        nvrhi::TimerQueryHandle query;
        bool pending = false;
    };

    nvrhi::IDevice* device_ = nullptr;
    Slot   slots_[kPassCount][kSlots] = {};
    int    active_[kPassCount] = {};
    int    writeCursor_[kPassCount] = {};
    int    pollCursor_[kPassCount] = {};
    bool   ready_ = false;
};

} // namespace nl::renderer::detail
