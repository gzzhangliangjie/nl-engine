# S5 · 与 UE5 官方做法差异台账（UE-DIFFS）

> 用途：集中记录本项目与 UE5 Nanite 官方实现**不一致**的地方，供 review 一次性过。
> 纪律来源：AGENTS.md「任何偏离官方口径的地方必须解释原因并留痕，不许静默偏离」——本文档即留痕的集中落点。
> 维护约定：**新差异随手追加一条；每个里程碑收口 / review 前过一遍本文档**，确认「升级路径 / 触发条件」仍然成立。
> 定性分三档：**有意简化**（故意选更简单的做法，留升级路径）/ **未实现**（终态要做，当前缺）/ **计划内待办**（已排期）。

## 算法差异

### D1 误差投影口径：zeux 一维公式 vs UE 切线投影（M5.0，有意简化）

| | 内容 |
|---|---|
| UE 做法 | `NaniteCullingCommon.ush:112` `GetProjectedEdgeScales`：对包围球做**精确切线投影**，返回投影误差 min/max 区间（float2）；显式分支处理球跨近断面（`DistToTSq < 0`）、相机入球（`bVisibleAngleSpansZero`）等退化情形；阈值带 `LODScale × UniformScale`。调用点 `NaniteClusterCulling.usf:255` `ShouldVisitChild`。 |
| 我们现状 | `core/src/lod_error.cpp` `screenErrorPixels()`：`screenPx = err / max(\|eye−C\|−R, near) × projYScale × 0.5 × H`（bevy `cull_shared.wesl:13`，溯源 zeux meshoptimizer `demo/nanite.cpp:115`）。 |
| 为什么 | ① 与 S3 error chain 同一权威源（bevy 自己注明抄 zeux），全链一个口径；② 一维标量，M5.1 compute 遍历与单测都简单；③ 2026-11-08 用户拍板。 |
| 已知偏差 | 偏轴簇用欧氏距离低估视平面 Z → 屏幕边缘可能选层偏粗（多画细节，不破画面）；球跨近断面只 clamp 不做切线几何。 |
| 升级路径 | `screenErrorPixels()` 是纯函数，换成 UE 切线投影只动它 + 重推 M5.0 单测手算值；调用方（viewer HUD / compute 遍历）不动。 |
| 触发条件 | M5.1/M5.2 在偏轴或进模场景实测选层明显偏粗时升级。 |

### D2 cut 语义：二值切层 vs UE 连续 LOD 混合（M5.0 起，有意简化）

| | 内容 |
|---|---|
| UE 做法 | `NaniteRasterizer.usf:2097` `ComputeClusterLODTransition`：父子簇之间算 0..1 过渡因子，连续混合。 |
| 我们现状 | cut 二值：`screenPx < cutThresholdPx` 即在此层停（S5 组原子 cut）。 |
| 为什么 | 组原子 cut 数据前提（父簇绝不跨组）只支撑整组离散切换；连续混合需要 per-pixel 过渡预算，复杂度整档提升。 |
| 缓解 | ~~S5.2 滞回（hysteresis）防抖~~（2026-11-08 拍板砍掉：三处参考均无运行时滞回——UE `NaniteClusterCulling.usf` 无状态双误差带 + TAA 掩盖、bevy/Carrot 纯无状态判定；滞回需 per-cluster 上帧状态，破坏 M5.1 刚验证的无状态性，且当前规模未实测到抖动危害。与 UE 的无状态判定口径**对齐**而非差异）。 |
| 升级路径 | 若视觉上切层跳变不可接受，再立项连续过渡（参考 UE 的 dithered transition）。 |

### D3a GPU 遍历结构：每线程显式栈 vs bevy 全局队列多 pass（M5.1，有意简化）

| | 内容 |
|---|---|
| bevy 做法 | `cull_bvh.wesl`：BVH 8 叉层次，**全局共享队列** + 多 pass（early/late 两阶段 + HZB second pass），线程从队列取任务，跨线程负载均衡；cut = `parent_is_imperceptible \|\| !aabb_in_frustum` 停止下钻。 |
| 我们现状 | `dag.slang`：每根簇线程持显式栈 `uint[512]`，单 pass 串行下钻；组展开用 `InterlockedExchange` 声标志去重（同组多个同误差父簇全部判失败时只让一个赢家压成员，防指数放大）。 |
| 为什么 | S5-PLAN 定死的简单版（zeux CPU demo 的 GPU 平移）；单模型 ≤ 数百组，栈深实测 ≤ 3 层，512 上限绰绰有余，全局队列的负载均衡收益为负（同步开销 > 收益）。 |
| 已知偏差 | 线程间工作量不均（近景根线程下钻深、远景瞬间 leaf emit）；无 early/late 两阶段，未来加遮蔽剔除（D6）需 second pass 时再评估。 |
| 升级路径 | 多实例 / 千组级资产出现负载不均时，换 bevy 式全局队列 + 多 pass（输出接口不变，只动遍历内部）。**M6.1 交汇记录（2026-11-09）**：Hi-Z second pass（本帧 HZB 复测被遮簇）长在队列多 pass 结构上，用户拍板本轮不做（单 dispatch 内联方案，见 D6），D3a 升级时一并评估。 |

### D3 DAG 遍历结构：单级组原子 cut vs UE 两级 hierarchical（S5 设计，有意简化）

| | 内容 |
|---|---|
| UE 做法 | 两级：mesh 级预剔除（`NaniteCulling.cpp` 出候选实例）→ cluster 级 hierarchical BVH/swizzle 遍历 + 两阶段 raster 队列（cull pass 产出、raster pass 消费）。 |
| 我们现状 | 单级：32B 组记录（error/sphere[4]）flat 列表 + 组原子 cut，遍历输出复用 S4 indirect buffer。 |
| 为什么 | 资产规模当前单模型（≤1.3M tri），组数 ≤ 数百，两级层次收益未显性；Carrot 的 flat 双阈值模型（`draw_virtual_geometry.slang:81`）证明 flat 也能出正确画面。 |
| 升级路径 | 资产规模上到多实例 / 千组级时引入 mesh 级预剔除；hierarchical 遍历与 P9 流式分页（V4）同期评估。 |

### D4 简化器：meshopt 多 pass vs UE 自研 collapse（M2.3a，有意简化）

| | 内容 |
|---|---|
| UE 做法 | `NaniteBuilder.cpp` 自研 collapse 简化器：自维护边界锁、误差随折叠同步重算、朝向不变量内建。 |
| 我们现状 | meshopt 多 pass 主循环（src 并入 dst + collapse_locked + cos75°）+ link condition + 双层边界锁（组间边界 + 模型开放边界）+ 绝对朝向锚（`check-orient` 扫全链验收）。 |
| 为什么 | M2.3a 判决实验：meshopt 质量达标且省一整套自研简化器；朝向缺口用外挂锚补齐（`check-orient` exit 1 = FAIL 兜底）。 |
| 已知偏差 | 误差链定义与 UE 不逐位相同（我们叶子误差 = 簇内最长边，同 zeux 语义；UE `Cluster.Error` 自有定义）。 |
| 升级路径 | 无主动升级计划；若 meshopt 在某类资产上质量不达标再议。 |

