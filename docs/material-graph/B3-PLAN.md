# B3-PLAN：图 → Slang 代码生成（材质图轨道）

> 轨道：`feat/material-graph`（A 类，非渲染，不进 kStage / 不进 bytes-near 渲染总闸）
> 路线：PARALLEL-TRACKS.md §2.1 B 路（自建轻量材质图 → 生成符合绑定约定的 .slang → 接现有 PBR 材质 pass）
> 前置：B2 已完成（节点图数据模型 + 序列化 + material-graph-tools，commit 1a912fa）

## 三处参考（立项铁律，已定向读）
- **PhasmaEngine**（`christoskaramou/PhasmaEngine`，最贴样本）：MaterialX `.mtlx` 文档经 MaterialX `SlangShaderGenerator` 编译成**各自的独立 Slang shader**，跑在 visibility buffer / forward / RT hit group。印证「图 → 一份 .slang」单源双目标可行，且**每个图编译成独立 shader 文件**（而非塞进统一大 shader）。
- **MaterialX `SlangShaderGenerator`**（官方，语义权威）：`generate(name, element, context)` 把元素及其上游依赖翻译成 shader 源码；分段 `emitVertexStage` / `emitPixelStage` / `emitUniforms` / `emitInputs` / `emitOutputs`；类型系统经 `toVec4` 归一。我们**不 vendored MaterialX**（A 路否定），只借用其「顶点/像素分段 + uniform/input/output 发射」的语义骨架，自建更薄的 emitter。
- **UE `MaterialEditor`**（只读结构）：节点图 + 上游 DAG 求值的编辑器结构；我们只取「从 Output 节点向上游递归求值」的求值模型，不抄任何 UE 代码。

## 目标（本计划范围）
把 `MaterialGraph`（B2 格式）编译成一份**符合本项目绑定约定**的 `.slang`：
- 复用 `model.slang` 的全部约定：命名入口 `vsMain`/`psMain`、`register(b0/t0/s0)`、`cbuffer SceneConstants`（即「固定管线材质常量」，B 路阶梯第 1 步在此落地）、SPIR-V 绑定平移由 slangc 命令行 `-fvk-b-shift` 负责（shader 内只写 register）。
- 首版内置节点：`ConstantFloat` / `Multiply` / `Output(baseColor: float)`。从 `Output` 节点向上游 DAG 递归求值，生成像素阶段表达式；顶点阶段为 `model.slang` 同构的 passthrough（position/normal/uv）。

## 可运行用例（验收用）
- `material-graph-tools gen <in.mg> <out.slang>`：把图写成 `.slang`。
- `material-graph-tools check`：内部装配示例图 → emit → 调用 `slangc` 对生成产物跑**双后端**（SPIR-V + DXIL）×（vsMain + psMain），全部 exit 0 才报 `genslang: OK`。

## 验收判据（A 类专属，工具版交代）
- 展示台 `kind:"tool"` 用例跑「图 → Slang → 编译双后端」管线；证据图 = **生成产物经 slangc 真实编译的输出**（编译成功 + 产物存在），不发手绘图。
- `ctest b3-graph-genslang`：env 注入 `NL_SLANG_BIN` / `NL_DXC_DIR`，跑 `check`，exit 0 即绿。
- 负向：无 `Output` 节点 / 类型不支持 → `emitSlang` 返回 false 且 err 非空。

## 不变量
- 生成器是**纯函数 + 确定性**：同一图两次 emit 字节一致（与 B2 序列化同纪律）。
- emitter 零图形 API、零第三方依赖，落在 `core/src/material_graph/emit.cpp`，与 `graph.cpp` 同库。
- 生成的 `.slang` 不引入 `[[vk::binding]]`、不使用任何 GLSL 注解，绑定完全交给 slangc 命令行。

## 不在本计划范围（后续阶梯）
- 热重载（B4）：改图即重编译 —— 依赖 B3 的 emit + slangc 调用链路，作为自然延伸。
- 接入 PBR 材质 pass（B5，S7 之后）：把生成的 baseColor 接到现有 PBR 光照，而非现在的纯色输出。
- 扩充节点库（纹理采样 / 法线混合 / 数学节点）：按需求增量加，每个新节点类型一个 emitter 分支 + 单测。
