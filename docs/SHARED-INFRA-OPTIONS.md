# 共享 temporal / post-chain 基建决策材料（S10 开工前，2026-11-11 补决策前置功课）

> **状态：已拍板（2026-11-11 用户选 方案 A + Q-H3 UE 口径：temporal 最小集、属性不落盘、TSR/tonemap 前；Q4 顺延项随之定「不落盘」）。** 本文件是**决策证据与选项**，其结论已工程化落地于 `docs/t0/T0-PLAN.md`；本文件保持为决策证据，不再改动口径。
> **S12 到期状态（2026-11-12）**：M12.0 开工拍板 Q12-B =「**M12.2 前按实测再拍**」→ Q4 顺延项**维持冻结**、Q-H2 结论不改写，M12.0/M12.1 零新增独立 GBuffer 资源；Q12-A = GI 分量独立（复用 `gatherLinear_` + `history0_/1_` ping-pong，零新增 RT）。权威落点 `docs/s12/S12-PLAN.md §0`（含三处实读、代价矩阵、T1~T8 任务书）。本文件口径未变，仅补状态指针。
> 归属：服务 `S10` / `S12` / `S15` / `S16` 四个阶段与 ROADMAP 穿插项 `post-chain`，不归属单一 `s*` 目录，故放 `docs/` 根。
> 触发：`S9` 全收官（tag `s9-done`）后下一站就是 `S10`，但 `M10.0` 第一条 todo 就依赖一份**从未落地**的 motion vector；`Q4` 的 GBuffer 落盘形态被顺延到「S10 与 S12 历史帧需求一起拍」；「离屏 HDR RT + post-chain」自 `S7` 之后一直是穿插项表里的一行，没有任何里程碑承载。按 `AGENTS.md`「开工闸：计划内不得留待拍板架构判定」补功课。
>
> **本轮边界**：只产出本材料。**零落码、零槽位、零 tag、零 `kStage` 改动、不改任何既有 `*-PLAN.md`、不 claim / release 轨道、不碰两把 stale 锁**。拍板后动作见 §8。

---

## 0. 冻结面与待拍板项

| 冻结面（拍板前不许写第一行） | 状态 |
|---|---|
| 新增任何离屏 RT / UAV / `register(u/t)` 槽位 / 纹理格式 / MRT 组合 | **冻结** |
| 新建里程碑编号、`*-PLAN.md` 任务书、example 版本卡、`cases.json` 卡 | **冻结** |
| 主帧 present 路径、`kStage`、既有 `example/` 基线 | **冻结** |
| `renderer/shaders/gbuffer_common.slangh`（PURE MOVE / Resource-free，0 处资源声明） | **保留**，本材料不触碰 |
| `rtTarget_`（M8.2 `RGBA32_FLOAT` UAV，M9.0/9.1/9.2 三条链共用）| **保留**，本材料只讨论是否复用，不改 |
| 在注释 / CMake / README / commit message 里把形态写死 | **冻结** |

**必须拍板项（3 项，其余口径见 §5 各方案默认，不另设题）**

| 编号 | 要定什么 | 为什么必须用户定 |
|---|---|---|
| **Q-H1** | 共享基建的**里程碑承载与范围**：单里程碑最小集（A）/ 单里程碑一次建齐含属性落盘（B）/ 拆两次（C） | 触碰排期、范围、资源契约；`AGENTS.md` 明确列为「必须摆证据 + 让用户定」 |
| **Q-H2** | `Q4` 顺延项：**GBuffer 属性要不要落盘**——不落（A）/ 单张 packed `RGBA32_UINT` + 历史帧（B）/ 精简 float MRT（B 变体） | `Q4` 已拍「S9 方案 A 不落盘」但**顺延项未拍**；`S10/S12/S15` 是消费端，格式写下即隐式拍死 |
| **Q-H3** | **TAA/TSR 与 tonemap 的相对位置**：UE（tonemap 前）/ Carrot（tonemap 后）/ bevy（post-process 链节点） | 三处口径**不一致**，且**本项目内部已自相矛盾**：`ROADMAP.md:269` 后处理链行写「离屏 RT + ACES tonemap → FXAA/TAA → bloom」（tonemap 在 AA **之前**），而 `S16-PLAN.md:123` 底线写「TSR 在 **tonemap 之前**、Lumen GI 之后」（正好相反）。挂点顺序决定 tonemap 后是否还需一份离屏输出，事后改等于重排整条链 |

> **Q-H1 与 Q-H2 的关系**：Q-H1 选 A / B 时，Q-H2 的答复已随方案给出（A=不落盘 / B=落 packed），**不是重复出题**；单列 Q-H2 是因为它涉及**改写已经拍板的 Q4 结论**，需要用户显式确认「S9 不落、S10 前落」这条边界，不能由选方案顺带默认。若用户只回一句「按 A」，视为同时确认 Q-H2=不落盘。
> 子判定（velocity 格式、写入 pass、history 形态、绑定布局、基线策略）已按三处证据 + 本项目历史约定收进 §5 各方案的**默认口径**，随 Q-H1 一并拍板；若要单独改，拍板时注明即可，不另开题。

