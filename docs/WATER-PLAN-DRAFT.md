# 水体渲染计划草案（WATER-PLAN-DRAFT，2026-11-11 深夜，纯文档轮）

> 与 `docs/WEATHER-PLAN-DRAFT.md` 同轮：开放调研、未分配里程碑编号、未 claim/release、零落码 / 零 shader / 零 RT·UAV·槽位 / 零 tag / 零 `kStage` / 零 example / 零回归。
> 工作区并行会话在途改动（当日日志、MEMORY.md、S11-PLAN、gi-tools/main.cpp、s10/s11 verify 产物）一行未碰。
> 方向口径（已给、非待拍板）：水体 = **波形几何 + 反射/折射 + 吸收散射 + 透明排序 +（可选）水下体积合成**；禁止「一张反射贴图 + 屏幕雾」贴皮；验收必须含反射/折射侧证据（关掉反射后水体感几乎不变即不合格）。
> **铁律**：自研、只读 UE、bevy 作轻量阶梯、Carrot 只给节奏；开源项目只作算法参照，UE 只读不抄；凡引入第三方须逐项核 LICENSE 原文 + C++17 + 零构建期新依赖并写 `VENDOR.md`。

## 0. 为什么单独列（与体积云/天气解耦）

- 体积云依赖 3D 噪声 + weather map + raymarch + 时域重投影 + 半分辨率 RT，三处只有 UE 有；时间演化直接撞 M11.1 统计口径。
- 天气 = 时变参数喂现有 GI，禁止贴皮，验收含 GI 侧证据（见 `WEATHER-PLAN-DRAFT.md`）。
- **水体是独立工程链**：它有自己独有的「反射必须先于水体」依赖（平面反射或 SSR 没落地，水体的反射半边就得返工），且波形/折射/吸收/泡沫/透明排序是一整套不与云/天气重叠的 pass。混在一起反而说不清依赖，故单列。

## 1. 三处定向实读（水体真实架构）

### 1.1 UE `SingleLayerWaterRendering.h`（只读不抄，权威口径）
- `FSingleLayerWaterPrePassResult`：含 `Froxel::FRenderer Froxels`、`DepthPrepassTexture`、`RefractionMaskTexture`、`SceneDepthWithoutWater`、`VelocityPrepassTexture`、`MotionVectorWorldOffsetPrepassTexture` —— **水体与体积 RT 交叉处理**，不是单 pass 能塞。
- `FSceneWithoutWaterTextures`：`SeparatedMainDirLightTexture` / `ColorTexture` / `DepthTexture` + `RefractionDownsampleFactor` —— 折射要先渲「没有水的场景」，再降采样采样。
- `FSingleLayerWaterTileClassification`：用 `FTiledReflection`（即 SSR 的 tile 分类）+ `TileMaskBuffer` 做逐 tile 反射/折射分类。
- `ESingleLayerWaterPrepassLocation`：水体深度预渲可在 base pass 之前（需全深度预渲）或之后；`ESingleLayerWaterVelocityOutputPass`：速度可走 DepthPrepass 或 BasePass 输出。
- 结论：UE 水体 = **深度预渲 + 无水体场景渲 + tile 分类反射（SSR/平面反射）+ 折射掩码 + 速度**，与 GI/体积 RT 强耦合。算法口径唯一权威。

### 1.2 bevy `transmission.wesl`（轻量阶梯，可作折射结构参照）
- `refracted_ray()`：Snell 定律算折射方向 + 经 `thickness` 推出射点 → clip space → 屏幕 UV 偏移；`ior_corrected_roughness()` 按 IOR 把 roughness 拉向 0；`specular_transmissive_light()` 取背景 transmission 纹理 + 按深度拒绝（depth prepass 剔除前景）+ roughness 螺旋模糊 + 色散分通道 IOR。
- **定位**：bevy 只有「薄玻璃透射」模型（thin-glass transmission），**不是水面**。但其「折射 UV 偏移 + 屏幕背景采样 + 深度拒绝 + roughness 模糊」结构是水面折射的直接可用阶梯。无 Gerstner/FFT 波形、无波形法线、无泡沫、无水下体积。

### 1.3 Carrot（零水体）
- 全目录 grep `water|ocean|Gerstner|planar|refraction` 无命中。只有静态 cubemap 天空 + 线性距离雾。给不了水体参照，只印证「自研路线默认无水体」。

## 2. 水体渲染架构分解（pass 链）

