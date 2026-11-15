# DirectX-Headers — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | DirectX-Headers — Microsoft 官方 D3D12 开源辅助头（DirectX 头 + dxguids + wsl 互操作头） |
| 版本 | **上游 master 快照，无版本字符串**（本目录为 include/src 子集，未收上游 CMakeLists，故无 project VERSION 可考；升级时对照上游 release tag 核对 include 内容差异） |
| 上游 | https://github.com/microsoft/DirectX-Headers |
| 获取方式 | vendored 子集入库（S0：`include/directx` + `include/dxguids` + `include/wsl` + `src/`） |
| 许可证 | **MIT**（`LICENSE` 原文随库入库；Copyright (c) Microsoft Corporation） |
| 形态 | 头文件为主；`src/` 提供少量实现（dxguids 定义等），经 NVRHI D3D12 后端消费 |
| 用途与消费点 | NVRHI D3D12 后端需要的开源辅助头（Windows SDK 自带 D3D12 头之外的部分，如 ID3D12Enhanced 系列 / wsl 互操作）；根 CMakeLists 头注释明确其为「NVRHI D3D12 后端需要的开源辅助头」 |
| 引入里程碑 | S0（台账 S0 节 117 行） |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT 原文在库内，允许商用 / 修改 / 再分发，无 copyleft 传染。
2. **无厂商锁定**：Microsoft 官方开源（非闭源 SDK），不读任何厂商 SDK 路径、不 find_package——D3D12 核心 API 头/库仍来自 Windows SDK（系统自带），本目录只补开源辅助面。
3. **零构建期新依赖**：vendored 头 + 少量 src，随 NVRHI D3D12 后端一并编入。
4. **已知风险如实记录**：无版本字符串使升级对照靠文件 diff（弱溯源面，已在本文件如实标注）；上游演进快于本项目消费面，锁定快照即可。
5. **可替换性**：只被 NVRHI D3D12 后端消费；换头只影响该后端编译。
