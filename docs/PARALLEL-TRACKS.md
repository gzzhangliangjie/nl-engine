# 并行轨道登记表（PARALLEL TRACKS）

> 用途：把"现在就能和渲染主线（S0~S14 + P8 + P9）并行推进"的模块登记成一张表，避免每次讨论都从零盘一遍。
> 触发：用户 2026-11-08 明确表示——除渲染研究模块（VSM/TSR）外，也想看**非渲染方向、独立可并行**的内容（尤其 Slang 材质图）。
> 性质：**候选清单 + 并行性评估 + 三处参考口径**，不是已实现代码，也不是最终 S-PLAN。任一轨道真正立项前，仍按 `AGENTS.md` 铁律读三处参考 + 开发历史，再按 `S*-PLAN.md` 格式出计划。

---

## 0. 并行性铁律边界（先说清楚什么能并行、什么不能）

来自 `ROADMAP.md` 第 255 行（2026-11-07 固化，M0.x 序列冻结时定）：

> **不为渲染研究单开第二条并行线**——并行线会让 `kStage` 阶段检查、展示台 exe 映射、M0.9 回归总闸全部变成两份，互相打架。

由此把"并行轨道"分成两类：

| 类别 | 定义 | 能否现在就并行 | 验收/交代形式 |
|---|---|---|---|
| **A 类 · 引擎化 / 非渲染轨道** | 不进 `kStage`、不进渲染用例库的子系统（图编辑器 / 底层并发 / 动画 / 脚本 / 音频） | ✅ 可以，与渲染主线**零冲突** | 走工具版交代（展示台 tool 卡 + 真实输出证据图），**不进 bytes/near 渲染回归总闸** |
| **B 类 · 渲染研究轨道** | 改 viewer / shader、产 GPU 画面的研究 | ⚠️ 只能降级为「穿插项」，不另开并行线 | 走渲染版交代（example 归位 + cases.json + `--check` 退出 0），插主线任意两步之间 |

**结论**：你感兴趣、能"并行做"的，基本都落在 **A 类**。B 类（VSM/TSR 等渲染研究）已按穿插项/研究轨道机制排好，本表只做汇总引用，不重复展开。

---

## 1. 已定研究轨道（已进 ROADMAP，不算新开，仅汇总）

| 模块 | 落点 | 与并行性的关系 |
|---|---|---|
| GPU-Driven 间接绘制骨架 | **= S4**（硬件光栅+GPU视锥剔除+indirect draw，原文即"从本步起进入 GPU-driven"），**不重复建** | 主线本体 |
| VSM（虚拟阴影贴图） | **S15，插入 S9 之后**（合并原 `shadow-map` 穿插项，作 rasterized 阴影对照） | B 类，已排期 |
| TSR（时域超分） | **S16，拆两段**：velocity/history 基建 **S7 后**先建（喂 S12 复用 motion vector），完整算法 S7 后轻量验证，最终 slot tonemap 前 | B 类，已排期 |
| VT（虚拟纹理） | **跳过**：P9 V4 用几何 residency + 纹理随 page 流式，不引入 feedback-buffer VT | 范围决策已定 |

---

## 2. A 类 · 现在就能并行的轨道（非渲染 / 引擎化）

> 评级与阶梯沿用 `docs/NON-RENDER-ROADMAP.md`；此处补「并行就绪度」与「三处参考口径」两列，便于直接立项。

### 2.1 ⭐ 材质图 → Slang 编译（用户最感兴趣，优先级最高）