### D5 无 per-instance 误差缩放旋钮（M5.0 起，未实现）

| | 内容 |
|---|---|
| UE 做法 | `LODScale × UniformScale` 全局/实例级质量旋钮，支持非均匀缩放的误差修正。 |
| 我们现状 | `SceneConstants` 无误差缩放字段；model 恒单位阵（M1.0 起节点变换烘进顶点）。 |
| 升级路径 | 多实例（S8+）落地时加：CB 加 `errorScale` 标量（均匀）即可覆盖 90% 场景；非均匀需回到 D1 的 UE 口径。 |

### D6 剔除维度：仅视锥 vs UE 视锥+遮蔽（M4.1 建档；**M6.1 遮蔽已实现，结构差异留痕**）

| | 内容 |
|---|---|
| UE 做法 | 视锥剔除之外还有 hierarchical-Z 遮蔽剔除、**三 pass culling**（`NaniteCullRaster.cpp` CULLING_PASS_NO_OCCLUSION / OCCLUSION_MAIN〔用上帧 HZB〕/ OCCLUSION_POST〔深度 prepass 后用本帧 HZB 复测被遮候选〕）、two-pass raster（RasterHomogeneous）、cluster swizzle 优化访存。HZB 测试口径 = `NaniteHZBCull.ush IsVisibleHZB`：簇最近点（Rect.Depth）≥ 区域最远表面（inverted-Z min）→ 可见。 |
| 我们现状（M6.1） | **Hi-Z 遮蔽剔除已实现**：dag.slang 单 dispatch 内联 hizOccluded（Mara&Morgan 球投影 + bevy mip 选择/4×4 采样 + 簇最近径向距离 > 区域 hizMax 整树剔除），金字塔 max 口径与 UE/bevy 一致（hiz.slang 文件头）。**差异 = 结构**：单 pass + 上一帧 HZB（一帧延迟），无 UE 的 OCCLUSION_POST 本帧复测、无 two-pass raster、无 swizzle。 |
| 为什么 | S6-PLAN M6.1 原文方案（2026-11-09 用户拍板）：单模型 ≤ 数百组，本帧复测需先建 D3a 队列多 pass 结构，收益（减少转动假遮挡过画）当前规模不显性。 |
| 已知偏差 | **度量空间**：金字塔与簇深度均用**视点径向距离** `distance(worldPos, eye)`（M6.0 定，与 M5.0 误差公式 |eye−C|−R 同空间），UE 用**设备深度**（inverted-Z，IsVisibleHZB 的 Rect.Depth）；max vs min 方向翻转是同一保守语义（簇最近 vs 区域最远）在两套度量下的等价换算，不是分歧；一帧延迟本身与 UE OCCLUSION_MAIN 同口径（UE 多的是 OCCLUSION_POST 本帧复测，已列升级路径）。**开关实现（M6.2）**：UE = 编译期 shader permutation（`FOcclusionCullingDim` OCCLUSION_CULLING bool）+ `r.Nanite.Culling.HZB` CVar 运行时选 permutation；我们 = CB 标量（g_dagHiz.x）shader 内分支——compute 单分支开销可忽略，且 ImGui 切换免重编/重选 shader；统计两侧同思路（UE FNaniteStats 单结构原子累加 / bevy 每 pass 分立 atomic counter / 我们 u3 计数器槽），UE 统计口径含 CandidateClusters（候选）与我们的 visited（emit+occluded）语义对齐。 |
| 升级路径 | bevy 式 HZB second pass（被遮簇压 second-pass 队列、prepass 后本帧 HZB 复测）= 与 D3a 队列多 pass 绑定，触发条件同 D3a（多实例/千组级负载不均 + 转动假剔除实测明显）；swizzle 视 profiler 证据再定。度量空间换设备深度需重推 hiz.slang/dag.slang 全部手算锚 + 重拍 s6-hiz/s6-cull 基线，无实测危害不主动做。 |

### D7 无软件光栅化兜底（未实现）

| | 内容 |
|---|---|
| UE 做法 | Nanite 全屏 pass 内置软件光栅（S 路径），处理硬件光栅漏掉的微小簇。 |
| 我们现状 | 无。微小簇漏画时无兜底（当前资产规模未见可见 artifacts）。 |
| 升级路径 | 若 M5.x+ 大规模资产出现微小簇闪烁/漏画，评估 S 级软件光栅（工作量大，需单独立项）。 |

### D-E1b-1 ECS 场景点光源的口径差异（M-E1b.2；轨道局部编号，避免与主线 D10~D15 撞号，收官 merge 时按主线序列重排）

| | 内容 |
|---|---|
| UE 做法 | `ULocalLightComponent` / `UPointLightComponent`：`bUseInverseSquaredFalloff` 默认开 + `AttenuationRadius` 夹紧 + 可选 `LightFalloffExponent` 非物理路径（8.0）；Intensity 单位 lumens/candela；`SourceRadius` 影响高光与软阴影；光照经 Cook-Torrance BRDF、clustered / deferred 灯池，另有阴影 / IES / 光照函数。 |
| 我们现状 | M-E1b.2 点光 = `type=1` + 世界 `position` + `range` 的纯数据组件；**衰减公式逐条照抄** `DeferredLightingCommon.ush:480-486`（含 `1/(d²+1)` 与半径窗）。**M-E1b.3 起（2026-11-09）响应已换代 Cook-Torrance**（与 shadePbr 同式，逐实例 roughness/metallic，视向量 V 从 b1 cameraPos 取，ambient 乘 `cDiff`），Lambert 响应成为历史；仍无 `SourceRadius`、无非物理 FalloffExponent 路径、无阴影/IES；灯强是相对权重而非 lumens，方向灯另按 `1/ΣI` 归一化（UE 无此归一化）；t6 单 StructuredBuffer 顺序遍历，无 clustered culling。Carrot `GPULight` 的 `constant/linear/quadratic` 三项衰减路径也没做（三处参考里的第三条差异）。 |
| 升级路径 | E1 轨道已收官 merge 回 master（pbr-direct 的 Cook-Torrance + t1 MR 贴图已在 master）：**点光已接进 PBR BRDF（M-E1b.3，2026-11-09）**；灯强换 lumens 口径、clustered forward 灯池与**阴影按 E1b.4 / S8 预研另立项**（用户拍板 A 拆分）；轨道局部编号 `D-E1b-1` 保留以示来源（与主线 D10~D15 不连续）。 |

### D-E1b-2 方向灯阴影 pass 的载体与质量口径（M-E1b.4 建档，2026-11-09；有意简化）

