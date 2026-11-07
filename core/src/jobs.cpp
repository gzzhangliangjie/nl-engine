// P8 M-J0：ThreadPool 实现。设计依据见 nl/core/jobs.hpp 头注释（bevy_tasks / UE ParallelFor 参考 + 不做窃取的理由）。

#include "nl/core/jobs.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nl::core {
namespace jobs {

struct ThreadPool::Impl {
    std::mutex mu;
    std::condition_variable cvWork;   // 唤醒 worker：有新任务 / 关闭
    std::condition_variable cvDone;   // 唤醒主线程：pending 归零
    std::deque<std::function<void()>> queue;
    std::size_t pending = 0;          // 已创建尚未完成的批次数（含执行中）
    bool shutdown = false;
    std::exception_ptr firstError;    // 本轮 parallelFor 的第一个批次异常
    std::vector<std::thread> workers;
};

ThreadPool::ThreadPool(unsigned threadCount) {
    unsigned n = threadCount ? threadCount : std::thread::hardware_concurrency();
    if (n == 0) n = 1;                // hardware_concurrency 允许返回 0（标准明说）
    threadCount_ = n;
    impl_ = new Impl;
    impl_->workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        impl_->workers.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->shutdown = true;
    }
    impl_->cvWork.notify_all();
    // 不弃任务：shutdown 后 worker 仍把队列残留跑完再退（防御性，正常使用时 queue 为空）
    for (auto& t : impl_->workers) {
        if (t.joinable()) t.join();
    }
    delete impl_;
}

void ThreadPool::workerLoop() {
    Impl& im = *impl_;
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(im.mu);
            im.cvWork.wait(lk, [&] { return im.shutdown || !im.queue.empty(); });
            if (im.queue.empty()) return;   // shutdown 且队列已清空
            task = std::move(im.queue.front());
            im.queue.pop_front();
        }
        runOne(task);
    }
}

void ThreadPool::runOne(std::function<void()>& task) {
    try {
        task();
    } catch (...) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->firstError) impl_->firstError = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        --impl_->pending;
        if (impl_->pending == 0) impl_->cvDone.notify_all();
    }
}

void ThreadPool::parallelFor(std::size_t count,
                             const std::function<void(std::size_t, std::size_t)>& body,
                             std::size_t minBatch) {
    if (count == 0) return;
    Impl& im = *impl_;

    // ---- 切块：minBatch 起步；批数 < 线程数 x 4 时收紧批次，喂饱线程又留长尾缓冲 ----
    std::size_t batch = minBatch ? minBatch : 1;
    std::size_t targetBatches = std::size_t(threadCount_) * 4;
    if ((count + batch - 1) / batch < targetBatches) {
        batch = (count + targetBatches - 1) / targetBatches;   // 收紧（不低于 minBatch 不强求）
    }
    const std::size_t nBatches = (count + batch - 1) / batch;

    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.firstError = nullptr;
        im.pending = nBatches;
        for (std::size_t b = 0; b < nBatches; ++b) {
            const std::size_t begin = b * batch;
            const std::size_t end = begin + batch < count ? begin + batch : count;
            im.queue.emplace_back([&body, begin, end] { body(begin, end); });
        }
    }
    im.cvWork.notify_all();

    // ---- 主线程参与执行（bevy scope block_on / UE 主线程帮忙模式）----
    for (;;) {
        std::function<void()> task;
        bool got = false;
        {
            std::unique_lock<std::mutex> lk(im.mu);
            if (!im.queue.empty()) {
                task = std::move(im.queue.front());
                im.queue.pop_front();
                got = true;
            } else if (im.pending == 0) {
                break;
            } else {
                im.cvDone.wait(lk, [&] { return im.pending == 0 || !im.queue.empty(); });
            }
        }
        if (got) runOne(task);
    }

    // 全部批次完成后重抛第一个异常（worker/池不受牵连，池可继续使用）
    if (im.firstError) std::rethrow_exception(im.firstError);
}

} // namespace jobs
} // namespace nl::core