- **为什么适合并行**：= 图编辑器（ImGui 已有）+ 代码生成（Slang 单源双后端已有）两部分**现在就能开**，接入渲染留到 S7 之后 PBR pass 就绪。
- **复用你已有地基**：Slang 单源双目标（`slangc` 一份 .slang 出 SPIR-V+DXIL）、ImGui vendored v1.91.9、绑定约定（SPIR-V `slangc -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`，shader 只写 `register(b0/t0/s0)`，禁 `[[vk::binding]]`）。
- **现成可复用的真实情况（2026-11-08 调研）**：
  | 候选 | 状态 | 对你意味着什么 |
  |---|---|---|
  | **MaterialX v1.39.5**（2026-05） | Apache 2.0，**已原生支持 Slang generator**；自带 Graph Editor（ImGui + `imgui-node-editor`） | 可 vendored；但它生成的 Slang 用自己的 binding 方案，需写**适配层**对接你的固定绑定与 scene/material 结构 |
  | **PhasmaEngine** | 社区引擎（Slang + meshlet），**已做 MaterialX→Slang** | **适配层最佳参考样本**，ROADMAP 已点名 |
  | **Falcor** | NVIDIA 完整 Slang 材质系统（128B blob + `createDynamicObject` + bindless） | ❌ 无可视化节点编辑器；借鉴**数据组织**而非编辑器 |
  | vkDuck / SHADERed | 独立 shader IDE/app，支持 Slang 节点 | ❌ 是 app 不是库，且绑 Vulkan SDK，不契合 NVRHI 单源 |
  | `imgui-node-editor` | MaterialX Graph Editor 用的节点 UI 库 | ✅ 图编辑器 UI 直接吃这个，外加你已 vendored 的 ImGui |
- **两条落地路线（待立项时拍板）**：
  - **A 路**：vendored MaterialX 库，拿它的节点语义 + 生成器内核，外写一层适配到你 NVRHI 固定绑定与现有 scene/material 结构。
  - **B 路（推荐，最贴技术栈）**：以 MaterialX 节点语义为参考、**自建轻量材质图**（节点序列化 → 生成符合你 binding 约定的 `.slang` 片段 → 接现有 PBR 材质 pass），适配量更小。
- **可见里程碑阶梯**：固定管线材质常量 → 节点图数据结构 + 序列化 → 图→Slang 代码生成 → 热重载（改图即重编译） → （S7 后）接入 PBR 材质 pass。**B2~B4 已完成**（2026-11-08，tag `mg-b2b4`，验收纪要 `docs/material-graph/B2-B4-ACCEPTANCE.md`）；交互式可视化编辑器 GUI 已立为支线 T1（`docs/material-graph/SIDE-TASKS.md`），不阻塞 B5。
- **验收形式**：工具版交代——展示台 tool 卡跑"图→Slang→编译双后端"管线，一张从生成产物真实编译输出的证据图（不许手绘）；不进渲染回归总闸。
- **三处参考口径**：UE `MaterialEditor`（只读结构）/ PhasmaEngine `MaterialX→Slang` 适配（最贴）/ MaterialX 官方 Graph+Generator（语义权威）；bevy 无等价、据实标注。

### 2.2 ⭐ Job / Task 并行系统（纯底层，最独立）— **已立项**

- **状态**：2026-11-08 已立项为正式计划 `docs/p8/P8-JOB-PLAN.md`（A 类并行轨道，P8 引擎化并发地基前置）；第一个真实消费者 = 离线管线（nanite-tools / core 离线部分）。
- **为什么适合并行**：零 GPU 依赖、零渲染依赖，是地基本身，现在就能开且不与任何 S 步冲突。
- **复用地基**：C++ 标准库 + 你已有的 `core/` 模块组织；只放 `core/jobs/`，零图形符号。
- **增量阶梯**：线程池 + `parallel_for` → scoped fork-join → 接入离线管线（cluster/stat/simplify/dag 并行化）→ （可选长线）命名线程亲和 + IO/流式池（喂 P9 V4）。
- **三处参考口径（已按铁律定向读）**：bevy_tasks（`TaskPool`/`scope`/`ParallelIterator`/`IoTaskPool`，work-stealing，主参考）/ UE TaskGraph（`ENamedThreads` 命名线程 + `TGraphTask` 前置依赖 DAG + `ParallelFor`，工业终态长线对标）/ Carrot（仅 `AsyncResource.hpp` + 散落 `std::thread`，无专职 TaskGraph，反向印证别过度设计）。
- **验收纪律（A 类专属）**：不进 `kStage` / 不进 bytes-near 渲染回归；走工具版交代（展示台 tool 卡 + 真实输出证据图）；**确定性是硬判据**——离线资产并行产物必须与单线程参考 MD5 一致。
- **注意**：验收"看加速比/架构清晰"不直观，与渲染 S 轨**明确隔离**，避免范围污染（见 `NON-RENDER-ROADMAP.md` 末节）。