| | 内容 |
|---|---|
| UE 做法 | `CachedShadowMapData` / `FShadowMapInteraction` 全家桶：方向灯单 cascade 走 `ShadowDepthRendering`（depth-only，真 depth buffer + `SV_Depth`），接收侧 `GetShadowMapWeightedPCFSamples`（4-tap DPCM/PCF + 屏幕空间 bias + `ShadowBias`/`ShadowSlopeBias` 分离）；资源是 shadow atlas（多灯打包一张）而非 per-light 纹理。 |
| Carrot 做法 | `ShadowMapPass`：每阴影灯一张独立 depth target（无 atlas），线性 depth 编码进 R32F 颜色图（与我们同款「颜色目标当深度图」思路，但靠 RenderGraph 管依赖）。 |
| bevy 做法 | cascade shadow map（最多 4 cascade）+ 真深度缓冲 + 硬件 sampler 比较（`CompareFunction::LessEqual` 的 sampler2DShadow）；接收侧 `sample_directional_light_shadow` 硬件 PCF。 |
| 我们现状（M-E1b.4） | 每阴影灯一层 `Texture2DArray R32F 1024×1024`（**非 atlas、非真 depth buffer**）：阴影 pass 无 depth attachment，nearest 深度靠 **R32F MIN 混合**（顺序无关，语义等价 LessEqual）；bias = CPU 常量 0.003 + shader 坡度项 `0.035×(1−NdotL)`（无 UE 的 bias/slope-bias 分离）；接收侧 3×3 整数 texel Load PCF（**非硬件比较 sampler**——vendored NVRHI 的 SRV 用 `Texture_SRV`，比较采样器另需 Format/SamplerDesc 比较位，本次未开）；正交投影取场景 AABB 单 box（无 cascade 分层）。 |
| 定性 | **有意简化**：单灯单 cascade + MIN 混合已满足「方向灯阴影肉眼正确 + 双后端 near 档」的验收判据；MIN 混合与真深度缓冲在本场景（凸物体 + 闭合网格）结果等价。 |
| 升级路径 | 真深度缓冲（加 depth attachment + polygon offset）或硬件比较采样器 → S8 Lumen 预研线；cascade / atlas 打包 → 场景规模上去之后（P9 V 真实资产阶段）；点光 cube shadow 未立项。 |

### D10 物理求解器：UE 自研 Chaos vs 我们 vendored Jolt（M-P0 建档，2026-11-09；有意简化）

| | 内容 |
|---|---|
| UE 做法 | 官方物理=自研 **Chaos** 求解器（`Engine/Source/Runtime/Experimental/Chaos/`、`ChaosCore/`、`ChaosSolverEngine/`、`ChaosSpatialPartitions/`、`Runtime/PhysicsCore/`、`Engine/Classes/PhysicsEngine/` 的 `BodyInstance`/`BodySetup`/`ConstraintInstance`）；引擎侧耦合面是 Scene/Actor + `FBodyInstance`，求解、宽相、约束、CCD 全在 Chaos 内。 |
| 我们现状（M-P0） | 物理走 A 类并行轨道 `feat/physics`：**vendored Jolt Physics v5.3.1（MIT）**，自写集成层（世界生命周期 / 体封装 / 碰撞层 / debug 渲染），求解器一行不自研。UE 5.8.4 源码树中**不存在 Jolt 后端选项**——立项时 `P8-PHYS-PLAN.md` / `NON-RENDER-ROADMAP.md` 里「UE 5.x 官方另提供 Jolt 后端」系误记，本条一并勘误。**2026-11-09 二次拍板**：结论「不自研求解器」不变，但依据改为**成本取舍**（自研成本过高、物理为 A 类配角），不再是「UE 口径 / 业界共识」——UE 自研 Chaos 恰恰是自己写求解器，不能反向论证（旧表述自相矛盾，已修正）。同日候选评审确认**继续用 Jolt 5.3.1**：本机唯一零联网可得的 C++ 求解器源码、MIT 与项目一致、纯 CPU 零图形 SDK、C++17 只依赖 STL、确定性已实测可锁、自带 DebugRenderer/Character、Carrot 2902 行集成层可作本地参照；UE 口径不参与选型。 |
| 定性 | **有意简化**（引擎化阶段）：本项目目标是 Nanite×Lumen 渲染研究，物理只要能验收「物体按物理规律动」，不追 Chaos 的工业完备度（网络物理回滚、ChaosVehicles、GeometryCollection 破碎、CCD 全链路）。 |
| 升级路径 | 需要 Chaos 级能力（破坏、载具、确定性网络回滚）时单独立项评估，不把 Jolt 换掉当默认；vendored Jolt 版本锁死 v5.3.1，**升级单独走一轮 M-P 验收**（确定性轨迹逐点比对 + 展示台卡全绿）。 |

### D11 VSM：页请求/跨帧缓存排在 M15.1，M15.0 为静态整页分配（S15 预研，有意简化）

| | 内容 |
|---|---|
| UE 做法 | `VirtualShadowMapPageMarking.usf` 从 GBuffer 深度 / 前层半透明反投影标页 → `VirtualShadowMapArray.cpp::BuildPageAllocation` 分配（含 `CacheManager` 跨帧静态/动态页缓存与 GPU invalidation）→ 每页 GPU culling → Nanite 多视图 depth-only 直写物理页 atlas → 页级 HZB → `VirtualShadowMapProjection.cpp` 投影。**依赖链起点是「标页」，不是「页池」**。 |
| 我们现状 | `docs/s15/S15-PLAN.md` M15.0 先做「物理页池 + 静态整页分配 + 投影采样」，M15.1 才补页请求/分配/回收，M15.2 才接簇级间接阴影——顺序与 UE 依赖链**相反**；M15.0 的「VSM」实为「带页表的静态 shadow map」。 |
| 定性 | **有意简化**（先出画再补复杂度，2026-11-09 预研后用户拍板保留节奏不返工）。附带修正：原底线「绝不碰 GBuffer 主路径」与 UE 标页吃 GBuffer 冲突，已改为「不改主路径输出格式，只读其 depth/GBuffer」。**2026-11-10 Q4 方案 A 口径注记**：此处 GBuffer 是 UE 侧概念；本项目 S9 不落屏幕 GBuffer 纹理，M15.1 落码时按 `gbuffer_common.slangh` 公共反解契约消费 vis buffer / depth 属性，别照字面去找一张属性纹理。 |
| 升级路径 | M15.1 按 UE 链补齐标页 → 分配 → 缓存失效；M15.2 参照 bevy `meshlet/visibility_buffer_raster_node.rs` 的 ShadowView 变体（`R32Uint` + 两阶段 HZB）把簇几何接进阴影路径，不引入 bevy 的每帧全量重绘语义。 |
| 触发条件 | 验收**不得按 UE 终态直接对标 M15.0**；若大场景近处阴影分辨率不足、或相机移动时页抖动明显，把标页与缓存提前到 M15.1 之前。 |

### D12 TSR：缺 jitter/分辨率框架、离屏 post-chain 前置、卷积上采样核对（S16 预研，计划内待办）

