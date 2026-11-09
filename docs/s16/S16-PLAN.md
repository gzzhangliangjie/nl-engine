# S16 详细开发路线图：TSR（Temporal Super Resolution，时域超分）

> 对应总路线 `docs/ROADMAP.md` 的新增研究轨道 S16。**拆分：velocity/history 基建在 S7 之后先建（喂 S12 时空滤波复用 motion vector），完整算法在 S7 之后轻量直光场景隔离验证，最终集成 slot 仍在 tonemap 前（Lumen 之后主帧）**。预计 5 个里程碑、业余时间 2~2.5 周。
> 目标：在 tonemap 之前的 HDR scene color 上做时域上采样，替代 TAA，把渲染分辨率降到 50~66% 仍接近全分辨率画质。全程 compute pass，完美契合本项目 Slang 单源双后端。
>
> **2026-11-09 预研轮（只读）**：三处参考按对照总表定向读完，并完成原文「待开工按铁律二次确认」。结论固化在下方「预研结论」节，开工前不再重复调研。用户 2026-11-09 拍板按建议落地（本轮只改文档：未动代码 / `kStage` / worktree / 回归 / tag）。UE 源码只读不抄（EULA）。

---

## 三处参考口径（2026-11-08 定向读；2026-11-09 预研二次确认修订）

| 来源 | 定位 | 实读文件 | 口径 |
|---|---|---|---|
| **UE5（唯一工业终态，只读不抄）** | 满分答卷 | `Renderer/Private/PostProcess/{TemporalSuperResolution.cpp,PostProcessing.cpp,TemporalAA.cpp}`、`HaltonUtilities.h`；`Engine/Shaders/Private/TemporalSuperResolution/{TSRCommon,TSRReprojectionField,TSRDepthVelocityAnalysis}.ush`、`{TSRSpatialAntiAliasing,TSRUpdateHistory,TSRRejectShading,TSRResolveHistory,TSRDilateVelocity,TSRDecimateHistory,TSRDetectThinGeometry,TSRWeightRelaxation}.usf` | 独立超分器，10+ compute pass：dilate velocity → decimate → reject shading（卷积网络）→ spatial AA → update history（jacobian reprojection field + 3×3 邻域 clamp + parallax/velocity weight rejection）→ resolve history（Mitchell-Netravali / Nyquist wave）；history 五套 array（Color/Metadata/Guide/Moire/Coverage）；jitter = `TemporalJitterPixels`（Halton）；位于 SeparateTranslucency / DOF 之后、MotionBlur / Tonemap 之前；放大倍率由 `PrimaryScreenPercentageMethod=TemporalUpscale` 决定，`r.TSR.History.ScreenPercentage` 决定 history 是否 2× 超采样 |
| **bevy（构件参照，非否决检查）** | 同代实证 | `bevy_anti_alias/src/taa/{mod.rs,taa.wesl}`、`bevy_anti_alias/src/dlss/{mod.rs,prepare.rs,node.rs}`、`bevy_core_pipeline/src/{upscaling,prepass,blit}`、`bevy_render/src/{camera.rs,view/mod.rs}`、`bevy_solari/src/realtime/{restir.wesl,gbuffer_utils.wesl,resolve_dlss_rr_textures.wesl}` | **二次确认：无 TSR 本体**。`bevy_anti_alias::taa` 是**同分辨率 AA**，但构件最可复用：5-tap Catmull-Rom history 采样、closest-depth motion vector（Karis 2014）、YCoCg 方差 clip-towards-AABB-center、confidence 存 alpha、8 点 Halton、`unjittered_clip_from_world` 剥离 jitter 的 velocity 基础设施、ping-pong history；**缺显式 depth/normal disocclusion 判据**。`upscaling` 只是单次 blit；真超分是 DLSS 黑盒外挂。仓库内最完整的 history rejection 在 `solari/restir.wesl` + `gbuffer_utils.wesl`（`pixel_dissimilar`：相对深度 >0.3% 或法线夹角 >90° 拒绝） |
| **Carrot（反例 / 节奏参照）** | 从零节奏 | `engine/engine/CarrotRenderingPipeline.cpp`、`render/{CameraBufferObject.cpp/.h,GBuffer.cpp,lighting/LightingPasses.cpp,raytracing/RayTracer.cpp}`、`shaders/post-process/taa.slang`、`shaders/lighting/svgf.slang`、`shaders/compute/{temporal-denoise.compute.glsl,firefly-rejection.slang}`、`shaders/raster/{draw_gbuffer,draw_virtual_geometry}.slang`、`shaders/modules/{camera,gbuffer,gi}.slang` | **二次确认：零超分**（`scaleFactor = 1.0f`，`RayTracer.cpp` 的 `ResolutionScale = 0.5f` 是未使用残留；全库无 FSR/DLSS/XeSS/TSR）。TAA 是 elopezr 博客简化版（固定 α=0.1、nearest `Load`、3×3 min/max clamp、无 YCoCg/方差裁剪/反遮挡/锐化），但**基建扎实**：R2 低差异 jitter、双帧相机矩阵、完整 motion vector（含 z 与 jitter 补偿）、velocity 打包进 GBuffer、`reuseResourceAcrossFrames` 跨帧复用；SVGF / temporal-denoise / GI 累积三套时域体系彼此割裂，没有统一超分框架 |

