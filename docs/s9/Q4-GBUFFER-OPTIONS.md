# Q4 决策材料：GBuffer 落盘形态（S9 / M9.0，2026-11-10 补决策前置功课）

> **状态：已拍板（2026-11-10 用户选方案 A「不落盘，即取即算」）。** 本文件保留为**决策证据**（三处实读 + 历史约定 + 三方案代价矩阵），**结论以 `S9-PLAN.md` 开工拍板记录 Q4 行为准**；§6 的「建议」在拍板前撰写，保留原样供追溯，不代表后续落码口径。
> 拍板后动作**已执行**：结论已写回 `S9-PLAN.md` Q4 行、任务书第 3 条挂账句已删、范围口径已同步；冻结解除仅限与方案 A 相容的落码。
> 触发：M9.0 开工时 S9-PLAN Q4 仍标「待拍板」却已启动落码，用户 2026-11-10 点名「架构判定没拍板就开工」的别扭状态。按 `AGENTS.md` 决策前置铁律补功课。

---

## 0. 冻结面（Q4 拍板前不许写第一行）

| 项 | 状态 |
|---|---|
| 新增独立 GBuffer 落盘（任何 RT / UAV / `register(u/t)` 槽位 / 格式） | **冻结** |
| 绑定布局、BindingSet 扩建、纹理状态登记、CMake 新 entry | **冻结**（拍板前）。**澄清**：冻结的是「独立 GBuffer 落盘资源的形态与槽位」，不是 compute 直光 pass 按 §2.7 必备的单一 BindingSet |
| 在注释 / README / commit message / CMake 里顺手把形态写死 | **冻结** |
| `renderer/shaders/gbuffer_common.slangh`（PURE MOVE，0 处资源声明） | **保留，不回滚**（合规机械执行） |
| `material.slang` 纯搬移（-54/+11）+ `CMakeLists.txt` `_GBUFFER_COMMON` 依赖 | **保留，不回滚** |
| 双后端构建 + 4 条 spot 回归（8/8 `bytes`） | 已完成，作为 M9.0 硬验收证据 |

冻结面机器核查：`grep -n "RWTexture\|RWStructured\|RWBuffer\|register(u\|register(t\|Texture2D\|RenderTarget\|MRT\|SV_Target[1-9]" renderer/shaders/gbuffer_common.slangh` → 0 命中。

> **2026-11-10 18:0x 纠偏**：本材料 §2 现状资源表与方案 A「绑定布局」曾把 `rt` 的 t0(TLAS) 与 `material` 的 t0(vis) 混编成一张槽表，漏写 `initRt → initDag`（不产 vis buffer）与 `initVisbuf(pulling)` 互斥、`--mode shadow` 尚未注册。已改为「槽位落码时按实读固化进 §2.18」，决策材料不再预设槽位号。依据见 `S9-PLAN.md`「落码前实读事实」。

---

## 1. 三处定向实读证据（本轮，带路径与行号）

### 1.1 Carrot：多张 MRT 落盘 + compute 直光，且自带「合并 RT」编码版

| 文件 | 行 | 结论 |
|---|---|---|
| `engine/resources/shaders/lighting/direct-raytracing.slang` | 9-15 | `[shader("compute")] [numthreads(8,8,1)]`，调 `computeDirectLighting` |
| `engine/resources/shaders/lighting/direct-base.slang` | 11, 22, 32, 62 | `ParameterBlock<GBufferInputs> gBufferInputs`；`PixelInfo(cameras.CurrentFrame(), gBufferInputs, uv)`；输出 `RWTexture2D<float4> outDirectLightingImage.Store(...)` |
| `engine/resources/shaders/includes/gbuffer_output.glsl` | 1-7 | **7 个 MRT `layout(location=0..6)`**：albedo vec4 / viewPosition vec4 / viewNormalTangent vec4 / intProperty uint / entityID uvec4 / metallicRoughnessVelocityXY vec4 / emissiveVelocityZ vec4，由 graphics base pass 写出 |
| `engine/resources/shaders/includes/gbuffer_input.glsl` | 1-12 | 读取端 12 个 binding（7 张 GBuffer + depth + skybox + 2 sampler） |
| `engine/resources/shaders/includes/gbuffer_unpack.glsl` | 1-6 | `unpackGBufferLight` 注释：「without emissive, metallic+roughness nor motion vectors **to reduce strain on texture memory**」 |
| `engine/resources/shaders/modules/gbuffer.slang` | 18-20, 58-61 | `GBuffer`（fat）与 `EncodedGBuffer`；注释原文：「**to reduce VRAM usage and render target count, some render targets are merged together**」 |

