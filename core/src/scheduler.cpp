// P8 并发地基 M-J5b：进程级常驻调度底座实现。组合两个既有 jobs::ThreadPool
// （compute + io），复用已验证的 parallelFor / scope 内核，零确定性风险。

#include "nl/core/scheduler.hpp"

#include <thread>

namespace nl::core {
namespace jobs {

Scheduler* Scheduler::s_instance = nullptr;
std::mutex Scheduler::s_mutex;
unsigned Scheduler::s_computeCfg = 0;
unsigned Scheduler::s_ioCfg = 0;

namespace {
unsigned resolveCompute(unsigned n) {
    if (n != 0) return n;
    const unsigned hw = std::thread::hardware_concurrency();
    return hw ? hw : 1;
}
// IO 阻塞为主，不必满核；收小到 max(2, hw/4)。
unsigned resolveIo(unsigned n) {
    if (n != 0) return n;
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) return 2;
    const unsigned v = hw / 4;
    return v < 2 ? 2 : v;
}
} // namespace

void Scheduler::configure(unsigned computeThreads, unsigned ioThreads) {
    std::lock_guard<std::mutex> lk(s_mutex);
    // 已建实例：规模冻结，no-op（不修改配置、不抛）。
    if (s_instance) return;
    s_computeCfg = computeThreads;
    s_ioCfg = ioThreads;
}

Scheduler& Scheduler::instance() {
    std::lock_guard<std::mutex> lk(s_mutex);
    if (!s_instance) {
        s_instance = new Scheduler(resolveCompute(s_computeCfg), resolveIo(s_ioCfg));
    }
    return *s_instance;
}

Scheduler::Scheduler(unsigned computeThreads, unsigned ioThreads)
    : compute_(std::make_unique<ThreadPool>(computeThreads)),
      io_(std::make_unique<ThreadPool>(ioThreads)),
      computeThreads_(computeThreads),
      ioThreads_(ioThreads) {}

} // namespace jobs
} // namespace nl::core
