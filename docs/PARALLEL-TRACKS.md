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
| VSM（虚拟阴影贴图） | **S15，插入 S9 之后**（合并原 `shadow-map` 穿插项，作 rasterized 阴影对照） | **已收官 2026-11-14**：M15.0~M15.2 整轨 merge `165216d`（tags `s15-vsm`/`s15-clipmap`/`s15-done` + 分支 `milestone/s15-done`），wt 已拆 / 分支保留 / TRACKS CLOSED；明细 MILESTONES S15 三节 |
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

**轨道状态唯一事实源 = `python tools/wt_pool.py status`**（active / closed / planned + claim 占用与 note）。本表只登记静态锚（worktree 路径 / 分支 + tag / 计划文档 / merge 锚），**收官后不再改写本表**；里程碑明细以 `docs/MILESTONES.md` 对应节为权威，本表不再复制（2026-11-13 用户拍板方案 B，根治双写漂移——此前 particles / script / audio / ecs 等行与注册表脱节即此病）。

| 轨道 | worktree | 分支 + tag | 计划 / 收官锚 |
|---|---|---|---|
| 真实资产接入练手（M-P9V1→P9 V2） | ~~`E:/UE_STUDY_2026/nl-engine-asset-ingest`~~（已拆，2026-11-13） | `feat/asset-ingest` + tags `asset-pbr-done`/`asset-mr-fix` + 分支 `milestone/asset-pbr-done`/`milestone/asset-mr-fix` | P9 V1 外部 OBJ→nmesh 练手 → P9 V2 Unity 权威材质链 + 引擎全 PBR（拍板「找成熟代码抄 / 全 PBR 不许只吃 baseColor」）→ MR 贴图接线 + 引擎 GLB 因子缺省 + e1b 基线重拍 8/8 ALL GREEN；merge `ea42d73` + `8641fbe`；明细 MILESTONES 资产轨两节 |
| 多人游戏 / 网络栈 | ~~`E:/UE_STUDY_2026/nl-engine-net`~~（已拆） | `feat/net` + tags `net-m0`~`net-done` + 分支 `milestone/net-done` | `docs/net/NETCODE-PLAN.md`（拍板 `NETCODE-OPTIONS.md` §6 / 归档 `NETCODE-TOOLS-GUIDE.md`）；merge `9aa58df`；明细 MILESTONES NET 节 |
| 性能与内存观测 | ~~`E:/UE_STUDY_2026/nl-engine-profiling`~~（已拆） | `feat/profiling` + tags `tracy-m0`~`tracy-m3` | `docs/p8/P8-TRACY-PLAN.md` + `docs/p8/TRACY-TOOLS-GUIDE.md`；merge `9bf037c`；明细 MILESTONES M-TR0~TR3 节 |
| 引擎日志系统升级 | ~~`E:/UE_STUDY_2026/nl-engine-logging`~~（已拆） | `feat/logging` + tags `log-m0`/`log-m2` | `docs/p8/P8-LOG-PLAN.md` + `docs/p8/LOG-TOOLS-GUIDE.md`；merge `aaf43b8`；明细 MILESTONES M-LOG0/M-LOG2 节 |
| 外置编辑器 | `E:/UE_STUDY_2026/nl-engine-editor`（2026-11-14 §7.1b 重开） | `feat/editor` + tags `e-m0e2-done`~`e-m6-done`、`e-m7-done`、`e-m8-done`、`e-m9-done`+`e-m10-done`、`e-m11-done`+`e-m12-done`+`e-m13-done`；M-E0~E6 merge `ac50c10` + `fc34176` | `docs/editor/EDITOR-PLAN.md` + `RPC-SPEC.md`；M-E7 模型预览 + M-E8 场景树增强 + M-E9 场景树编辑 + M-E10 对象级泛化面 + M-E11 视口截图基建（三源择优+遮挡自检，引擎侧帧 dump 留账）+ M-E12 编辑 undo-redo（viewer 命令栈 scene.undo/redo + Structural 快照重建/remapStacks）+ M-E13 outliner 多选框选 + 批量操作（selftest 确定性路径）全收官；前置 merge master `26bc979`；**2026-11-14 整轨 merged 回 master（`77bb1c7` no-ff + 修复 `4930104`，S13 复验 10 卡全绿，tag `editor-done` + `milestone/editor-done`）**；明细 MILESTONES M-E 节 |
| 材质图 → Slang | ~~`E:/UE_STUDY_2026/nl-engine-material-graph`~~（已拆） | `feat/material-graph` + tags `mg-b2b4`/`mg-t1` | `docs/material-graph/SIDE-TASKS.md`（终点定义）+ `B2-B4-ACCEPTANCE.md`；merge `e5505bc`；明细 MILESTONES T1 节 |
| Job / Task 并行 | ~~`E:/UE_STUDY_2026/nl-engine-job-system`~~（已拆） | `feat/job-system` + tags `m-j5a`/`m-j5b`/`m-j5c-prep` | `docs/p8/P8-JOB-PLAN.md`；merge `fb2b4f4`；明细 MILESTONES M-J5 节 |
| 动作系统 / 动画控制器 | ~~`E:/UE_STUDY_2026/nl-engine-action-system`~~（已拆） | `feat/action-system` + tags `anim-ma1`~`anim-ma5c` | `docs/action-system/ACTION-PLAN.md` + ANIM-TOOLS-GUIDE；merge `5a1912f`；明细 MILESTONES M-A 节 |
| 游戏循环 / 双帧模型 | ~~`E:/UE_STUDY_2026/nl-engine-gameloop`~~（已拆） | `feat/gameloop` + tags `gl-m0`~`gl-m5` | `docs/p8/P8-GAMELOOP-PLAN.md` + `GAMELOOP-TOOLS-GUIDE.md`；merge `54221e2`；明细 MILESTONES M-GL0~GL5 节（M-GL2 内含音频 M-S3） |
| 开发者控制台 | ~~`E:/UE_STUDY_2026/nl-engine-console`~~（已拆） | `feat/console` + tag `console-mc3` | `docs/p8/P8-CONSOLE-PLAN.md` + `CONSOLE-TOOLS-GUIDE.md`；merge `9e12cc6`；明细 MILESTONES M-C0~C3 节 |
| PBR 直射光 + 贴图采样（B 类穿插） | ~~`E:/UE_STUDY_2026/nl-engine-pbr-direct`~~（已拆） | `feat/pbr-direct` + tag `pbr-direct` | `docs/s0/PBR-DIRECT-PLAN.md`；merge `e57173d`；明细 MILESTONES pbr-direct 节 |
| 粒子系统（B 类穿插） | ~~`E:/UE_STUDY_2026/nl-engine-particles`~~（已拆） | `feat/particles` + tags `particles-pt0`~`pt5` | `docs/particles/PARTICLES-PLAN.md` + `VFX-TOOLS-GUIDE.md`；merge `204a84e`（PT0~PT3）+ `96892a4`（PT4/PT5）；明细 MILESTONES M-PT0~PT5 节 |
| 输入层扩展 | ~~`E:/UE_STUDY_2026/nl-engine-input`~~（已拆） | `feat/input` + tags `input-m0`~`input-m2` | `docs/p8/P8-INPUT-PLAN.md` + `INPUT-TOOLS-GUIDE.md`；merge `0df16a0`；明细 MILESTONES M-I0~I2 节 |
| 玩法相机族 | ~~`E:/UE_STUDY_2026/nl-engine-camera`~~（已拆，2026-11-13 二次拆除） | `feat/camera` + `milestone/cam-m5` + tags `cam-m0`/`cam-1a`/`cam-3a`/`cam-b`/`cam-m5` | `docs/p8/P8-CAMERA-PLAN.md` + `CAM-TOOLS-GUIDE.md`；a 段 merge `7b0f24a`、b 段 merge `745ff83`、M-CAM5 merge `52700f1`（视觉卡 cam-1b/2b/3b/mcam4 + 双 bug 勘误）；明细 MILESTONES M-CAM5 四栏 |
| 驾驶能力轴（碰撞桥 + 载具物理） | ~~`E:/UE_STUDY_2026/nl-engine-drive`~~（已拆） | `feat/drive` + tags `drive-m0`~`drive-m3` | `docs/p8/P8-DRIVE-PLAN.md` + `DRIVE-TOOLS-GUIDE.md`；merge `a2e3cff`；明细 MILESTONES M-D0~D3 节 |
| S13 簇级 BLAS 聚合 + 延迟一帧回读（B 类穿插，用户拍板开 wt） | ~~`E:/UE_STUDY_2026/nl-engine-s13`~~（已拆） | `feat/s13` + tags `s13-preblas`/`s13-merge`/`s13-done` + 分支 `milestone/s13-done` | `docs/s13/S13-PLAN.md`（M13.0~M13.2）；merge `f07201c` + s13-preblas 基线重拍 `09a30d8`（M13.1 HUD 行演进坑入档）；明细 MILESTONES S13 节 |
| S14-A 软光栅亚像素三角形（B 类穿插，M14 轨道 A 段） | `E:/UE_STUDY_2026/nl-engine-s14-softrast`（在册） | `feat/s14-softrast` + tags `s14a-*`，收官 `s14-softrast` | `docs/s14/M14-PLAN.md`（总纲：A+B 并行 / C 接 B / D 砍 / kStage 冻结 S8.2）；M14A0~M14A3；2026-11-13 建于 master `1892866` |
| S14-B ReSTIR GI（B 类穿插，M14 轨道 B 段；C 自适应探针 M14C1~C3 接力本 wt） | ~~`E:/UE_STUDY_2026/nl-engine-s14-restir`~~（已拆，2026-11-15） | `feat/s14-restir` + tags `s14b-*`，收官 `s14-restir`；`s14-adaptive`/`s14c-spawn`/`s14c-fixup`（M14C3 收官 `170417c`） | `docs/s14/M14-PLAN.md` + `S14B-RESTIR-PLAN.md` + `S14C-ADAPTIVE-PLAN.md`（§10/§11）；**S14 整轨 merge 回 master `cfaf58f` + merge 复验修复 `b197623`（2026-11-15 用户指令「提交 合并回主干 拆wt」：冲突 12 文件双方增量并留——签名族/天气+adaptive CB 并留 192B/退出闸超集/artifact 清单补登 csadaptiveskyfixup（M8.1 教训第四犯）/restir.slang legacy skyGradient → M10.1 常量内联；主仓双后端 BUILD OK 重链 + 回归冒烟 s14c-adaptive 2/2 + s14b-restir 2/2 ALL GREEN bytes）；wt 已拆（2 junction 摘除 + prune），TRACKS CLOSED（重开走 §7.1b）** |
| 天气变化参数层（A 类穿插，2026-11-12 立项） | ~~`E:/UE_STUDY_2026/nl-engine-weather`~~（已拆，2026-11-14） | `feat/weather` + tags `weather-w0`~`weather-w3` | `docs/WEATHER-PLAN.md`（M-WEA0~WEA3；方案 A 参数层驱动 Lumen，Q-WE1~WE6 已拍板 2026-11-12；M-WEA0 开工拍板 Q-WEA0-A/B = 离散天气态+平滑过渡 / nl_core 新模块，2026-11-13）；2026-11-13 建于 master `6a824b1`；A 类工具版交代，零渲染改动不 bump `kStage`；**M-WEA0+1 已 merge `4090d94` / M-WEA2+3 已 merge `ebf5f50`（2026-11-14 整轨收官，46 runs 全 bytes）；wt 已拆（2026-11-14 用户指令，2 junction 摘除 + robocopy /MIR + prune），TRACKS CLOSED（重开走 §7.1b）** |
| 天气变化参数层（A 类穿插，2026-11-12 立项） | `E:/UE_STUDY_2026/nl-engine-weather`（在册） | `feat/weather` + tags `weather-w0`~`weather-w3` | `docs/WEATHER-PLAN.md`（M-WEA0~WEA3；方案 A 参数层驱动 Lumen，Q-WE1~WE6 已拍板 2026-11-12；M-WEA0 开工拍板 Q-WEA0-A/B = 离散天气态+平滑过渡 / nl_core 新模块，2026-11-13）；2026-11-13 建于 master `6a824b1`；A 类工具版交代，零渲染改动不 bump `kStage` |
| 大世界生成工具 / citygen（A 类，2026-11-12 立项 M-W0） | `E:/UE_STUDY_2026/nl-engine-worldgen`（在册） | `feat/worldgen` + tags `w0-m0`~`w0-m3`，M-W1 拟 `w1-m0`/`w1-m1` | `docs/worldgen/W0-PLAN.md`（M-W0 收官）+ `W1-PLAN.md`（M-W1 收官 tag `w1-m0`/`w1-m1`：city2engine 驱动步 + ambientCG 贴图化消费）+ `W2-PLAN.md`（M-W2 收官 tag `w2-m0`/`w2-m1`：程序化楼型池）+ `W3-PLAN.md`（**M-W3 整轨收官 2026-11-14，tag `w3-m0`/`w3-m1`：树 12/车 8/路灯 CC0 资产通道，FNV-1a 纯函数摆放零 RNG，drive vendored 装配，scene 315 实体**）；明细 MILESTONES W0/W1/W2/W3/T0 节；M-T0 交通整轨收官（tag `traffic-m0`：core nl/traffic lane graph + A* + C1 跟车，traffic-tools 9 ctest，卡 `t0-traffic`）；M-T1 调度器收官（tag `traffic-m1`：A2 信号相位 + B1 占用锁 + C1 跨路段互斥，自适应控制几何三重死锁根修，卡 `t1-traffic`）；M-T2 调度器增强收官（tag `traffic-m2`：组共享锁 +143% 吞吐 + 转弯独占 + gap-out 感应 + loop 环线，卡 `t2-traffic`）；M-T3 CityCar 接 viewer（tag `traffic-m3`：TrafficCarSystem ECS 同构 + `--traffic-city` 42 车永续车流）+ M-T4 红绿灯可视化 + 信号调度接通引擎 + citygen signal 填充（tag `traffic-m4`：SignalLampComponent + autoSignal + serializeRoadnet 按进入边>=2 置位，修复 sim_.control 未挂接缺陷）；**整轨已 merge master（2026-11-14 `1e8f4e1` no-ff：W0~W3 + T0 五里程碑全量并入）；merge 后该轨 14 卡重跑 + ctest 164/164 双后端见 MILESTONES T0 节** |
| 水体渲染（B 类穿插，2026-11-14 立项方案 B） | ~~`E:/UE_STUDY_2026/nl-engine-water`~~（已拆，2026-11-14） | `feat/water` + tags `water-t0`~`water-t3` | `docs/WATER-PLAN.md`（M-WAT0~WAT3 四里程碑：SSR 本体 / Gerstner 波形 / 折射+Beer-Lambert 吸收+Fresnel+泡沫 / 岸线软化+岸线泡沫；Q-WA1~WA7 已拍板）；**整轨已 merge `92a0863`（2026-11-14 用户指令「合并回主干」，主仓双后端 BUILD OK + 重跑 12/12 历史卡 bytes 全绿）；wt 已拆（2026-11-14 用户指令，2 junction 摘除 + robocopy /MIR + prune），TRACKS CLOSED（重开走 §7.1b）** |
| 水体渲染（B 类穿插，2026-11-14 立项方案 B） | `E:/UE_STUDY_2026/nl-engine-water`（在册） | `feat/water` + tags `water-t0`~`water-t2`，收官 `water-done` | `docs/WATER-PLAN.md`（M-WAT0 SSR 本体 / M-WAT1 Gerstner 波形 / M-WAT2 折射吸收收口；Q-WA1~WA6 已拍板 2026-11-14：SSR 本体由 M-WAT0 承载、零 vendored）；2026-11-14 建于 master `b150fd9`；验收含反射/折射侧证据（关反射水体感几乎不变即不合格） |
| 游戏界面 RmlUi 接入（A 类，2026-11-15 立项 M-UI0） | `E:/UE_STUDY_2026/nl-engine-ui-rmlui`（**CLOSED 2026-11-15，wt 已拆除**：junction 摘除 + worktree prune + 分支删于合并后） | `feat/ui-rmlui` + tag `ui-rmlui` @ `22f636d` | M-UI0 整轨收官（M-UI0.0 vendored RmlUi 6.3+FreeType 2.13.3 / 0.1 renderer 双后端接入 / 0.2 C# Ui 域全链 + demo+热重载+F8+`--rmlui-click` 无头验收 / 收官 MILESTONES+close 闸+release --done）：双后端 BUILD OK + selftest 16/16 ×2 + 验收锚全绿（entry ok / click id=btnDemo / ontick alive）+ 缺省关基线零漂 + `gen.py --check` 绿 + kStage 不 bump；踩坑 11 条入 MILESTONES；**整轨已合并 master（--no-ff `cfe6a8f`，2026-11-15）** |

