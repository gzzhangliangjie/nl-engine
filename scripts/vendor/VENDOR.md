# scripts/vendor 溯源（P9 V2 资产导入轨，2026-11-13）

本目录 vendored 两个纯 Python 库，服务 `fbx2glb.py --unity` 权威材质链（`.mat` 解析）。
选型走 AGENTS.md「自研边界与第三方库选型闸」：用户拍板「找成熟代码抄」，禁手写解析。

## unityparser 4.0.0

- 上游：https://github.com/socialpoint-labs/unity-yaml-parser（tag v4.0.0）
- 许可证：Apache-2.0（LICENSE 原文随目录入库）
- 用途：Unity `.mat`/`.meta` YAML 解析（两代序列化格式兼容）
- 入库子集：`unityparser/` 全部 `.py` + LICENSE；`__pycache__` 不入库
- **本地补丁（diff 上游 v4.0.0）**：`constructor.py` 的 `construct_mapping`
  增加重复键收集（旧版 Unity 4.x 格式 `{data: [{first, second}]}` 在 YAML 里
  是重复键，PyYAML 默认静默丢项 → 旧格式 7 槽只剩 1）。重复键收集进
  `DupSeq(list)`（带 `dupseq=True` 属性），`unity_mat.py` 侧 `_normalize_entries`
  归一化消费。
- 风险核查：纯 Python、零构建期依赖、Apache-2.0 与项目 MIT 兼容（Apache-2.0
  含显式专利授权条款，无已知诉讼风险）。

## PyYAML 6.0.2（纯 Python 子集）

- 上游：https://github.com/yaml/pyyaml（tag 6.0.2）
- 许可证：MIT（LICENSE 原文随目录入库）
- 用途：unityparser 的 YAML 后端
- 入库子集：`yaml/` 全部 `.py`（`cyaml` C 扩展 ImportError 被 `__init__` 吞掉，
  纯 Python 路径完整可用）；`__pycache__` 不入库
- 风险核查：MIT，无专利条款（同 MIT 惯例如实注明），零构建期依赖。

## vendor-ref/（参考实现，不参与构建）

UnityGLTF（KhronosGroup/UnityGLTF，MIT）`ExporterMaterials.cs` /
`ExporterTextures.cs` / `GLTFSceneExporter.cs` 三个文件，作为 `.mat` → glTF
材质映射链的**只读口径参考**（_Mode→alphaMode、_EMISSION/_NORMALMAP/
_METALLICGLOSSMAP 槽位语义、KHR_texture_transform Y 翻转等）。抄思路不抄代码
——映射实现是本项目自写的 `scripts/unity_mat.py`。
