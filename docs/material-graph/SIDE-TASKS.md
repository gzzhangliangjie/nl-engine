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
  **T1-M3 ✅ / 轨道封顶**（2026-11-09，用户拍板「及时收手」）——编辑器定位确定为**调试工具级**（服务引擎开发者调材质/验链路，不追 Blender/MaterialX Editor 级 UX，UI 不再投入）。本轮内容全在**代码生成层**：节点库扩到 **10 种**（+Subtract/Divide/Power/Lerp/OneMinus/Abs/Clamp，Clamp 的 min/max 走属性）；**uniform 曝光**（ConstantFloat 带 `uniform=1` 进 `cbuffer MaterialParams : register(b1)`，编辑器勾选框 + uname 输入；运行时绑定接 PBR 属 B5）；`kitchenSink` 全算子图入 b3 check 真实编译。顺带修掉两个潜伏 bug：① emitter memo 泄漏——上游输出被多条边引用时二次引用把内部 key（`4:6`）写进生成代码（B3 起、示例图单引用未触发）；② 序列化 props 按 `unordered_map` 迭代序写出，多属性节点 roundtrip 不字节一致——改按 key 排序。示例图升级 9 节点 9 边（`lerp(0.2+0.8*0.5, 1-0.2, uTint)`=0.65）。ctest b2/b3/b4 3/3 绿，双后端编译通过。
  剩余：拆解 6（预览视口，等 B5 / S7）——这是轨道唯一遗留项。

## 轨道终点定义（2026-11-09 封顶拍板）

- **定位**：材质图 = **可调参的表面材质薄层**（与 UE 分层一致：复杂 shader 框架手写 `.slang`，图只覆盖表面参数层的可视化调参 + 像素表达式生成 + 热重载）。复杂 shader（VSM/TSR/indirect 等）与图零关系。
- **已完成**：B1~B4 主线阶梯 + T1 编辑器（拆解 1~5）+ 节点库 10 种 + uniform 曝光。
- **不再投入**：编辑器 UI 新功能（undo/redo、搜索调板、缩略图预览等明确不做）；节点库按需增量登记（每种 = emitter 一分支 + 单测 + 配色一行的成本）。
- **唯一遗留**：T1 拆解 6 预览视口（依赖 B5 / S7）；B5 本体（接 PBR 材质 pass + MaterialParams 运行时绑定）排 S7 后。
