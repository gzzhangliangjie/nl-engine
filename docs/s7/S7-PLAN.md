# S7 详细开发路线图：Visibility Buffer + 材质解析 + PBR 直光

> 对应总路线 `docs/ROADMAP.md` 的 S7。预计 3 个里程碑、业余时间 1~1.5 周。
> 告别 GBuffer：光栅 pass 只写一个 64 位值（簇 id + 三角 id + 深度），材质在第二 pass 反解。**64 位原子是本步的硬前提**（S0 验过 feature），本步是它的兑现时刻。
>
> **2026-11-09 预研轮**：三处参考已按对照总表全部读过（Carrot `VisibilityBuffer.cpp/.h`、bevy `visibility_buffer_hardware_raster.wesl` / `visibility_buffer_resolve.wesl` / `material_shade_nodes.rs` / `meshlet_mesh_material.wesl`、UE `NaniteShading.cpp` / `NaniteDataDecode.ush` / `NaniteWritePixel.ush` / `NaniteVertexFactory.ush` 定向调研），结论固化在本文件，开工前不再重复调研。

---

## 预研结论（2026-11-09，三处对照）

### 1. 打包格式与原子方向

| 来源 | 格式 | 原子 | 说明 |
|---|---|---|---|
| UE 5.8.4 | R64_UINT（不支持退 R32G32_UINT）：高 32 = `asfloat(DeviceZ)`，低 32 = `(VisibleClusterIndex+1) << 8 \| TriIndex(8b)` | 64 位 `ImageInterlockedMaxUInt64`（`NaniteWritePixel.ush` 20-35 行） | **reversed-Z**（near=1）下位图 max = 最近者胜；cluster 存的是可见簇列表下标且 +1 使 0=空像素 |
| bevy | u64 = `(bitcast<u32>(z) << 32) \| (cluster_id << 7 \| triangle_id)` | `textureAtomicMax`（`visibility_buffer_hardware_raster.wesl` 59-65 行） | **带硬件深度测试的簇光栅**（vis 原子是辅助记录）；triId 仅 7 位 = 簇内 ≤128 三角 |
| Carrot | R64Uint 存储图（RenderGraph storage target），clear 用独立 compute pass | 由 raster pipeline 写入 | CPU 侧只管 pass 编排（clear → rasterize → debug → material），打包细节在 pipeline shader |

**定稿（Option A）**：
- 布局：`u64 = (u64(asfloat(z)) << 32) | (clusterId(24) | triId(8))`，clusterId 用全局簇 id。
- 原子：**atomicMin**（本引擎维持标准 Z，near=0 → `asfloat` 位图单调递增，min = 最近者胜）。深度方向与原子方向的对应关系写 assert，实现时再核一遍。
- clear：vis buffer 清 `0xFFFFFFFFFFFFFFFF`，任意真实写入即胜（不像 UE 需 cluster+1 空 sentinel）。
- 位运算打包/解包宏集中定义在 shader 公共头（`SHADER-CONVENTIONS.md` 同款纪律）。
- **介质（2026-11-09 开工实测后修订，用户拍板）**：~~纹理格式优先 R64UINT，验证层报错再降级 R32G32_UINT~~ **作废**。vendored NVRHI 全树零 R64 格式（`Format` 枚举、`dxgi-format.cpp`、`vulkan-constants.cpp` 三处实查），UE/bevy 的 R64_UINT image atomic 在当前依赖上根本创建不出来。改走 `RWStructuredBuffer<uint64_t>` 线性 buffer（宽×高）+ **buffer** 64 位原子，寻址函数 `visIndex()` 是二维像素压一维下标的唯一入口。打包/clear/原子方向全部不变，偏离登记 `docs/s5/UE-DIFFS.md` D15。
- 由此带出两处能力补齐：DXIL 必须 `-profile sm_6_6`（CMake 只给 visbuf 四个新产物加，老产物一个字节不动）；Vulkan `vkCreateDevice` 必须启用 `VK_KHR_shader_atomic_int64` + `VkPhysicalDeviceShaderAtomicInt64Features.shaderBufferInt64Atomics`，否则 SPIR-V 能编出来、运行时被拒。
- triId 8 位 = 上限 256 三角/簇：实现时对 `.nmesh` 簇三角数 assert ≤256（meshopt 上限 128，余量充足）；clusterId 24 位同理断言簇数 < 2^24。