| 通用物理查询 API（A 类，波 1，2026-11-15 立项 M-PQ0） | `E:/UE_STUDY_2026/nl-engine-phys-query`（在册） | `feat/phys-query` + tag `phys-query-pq0` | `docs/p8/PHYQUERY-PLAN.md` + `PHYQUERY-TOOLS-GUIDE.md`；用户拍板方案 A 最小面（3 组 Jolt-free POD + 3 函数）；冻结面 = exports.api / Bindings / viewer 一律不碰（波 0 闸等 input 轨 merge）；A 类工具版交代，不 bump `kStage` |
| 2D 精灵批渲染（B 类穿插，波 1，2026-11-15 立项 M-SP0） | `E:/UE_STUDY_2026/nl-engine-sprite2d`（在册） | `feat/sprite2d` + tag `sprite2d-sp0` | `docs/sprite2d/SPRITE2D-PLAN.md` + `SPRITE2D-TOOLS-GUIDE.md`；用户拍板方案 A 严格切分（core POD + renderer 独立 Sprite2DPass + 一份 .slang 双后端 + 正交 + 按 textureId 分桶），目标 = 落地 Brotato 世界空间 2D；RmlUi element/scissor 桥接后置不进本里程碑；demo 旗标与 PNG 基线等波 0 |