> 三者不是「有 / 无」二选一：UE 给终态验收口径，bevy 给可直接审计的 TAA 构件（从零起步优先照它搭骨架），Carrot 给「多套时域体系割裂」的反例。**原「bevy/Carrot 仅作否决检查」的口径作废**（2026-11-09 预研结论）。

## 预研结论（2026-11-09，三处对照 + 二次确认）

### 1. 最致命缺口：原计划没有 jitter / 渲染-输出分辨率分离框架

三处一致把抖动序列当成超分的前提：UE `TemporalJitterPixels`（`FHaltonSequence`）、bevy 8 点 Halton + `TemporalJitter` + `MipBias`、Carrot R2 低差异序列 + 双帧 jittered/non-jittered 投影矩阵。**没有 jitter 就没有亚像素新信息**，TSR 会退化成「时域降噪 + 双线性放大」，达不到「50~66% 渲染分辨率仍接近全分辨率」的目标；camera cut / pre-exposure 校正也只半句带过。
→ **并入 M16.0**（不新增里程碑编号，避免 tag 连锁改动）：M16.0 必须先落地「jitter 序列 + 双帧矩阵 + 渲染分辨率/输出分辨率分离 + jitter 剥离的 velocity」。

### 2. 前提「已具备 HDR scene color（tonemap 前）」在当前代码不成立

S6 现状（2026-11-09 实读 `renderer/`）：swapchain framebuffer 直接出图，`model.slang:216` 输出端 gamma 编码（注释明写「不算 tonemap，plan 边界不碰后处理」）；只有 D32 `SceneDepth` / `HizPrepassDepth` 与 R32F `hizPyramid_`，**无离屏 HDR RT、无 tonemap pass、无 post-chain、无 velocity / normal GBuffer**。所谓「tonemap 前 slot」当前不存在。
→ **离屏 HDR RT + post-chain 必须作为共享前置单列**：SSR 预研 `docs/s6/SSR-RESEARCH-NOTES.md` §3/§4 已独立指出同一缺口（「离屏 RT 合成与 post-chain 穿插项是同一份基建，别重复建设」）。归属写进 ROADMAP 穿插项表，SSR / S16 / 后续 bloom 共用，不各建一份。

### 3. 上采样核：原 M16.2「5×5 联合双边」与三处口径都不一致

UE 用 reprojection field + Mitchell-Netravali / Nyquist 卷积 + spatial AA；bevy 用 5-tap Catmull-Rom；Carrot 用 nearest + 3×3 clamp。联合双边是 **S12 GI 降噪**的滤波器（S12-PLAN M12.1），搬来当上采样核属自创路线。
→ M16.2 改为**历史卷积上采样（5-tap Catmull-Rom，对齐 UE/bevy）**，depth/normal 引导降级为可选的边缘对照；定性「与 UE 对齐」而非新增偏离（见 `docs/s5/UE-DIFFS.md` D12）。tag 由 `s16-bilateral` 改 `s16-upsample`（S16 未开工、tag 不存在，无历史依赖）。

### 4. velocity 共享口径统一（与 S12 冲突已消除）

S12-PLAN M12.0 原允许「物体全静态时可先只做相机版 motion vector」，但 TSR 必须逐像素物体速度，否则动目标 ghost 直接爆。ROADMAP 说 S16 前半段「喂 S12 复用 motion vector」——**共享的必须是完整逐像素 velocity GBuffer**（当前 clip pos vs 上一帧 clip pos，含 jitter 剥离），不是相机版。S12-PLAN 已同步修订。

### 5. 二次确认：缺位分三档

| 档 | 内容 | 对本模块的意义 |
|---|---|---|
| 本体缺位 | bevy / Carrot 均无 TSR / 时域上采样 | UE 仍是唯一终态对照，不变 |
| 构件齐备 | bevy 的 TAA（Catmull-Rom / YCoCg 方差 clip / confidence）+ Carrot 的 jitter/双帧相机/velocity 打包 + bevy ReSTIR 的 `pixel_dissimilar` | M16.0~M16.3 的骨架基本有可审计参照，不必从零设计 |
| 明确反例 | Carrot 四套时域体系割裂、TAA 半成品（`momentsAndHistory` / `linearSampler` 声明未用）；bevy 把真超分交给 DLSS 黑盒 | 我们必须收敛成**一条**统一的超分框架，不各建一份 |