**定性**：Carrot = graphics base pass 写多张 MRT → compute 直光读；同时承认 RT 数量/显存是问题，另备 packed 编码版。直光输出是**一张 UAV**，不是 MRT。

### 1.2 bevy：两套口径并存，彼此不一致

| 文件 | 行 | 结论 |
|---|---|---|
| `bevy_pbr/src/meshlet/visibility_buffer_resolve.wesl` | 100-176 | `resolve_vertex_output(frag_coord)` 从 `meshlet_visibility_buffer`（u32）拉三顶点 → 透视正确 barycentrics → 直接返回完整 `VertexOutput`（world_position / world_normal / uv / ddx_uv / ddy_uv / tangent / material slot） |
| `bevy_pbr/src/meshlet/meshlet_mesh_material.wesl` | 22, 31 | 材质 pass 直接调上式，**不落 GBuffer** —— 与本项目 `material.slang::psMatResolve` 现状同构 |
| `bevy_solari/src/realtime/bindings.wesl` | 11-14, 27-28 | `@binding(3) gbuffer: texture_2d<u32>`（**1 张 packed uint4**）+ `@binding(4) depth_buffer`；另有 `@binding(13) previous_gbuffer` / `@binding(14) previous_depth_buffer`（**历史帧**） |
| `bevy_solari/src/realtime/gbuffer_utils.wesl` | 13-35 | `gpixel_resolve(gpixel: vec4<u32>, depth, pixel_id, ...)`：R=baseColor+roughness 4×8unorm、G=emissive RGB9E5、B=props、A=24bit 八面体法线+flags；**worldPos 不落盘**，`reconstruct_world_position(pixel_id, depth, view_size, world_from_clip)` 反解 |
| `bevy_solari/src/realtime/restir.wesl` | 97, 130 | `previous_gbuffer` / `previous_depth_buffer` 供 temporal + spatial 复用 |
| `bevy_pbr/src/deferred/functions.wesl` | 20-60, 140-143 | `deferred_gbuffer_from_pbr_input` 产出 `vec4<u32>` → `out.deferred`，写入端是 **graphics fragment pass 的一张 uint RT**，不是 compute |

**定性**：bevy 对「材质着色」走 vis buffer 即取即算（不落盘），对「solari GI/直光」落 **1 张 packed uint4 + depth + 同款历史帧**。两套并存，取哪套取决于消费端是否需要跨帧/邻域属性。

### 1.3 UE 5.8.4：GBuffer MRT 与 Lumen 直光层解耦（只读对照，未抄代码）

