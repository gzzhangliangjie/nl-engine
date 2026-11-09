#pragma once
// P8 并发地基（A 类并行轨道，纯 CPU，零图形符号）。
// 模型参考（只学结构不抄代码）：
//   - bevy_tasks 固定池（TaskPoolBuilder.num_threads）+ scope 主线程 block_on 参与驱动；
//   - UE ParallelFor：按 MinBatchSize 切块分给 TaskGraph，主线程 FinishedSignal->Wait。
// M-J0~J3 用标准库 mutex/cv 实现数据并行 + fork-join（离线任务粒度大、锁竞争可忽略，
// 不做 per-thread deque 窃取，Carrot 反向证据）。M-J5a 起「有前置依赖」的任务改走
// nl/core/jobgraph.hpp（vendored Taskflow 3.11.0）；本池保持原样、不替换。
// 旧注释「不引第三方线程库」已按 2026-11-09 用户拍板作废（见 AGENTS.md 选型闸）。
//
// 边界（写死，防误用）：
//   - parallelFor 不可嵌套调用同一实例；多线程不可并发调用同一实例的 parallelFor/scope。
//   - scope 的 spawn 支持嵌套（任务体内可再 spawn，不等待——join 由 scope 统一兜底），
//     但任务不得阻塞等待其它 scope 任务的完成信号（无前置依赖图，那是 UE TGraphTask 的长线模型）。
//   - body/spawn 任务只允许写自己的槽位 / 局部数据，共享状态由调用方自行同步。

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace nl::core {
namespace jobs {

// 固定大小线程池。确定性纪律：parallelFor / scope 的结果只取决于 body 语义本身，
// 池不引入任何跨任务的执行顺序约定（各任务必须互不依赖，M-J2 起消费者自行保证）。
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

    // ---- M-J1：scoped fork-join（仿 bevy pool.scope）----
    // 同步 fork-join：fn 内 spawn 的任务（含嵌套 spawn）全部完成后才返回；
    // 结果按 spawn 顺序收集（bevy scope 同款语义）；任一任务抛异常 → 全部 join 后重抛第一个。
    // 与 parallelFor 共用同一底层队列，不另起线程。
    template <class T> class Scope;   // 前置声明（scope() 签名需要）

    template <class T>
    std::vector<T> scope(const std::function<void(Scope<T>&)>& fn);

    template <class T>
    class Scope {
    public:
        // spawn 一任务到 scope；可在 scope 闭包或任务体内调用（嵌套），不等待。
        // 生命周期：任务持有 scope 状态的裸指针，scope() join 完成前对象必活——
        // C++ 无 Rust 借用检查，靠 scope 同步语义保证无悬空（join 前任务全完成）。
        void spawn(std::function<T()> fn);

    private:
        friend class ThreadPool;
        explicit Scope(ThreadPool* pool);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        void spawnTask(std::function<T()> fn);   // spawn/嵌套共用入口

        struct State {
            std::mutex mu;                       // 保护 nextIndex/slots/firstError
            std::size_t nextIndex = 0;           // spawn 序号（结果顺序）
            std::vector<std::optional<T>> slots; // 按 spawn 序占位回填
            std::exception_ptr firstError;
            // pending 由 ThreadPool::Impl.mu 保护（主线程 drain 谓词在同一锁下读）
            std::size_t pending = 0;
        };
        ThreadPool* pool_ = nullptr;
        State* state_ = nullptr;
    };

private:
    struct Impl {
        std::mutex mu;
        std::condition_variable cvWork;   // 唤醒 worker：有新任务 / 关闭
        std::condition_variable cvDone;   // 「有任务完成」信号；等待方用各自谓词过滤
        std::deque<std::function<void()>> queue;
        std::size_t pending = 0;          // parallelFor 专用计数（mu 保护）
        std::exception_ptr firstError = nullptr;   // parallelFor 首个批次异常（mu 保护）
        bool shutdown = false;
        std::vector<std::thread> workers;
    };

    void workerLoop();
    Impl& impl() { return *impl_; }

    Impl* impl_ = nullptr;
    unsigned threadCount_ = 1;
};

} // namespace jobs
} // namespace nl::core

// ---- 模板实现（.hpp 内展开；impl() 与 Impl 供嵌套类使用）----
namespace nl::core {
namespace jobs {

template <class T>
ThreadPool::Scope<T>::Scope(ThreadPool* pool) : pool_(pool), state_(new State) {}

template <class T>
ThreadPool::Scope<T>::~Scope() { delete state_; }

template <class T>
void ThreadPool::Scope<T>::spawn(std::function<T()> fn) { spawnTask(std::move(fn)); }

template <class T>
void ThreadPool::Scope<T>::spawnTask(std::function<T()> fn) {
    State& st = *state_;
    Impl& im = *pool_->impl_;

    // 1) 分配结果槽位（st.mu 保护）
    std::size_t idx;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        idx = st.nextIndex++;
        st.slots.emplace_back();
    }
    // 2) 入队（pending 由 im.mu 保护；锁顺序恒 st.mu -> im.mu，无死锁）
    {
        std::lock_guard<std::mutex> lk(im.mu);
        ++st.pending;
        im.queue.push_back([fn = std::move(fn), idx, sp = this] {
            try {
                T v = fn();
                std::lock_guard<std::mutex> lkSt(sp->state_->mu);
                sp->state_->slots[idx] = std::move(v);
            } catch (...) {
                std::lock_guard<std::mutex> lkSt(sp->state_->mu);
                if (!sp->state_->firstError) sp->state_->firstError = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lkIm(sp->pool_->impl_->mu);
                --sp->state_->pending;
                sp->pool_->impl_->cvDone.notify_all();
            }
        });
        im.cvWork.notify_one();
    }
}

template <class T>
std::vector<T> ThreadPool::scope(const std::function<void(Scope<T>&)>& fn) {
    Scope<T> s(this);
    typename Scope<T>::State& st = *s.state_;
    Impl& im = *impl_;

    fn(s);

    // 主线程参与 drain（与 parallelFor 同款）：吃到队列空且 pending==0。
    // 嵌套 spawn 在任务执行中入队，循环天然覆盖。
    for (;;) {
        std::function<void()> task;
        bool got = false;
        {
            std::unique_lock<std::mutex> lk(im.mu);
            if (!im.queue.empty()) {
                task = std::move(im.queue.front());
                im.queue.pop_front();
                got = true;
            } else if (st.pending == 0) {
                break;
            } else {
                im.cvDone.wait(lk, [&] {
                    return (st.pending == 0 && im.queue.empty()) || !im.queue.empty();
                });
            }
        }
        if (got) {
            task();
            std::lock_guard<std::mutex> lk(im.mu);
            im.cvDone.notify_all();
        }
    }

    // 收集：异常则丢弃结果重抛第一个（与 parallelFor 语义一致）
    std::vector<T> out;
    if (st.firstError) std::rethrow_exception(st.firstError);
    out.reserve(st.slots.size());
    for (auto& slot : st.slots) out.push_back(std::move(*slot));
    return out;
}

} // namespace jobs
} // namespace nl::core