---

## 1. 现状：四条「指向不存在的东西」的声明

| 位置 | 原文 / 口径 | 悬空点 |
|---|---|---|
| `docs/s10/S10-PLAN.md:11` | 「重投影上一帧探针（**复用 M12 要做的 motion vector**，先做简化版：同屏位置直接挪）」 | 依赖倒挂到未来里程碑；简化版是占位 |
| `docs/s12/S12-PLAN.md:10` | 「复用 **S16 前半段（S7 后）建好的**完整逐像素 velocity GBuffer，**本步不另建一份**」 | 那个「建好的」从未存在 |
| `docs/s16/S16-PLAN.md:29,55-59,93` | 「**无离屏 HDR RT、无 tonemap pass、无 post-chain、无 velocity / normal GBuffer**」「共享前置**需新建**」「M16.4 接入主帧 tonemap 前；**需先有共享前置 post-chain**」 | 一个里程碑内部就藏着未落地前置；头部「S7 之后先建」的窗口 S7/S8/S9 全过，一步没插 |
| `docs/ROADMAP.md:269,271` | 后处理链 / TSR 基建两行标「**S7 之后**」，备注「开工时补计划」 | 穿插项表的一行，无编号、无 tag、无承载里程碑 |

**Q4 顺延项原文**（三处一致）：

- `docs/s9/S9-PLAN.md:12`：「落盘形态推迟到 **S10 与 S12 历史帧需求一起拍**」
- `docs/MILESTONES.md:2398`：同款「独立 GBuffer 落盘形态仍推迟到 S10 与 S12 历史帧需求一起拍」
- `docs/s9/Q4-GBUFFER-OPTIONS.md:117,179-180`：方案 A「适用前提 = 把资源形态决策留到 S10 与 S12 的 temporal 需求一起拍」；§7 待办 4「把方案 A 的绑定布局 / 状态登记口径固化进 `SHADER-CONVENTIONS.md` §2.18」**尚未执行**

> **定性**：`Q4` 不是解决了，是**顺延**。S9 收官后 S10 正好在门口，这颗雷绕一圈回到开工闸前。本材料就是把顺延项与共享基建链**一次拍完**，不让 `S10` 重演「带病开工」。

### 1.1 本项目实际资源（本轮实读，非文档转述）

| 位置 | 现状 |
|---|---|
| `renderer/src/backend_dx12.cpp:653-694` | `createTargets()` 只建 swapchain `RGBA8_UNORM` + `D32 SceneDepth`；Vulkan 侧 `backend_vulkan.cpp:888` 同构 |
| `renderer/src/scene.cpp:1005-1016` | Hi-Z `R32_FLOAT` 金字塔 `hizPyramid_` |
| `renderer/src/scene.cpp:2338-2376` | `ensureRtTargets` 懒建 `rtTarget_` = `RGBA32_FLOAT` typed UAV（M8.2），`initialState=UnorderedAccess` + `keepInitialState`；M9.0/9.1/9.2 三链共用此目标 + `psRtPresent` / `psRefPresent` 呈现骨架 |
| `renderer/shaders/gbuffer_common.slangh:11-24` | `PURE MOVE` / `Resource-free by design`；`gbInterpolateVisTriangle` + `gbReconstructWorldPos` 是纯函数，无绑定 |
| `docs/SHADER-CONVENTIONS.md:216` | 「多 pass / 多 render target 的 shader 尚未涉及」——**MRT 是本项目首次** |

**结论**：swapchain framebuffer 直出、无离屏 HDR scene color、无 tonemap pass、无 velocity、无 history ping-pong。`S16-PLAN.md:29` 的「S6 现状并无此 slot」到今天仍成立（S9 只加了公共反解契约，未加任何独立资源）。

---

## 2. 三处定向实读证据（本轮，带路径与行号）

### 2.1 Carrot：velocity 打包进 GBuffer MRT + TAA 在 tonemap **之后**