| 文件 | 行 | 结论 |
|---|---|---|
| `Renderer/Internal/SceneTextures.h` | 117-124, 132-139 | `FSceneTextures::GetGBufferRenderTargets` 组装 MRT；成员 `GBufferA~F` + `GBufferSGGX`（另有 Depth / Velocity / ScreenSpaceAO） |
| `Renderer/Private/SceneTextures.cpp` | 904-950 | 第一槽恒 `Color.Target`，其余按 `FGBufferBindings` 的 `Index` **动态启用** `GBufferA~E` + `Velocity`（`Index > 0` 才挂） |
| `RenderCore/Public/GBufferInfo.h` | 265-289 | `FGBufferBinding { Index, Format, Flags }`、`FGBufferInfo::MaxTargets = 8` → 按配置启用的 **MRT 子集，上限 8** |
| `Engine/Shaders/Private/Lumen/LumenSceneDirectLightingHardwareRayTracing.usf` | 252-276, 382, 408 | **关键反证**：阴影射线起点/法线取自 `GetLumenCardPageData` → `GetSurfaceCacheData(Card, CardUV, AtlasUV)` 的 `SurfaceCacheData.WorldPosition / WorldNormal`（**Lumen Scene Surface Cache atlas**）；结果写 `ShadowMaskRay` RW 缓冲再合成 `DirectLightingAtlas`。**Lumen 硬件光追直光不读屏幕 GBuffer** |
| `Renderer/Private/Lumen/LumenSceneDirectLighting.cpp` / `...HardwareRayTracing.cpp` | 118-146 / 41-47, 164-172 | bias 组（MeshSDF / GlobalSDF / HWRT ShadowRayBias / EndBias / HeightfieldProjectionBias），与 `UE-DIFFS.md` D19「防 proxy geo 自遮挡」同源 |

**定性**：UE 的 GBuffer MRT 服务于 base pass / 通用 deferred lighting；Lumen 直光层另起 Surface Cache atlas + shadow mask 缓冲，**两套解耦**。因此「阴影射线必须先有屏幕 GBuffer」不是工业终态的必然前提。

### 1.4 三处口径对照

| | 落盘形态 | 写入 pass | 直光消费端读什么 | worldPos 来源 |
|---|---|---|---|---|
| Carrot | 7 张 MRT（另有 packed `EncodedGBuffer` 版） | graphics base pass | 独立 `GBufferInputs` 纹理集 | 落盘 `viewPosition` |
| bevy meshlet 材质 pass | **不落盘** | 全屏材质 pass | vis buffer 即取即算 | 三顶点插值 |
| bevy solari | **1 张 packed `vec4<u32>` + depth（+历史帧同款）** | graphics deferred prepass | `gpixel_resolve(textureLoad(gbuffer,...), depth, ...)` | depth + `world_from_clip` 反解 |
| UE 5.8.4 | SceneColor + `GBufferA~E` + Velocity（≤8 槽，按配置） | base pass MRT | **Lumen 直光不读它**，读 Surface Cache atlas | Surface Cache `WorldPosition` |

> 三处口径**不一致**（多张 MRT / 不落盘 / 单张 packed / 另一套 atlas），按 `AGENTS.md`「三处来源口径不一致、需二选一或综合」= **必须摆证据请用户拍板**。

---

## 2. 本项目历史约定（本轮实读）