### 2. barycentrics：不存，解析重构（三处口径完全一致）

UE 显式 `#error` 禁用 SV_Barycentrics（`NaniteVertexFactory.ush` 1235-1237 行），vis buffer 只 64 位；bevy 用 The-Forge 技巧从「像素坐标 + pulled 三顶点」解析算 barycentrics 和 ddx/ddy（`visibility_buffer_resolve.wesl::compute_partial_derivatives` 39-82 行）。**原计划「barycentrics 另存 R16G16 pass」条目作废**——省一张 RT 的带宽与写路径，代价只是材质 pass 每像素多一次 3 顶点 pull（反解本来就要 pull）。

bevy 的 `compute_partial_derivatives` 可整体移植（Slang 语法几乎兼容），它同时给出：
- perspective-correct barycentrics（M7.1 老坑自动解决）；
- `ddx/ddy` uv 与 world position 导数——**材质 pass 里贴图采样必须用它**：全屏三角形绘制下相邻像素可能属于不同三角形，硬件四边形导数不可靠（UE 为此区分 quad 模式/像素模式；bevy 统一解析式）。

### 3. 材质 pass 执行模型（拍板点 D-S7-1，三选一）

| 方案 | 机制 | 优点 | 代价 |
|---|---|---|---|
| **A. Carrot 式单全屏 pass** | 一个全屏 quad draw，每像素读 vis buffer → 反解材质，材质数据走间接表（`material-pass` pipeline，`VisibilityBuffer.cpp` 160-193 行） | 实现最短；一种 pipeline；无 binning 基建 | 每像素都要分支取材质；无按材质批处理优化空间 |
| **B. bevy 式每材质全屏三角形** | 每材质一个 draw（`material_id*3` 顶点段，顶点着色器把 `material_id/65535` 当深度输出），配合**材质深度缓冲**做 depth test 当掩码（`material_shade_nodes.rs` 98-110 行、`meshlet_mesh_material.wesl` 11-18 行） | 按材质切 pipeline/bindgroup 自然；硬件深度剔除非本材质像素 | 需要 raster pass 额外把材质编码进深度（多一张 depth 流）；draw 数 = 材质数 |
| **C. UE 式 shading bin 压缩像素列表** | 光栅导出 ShadingMask → binning compute（8x8 tile，quad/pixel 双模式）→ 每材质一条 indirect dispatch 扫压缩像素列表（`NaniteShading.cpp` ShadeBinning 1958 行起） | 工业终态，只 shade 被覆盖像素 | 复杂度高一个量级，S7 阶段过度设计 |

**建议**：S7 取 **A**（单全屏 pass），材质数少时与 B/C 无可见性能差异，且与 §2.7 单 BindingSet 纪律最合拍；ddx/ddy 走 §2 的解析式。升级路径 = C（Binning 留到 S13 或材质数暴涨时再评估），在 UE-DIFFS 里登记为「计划内待办」。

### 4. 其余口径

- **深度**：材质 pass 不写深度（UE 由 `NaniteDepthExport` 独立重建；我们主光栅已有深度），`SvPosition.z` 需要时从 vis buffer 高 32 位 `asfloat` 恢复（UE `NaniteVertexFactory.ush` 1197 行同款）。
- **pass 编排顺序**（Carrot 实证）：clear vis buffer → 簇光栅写 vis → （可选 debug 视图）→ 材质 pass 输出颜色/GBuffer。与现有 S4 光栅 pass 的关系：光栅 pass 输出改写为 vis 打包值，深度照旧。
- **材质间接表**：cluster → 材质实例 → 纹理索引；查找函数集中一个头文件（原计划保留，S9/S10/S13 复用）。Slang 侧用 bindless 数组，绑进 §2.7 单一 BindingSet。
- **PBR 直光**：直接复用穿插项 `s0/PBR-DIRECT-PLAN.md` 的 shader 分支（M0.8 机制），S7 只剩 vis buffer + 反解 + 间接表。

---

## M7.0 vis buffer 打包写入（2026-11-09 收官，tag `s7-visbuf`）