| | 内容 |
|---|---|
| UE 做法 | `PostProcess/TemporalSuperResolution.cpp` + `HaltonUtilities.h`：`TemporalJitterPixels`（Halton）+ `PrimaryScreenPercentageMethod=TemporalUpscale` 决定渲染→输出分辨率分离；10+ pass（dilate velocity → reject shading → update history → resolve，Mitchell-Netravali / Nyquist + jacobian reprojection field）；位于 SeparateTranslucency/DOF 之后、Tonemap 之前；history 五套 array + pre-exposure / camera cut。 |
| 我们现状（S6） | 原 `S16-PLAN.md` 全文未提 jitter / 渲染-输出分辨率分离；「已具备 HDR scene color（tonemap 前）」在当前代码**不成立**——swapchain framebuffer 直出 + `model.slang:216` 输出端 gamma 编码（注释明写「不算 tonemap」），无离屏 HDR RT / tonemap pass / post-chain / velocity GBuffer；原 M16.2 用 5×5 联合双边当上采样核（那是 S12 GI 降噪滤波器），三处参考无一采用。 |
| 定性 | **计划内待办已落地（2026-11-11 T0 拍板）**：jitter + 双帧矩阵 + 渲染/输出分辨率分离并入 M16.0；离屏 HDR RT + post-chain 已由 **T0 里程碑**单列为 SSR / S16 / bloom **共享前置**（见 `docs/t0/T0-PLAN.md`，`docs/s6/SSR-RESEARCH-NOTES.md` §3/§4 指出的同一缺口，不重复建设）；M16.2 改 5-tap Catmull-Rom 对齐 UE/bevy（tag 随之 `s16-bilateral`→`s16-upsample`）；S12/S16 共享的 velocity GBuffer 由 T0 落地。 |
| 升级路径 | 卷积网络 shading rejection / parallax reprojection field / Nyquist 2× history / thin geometry 与 `r.Nanite.Foliage` 联动——质量层，S16 首版不做。 |
| 触发条件 | 首版出现动目标 ghost / disocclusion smear / 亚像素细几何闪烁时，按 UE `TSRRejectShading.usf` + `TSRReprojectionField.ush` 补齐，**不要靠调 α 掩盖**。 |

### D13 碰撞过滤：UE 三态响应容器 vs 我们对称 bool 矩阵（M-P1 建档，2026-11-09；有意简化）

| | 内容 |
|---|---|
| UE 做法 | `Engine/Classes/Engine/EngineTypes.h` 的 `FCollisionResponseContainer` 对每个通道给 `ECollisionResponse`（Ignore / Overlap / Block **三态**）；`Engine/Classes/Engine/CollisionProfile.h` 的碰撞配置用 `ObjectType` + `ResponseToChannels` 组合，查询与模拟可配不同响应；通道是引擎级可命名、可在 Project Settings 编辑的资源。 |
| 我们现状（M-P1） | `nl::physics::LayerMatrix` = 4 个固定层、对称 bool 掩码（可碰撞 / 不可碰撞），同时喂给 Jolt `ObjectLayerPairFilter` 与 `ObjectVsBroadPhaseLayerFilter`（宽相层 1:1 映射）。没有 Overlap-only、没有查询/模拟分响应、没有运行时命名通道。 |
| 定性 | **有意简化**（A 类并行轨道）：M-P1 的目标是把层过滤与「引擎对象 ↔ Jolt 体」双向关联跑通，两条轨迹（停在挡板 / 穿透挡板）已足以自证；Overlap 事件与命名通道属编辑器 / 场景集成阶段（M-P5）的话题。 |
| 升级路径 | M-P5 场景集成（P8 E3 序列化 + Inspector）需要触发器 / 重叠事件时，把 bool 矩阵扩成三态并接 Jolt `ContactListener`；层数量与通道命名届时才定，避免现在定死。 |

### D14 休眠口径：UE Chaos IslandManager 整岛休眠 vs 我们 Jolt `mAllowSleeping` 开关（M-P2 建档，2026-11-09；有意简化）

| | 内容 |
|---|---|
| UE 做法 | `Runtime/Experimental/Chaos/Public/Chaos/Defines.h` 定义 `SleepingLinearThreshold` / `SleepingAngularThreshold` / `SleepCounterThreshold`；`Chaos/Public/Chaos/Island/IslandManager.h` 持有 `SleepLinearThresholdSq` / `SleepAngularThresholdSq` / `SleepCounterThreshold`，由 IslandManager 按岛判定并**整岛休眠**；`ParticleHandle.h` 还有逐体 `SleepThresholdMultiplier` 缩放阈值。休眠是求解器演化层的一部分，不由使用者按体开关。 |
| 我们现状（M-P2） | `nl::physics::StackConfig::allowSleeping` 直接写 `JPH::PhysicsSettings::mAllowSleeping` 与 `BodyCreationSettings::mAllowSleeping`，是全局 / 逐体 bool 开关；`mTimeBeforeSleep=0.5s`、`mPointVelocitySleepThreshold=0.03` 用上游默认未暴露。只读 `PhysicsSystem::GetNumActiveBodies(EBodyType::RigidBody)` 观测，不改求解器。 |
| 定性 | **有意简化**（A 类并行轨道）：M-P2 目标是长时间步进不抖 + 可验收的休眠观测。三用例已区分「摩擦与接触求解器是稳定来源」与「休眠只省算力」（baseline 活跃体 0 / no_sleep 活跃体 6，两例尾段极差均 < 0.00005）；未实现逐岛阈值、逐体倍率、唤醒事件。 |
| 升级路径 | M-P5 场景集成需要大量休眠体 / 唤醒策略时再暴露阈值并接 `ContactListener`；在此之前保持上游默认，避免为调参引入非确定性。 |

### D15 vis buffer 载体：UE/bevy 的 R64_UINT image atomic vs 我们 u64 buffer atomic（M7.0 建档，2026-11-09；计划内偏离）