> 已建 worktree 均以 `junction` 共享主 worktree 的 `.workbuddy` 与 `thirdparty/slang/dist`，且 `.workbuddy` 已 `skip-worktree`（git status 干净，不会误把纪律源 commit 进 feature 分支）。**在册 worktree 清单与各轨道 active/closed 状态一律以 `wt_pool.py status` 为准（2026-11-13 起），本节不再维护文字版快照。**

### 7.1b 待建 worktree（已立项，执行排 P8 场景整合之后）

2026-11-08 用户拍板：寻路 / 物理 / 3D 音频**依赖场景整合**，先立项出计划，**执行放到 P8 E1（ECS-lite 场景）验收之后**；届时按 §7 模式新建 worktree。**2026-11-09 用户指令提前解锁物理轨道**（见 §7.1e），寻路 / 音频仍排其后。**→ 2026-11-11 寻路整轨 M-N0~M-N6 全量收官 merge 回 master（`5356c80` + `d3d6127`），worktree 已拆 / 分支保留 / TRACKS CLOSED；本节「待建」对寻路不再适用，重开按 §7.1b 重建。→ 2026-11-13 音频整轨确认收官（M-S3 随 M-GL2 交付，见下行），本节「待建」对音频同样不再适用。**

> **wt 建后唯一权威步骤（2026-11-15 根治，替代此前全部手工步骤）**：worktree add 之后跑一次
> `python tools/wt_pool.py setup --track <轨道>`
> —— 幂等完成：① `.workbuddy` junction 共享主树（wt 侧若有 checkout 出来的实体目录自动替换，权威在主树）；② `thirdparty/slang/dist` junction 共享主树（untracked 构建产物，wt 侧天然不存在）；③ 遗留 wt index 里仍被跟踪的 `.workbuddy` 路径批量 skip-worktree。前提：master 已把 `.workbuddy/` 全部移出跟踪（2026-11-15 `git rm --cached .workbuddy/memory/2026-11-14.md`，`.gitignore` 的 `/.workbuddy/` 自此真正生效），新 wt checkout 不再产生实体 `.workbuddy`。