- [x] 定稿打包格式（预研 §1 Option A）：`clusterId(24) | triId(8) | asfloat(z)` 的 64 位布局，打包/解包/hash/寻址集中在 `renderer/shaders/visbuf_common.slangh`（S9/S10/S13 复用）
- [x] 簇光栅 pass 输出改写 vis buffer：`InterlockedMin` 打包值；介质改 `RWStructuredBuffer<uint64_t>`；clear 用 `cmd->clearBufferUInt(visBuffer_, 0xFFFFFFFFu)`——每个 u32 全 1 即每个 u64 = VIS_EMPTY，**不需要独立 compute clear pass**
- [x] `--mode visbuf` 可视化：clusterId→颜色 / 全局三角 id→颜色 两档（Carrot 的 debug pass，M7.1 在此换成 material pass）

**自检（实测全过）**：簇/三角颜色分明、边缘干净无撕裂；双后端三对 PNG 逐字节相同；`deviceErrors=0`。额外做了硬对拍——visbuf 簇色档 vs 传统 `--mode cull --frustum off --shading 4`，前景 **64262 px 对 64262 px，轮廓差 0、颜色差 0、最大通道差 0**（`example/s7-visbuf/verify.py`），一次性证明反解与遮挡关系正确。

**落地实录**（介质偏离的完整推理与证据）：
1. 开工前用 slangc 实测：DXIL 默认 profile 下 64 位原子报 `opcode '64-bit atomic operations' should only be used in Shader Model 6.6+`；加 `-profile sm_6_6` 后纹理/buffer/PS 三路全通过。SPIR-V 直通（生成 `OpCapability Int64` + `Int64Atomics`）。
2. vendored NVRHI 三处实查零 R64 格式 → R64UINT 纹理路径判死；本机双后端实跑 `shaderInt64Atomics=1`（DX12 另补 Options11 的 `AtomicInt64OnDescriptorHeapResourceSupported`，Options9 两位只管 typed resource / group shared）。
3. `model.slang` 一个字节未动：visbuf 四个入口全收进独立 `visbuf.slang`，老 shader 产物与 17 对 exe=current 的历史渲染结果零扰动（spot 回归 m0.9/s2.0/s3.2 三对 bytes 全绿）。
4. 三段式 pass 编排（Carrot 口径）：`clearBufferUInt` → 簇光栅（VS 输出 nointerpolation clusterId/triInCluster，PS atomicMin 写 u0）→ 全屏 viz（读同块 buffer 的 t0 SRV 视角解包输出）。两条管线各一个 BindingSet（§2.7）。

⚠️ 常见坑（预研 + 落地更新）：
- atomicMin 比较的是**打包后的 u64 整数**，深度必须在**高 32 位**才能按整型序主导比较（IEEE 位图单调性只对 [0,1] 正浮点成立，负数/NaN 会破坏序——clear=max 兜底）
- ~~NVRHI 纹理格式名 `Format::R64UINT` / `imageAtomicMin`~~ **不存在**：vendored NVRHI 无 R64 格式；实际用 `InterlockedMin(RWStructuredBuffer<uint64_t>[idx], v)`，对应 SPIR-V buffer 64 位原子 + DXIL SM6.6 64-bit atomics。SPIR-V 需 `Int64Atomics` capability、设备需 `shaderBufferInt64Atomics`；**查询 ≠ 启用**，`reportFeatures()` 只查、`vkCreateDevice` 才启用（本项目首版就踩了这个「只查不启」的坑）
- `clearBufferUInt` 在 validation 层要求命令列表类型 ≤ Compute，graphics 列表（Graphics=0）满足；Vulkan 实现内部 `endRenderPass()`，故必须放在首个 `setGraphicsState` 之前
- u64 buffer 同时有 structured UAV（stride 8）与 DX12 自动建的 R32_UINT typed clear UAV，两种视图并存合法；`canHaveUAVs=true` + `initialState=CopyDest` + `keepInitialState=true`，帧内由 NVRHI 动态跟踪晋升 CopyDest→UAV→ShaderResource

## M7.1 材质 pass：vertex pulling 反解（2026-11-09 收官，tag `s7-pulling`）

- [x] 单全屏 pass（拍板点 D-S7-1 默认方案 A）：读 vis buffer → clusterId 查簇 → 三角 id 查索引 → **vertex pulling**（fetch 三顶点）
- [x] 移植 bevy `compute_partial_derivatives`：解析 barycentrics + ddx/ddy → perspective-correct 插值 pos/normal/uv，贴图采样显式传导数（`textureGrad`/`sampleGrad`）
- [x] 材质 ID 间接表：`clusterMeta[3]` 的 materialId 槽已接线（单材质恒 0，M7.2 走 bindless 间接表）
- [x] PBR 直光（mode 5 复刻 pbr-direct 的 Cook-Torrance，SampleGrad 吃解析导数）；深度重建暂未用于着色（单材质不透明模型，硬件深度已保证遮挡）