| | 内容 |
|---|---|
| UE 做法 | `NaniteWritePixel.ush:20-35` 对 R64_UINT 纹理做 `ImageInterlockedMaxUInt64`（reversed-Z 下 max=最近者胜；R64 不支持时退 R32G32_UINT 两次 32 位原子）；bevy `visibility_buffer_hardware_raster.wesl:59-65` 的 `textureAtomicMax` 同为 image 64 位原子；Carrot `VisibilityBuffer.cpp` 用 R64Uint 存储图作 RenderGraph storage target。 |
| 我们现状（M7.0） | `RWStructuredBuffer<uint64_t>` 线性 buffer（宽×高）+ `InterlockedMin`，打包格式/原子方向/clear 口径**全部保持 Option A**；`visIndex(pixel, width) = y*width+x` 是二维压一维的唯一入口。vendored NVRHI 全树零 R64 格式（`Format` 枚举、`dxgi-format.cpp`、`vulkan-constants.cpp` 三处实查），R64_UINT 纹理根本创建不出来。 |
| 定性 | **计划内偏离**（2026-11-09 开工前实测撞墙 → 用户拍板方案 1「改自己的代码，不改第三方 NVRHI」）。逻辑上仍是「每像素一个 u64」；M7.2 的逐像素一致判据只看反解内容，与载体无关。没有为「介质与 UE 同名」去改 vendored 库而破坏「锁版本、只裁剪不改写」的升级纪律。 |
| 能力差异 | UE 用 image 64 位原子（`shaderImageInt64Atomics` / `AtomicInt64OnTypedResourceSupported`）；我们用 **buffer** 64 位原子（`shaderBufferInt64Atomics` / `AtomicInt64OnDescriptorHeapResourceSupported`，后者在 D3D12 Options11 而非 Options9）。原子方向相反是深度约定所致：UE reversed-Z → max，我们标准 Z → min，等价。 |
| 升级路径 | 若 NVRHI 上游自带 R64_UINT，或 S13 真上 shading binning / 软光栅需要纹理局部性，再把介质换回纹理；`visbuf_common.slangh` 的 `visIndex()` 与读写点集中，改动面可控。 |
| 触发条件 | 出现 vis buffer 带宽瓶颈，或 NVRHI 升级后已具备 R64 格式且硬件 `shaderImageInt64Atomics` 实测可用。 |

### D16 材质反解的导数坐标系：bevy `ddy *= -1.0` 翻转不带过来（M7.1 建档，2026-11-09；有意差异——宿主约定不同）

| | 内容 |
|---|---|
| bevy 做法 | `compute_partial_derivatives`（visibility_buffer 材质 resolve）在算出屏幕空间 uv 导数后做 `ddy *= -1.0`——因为 wgpu/NDC 约定下其屏幕空间重建所用坐标系与纹理 V 方向相反，翻转后才能喂 `textureSampleGrad`。 |
| 我们现状（M7.1） | **不翻转**。NVRHI 的 `SV_Position.xy` 已是像素坐标系（原点左上、y-down），与光栅 pass 输出的 viewport 变换（y 翻转）后的屏幕空间 barycentrics 同一直角系，`fragPos.xy` 直接进解析式，`SampleGrad(g_sampler, uvF, duv_dx, duv_dy)` 原样喂。验证：贴图纹理无镜像/倒置，与 M0.4 传统路径前景 99.71% 像素差 ≤2。 |
| 定性 | **有意差异**（宿主坐标系约定不同，等价实现各取所需；照抄 bevy 的翻转反而上下颠倒）。 |
| 关联坑 | 全屏 pass 内隐式 `ddx/ddy(uv)` 不可用（全屏三角形无网格邻接语义）——解析导数 + `SampleGrad` 是唯一正确路径，bevy/Carrot/The-Forge 三方口径一致。 |
| 触发条件 | 无（S9/S10/S13 复用 `visbuf_common.slangh` + 本条约定即可）。 |
### D17 音频距离衰减：超 maxDistance 不自动静音（P8 M-S1 建档，2026-11-09；有意简化 + 口径补强）

| | 内容 |
|---|---|
| UE 做法 | `Engine/Classes/Components/AudioComponent.h:815` 的 `IsInAudibleRange()`：距离 > `MaxDistance` 时声源判定为不在可听范围、直接不参与混音（增益 0）；衰减模型只作用于 `[MinDistance, MaxDistance]` 区间，`Attenuation.h:16-24` 的 `EAttenuationDistanceModel` 有 6 种（Linear/Logarithmic/NaturalSound/Custom/InternalOnly 等），且支持 4 种 shape + LPF。 |
| 我们现状（M-S1） | 落码依据 = miniaudio 原生 `ma_attenuation_model` **4 种**（none/inverse/linear/exponential，inverse 等价 OpenAL `INVERSE_DISTANCE_CLAMPED`，与 Carrot 底座同源）；公式对距离做 `clamp(d, min, max)`，**超 maxDistance 后仍给极小正值、不会自动静音**。故 `nl/audio` 薄层按 UE `IsInAudibleRange` 口径补剔除：距离 > maxDistance 时增益强制 0（静音），证据图 `z=20` 静音竖线可见、`selftest` 用例 `beyond maxDistance is silent` 守住。 |
| 定性 | **有意简化 + 口径补强**：miniaudio 只提供 4 种模型（无 UE 的 NaturalSound/Custom/shape/LPF），属引擎能力边界不可改；但「超距静音」是听感正确性而非引擎特性，由 `nl/audio` 显式补上，避免远处声源残留极小底噪。6 种 UE 模型与 shape/LPF 不在本轮范围。 |
| 升级路径 | M-S2 游戏内调试若需可视化衰减半径、或 M-S3 场景集成需按 UE `MaxDistance` 语义做剔除/LOD 式静音，本处已是统一入口；如需 NaturalSound/Custom 曲线，届时在 `nl/audio` 薄层内按 4 种 model 的扩展点接入，不碰 miniaudio 内部。 |
| 触发条件 | 验收**不得**按 UE 终态 6 模型对标 M-S1；若发现超距声源残留底噪或衰减曲线形状与 UE 某模型不一致且影响听感，回到本处补强。 |
### D18 材质归属编码：UE 簇内 MaterialTableOffset/Length vs 我们 NSUB 子网格序号（M7.2 建档，2026-11-10；等价实现——同一结论的不同编码）

| | 内容 |
|---|---|
| UE 做法 | 材质归属**构建期随簇记录**：每个 meshlet cluster 头带 `MaterialTableOffset` / `MaterialTableLength`（`NaniteDataDecode.ush:116-117`，渲染期 `NaniteShading` 用 `DecodeClusters` 解出后按像素 triId 落到 MaterialTable 区间查材质实例）。核心结论：**材质归属不能渲染期猜**，必须构建期可得。 |
| 我们现状（M7.2） | 同一结论的更轻编码：`.nmesh` 'NSUB' 尾段（v3 只加不改）按**子网格**存簇区间（clusterFirst/clusterCount 8B/项），加载期展开成逐簇 materialId 数组填进 `clusterMeta[3]`，渲染期一次查表。材质 id = 子网格序号（每子网格一个材质，与本引擎「一个 prim 一个材质」的资产口径对齐）。UE 的 MaterialTableOffset/Length 支持簇内多材质段（更细粒度），我们单材质/子网格即可覆盖 S7~S13 需求。 |
| 定性 | **等价实现**（编码粒度差异，核心结论逐字对齐：构建期随簇记录）。无 NSUB 的旧文件恒 materialId=0，cull/hiz/visbuf 光栅零扰动。 |
| 升级路径 | 若未来需要「一个子网格多材质」（per-tri 材质段），再向 UE 的簇内 Offset/Length 编码迁移——NSUB 段按 magic 识别可加段不改旧段（v3 尾段纪律），`kMaxVisMaterials=4` 上限同步扩。 |
| 触发条件 | S9/S10/S13 复用材质间接表时若出现 per-tri 材质需求，回本处评估编码升级；否则维持子网格粒度。 |