### 6. 共同前置能力（与 S15 共有，UE 侧实证）

GPUScene 持久索引 + 跨帧 GPU 资源池 + 稳定 id 重映射 + depth/velocity + 动态分辨率协商 + HZB。S6 现状已有 GPU-driven cull + Hi-Z 金字塔 + D32 深度；缺跨帧持久 GPU 资源、稳定 id 映射、velocity、动态分辨率。history ping-pong 是本项目第一次真正的跨帧颜色资源，`createTargets()` slot 级同步表纪律必须前置遵守。

---

## 前置（拆分：velocity/history 基建 S7 后先建喂 S12，完整算法 S7 后轻量场景验证，最终 slot tonemap 前，不依赖 Lumen）
- **共享前置（需新建，归属 post-chain 穿插项）**：离屏 HDR RT + tonemap / 合成 pass。SSR 与 S16 共用同一份，不重复建设（预研 §2；`docs/s6/SSR-RESEARCH-NOTES.md` §3/§4）。**S6 现状并无此 slot**。
- **需新建**：完整逐像素 **velocity GBuffer**（当前 clip pos vs 上一帧 clip pos，用已有 volatile MVP CB + 双帧矩阵，jitter 剥离）。与 S12 共享同一张，不各建一份（预研 §4）。
- **需新建**：jitter 序列 + 渲染分辨率 / 输出分辨率分离（预研 §1，并入 M16.0）。
- **S7 / S9 之后具备**：depth / normal GBuffer（S7 vis buffer 材质 pass 反解，S9 GBuffer）、法线/粗糙度（SSR 亦依赖）。
- 与 S12 时空滤波共享 velocity 资源，但 S12 是 GI 降噪、TSR 是上采样，互不替代。

## M16.0 jitter + 双帧矩阵 + velocity + 半分辨率渲染 + 朴素双线性（3~4 天）
- [ ] **jitter 序列**（Halton，8 点起步）+ jittered / non-jittered 双套投影矩阵写进相机 CB；渲染分辨率与输出分辨率分离（50% 渲染 → 100% 输出）。
- [ ] **velocity GBuffer**：当前 clip pos vs 上一帧 clip pos，用 `unjittered_clip_from_world` 剥离 jitter（bevy 口径），逐像素含物体速度，不是相机版。
- [ ] 双线性上采样到全分辨率（对照基线）。
- [ ] velocity / jitter 当颜色可视化，确认移动物体与抖动方向正确。

**自检**：半分辨率双线性图内容正确但模糊；velocity 可视化正确（轮廓/移动方向对）；**关掉 jitter 后连续帧无新亚像素信息**（退化为纯时域降噪，这条是 jitter 生效的反向证据）。tag：`s16-vel`。

## M16.1 历史重投影（2 天）
- [ ] 上一帧 upscaled 结果存为 history 纹理（ping-pong），history 资源按 `createTargets()` slot 级同步表初始化。
- [ ] 用 velocity 把 history 重投影到当前帧（closest-depth motion vector 选点，Karis 2014）。
- [ ] camera cut / 场景切换重置 history；pre-exposure 校正（避免曝光变化被当成闪烁）。
- [ ] 相机静止时结果时域稳定（不抖）。

**自检**：静止场景 TSR 输出与全分辨率参考逐像素接近、无时间抖动；相机切场景时重置 history（无残留鬼影）。tag：`s16-reproj`。

## M16.2 历史卷积上采样（5-tap Catmull-Rom，对齐 UE/bevy）（2~3 天）
- [ ] 用 **5-tap Catmull-Rom** 卷积替代双线性（bevy `taa.wesl` 同款；UE 终态为 reprojection field + Mitchell-Netravali / Nyquist 卷积，列为升级路径）。
- [ ] depth/normal 引导的联合双边**仅作可选边缘对照**，不作主上采样核（S12 的滤波器，别混用）。

**自检**：边缘比双线性更锐、shimmer 更少；与全分辨率参考误差下降。tag：`s16-upsample`（原 `s16-bilateral` 随预研改名，S16 尚未开工无历史 tag 依赖）。