### 2.3 骨骼蒙皮（动画底层数学层）

- **为什么适合并行**：直接吃 mesh 管线 + 矩阵调色板，S0 起就有 Suzanne 资产可验。
- **增量阶梯**：静态姿势 → 线性混合蒙皮（LBS）→ 多 clip 基础混合（多层 LBS）。
- **注意**：动作状态机 / 混合树 / 片段采样这些"上层控制器"逻辑**不在本层**，见 **2.4 动作系统**。本层只负责"给定一组骨骼局部变换 → 算出蒙皮顶点"。
- **三处参考口径**：UE `SkeletalMesh`/`SkinnedMeshComponent` 结构 / bevy `skinning` 数据结构 / assimp（加载器，零网络可 vendored 部分）。

### 2.4 ⭐ 动作系统 / 动画控制器（纯逻辑层，并行性最高的 A 类）

- **这是什么**：驱动蒙皮的"上层控制器"——动作状态机（idle/walk/run/attack 切换）、混合树（按速度/朝向 lerp 多个 clip）、片段采样（在动画曲线上按时间 t 取局部骨骼变换）。它坐在 2.3 骨骼蒙皮之上，但**本身是纯逻辑**，不渲染也能跑、能测。
- **为什么最适合并行**：状态机 / 混合树 / 曲线采样的数学全是 CPU 确定性计算，**完全不需要 GPU、不需要 kStage、甚至不需要渲染管线**——比材质图（要等 S7 PBR 才能接）和 Job（要等消费者）更独立。逻辑层现在就能开，只有"看到角色动起来"的**视觉验收**需要蒙皮（2.3）+ 带骨骼的网格（Suzanne 无骨骼，需补一个 rigged GLB），可后挂。
- **复用地基**：`core/` 模块组织 + C++ 标准库；可选吃 Job 系统（2.2）做多 clip / 多骨骼并行采样。
- **增量阶梯（可见验收）**：
  1. 片段数据结构 + 曲线采样（AnimCurve 按 t 取骨骼局部变换，确定性单测）
  2. 线性 / 加法混合（blend two clips by weight）
  3. 动作状态机（状态 + 转移条件 + 混合时长）→ 用确定性状态序列单测
  4. 混合树（1D / 2D blend space by speed / angle）
  5. （蒙皮就绪后）接 2.3 把局部变换刷到骨骼 → 驱动蒙皮，做视觉验收
- **验收形式**：工具版交代——展示台 tool 卡跑"状态机 + 混合"管线，输出确定性骨骼变换序列做 MD5 / 数值比对（**不进渲染回归总闸**）；视觉验收待蒙皮就绪后挂。
- **三处参考口径（待立项按铁律二次确认）**：UE `Engine/Source/Runtime/Engine/Classes/Animation/`（`UAnimInstance` / `FAnimNode_StateMachine` / `UBlendSpace` / `UAnimSequence` 曲线采样 `FAnimTrack::GetBonePose`，只读结构）/ bevy_animation（`AnimationClip` / `AnimationPlayer` / `AnimationGraph` blend 节点，最贴数据组织）/ Carrot（据实标注，预计无专职动画系统）。
- **注意（范围澄清）**：若你指的是 **gameplay 技能 / 能力系统**（技能 CD、连招、buff），那是另一条 gameplay 轨道，和"动画动作"是两回事——告诉我我再另立。

### 2.5 Lua / 脚本 VM 接入

- **为什么适合并行**：完全独立子系统，参考极多。
- **增量阶梯**：绑定核心 API → 调用渲染 → 游戏逻辑脚本化。
- **三处参考口径**：sol2（C++↔Lua 桥，最常用）/ 诸多社区实现；UE/bevy 无直接等价（据实标注）。

