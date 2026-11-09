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

## 计划内待办（非偏差，挂账防丢）

- **D8 资产分页 v4**（`NPAG` 页表 + 128KiB 字节预算）：对标 UE `FStreamOutMeshDataHeader`，代码落地排在 **P9 V1**（V1 即写 v4，不破 M3.2 回归基线）。
- **D9 流式调度**：UE 的 streaming priority/eviction 全套，P9 V4 范围。

## 对齐确认（不是差异，防 review 误判）

- **fallback**：weldPositions 顶层拼接 2.6% ≈ UE 默认 2%（`NaniteBuilder` fallback percent），语义对齐（M2.6 验收）。
- **误差链权威源**：全链（S3 构建 → S5 投影）统一 zeux 口径，bevy 只是载体；与 D1 呼应。

---
*维护记录：2026-11-08 建（M5.0 收口），D1~D7 来自 S0~S5.0 实读 UE/bevy/Carrot 代码的对照结论；2026-11-08 M5.1 收口增补 D3a（遍历结构差异）；2026-11-09 M6.1 收口更新 D6（Hi-Z 遮蔽已实现，单 dispatch + 上帧 HZB 的结构差异留痕）与 D3a（second pass 交汇拍板记录）；2026-11-09 D6 补「已知偏差」行（度量空间径向距离 vs UE 设备深度 + max/min 等价换算说明，用户要求差异必须落档）；2026-11-09 M6.2 收口 D6 补开关实现差异（UE 编译期 permutation+CVar vs 我们 CB 运行时分支）与统计口径对齐说明（实读 NaniteCullRaster.cpp:250-262 r.Nanite.Culling.HZB/Frustum、NaniteDefinitions.h:517 FNaniteStats、bevy bindings.wesl:143-152 atomic counters、Carrot 无 Hi-Z 模块无对照）；出处行号以 2026-11-08~09 实读版本为准。*
