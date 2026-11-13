# S14A 软光栅亚像素三角形计划（M14A0~M14A3）

> 轨道：`feat/s14-softrast`，wt `E:/UE_STUDY_2026/nl-engine-s14-softrast`；总纲 `docs/s14/M14-PLAN.md`（2026-11-13 拍板：A+B 并行 / C 接 B / D 砍 / kStage 冻结 S8.2）。
> 本文件 = M14A0 产出。§1 三处实读结论；§2 里程碑；§3 待拍板清单（**清零后才允许落码**，AGENTS.md 开工闸）。

---

## 1. 三处实读结论（2026-11-13，M14A0）

### bevy `visibility_buffer_software_raster.wesl`（189 行，全文精读）——**主参考**
- **分流判据在 cull 阶段**（`cull_clusters.wesl` 实读）：`project_aabb` 投影簇 AABB 到屏幕 → `aabb_size <= 64×64 px` 走软光栅；软簇写入 `meshlet_raster_clusters[prev_hw_count + atomicAdd(sw_args)]`（列表头段），硬簇写尾段（`rightmost_slot` 反向），两段共用一张 cluster 表；HW/SW 各自 indirect args。**软光栅 pass 吃 `meshlet_previous_raster_counts[0]`（HW 已画数量）偏移**。
- **软光栅本体**：1 簇 = 1 workgroup（128 线程），workgroup shared memory 存投影后顶点（`vec3(viewportXY, ndcZ)`，上限 256 顶点循环覆盖）；每线程 1 三角形：背面剔除（edge function 面积 ≤ 0）→ 梯度.setup（w_x/w_y 增量式）→ bbox；`subgroupAny(max_x-min_x > 4)` 分两路——宽三角走 scanline（边缘方程解 X 区间），窄三角走 bbox 全扫（避免分支分歧，WebGPU 无 quad control 的妥协）。
- **像素写入**：`packed_ids = (clusterId << 7) | triId`（bevy tri 7bit），`depth = bitcast<u32>(z)`，**`textureAtomicMax` 对 u64 整体**——bevy 深度约定 max=最近；本项目 deviceZ near=0 min=最近，映射到本项目 `visPack()` + `atomicMin` 语义直换（`visbuf_common.slangh` 唯一改动点语义不变）。
- **已知 TODO（照单避开）**：无定点数数学、无 top-left rule（同深度闪烁隐患，本项目 M14A2 数值对比要盯同深度像素抖动）；bbox 路径里「不应需要的 min3(w)>=0 补判」实测必要（照抄）。

### UE `NaniteRasterBinning.usf` + `NaniteRasterizationCommon.ush`（定向读）——**工程终态口径**
- **分流在 binning/candidates 层**：`bSoftware = RelativeClusterIndex < SWClusterCount`（binning 输入即分类好），软硬簇分属不同 raster group + 各自 depth bucket，`ExportRasterGroup(... bSoftware ...)` 分流导出；**vis buffer 编码统一**（`SOFTWARE_RASTER` 只影响 visualize 档位标签），resolve 侧完全无感。
- 对本项目的映射：分流决策放 **cull 之后的独立 compute**（不动 `cull.slang` 冻结形态——M11.1 冻结基线纪律），产出「软簇列表 + 硬簇列表」两段式，HW path（`model.slang` indirect draw）与 SW pass 各吃一段。
- UE 的 SWRaster 本体是曲面细分微三角路径（本项目不需要），只抄「分流层位 + 编码统一」两条思路。

### Carrot（据实标注）
`engine/render/` 全目录 grep：**无软光栅、无 vis buffer 等价**（Carrot 走传统 forward 渲染，ImGui/RenderPass 命中只是无关关键词）。A 轨 Carrot 无口径，节奏对照仅此一句。