| 轨道 | 届时 worktree 绝对路径 | 分支（届时建） | 计划 |
|---|---|---|---|
| 寻路（vendored Recast/Detour） | ~~`E:/UE_STUDY_2026/nl-engine-pathfinding`~~（已拆） | `feat/pathfinding` + tags `nav-m0`~`nav-n6` + `nav-done` + 分支 `milestone/nav-done` | `docs/p8/P8-NAV-PLAN.md` + `NAV-TOOLS-GUIDE.md`；merge `5356c80` + `d3d6127`；明细 MILESTONES M-N0~N6 节 |
| ~~物理（Jolt vendored 集成）~~ | ~~`E:/UE_STUDY_2026/nl-engine-physics`~~（已拆） | `feat/physics` + tags `phys-p0`~`phys-j5c` | `docs/p8/P8-PHYS-PLAN.md` + `PHYS-TOOLS-GUIDE.md`；merge `7d844ef`；明细 MILESTONES M-P0~P4 + M-J5c 节；另见 §7.1e |
| ~~3D 空间音频 + 游戏内调试~~ | ~~`E:/UE_STUDY_2026/nl-engine-audio`~~（已拆） | `feat/audio` + tags `audio-s0`~`audio-s2` | `docs/p8/P8-AUDIO-PLAN.md` + `AUDIO-TOOLS-GUIDE.md`；merge `1aac3d2`；M-S3 随 M-GL2（`gl-m2`）交付；明细 MILESTONES M-S0~S2 + M-GL2 节 |
| 大世界生成工具 / citygen（A 类，2026-11-12 立项 M-W0） | `E:/UE_STUDY_2026/nl-engine-worldgen`（**已建**，2026-11-13，起点 master `52700f1`；junction 共享 `.workbuddy`/`thirdparty/slang/dist` + skip-worktree（9 个 `.workbuddy` tracked 文件），纪律同 logging 轨道） | `feat/worldgen`（**已建**） | **M-W0 整轨收官（2026-11-13，tag `w0-m0`~`w0-m3`）**：离线 `city-tools` 纯 CPU 产俯视 PNG（密度分区+路网叠加+树）+ `roadnet.json`（49 节点/168 有向边契约）+ 36 块楼体 OBJ 中间件 + `city.scene`（E1 World JSON 引用闭合）+ CityStats 统计锚；selftest 72 断言 + ctest 6 用例双树 + 4 展示台卡；close 闸 + `release --done` 已过。**M-W1 已立项（2026-11-13）：城市资产进引擎**（Q-W1A=双轨另产 city.scene.engine / Q-W1B=扁平 glb 白名单零改动 / Q-W1C=obj2glb 等 asset-ingest merge / Q-W1D=街景证据图用户终端补拍）；**落码闸 = feat/asset-ingest OBJ→glb 链 merge master（已达成，feat/worldgen 已合并 master）** | 计划 `docs/worldgen/W0-PLAN.md`（M-W0 已收官）+ `docs/worldgen/W1-PLAN.md`（M-W1 已立项）；tag 名字制 `w0-*`/`w1-*`，不占 m0.* 编号 |

