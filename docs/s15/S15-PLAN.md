# S15 详细开发路线图：Virtual Shadow Maps（虚拟阴影贴图）

> 对应总路线 `docs/ROADMAP.md` 的新增研究轨道 S15。**插入位置：S9 之后（阴影阶段 rasterized 对照路线，合并原 `shadow-map` 穿插项）**。预计 3 个里程碑、业余时间 1.5~2 周。
> 目标：用「clipmap 级联 + 物理页池 + 页表」替代传统 shadow map / S9 的光追阴影射线，得到随距离无缝、可复用 Nanite 簇渲染路径、能与 GPU-driven 共存的阴影系统。
>
> **2026-11-09 预研轮（只读）**：三处参考按对照总表定向读完，并完成原文「待开工按铁律二次确认」——结论固化在下方「预研结论」节，开工前不再重复调研。用户 2026-11-09 拍板按建议落地（本轮只改文档：未动代码 / `kStage` / worktree / 回归 / tag）。UE 源码只读不抄（EULA）。

---

## 三处参考口径（2026-11-08 定向读；2026-11-09 预研二次确认修订）

| 来源 | 定位 | 实读文件 | 口径 |
|---|---|---|---|
| **UE5（唯一工业终态，只读不抄）** | 满分答卷 | `Renderer/Private/VirtualShadowMaps/{VirtualShadowMapArray,VirtualShadowMapClipmap,VirtualShadowMapCacheManager,VirtualShadowMapProjection,VirtualShadowMapShaders}.h/.cpp`；`Engine/Shaders/Private/VirtualShadowMaps/{VirtualShadowMapPageAccessCommon,VirtualShadowMapPageCacheCommon,VirtualShadowMapSMRTTemplate,VirtualShadowMapSMRTCommon}.ush`、`{VirtualShadowMapPageMarking,VirtualShadowMapBuildPerPageDrawCommands}.usf`；`Shaders/Shared/VirtualShadowMapDefinitions.h`；消费点 `LightRendering.cpp`、`Shadows/ShadowSceneRenderer.cpp`、`Nanite/NaniteCullRaster.cpp` | 16k² 虚拟地址 / 128² 页 / 8 级 mip；32-bit PageTable + 层级 PageFlags / PageReceiverMasks；物理页池 + 跨帧静态/动态页缓存与 GPU invalidation；Clipmap level 6~22；Nanite 多视图 depth-only 直写物理页 atlas；每页 GPU culling；页级 7 级 HZB；SMRT 射线步进；OnePassProjection → MaskBits 在 DirectLighting 开头被消费 |
| **bevy（构件参照，非否决检查）** | 同代实证 | `bevy_pbr/src/meshlet/{visibility_buffer_raster_node.rs,pipelines.rs,resource_manager.rs,cull_{instances,bvh,clusters,shared}.wesl,visibility_buffer_{software,hardware}_raster.wesl,resolve_render_targets.wesl,material_pipeline_prepare.rs}`；`bevy_pbr/src/render/{light.rs,shadow_sampling.wesl,shadows.wesl}`；`bevy_solari/src/realtime/mod.rs` | **二次确认：无 VSM**（page table / cache / clipmap 全库零命中），但有**完整 meshlet 阴影路径**：每个 `ShadowView` 从光源 `clip_from_world` 全量重跑 clear → first cull → raster → 本帧 HZB → second cull → raster → `resolve_depth`；阴影视图用 `R32Uint` 变体只写 depth（省掉 ID 解算，主视图为 `R64Uint`）；输出仍是固定分辨率 depth texture array，无页 / 缓存 / 增量。**这是 M15.2「簇级间接阴影」最可直接审计的范式** |
| **Carrot（反例 / 节奏参照）** | 从零节奏 | `engine/engine/CarrotRenderingPipeline.cpp`、`render/{PassEnum.h,Lights.h,ModelRenderer.cpp,resources/Pipeline.cpp,GBuffer.cpp}`、`shaders/lighting/{direct-base,direct-raytracing,direct-noraytracing}.slang`、`shaders/modules/{raytracing,raytracing_capabilities,lighting,virtual_geometry}.slang` | **二次确认：VSM 为零**——全库仅 `Pipeline.cpp` 两行 `// TODO: change for shadow maps`（depthClamp / depthBias），`PassEnum` 无 shadow pass；阴影靠 `rayQueryEXT` 单条 shadow ray；`canUseTLAS = !hasVirtualizedGeometry` → **虚拟几何不投影**，无光追硬件时阴影直接消失。作为「不做什么」的反例保留 |

