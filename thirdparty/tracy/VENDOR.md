# Tracy — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件，且 **GUI 与 client 必须成对升级**（协议强绑定，见下）。

| 项 | 值 |
|---|---|
| 库 | Tracy Profiler — real time, nanosecond resolution, remote telemetry, hybrid frame and sampling profiler |
| 版本 | **v0.15.0**（上游 2026-11-06 发布；`public/common/TracyVersion.hpp` → `Major=0 Minor=15 Patch=0`） |
| 上游 | https://github.com/wolfpld/tracy |
| 获取方式 | tag 归档 `https://gh-proxy.com/https://github.com/wolfpld/tracy/archive/refs/tags/v0.15.0.tar.gz`（gh-proxy 镜像实测 http 200，21MB；GitHub Releases 无独立源码资产）。GUI 二进制 `windows-0.15.0.zip`（31MB，同镜像） |
| 许可证 | **BSD-3-Clause**（`LICENSE` 原文随本目录入库；Copyright (c) 2017-2026, Bartosz Taudul）。`public/libbacktrace/` 自带 `LICENSE`（FSF 2012-2016，同款 BSD 风格三条款），一并保留原文 |
| 形态 | **纯源码静态库**：只编 `public/TracyClient.cpp` 一个伞形翻译单元（内部 include 各 client/common cpp），零 DLL、零外部依赖 |
| C++ 标准 | 上游 `target_compile_features(TracyClient PUBLIC cxx_std_11)`；本项目 `CMAKE_CXX_STANDARD 17` 覆盖 |
| 编译器 | MSVC 14.38（VS2022）实测口径；0.15 上游专门修了 MSVC C4366 对齐警告。Windows 下 `TracyClient.cpp` 内 `#pragma comment(lib, ...)` 自动链 `ws2_32/advapi32/user32`，无需 CMake 额外配置 |
| 入库子集 | `public/` 全部（88 文件，2.1MB：`tracy/` 包装头 + `client/` + `common/` + `libbacktrace/` + `TracyClient.cpp`）+ 根 `LICENSE`。**排除** `profiler/` `capture/` `server/` `csvexport/` `import/` `merge/` `monitor/` `update/` `python/` `tests/` `manual/` `meson.build` `cmake/` 及上游构建脚本 |
| 进程外工具 | profiler GUI 解压在 `E:/UE_STUDY_2026/.build-tools/tracy-gui/0.15.0/`（`tracy-profiler.exe` 等 8 个 exe），**不入库、不进构建链**；本机 RenderDoc 仍未安装，见 `docs/s0/S0-PLAN.md` L1 |
| 引入里程碑 | A 类并行轨道 `feat/profiling` **M-TR0**（2026-11-11），用途 = CPU 时间线 / 帧边界 / 内存分配与泄漏检查；计划 `docs/p8/P8-TRACY-PLAN.md` |

## 风险核查（选型闸逐条）

1. **开源 + 许可兼容**：BSD-3-Clause 原文在库内，允许商用 / 修改 / 再分发，与本项目 MIT 兼容；无 copyleft 传染。libbacktrace 自带三条款 BSD 风格 LICENSE 原文保留。
2. **无 EULA / 厂商锁定**：独立开源项目、非厂商图形 SDK；不读取任何 SDK 路径、不 `find_package`、不联网。**不触碰「构建链禁厂商图形 SDK / 图形符号只在 renderer/」铁律**——Tracy client 不接图形 API（TracyD3D12/TracyVulkan 头虽在 `public/tracy/` 但本项目不 include，GPU 侧仍走 NVRHI timestamp query）。
3. **零构建期新依赖**：静态源码，Windows 系统库经 pragma 自动链；`NL_TRACY=OFF`（默认）时连 target 都不编。
4. **已知风险如实记录**：BSD-3-Clause 无显式专利授权条款（与 Apache-2.0 的差别），不能宣称「绝对零风险」；社区活跃、工业界广泛使用，无已知专利 / 授权诉讼。
5. **可替换性**：业务代码只经 `nl/core/profiler.hpp` 薄封装用宏（`NL_PROFILE_*`），不把 `tracy::` 类型泄漏进业务层；换库改封装层即可。
6. **协议版本强绑定**：client 与 GUI 必须同版本（0.15 dropped pre-0.11 trace 支持）。当前固定 0.15.0 配对；升级时同步换 GUI zip 并更新本文件。

## 0.15 实测要点（与旧版印象不同，落地口径以此为准）

- `TRACY_ENABLE` 未定义 → `public/tracy/Tracy.hpp` 第 25 行 guard 让全部宏空转，OFF 构建零成本。
- **`public/client/TracyOverride.cpp` 在 0.15 只覆盖 Linux `dlclose`，整个 `public/` 内 `operator new` 零命中**——即 **Tracy 0.15 不自带全局 new/delete 内存钩**。内存上报须走显式 API `TracyAllocS(ptr,size,depth)` / `TracyFreeS(ptr,depth)`（或 `TracyAlloc`/`TracyFree`，但 `TRACY_CALLSTACK` 默认 0 无调用栈，归因要显式 depth）。M-TR1 的全局内存钩是**本项目的薄封装实现**，不是上游文件。
- 帧边界：`FrameMark` 宏（放在 present 完成处，与 RenderDoc 的 Present 边界对齐）。
- client 默认 broadcast 监听 TCP 8086；首次运行 Windows 防火墙弹窗属预期。
