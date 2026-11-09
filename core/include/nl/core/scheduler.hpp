#pragma once
// P8 并发地基 M-J5b：进程级常驻调度底座（A 类并行轨道，纯 CPU，零图形符号）。
//
// 设计依据（三处参考，只学结构不抄代码，详见 docs/p8/P8-JOB-PLAN.md §三处参考口径）：
//   - bevy_tasks：ComputeTaskPool / AsyncComputeTaskPool / IoTaskPool 三池分流（usages.rs:57-75）；
//   - Carrot：4 条 TaskLane（MainLoop / Rendering / FrameParallelWork / AssetLoading）按工作类型分 lane；
//   - UE TaskGraph：ENamedThreads 命名线程亲和（与本底座的「分池」同属一类职责，但不绑依赖图）。
// 本项目落地：常驻 Scheduler 组合两个既有 ThreadPool——compute()（CPU 密集）+ io()（阻塞 IO / 资产预取）。
//
// 与老 jobs::ThreadPool 的关系（兼容层）：
//   - ThreadPool 一行不动（M-J0~J3 的 26 项 selftest + 全部消费者零改动）；
//     本底座直接组合 ThreadPool 实例，复用已验证的 parallelFor / scope 内核，零确定性风险。
//   - 新消费者走 Scheduler::instance().compute()/.io()，不再各自 `new` 池——「统一常驻化」在此落地。
//
// 边界（写死）：
//   - configure() 必须在首次 instance() 前调用；之后调用是 no-op，常驻规模稳定（防帧时间 / 确定性判据污染）。
//   - 底座与进程同生命周期，不析构（避免静态析构顺序问题）。
//   - 不进 kStage / 不进 bytes-near 渲染回归（纯 CPU 库）。
//
// 头依赖：本文件 include jobs.hpp，ThreadPool / ThreadPool::Scope<T> 的完整定义在此可见。

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "nl/core/jobs.hpp"

namespace nl::core {
namespace jobs {

// 进程级常驻调度底座。组合 compute / io 两个 ThreadPool。
class Scheduler {
public:
    // 在首次 instance() 前调用，设定两个池的线程数。
    // 任一为 0 → 取 std::thread::hardware_concurrency()；io 默认再收小到 max(2, hw/4)
    // （IO 阻塞为主，不需满核）。已建实例后调用无效（no-op，不抛），保证常驻规模稳定。
    static void configure(unsigned computeThreads = 0, unsigned ioThreads = 0);

    // 常驻单例（首次访问惰性构建；用 configure() 的配置，未配置则用默认）。
    static Scheduler& instance();

    // 计算池句柄：CPU 密集数据并行 / fork-join（material 编译、simplify、anim dump、check-orient）。
    ThreadPool& compute() { return *compute_; }
    // IO 池句柄：阻塞 IO / 资产预取（P9 V4 residency 预备位，当前薄壳，独立实例避免拖垮 compute）。
    ThreadPool& io() { return *io_; }

    unsigned computeThreads() const { return computeThreads_; }
    unsigned ioThreads() const { return ioThreads_; }

    // 便捷转发（直接跑在 compute 池上）。
    void parallelFor(std::size_t count,
                     const std::function<void(std::size_t, std::size_t)>& body,
                     std::size_t minBatch = 1) {
        compute_->parallelFor(count, body, minBatch);
    }

    template <class T>
    std::vector<T> scope(const std::function<void(ThreadPool::Scope<T>&)>& fn) {
        return compute_->scope<T>(fn);
    }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

private:
    Scheduler(unsigned computeThreads, unsigned ioThreads);
    ~Scheduler() = default;   // 进程级常驻：不析构

    std::unique_ptr<ThreadPool> compute_;
    std::unique_ptr<ThreadPool> io_;
    unsigned computeThreads_ = 1;
    unsigned ioThreads_ = 1;

    static Scheduler* s_instance;
    static std::mutex s_mutex;
    static unsigned s_computeCfg;
    static unsigned s_ioCfg;
};

} // namespace jobs
} // namespace nl::core
