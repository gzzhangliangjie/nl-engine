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
| TSR（时域超分） | **S16，拆两段**：velocity/history 基建 **S7 后**先建（喂 S12 复用 motion vector），完整算法 S7 后轻量验证，最终 slot tonemap 前 | B 类，已排期。**2026-11-09 预研补**：jitter(Halton)+双帧矩阵+渲染/输出分辨率分离并入 M16.0；velocity 统一为完整逐像素版（S12 复用同一张，不是相机版）；**离屏 HDR RT + post-chain 为 SSR/S16/bloom 三家共享前置**，归 ROADMAP「后处理链」行只建一份；结论见 `s16/S16-PLAN.md` 预研结论节、UE-DIFFS D12 |
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
- **可见里程碑阶梯**：固定管线材质常量 → 节点图数据结构 + 序列化 → 图→Slang 代码生成 → 热重载（改图即重编译） → （S7 后）接入 PBR 材质 pass。**B2~B4 已完成**（2026-11-08，tag `mg-b2b4`，验收纪要 `docs/material-graph/B2-B4-ACCEPTANCE.md`）；支线 T1 交互式编辑器已完成封顶（2026-11-09，tag `mg-t1`，M1~M3 见 `docs/material-graph/SIDE-TASKS.md`，唯一遗留 = 拆解 6 预览视口等 B5）。
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

### 2.6 3D 空间音频（2026-11-09 方案变更：vendored miniaudio）

- **为什么适合并行**：完全独立，不碰 GPU；A 类，不进渲染回归。
- **增量阶梯（重写后 4 步）**：单声源播放 + 离线确定性 dump → 3D 定位/距离衰减/多声源 → 游戏内调试（console cvar + ImGui 声源面板）→ E1 场景集成。
- **三处参考口径**：UE `AudioDebug.cpp`（只读定调试字段）/ Carrot `engine/audio/`（对象模型与绑定形状，不抄 AudioThread 与 OpenAL 底座）/ bevy `SpatialListener`（ECS 同步挂法）；上游 miniaudio 官方示例清单见 `P8-AUDIO-PLAN.md` §3。
- **不在范围**：HRTF / 几何遮挡 / EFX 混响卷积 / Steam Audio / FMOD/Wwise 内容管线（目标只要「3D 音效 + 能调试」）。

### 2.7 游戏循环 / 双帧模型（渲染帧 vs 逻辑帧，2026-11-09 立项登记）