### 7.1e 物理轨道（2026-11-09 提前开工，M-P0~M-P3）

| 轨道 | worktree 绝对路径 | 分支 | 状态 |
|---|---|---|---|
| 物理（Jolt vendored 集成） | ~~`E:/UE_STUDY_2026/nl-engine-physics`~~（已拆） | `feat/physics`（分支保留） | `docs/p8/P8-PHYS-PLAN.md` + `PHYS-TOOLS-GUIDE.md`；merge `7d844ef`（tags `phys-p0`~`phys-p4` + `phys-j5c`）；明细 MILESTONES M-P0~P4 + M-J5c 节；A 类工具版交代，不进 `kStage`/渲染回归 |

- 用户指令「建 feat/physics 开发物理模块内容」= 提前解锁排期闸（计划 §排期约束括号内已允许 M-P0~M-P3 与 E1 并行）；**M-P5 场景集成仍 gate 在 P8 E3（序列化 + Inspector）之后**，不随本轮解锁。
- 三处口径二次核实（2026-11-09）：Carrot `engine/physics/` 2902 行 Jolt 集成层 + 本地 vendored JoltPhysics **v5.3.1**；bevy 本体无物理 crate（仅 `examples/movement/physics_in_fixed_timestep.rs` 示例），生态 avian，不作口径；UE 5.8.4 只有自研 **Chaos**（`Runtime/Experimental/Chaos*` + `PhysicsCore/` + `Engine/Classes/PhysicsEngine/`），**全树无 Jolt 后端、ThirdParty 下亦无 PhysX SDK**——立项文档里「UE 官方提供 Jolt 后端」为误记，已勘误并落 `UE-DIFFS.md` D10。
- **选型二次拍板（2026-11-09）**：用户「物理用现成的、没必要从头写、成本太高；纠结的只是用哪个」→ 不自研方向不变，**继续走 Jolt 5.3.1**；理由为**成本取舍 + 工程适配**（本机唯一可得 / MIT / 纯 CPU / 确定性 / 自带接口 / Carrot 集成层参照）。UE 口径不参与选型，仅作只读对照。**「例外理由」旧措辞按 2026-11-09 四修作废**：`AGENTS.md` 选型闸后，vendored 成熟库是通例而非例外。候选对比与换库代价见 `NON-RENDER-ROADMAP.md` 头部修订与 `P8-PHYS-PLAN.md` §选型评审结论。