> 三者不是「有 / 无」二选一：UE 给终态验收口径，bevy 给可审计构件（从零起步优先照它搭骨架），Carrot 给反例。**原「bevy/Carrot 仅作否决检查」的口径作废**（2026-11-09 预研结论）。

## 预研结论（2026-11-09，三处对照 + 二次确认）

### 1. 二次确认：缺位分三档，不是「都没有」

| 档 | 内容 | 对本模块的意义 |
|---|---|---|
| 本体缺位 | bevy / Carrot 均无 VSM 页表 / 物理页池 / 跨帧缓存 | UE 仍是唯一终态对照，不变 |
| 构件齐备 | bevy 有「一套 GPU-driven 几何管线复用给每个阴影视图」的完整实现；Carrot 有 jitter / 双帧相机 / velocity 打包 GBuffer 基建 | M15.2 不必从零设计阴影视图，照 bevy 的 `R32Uint` 变体 + 两阶段 HZB 剔除搭骨架 |
| 明确反例 | Carrot 虚拟几何不进 TLAS → 不投影 | 我们的 VSM 必须让簇几何进阴影路径，这条是 M15.2 验收的隐性判据 |

### 2. UE 依赖链 vs M15.x 顺序（登记为**有意简化**，见 `docs/s5/UE-DIFFS.md` D11）

- UE 顺序：`GBuffer/Depth 标页 → BuildPageAllocation（含跨帧缓存失效） → 每页 GPU culling → Nanite 多视图 depth-only 直写物理页 atlas → 页后处理 / 页级 HZB → 投影采样`。
- 我们的顺序（M15.0→M15.2）是**逆序渐进**：先「物理页池 + 静态整页分配 + 投影采样」出画，再补页请求/分配/回收，最后接簇级间接阴影。
- 定性：M15.0 的「VSM」实为「带页表的静态 shadow map」，UE 的两个核心（页请求 + 跨帧缓存）被压到 M15.1/M15.2。**保留这个节奏不返工**（先出画再补复杂度符合本项目节奏），但验收时**不得按 UE 终态直接对标**，并在 D11 写明升级路径与触发条件。

### 3. 底线措辞修正（原「绝不碰 GBuffer 主路径」自相矛盾）

UE 的 `GeneratePageFlagsFromPixels` **吃 GBuffer 深度 / 前层半透明 / froxel** 反投影标页，投影 CS 也在 DirectLighting 开头吃 GBuffer。M15.0 静态整页分配可以不读，M15.1「每帧页请求」必然要读主视图深度。
→ 底线改为：**不改主路径输出格式，只读其 depth / GBuffer；新增的只有阴影深度目标 + 页表 GPU 资源 + 投影采样**。

### 4. 共同前置能力（UE 侧实证，VSM 与 TSR 共有）

GPUScene 持久索引 + 跨帧 GPU 资源池 + 稳定 id 重映射 + depth/velocity + 动态分辨率协商 + HZB。我们 S6 现状已有 GPU-driven cull（S4）+ Hi-Z 金字塔 + D32 深度；**缺跨帧持久 GPU 资源与稳定 id 映射**。M15.1 的页缓存是本项目第一次真正跨帧持久化 GPU 资源，`createTargets()` slot 级同步表纪律（渲染约定）必须前置遵守。

---

## 前置（已具备，依赖 S4+S7/S9 在 S9 已全部就绪 → 插入 S9 之后，无需等 Lumen）
> **2026-11-10 Q4 拍板方案 A 勘误**：S9 不落 GBuffer 纹理，交付的是 `gbuffer_common.slangh` 公共反解契约；下文凡「S9 GBuffer / 读 GBuffer」一律指 UE 侧概念或本项目 vis buffer 即取即算属性。
- **S4 硬件光栅 + GPU 视锥剔除 + indirect draw**：VSM 的阴影深度渲染直接复用 S4 的簇渲染/剔除路径，只把渲染目标从屏幕换成物理页池。
- **S7 vis buffer / S9 公共反解契约**：最终着色采样 VSM 投影结果；M15.1 起页请求需要**只读**主视图 depth / vis buffer 即取即算属性（见预研 §3，不改其输出格式；Q4 方案 A 下无屏幕 GBuffer 纹理）。
- VSM 与 S9 光追阴影射线是「阴影的两条平行路线」，互为验收对照，不互相替代。

