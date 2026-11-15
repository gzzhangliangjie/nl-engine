# Vulkan-Headers — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | Vulkan-Headers — Vulkan API 头文件（Khronos 官方生成头） |
| 版本 | **VK_HEADER_VERSION 352**（`include/vulkan/vulkan_core.h` 内 `VK_HEADER_VERSION` 宏；Khronos 按头版本号发布，非 x.y.z SDK 号） |
| 上游 | https://github.com/KhronosGroup/Vulkan-Headers |
| 获取方式 | vendored 入库（S0；上游 tag 归档 / 本机已有源拷贝，见台账 S0 节） |
| 许可证 | **Apache-2.0 OR MIT 双许可**（`LICENSE.txt` 原文随库入库；Copyright 2015-2026 The Khronos Group Inc.；任选其一均可用） |
| 形态 | 纯头文件（include-only），无编译目标；NVRHI Vulkan 后端 include 消费 |
| 用途与消费点 | NVRHI Vulkan 后端编译期 Vulkan API 符号；**不链接 loader**——运行时 loader 是驱动自带的 vulkan-1.dll（架构纪律：不读 VULKAN_SDK、不 find_package(Vulkan)、不联网拉依赖） |
| 引入里程碑 | S0（台账 S0 节 117 行） |

## 风险核查（选型闸逐条）

1. **许可兼容**：Apache-2.0 / MIT 任选，皆与本项目兼容；Apache-2.0 附带显式专利授权（比 BSD 系更稳）。
2. **无厂商锁定**：Khronos 官方头；不读任何 SDK 路径，构建链零 SDK 依赖。
3. **零构建期新依赖**：纯头文件无编译产物，loader 靠驱动自带 DLL。
4. **已知风险如实记录**：头版本决定可用的 Vulkan API 面；驱动机器的 vulkan-1.dll 版本若低于头版本声明的 API，运行时报 feature not available——本项目按 NVRHI 接口面使用，不直接摸扩展，风险面小。
5. **可替换性**：只被 NVRHI Vulkan 后端 include；换 SDK 版头只影响该后端编译。