### D19 光追阴影几何 = 粗代理，与光栅可见几何轮廓不重合（S9 M9.0 开工建档，2026-11-10；计划内简化，用户拍板接受）

| | 内容 |
|---|---|
| UE 做法 | Nanite 光追用独立 **proxy mesh**（`NaniteStreamOut.h` / `NaniteStreamOut.cpp` 的 stream-out 抽当前可见 LOD 的粗代表），不直接拿屏幕上的最细簇几何；`LumenSceneDirectLightingHardwareRayTracing.cpp:44` 的 cvar 说明 `HardwareRayTracingShadowRayBias` 是为「防 proxy geo 自遮挡」——工业终态也承认代理几何与主帧几何不同源，靠 bias 吸收误差。 |
| 我们现状（S8 / M9.1） | BLAS 直接吃 S3 fallback（`<prefix>.lod0.nmesh` 的 'NFLB' 尾段，Suzanne = 96 顶点 / 104 tri = 最细 3936 tri 的 **2.6%**，对齐 UE `NaniteBuilder` fallback 默认 ~2%），不是随可见 LOD stream-out 的粗代表，比 UE proxy 更粗一档。M8.2 `docs/screenshots/s8.png` 已登记「rt 命中图（粗代理轮廓）与 hiz 栅格基线（最细 LOD）同相机轮廓不重合是设计本意」。M9.1 阴影射线走同一 TLAS，故**影子轮廓按粗代理走**：猴耳 / 眼窝等细模细节的影子与画面对不上，但区域级遮挡判定正确。 |
| 定性 | **计划内简化（有意，非 bug）**。2026-11-10 用户拍板「接受差异并留痕」，不扩范围提前做 S13。三处口径：Carrot `AccelerationStructure.cpp` 按组/簇聚合 BLAS（S13 才有）、bevy `solari` 的 `trace_visibility` 只关心遮挡布尔值、UE 用 proxy + bias 吸收——均不要求光追几何与光栅几何逐三角一致。 |
| 升级路径 | **ROADMAP S13「簇级 BLAS 聚合 + 延迟一帧回读」**：可见簇列表回读 → 拷贝预建 per-cluster BLAS 聚合 → TLAS，光追几何与渲染几何对齐。M9 内不动。 |
| 触发条件 | M9.1 验收**不得**按「阴影边缘与光栅轮廓逐像素吻合」判 FAIL；判据已改写为「在粗代理几何上边缘干净 + 另出一张光栅/代理几何差异可视化图」。若未来出现区域级遮挡错误（不是细节级轮廓差），回本处评估是否提前 S13。 |


### D20 历史拒绝：UE 帧数置信度 + 邻域 clamp（无显式深度/法线拒绝）vs 我们 B1 零落盘退化判据（S12 M12.2 建档，2026-11-13；计划内简化，用户开工闸拍板）

| | 内容 |
|---|---|
| UE 做法 | 只读对照 `Lumen/LumenScreenProbeGatherTemporal.usf:481-527` / `.ush:71-90`：该 pass **没有 explicit 深度/法线差异拒绝**，靠 `NumFramesAccumulated`（4bit 打包，越界/失效即重置）+ `EncodedHistoryScreenCoord` 双线性权重 + `bAnyHistoryValid` + 邻域 clamp 控制历史污染；有效性信息来自探针自身状态而非另存一套 previous GBuffer。 |
| 我们现状（M12.2） | **Q12-B 续拍 = B1「维持不落盘」**（Q4 方案 A 三次顺延结案）。`filter_temporal.slang` 内 `rejectHistory()` 五条：越界 + 重投影 `deviceZ` 差 > thr + 当前帧法线一阶代理（右/下邻域深度梯度外推局部切平面残差）+ 3×3 **深度**邻域 min/max clamp（零资源）+ 帧数置信度（拒绝即 `len=0`）。**零新增 previous normal / previous_gbuffer / RT / CB / 纹理**：`filterLayout_` 仅增 t2 = 已有 `probeDepth_` SRV，`g_flags.w` 复用为「开关 + 阈值」，FilterConstants 仍 32B。 |
| 定性 | **计划内简化（有意，非 bug）**，且是三处口径不一致后由用户拍板取舍的结果：Carrot `lighting/svgf.slang:39-79` 读整套 `previousFrameGBufferInputs`（viewZ 差 / 法线 dot / entityID 三重拒绝）；Carrot `temporal-denoise.compute.glsl:68-84` 只多 1 张 `previousViewPos` + 5×5 min/max clamp；bevy `taa.wesl:142-185` 零 previous 属性、纯 YCoCg 方差 clip + confidence；bevy `solari/gbuffer_utils.wesl:37-43` 需 packed `previous_gbuffer` + `previous_depth` 的 `pixel_dissimilar`。我们取「零属性 + 当前帧退化判据 + 帧数置信度」，与 UE 的无显式属性拒绝同族，额外补了深度/切平面判据。 |
| 升级路径 | 若多物体场景出现可量化的 disocclusion 鬼影 / 光斑残留，**单独拍板**补 previous normal（B2）或完整 previous_gbuffer（B3）；代价 = 新增纹理 + 写入 pass + 双缓冲、Q4 结论改写、全仓勘误 7 处（S9-PLAN / T0-PLAN / SHARED-INFRA-OPTIONS / ROADMAP / SHADER-CONVENTIONS / MILESTONES / 本文件），M12.2 冻结面不动。 |
| 触发条件 | 多物体场景（真实几何边缘、`deviceZ` 多档）复评时若有量化收益再评估。本场景单凸体 + 纯天空、`probeDepth_` 的 `deviceZ` 在 8-bit 下仅 **184~186 三档**，静态 `velocity=0` 时拒绝层结构性不触发、测不出量化收益 —— **不得**据此宣称「历史拒绝已消除主观鬼影」；M12.2 的运动证据只是跟手性代理（`meanMAE(ON,同帧无历史)/meanMAE(OFF,同帧无历史)` = 0.846）。 |

### D22 天气天穹/太阳：UE 全套大气物理（LUT+组件重建）vs 我们解析参数化 + CPU 状态层（WEA M-WEA1 建档，2026-11-14；计划内简化，WEATHER-PLAN Q-WE3/Q-WE5 拍板）