### 2.6 3D 空间音频

- **为什么适合并行**：完全独立，不碰 GPU。
- **增量阶梯**：单声源定位 → 距离衰减 → 遮挡 → 混响卷积。
- **三处参考口径**：Steam Audio（开源，物理建模）/ bevy rodio（轻量）/ UE Audio（结构参考，只读）。

---

## 3. B 类 · 已登记穿插项（渲染研究，不另开并行线，仅汇总）

> 来源 `ROADMAP.md` 穿插项表 + 外部技术点归宿对照。这些**不是**本表的"并行新增"，列出来是为了让你一眼看清"渲染侧还有哪些可插主线"，避免和 A 类混淆。

| 穿插项 | 插在哪 | 归宿 |
|---|---|---|
| PBR 直射光 + 贴图采样 | S1~S6 之间 | S7 前置 |
| 卡通着色（toon） | 随时 | 纯展示 |
| 光栅阴影 | **已由 VSM（S9 后）升级替代** | — |
| PRT 球谐环境光 | pbr-direct 之后 | PBR 缺口廉价补法 |
| 屏幕空间 AO（SSAO） | pbr-direct 之后 | S10 GI 之前填接触暗角 |
| 后处理链（tonemap/AA/bloom） | S7 之后 | TAA motion vector 与 S12 共享 |
| SSR | S6 之后 | 复用 S6 深度金字塔 |
| VSM（虚拟阴影） | **S9 之后** | = S15 |
| TSR 基建（velocity/history） | **S7 之后** | = S16 前半段 |
| `--profile` 按 pass GPU 计时 | 视需要 | S0 可观测性 L2 |

---

## 4. 推荐第一批并行开哪几个

按"零冲突 + 最贴你地基 + 用户兴趣"排序：

1. **Job / Task 并行系统** —— **已立项** `docs/p8/P8-JOB-PLAN.md`（纯底层无依赖，第一个消费者=离线管线）；等于把 P8 引擎化的"并发地基"前置。
2. **材质图（图编辑器 + Slang 代码生成）** —— 走 B 路（自建轻量图，参考 MaterialX 语义 + PhasmaEngine 适配），**已封顶**（见 §6 与 `docs/material-graph/SIDE-TASKS.md`）。
3. **动作系统 / 动画控制器（2.4）** —— **逻辑层最独立**：状态机 + 混合树 + 曲线采样全是 CPU 确定性计算，不需要 GPU / 不碰 kStage / 不依赖任何 S 步，现在就能开并以确定性单测验收；只有"看到角色动"的视觉验收要等蒙皮（2.3）+ rigged GLB，可后挂。是三个候选里最干净的 A 类样板。

这三条 = 把 P8 引擎化的"并发地基（Job）+ 资产表达（材质图）+ 角色驱动（动作系统）"三块最独立子系统并行啃掉，不影响 S0~S14 主线节奏。若按 worktree 隔离，对应 `feat/job-system` / `feat/material-graph` / `feat/action-system` 三个独立工作树。

---

## 5. 立项纪律（铁律，复用不重写）

- 任一 A 类轨道立项前，照例**先读三处参考**（Carrot / bevy / UE5）+ 开发历史（`MILESTONES.md` / `carrot-history.md` / `MEMORY.md` / 当日日志 / 当前 `S*-PLAN.md`），带证据做决断。
- 计划格式统一按 `docs/s*/S*-PLAN.md`（目标 / 每步验收判据 / 三处对照 / 双后端纪律）。
- **A 类走工具版交代**：展示台 tool 卡 + 真实输出证据图，不进渲染 `bytes/near` 总闸；不动 S0~S14 编号。
- 三处无对应实现的（如 bevy/Carrot 无 VSM/TSR/材质图等价），开工时**二次确认**并据实标注，不凭已知范围直接下结论。

---

## 6. 后续动作（待用户圈定）