| 文件 | 行 | 结论 |
|---|---|---|
| `engine/engine/render/GBuffer.cpp` | 42-96 | **7 张 MRT + depth**：Albedo `R8G8B8A8Unorm`、View Positions `R32G32B32A32Sfloat`、ViewSpaceNormalTangents `R16G16B16A16Sfloat`、Flags `R32Uint`、EntityID `R32G32B32A32Uint`、**MetallicRoughnessVelocityXY `R32G32B32A32Sfloat`**、Emissive `R32G32B32A32Sfloat` |
| 同上 | 99-105 | `reuseResourceAcrossFrames(..., 1)` 对 7 张 GBuffer + depth **全部跨帧复用**（注释「used by editor & temporal algorithms」）——即历史帧由 render graph 机制提供，不是显式 ping-pong 成员 |
| `engine/resources/shaders/raster/draw_gbuffer.slang` | 27-38, 99-107 | 双帧矩阵（`CurrentFrame()/PreviousFrame()` 的 `jitteredProjection` + `lastFrameTransform`）；velocity = 上一帧 clip.xy − 当前 `svPos.xy`，再 `+= (cur.jitter + prev.jitter) * 0.5` **剥离 jitter**；写进 `result.motionVector.xy` 即上述第 6 张 MRT 的 zw |
| `engine/engine/CarrotRenderingPipeline.cpp` | 69-127 | `makeTAAPass` = compute 8×8；`createStorageTarget("Temporal accumulation", toAntiAlias.format)` + `reuseResourceAcrossFrames(..., 1)`；另有 `Moments & history length` `R32G32B32A32Sfloat` 也跨帧复用；`bindLastFrameTexture` 用 `frame.getPreviousFrameNumber()` 取上一帧 |
| 同上 | 216-249 | `tone-mapping` pass 建 `"Tone mapped"` RT（HDR 格式，注释「`RGBA8_UNORM` 会让 temporal blend 失败」）→ **`finalTAA = makeTAAPass("final", toneMapping.getData().postProcessed, ...)`，TAA 在 tonemap 之后** |
| 同上 | 352-373 | `blit-to-swapchain` → `present` |
| `engine/resources/shaders/post-process/taa.slang` | 20-38, 47-78 | 3×3 closest-depth 邻域从 GBuffer 读 motionVector；`currentFrameColor` / `accumulationBuffer` / `momentsAndHistory`；`Load` + 3×3 min/max 邻域钳制 + α=0.1 |
| `engine/engine/vulkan/VulkanDriver.cpp` | 854-859 | HDR 格式首选 `B10G11R11UfloatPack32`、fallback `R32G32B32A32Sfloat`，要求同时支持 `ColorAttachment \| StorageImage \| SampledImage` |

**定性**：Carrot 把 velocity 塞进 GBuffer 的一张 MRT，history 靠 render graph 跨帧复用；**TAA 吃的是 tonemap 后的图**（与 UE 相反）。`S16-PLAN.md:120` 已把「三套时域体系割裂（finalTAA / svgf / temporal-denoise）」列为反例，本项目须收敛成一条。

### 2.2 bevy：velocity 独立 prepass 附件 + history 双纹理 ping-pong

| 文件 | 行 | 结论 |
|---|---|---|
| `crates/bevy_core_pipeline/src/prepass/mod.rs` | 59-60 | `MOTION_VECTOR_PREPASS_FORMAT = Rg16Float`；`NORMAL_PREPASS_FORMAT = Rgb10a2Unorm` |
| 同上 | 73-79, 122-144 | `MotionVectorPrepass` 是**独立组件 / 独立 `ColorAttachment`**；`ViewPrepassTextures` 里 depth / normal / motion_vectors / deferred 各为 `Option<ColorAttachment>`，并列而非 GBuffer 的一部分 |
| 同上 | 99-115 | `PreviousViewData` 6 个矩阵含 `unjittered_clip_from_world`，独立 `PreviousViewUniforms` uniform buffer |
| 同上 | 392-418 | `prepass_target_descriptors` 按开关生成 MRT 槽（normal / motion / deferred / deferred_lighting_pass_id）——**可选 MRT** |
| `crates/bevy_core_pipeline/src/prepass/background_motion_vectors.wesl` | 17-31 | 天空像素（`depth==0`，reversed-Z）用 `GreaterEqual` 深度测试只写未覆盖像素；**两侧都用 `unjittered_clip_from_world` 剥离 jitter**；`(0.5,-0.5)` 转 uv |
| `crates/bevy_anti_alias/src/taa/mod.rs` | 110-114 | `#[require(TemporalJitter, MipBias, DepthPrepass, MotionVectorPrepass)]` —— TAA **显式声明依赖四个组件**，这是「共享前置」的组件化表达 |
| 同上 | 169-207 | `view_target.post_process_write()` 取 source/destination；**一个 render pass 挂两个 color attachment**（destination + history write）；bind group 6 项 = source / history.read / motion_vectors / depth / nearest / linear |
| 同上 | 392-433 | history **两张纹理** `format: view.target_format`，按 `frame_count.is_multiple_of(2)` 交换 read/write |
| `crates/bevy_render/src/view/mod.rs` | 795-799, 917-989 | `ViewTarget` 持 `MainTargetTextures` a/b + `AtomicUsize`，`post_process_write()` 自然交换 —— 离屏 HDR **双缓冲** |

**定性**：bevy 的 velocity、normal、depth 各自独立，GBuffer 属性在 meshlet 材质 pass 不落盘（`Q4-GBUFFER-OPTIONS.md:46` 已实读）；solari 另落 1 张 packed `vec4<u32>` + `previous_gbuffer` / `previous_depth_buffer`（同上 :47-49）。**两套并存，取哪套取决于消费端是否需要跨帧/邻域属性**。

### 2.3 UE 5.8.4：独立 SceneVelocity + TSR 在 tonemap **之前**（只读对照，未抄代码）