| | 内容 |
|---|---|
| UE 做法 | 只读对照 `Renderer/Private/SkyAtmosphereRendering.h`：`FAtmosphereUniformShaderParameters`（Rayleigh/Mie 散射+吸收、Absorption 双层密度、GroundAlbedo 全套物理参数 uniform，组件改→CPU 侧重建）+ transmittance/multiscattering LUT 组；天气=大气参数随状态重算 + 天空光 cubemap 按状态重烤。太阳光经 `directional_light_transmittance` LUT 采样。 |
| 我们现状（M-WEA1） | 解析参数化天穹（`skyGradientParams`，M10.1 基色 + 夜/暖/浊/云 CPU 调制终值经 CB 下发）+ 太阳方向/强度/色温 RGB（`FrameParams.lightDir`/`weatherSunCol`，每帧由 `nl/core/weather` 状态层 sample）。零 LUT、零新增纹理、零 raymarch（Q-WE3/Q-WE4 拍板随方案 A）。bevy 口径参照：`bevy_solari realtime/atmosphere.wesl` import `bevy_pbr::atmosphere::directional_light_transmittance`（LUT 采样同 UE 族）；`scene/binder/lights.rs` 太阳 = directional light GPU 结构（direction_to_light/illuminance/sun_disk_angular_size），无专门天气层。Carrot Skybox = 静态 cubemap 三选一（反面教材维持原判）。 |
| 定性 | **计划内简化（有意，非 bug）**：参数层先行（WEATHER-PLAN 方案 A），GI 响应靠天气参数挂 Lumen 输入项自动达成；大气 LUT 全套属方案 B 远期（`WEATHER-PHASE2-PLAN.md` 出站条件见 §6）。偏离点：太阳盘形状/大气散射弧/多散射不出（解析近似），天穹为两色渐变 + 调制而非物理积分。 |
| 升级路径 | 方案 B（大气 LUT + froxel 体积雾 + 体积云）在参数层全收口且用户确认「视觉仍不够」才立项；届时 `weather.slangh` 单一事实源替换为 LUT 采样、CB 布局重拍（Q-WE5 闸门复用）。 |
| 确定性口径 | 天气曲线（昼因子 smoothstep / 灰化顺序 / 地平线余光 / 晚霞暖序）已按 2026-11-14 拍板修订并入档 `SHADER-CONVENTIONS §2.22`；CPU（weather.cpp）与 python 孪生（verify.py）两实现同式同源。 |


### D23 高度雾：UE HeightFog 全集（双指数密度/方向 inscattering/StartDistance/cubemap/Lumen 逐射线开关）vs 我们单层解析近似（WEA M-WEA2 建档，2026-11-14；计划内简化，WEATHER-PLAN §9c Q-W2-1/Q-W2-2 拍板）

| | 内容 |
|---|---|
| UE 做法 | `Shaders/Private/HeightFogCommon.ush`：解析指数高度雾**线积分** `CalculateLineIntegralShared`（exp2 形式 + Taylor 展开，双指数双密度参数组）；输出 (inscattering rgb, transmittance a)；inscattering 含方向项（`ComputeInscatteringColor` 太阳方向渐变）+ 可选 cubemap；`OverrideOrigin` 版本供任意射线起点（Lumen 逐射线用，`LumenTracingCommon.ush` + cvar `SampleHeightFogOnGI` 开关）；VolumetricFog 为独立 froxel 体积系统。 |
| 我们现状（M-WEA2） | 单层指数高度雾（`sky.slangh heightFogTransmittance`）：局部密度 ρ(h)=fogDensity·exp(-0.05·max(0,h−fogHeight))（**雾标高以下 clamp 恒定**——UE 地面以下继续指数增，我们防爆防 NaN）；线积分 exp 形式 + Taylor（|a|<1e-4）+ exp 参数 clamp ±20；应用 = UE 口径（直光合成后逐像素 + 探针/cache/refGI miss 逐射线 + sky-bg）。雾色 = 天穹地平线色（C1 落地修正：1×float4 装不下 5 标量）。bevy 口径：`fog.wesl` 四式解析雾 + 雾色吃直光 inscattering（我们雾色取 horizon 已隐含该效果；方向项不做）。Carrot = 屏幕 lerp 雾（反面教材，不采）。 |
| 定性 | **计划内简化（有意，非 bug）**：单层近似覆盖「阴雾天远场变灰白、GI 随之平滑变暗」的任务书验收叙事；无方向性光柱/雾层内渐变（方案 B froxel 远期）。探针 miss 下半球（dirY<0）穿入雾层底部指数增厚，exp 参数 clamp ±20 → T 有界趋 0（与 UE「地下雾无限厚」同口径，视觉 = 雾白）。 |
| 升级路径 | froxel 体积雾 raymarch 属方案 B（WEATHER-PHASE2 出站条件见 WEATHER-PLAN §6）；像素 GI 贡献的相机路径透射（gather 段 ×T）留 M-WEA3+ 单独拍板。 |


### D24 湿材质：UE 材质层 WetnessMask 手工节点 vs 本仓引擎级统一湿化（WEA M-WEA3 建档，2026-11-14；超集简化，WEATHER-PLAN §9d Q-W3-1 拍板）

| | 内容 |
|---|---|
| UE 做法 | 引擎零湿化逻辑（`Shaders/Private` grep Wetness 零命中）——湿表面是材质作者在材质图用 WetnessMask/顶点色手工调 roughness/albedo 的节点级组合；Landscape 物理材质按层混合。无全局湿度标量。 |
| 我们现状（M-WEA3） | 引擎级统一湿化：全局 wetness 标量（WeatherState ODE 记忆态，M-WEA0）→ 双处 shadePbrGrad 内 `wetW = wetness × lerp(0.35,1,saturate(N·up))`，roughness 压到 35% + albedo ×0.65（N·up 加权 = 雨从上往下淋近似）。本仓所有 PBR 材质统一生效，无逐材质 mask。 |
| 定性 | **超集简化（有意）**：本仓场景无逐材质湿度 mask 需求（Suzanne 统一湿化即视觉证据）；UE 口径自由度高但需要资产端配合，引擎统一版零资产成本。GI 弹射面反射率不随湿度变化（探针 hit 走统一模型色非 PBR，天然不波及）。 |
| 升级路径 | 逐材质湿化 mask（材质属性加 wetnessScale）留后续拍板；froxel 雨幕粒子属方案 B 远期。 |


## 计划内待办（非偏差，挂账防丢）

- **D8 资产分页 v4**（`NPAG` 页表 + 128KiB 字节预算）：对标 UE `FStreamOutMeshDataHeader`，代码落地排在 **P9 V1**（V1 即写 v4，不破 M3.2 回归基线）。
- **D9 流式调度**：UE 的 streaming priority/eviction 全套，P9 V4 范围。

## 对齐确认（不是差异，防 review 误判）

- **fallback**：weldPositions 顶层拼接 2.6% ≈ UE 默认 2%（`NaniteBuilder` fallback percent），语义对齐（M2.6 验收）。
- **误差链权威源**：全链（S3 构建 → S5 投影）统一 zeux 口径，bevy 只是载体；与 D1 呼应。
- **BLAS 几何来源 = 粗代理**：UE Nanite ray tracing 用 proxy mesh（粗代理几何），我们 BLAS 吃 S3 fallback（'NFLB' 段，Suzanne lod0 = 104 tri）——同构设计（M8.0 建档、M8.2 s8.png 收口确认）。**防 review 误判**：s8.png 里 rt 命中图（粗代理轮廓）与 hiz 栅格基线（最细 LOD 3936 tri）同相机对比轮廓不重合是设计本意，不是 bug；射线命中结果确实来自 BLAS 几何。