### 7.1c ECS-lite 场景轨道（2026-11-09 立项，E1a 先行）

> 2026-11-09 用户拍板：ECS（P8 E1）是下游三轨的瓶颈，**E1a（纯逻辑内核）提前开 worktree**，E1b（渲染集成）后挂等 S6 收官。计划 `docs/p8/P8-ECS-PLAN.md`（M-E0~E3，Carrot 式 OOP 组件路线）。

| 轨道 | worktree 绝对路径 | 分支 | 计划 |
|---|---|---|---|
| ECS-lite 场景 | ~~`E:/UE_STUDY_2026/nl-engine-ecs`~~（已拆） | `feat/ecs` + tags `ecs-e0`~`ecs-e1b.5` | `docs/p8/P8-ECS-PLAN.md` + `ECS-TOOLS-GUIDE.md`；E1a merge `2d4ad49`、E1b 链 merge `e2ac82c` + `6560b2c`（全量 406/406 ALL GREEN）；明细 MILESTONES M-E0~E3 + E1b 节 |

### 7.1d C# 脚本轨道（2026-11-09 立项，E4 脚本段解禁提前）

> 2026-11-09 用户拍板（三轮收敛：Lua → 鸣潮参考 TS+C# → **定 C#**「热更也已成熟」）：E4 脚本段解禁提前，目标 = 功能扩展 + 热更，技术路线 = **.NET hosting（hostfxr）+ C# 脚本程序集 + collectible ALC 热更**（鸣潮 3.6 同款方向）。计划 `docs/p8/P8-SCRIPT-PLAN.md`（M-SC0~SC3）；M-SC0 无依赖即刻开工，M-SC1 gated on console M-C3 merge，M-SC2 消费 E1a 成果。

