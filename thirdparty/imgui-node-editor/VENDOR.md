# imgui-node-editor — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | ImGui Node Editor — 基于 Dear ImGui 的节点/连线图编辑器库 |
| 版本 | **v0.9.4**（`imgui_node_editor.h` → `IMGUI_NODE_EDITOR_VERSION "0.9.4"`） |
| 上游 | https://github.com/thedmd/imgui-node-editor |
| 获取方式 | vendored 源码子集入库（T1-M1；源 cpp/h + crude_json 等，含 LICENSE） |
| 许可证 | **MIT**（`LICENSE` 原文随库入库） |
| 形态 | 纯源码随 editor 宿主编入；依赖 vendored imgui（同代 API） |
| 用途与消费点 | 材质图交互式编辑（`--editor <file.mg>`）：画布拖拽/连线/属性编辑/Save/Compile（T1-M1~M3 全套）；NodeEditor.json 为运行时布局状态 |
| 引入里程碑 | T1-M1（台账 1141 行） |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT 原文在库内，无风险面。
2. **无厂商锁定**：独立开源库，只依赖 imgui 本体。
3. **零构建期新依赖**：纯源码。
4. **已知风险如实记录（有台账实证）**：① v0.9.4 API 是 `PinKind`（新版才叫 `PinDirection`），升级有 API 断裂面；② 对 size<=0 回退「占满剩余」语义（T1 踩坑②）；③ NodeEditor.json 运行时状态污染确定性截图，拍摄前 rm（T1 踩坑③）。
5. **可替换性**：只在 editor 宿主消费，材质图数据模型（.mg v1 文本序列化）是自研独立格式，换编辑器库不迁移数据。
