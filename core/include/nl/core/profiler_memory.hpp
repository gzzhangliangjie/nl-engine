#pragma once
// M-TR1 K2：进程内存占用查询（PSAPI，零图形依赖、双后端对称）。
// 始终可用，与 Tracy 开关无关：Tracy memory 曲线是离线归因，这里是 HUD 常驻速览，
// 两者互补（P8-TRACY-PLAN §2.5 / §3 M-TR1）。
#include <cstdint>

namespace nl {
namespace profiler {
    // 工作集（物理内存占用，含共享页）。
    std::uint64_t processWorkingSetBytes();
    // 私有提交内存（不含共享，最贴近"本进程独占占用"）。
    std::uint64_t processPrivateBytes();
}
}