| 来源 | 条款 | 对 Q4 的约束 |
|---|---|---|
| `docs/SHADER-CONVENTIONS.md` §2.5（123-137）+ `CMakeLists.txt:411` | `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0`；所有资源编进 **vk set 0** | 新增 UAV / SRV 不得改 shift 口径；u 槽已有 384 偏移 |
| §2.7（139-143） | **每 graphics/compute 管线恰好一个 BindingSet**；多份资源并进一个 layout/set。历史事故：多 set 在 DX12 正常、Vulkan 静默读 NaN | 写 pass 与读 pass 各建自己的单一 set；不许一个管线跨 set 引用 |
| §2.15（203-210） | `setPermanentBufferState` 一次性冻结；**跟踪状态不跨命令列表**，每个新列表 open 后须 `beginTracking...` 如实声明「当前态」；写读交替的资源不设 permanent，用成员跨列表跟踪末态 | 纹理侧对应逐张 `beginTrackingTextureState` 的每帧 RT↔SRV 切换（§2.13 阴影 pass 有完整范式）；若用 compute UAV，须在帧列表内登记 |
| §2.8（144-149） | **帧内禁开第二个 immediate command list**；懒建资源补清屏塞进当前帧 cmd | 新增 GBuffer pass 必须并入现有帧列表，不能另开列表做 clear/barrier |
| §2.16（238-241） | draw 主路径重构保留 `modelBatches_.empty()` 兜底；spot 必须含 lod/dag；**基线 diff 超 ≈527px 签名必须追因、禁强刷** | 任何新增全屏 pass 若改变既有路径，spot 不能只测 init/per-prim |
| `docs/MILESTONES.md` s8-done 段（2166-2180） | M8.2 已落 `rt.slang` `RWTexture2D<float4>` **RGBA32_FLOAT typed UAV**（`ensureRtTargets` 懒建 + 视口依赖 set）+ `csRt` 8×8 inline RayQuery + `vsRtPresent/psRtPresent` `Texture2D.Load` 逐字节确定；`kStage` bump `S8.2` 曾致 **50 张 HUD 基线重拍** | 已有可复用的 compute 8×8 + typed UAV 骨架；改 `kStage` 有确定的重拍代价 |
| `.workbuddy/memory/MEMORY.md:19,23` | `kStage` bump 波及 = args 含 `--hud` 且 exe=current 的卡全重拍；`reshot-hud-stage.py` 超 527px 签名要追因才许 `--force`；§2.14 材质间接表 S9/S10/S13 复用 | 方案 C 若 bump，代价可量化；公共头已为 S10/S12 留好契约 |
| 现状资源（`material.slang:39-72`） | b0/b1/b2 + t0 vis + t1 vertexPool + t2 localIndices + t3 clusterMeta + t4..t7 / t8..t11 贴图数组 + t12 `GPUMaterial` + s0，**单一 BindingSet** | 方案 A 的 compute 版读侧照这套 slot 分配（**另需 u0 输出**）；与 `rt` set 是两条资产链，不混编 |
| 现状资源（`rt.slang:23-36,118`） | b0 `RtConstants` + t0 TLAS + u0 `RGBA32_FLOAT` UAV；present 端 t0 SRV | 方案 A 复用其 `RGBA32_FLOAT` 目标与 present 骨架；**不能把 vis 资源直接塞进这个 set**（`initRt → initDag` 不产 vis buffer） |

---

## 3. 前提修正（自我纠偏，按 AGENTS.md「自我纠偏」条）

上一轮把「**新增独立 GBuffer 落盘**」写进 M9.0 任务书第 3 条，同时把形态标成「待用户拍板」。但 **Q1 用户原文只拍了「重定义为 GBuffer 抽取 + 公共反解头」**，没有拍「落盘」；「落盘」是助手自行展开的架构判定。更关键的是：落盘的第一行代码就是 RT/UAV 数量、格式、槽位——**写下即等于把决策隐式拍死**，而 S10 / S12 / S15 是消费端，返工最贵。

本轮纠正：冻结落盘、补三处实读、摊方案、等用户拍板、拍板后写回计划并删挂账句。已完成的 PURE MOVE 属「已定计划内机械执行」，保留。

---

## 4. 三个可拍板方案

> 分辨率口径统一按现有展示台 `960×540 = 518,400 px` 估算；显存单位 MiB，带宽按每帧「写 1 次 + 消费端各读 1 次」计。

### 方案 A — 不落盘，即取即算（bevy meshlet 材质 pass / UE Lumen 解耦口径）

