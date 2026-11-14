# VENDOR.md — vendor-ref/unitygltf（导出器参考子集）

> 2026-11-14 公开前补记。原入库（P9 V2 asset ingest，commit 15e9f48）时未按
> `thirdparty/<lib>/VENDOR.md` 规范留档，本文件为补记；已知的溯源事实如实
> 写明，未知的条目明确标注「未记录」，不臆造。

## 溯源

| 条目 | 内容 |
|---|---|
| 上游项目 | UnityGLTF（现维护仓：github.com/KhronosGroup/UnityGLTF，前身为 Siccity/UnityGLTF） |
| 上游地址 | https://github.com/KhronosGroup/UnityGLTF |
| 许可证 | MIT（上游仓库 LICENSE；**本目录文件未携带上游 license 头与版权行**，见下方「已知缺口」） |
| 版本 / commit | **未记录**（P9 入库时未留 tag/commit，无法回溯确切版本） |
| C++ 标准 | 不适用（C# 参考件，不参与本仓编译链接） |

## 入库子集

仅 3 个导出器 partial 源文件，作 **Unity 侧 glTF 导出链路的只读参考**
（P9 asset ingest 轨道：Unity 权威材质链 → glTF 的导出行为对照）：

- `ExporterMaterials.cs`
- `ExporterTextures.cs`
- `GLTFSceneExporter.cs`

## 边界与用途

- **只读参考，不编译、不链接、不进任何构建产物**；与 `thirdparty/` 下的
  vendored 编译依赖（NVRHI/Jolt/Recast 等）性质不同，故单独放 `vendor-ref/`。
- 业务代码不得 include/复制本目录内容进构建链。
- UE 导出链对照另有纪律约束（AGENTS.md：UE 源码只读不抄），本目录仅覆盖
  UnityGLTF 这一份参考。

## 已知缺口（升级/后续收口时补齐）

1. 文件内无上游版权行与 license 头——严格按选型硬闸第 1 条「LICENSE 原文
   随 vendored 入库」尚欠 LICENSE 原文与版权归属行；需先确定入库时的上游
   commit 再补，**不许凭记忆臆写版权行**。
2. 入库版本未记录；若未来需要升级子集，重新从上游拉对应文件并连同当时
   的 LICENSE/版权行一并留档。

## 风险核查结论

- MIT 许可，与本仓 MIT 兼容，可公开托管；MIT 无显式专利授权条款的事实
  如实记录（与 AGENTS.md 口径一致，不宣称「绝对零风险」）。
- 子集不含上游 editor 测试件 / 第三方依赖（glTF.Schema 等引用命名空间
  仅存在于上游完整工程，本目录未带入）。
