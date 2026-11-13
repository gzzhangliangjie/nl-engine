# VENDOR.md — bitsery

## 基本信息

| 项 | 值 |
|---|---|
| 库 | bitsery（二进制序列化 / 位打包，header-only C++11） |
| 版本 | **v5.2.5**（上游 tag） |
| 上游 commit | `9bebfd4`（tag v5.2.5 HEAD，浅克隆时实取） |
| 上游地址 | https://github.com/fraillt/bitsery |
| 许可证 | **MIT**（LICENSE 原文随本目录入库） |
| C++ 标准 | C++11 起可用（本仓按 C++17 / MSVC 14.38 编译） |
| 获取方式 | gh-proxy 镜像浅克隆（`git clone --depth 1 --branch v5.2.5`），本地时间 2026-11-13 |

## 入库子集

- `include/bitsery/`：**全量头文件**（62 个 .h，432K）——header-only，无排除必要；`adapter` / `brief_syntax` / `details` / `ext` / `traits` 全保留。
- `LICENSE`、`README.md`、`CHANGELOG.md` 原文入库。
- **排除**：上游 `tests/`、`examples/`、`doc/`、`scripts/`、`CMakeLists.txt`（本仓自建接口层，见下）。

## 构建接入

- **零构建期新依赖**：纯头文件，无 .c/.cpp，无链接项，无运行时 DLL。
- 接口层：`core/include/nl/net/` 内 `Snapshot.hpp` 直接 `#include <bitsery/...>`？**否**——按薄封装铁律，bitsery 头只在 `core/src/net/SnapshotCodec.cpp` 单 TU 内包含，`Snapshot.hpp` 只暴露 POD 结构 + encode/decode 自由函数（`std::vector<uint8_t>` 进出），业务代码不见 bitsery 类型。
- CMake：无需 add_library，仅 `target_include_directories(nl_core PRIVATE thirdparty/bitsery/include)`（PRIVATE，不泄漏）。

## 风险核查结论

- MIT 无显式专利授权条款——如实声明，不宣称「绝对零风险」（选型硬闸第 2 条口径）。
- 上游 2025-10 仍维护（v5.2.5 发布于 2024-09，CHANGELOG 连续）；社区广泛用于游戏实时网络（README 自称，未采信其基准数字）。
- 用途边界：**仅 M-NET1 快照位打包编解码**；不做指针/继承/智能指针序列化（`ext/pointer*.h` 未用），不接 std 容器扩展（快照定长 POD）。

## 与拍板材料的对应

- 选型权威落点 `docs/net/NETCODE-OPTIONS.md` §4.1（Q-N4 拍板 = msgpack_min 保留 + bitsery vendored）。
- 引入动作登记 `docs/net/NETCODE-PLAN.md` M-NET1 任务 1。