| 维度 | 内容 |
|---|---|
| **RT/UAV 数量与格式** | **0 张新增 GBuffer 纹理**。M9.1 直光输出复用/扩 M8.2 已有的 `RGBA32_FLOAT` typed UAV（`rt.slang` u0）；无量化、无 packed |
| **写入 pass 形态** | **compute 8×8**（`rt.cs` / Carrot `direct-raytracing.slang` 同款）：单 pass 内 `gbInterpolateVisTriangle` → 发 shadow ray → shade → 写输出 UAV。resolve 与 trace 同 pass，中间不落盘 |
| **绑定布局** | **不预设槽位号**。compute 直光 pass 按 §2.7 建自己的**单一 BindingSet**：读侧照 `material.slang:39-72`（b0/b1/b2 + t0 vis + t1..t3 拉取资源 + t4..t11 贴图数组 + t12 材质表 + s0），写侧复用 M8.2 的 u0 `RGBA32_FLOAT`；资产链走 `initVisbuf(pulling=true)` 而非 `initRt → initDag`。shift 沿用 §2.5（b 256 / t 0 / s 128 / u 384，set 0），**不改编译参数**；状态登记沿用 §2.15 / M8.2 口径。槽位表在落码时按实读固化进 `SHADER-CONVENTIONS.md` §2.18 |
| **显存 / 带宽 / 精度** | 新增显存 **0 MiB**；带宽 = 每个消费端各自重做一次 vertex pulling + barycentrics（vis + vertexPool + indices + clusterMeta 四读），**算力重复**但无额外显存往返；精度 = 原生 float32，与 mode 5 逐位同源，保形零风险 |
| **S10 / S12 / S15 扩展面** | S10 screen probes 需邻域像素属性 → 邻域也要重 resolve，代价 ×N；**S12 temporal 需要上一帧属性 → 等于必须补一份 history 落盘**（bevy `previous_gbuffer` 正是为此）；S15 VSM「只读 GBuffer」底线（D11）需重新理解成「只读反解契约」。**落盘决策被推迟，但不会消失** |
| **现有基线改动面** | **0**。`kStage` 不 bump，现有 HUD / pbr-direct / s7.2 / s8.2 基线全不动；M9.0 交付 = 公共头 + 双后端 8/8 `bytes` 报告。M9.1 新增 `--mode shadow` 时才新建 `example/s9-shadow/` 基线 |
| **适用前提** | 认可 M9.0 = 「抽公共契约」，把资源形态决策留到 S10 与 S12 的 temporal 需求一起拍 |

### 方案 B — 单张 packed `RGBA32_UINT` GBuffer + depth（bevy solari 口径）

| 维度 | 内容 |
|---|---|
| **RT/UAV 数量与格式** | **1 张 `RGBA32_UINT`（16 B/px，约 7.91 MiB）+ 1 张 depth**（可复用现有 depth，或新增 `R32F` 约 1.98 MiB）。打包：R=baseColor.rgb + roughness（4×8unorm）、G=emissive（RGB9E5）、B=props、A=24bit 八面体法线 + flags。**worldPos 不落盘**，由公共头 `gbReconstructWorldPos(pixel, deviceZ, size, invViewProj)` 反解（口径已与 `rt.slang::csRt` 核过一致） |
| **写入 pass 形态** | 推荐复用现有**全屏 graphics pass**（`vsMatResolve` 全屏三角 + 新 `psGBufferWrite`，MRT 仅 1 张 uint RT，`SV_Target0`；bevy deferred prepass 同款）；备选 compute 8×8 写 `RWTexture2D<uint4>` u0。子形态随本方案一并定，若要换请在拍板时注明 |
| **绑定布局** | 写 pass 一个 set（b0/b2 + t0 vis + t1..t3 顶点/簇数据 + s0 + 1 个 uint RT），读 pass 一个 set（t0 packed + t1 depth + b0 invViewProj），**每管线各一个 set**（§2.7）；uint RT 在 Vulkan 作 color attachment 需核 NVRHI 格式支持；若走 compute UAV 则 u0 走 384 偏移并核 typed UAV 能力 |
| **状态登记（§2.15 / §2.8）** | 纹理每帧 `RenderTarget ↔ ShaderResource` 切换须逐张 `beginTrackingTextureState` 如实声明当前态；clear 在 `setGraphicsState` 前；**不另开帧内第二 cmd list**（§2.8）。不可对这张纹理设 permanent 态 |
| **显存 / 带宽 / 精度** | 7.91 MiB（+ depth 1.98 MiB）；带宽 = 1 写 + N 读，最省。**代价：量化** —— 8bit/通道 baseColor、24bit 八面体法线、RGB9E5 emissive，与现有 mode 5 全 float 路径**不可能逐像素一致**；要保住 M9.0 硬验收，必须让 mode 5 继续走原反解路径、packed 仅作旁路，长期维护两条路径 |
| **S10 / S12 / S15 扩展面** | 最优：一次 `textureLoad` 拿全属性；S12 temporal 直接照 bevy 加 `previous_gbuffer`（显存翻倍到 ~15.8 MiB）；S15 读 worldPos 需 depth + 相机矩阵（公共头已备） |
| **现有基线改动面** | 新增 1 个 pass + 1 张纹理 → 需新 `example/s9-gbuffer/` 基线 + `cases.json` 版本卡 + `server.py --check`；若不动 mode 5，`kStage` 可不 bump，现有基线 0 改动；一旦让 mode 5 改读 packed，pbr-direct / s7.2 基线全 FAIL（量化） |