- **Job 系统已立项**：`docs/p8/P8-JOB-PLAN.md`（M-J0~M-J4，首个消费者=离线管线）；下一步可执行 M-J0。
- **材质图已封顶（B 路自建，2026-11-09 用户拍板「及时收手」）**：B1~B4 主线阶梯 + T1 编辑器（拆解 1~5，`viewer --editor`）+ 节点库 10 种 + uniform 曝光（`MaterialParams` cbuffer b1，运行时绑定留 B5）全部完成；定位=**调试工具级**的可调参表面材质薄层（与 UE 分层一致，复杂 shader 手写与图无关），UI 不再投入。见 `docs/material-graph/SIDE-TASKS.md`（含终点定义）；B5 接 PBR pass 排 S7 后（轨道唯一遗留 = T1 拆解 6 预览视口）。
- **动作系统已立项并完成纯逻辑层（2.4）**：2026-11-08 用户拍板 A 路自建（网上调研结论：状态机/混合树层业界无现成库，ozz 只覆盖下层采样/混合/蒙皮数学）；M-A1 曲线采样 / M-A2 线性加法混合 / M-A3 状态机（UE 回退缩时）/ M-A4 1D blend space 全部完成并过确定性自测 + ctest 5/5 + 展示台 ma-track 卡，tag `anim-ma1`~`anim-ma4`（track 分支内，merge 走 §7.2 文档闸）。**M-A5 按重定义推进中**（先建蒙皮再视觉验收）：**M-A5.1 CPU 蒙皮层已落地**（`cde41c7`：`nl/anim/skinning` LBS + glTF 蒙皮数据入口 + CesiumMan rest 姿态端到端验收）。
- 其余 A 类轨道（骨骼蒙皮 2.3 / Lua 2.5 / 3D 音频 2.6）仍为候选，立项前按铁律读三处 + 出计划。

> 注：三个 worktree 已于 2026-11-08 **物理建立**（见 §7.1 实例注册表），但各轨道计划仍按 §5 立项纪律补，**未立项前不开干**。

---

## 7. track 工作纪律（固定 feat worktree 模式）

> 2026-11-08 用户拍板：本项目**采用固定的 feat worktree 模式**——每条并行轨道对应一个长期 `feat/*` 分支 + 一个常驻 worktree，不动态建临时分支。理由：单用户开发、无多进程抢坑，固定分支 + 工作线粘性已满足"互不干扰"，不必引入更复杂的调度机制。

### 7.1 实例注册表

每个 worktree 必须登记占用说明，避免"正在做但未提交"时自己三查抓瞎。

| 轨道 | worktree 绝对路径 | 分支 | 状态 | 占用说明 |
|---|---|---|---|---|
| 材质图 → Slang | `E:/UE_STUDY_2026/nl-engine-material-graph` | `feat/material-graph` | **已立项 / B2~B4 完成**：B 路自建，计划 `docs/material-graph/B3-PLAN.md`，验收纪要 `docs/material-graph/B2-B4-ACCEPTANCE.md` | B2 数据模型+序列化 / B3 图→Slang 双后端编译 / B4 热重载+证据图均已过 ctest；B5 接 PBR pass 排 S7 后；共享 `.workbuddy`+`slang/dist` junction |
| Job / Task 并行 | ~~`E:/UE_STUDY_2026/nl-engine-job-system`~~（worktree 已移除，2026-11-09） | `feat/job-system`（保留） | **已收官**：M-J0~J3 完成（tags job-pool/job-scope/job-cluster/job-offline），已双向合并回 master，可视化证据图补交完成；M-J4 可选不排期（等 P9 V4） | 全部内容在 master `c136bcd`+；P9 V4 需要流式加载时按 §7.1b 重建 worktree |
| 动作系统 / 动画控制器 | `E:/UE_STUDY_2026/nl-engine-action-system` | `feat/action-system` | **已立项 / M-A0~M-A4 完成 + M-A5.1 CPU 蒙皮落地（track 分支，未 merge）**：计划 `docs/action-system/ACTION-PLAN.md`（在分支上），tag `anim-ma1`~`anim-ma4d` | nl/anim 纯逻辑层（采样/混合/状态机/1D blend space）+ `nl/anim/skinning`（LBS CPU 蒙皮）自测全绿；M-A5 后续（GPU 调色板→视觉验收）进行中；共享 junction |
| 开发者控制台 | `E:/UE_STUDY_2026/nl-engine-console` | `feat/console` | **进行中 / M-C0~C1 完成**（2026-11-09，track 分支 b4787da）：纯逻辑内核 + ImGui 前端（呼出/日志配色/历史/Tab 补全），双后端证据截图 | M-C0~C3：`core/console/` 纯逻辑内核 → ImGui 前端 → RuntimeOption 运行时调参；不依赖场景整合；共享 junction |

