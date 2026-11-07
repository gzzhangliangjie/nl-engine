#pragma once
// P8 M-J0：线程池 + parallel_for 最小内核（A 类并行轨道，纯 CPU，零图形符号）。
// 模型参考（只学结构不抄代码）：
//   - bevy_tasks 固定池（TaskPoolBuilder.num_threads）+ scope 主线程 block_on 参与驱动；
//   - UE ParallelFor：按 MinBatchSize 切块分给 TaskGraph，主线程 FinishedSignal->Wait。
// 不引第三方线程库（P8-JOB-PLAN 底线）；标准库 mutex/cv 足够——离线管线任务粒度大
// （每组数千三角），共享队列锁竞争可忽略，不做 per-thread deque 窃取（Carrot 反向证据）。
//
// 边界（写死，防误用）：
//   - parallelFor 不可嵌套调用同一实例（外层任务占满线程会死锁）；嵌套支持在 M-J1 scope 落地。
//   - 同一实例的 parallelFor 不可被多线程并发调用；不同实例互不影响。
//   - body 只允许写自己的槽位 / 局部数据，共享状态由调用方自行同步（写各 slot 的模式见 selftest）。

#include <cstddef>
#include <functional>

namespace nl::core {
namespace jobs {

// 固定大小线程池。确定性纪律：parallelFor 的结果只取决于 body 语义本身，
// 池不引入任何跨批次的执行顺序约定（各批次必须互不依赖，M-J2 起消费者自行保证）。
class ThreadPool {
public:
    // threadCount == 0 时取 std::thread::hardware_concurrency()；硬件数取不到时回退 1。
    explicit ThreadPool(unsigned threadCount = 0);
    ~ThreadPool();                       // join 全部 worker；不弃任务（队列残留任务先跑完）
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 把 [0, count) 切成 [begin, end) 批次并行执行 body。
    // 切块策略：起步按 minBatch 切；批数不足以喂饱线程数 x 4 时收紧批次
    // （动态竞争队列天然负载均衡，粗批次省锁、细批次防长尾——UE MinBatchSize + 动态均衡的折中）。
    // count == 0 直接返回（负向用例）。
    // body 抛异常：捕获暂存，等全部批次完成后重抛第一个异常（worker 与线程池不受牵连）。
    // 主线程也参与执行批次（bevy scope 主线程驱动 / UE 主线程帮忙模式），submit 后立即吃满劳力。
    void parallelFor(std::size_t count,
                     const std::function<void(std::size_t begin, std::size_t end)>& body,
                     std::size_t minBatch = 1);

    unsigned threadCount() const { return threadCount_; }

private:
    void workerLoop();
    void runOne(std::function<void()>& task);   // 执行 + 计账（pending--/异常暂存）

    struct Impl;
    Impl* impl_ = nullptr;
    unsigned threadCount_ = 1;
};

} // namespace jobs
} // namespace nl::core
