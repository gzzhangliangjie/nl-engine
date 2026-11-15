# NVRHI — vendored 源码溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | NVRHI — NVIDIA rendering hardware interface（DX12 / Vulkan 双后端抽象层） |
| 版本 | **上游 master 快照，无版本字符串**（NVRHI 上游不发布版本宏；引入时拉取 2021 代接口——vendored `doc/Tutorial.md` 无 `beginRenderPass` 可佐证代际，升级需对照 doc） |
| 上游 | https://github.com/NVIDIA-SDK/NVRHI |
| 获取方式 | vendored 源码入库（S0）；`doc/`（Tutorial / ProgrammingGuide）一并保留作接口对照 |
| 许可证 | **MIT**（`LICENSE.txt` 原文随库入库；Copyright (c) 2014-2021, NVIDIA CORPORATION）；`ThirdPartyLicenses.txt` 记录库内第三方段许可 |
| 形态 | 纯源码静态库（根 CMakeLists `add_subdirectory(thirdparty/nvrhi EXCLUDE_FROM_ALL)`） |
| 用途与消费点 | 唯一图形 API 抽象层——`renderer/` 独占消费（架构铁律：图形 API 符号只允许活在 renderer/）；DX12 与 Vulkan 双后端 |
| 引入里程碑 | S0 / M0.2（`CMakeLists.txt` 头注释；台账 S0 节 117 行） |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT 原文在库内，允许商用 / 修改 / 再分发，无 copyleft 传染。
2. **无厂商锁定**：MIT 开源（非 NVIDIA 专有 SDK），无 EULA、不联网、不读厂商 SDK 路径——正符合「构建链禁厂商图形 SDK」铁律（它是抽象层不是 SDK）。
3. **零构建期新依赖**：DX12 头/库来自 Windows SDK；Vulkan 头走本地 vulkan-headers，loader 用驱动自带 vulkan-1.dll。
4. **已知风险如实记录**：引入时为 2021 代接口快照（较上游滞后）；上游属低频维护状态，双后端语义差异（binding 偏移、资源态）由本项目统一旗标吸收（如 SPIR-V `-fvk-u-shift 384 0`）。
5. **可替换性**：renderer/ 内部按 NVRHI 接口编程，业务层（viewer/core/modes）只见 FrameParams 与模式分发；替换抽象层只需重写 renderer 侧 backend 文件。
