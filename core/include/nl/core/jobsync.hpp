#pragma once
// P8 M-J5c-prep：动态依赖计数 job + Barrier（Jolt JobSystem 同构能力，Jolt 无关）。
// 目的：把 physics / ECS 将来接入所需的「动态依赖执行」桩打在本轨道——
//   - physics 侧：JPH::JobSystem 适配器把 CreateJob / RemoveDependencyAndQueue / Barrier
//     映射到本原语（JobSystemWithBarrier 模式），待 physics 轨道合回 master 后再绑；
//   - ECS 侧：静态 system 依赖图直接走 nl/core/jobgraph.hpp（Taskflow DAG），不经此处。
// 合约参考（读证，只学合约不抄代码，2026-11-09）：
//   thirdparty/jolt/Jolt/Core/JobSystem.h 头注释 ——
//   a) 子 job CreateJob(name, numDependencies=1)，父 job 体尾显式 second_job.RemoveDependency()
//      → 计数归 0 由 JobSystem 入队（动态图：图可在任务运行中生长）；
//   b) Barrier::AddJob 在 WaitForJobs 等待期间合法（无法预先建全图）；
//   c) WaitForJobs 必须能帮跑（execute jobs while waiting），否则全池线程都在等会饿死被等任务。
//
// 生命周期契约（对齐 Jolt「创建即带依赖计数」的防早跑思路）：
//   1) create(exec, numDependencies) 后计数 = numDependencies + 1：多出的 1 是「创建者持有」，
//      上游链接完成后必须 removeDependency()（或 launch()）释放，job 才有资格入队。
//   2) link(upstream, downstream) 在 upstream 上登记 downstream（upstream 完成时自动对其
//      removeDependency(1)），并给 downstream 计数 +1；仅允许 downstream 未发布时调用。
//   3) 任一次 removeDependency 即视为「发布」：此后 link 拒绝（负向返回 false）。
//   4) Barrier::addJob 对已完成 job 即时对冲记账；wait() 帮跑模式：等待期参与执行池队列。
//   5) JobHandle 引用计数管理生命周期：句柄复制/析构安全；job 至少活到完成通知发完
//      （创建者句柄可先析构，入队后 job 由池闭包自持引用）。
//
// 线程安全：全部 API 线程安全（计数原子 + 登记表 per-job mutex）；同实例 ThreadPool 的
// parallelFor/scope 串行边界不变（jobs.hpp 头注释），jobsync 只经 post/runOne 与池交互。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>
#include "nl/core/jobs.hpp"

namespace nl::core {
namespace jobs {

class Job;      // 内部类型，jobsync.cpp 定义
class Barrier;

// 引用计数句柄（Jolt JobHandle 同构面：AddDependency / RemoveDependency / IsDone）。
class JobHandle {
public:
    JobHandle() = default;
    ~JobHandle();
    JobHandle(const JobHandle& other);
    JobHandle& operator=(const JobHandle& other);
    JobHandle(JobHandle&& other) noexcept;
    JobHandle& operator=(JobHandle&& other) noexcept;

    // 增依赖：仅对未发布 job 合法（契约 2/3）。
    void addDependency(unsigned count = 1) const;
    // 减依赖：归 0 → 入池队列；返回是否因本次调用入队。
    // 对 null 句柄返回 false；本调用即「发布」（此后 link 拒绝）。
    bool removeDependency(unsigned count = 1) const;
    bool isDone() const;
    explicit operator bool() const { return job_ != nullptr; }

private:
    friend class JobSync;
    friend class Barrier;
    explicit JobHandle(Job* job);
    Job* job_ = nullptr;
};

// 完成等待栅栏（Jolt Barrier 同构）：等待期间允许动态 AddJob；wait() 帮跑。
class Barrier {
public:
    explicit Barrier(ThreadPool& pool);
    ~Barrier();                          // 隐式 wait()（对齐 Jolt：Destroy 前必须等完）
    Barrier(const Barrier&) = delete;
    Barrier& operator=(const Barrier&) = delete;

    // 登记待等 job；等待期间可继续调用（Jolt 合约 b）。
    // 已完成 job 记即时完成（对冲路径）；null 句柄忽略。
    void addJob(const JobHandle& job);
    // 帮跑等待：阻塞期参与执行池队列（Jolt 合约 c），防「全池都在等」饿死。
    void wait();
    bool done() const;

private:
    friend class Job;
    void onJobFinished();                // Job 完成回调（内部）
    ThreadPool& pool_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::size_t pending_ = 0;            // 未完成登记数（mu_ 保护）
};

// 动态依赖执行器门面：job 落在给定池（常驻用法传 Scheduler::instance().compute()）。
class JobSync {
public:
    // numDependencies：外部将显式 removeDependency 的次数；创建者持有额外 +1（契约 1）。
    static JobHandle create(const std::function<void()>& exec, unsigned numDependencies,
                            ThreadPool& pool);
    static JobHandle create(const std::function<void()>& exec, ThreadPool& pool) {
        return create(exec, 0, pool);
    }
    // 发布：释放创建者持有（等价 removeDependency()）。
    static void launch(const JobHandle& job);
    // 链接 upstream → downstream：upstream 完成时自动对 downstream removeDependency(1)。
    // downstream 已发布 / 任一句柄为空 / upstream 已完成 → 拒绝返回 false（负向）。
    static bool link(const JobHandle& upstream, const JobHandle& downstream);
    // 等待单 job 完成（帮跑模式）。
    static void wait(const JobHandle& job, ThreadPool& pool);
};

} // namespace jobs
} // namespace nl::core