| 轨道 | worktree 绝对路径 | 分支 | 计划 |
|---|---|---|---|
| C# 脚本 | ~~`E:/UE_STUDY_2026/nl-engine-script`~~（已拆，2026-11-14） | `feat/script` + tags `m-sc0`~`m-sc11` | `docs/p8/P8-SCRIPT-PLAN.md` + `SCRIPT-TOOLS-GUIDE.md`；M-SC0~SC3 merge `1b49b5f`，**M-SC4~SC11 整轨 merge `4c5c582`（tag `script-merge`，2026-11-14 用户指令；merge 后 11 历史卡回归 90/90 bytes 全绿 + 便携 ctest 164/164 双后端）；wt 已拆（2026-11-14 用户指令，2 junction 摘除 + prune），TRACKS CLOSED（重开走 §7.1b）** |
| C# 脚本 | `E:/UE_STUDY_2026/nl-engine-script`（2026-11-13 重开重建） | `feat/script` + tags `m-sc0`~`m-sc3` | `docs/p8/P8-SCRIPT-PLAN.md` + `SCRIPT-TOOLS-GUIDE.md`；M-SC0~SC3 merge `1b49b5f`；**2026-11-13 拍板重开扩胶水层 API 面（M-SC4~SC9：Time/Input/Log、Camera、模型场景、Physics 载具、Audio），wt 重建于 master 08a67c0，注册表已转 active** |

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

