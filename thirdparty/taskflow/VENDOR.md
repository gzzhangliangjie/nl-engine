# Taskflow — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | Taskflow — A General-purpose Task-parallel Programming System |
| 版本 | **v3.11.0**（tag `816b4ad53b44196c88f409eb7b4a25a0e3bfdf42`，发布 2025-11-24；`TF_VERSION 301100`） |
| 上游 | https://github.com/taskflow/taskflow |
| 获取方式 | `git clone --depth 1 --branch v3.11.0 --filter=blob:none --sparse`（经本机会话镜像），`git sparse-checkout set taskflow` |
| 许可证 | **MIT**（`LICENSE` 原文随本目录入库；Copyright (c) 2018-2025 Dr. Tsung-Wei Huang, The University of Wisconsin at Madison） |
| 形态 | **header-only**；本项目以 CMake `INTERFACE` 目标 `taskflow` 暴露 include root = `thirdparty/`（上游仓库根），业务侧 `#include <taskflow/taskflow.hpp>` |
| C++ 标准 | **C++17**（与项目 `CMAKE_CXX_STANDARD 17` 一致）。v4.x 起要求 C++20，故**不取最新而取 v3 末版** |
| 编译器 | 官方最低 MSVC 19.27；本机 VS2022 / MSVC 14.38（19.38）满足 |
| 入库子集 | `algorithm/ core/ dsl/ utility/ taskflow.hpp`，共 52 文件 / 795 KiB。**排除 `cuda/`**（CUDA 扩展不使用）；`core/declarations.hpp` 中残留的 cudaFlow 前向声明只是声明，不引入 CUDA 头 |
| 引入里程碑 | P8 job 轨道 **M-J5a**（2026-11-09），用途 = 显式前置依赖 DAG，替换原拟自研 `JobGraph` |

## 风险核查（选型闸逐条）

1. **开源 + 许可兼容**：MIT 原文在库内，允许商用 / 修改 / 再分发，与本项目 MIT 兼容；无 copyleft 传染。
2. **无 EULA / 厂商锁定**：独立学术项目，非 UE / 任何厂商 SDK 拷贝；不含 EULA 限制代码。UE 源码仍只读不抄。
3. **零构建期新依赖**：header-only、无 `.lib` / DLL、无联网拉取、无 `find_package`；不触碰「构建链不依赖厂商图形 SDK」铁律。
4. **已知风险如实记录**：MIT 无显式专利授权条款（这是 MIT 与 Apache-2.0 的差别），不能宣称「绝对零风险」；Taskflow 为学术开源项目、工业界广泛使用，无已知专利 / 授权诉讼。若未来需要显式专利授权，须重新评审选型。
5. **可替换性**：本项目只通过 `nl/core/jobgraph.hpp` 薄封装使用 `tf::`，不把 `tf::` 类型泄漏进业务代码；换库时改封装层即可。

## API 面（已回 vendored 头文件核实，非只读 README）

- `tf::Taskflow::emplace(...)` → `tf::Task`（`core/flow_builder.hpp:54` 等）。
- `tf::Task::precede(Ts&&...)` / `succeed(Ts&&...)`（`core/task.hpp:511/533`）—— 显式依赖边，支持单参。
- `tf::Executor(unsigned N)`（`core/executor.hpp:91`）、`executor.run(flow).wait()`（`:123`）、`wait_for_all()`、析构自带 join。
- `tf::Taskflow::dump(std::ostream&)` 输出 DOT，可用于拓扑证据。
- 静态图**不做环检测**（成环会挂死），因此本项目的负向用例在封装层用 Kahn 拓扑排序自行拒环，不依赖库行为。

## 确定性注意（项目硬判据）

Taskflow 是 work-stealing 执行器，**不承诺无依赖任务之间的执行顺序**。因此 `jobgraph` 封装规定：
- 任务只写自己的槽位；结果归并**按登记序**（id 升序），不按完成序；
- 依赖顺序由显式边保证（A→{B,C}→D 菱形）；
- 任何输出与单线程参考不一致即 FAIL。
