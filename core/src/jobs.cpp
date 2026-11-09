// P8 并发地基：ThreadPool 非模板部分实现。设计依据见 nl/core/jobs.hpp 头注释
// （bevy_tasks / UE ParallelFor 参考 + 不做窃取的理由）。M-J1 起任务闭包自含完成计账
// （parallelFor 用 Impl.pending，scope 用 Scope<T>::State.pending），worker 只负责取任务执行。

#include "nl/core/jobs.hpp"

#include <algorithm>

namespace nl::core {
namespace jobs {

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
        // 任务闭包自含计账与异常处理（parallelFor/scope 各自管各自的计数）；
        // worker 层兜底吞异常防池死亡（正常路径不应到达）。
        try {
            task();
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lk(im.mu);
            im.cvDone.notify_all();
        }
    }
}

// ---- M-J5c-prep：动态依赖执行器原语（与 parallelFor/scope 共用 Impl.queue；worker 不感知）----
void ThreadPool::post(std::function<void()> fn) {
    Impl& im = *impl_;
    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.queue.push_back(std::move(fn));
        im.cvWork.notify_one();
    }
}

bool ThreadPool::runOne() {
    Impl& im = *impl_;
    std::function<void()> task;
    {
        std::lock_guard<std::mutex> lk(im.mu);
        if (im.queue.empty()) return false;
        task = std::move(im.queue.front());
        im.queue.pop_front();
    }
    try {
        task();
    } catch (...) {
        // 与 workerLoop 同款兜底：闭包应自含异常处理（parallelFor 批次闭包自含计账，
        // 被 runOne 帮跑也正确；jobsync 闭包不抛）。
    }
    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.cvDone.notify_all();
    }
    return true;
}

void ThreadPool::parallelFor(std::size_t count,
                             const std::function<void(std::size_t, std::size_t)>& body,
                             std::size_t minBatch) {
    if (count == 0) return;
    Impl& im = *impl_;

    // ---- 切块：minBatch 起步；批数 < 线程数 x 4 时收紧批次，喂饱线程又留长尾缓冲 ----
    std::size_t batch = minBatch ? minBatch : 1;
    const std::size_t targetBatches = std::size_t(threadCount_) * 4;
    if ((count + batch - 1) / batch < targetBatches) {
        batch = (count + targetBatches - 1) / targetBatches;   // 收紧
    }
    const std::size_t nBatches = (count + batch - 1) / batch;

    {
        std::lock_guard<std::mutex> lk(im.mu);
        // 本轮异常暂存：共享队列里可能残留上一轮任务的尾焰？不会——边界写死同实例
        // 串行使用；此处直接用 Impl.pending + 首个异常槽。
        im.firstError = nullptr;
        im.pending = nBatches;
        for (std::size_t b = 0; b < nBatches; ++b) {
            const std::size_t begin = b * batch;
            const std::size_t end = begin + batch < count ? begin + batch : count;
            im.queue.emplace_back([&body, begin, end, &im] {
                try {
                    body(begin, end);
                } catch (...) {
                    std::lock_guard<std::mutex> lkIm(im.mu);
                    if (!im.firstError) im.firstError = std::current_exception();
                }
                std::lock_guard<std::mutex> lkIm(im.mu);
                --im.pending;
                im.cvDone.notify_all();
            });
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
        if (got) {
            task();
            std::lock_guard<std::mutex> lk(im.mu);
            im.cvDone.notify_all();
        }
    }

    // 全部批次完成后重抛第一个异常（worker/池不受牵连，池可继续使用）
    if (im.firstError) std::rethrow_exception(im.firstError);
}

} // namespace jobs
} // namespace nl::core