### 项目现状（wt `feat/s14-softrast` 代码实读）
- vis buffer：`RWStructuredBuffer<uint64_t>` 线性 buffer + `atomicMin`（NVRHI 无 R64 纹理，UE-DIFFS 已留档）；`visPack = (asuint(deviceZ)<<32) | (cluster24|tri8)`，`VIS_EMPTY` 全 1 sentinel；`visIndex()` 唯一寻址入口。
- cull 输出：`g_visibleList`(u0) + `g_cullArgs`(u1)（`cull.slang`），indirect draw 全簇全画；模式枚举 `ViewMode`（`core/include/nl/core/modes.hpp`，Filter=20 后追加）。
- shader 清单已有 `visbuf.slang` / `visbuf_common.slangh` / `model.slang`（HW path），软光栅新增 `soft_raster.slang` 不碰这些文件的既有入口。

## 2. 里程碑

| # | 内容 | 验收 | tag |
|---|---|---|---|
| **M14A0** | 本计划落盘 + 待拍板清零 | §3 三问全拍板并写回 | `s14a-plan` |
| **M14A1** | 全软光栅对照模式：`--mode soft-raster` 整场景走 compute 软光栅（复用 `g_visibleList` 全量，不做分流），vis buffer 写入/resolve 全走既有链；bevy 1 簇 1 workgroup + 梯度增量式 + scanline/bbox 双路照搬（subgroup 换本机能力） | 双后端出图；`--mode visbuf` 与 `--mode soft-raster` 同机位 vis buffer 逐像素一致（或误差量化）；example 卡 + cases.json（语义合并格式） | `s14a-raster` |
| **M14A2** | SW/HW 分流：cull 后加分类 compute（簇投影 AABB ≤ 阈值 → 软列表，硬列表反向），HW indirect 改吃硬列表；阈值 ImGui 可调 | 小三角形覆盖正确（远景闪烁对比证据图）；全硬件版逐像素数值对比（差异像素率写进文档） | `s14a-correct` |
| **M14A3** | profile 定默认阈值/瓦片参数 + 帧时间对比 + 收官交代（渲染版全套） | `run.bat verify` + `regression.py --only` 双绿；MILESTONES 四栏 | `s14-softrast` |

**坑照单**（S14-PLAN 原文 + bevy 实读补充）：WebGPU 原子限制方案别照搬（本机 DX12/Vulkan 有真 64 位 buffer atomic，本项目已在用）；分箱尺寸先 profile 再定；同深度像素竞争胜者不确定（写对比脚本时排除/单独统计）。

## 3. 待拍板清单（M14A0 开工闸，清零才落码）

### Q-A1 分流演进路线
- **A1-A（推荐）**：M14A1 全软光栅对照 → M14A2 才加分流。理由：先拿正确性基线（软光栅 vs 硬光栅逐像素对拍），分流只是列表切分，风险后置；bevy 也是先有 SW pass 再接分流。
- A1-B：一步到位 M14A1 就带分流。省一轮回归，但正确性问题和分流问题耦合在一个里程碑，FAIL 时难归因。

### Q-A2 分流判据（M14A2 用，可先拍方向）
- **A2-A（推荐）**：簇投影 AABB ≤ 阈值 px（bevy 实测口径 64×64，本项目阈值 ImGui 可调、默认取 32×32 或 64×64 待 profile）。
- A2-B：簇三角形平均屏幕面积 < 1 px²（更贴「亚像素三角形」语义，但要逐簇遍历三角形，cull 侧成本高）。

### Q-A3 同深度竞争口径
- **A3-A（推荐）**：M14A1/M14A2 验收对比脚本**排除同深度像素**（硬件光栅同深度像素胜者也不确定，无 bit-exact 期望），单独输出「同深度像素率」统计留档。
- A3-B：加 top-left rule + 定点数（bevy TODO 项）追求 bit-exact——工程量大，S14-PLAN 验收「无明显差异」语义不需要。

**拍板前冻结面**：`soft_raster.slang`、`cull` 后分类 pass、`ViewMode` 追加、CMake shader 登记——一行不落；主树/master 一行不碰。