### 方案 C — 精简 MRT 2~3 张 float（Carrot / UE 口径）

| 维度 | 内容 |
|---|---|
| **RT/UAV 数量与格式** | **2 张 `RGBA32_FLOAT`**（RT0 = worldPos.xyz + deviceZ；RT1 = normal.xyz + materialId/metallic/roughness 打包）起步，可选 RT2 = albedo + emissive。每像素 32 B → 2 张约 **15.8 MiB**、3 张约 **23.7 MiB** |
| **写入 pass 形态** | **graphics 全屏 MRT pass**：复用 `vsMatResolve` 全屏三角，新 PS 写 `SV_Target0..2`；`GetGBufferRenderTargets` 式按序号挂槽（UE 口径）。需核 NVRHI `MaxSimultaneousRenderTargets` / framebuffer 校验与双后端 `RGBA32_FLOAT` MRT 支持 |
| **绑定布局** | 写 pass 单一 set（同方案 B 的 vis/顶点/簇/CB/sampler）+ 2~3 个 RT slot；读 pass 单一 set（t0..t2 SRV + b0）；**每管线各一个 set**（§2.7），shader 不得跨 set 引用（本项目 slangc 全编进 set 0）；shift 沿用 §2.5 |
| **状态登记（§2.15 / §2.8）** | 2~3 张纹理逐张 `beginTrackingTextureState`，每帧 RT↔SRV 往返；清屏在 `setGraphicsState` 前；帧内禁第二 cmd list；不设 permanent |
| **显存 / 带宽 / 精度** | 最贵（15.8~23.7 MiB，2~3 写 + N 读）；**收益：全 float32 无量化**，是三方案里最可能满足「`--mode shadow` 无阴影版与 mode 5 逐像素一致」的落盘方案（仍需按 M-PT2/PT3 实测，FMA 路径变化可能引入 1 LSB） |
| **S10 / S12 / S15 扩展面** | 消费端最直观（各属性独立 SRV）；但 **S12 temporal 要 double buffer 整套 MRT** → 显存翻倍到 31.6~47.4 MiB；S15 只读 worldPos/normal 两张即可 |
| **现有基线改动面** | **最大**：新增 2~3 张纹理 + 新 MRT pass + 新 example 卡；若新增模式导致 `kStage` bump，按 s8-done 台账 = **约 50 张 HUD 基线重拍**，且 `reshot-hud-stage.py` 超 527px 签名必须追因才许 `--force` |

---

## 5. 横向矩阵