| 文件 | 行 | 结论 |
|---|---|---|
| `Renderer/Private/SceneTextures.cpp` | 753-754 | `SceneTextures.Velocity = GraphBuilder.CreateTexture(FVelocityRendering::GetRenderTargetDesc(...), TEXT("SceneVelocity"))` —— **独立 RT** |
| 同上 | 756-797, 904-950 | `GBufferA~F` 按 `FGBufferBindings` 的 `Index >= 0` **动态启用**；第一槽恒 `Color.Target`；`MaxTargets = 8`（`UE-DIFFS.md` D12 已实读） |
| `Renderer/Private/VelocityRendering.cpp` | 788-812 | velocity 格式 `PF_G16R16`（16bit UNORM ×2），需要 velocity depth 时 `PF_A16B16G16R16`；`GetCreateFlags` = `RenderTargetable \| UAV \| ShaderResource`；`FClearValueBinding::Transparent` |
| `Renderer/Private/PostProcess/PostProcessing.cpp` | 459 | `SceneColorFormat = PF_FloatR11G11B10`（带 alpha 时 `PF_FloatRGBA`）—— HDR scene color |
| 同上 | 507-514 | `EPass` 枚举顺序 `MotionBlur → PostProcessMaterialBeforeBloom → Tonemap → FXAA/SMAA → ...` |
| 同上 | 1107-1143 | `TAAConfig != Disabled` 时构造 `FDefaultTemporalUpscaler::FInputs`，吃 `SceneColor / SceneDepth / SceneVelocity(Velocity.Texture)`；**这段在 `AddTonemapPass` 之前** |
| 同上 | 1577, 1614-1639 | `SceneColorBeforeTonemapSlice` → `AddTonemapPass` → `SceneColorAfterTonemap` |

**定性**：UE 的 velocity 是独立 RT、TSR 在 tonemap **之前**、history 五套 array（`S16-PLAN.md:15` 已实读）。GBuffer MRT 服务 base pass / deferred lighting，**Lumen HWRT 直光不读屏幕 GBuffer**（`Q4-GBUFFER-OPTIONS.md:61-64` 已实锤），故「阴影/探针必须先有屏幕 GBuffer」不是工业终态的必然前提。

### 2.4 三处口径对照与不一致点

| 维度 | Carrot | bevy | UE 5.8.4 |
|---|---|---|---|
| velocity 落点 | 打包进 GBuffer 第 6 张 MRT 的 zw（`RGBA32F`） | **独立** `Rg16Float` prepass 附件 | **独立** `PF_G16R16` SceneVelocity RT |
| velocity 格式 | `R32G32B32A32Sfloat` 打包（浪费 2 通道） | `Rg16Float` | `G16R16` UNORM（需 [-1,1]→[0,1] 编码） |
| history 形态 | render graph `reuseResourceAcrossFrames(...,1)` + 独立 accumulation storage target | **两张** `view.target_format` 纹理显式 ping-pong | TSR 五套 history array |
| 属性 GBuffer | 7 张 MRT 全落 | 材质 pass 不落；solari 落 1 张 packed `uint4` + 历史帧 | `GBufferA~F` ≤8 槽按配置 |
| **TAA/TSR vs tonemap** | **之后**（finalTAA 吃 tonemap 输出） | post-process 链节点（`post_process_write()`） | **之前**（UpscalerInputs → Tonemap） |
| HDR scene color | `B10G11R11UfloatPack32` / `RGBA32F` | `ViewTarget` a/b 双缓冲 | `PF_FloatR11G11B10` |

> **三处不一致**（velocity 落点 / history 形态 / **tonemap 相对位置** / 属性是否落盘）。按 `AGENTS.md`「三处来源口径不一致、需二选一或综合」= **必须摆证据请用户拍板**，不得自行择优。

---

## 3. 本项目历史约定（硬约束，本轮实读）