### D21 VSM 页表载体与深度 pass 几何源：UE Texture2D 页表 + 每页 UV 标记剔除 vs 我们 StructuredBuffer 页表 + 全簇直绘（M15.0 建档，2026-11-13；有意简化）

| | 内容 |
|---|---|
| UE 做法 | 页表是 `Texture2D`/`R32Uint`（`VirtualShadowMapPageAccessCommon.ush` 32-bit 编码 physX[0:9]/physY[10:19]/LODOffset[20:25]，硬件采样器 Load）；深度 pass 由 `BuildPerPageDrawCommands.usf` 按标页结果生成逐页 draw command（每页视锥/UV 剔除），Nanite 多视图 depth-only 直写页 atlas。 |
| 我们现状 | M15.0 页表 = `StructuredBuffer<uint>` 256 项（entry = poolPageX\|poolPageY<<8，只做 Load 查表，无硬件采样需求；poolPage 与 virtualPage 1:1，`0xFFFFFFFF` 未分配判亮）；深度 pass = `vsm.slang` 全簇 vertex pulling 直绘（不读 visibleList——那是相机视锥列表，会把相机外遮挡体漏掉），viewport = 虚拟空间 2048² 落页池左上 16×16 页区。 |
| 定性 | **有意简化**（D11 预测的静态 1:1 分配如期落地）：buffer 页表省 upload context 与格式限制，M15.0 页量 256 项无采样性能差异；全簇直绘在演示资产规模（711 簇/11808 tri）下无剔除必要。 |
| 升级路径 | M15.1 标页/分配/回收落地时页表迁 `R32Uint` Texture2D（对齐 UE 编码留 LODOffset 位）+ per-page draw commands（UE `BuildPerPageDrawCommands` 对位）；页池扩上限池（`MaxPhysicalPages`）后 entry 位宽同步扩（physX/physY 各 10 bit 对位 UE）。 |
| 触发条件 | 页量 > 4096（页表 buffer 降速）或页搬运需要硬件 SampleLevel 比较时迁移；大场景出现相机外遮挡体漏影属预期（全簇直绘不剔），无需修。**M15.1 拍板注记（2026-11-13 方案 A）**：M15.1 落地 CPU 分配器 + StructuredBuffer 页表扩展编码（level 高位），Texture2D 迁移继续挂触发条件不提前；标页走 GPU compute（UE PageMarking 对位）但分配/缓存/回收在 CPU（确定性优先，UE 全 GPU 分配的差异在此登记）。**M15.1 收口增注（2026-11-13 换基准拍板）**：深度 pass 全簇直绘 vs 射线 104-tri 粗代理的几何基准差已量化——fallback 假影 → 漏阴影 1072px、细网格真影粗代理没有 → 假阴影 1131px、页 texel 粒度少量边缘差，合计 **0.47%（near 口径）**；验收门基准换 `--vsm-rayref` 同 blob 射线兜底渲染（s9 冻结 PNG 与当前 blob 共享代码漂移 ~4.7% 不进门，退役为 degraded guard 专用）。python 软件光栅化逐 texel 对比 130.9 万 texel 全对齐（页池写入侧 100% 正确）方法论与页缓存 key 负 wx 跨级碰撞根因详 S15-PLAN M15.1 收口段与 MILESTONES M15.1 节。 |

---
*维护记录：2026-11-08 建（M5.0 收口），D1~D7 来自 S0~S5.0 实读 UE/bevy/Carrot 代码的对照结论；2026-11-08 M5.1 收口增补 D3a（遍历结构差异）；2026-11-09 M6.1 收口更新 D6（Hi-Z 遮蔽已实现，单 dispatch + 上帧 HZB 的结构差异留痕）与 D3a（second pass 交汇拍板记录）；2026-11-09 D6 补「已知偏差」行（度量空间径向距离 vs UE 设备深度 + max/min 等价换算说明，用户要求差异必须落档）；2026-11-09 M6.2 收口 D6 补开关实现差异（UE 编译期 permutation+CVar vs 我们 CB 运行时分支）与统计口径对齐说明（实读 NaniteCullRaster.cpp:250-262 r.Nanite.Culling.HZB/Frustum、NaniteDefinitions.h:517 FNaniteStats、bevy bindings.wesl:143-152 atomic counters、Carrot 无 Hi-Z 模块无对照）；出处行号以 2026-11-08~09 实读版本为准；2026-11-09 S15/S16 只读预研二次确认后新增 D11（VSM 逆序渐进 / 「只读 GBuffer」底线修订）与 D12（TSR jitter 与离屏 post-chain 共享前置 / 卷积上采样核对齐 UE-bevy），开工前不再重复调研；2026-11-09 M-P2 收口修复编号撞号（M-P1 碰撞过滤条目原误编 D12，与 S16 TSR 的 D12 重号，改为 D13 并同步四处引用），新增 D13（碰撞过滤三态 vs bool 矩阵）与 D14（休眠岛口径）；2026-11-09 M7.0 收口新增 D15（vis buffer 载体 R64_UINT image atomic → u64 buffer atomic，含 buffer/image 能力位差异与升级触发条件）；2026-11-09 M7.1 收口新增 D16（bevy ddy 翻转不带过来——NVRHI SV_Position 已是 y-down 像素系 + 全屏 pass 必须 SampleGrad 的关联坑）；2026-11-09 P8 M-S1（feat/audio merge 回 master）建档 D17（音频距离衰减超 maxDistance 不自动静音，miniaudio 4 模型 vs UE 6 模型 + `IsInAudibleRange` 由 `nl/audio` 补剔除；feat/audio 分支上原编 D16 与 M7.1 的 D16 撞号，合并时改为 D17）；2026-11-10 M7.2 收口（S7 收官）新增 D18（材质归属编码：UE 簇内 MaterialTableOffset/Length vs 我们 NSUB 子网格序号——「构建期随簇记录」结论逐字对齐，编码粒度等价实现）；2026-11-10 M8.2 收口（S8 收官）「对齐确认」增补 BLAS 几何来源=粗代理行（UE Nanite RT proxy mesh vs 我们 S3 fallback——同构设计防误判，s8.png 口径注记）；2026-11-10 M9.0 开工新增 D19（光追阴影几何=粗代理，与光栅可见几何轮廓不重合——用户拍板「接受差异并留痕」，M9.1 验收判据改写为粗代理几何上边缘干净 + 差异可视化图，几何对齐留 S13）；2026-11-13 S12 M12.2 收官新增 D20（…详 D20 节）（历史拒绝：UE 帧数置信度 + 邻域 clamp 无显式属性拒绝 vs 我们 Q12-B 续拍 B1 零落盘退化判据——三处口径不一致后用户开工闸拍板，多物体场景复评为升级触发条件）。2026-11-13 M15.0 收口新增 D21（VSM 页表载体 StructuredBuffer vs UE Texture2D + 深度 pass 全簇直绘 vs 每页 UV 剔除，页表迁 Texture2D 的升级路径在案）。
