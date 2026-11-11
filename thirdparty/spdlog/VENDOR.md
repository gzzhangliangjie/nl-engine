# spdlog — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | spdlog — Fast C++ logging library |
| 版本 | **v1.17.0**（tag `79524dd`，commit 79524ddcbdc4496eaa03e3d5268f45cf7943ddb5；`SPDLOG_VERSION "1.17.0"`） |
| 上游 | https://github.com/gabime/spdlog |
| 获取方式 | `git clone --depth 1 --branch v1.17.0 https://gh-proxy.com/https://github.com/gabime/spdlog.git`（gh-proxy 镜像实测可达） |
| 许可证 | **MIT**（`LICENSE` 原文随本目录入库；Copyright (c) 2016-present Gabi Melman & spdlog contributors） |
| 形态 | **编译型静态库**（`SPDLOG_COMPILED_LIB`），源码 `src/*.cpp` 直接编进本项目 `nl_spdlog` 目标；bundled fmt 随头分发（`include/spdlog/fmt/`），`bundled_fmtlib_format.cpp` 一并编译 |
| C++ 标准 | **C++17**（与项目 `CMAKE_CXX_STANDARD 17` 一致；spdlog v1.x 官方支持 C++11 起） |
| 编译器 | 官方支持 MSVC；本机 VS2022 / MSVC 14.38（19.38）满足 |
| 入库子集 | `include/spdlog/`（含 bundled fmt 头）+ `src/*.cpp` 7 个 + `LICENSE`，共 113 文件。**排除 `tests/` `bench/` `example/` `logos/` `scripts/` `cmake/` 及上游 CMakeLists** |
| 引入里程碑 | A 类并行轨道 **M-LOG0**（2026-11-11），用途 = `nl::core::log` 兼容层内核替换（文件落盘 + ODS 转发 + 分类通道 + 过滤指令） |

## 风险核查（选型闸逐条）

1. **开源 + 许可兼容**：MIT 原文在库内，允许商用 / 修改 / 再分发，与本项目 MIT 兼容；无 copyleft 传染。
2. **无 EULA / 厂商锁定**：独立开源项目、业界事实标准（GitHub 25k+ star），非厂商 SDK；不含 EULA 限制代码。
3. **零构建期新依赖**：bundled fmt 版（非 external fmt），无 `.lib` / DLL 外部依赖、无联网拉取、无 `find_package`；不触碰「构建链不依赖厂商图形 SDK」铁律。
4. **已知风险如实记录**：MIT 无显式专利授权条款（这是 MIT 与 Apache-2.0 的差别），不能宣称「绝对零风险」；spdlog 社区极活跃、工业界广泛使用，无已知专利 / 授权诉讼。
5. **可替换性**：本项目只通过 `nl/core/log.hpp` 薄封装使用，**不把 `spdlog::` 类型泄漏进业务代码**；换库时改封装层即可（P8-LOG-PLAN §4 兼容铁律）。

## API 面（落码用到的，回 vendored 头核实）

- `spdlog::logger` / `spdlog::sinks::sink` 组合：多 sink 同 logger 广播（UE redirector 语义的轻量对应）。
- `sinks::rotating_file_sink`（本次只用 `basic_file_sink_mt`，启动建新文件不轮转）。
- `sinks::msink_sink`/Windows 专用：`spdlog/sinks/msvc_sink.h` = OutputDebugStringA 转发（ODS 喂 L3 调试链路）。
- `set_pattern` 自定义格式（保 `[nl:level] message` 锚点格式）。
- level 枚举 `trace/debug/info/warn/err/critical/off` 与 6 级别映射。
- `spdlog::details::registry` / 直接构造 `std::shared_ptr<spdlog::logger>` 自管生命周期（不进全局 registry，避免 name 冲突）。