## M15.0 单级 VSM 框架：物理页池 + 投影采样（2~3 天）
- [ ] 建立物理页池（Physical Page Pool）：一张大 depth 图按 PageSize 切页，`MaxPhysicalPages` 上限（对位 UE `r.Shadow.Virtual.MaxPhysicalPages`）。
- [ ] 单光源 clipmap 一级：把该光源视锥覆盖区渲进若干物理页（先整页分配，不做动态请求）。
- [ ] 投影采样 shader：世界坐标 → 光源 clipmap 页坐标 → 采样物理页深度比较。
- [ ] 先把阴影画到屏幕，替代 S9 射线阴影做对照。

> 预研注记：本里程碑是**带页表的静态 shadow map**（UE 依赖链的末段先做），属 D11 有意简化，不按 UE 终态验收。

**自检**：单方向光阴影形状/软硬与 S9 参考图方向一致；关掉 VSM 退回 S9 路线时画面退化可见。tag：`s15-vsm`。

## M15.1 级联 clipmap + 页请求/分配/回收（3~4 天）
> **2026-11-13 用户拍板（方案 A：GPU 标页 + CPU 分配器）**：compute 标页照 UE `PageMarking` 对位（吃主视图 depth 反投影 → 请求位图）→ 复用 S4 `cullReadback_` 环形习语异步回读（1 帧延迟，UE 同吃延迟）→ CPU 维护空闲链表/跨帧缓存/失效回收 → CPU 写页表 → CPU 逐分配页下发 draw（每页一个 viewport，M15.2 再换 GPU indirect）。确定性保留（bytes 基线纪律不破）；HUD 零成本。页表载体**不迁** Texture2D（扩展 StructuredBuffer 编码加 level 高位；D21 迁移触发条件 M15.1 不满足）。多级参数：4 级、level radius 对位 UE `2^(level+1)` 口径、每级虚拟空间 2048²/页表 256 项。
- [x] 多级 clipmap：按距相机距离分 N 级（对位 UE `VirtualShadowMapClipmap`），近处高分辨率、远处低分辨率。
- [x] 每帧页请求 → 分配物理页 → 渲染 → 未命中/失效页回收（对位 UE `VirtualShadowMapCacheManager`：缓存有效页跨帧复用）。
- [x] 页请求读主视图 depth / vis buffer（Q4 方案 A 下无屏幕 GBuffer 纹理；预研 §3，只读不改格式）；跨帧页缓存资源按 `createTargets()` slot 级同步表初始化。
- [x] HUD 显示物理页占用 / 各级分辨率。

**M15.1 收口（2026-11-13）**：tag `s15-clipmap`。实测参数（MVP dump 权威）：coarsestWindow = 4×场景最大边 = 31.495（AABB ext = 7.874×1.925×1.626），L0 half = 1.968 / texel = 0.001922，窗口中心严格居中（focus=原点时 floor snap 无偏移）。

**M15.1 漏阴影定位与验收口径变更（2026-11-13，两次用户拍板）**：
1. **活体最小探针（拍板 A）**：stdout 逐级 alloc 日志 + 服务级伪彩探针 + 页池/页表/MVP 全 dump。决定性实验 = **python 软件光栅化 vs GPU 页池逐 texel 对比**（复刻 shadowLookAt/shadowOrtho/窗口 remap，nmesh 52B record 权威布局）：130.9 万 texel 全对齐、深度差 ≤0.0035、漏画 0——**页池写入侧 100% 正确**。
2. **根因（实锤修复）**：页缓存 key 负 wx 跨级碰撞——`key=(l<<62)|(uint32(wx)<<32)|uint32(wy)` 用 OR，wx<0 时 uint32 回转值最高两位是 11，OR 吞掉级位段 → 四级同 (wx,wy) 的 key 相同 → 跨级共池页 + dirty 只画一次。修复 = wx 掩 `0x3FFFFFFF` 再 `<<32`（分配器 + 页表重写两处；key 纪律注释落 scene.hpp）。
3. **换基准（拍板 B）**：修复后 verify 的 vsm-vs-ray 门仍 5.28%（vs s9 冻结 PNG）。三图对比实锤：同 blob 射线兜底 vs VSM 真实近似差仅 0.42%（sum>30 口径）/ **0.47%（near 口径，0.01 亦过）**；而 s9 冻结 blob 与当前 csDirectVsm blob 共享代码数值漂移 6924px（~4.7%，拍板 B 冻结 csDirect 所致，M15.0 时同源无此漂移）。**门基准换为 `--vsm-rayref`**（新增 CLI flag，fp.options[3] 通道，csDirectVsm 入口 params.x=0 走射线兜底 = 同 blob 同本底）；s9-shadow 冻结 PNG 退役为 degraded guard 专用。剩余 0.47% 构成 = fallback 104-tri 粗代理假影（漏阴影 1072px）+ 细网格真影粗代理没有（假阴影 1131px）+ 页 texel 粒度——几何基准差异非实现 bug，与 M15.0 卡「深度 pass 全簇 vs 射线 104-tri 粗代理设计差异」同构。