- **为什么适合并行**：纯逻辑层（GameClock + 主循环改造），不碰 GPU 管线；A 类，不进渲染回归。
- **拍板结论（2026-11-09 用户同意三点）**：逻辑帧固定 60Hz + accumulator、渲染帧 vsync 驱动；起步不插值；M-GL2 与 M-S3 场景集成合并收口。暂停 = 逻辑帧时钟停摆、渲染帧照常。
- **Gate**：E1b 链 merge 回 master 之后动工（已解除）。里程碑草案 M-GL0（GameClock+selftest）→ M-GL1（viewer 接双帧 + console）→ M-GL2（接物理/音频/ECS）。
- **三处参考口径**：Carrot `PhysicsSystem` accumulator（实读，cpp:130-138）/ bevy `Time<Fixed>`+`Time<Virtual>`+max_delta 250ms（实读）/ UE `UnrealEngine.cpp FEngineLoop::Tick`（M-GL0 开工已补实读：渲染帧为主流派，维持逻辑帧为主拍板）；详见 `docs/p8/P8-GAMELOOP-PLAN.md`。
- **状态（2026-11-10）**：**M-GL0 已收口**（tag `gl-m0`，`nl::core::GameClock` + gl-tools selftest 30 锚点 + verify.py 独立孪生对拍全绿）；下一站 M-GL1。

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
| 材质图 → Slang | ~~`E:/UE_STUDY_2026/nl-engine-material-graph`~~（worktree 已移除，2026-11-09） | `feat/material-graph`（保留） | **已收官 / 全量 merge 回 master**（tag `mg-b2b4`(B2~B4) + `mg-t1`(T1 编辑器 M1~M3)，merge commit `e5505bc`）：B 路全阶梯——数据模型+序列化 / 图→Slang 双后端编译 / 热重载 / 交互式节点编辑器（imgui-node-editor v0.9.4，`viewer --editor`）+ 节点库 10 种 + uniform 曝光（`MaterialParams` b1，运行时绑定留 B5）；验收纪要 `docs/material-graph/B2-B4-ACCEPTANCE.md` + MILESTONES T1 节 + `SIDE-TASKS.md`（终点定义：调试工具级，UI 封顶）；主干 `viewer --editor` 冒烟截图验收（HUD+编辑器+场景叠加正常） | 全部内容在 master `e5505bc`+；B5 接 PBR pass + T1-6 预览视口排 S7 后，届时按 §7.1b 重建 worktree |
| Job / Task 并行 | ~~`E:/UE_STUDY_2026/nl-engine-job-system`~~（worktree 已移除，2026-11-09） | `feat/job-system`（保留） | **已收官 / 全量 merge 回 master**（merge commit `fb2b4f4`，2026-11-09，tags `m-j5a`/`m-j5b`/`m-j5c-prep`，M-J0~J4 此前已双向合并）：M-J5a vendored Taskflow 3.11.0 显式依赖 DAG + 4 路 slangc fan-in；M-J5b 进程级常驻 `Scheduler`（compute/io 分池，configure 冻结、实例后 no-op，老 `ThreadPool` 兼容层一行不动）；M-J5c-prep 动态依赖计数 job + Barrier（Jolt `JobSystem` 三支柱合约同构桩、零 Jolt 头，池侧仅加 `post()`/`runOne()`）。selftest：jobs 25 / scheduler 11 / jobgraph 9 / jobsync 20 全绿；原 M-J5c 收窄为「physics/ECS 各自树内纯绑定」（适配器 = `JoltJobSystem : JPH::JobSystemWithBarrier` 映射到 jobsync 原语；ECS system 调度走既有 jobgraph）。M-P0 逐样本位一致契约未动，翻物理并行开关须用户单独拍板 | 计划 `docs/p8/P8-JOB-PLAN.md`（含 M-J5b/M-J5c-prep 落地纪要与生命周期契约）；M-J4 不排期（P9 V4） |
| 动作系统 / 动画控制器 | ~~`E:/UE_STUDY_2026/nl-engine-action-system`~~（worktree 已移除，2026-11-09） | `feat/action-system`（保留） | **已收官 / M-A0~M-A5.4 全量 merge 回 master**（merge commit `5a1912f`，2026-11-09，tag `anim-ma1`~`anim-ma5c`）：nl/anim 纯逻辑层（采样/混合/状态机/1D blend space/FK/CPU LBS 蒙皮）+ anim-tools 自测 + viewer skin 模式 + skindump/skin-demo 四资产演示页（CesiumMan/Fox Walk/Fox Run/RiggedFigure）+ Fox.glb 链路三修复；验收纪要 MILESTONES M-A 节 + ACTION-PLAN + ANIM-TOOLS-GUIDE；GPU 调色板等更大规模蒙皮资产再立项 |
| 游戏循环 / 双帧模型 | `E:/UE_STUDY_2026/nl-engine-gameloop`（已建，2026-11-10，junction 共享 `.workbuddy`/`slang dist`） | `feat/gameloop` | **M-GL0 已收口**（tag `gl-m0`，2026-11-10）：`nl::core::GameClock`（60Hz accumulator + 250ms 截断 + bevy 暂停语义 + alpha 预留）+ `gl-tools`（selftest 30 锚点 + trace TSV）+ verify.py 独立 python 孪生逐行对拍全等 + example/gl0 工具版交代全套 + ctest 双后端；下一站 M-GL1（viewer 接双帧 + game.pause/timeScale console）；计划 `docs/p8/P8-GAMELOOP-PLAN.md` + `GAMELOOP-TOOLS-GUIDE.md` |
| 开发者控制台 | ~~`E:/UE_STUDY_2026/nl-engine-console`~~（worktree 拆除待批准，merge 已落地 2026-11-09） | `feat/console`（保留） | **已收官 / M-C0~C3 全量 merge 回 master**（merge commit `9e12cc6`，2026-11-09，tag `console-mc3`）：`core/console/` 纯逻辑内核（CVar 四类型 + ExecCode 四档退出码）+ ImGui 前端（`` ` `` 呼出 / Tab 补全 / ↑↓ 历史 / 可拖动缩放 / IME 解绑）+ installBuiltins（help/cvar.list/cvar.set）+ cam.*/hud.show 真参数回调 + panelShow 解耦面板与控制台；selftest 49/49、双后端构建+selftest 绿、展示台 console-mc3 卡五用例实跑（expectExit 2/0/0/1/3）；验收纪要 MILESTONES P8 M-C 节 + `docs/p8/CONSOLE-TOOLS-GUIDE.md` + `docs/screenshots/CONSOLE-EVIDENCE.md`；M-SC1 console 绑定解锁 | M-C0~C3 计划 `docs/p8/P8-CONSOLE-PLAN.md`；共享 junction；M-C4 级别增强（模糊补全/日志导出）需要时另立项 |
| PBR 直射光 + 贴图采样（B 类穿插项专列） | ~~`E:/UE_STUDY_2026/nl-engine-pbr-direct`~~（worktree 已移除，2026-11-09） | `feat/pbr-direct`（保留） | **已收官**（2026-11-09，tag `pbr-direct`，merge commit `e57173d` 全量合入 master）：mode 5 = Cook-Torrance 直射光（只编译进 psMain）+ t1 MR 贴图 + b1 LightConstants 80B + `--pbr-naive` sRGB 对照；双后端 bytes、textured 不变量不变、example/pbr-direct 5 用例；kStage=`pbr-direct`（17 对 exe=current HUD 基线随 bump 重拍）。**merge 后 PRT / Kulla-Conty 解锁，S7 只剩 vis buffer + vertex pulling + 材质解析** | 计划 `docs/s0/PBR-DIRECT-PLAN.md`（含收口偏差 6 条留痕）；验收纪要 `docs/MILESTONES.md` pbr-direct 节 |

> 四个已建 worktree 均已 `junction` 共享主 worktree 的 `.workbuddy` 与 `thirdparty/slang/dist`，且 `.workbuddy` 已 `skip-worktree`（git status 干净，不会误把纪律源 commit 进 feature 分支）。

### 7.1b 待建 worktree（已立项，执行排 P8 场景整合之后）

2026-11-08 用户拍板：寻路 / 物理 / 3D 音频**依赖场景整合**，先立项出计划，**执行放到 P8 E1（ECS-lite 场景）验收之后**；届时按 §7 模式新建 worktree。**2026-11-09 用户指令提前解锁物理轨道**（见 §7.1e），寻路 / 音频仍排其后。

| 轨道 | 届时 worktree 绝对路径 | 分支（届时建） | 计划 |
|---|---|---|---|
| 寻路（vendored Recast/Detour，2026-11-09 方案变更） | `E:/UE_STUDY_2026/nl-engine-pathfinding` | `feat/pathfinding` | 已立项 `docs/p8/P8-NAV-PLAN.md`（旧「从零写」6 步阶梯作废；新阶梯 = vendored + 封装 + 编辑器结合 + E1 集成） |
| ~~物理（Jolt vendored 集成）~~ | ~~`E:/UE_STUDY_2026/nl-engine-physics`~~（**已拆**，2026-11-10；M-P5 gate E3，届时开新 wt 接入） | `feat/physics`（**分支保留**） | **全量 merge 回 master**（2026-11-10，merge `7d844ef`，M-P0~P4 + M-J5c，tags `phys-p0`~`phys-j5c` 随入）；见 §7.1e；`docs/p8/P8-PHYS-PLAN.md` |
| 3D 空间音频 + 游戏内调试（**vendored miniaudio + 自写薄集成层**，2026-11-09 方案变更） | ~~`E:/UE_STUDY_2026/nl-engine-audio`~~（**已拆**，2026-11-09；M-S3 后续开新 wt 接入） | `feat/audio`（**分支保留**） | **M-S0/M-S1/M-S2 已 merge 回 master**（2026-11-09，merge `1aac3d2`）；剩 M-S3 场景集成 gate 在 E1/E1b 后；`docs/p8/P8-AUDIO-PLAN.md` |

### 7.1e 物理轨道（2026-11-09 提前开工，M-P0~M-P3）

| 轨道 | worktree 绝对路径 | 分支 | 状态 |
|---|---|---|---|
| 物理（Jolt vendored 集成） | ~~`E:/UE_STUDY_2026/nl-engine-physics`~~（**已拆**，2026-11-10——merge `7d844ef` 回 master 后回收；拆时 junction `.workbuddy`/`slang dist` 先以 PowerShell `[IO.Directory]::Delete($path,$false)` 摘链接再 force remove，主树共享内容核对无损） | `feat/physics`（**分支保留**） | **M-P0 / M-P1 / M-P2 / M-P3 / M-P4 收口（tags `phys-p0` / `phys-p1` / `phys-p2` / `phys-p3` / `phys-p4`）+ M-J5c 并行物理（tag `phys-j5c`，PoolJobSystem 适配器，N=1/N=4/串行逐位 IDENTICAL）**：vendored Jolt v5.3.1 + `nl/physics` 最小世界 + 4 层碰撞矩阵与 owner↔BodyID 双向关联 + 6 箱体堆叠稳定/休眠岛三用例 + 碰撞形状调试线框接 viewer + 角色胶囊走查（CharacterVirtual，CharacterMovement 式扫掠/地面检测/斜坡台阶，动作系统输入接口预留）；`phys-tools` layers/check-layers/stack/check-stack/debug/check-debug/character/check-character/check-jobs；M-P0 落球 + M-P1 层矩阵 + M-P2 堆叠 + M-P3 debug + M-P4 character 五基线逐字节回归锁死；**轨道已收官 merge 回 master（2026-11-10），wt 已拆**；A 类工具版交代，不进 `kStage`/渲染回归 |

- 用户指令「建 feat/physics 开发物理模块内容」= 提前解锁排期闸（计划 §排期约束括号内已允许 M-P0~M-P3 与 E1 并行）；**M-P5 场景集成仍 gate 在 P8 E3（序列化 + Inspector）之后**，不随本轮解锁。
- 三处口径二次核实（2026-11-09）：Carrot `engine/physics/` 2902 行 Jolt 集成层 + 本地 vendored JoltPhysics **v5.3.1**；bevy 本体无物理 crate（仅 `examples/movement/physics_in_fixed_timestep.rs` 示例），生态 avian，不作口径；UE 5.8.4 只有自研 **Chaos**（`Runtime/Experimental/Chaos*` + `PhysicsCore/` + `Engine/Classes/PhysicsEngine/`），**全树无 Jolt 后端、ThirdParty 下亦无 PhysX SDK**——立项文档里「UE 官方提供 Jolt 后端」为误记，已勘误并落 `UE-DIFFS.md` D10。
- **M-P1 状态（2026-11-09）**：4 层对称矩阵（对象层与宽相层 1:1）、userData 索引式双向关联、四用例拦住/穿透轨迹、M-P0 基线 MD5 逐字节一致；见 `example/phys-p1/` 与 `MILESTONES.md` M-P1 四栏。
- **M-P3 状态（2026-11-09）**：碰撞形状调试线框接 viewer（地板蓝/盒绿/球橙/胶囊品红，25488 顶点逐帧恒定，深度遮挡正确）；修复 Vulkan 共享 builder entryName 硬编码（NVRHI Vulkan 把 entryName 写进 `vkPipelineShaderStageCreateInfo::pName`，传 `vsMain` 致管线创建期 VK_ERROR_UNKNOWN，VVL 静默）→ §2.4 已扩充；双后端 `deviceErrors=0`，线框互比 599px 记 FAIL 档（薄线噪声先例口径）；四基线逐字节回归锁死；见 `example/phys-p3/`、`MILESTONES.md` M-P3 四栏与 `PHYS-TOOLS-GUIDE.md` §10。
- **M-P4 状态（2026-11-09）**：角色胶囊走查（Jolt `CharacterVirtual`，CharacterMovement 式——Carrot 的 `JPH::Character` 真实体式不采用，计划口径为扫掠式）；走查场地板→30° 斜坡→3 级台阶→60° 斜坡挡住（场地中心化 x−13 适配 OrbitCamera 取景）；mPosition 语义从源码钉死（胶囊中心=py、脚底=py−1.08、贴地=top+1.10）；重力/输入应用侧合成（Jolt 约定）；双后端 `deviceErrors=0`、互比 1103px 记 FAIL 档（薄线噪声）；五基线逐字节回归锁死；`CharacterConfig.walkSpeed`/`groundState` 为动作系统（M-A 轨道）衔接预留接口；见 `example/phys-p4/`、`MILESTONES.md` M-P4 四栏与 `PHYS-TOOLS-GUIDE.md` §11。**M-P5 场景集成仍 gate P8 E3**。
- **M-J5c 状态（2026-11-09，job 计划穿插项落在 physics 轨道，tag `phys-j5c`）**：`PoolJobSystem`（继承 `JPH::JobSystemWithBarrier`）把 `PhysicsSystem::Update` 映射到 nl/core ThreadPool——依赖计数留在 `JPH::Job` 内部、QueueJob 按 Jolt 合约 AddRef 投递/Execute+Release 成对、Barrier Wait 帮跑由 Jolt 基类兜底；`DropConfig/StackConfig.jobsThreads`（0=基线默认，>=1=适配器）。**两层验收全绿且逐字节**：N=1 vs SingleThreaded 硬证明、N=4 run-to-run、N=4 vs 串行（岛间无共享累加实测坐实）；`simulate --jobs 1` 全文件与 M-P0 drop.tsv 一致；五基线零扰动；ctest 物理+j5c 15/15 双后端。底座用专用 ThreadPool(N)（Scheduler configure 冻结不允许多档对拍，偏差留档）；ECS system 调度接入仍待 ECS 轨道。见 `example/phys-j5c/`、`PHYS-TOOLS-GUIDE.md` §12、P8-JOB-PLAN M-J5c 收口纪要。
- **选型二次拍板（2026-11-09）**：用户「物理用现成的、没必要从头写、成本太高；纠结的只是用哪个」→ 不自研方向不变，**继续走 Jolt 5.3.1**；理由为**成本取舍 + 工程适配**（本机唯一可得 / MIT / 纯 CPU / 确定性 / 自带接口 / Carrot 集成层参照）。UE 口径不参与选型，仅作只读对照。**「例外理由」旧措辞按 2026-11-09 四修作废**：`AGENTS.md` 选型闸后，vendored 成熟库是通例而非例外。候选对比与换库代价见 `NON-RENDER-ROADMAP.md` 头部修订与 `P8-PHYS-PLAN.md` §选型评审结论。

### 7.1c ECS-lite 场景轨道（2026-11-09 立项，E1a 先行）

> 2026-11-09 用户拍板：ECS（P8 E1）是下游三轨的瓶颈，**E1a（纯逻辑内核）提前开 worktree**，E1b（渲染集成）后挂等 S6 收官。计划 `docs/p8/P8-ECS-PLAN.md`（M-E0~E3，Carrot 式 OOP 组件路线）。

| 轨道 | worktree 绝对路径 | 分支 | 计划 |
|---|---|---|---|
| ECS-lite 场景 | ~~`E:/UE_STUDY_2026/nl-engine-ecs`~~（**已拆**，2026-11-09：junction 两处 PowerShell 原生摘除 + `git worktree remove`，分支保留） | `feat/ecs`（**分支保留**） | **E1a + E1b 链全收官并 merge 回 master**（2026-11-09：E1a merge `2d4ad49`；E1b 链 merge `e2ac82c` + 收口闸 `6560b2c`，tags `ecs-e0`~`ecs-e1b.5`，全量回归 **406/406 ALL GREEN**）。`nl/scene` 内核 + 渲染集成（5 物体→50 物体规模锚/点光/逐实例 Cook-Torrance/2D 阴影/并行 tick）+ `ecs-e1b`/`ecs-parallel` 展示台卡 + `docs/p8/ECS-TOOLS-GUIDE.md`。收口闸修存量债 6 类（S7.1 HUD 基线漏拍 32 张、harness case 级 exe/cwd 覆盖、audio-s1 expectLog 过期、m-sc1 负向失效），详见 `docs/MILESTONES.md` merge 段 |

### 7.1d C# 脚本轨道（2026-11-09 立项，E4 脚本段解禁提前）

> 2026-11-09 用户拍板（三轮收敛：Lua → 鸣潮参考 TS+C# → **定 C#**「热更也已成熟」）：E4 脚本段解禁提前，目标 = 功能扩展 + 热更，技术路线 = **.NET hosting（hostfxr）+ C# 脚本程序集 + collectible ALC 热更**（鸣潮 3.6 同款方向）。计划 `docs/p8/P8-SCRIPT-PLAN.md`（M-SC0~SC3）；M-SC0 无依赖即刻开工，M-SC1 gated on console M-C3 merge，M-SC2 消费 E1a 成果。

| 轨道 | worktree 绝对路径 | 分支 | 计划 |
|---|---|---|---|
| C# 脚本 | `E:/UE_STUDY_2026/nl-engine-script`（**已建**，wt_pool track `script` active；junction 共享 `.workbuddy`/`slang dist`） | `feat/script` | M-SC0~SC3（`docs/p8/P8-SCRIPT-PLAN.md`）：hosting 内核 → console 绑定 → ECS 数据面绑定 → 热更收口。**M-SC0 已收口（2026-11-09，tag `m-sc0`）**：`nl::script::Vm` hosting 内核 + `nl-script` 工具（selftest 17/17 含热重编行为更新）+ 展示台 m-sc0 卡 + SCRIPT-TOOLS-GUIDE |

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

### 7.6 对话归属登记 = wt_pool 占坑闸（2026-11-09 用户拍板，工具 `tools/wt_pool.py`）

起因：2026-11-09 事故——动作系统会话越轨删除了其他轨道的 worktree。教训机械化的产物：**任务号 = 里程碑编号**（用户拍板：里程碑编号本来就关联 wt，不另设任务编号体系）。

- **对话开场归属**：每个对话开场声明里程碑编号，没给就提示补充，补上后 `python tools/wt_pool.py claim --track <轨道> --milestone <编号>` 登记（幂等，同里程碑重入无害）。
- **动 wt 前过闸**：编辑 / merge / 删除前 `check --track <轨道> [--milestone <编号>]`；`guard --op edit|merge|delete` 输出 exit code 供脚本闸用（0=放行 / 1=阻断 / 2=警告需确认）。
- **CLOSED 轨道硬拒绝**：claim/check 默认拒绝已收官轨道（wt 应已移除、分支保留）；重建走 §7.1b，重建后更新 TRACKS 注册表状态。
- **锁文件**：主树根 `.wt_pool_busy.json`（经 `git rev-parse --git-common-dir` 定位主树，全部 worktree 共享一把锁；已 .gitignore）。锁记录里程碑 / wt 路径 / 分支 / pid / host / 时间 / 备注，status 会标 `pid-dead` 陈旧占坑。
- **自测**：`python tools/wt_pool.py selftest`（21 项断言，进程内临时锁，不碰真实锁）；全表对照 `python tools/wt_pool.py status`。
- **维护约定**：TRACKS 注册表（脚本内）与 §7.1/§7.1b 表同步更新——轨道收官 / 重建时两处一起改，脚本表是机械闸的事实源。

### 7.7 树边界闸（2026-11-10 用户拍板，`scope` + `tools/tree_gate.py`）

起因：2026-11-10 用户报告——已占 wt 坑的对话仍经常越界对**主树或其他 wt** 操作（跑错树的 regression、重拍错树的 example 基线等），§7.6 只防「碰别人已 claim 的 wt」，不防「占坑会话自己伸手出去」。两道机械闸：

- **cwd 自查 `scope`**：claim 后第一步、以及任何切换目录之后跑 `python tools/wt_pool.py scope [--milestone <编号>]`。判定：cwd 在自己 claim 的 wt 内 = exit 0；cwd 在主树且锁里有活跃 claim = exit 2 警告（主线无 claim 会话核实后可忽略）；cwd 在别的 track 的 wt 内 / 未 claim 的 wt 内 / 任何注册树之外 = exit 1 拒绝。
- **脚本树闸 `tree_gate.py`**：共享入口工具（`tools/regression.py`、`tools/showcase/server.py`、`tools/reshot-hud-s71.py`；后续同类入口照接）开头调 `gate(__file__)`——**脚本所在树 ≠ cwd 所在树 → exit 2 拒绝**。判据零误伤：主线会话主树脚本+主树 cwd 放行；wt 会话自己树的脚本+自己树 cwd 放行；其余一律拒绝。刻意例外（CI 沙箱 cwd 无关等）设 `NL_TREE_GATE_ALLOW=1`（输出 WARN 放行）。
- **`wt_pool.py` 本身不接树闸**：它就是跨树占坑工具（锁在共享 .git），从任何树跑都是合法的。
- **作用边界**：树闸防的是「跑错树 / 写错树」；git 层面的还原类危险操作仍按 AGENTS.md「git 危险操作闸门」节执行（status --porcelain 闸门）。