| 来源 | 条款 | 对共享基建的约束 |
|---|---|---|
| `docs/SHADER-CONVENTIONS.md` §2.5 + `CMakeLists.txt:419,423`（本轮实读） | `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0`；所有资源编进 **vk set 0** | 新增 UAV / SRV 不得改 shift 口径；u 槽已有 384 偏移。`Q4-GBUFFER-OPTIONS.md:83` 记的 `CMakeLists.txt:411` 是旧行号，以 423 为准 |
| §2.7 | **每 graphics/compute 管线恰好一个 BindingSet**；多份资源并进一个 layout/set（多 set 在 DX12 正常、Vulkan 静默读 NaN） | 每个新 pass 各建自己的单一 set；不许跨 set 引用 |
| §2.8 | **帧内禁止开第二个 immediate command list**；懒建资源的清屏塞进当前帧 cmd | 离屏 RT 首帧建 / clear 必须并入帧列表，不能另开列表 |
| §2.15 | `setPermanentBufferState` 一次性冻结；**跟踪状态不跨命令列表**；写读交替的资源不设 permanent，用成员跨列表跟踪末态 | 纹理侧对应逐张 `beginTrackingTextureState` 每帧 `RenderTarget ↔ ShaderResource` 切换；不可对 ping-pong 纹理设 permanent 态 |
| §2.16 | draw 主路径重构保留 `modelBatches_.empty()` 单批兜底；spot 必含 lod/dag；基线 diff 超 ≈527px 签名必须追因、禁强刷 | 主帧从直出 swapchain 改渲到离屏 RT **属主路径重构**，spot 不能只测 init/per-prim |
| §2.9 | 公共 `#include` 头用 `.slangh`，不进 CMake `*.slang` GLOB | velocity / history 的公共数学契约若抽头，同样用 `.slangh` |
| `docs/MILESTONES.md` s8-done 段 + `.workbuddy/memory/MEMORY.md` | `kStage` bump → `args` 含 `--hud` 且 `exe=current` 的卡全重拍（约 50 张 HUD 基线）；`reshot-hud-stage.py` 超 527px 签名须追因才许 `--force` | 方案若 bump `kStage`，代价可量化；不 bump 则新卡须 `run.bat verify` + `regression.py --only` 双验 |
| `AGENTS.md` 交付纪律 1b / 第 3 条 | 主线收口按 tag + 四栏台账 + 全量总闸 + `--check`；`--check` 由 `kStage` 判定，不 bump 时只校验旧版本卡 | 新里程碑的 example 卡不能只靠 `server.py --check` 交付 |
| `docs/s9/S9-PLAN.md` Q4 行 | **已拍板：方案 A 不落盘，即取即算**；本里程碑 0 新增独立 GBuffer 纹理 / MRT / 落盘 UAV | Q4 顺延项若拍「落盘」，是对已拍结论的**范围扩展**，须写清「S9 不落、S10 前落」的边界，不能表述成推翻 |
| `docs/s6/SSR-RESEARCH-NOTES.md` §3/§4 | 「离屏 RT 合成与 post-chain 穿插项是**同一份**基建，别重复建设」；SSR 需法线/粗糙度，插点 S7 之后 | 共享前置必须**一份**，SSR / S16 / bloom 共用，不各建一份 |
| `docs/s5/UE-DIFFS.md` D12 | 「离屏 HDR RT + post-chain 单列为 SSR / S16 / bloom 共享前置」= 计划内待办 | 本材料若拍板，D12 的「待办」应转「已定形态」 |

---

## 4. 下游消费端到底要什么（决定方案扩展面）

| 消费端 | 硬需求 | 对资源形态的约束 |
|---|---|---|
| `S10` M10.0 探针重投影 | 逐像素 motion vector（或明确接受简化版） | velocity 必须是**当前帧可读**的纹理；若照 `S10-PLAN.md:11` 走简化版，本里程碑可只交 velocity 不交 history |
| `S10` M10.2 final gather | 邻域探针权重需像素世界位置 / 法线 | 不落盘时靠 `gbuffer_common.slangh` 即取即算（重复 resolve ×N）；落盘时一次 `textureLoad` |
| `S10` M10.3 | 与 **S10 自立 GI 参考图**并排（Q3 已勘误） | 与形态无关，但 post-chain 会改变画面输出路径，须保证并排读盘同机位 |
| `S12` M12.0 | motion vector 回投 + 历史帧 α 混合 | 需 velocity + **history color**（至少 1 张跨帧） |
| `S12` M12.2 历史拒绝 | 重投影深度差 > 阈值 **或法线差 > 阈值** → 丢历史 | 不落 previous normal 就得退化为「只用深度差 + 当前帧法线」，或补一张 previous 属性 |
| `S15` M15.1 页请求 | **只读**主视图 depth / vis buffer 即取即算属性（Q4 方案 A 勘误后） | 不改输出格式；页缓存是本项目第一次跨帧持久 GPU 资源，`createTargets()` slot 级同步表须前置遵守 |
| `S16` M16.0 / M16.1 / M16.4 | jitter + 双帧矩阵 + 完整逐像素 velocity + history ping-pong + 「tonemap 前 slot」 | 本里程碑若承载，S16 头部「S7 之后先建」应改为「由 M-T0 承载」，M16.4 的挂账句可删 |
| SSR / bloom 穿插项 | 离屏 HDR scene color + 合成 pass + 颜色 mip 链 | 共享同一份 post-chain；`SSR-RESEARCH-NOTES.md:44` 点名「场景颜色补 GenerateMips」 |

---

## 5. 三个可拍板方案

> 显存口径统一按展示台 `960×540 = 518,400 px`；带宽按每帧「写 1 次 + 每个消费端各读 1 次」估算。
> **NVRHI 格式名已实读核对**（`thirdparty/nvrhi/include/nvrhi/nvrhi.h:191,196,197,202,207,211`）：`R11G11B10_FLOAT` / `RG16_FLOAT` / `R32_UINT` / `RGBA16_FLOAT` / `RG32_FLOAT` / `RGBA32_UINT` 均存在。
> **不预设槽位号**（`Q4-GBUFFER-OPTIONS.md:22` 18:0x 纠偏教训）：绑定布局只写结构与纪律，槽位在落码时按实读固化进 `SHADER-CONVENTIONS.md` §2.18。

### 方案 A — 单里程碑「temporal 最小集」：velocity + history + 最小 post-chain，属性**继续不落盘**