**自检（实测）**：silhouette 与 M0.4 传统画法**严格一致（64203 px 对 64203 px，轮廓差 0）**；颜色为**等价而非逐位**——属性交付路径本质不同（解析导数+手工透视插值 vs 硬件 quad 插值+隐式 mip），实测 73.2% 像素逐位相同、99.71% 差 ≤2、孤立尾峰 15、均值偏移 0.019（无系统性光照错配）；pulling 双后端 near（23 px ±1、maxDelta=1）；normal-reference 双后端 bytes。验收口径偏离计划原话「逐像素一致」已如实落档（`example/s7-pulling/verify.py` 阈值与理由）。tag：`s7-pulling`。

**落地实录**：
1. `material.slang` 六入口（vsMatResolve 全屏三角形 + psMatResolve），mode 0 逐字复刻 `model.slang::shade` 的 mode 0（`albedo*2.2 × (w+(1-w)·|ndl|)`）；CMake 沿用 `_VISBUF_COMMON` 依赖链。
2. `--mode pulling` 全链穿线：`ViewMode::Pulling`(10) → `Renderer::create(pullingMode)` → `initVisbuf(..., pulling)`——同一 Scene 复用 vis 资源，追加从 `.nmesh` 同名推导 `.glb` 加载真贴图（baseColor+MR）+ `LightConstants`(80B, b1) + `MatResolveConstants`(16B, b2)。
3. 空像素 `discard`：M0.4 清屏色透出，与 viz 档背景行为一致。

⚠️ 常见坑（落地更新）：
- bevy `ddy *= -1.0` 翻转**不要带过来**：NVRHI 的 `SV_Position.xy` 已是像素坐标系（y-down），与屏幕空间 barycentrics 同一直角系，直接用 `fragPos.xy`（已进 `docs/s5/UE-DIFFS.md` D16）
- 全屏 pass 的纹理采样**必须 SampleGrad**：`ddx(uv)` 在无映射导数的全屏三角形上是未定义/错误方向，bevy 同款坑
- pulling 三顶点从 SRV 逐顶点 fetch（8 float/顶点：pos/normal/uv），不复制顶点缓冲

## M7.2 多材质 + 对照验收（1 天）

- [ ] 多材质模型（≥3 种材质/贴图）跑通间接表
- [ ] 传统 GBuffer 直光路径保留为对照模式

**自检（S7 验收）**：vis buffer 路径 vs 传统直画 GBuffer 对照组**逐像素一致**（模式开关对比，差异截图为证）。tag：`s7-done`，截图 `docs/screenshots/s7.png`（visbuf 调试图 + 一致性对比）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M7.0 vis buffer 打包 | 1~2 天 | `s7-visbuf` |
| M7.1 反解+PBR | 2 天 | `s7-pulling` |
| M7.2 多材质+验收 | 1 天 | `s7-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 64 位打包/atomicMin 设计 | 本文件预研 §1；`bevy/.../visibility_buffer_hardware_raster.wesl` 59-65 行；UE `NaniteWritePixel.ush` 20-96 行 |
| barycentrics/导数解析 | bevy `visibility_buffer_resolve.wesl::compute_partial_derivatives` 39-82 行（The-Forge 出处注释在 37 行） |
| 材质 pass 编排 | Carrot `render/VisibilityBuffer.cpp`（pass 编排全貌）；bevy `material_shade_nodes.rs`（每材质三角形调度）；UE `NaniteShading.cpp::DispatchBasePass`（binning 工业版，升级路径） |
| 概念与坑 | Carrot《Recreating Nanite: A basic material pass》 |

## 底线

- 从本步起**所有着色走 vis buffer**，传统路径仅作对照保留——两条并存会越养越胖
- 材质间接表的查找函数集中一个头文件：S9/S10/S13 都要复用
- vis buffer **不存 barycentrics、不存材质 id**：64 位只装 簇/三角/深度；一切插值数据反解时按需 pull（三处口径一致，偏离需走 UE-DIFFS 留痕）