| | A 不落盘 | B packed uint4 | C 精简 MRT |
|---|---|---|---|
| 新增纹理 | 0 | 1（+depth） | 2~3 |
| 显存 @960×540 | 0 MiB | 7.9~9.9 MiB | 15.8~23.7 MiB |
| 写入 pass | compute 8×8 | 全屏 graphics（1 uint RT） | 全屏 graphics MRT |
| 精度 | float32 无量化 | **有量化** | float32 无量化 |
| mode 5 逐像素一致 | **天然成立** | 需保留旁路，否则破 | 大概率成立（需实测） |
| 现有基线改动 | **0** | 0（旁路）/ 全 FAIL（切换） | 大（可能 50 张 HUD 重拍） |
| S12 temporal | 到时必补落盘 | 现成 `previous_gbuffer` 范例 | MRT 双份，显存翻倍 |
| S10 邻域采样 | 重复 resolve ×N | 一次 load | 一次 load |
| 三处依据 | bevy meshlet / UE Lumen 解耦 | bevy solari / Carrot EncodedGBuffer | Carrot MRT / UE GBufferA~E |

---

## 6. 建议（**建议，不是结论，不替用户拍板**）

倾向 **方案 A**：

1. 三处证据里 **UE Lumen 直光明确不读屏幕 GBuffer**、**bevy meshlet 材质 pass 即取即算**，与本项目 `material.slang` 现状同构；「阴影射线必须先有屏幕 GBuffer」不是工业终态的必然前提。
2. M9.0 硬验收（S7 / pbr-direct / s8.2 双后端 `bytes` 不变；`--mode shadow` 与 mode 5 逐像素一致；反解数学逐字搬移）在 A 下零风险；B 的量化会直接破坏「逐像素一致」，C 需额外实测且有 1 LSB 风险。
3. 落盘的**真正需求方是 S12 的历史帧属性**（bevy 专门留了 `previous_gbuffer`）。现在拍格式等于替两个还没开工的里程碑定死资源契约；公共头已把「数学契约」抽出，将来选 B 或 C 都能复用 `gbInterpolateVisTriangle` / `gbReconstructWorldPos`，返工面被压到写入端一层。
4. 代价要说清：**A 是范围收缩**（M9.0 交付从「公共头 + 落盘」缩为「公共头」），且 S10/S12 时落盘决策会回来；若用户要求 M9.0 就必须有可见的 GBuffer 资源，则 **C 比 B 更贴合硬验收**（无量化），但要接受 ~50 张 HUD 基线重拍的代价。

**方案 B 的显式警示**：24bit 八面体法线 + 8bit 通道 + RGB9E5 是 GI 降噪场景的合理取舍，但本项目 M9.0 的判据是「与 mode 5 逐像素一致」——把有损 packed 放进这条对照链会自相矛盾。

---

## 7. 拍板后动作清单（2026-11-10 已拍板 A，状态已同步）

1. **已执行**：结论写回 `docs/s9/S9-PLAN.md` 开工拍板记录 Q4 行（「已拍板：方案 A 不落盘，即取即算 + 依据」）。
2. **已执行**：M9.0 任务书按「任务 / 决策 / 实测 / 边界」四层重写为 T1~T8；删「落盘形态待用户拍板」挂账句与上一版编造的绑定槽位表，硬验收三条原样保留。
3. **已执行**：本文件状态改为「已拍板，保留为决策证据」；`MILESTONES.md` 四栏「下一步」、`MEMORY.md` 进度行、当日日志的旧表述清理。
4. **待办**：把方案 A 的绑定布局 / 状态登记口径固化进 `docs/SHADER-CONVENTIONS.md`（新增 §2.18）。
5. **待办**：与 A 相容的第一行落码（T4：`--mode shadow` 无阴影版 compute 直光路径，用户拍板 ②）；**不新增独立 GBuffer 纹理 / RT / UAV**，compute pass 自身单一 BindingSet 按 §2.7 建并固化 §2.18。`kStage` 是否 bump 随实现定，bump 则按 `reshot-hud-stage.py` 安全闸重拍。
6. **待办**：M9.0 收口照 `AGENTS.md` 交付纪律 —— 四项自查、四栏台账、修改文件清单、`wt_pool.py close` 硬闸、tag `s9-gbuffer`。