| 维度 | 内容 |
|---|---|
| **里程碑承载** | 新建**一个**里程碑插在 `s9-done` 之后、`s10-place` 之前（编号 / tag 名拍板后定，建议 `M-T0` 类命名，不占 `S*` 大版本号）；`S16-PLAN.md:3` 头部「velocity/history 基建 S7 之后先建」改为「由本里程碑承载」，三段拆分表标清哪几步归它 |
| **RT / UAV 数量与格式** | 新增 4 张（+1 张可选）：① 离屏 HDR scene color `R11G11B10_FLOAT`（1.98 MiB，Carrot/UE 同款紧凑 HDR）；② 独立 velocity `RG16_FLOAT`（1.98 MiB，bevy 口径；比 Carrot 的 `RGBA32F` 打包省 4×，比 UE 的 UNORM 少一层编码）；③ history ping-pong **2 张** `R11G11B10_FLOAT`（3.95 MiB，bevy 口径按帧奇偶交换）；④ 可选 tonemap 后离屏输出（若 Q-H3 选 UE 口径则直接写 swapchain，不建这张）。**新增显存约 7.9 MiB**；`D32 SceneDepth` 复用现有 |
| **写入 pass 形态** | 主帧从直出 swapchain 改为渲到离屏 HDR RT（**属 §2.16 主路径重构**）；velocity 走**独立 geometry/prepass 附加 MRT**（bevy/UE 口径，双帧矩阵 + `unjittered` 剥离 jitter，天空像素单独补写）；新增最小全屏 `present/tonemap` pass 做合成 + gamma。全部并入现有帧命令列表（§2.8） |
| **Q4 顺延项答复** | **不落盘**。`S10` 邻域 / `S12` disocclusion 靠 `gbuffer_common.slangh` 即取即算 + depth 反解；`S15` 维持只读 depth / vis。代价：`S12` M12.2 的「法线差 > 阈值」判据退化为「深度差 + 当前帧法线」，或到时补一张 previous normal |
| **绑定布局 / 状态登记** | 每个新 pass 各建**单一 BindingSet**（§2.7）；shift 沿用 §2.5（b 256 / t 0 / s 128 / u 384，set 0）；ping-pong 纹理不设 permanent，每帧逐张 `beginTrackingTextureState` 声明当前态（§2.15）；clear 在 `setGraphicsState` 前、帧内不另开 cmd list（§2.8） |
| **显存 / 带宽 / 精度** | ~7.9 MiB；每帧 3 写 + 各消费端读；HDR 全 float 无量化，但 `R11G11B10` 无 alpha 且 mantissa 较窄，tonemap 前 HDR 高光精度需实测；velocity `RG16_FLOAT` 半精度对大位移需实测 |
| **基线改动面** | **中等偏大**：主帧换 RT 属主路径重构，现有双后端截图基线的 gamma / 清屏路径可能被碰；`kStage` 若不 bump，须 `run.bat verify` + `regression.py --only` 双验新卡，并跑 `s8.2` / `s9-*` / `pbr-direct` 保形 spot；spot 必含 lod/dag（§2.16） |
| **下游扩展面** | `S10` M10.0 有真 velocity（删简化版占位）；`S12` M12.0 声明落地；`S16` M16.0/M16.1 基建被承载、M16.4 挂账句可删；SSR / bloom 有共享 post-chain 落点；`S15` 零影响 |
| **风险** | ① `S12` 法线历史判据退化；② 主路径重构是本项目 M7.2 真回归的高发区，保形证据必须够；③ 属性落盘**第三次顺延**（S9 → S10 → S12），若 `S12` 真要 `previous_gbuffer`，等于再建一轮 |

### 方案 B — 单里程碑「一次建齐」：A + packed 属性 GBuffer + 历史帧（Q4 顺延项一次拍死）

