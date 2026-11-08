# 材质图轨道 · 支线任务登记

> 2026-11-08 登记。主线阶梯（B 路）：B1 固定管线材质常量 ✅ → B2 数据模型+序列化 ✅ → B3 图→Slang 双后端编译 ✅ → B4 热重载 ✅（tag `mg-b2b4`）→ B5 接 PBR pass（S7 后）。
> 本文件登记**主线阶梯之外的支线任务**，防止「提了但没落文档」被遗忘。

## 支线 T1：交互式可视化节点编辑器（GUI）

- **动机**：B2~B4 交付的都是 CLI / 数据层——`render` 子命令只能出**静态**证据图（SVG/PNG），没有真正的交互式编辑界面。作为「Slang 可视化编辑器」的最终形态，GUI 是这条轨道的题中之义，但不阻塞 B5（接 PBR pass），故立为支线。
- **UI 选型**（§2.1 调研已点名 ✅）：`imgui-node-editor`（MaterialX Graph Editor 同款节点 UI 库）+ 已 vendored 的 **ImGui v1.91.9**。不引入新的窗口框架，复用 viewer/renderer 的 ImGui 接入先例（`renderer/src/hud.cpp` 摊平管线）。
- **范围拆解（按依赖排序）**：
  1. **节点画布**：平移 / 缩放 / 拖拽节点——直接读写 `Node.x/y`（`material_graph v1` 序列化已支持布局坐标，落盘即持久）。
  2. **连线编辑**：引脚拖拽创建 / 点击删除——走 `MaterialGraph::connect` + `validate`（输入单入边、类型一致约束自动生效）。
  3. **属性面板**：props 编辑（如 `ConstantFloat.value`），改完即时反映到节点框内文本。
  4. **文件操作**：打开 / 保存 `.mg`（`serialize`/`deserialize` 已有，roundtrip 字节一致已验收）。
  5. **热重载联动**：编辑即落盘 → 复用 B4 的 `recompileGraph()`（进程内直接调用，不必绕文件轮询）→ 编译结果状态条（OK / FAIL + 四路结果）。
  6. **（可选）预览视口**：接 renderer 出实时画面，看 baseColor 变化——依赖 B5。
- **验收形式**：A 类工具版交代——编辑器窗口截图 / 演示（拖节点 → 连线 → 改属性 → 重编译 OK 的完整操作序列），不进渲染回归总闸。
- **排期建议**：不依赖 B5，随时可开；若与 B5 并行，注意工作线粘性——**新任务 = 同 worktree（`nl-engine-material-graph`）新工作线**，先合一波主干再开干（2026-11-08 用户拍板的合并惯例）。
- **参考**：MaterialX Graph Editor（imgui-node-editor 用法权威）、UE MaterialEditor（信息架构：图区 + 属性细节面板 + 节点搜索，只读不抄）。
- **进度**：**T1-M1 ✅**（2026-11-09，`2e60906`）——拆解 1~5 全部落地：`viewer --editor <file.mg>`（经 `HudFrame.editor` 挂入 hud 的 NewFrame..Render 区间，`editor=null` 路径零改动不碰基线）、画布拖拽（读写 `Node.x/y`）、引脚拖拽连线（方向/类型校验）、`ConstantFloat.value` 即时编辑、Save / Compile（`core/material_graph/compile.cpp` emit + slangc 四路）。双后端编译通过，证据 `t1-editor-window.png`。
  **T1-M2 ✅**（2026-11-09）——右键空白加节点（落鼠标处）/ 右键节点删除 / Del 删节点（级联清边，`removeNode` 已有）/ Ctrl+S 快捷保存 / 新节点类型 **Add** 全链打通（emitter 二值运算泛化 → 编辑器 → 示例图 6 节点 5 边 → b2/b3/b4 ctest 覆盖，生成表达式 `0.2f + (0.8f * 0.5f)` 经 slangc 双后端四路编译）。坑：库对 `size<=0` 回退「占满剩余」，底部状态条须外层包 ImGui host child 预留；`NodeEditor.json` 运行时状态会污染确定性截图，拍摄前须删除。证据 `t1-editor-window.png`（已更新）。
  剩余：拆解 6（预览视口，等 B5 / S7）。
