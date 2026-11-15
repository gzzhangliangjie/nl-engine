# METIS — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。**详细溯源见本目录 `VENDORED.md`**（先于本规范落档，内容完整有效），本文件按统一格式补关键行。

| 项 | 值 |
|---|---|
| 库 | METIS 5 + GKlib — 图分割库（KarypisLab） |
| 版本 | **上游 main 分支浅克隆快照**（2026-11-07；METIS 上游无常规版本 tag 口径，快照日期即溯源锚） |
| 上游 | https://github.com/KarypisLab/METIS + https://github.com/KarypisLab/GKlib |
| 获取方式 | main 分支浅克隆（见 `VENDORED.md`） |
| 许可证 | **Apache-2.0 系**（`LICENSE.metis` / `LICENSE.gklib` 原文随库入库） |
| 形态 | 纯源码静态库 `nl_metis`（本目录自带 CMakeLists，不用上游构建系统） |
| 用途与消费点 | nanite-tools 离线管线簇聚组（S2 M2.0：节点=簇、边=共享顶点数权重图分割）；nl_core PRIVATE 链 |
| 引入里程碑 | M2.0（S2 nanite 离线管线） |

## 改动留痕（摘自 VENDORED.md）

1. 仅 `include/metis.h`：宽度宏 `IDXTYPEWIDTH 32` / `REALTYPEWIDTH 32` 取消注释（上游要求手动改）。
2. `INT32_MIN` 等 4 宏加 `#ifndef` guard（UCRT limits.h 已定义，MSVC C4005 消噪）。其余源码原样。

## 风险核查（选型闸逐条）

1. **许可兼容**：Apache-2.0 系与本项目兼容；Apache-2.0 含显式专利授权。
2. **无厂商锁定**：学术开源库，零 EULA。
3. **零构建期新依赖**：纯源码，自建 CMakeLists。
4. **已知风险如实记录**：离线管线专用（不进运行时）；参考依据 = bevy 0.15/0.16 同款选型（JMS55：DAG 质量关键路径）。
5. **可替换性**：nanite-tools 离线工具消费，图分割接口面小（nodes/eweights/UFactor），替换改调用点即可。