| 维度 | 内容 |
|---|---|
| **里程碑承载** | 同 A，但范围含属性落盘；`S9-PLAN.md` Q4 行需补「S9 不落、**M-T0 落**」的边界说明，`ROADMAP.md:171` 的「Q4 方案 A：S9 不落 GBuffer 纹理」同步勘误，避免全仓旧口径残留 |
| **RT / UAV 数量与格式** | A 的 ~7.9 MiB **+** 1 张 `RGBA32_UINT` packed gpixel（7.91 MiB，bevy solari `gpixel_resolve` 口径：R=baseColor+roughness 4×8unorm / G=emissive RGB9E5 / B=props / A=24bit 八面体法线+flags，**worldPos 不落盘**由公共头反解）**+** 1 张 `previous_gbuffer`（7.91 MiB）+ 1 张 `previous_depth`（1.98 MiB，或复用 depth 双缓冲）。**新增显存约 25.7 MiB** |
| **写入 pass 形态** | A 的 pass 链 + 一个属性写入 pass（全屏 graphics 写 1 张 uint RT，或 compute 8×8 写 `RWTexture2D<uint4>`）；`previous_*` 用 ping-pong 而非额外拷贝 |
| **Q4 顺延项答复** | **落盘**，且同时拍死 `S12` 历史帧形态（bevy `restir.wesl` 的 `previous_gbuffer` / `previous_depth_buffer` 同款）。`S10` 邻域采样一次 `textureLoad`；`S12` M12.2 法线拒绝判据完整；`S15` 页请求可读属性 |
| **绑定布局 / 状态登记** | 同 A，但属性纹理 + 历史纹理逐张纳入状态登记；uint RT 在 Vulkan 作 color attachment 需核 NVRHI 格式支持；若走 compute UAV 则 u0 走 384 偏移并核 typed UAV 能力 |
| **显存 / 带宽 / 精度** | ~25.7 MiB；**代价是量化**：8bit/通道 baseColor、24bit 八面体法线、RGB9E5 emissive，与现有 `mode 5` 全 float 路径**不可能逐像素一致** → 必须保留 mode 5 原反解路径作旁路，长期维护两条路径 |
| **基线改动面** | **最大**：主路径重构 + 新 pass + 新纹理 + 新 example 卡；量化旁路一旦漏掉，`pbr-direct` / `s7.2` / `s9-*` 基线全 FAIL；若 `kStage` bump 则约 50 张 HUD 基线重拍 + 527px 签名追因 |
| **下游扩展面** | 最完整：`S10/S12/S15/S16` 全部有确定落点，**不再返工**；`Q4` 顺延项一次性关闭 |
| **风险** | ① 量化与「逐像素一致」硬验收哲学冲突（Q4 材料 §6 已警示）；② 范围最大、工期最长，且 `S10` 探针是否真需要邻域属性尚属预判，可能过度设计；③ 改动已拍板结论的**范围**，须全仓扫旧词（`ROADMAP` / `S15-PLAN:48` / `UE-DIFFS` D11 / `MILESTONES` / `Q4-GBUFFER-OPTIONS`） |

### 方案 C — 拆两次：S10 前只建 velocity，post-chain / history / Q4 属性推到 S12 前

| 维度 | 内容 |
|---|---|
| **里程碑承载** | 两个里程碑。第一次只交 velocity + 双帧矩阵（1 张 `RG16_FLOAT`，约 1.98 MiB），让 `M10.0` 立刻有真 velocity；离屏 HDR / tonemap / history / 属性形态留到 `S12` 前第二个里程碑 |
| **写入 pass 形态** | 不动主帧 present 路径，只加一个 velocity 附加写入 pass → **现有基线改动面最小**（`kStage` 可不 bump） |
| **Q4 顺延项答复** | **仍未拍**。`post-chain` 依旧是穿插项表的一行，`S16-PLAN.md:93` M16.4「需先有共享前置 post-chain」继续悬空，`S12-PLAN.md:10` 声明复用的「S16 前半段建好的」仍无承载 |
| **下游扩展面** | 只解 `S10` M10.0 一条；`S12` / `S16` / SSR / bloom 全部继续挂账；第二个里程碑要**再动一次主帧 present 路径** |
| **风险** | **本质是把本轮要解决的决策再推一次**，直接违反 `AGENTS.md` 开工闸「架构判定未定不许开里程碑落码」；且主帧重构做两遍，基线重拍两轮。列出仅供对照，**不建议** |

---

## 6. 横向矩阵

| | A 最小集 | B 一次建齐 | C 拆两次 |
|---|---|---|---|
| 里程碑数（S10 前） | 1 | 1 | 1（+ S12 前再 1） |
| 新增纹理 | 4 张（+1 可选） | 7 张（+1 可选） | 1 张（第二阶段另算） |
| 新增显存 @960×540 | ~7.9 MiB | ~25.7 MiB | ~2 MiB（第一阶段） |
| Q4 顺延项 | **拍为「不落盘」** | **拍为「落 packed」** | **未拍** |
| 主帧 present 路径 | 重构 1 次 | 重构 1 次 | 不动（第二阶段再动 1 次） |
| 现有基线改动 | 中 | 大（量化旁路） | 小 |
| `S10` M10.0 | 真 velocity | 真 velocity + 邻域属性 | 真 velocity |
| `S12` M12.0 / M12.2 | history 有；法线判据退化 | 完整 | 无 history，声明继续悬空 |
| `S16` M16.4 挂账句 | 可删 | 可删 | 仍悬空 |
| SSR / bloom 共享 post-chain | 有落点 | 有落点 | 仍无 |
| 精度 | 全 float 无量化 | **有量化** | 全 float |
| 三处依据 | bevy prepass / UE 解耦 / UE tonemap 前 | bevy solari `previous_gbuffer` / Carrot MRT | 无（纯进度取舍） |
| 再返工概率 | 中（`S12` 或需 previous normal） | 低 | **高**（决策再顺延） |

---

## 7. 建议（**建议，不是结论，不替用户拍板**）

倾向 **方案 A + Q-H3 选 UE 口径（tonemap 前）**：

