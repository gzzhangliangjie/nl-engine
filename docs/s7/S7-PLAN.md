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
- clear：`R64UINT` 清 `0xFFFFFFFFFFFFFFFF`，任意真实写入即胜（不像 UE 需 cluster+1 空 sentinel）。
- 位运算打包/解包宏集中定义在 shader 公共头（`SHADER-CONVENTIONS.md` 同款纪律）。
- 纹理格式优先 R64UINT，验证层报错再降级 R32G32_UINT（两条 32 位原子分两次写，顺序深度字在前——UE 同款降级路径）。
- triId 8 位 = 上限 256 三角/簇：实现时对 `.nmesh` 簇三角数 assert ≤256（meshopt 上限 128，余量充足）。

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

## M7.0 vis buffer 打包写入（1~2 天）

- [ ] 定稿打包格式（预研 §1 Option A）：`clusterId(24) | triId(8) | asfloat(z)` 的 64 位布局，打包/解包宏集中定义
- [ ] 簇光栅 pass 输出改写 vis buffer：`atomicMin` 打包值；R64UINT 创建失败降级 R32G32_UINT；clear 至 max（独立 compute clear 或 `setClearValue`）
- [ ] `--mode visbuf` 可视化：clusterId→颜色 / triangleId→颜色 两档

**自检**：visbuf 调试图簇/三角颜色分明、边缘干净无撕裂；遮挡关系正确（atomicMin 生效，边缘即最近三角）。tag：`s7-visbuf`。

⚠️ 常见坑（预研更新）：
- atomicMin 比较的是**打包后的 u64 整数**，深度必须在**高 32 位**才能按整型序主导比较（IEEE 位图单调性只对 [0,1] 正浮点成立，负数/NaN 会破坏序——clear=max 兜底）
- NVRHI 纹理格式名 `Format::R64UINT`；64 位 image atomic 需 Slang `imageAtomicMin` 对应 SPIR-V `ImageAtomicMin`/DXIL 64-bit atomics（S0 验过 feature，本步兑现）

## M7.1 材质 pass：vertex pulling 反解（2 天）

- [ ] 单全屏 pass（拍板点 D-S7-1 默认方案 A）：读 vis buffer → clusterId 查簇 → 三角 id 查索引 → **vertex pulling**（fetch 三顶点）
- [ ] 移植 bevy `compute_partial_derivatives`：解析 barycentrics + ddx/ddy → perspective-correct 插值 pos/normal/uv，贴图采样显式传导数（`textureGrad`/`sampleGrad`）
- [ ] 材质 ID 间接表：cluster → 材质实例 → 纹理索引，bindless 数组、§2.7 单 BindingSet
- [ ] PBR 直光（1~2 盏方向光，复用 PBR-DIRECT-PLAN 分支）+ 深度测试用重建深度（vis 高 32 位恢复）

**自检**：单材质模型与 M0.4 传统画法**逐像素一致**（先保证单材质对，再上多材质）。tag：`s7-pulling`。

⚠️ 常见坑（预研更新）：
- ~~barycentrics 另存 RT~~（作废，见预研 §2）
- 解析导数函数移植时注意 Y 方向：bevy（wgpu）`ddy *= -1.0` 的翻转与 Vulkan/DX12 NDC 约定不同，移植后用「贴图梯度可视化」用例验一次
- vertex pulling 的顶点数据用 SRV 逐顶点 fetch（.nmesh 顶点布局），别复制顶点

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
