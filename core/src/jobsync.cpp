// P8 M-J5c-prep 实现：动态依赖计数 job + Barrier。设计依据见 nl/core/jobsync.hpp 头注释
// （Jolt JobSystem.h 合约参考：创建即带依赖计数 + RemoveDependencyAndQueue + Barrier 帮跑）。
// 执行落点：ThreadPool::post / runOne（M-J5c-prep 新增的两个最小原语），不碰池内核。

#include "nl/core/jobsync.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace nl::core {
namespace jobs {

// ---- 内部 Job：动态依赖计数 + 完成登记表（Jolt Job 同构，引用计数管生命周期）----
class Job {
public:
    enum State : unsigned { kCreated = 0, kRunning = 1, kDone = 2 };

    Job(const std::function<void()>& exec, unsigned numDependencies, ThreadPool& pool)
        : exec_(exec), pool_(pool), pending_(numDependencies + 1) {}   // +1 = 创建者持有（契约 1）

    // ---- 生命周期（引用计数）----
    void retain() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }

    // ---- 依赖计数 ----
    void addDependency(unsigned count) { pending_.fetch_add(count, std::memory_order_acq_rel); }

    // 归 0 → 入队；返回是否因本次调用入队。过度释放（prev < count）防御性拒绝不动计数。
    bool removeDependency(unsigned count) {
        published_.store(true, std::memory_order_release);   // 契约 3：任一次释放即发布
        unsigned prev = pending_.load(std::memory_order_acquire);
        for (;;) {
            if (prev < count) return false;
            if (pending_.compare_exchange_weak(prev, prev - count,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                break;
            }
        }
        if (prev != count) return false;                     // 未归 0
        queue_();
        return true;
    }

    bool isDone() const { return state_.load(std::memory_order_acquire) == kDone; }
    bool published() const { return published_.load(std::memory_order_acquire); }

    // 登记 downstream（持本 job 的 mu_）：upstream 完成时自动对其 removeDependency(1)。
    // 本 job 已完成 → 拒绝（链接须发生在完成前）。
    bool addDependent(Job* downstream) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_.load(std::memory_order_acquire) == kDone) return false;
        downstream->retain();
        downstream->addDependency(1);
        dependents_.push_back(downstream);
        return true;
    }

    // 登记 barrier：本 job 已完成 → 返回 false（调用方走即时对冲路径）。
    bool addWaiter(Barrier* barrier) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_.load(std::memory_order_acquire) == kDone) return false;
        retain();                                            // 栅栏持有 job（Jolt AddRef 约定）
        waiters_.push_back(barrier);
        return true;
    }

private:
    void queue_() {
        retain();                                            // 入队前自持：创建者句柄可先析构
        pool_.post([this] {
            state_.store(kRunning, std::memory_order_release);
            exec_();
            finish_();
            release();
        });
    }

    void finish_() {
        retain();                                            // 自保：通知过程中外部引用可清零
        std::vector<Barrier*> bars;
        std::vector<Job*> deps;
        {
            std::lock_guard<std::mutex> lk(mu_);
            state_.store(kDone, std::memory_order_release);
            bars.swap(waiters_);
            deps.swap(dependents_);
        }
        for (Barrier* b : bars) {
            b->onJobFinished();
            release();                                       // 对冲 addWaiter 的 retain
        }
        for (Job* d : deps) {
            d->removeDependency(1);                          // 归 0 → 完成线程入队（不内联递归）
            d->release();                                    // 对冲 addDependent 的 retain
        }
        release();                                           // 对冲自保
    }

    std::function<void()> exec_;
    ThreadPool& pool_;
    std::mutex mu_;                                          // 保护 dependents_/waiters_/Done 转变
    std::vector<Job*> dependents_;
    std::vector<Barrier*> waiters_;
    std::atomic<unsigned> pending_;                          // 依赖计数（含创建者持有；单调减）
    std::atomic<unsigned> state_{kCreated};
    std::atomic<bool> published_{false};
    std::atomic<int> refs_{1};
};

// ---- Barrier ----
Barrier::Barrier(ThreadPool& pool) : pool_(pool) {}
Barrier::~Barrier() { wait(); }

void Barrier::addJob(const JobHandle& job) {
    if (!job.job_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++pending_;                                          // 先记账（即时对冲路径也要记）
    }
    if (!job.job_->addWaiter(this)) {
        onJobFinished();                                     // job 已完成 → 即时对冲
    }
}

void Barrier::onJobFinished() {
    std::size_t left;
    {
        std::lock_guard<std::mutex> lk(mu_);
        left = --pending_;
    }
    if (left == 0) cv_.notify_all();
}

bool Barrier::done() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_ == 0;
}

void Barrier::wait() {
    for (;;) {
        if (pool_.runOne()) continue;                        // 帮跑：被等任务在队列里就直接吃掉执行
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (pending_ == 0) return;
            // onJobFinished 会 notify；超时兜底防漏唤醒（1ms 粒度，工具规模足够）
            cv_.wait_for(lk, std::chrono::milliseconds(1), [&] { return pending_ == 0; });
            if (pending_ == 0) return;
        }
    }
}

// ---- JobHandle ----
JobHandle::~JobHandle() { if (job_) job_->release(); }
JobHandle::JobHandle(const JobHandle& other) : job_(other.job_) { if (job_) job_->retain(); }
JobHandle& JobHandle::operator=(const JobHandle& other) {
    if (this != &other) {
        if (other.job_) other.job_->retain();
        if (job_) job_->release();
        job_ = other.job_;
    }
    return *this;
}
JobHandle::JobHandle(JobHandle&& other) noexcept : job_(other.job_) { other.job_ = nullptr; }
JobHandle& JobHandle::operator=(JobHandle&& other) noexcept {
    if (this != &other) {
        if (job_) job_->release();
        job_ = other.job_;
        other.job_ = nullptr;
    }
    return *this;
}
void JobHandle::addDependency(unsigned count) const { if (job_) job_->addDependency(count); }
bool JobHandle::removeDependency(unsigned count) const {
    return job_ ? job_->removeDependency(count) : false;
}
bool JobHandle::isDone() const { return job_ && job_->isDone(); }
JobHandle::JobHandle(Job* job) : job_(job) {}

// ---- JobSync ----
JobHandle JobSync::create(const std::function<void()>& exec, unsigned numDependencies,
                          ThreadPool& pool) {
    return JobHandle(new Job(exec, numDependencies, pool));
}

void JobSync::launch(const JobHandle& job) {
    if (job.job_) job.job_->removeDependency(1);             // 释放创建者持有（契约 1）
}

bool JobSync::link(const JobHandle& upstream, const JobHandle& downstream) {
    if (!upstream.job_ || !downstream.job_) return false;
    if (downstream.job_->published()) return false;          // 已发布（契约 3）→ 拒绝
    return upstream.job_->addDependent(downstream.job_);
}

void JobSync::wait(const JobHandle& job, ThreadPool& pool) {
    if (!job.job_) return;
    while (!job.job_->isDone()) {
        if (!pool.runOne()) std::this_thread::yield();       // 帮跑；job 在跑则让出
    }
}

} // namespace jobs
} // namespace nl::core