> 四个已建 worktree 均已 `junction` 共享主 worktree 的 `.workbuddy` 与 `thirdparty/slang/dist`，且 `.workbuddy` 已 `skip-worktree`（git status 干净，不会误把纪律源 commit 进 feature 分支）。

### 7.1b 待建 worktree（已立项，执行排 P8 场景整合之后）

2026-11-08 用户拍板：寻路 / 物理 / 3D 音频**依赖场景整合**，先立项出计划，**执行放到 P8 E1（ECS-lite 场景）验收之后**；届时按 §7 模式新建 worktree：

| 轨道 | 届时 worktree 绝对路径 | 分支（届时建） | 计划 |
|---|---|---|---|
| 寻路（NavMesh + A\*，从零写） | `E:/UE_STUDY_2026/nl-engine-pathfinding` | `feat/pathfinding` | 已立项 `docs/p8/P8-NAV-PLAN.md` |
| 物理（Jolt vendored 集成） | `E:/UE_STUDY_2026/nl-engine-physics` | `feat/physics` | 已立项 `docs/p8/P8-PHYS-PLAN.md` |
| 3D 空间音频（OpenAL Soft + 从零集成层） | `E:/UE_STUDY_2026/nl-engine-audio` | `feat/audio` | 已立项 `docs/p8/P8-AUDIO-PLAN.md` |

### 7.2 固化文档闸

track 分支 **merge 回 master 前**，必须在本轨道 `S*-PLAN` / `MILESTONES.md` 写验收纪要（目标达成 / 判据 / 证据 / 踩坑），否则不 merge。坑点只活在工作记忆 = 下个会话必重踩；我们已有 `MILESTONES.md` 四栏写实，此处**纪律化**为 merge 前置。

### 7.3 假绿防护 · 占坑 ≠ 文件指向

在 track worktree 干活时：
- 所有编辑工具的绝对路径、shell 每条命令的 cwd **显式在对应 wt 路径内**（`cd /e/UE_STUDY_2026/nl-engine-job-system` 开头），不依赖默认落点；
- merge 回 master 前**双向自查**：`git -C <wt> status` 看得到本批改动、主 worktree `git status` **没有**本批改动（出现即落错树，立即迁移）；
- 验证可信度自查：改了什么就在产物里找什么——文本/数值类改动后若无任何差异是红旗不是绿灯。

### 7.4 工作线粘性

同一轨道回同一 worktree，一条工作线**只 merge 一次**（验收 FAIL→补丁→复验全部在占坑内闭环，不中途 merge 又裸干）。任务收尾才 merge 回 master，下个任务回同一坑。

### 7.5 收尾即同步关联文档（2026-11-08 用户拍板固化）

worktree 内每完成一个里程碑 / 阶段，除在 track 分支写本轨道验收纪要（§7.2）外，**必须同步更新 master 侧关联文档**，不许让 master 侧文档停在过时值：

- `AGENTS.md` 并行轨道注册表（轨道状态列）；
- 本文件 §7.1 实例注册表（状态 / 占用说明列）；
- 相关候选 / 计划文档的状态标注（如 `NON-RENDER-ROADMAP.md` 候选总表）。

注册表与跨轨道文档的更新在**主 worktree 提交**，不进 feature 分支（避免双份冲突）。教训留痕：动作系统 M-A5.1（CPU 蒙皮）落地后，master 侧三处注册表仍停在「M-A4 / 候选」，下个会话即误判进度。