1. **波形几何**：顶点位移（Gerstner 解析 / FFT-Tessendorf 频谱）。Gerstner 零纹理、可解析法线；FFT 需 compute 跑频谱 + 新依赖风险。
2. **法线**：Gerstner 解析导数（per-pixel）或 高度图/法线图采样。
3. **反射**：平面反射（镜像相机重渲）/ SSR（屏幕空间，已计划）/ cubemap IBL（静态、最廉价）。**这是水体最贵的半边**。
4. **折射**：渲「无水体场景」到纹理 → 按波形法线扭曲 UV 采样 + 深度差 Beer-Lambert 吸收。
5. **吸收/散射**：Beer-Lambert 按水深衰减 RGB（深绿/浅蓝），菲涅尔混合反射/折射。
6. **泡沫**：波峰阈值（Gerstner 偏度/Jacobian）或几何相交处。
7. **透明排序**：水体半透，须与场景透明物体（粒子/玻璃）协调排序。
8. **水下体积合成（可选）**：半分辨率水下雾/体积，与体积雾 RT 交叉（UE `Compose...UnderWater` 单独合成）。

## 3. 与现有基建对接

| 需求 | 现有基建 | 缺口 |
|---|---|---|
| 折射「无水体场景」纹理 | T0 `hdrSceneColor_` + post-chain 离屏 RT 齐 | 需一份「跳过水体」的场景渲（深度预渲分类即可） |
| 反射（屏幕空间） | **SSR 已排 S7 后插、预研齐、T0 基建就绪** | SSR 即水体反射的屏幕空间解法，一步两用 |
| 波形法线/深度 | T0 `velocity_` + depth prepass（M10.0） | Gerstner 顶点位移须在 raster 主路径加 pass |
| 吸收/菲涅尔/泡沫 | PBR direct 材质属性 | 水面材质函数（F0/ior/吸收系数）须新建 |
| 水下体积 | 体积雾/God Rays（候选 B 组） | 与体积 RT 交叉，须拍板 |
| GI 侧反射贡献 | S11 world cache / M10 探针 | 水体反射进 GI 属进阶，首版不做 |

## 4. 开源方案初筛（许可证来自搜索摘要，引入前须逐项核 LICENSE 原文）

**波形**：
- `iago-cpalmer/Ocean-Wave-Simulator`（MIT，C++/OpenGL 4.3）：Gerstner + fBm（至 2048 波）、**解析 per-pixel 法线**、Fresnel 天空盒反射、高光、泡沫、距离雾、ImGui。→ **Gerstner+法线+泡沫+Fresnel 最佳参照**。
- `CaffeineViking/osgw`（MIT，C++/OpenGL）：`gerstner.glsl` **解耦、可整段 copy-paste** 的 Gerstner 波形函数 + 细分 LOD + simplex 涟漪。→ **波形函数最干净参照**。
- `louis-jean0/OceanGL`（MIT，C++/OpenGL）：Gerstner + FBM + 动态天空盒 + 实时反射/折射 + ImGui。→ 完整管线可读参照。
- `jmermar/Vulkan-Water-Renderer`（MIT，C++/Vulkan）：FBM+sine 波、**cubemap IBL 反射**、compute chunk 派发 + 细分 LOD、Cook-Torrance PBR。→ **最贴我们 Vulkan/NVRHI 栈的反射取法**。
- `kentril0/WaterSurfaceRendering`（MIT，C++17/Vulkan）：Tessendorf FFT（CPU，AVX+OpenMP）。⚠️ 依赖 **FFTW 3.3.10 + OpenMP**（构建期新依赖，按选型硬闸禁入 vendored；FFT 思路可读，落地须自实现 compute FFT）。

**反射/折射**：
- `Yanyilucas/PurePool`（MIT）：FBO 反射/折射 + Fresnel + Beer-Lambert。→ **最贴「水体反射+折射」**。
- `ryzomcore` water_planar_reflection：平面反射 pass API + 分平面预算/粘性加权。→ 平面反射架构最可读。
- `crest-ocean/crest`（MIT，Unity，SIGGRAPH 17/19）：工业级 ocean。栈不对（Unity），只抄波形/焦散思路。
- ⚠️ `deiss/fftocean` = **GPL-3.0，按选型硬闸禁入**。

## 5. 方案 A/B/C + 代价矩阵