## 8. 合并冲突根治机制（2026-11-10 落地）

> 背景：每条 feat/* 轨道都向 `example/cases.json`（1.2 万行）尾部追加版本对象、向根 `CMakeLists.txt` 追加工具 target 与 ctest 用例——git 文本合并不懂语义，**每次 merge 这两处必撞**（gameloop / input / nav 三次合并实测）。2026-11-10 用户拍板「1+2」方案根治。

### 8.1 cases.json 语义合并驱动

- **机制**：`.gitattributes` 登记 `example/cases.json merge=nlcases`；仓库 `.git/config`（worktree 共享，主仓设一次全轨道生效）登记 `merge.nlcases.driver = python tools/merge-cases.py %O %A %B %L %P`。
- **语义**：三路 JSON 合并——`versions` / `cases` 数组按 `id` 做并集（各自追加的新条目自动合并、互不冲突），字典字段级合并；只有「两边改了**同一个**对象的同一个字段且值不同」才回落手工。
- **规范格式**：`json.dumps(indent=1, ensure_ascii=False)` + 尾部换行（驱动输出统一此格式；2026-11-10 已把 input 轨道手工追加的 3 个单行压缩条目归一化）。**手工往 cases.json 追加条目时也要用这个格式**（用 python json 库写，别手拼字符串）。
- **自检**：`python tools/merge-cases.py --selftest`（7 合成用例：追加并集 / 字段编辑+追加 / 真冲突 / 格式差异无关 / cases 子数组 / 字符串数组 / 标量编辑）。
- **注意**：驱动失败（exit≠0）时 git 会标冲突回落手工，此时按老办法解；解完的文件必须仍是合法 JSON。

### 8.2 CMakeLists.txt 拆分

- **结构**：viewer + 11 个工具 target 的 `add_executable` 与**各自的 ctest 用例**已全部拆到 `<dir>/CMakeLists.txt`；根文件只留全局设置、thirdparty、nl_core/nl_renderer、shader 编译、`add_subdirectory` 清单。
- **新轨道加工具的三步**：建 `<dir>/main.cpp` → 建 `<dir>/CMakeLists.txt`（照抄任一现役工具骨架：add_executable + target_link_libraries + if(MSVC) 编译选项 + 自己的 add_test）→ 根文件 `add_subdirectory(<dir>)` 清单加一行。此后新工具/新用例不再进冲突面。
- **不变量（拆分时钉下的，勿动）**：
  - 根文件 `set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})`——exe 必须落顶层 `build/<backend>/`（cases.json exe 路径 + build-m0.2.bat selftest 依赖）；
  - `enable_testing()` 必须在 `add_subdirectory` 清单**之前**（目录属性在 add_subdirectory 时拷贝）；
  - 根级 `set(NL_ROOT ${CMAKE_CURRENT_SOURCE_DIR})`——子目录引用 `assets/` 等根级路径统一用 `${NL_ROOT}`（子目录里 `CMAKE_CURRENT_SOURCE_DIR` 已不是仓库根）；
  - ctest 测试间同工具依赖（如 s2 DEPENDS s1）搬到同目录后语义不变；**跨工具的测试依赖不许新增**（拆分保证各自目录自治）。