1. **三处证据里，velocity 独立成一张、history 独立 ping-pong、post-chain 与属性落盘解耦，是 bevy 与 UE 的共同口径**；Carrot 把 velocity 打包进 GBuffer MRT 是「从零起步」的历史选择，且 `S16-PLAN.md:120` 已把它的三套时域体系割裂列为反例。方案 A 恰好落在两家共同点上。
2. **Q4 已拍「方案 A 不落盘」**，A 案不翻案、只把顺延项明确拍为「不落」，改动面与基线风险最小；`S15-PLAN.md:48/50/66/102` 已按「无屏幕 GBuffer 纹理」全仓勘误过，B 案要再改一轮。
3. **真正逼出属性落盘的是 `S12` 的历史帧需求**，而 `S12` M12.2 的法线差判据可以用「深度差 + 当前帧法线即取即算」先顶住；先建 temporal 骨架、属性形态留到 `S12` 有了实测噪声证据再定，比现在替两个未开工里程碑定死格式更稳。这一点**与 Q4 材料 §6 第 3 条同逻辑，但必须承认代价：属性落盘会第三次顺延**，用户若不能接受，应选 B。
4. **Q-H3 建议 UE 口径**：`S16-PLAN.md:123` 已按「TSR 在 tonemap 之前、Lumen GI 之后」定为底线，UE 是工业终态；Carrot 的 tonemap 后 TAA 是它的简化实现，且与本项目 S16 计划冲突。选 UE 口径还能省掉一张 tonemap 后离屏输出。
5. **方案 C 明确不建议**：它把本轮要关闭的架构判定再推一次，等于重复 M9.0 的「待拍板就开工」，且主帧重构做两遍。

**方案 B 的适用前提**：若用户要求「Q4 顺延项本轮必须彻底关闭、不许再留任何落盘未定项」，B 是唯一满足的；但必须接受量化旁路、约 25.7 MiB 显存、最大基线改动面，以及全仓旧口径勘误。

**必须说清的诚实边界**：本材料给的是**资源形态与排期的选项**，不是「已验证可落地」的结论。`R11G11B10_FLOAT` 的 color attachment + storage + sampled 三能力、`RG16_FLOAT` 作 MRT 附件、uint RT 在 Vulkan 的可渲染性、以及主路径重构后的保形结果，都要在拍板后的落码里程碑里双后端实测；本轮零代码、零实测。

---

## 8. 拍板后动作清单（**本轮一条未执行**，按此顺序）

1. 结论写回 `docs/s9/S9-PLAN.md` 开工拍板记录（Q4 行补「顺延项已于 M-T0 拍板」），以及 `docs/MILESTONES.md` 的「下一步」行；删 `S9-PLAN.md:12` / `MILESTONES.md:2398` 的「推迟到 S10 与 S12 一起拍」挂账句。
2. **新建 `*-PLAN.md` 任务书**（编号 / tag 名按拍板定），把「离屏 HDR RT + post-chain + velocity + history」从 ROADMAP 穿插项表收进这个有编号的筐；`docs/ROADMAP.md:269,271` 两行的「S7 之后 / 开工时补计划」改为指向该里程碑。
3. 同步下游声明：`docs/s10/S10-PLAN.md:11` 删「复用 M12 要做的 / 简化版同屏位置直接挪」；`docs/s12/S12-PLAN.md:10` 删「复用 S16 前半段（S7 后）建好的」改为指向已落地资源；`docs/s16/S16-PLAN.md:3,29,55-59,93` 的「S6 现状并无此 slot / 需新建 / 需先有共享前置 post-chain」按新形态改写，三段拆分表标清哪几步归 M-T0。
4. **全仓扫旧口径**（`AGENTS.md`「拍板后全仓扫旧口径」条）：`S16-PLAN.md:59` 仍写「S9 GBuffer」（Q4 方案 A 后不成立）；`ROADMAP.md:171`、`S15-PLAN.md:48`、`UE-DIFFS.md` D11/D12、`Q4-GBUFFER-OPTIONS.md:3-4,179` 的状态行；逐处勘误并 append 当日日志。
5. 补 `Q4-GBUFFER-OPTIONS.md` §7 待办 4：把拍板后的绑定布局 / 状态登记口径固化进 `docs/SHADER-CONVENTIONS.md` **新增 §2.18**（含 shift、单一 BindingSet、逐张 `beginTrackingTextureState`、ping-pong 不设 permanent）。
6. 删除 `S11-PLAN.md:12,18` 的「开工时按笔记定夺（不阻塞）」/「开工时的第一个决策点」——同型残留，属上一轮 review 第 2 条；以及 `S16-PLAN.md:3` 头部与线性 5 步表的分段不对应。
7. 之后才允许写第一行 RT / UAV / 槽位 / 格式代码；交付照 `AGENTS.md` 纪律：四项自查、四栏台账、修改文件清单、tag、example 卡 + `run.bat verify` + `--only` 双验、`--check`（不 bump 时只校验旧阶段卡）。
8. 主线收口按 tag + 四栏台账 + 全量总闸 + `--check`，**不跑 `wt_pool.py close`**（该闸只对并行轨道生效，见 `AGENTS.md` 1b）。