**自检**：大场景近处清晰、远处不糊不抖；相机前后移动时各级分辨率过渡平滑无突跳。tag：`s15-clipmap`。

## M15.2 Nanite/簇 间接阴影 + 去闪烁（2~3 天）
- [ ] 阴影深度渲染复用 S4 的 GPU-driven 间接绘制（簇级剔除/indirect），不另写一遍提交；构件参照 bevy `visibility_buffer_raster_node.rs` 的 ShadowView 变体（`R32Uint` 只写 depth + 两阶段 HZB 剔除），不引入 bevy 的每帧全量重绘语义（我们走页缓存）。
- [ ] **簇几何必须进阴影路径**（Carrot 反例：虚拟几何不投影）；验收时与 S9 光追阴影并排核对投影覆盖。
- [ ] temporal / rejection 去闪烁：阴影页跨帧稳定，边缘不闪（参考 UE `ShadingRejection` 思路，但本模块只做阴影页稳定性，不搬 TSR 整套路）。
- [ ] 双后端：DX12 / Vulkan 同图，分档 bytes/near。

**自检**：阴影边缘静止稳定、移动无 ghosting；与 S9 的直光分量参考图并排（Q3：一跳图不含间接光，GI 验收另立 S15 自立参考）；`regression.py` 全绿；kStage bump 后重拍 m0.9/s2.0 HUD 基线（M3.2 固化④）。tag：`s15-done`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M15.0 单级页池+投影 | 2~3 天 | `s15-vsm` |
| M15.1 级联+页管理 | 3~4 天 | `s15-clipmap` |
| M15.2 间接阴影+去闪烁 | 2~3 天 | `s15-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 物理页池/页表结构 | UE `VirtualShadowMaps/VirtualShadowMapArray.cpp`（页池分配、`MaxPhysicalPages`）+ `Shaders/Private/VirtualShadowMaps/VirtualShadowMapPageAccessCommon.ush`（32-bit 页表项编解码） |
| 级联 clipmap 布局 | UE `VirtualShadowMaps/VirtualShadowMapClipmap.cpp`（level 6~22、page-snapped 中心） |
| 投影采样/OnePassProjection | UE `VirtualShadowMaps/VirtualShadowMapProjection.cpp` + `LightRendering.cpp`（MaskBits 消费点） |
| 页缓存/请求/失效管理 | UE `VirtualShadowMaps/VirtualShadowMapCacheManager.cpp` + `VirtualShadowMapPageMarking.usf`（GBuffer 反投影标页） |
| 簇级阴影视图（M15.2 构件） | bevy `meshlet/visibility_buffer_raster_node.rs` + `pipelines.rs`（6 条 `*_shadow_view` 变体）+ `resolve_render_targets.wesl`（`R32Uint` → `frag_depth`） |
| 每页 GPU culling | UE `VirtualShadowMapBuildPerPageDrawCommands.usf`（`CullPerPageDrawCommandsCs` + load balancer） |
| 不投影反例（避坑） | Carrot `ModelRenderer.cpp:387` `canUseTLAS = !hasVirtualizedGeometry` |

## 底线
- VSM 是「阴影路线」不是「渲染路线」：**不改主路径输出格式，只读其 depth / vis buffer 属性**（预研 §3 修订；Q4 方案 A 后本项目无屏幕 GBuffer 纹理）；新增资源仅阴影深度目标 + 页表 GPU 资源 + 投影采样；S9 光追阴影作为对照保留。
- 物理页池是核心资源，slot 级同步表必须在 `createTargets()` 初始化（沿用渲染约定，防首帧 139 段错误）。
- 每次 kStage bump 必须重拍 m0.9 gate-hud 与 s2.0 lod-hud 两对基线（M3.2 固化④）。
- M15.0~M15.2 相对 UE 依赖链为逆序渐进，定性「有意简化」见 `docs/s5/UE-DIFFS.md` D11，验收不按 UE 终态对标。
- UE 代码一行不进 repo；参考结构与边界处理思路。
