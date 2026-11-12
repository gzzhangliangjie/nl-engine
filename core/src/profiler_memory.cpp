// M-TR1 (P8-TRACY-PLAN)：进程内存观测薄封装。
//
// 两部分，职责不同：
//  A) K2 进程内存占用（PSAPI，零图形依赖、双后端对称）—— 始终编译、始终可用，
//     经 nl::profiler::processWorkingSetBytes()/processPrivateBytes() 供 HUD 常驻速览，
//     与 Tracy memory 曲线互补（Tracy 离线归因，HUD 常驻速览）。
//  B) Tracy 内存钩（全局 operator new/delete 覆盖，上报 TracyAllocS/TracyFreeS）——
//     仅 NL_TRACY=ON 时编译生效；OFF 构建退回 CRT 默认，零成本。
//
// 关于全局 new/delete 钩：Tracy 0.15 已移除内置全局钩（见 VENDOR.md 0.15 实测要点），
// 内存上报只能走显式 API。以下覆盖是**本项目薄封装实现**，非上游文件。

#include "nl/core/profiler_memory.hpp"

#include <cstdlib>
#include <cstddef>
#include <new>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")
#endif

namespace nl {
namespace profiler {

#ifdef _WIN32
std::uint64_t processWorkingSetBytes() {
    // PrivateUsage 只在扩展结构 PROCESS_MEMORY_COUNTERS_EX 里，按 cb 大小传入 API。
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc)))
        return pmc.WorkingSetSize;
    return 0;
}
std::uint64_t processPrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc)))
        return pmc.PrivateUsage;
    return 0;
}
#else
std::uint64_t processWorkingSetBytes() { return 0; }
std::uint64_t processPrivateBytes()    { return 0; }
#endif

} // namespace profiler
} // namespace nl

// ---------------------------------------------------------------------------
// B) Tracy 内存钩（仅 NL_TRACY 生效）。运算符必须位于全局命名空间。
// ---------------------------------------------------------------------------
#ifdef NL_TRACY
#  if defined(_MSC_VER)
#    pragma warning(push, 0)   // nl_core 是 /W4，第三方头别污染警告面
#  endif
#  include <tracy/Tracy.hpp>
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif

// 调用栈深度：0 = 不抓调用栈（默认，最低开销）；
// 需要把泄漏归因到源码时，用 -DNL_TRACY_MEM_DEPTH=16 重新编译，捕获 16 帧调用栈。
#  ifndef NL_TRACY_MEM_DEPTH
#    define NL_TRACY_MEM_DEPTH 0
#  endif

namespace {
constexpr std::size_t s_size(std::size_t n) { return n ? n : 1; }
}

void* operator new(std::size_t size) {
    void* p = std::malloc(s_size(size));
    if (!p) throw std::bad_alloc{};
    TracyAllocS(p, size, NL_TRACY_MEM_DEPTH);
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(s_size(size));
    if (p) TracyAllocS(p, size, NL_TRACY_MEM_DEPTH);
    return p;
}
void operator delete(void* p) noexcept {
    if (!p) return;
    TracyFreeS(p, NL_TRACY_MEM_DEPTH);
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (!p) return;
    TracyFreeS(p, NL_TRACY_MEM_DEPTH);
    std::free(p);
}
void* operator new[](std::size_t size) {
    void* p = std::malloc(s_size(size));
    if (!p) throw std::bad_alloc{};
    TracyAllocS(p, size, NL_TRACY_MEM_DEPTH);
    return p;
}
void operator delete[](void* p) noexcept {
    if (!p) return;
    TracyFreeS(p, NL_TRACY_MEM_DEPTH);
    std::free(p);
}

#if defined(_MSC_VER)
// C++17 对齐 new/delete（STL/引擎部分分配走对齐版）。
void* operator new(std::size_t size, std::align_val_t align) {
    void* p = _aligned_malloc(s_size(size), static_cast<std::size_t>(align));
    if (!p) throw std::bad_alloc{};
    TracyAllocS(p, size, NL_TRACY_MEM_DEPTH);
    return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
    if (!p) return;
    TracyFreeS(p, NL_TRACY_MEM_DEPTH);
    _aligned_free(p);
}
void* operator new[](std::size_t size, std::align_val_t align) {
    void* p = _aligned_malloc(s_size(size), static_cast<std::size_t>(align));
    if (!p) throw std::bad_alloc{};
    TracyAllocS(p, size, NL_TRACY_MEM_DEPTH);
    return p;
}
void operator delete[](void* p, std::align_val_t) noexcept {
    if (!p) return;
    TracyFreeS(p, NL_TRACY_MEM_DEPTH);
    _aligned_free(p);
}
#endif

#endif // NL_TRACY