## M16.3 邻域钳制（YCoCg 方差 clip）+ 拒绝掩码 + dilate（2~3 天）
- [ ] 邻域钳制抑制重影拖尾：bevy `taa.wesl` 的 YCoCg `moment_1/2` → mean±σ → `clip_towards_aabb_center`（对位 UE `RejectHistory` / `ShadingRejection` 思路）。
- [ ] 深度/法线不连续处弃用 history（disocclusion）：参照 bevy `solari/gbuffer_utils.wesl` 的 `pixel_dissimilar`（相对深度 >0.3% 或法线夹角 >90°），dilate 好样本。
- [ ] 关掉钳制 / 拒绝能单独回退（退化可验）。

**自检**：移动物体不拉尾迹；物体露出背景处不 smear；关掉钳制/拒绝立即回退（退化可单独验）。tag：`s16-reject`。

## M16.4 锐化 + 最终混合 + 双后端（2 天）
- [ ] 锐化补偿卷积上采样带来的模糊（对位 UE 锐化系数 / `TSRSpatialAntiAliasing` 边缘浏览思路）。
- [ ] 输出写 history，接入主帧（tonemap 前；需先有共享前置 post-chain）。
- [ ] DX12 / Vulkan 同图，分档 bytes/near。

**自检**：锐度接近全分辨率参考、客观误差（PSNR vs 全分辨率基线）可接受；`regression.py` 全绿；kStage bump 后重拍 m0.9/s2.0 HUD 基线。tag：`s16-done`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M16.0 jitter+velocity+半分辨率 | 3~4 天 | `s16-vel` |
| M16.1 历史重投影 | 2 天 | `s16-reproj` |
| M16.2 卷积上采样 | 2~3 天 | `s16-upsample` |
| M16.3 钳制+拒绝 | 2~3 天 | `s16-reject` |
| M16.4 锐化+双后端 | 2 天 | `s16-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 历史更新/重投影 pass | UE `PostProcess/TemporalSuperResolution.cpp`（`AddTemporalSuperResolutionPasses` / `TSRUpdateHistory.usf`）+ `PostProcessing.cpp`（帧管线时机） |
| 拒绝掩码/去鬼影 | UE `TSRRejectShading.usf` / `TSRSpatialAntiAliasing.usf`；可审计构件 bevy `taa.wesl`（YCoCg 方差 clip）+ `solari/gbuffer_utils.wesl`（`pixel_dissimilar`） |
| 历史编码/分辨率/jitter | UE `TSRCommon.ush` / `HaltonUtilities.h` / `r.TSR.History.*`；bevy `bevy_anti_alias/src/taa/mod.rs`（8 点 Halton + ping-pong）+ `bevy_render/src/camera.rs`（`TemporalJitter`） |
| 卷积上采样核 | bevy `taa.wesl`（5-tap Catmull-Rom 权重）；UE `TSRResolveHistory.usf`（Mitchell-Netravali）+ `TSRReprojectionField.ush`（jacobian，升级路径） |
| motion vector / velocity 基建 | bevy `bevy_core_pipeline/src/prepass/mod.rs`（`MotionVectorPrepass` / `PreviousViewUniforms`）+ `background_motion_vectors.wesl`（jitter 剥离）；Carrot `CameraBufferObject.cpp`（R2 jitter + 双帧矩阵）+ `GBuffer.cpp`（velocity 打包） |
| 共享前置（离屏 RT / post-chain） | `docs/s6/SSR-RESEARCH-NOTES.md` §3/§4 + ROADMAP 穿插项「后处理链」行 |
| 时域体系割裂反例 | Carrot `CarrotRenderingPipeline.cpp` 的 `finalTAA` vs `lighting/svgf.slang` vs `compute/temporal-denoise.compute.glsl` 三套并存 |

## 底线
- TSR 在 **tonemap 之前、Lumen GI 之后** 插入主帧；但**该 slot 当前不存在**，须先落地「离屏 HDR RT + post-chain」共享前置（预研 §2），与 SSR / bloom 共用，不各建一份。
- **jitter 是超分的前提**：M16.0 必须先有抖动序列 + 双帧矩阵 + 渲染/输出分辨率分离；velocity 用 unjittered 矩阵剥离抖动。
- **velocity 精度是第一大病源**（世界/屏幕空间、上一帧矩阵、轮廓精度、jitter 剥离）；M16.0 可视化验收不可省；与 S12 共享同一张完整逐像素 velocity GBuffer。
- 「看着还行」是主观的：M16.0 起建全分辨率参考 + 客观误差指标（PSNR），接 `regression.py`。
- 每次 kStage bump 重拍 m0.9/s2.0 HUD 基线（M3.2 固化④）；UE 代码一行不进 repo。
- 与 UE 的框架差异（无卷积网络 shading rejection / 无 parallax reprojection field / 无 Nyquist 超采样 history）定性「计划内待办」，见 `docs/s5/UE-DIFFS.md` D12。