| 方案 | 波形 | 反射 | 折射 | 代价 | 与冻结基线关系 |
|---|---|---|---|---|---|
| **A 贴皮**（否决） | 静态法线图 | 反射贴图 | 无 | 最低 | 验收必挂（关反射几乎不变） |
| **B 首版（推荐）** | **Gerstner 解析**（osgw/iago 参照） | **SSR**（复用已计划项）| 无水体场景纹理 + 扭曲 UV + Beer-Lambert | 中：加 raster 波形 pass + 折射 RT | 不碰 GI 冻结基线；反射复用 SSR 基建 |
| **C 平面反射** | Gerstner | **镜像相机重渲** | 同 B | 高：每帧多渲一遍场景，与 P9「超大场景 60fps」冲突 | 独立 RT，不碰 GI |
| **D FFT 海洋** | **FFT-Tessendorf** | SSR/平面 | 同 B | 最高：compute FFT + 自实现（FFTW 禁）| 时间演化 + 重投影撞 M11.1 统计口径 |

**建议路径**：B（Gerstner + SSR 反射 + 折射吸收）一步两用、复用最多、不碰 GI 基线 → 日后反射要更真再升 C（平面反射）或 D（FFT 海洋）。

## 6. 依赖顺序铁律

**反射基础设施必须先于水体落地**：
- SSR 已排 S7 后插、预研齐、T0 基建就绪 → 水体首版反射直接吃 SSR，免平面反射 pass。
- 若用户坚持平面反射（C），须先单独立平面反射里程碑（UE `PlanarReflectionRendering` / ryzomcore 参照），否则水体反射半边返工。
- 折射的「无水体场景」纹理复用 T0 `hdrSceneColor_` 思路，但须深度预渲分类（UE `RefractionMask` 同构）。

## 7. 硬约束核对（不新增偏离）

1. 自研边界闸：只能自研，UE 只读不抄，bevy/Carrot 作阶梯。
2. `sky.slangh` 是 M10.1/M11.1 探针 miss 与 `--sky-bg` 公共函数，M11.1 已实锤「冻结基线上的代码形态也是基线」——水体反射若改天空采样路径须单独授权重拍。
3. Q4 冻结不新增独立 GBuffer 纹理/MRT/落盘 UAV；T0 已落 4 张纹理 + 共享 post-chain。波形顶点位移/折射 RT 须拍板归属（建议走 T0 同款离屏 RT，不进 GBuffer）。
4. M11.1 Q11-1 统计口径：带时间演化（FFT/动画波形）+ 时域重投影的效果，bytes 基线大概率不保，须固定步长/固定帧数或统计断言。**首版 Gerstner 若用固定时间步进可保 bytes；FFT（D）必走统计**。
5. 主线在 S11 收尾、`kStage` 仍 S8.2 → 水体只能作穿插项（不占 m0.* 编号、tag 用名字、不挡主线）或扩 S14。

## 8. 待拍板（未替用户定）

- `Q-WA1` 立项归属：穿插项 / S14 第六分支 / 独立 W 阶段 / A 类轨道。
- `Q-WA2` 波形：Gerstner（B，推荐）/ FFT（D，自实现 compute FFT）。
- `Q-WA3` 反射：SSR（复用，推荐）/ 平面反射（须先立平面反射里程碑）/ cubemap IBL（最廉价、静态）。
- `Q-WA4` 折射：首版做不做（Beer-Lambert 吸收 + 扭曲 UV，建议做；水下体积合成留进阶）。
- `Q-WA5` 冻结基线重拍授权（若水体反射改 `sky.slangh` 采样路径）。
- `Q-WA6` 是否引入第三方（建议零 vendored，只读算法；kentril0 的 FFTW / deiss 的 GPL 均禁）。

拍板后须写回 ROADMAP/S14-PLAN + 新建任务书（如 `docs/w*/W*-PLAN.md`）+ 全仓口径同步 + SHADER-CONVENTIONS 新节，才允许落第一行码。编号用 `Q-WA*` 与同日 weather 的 `Q-WE*`、worldgen 的 `Q-W*` 区分，勿混。

## 9. 与体积云/天气/夜景的关系

- **体积云**：独立里程碑，只在 UE 有；时间演化撞 M11.1 统计口径。不依赖水体。
- **天气**：时变参数喂 GI，禁止贴皮，验收含 GI 侧证据。水体可消费天气的「降雨→水面涟漪/粗糙度」参数（进阶）。
- **夜景灯光**（同日新增候选）：= 自发光 + bloom + 体积光/光柱 + 夜间雾 + 夜空 + 点面光源 GI 的组合场景；水体在夜景下 = 反射霓虹/bloom + 夜间吸收，是夜景验收的反射面。三者（水体/体积云/夜景）在「反射/体积 RT 交叉处理」上可能共用基建，但立项各自独立。
