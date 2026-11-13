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

## 3. 拍板记录（2026-11-13 用户「全部按推荐」拍板，开工闸已清零）

### Q-A1 分流演进路线 → **A1-A（拍板）**
M14A1 全软光栅对照 → M14A2 才加分流。理由：先拿正确性基线（软光栅 vs 硬光栅逐像素对拍），分流只是列表切分，风险后置；bevy 也是先有 SW pass 再接分流。

### Q-A2 分流判据（M14A2 用）→ **A2-A（拍板）**
簇投影 AABB ≤ 阈值 px（bevy 实测口径 64×64，本项目阈值 ImGui 可调、默认 64×64 待 profile 复核）。

### Q-A3 同深度竞争口径 → **A3-A（拍板）**
M14A1/M14A2 验收对比脚本**排除同深度像素**（硬件光栅同深度像素胜者也不确定，无 bit-exact 期望），单独输出「同深度像素率」统计留档。

**冻结面解除**：M14A1 允许落码 `soft_raster.slang`、`ViewMode::SoftRaster` 追加、renderer 接线、CMake 登记；主树/master 仍一行不碰。

## 4. M14A2 验收记录（2026-11-13，SW/HW 分流）

**实现**：`classify.slang`（csClassify 64 线程分类 + csArgsPatch 1 线程回填
argsHard[1]=hardCount）；判据 = 2×球投影半径方块 ≤ thresholdPx（bevy cull_clusters
64px 口径，球投影 AABB 近似，偏差已注明）；帧路径 = classify → HW 硬簇
（visRasterHardSet_ + argsHard_）→ SW 软簇（csSoftRaster split 分支）——两路写
同一块 visBuffer_（u64 atomicMin 合并，UE「vis 编码统一」口径）。阈值
`--sw-threshold <px>`（默认 64，clamp 8..256）+ HUD 滑条（soft-raster + --sw-split
才 show，既有 --hud 基线零布局变化）。clusterSpheres_ 去 frustum 门控恒创建。

**验收数值（DX12 实测，VULKAN 分流模式 deviceErrors=0 同跑通）**：

| 场景 | 对比 | 差异率 | 结论 |
|---|---|---|---|
| 默认机位 th=64 | split vs A1 全软 | **0.000%** | 全簇 ≤64px → 全软，与 A1 逐位一致 |
| 默认机位 th=8 | split vs A1 全软 / vs 全硬件 | 1.359% / 2.048% | 真混合（部分簇 >8px 走硬路） |
| 近机位 dist1.2 th=8/64 | split vs 全硬件 | 1.629% | 全硬（近处簇全 >64px，两档同侧）；差异 = 簇边界同深度竞争像素（Q-A3 口径，atomic 顺序敏感） |
| 远机位 dist18 th=64 | split vs 全硬件 | **0.513%** | 亚像素域 SW/HW 边缘规则差（Q-A3 口径） |
| dist18 + 0.25°/帧 ×24 帧 | HW 连续帧差异率 vs SW | **0.205% vs 0.128%（1.59x）** | **远景闪烁证据**：同机位同转速下 SW 分流域帧间抖动低 37% |

**踩坑**：① counts.z（split 旗标）CPU 清零时必须随帧写入——原子只累加 x/y，旗标
没人写 → SW 恒全画（首测 th8/th64 输出逐位相同的根因）；② NVRHI BindingSet
类型逐槽（M14B1 教训复用）；③ --screenshot-seq 模式主循环不限帧数（1699 帧
SIGTERM 掐停），长序列用后台 + 轮询够数即停。

**待办（M14A3 收官）**：example 卡（s14a-split）+ cases.json + tag `s14a-correct`/
`s14-softrast` + profile 定默认阈值 + 帧时间对比。

## 5. M14A3 验收记录（2026-11-13 收官，tag `s14-softrast`）

### 5.1 profile 矩阵（DX12，128 帧，--pass-timing 首解锚 + NVRHI GPU 帧 avg）

**默认机位**（960x540，yaw 12 / pitch 9 / dist 7）：

| 配置 | visRaster | softRaster | classify | GPU 帧 avg ms |
|---|---|---|---|---|
| visbuf 全硬件 | 0.0174 | — | — | **0.117** |
| soft-raster 全软 | — | 0.0297 | — | 0.132 |
| split th=8 | 0.0010 | 0.0061 | 0.0051 | 0.118 |
| split th=32 | 0.0010 | 0.0123 | 0.0051 | 0.127 |
| **split th=64（默认）** | 0.0010 | 0.0123 | 0.0051 | **0.125** |
| split th=128 | 0.0010 | 0.0276 | 0.0051 | 0.140 |
| split th=256 | 0.0010 | 0.0276 | 0.0041 | 0.145 |

**远景机位**（dist 18，A2 闪烁域抽查）：

| 配置 | GPU 帧 avg ms |
|---|---|
| visbuf 全硬件 | 0.162 |
| split th=8 | 0.119（**-26%** vs 全硬） |
| **split th=64（默认）** | 0.124（**-24%** vs 全硬） |
| soft-raster 全软 | 0.120 |

### 5.2 默认阈值结论：**64 维持不变**（profile 复核通过，零代码改动）

- th ≥ 128：帧成本逼近全软（0.140~0.145 vs 0.132），且无质量增益证据（>64px 簇三角形已足够大，SW 无闪烁优势）；
- th = 8：丢掉 8~64px 簇的防闪烁收益（A2 实测 SW 域抖动低 37%）；
- th = 64：默认机位成本 ≈ 全硬 +7%（0.125 vs 0.117）、远景**比全硬省 24%**（0.124 vs 0.162）、全簇防闪烁收益、bevy cull_clusters 同口径 —— 四项全绿；
- classify pass 恒 0.005 ms 量级，分流开关零负担。

### 5.3 渲染版交代（A1/A2 欠账一并补齐）

- `example/s14a-raster/`：全软基线双后端 PNG + json 侧车 + run.bat verify（bytes 门 + 负向）；cases.json 第 119/120 卡之 s14a-raster（pair soft + manual soft-verify）。
- `example/s14a-split/`：分流基线（--sw-split 默认 th64）双后端 PNG + json 侧车 + run.bat verify（bytes 门 + **blend-visible 门**（th8 重拍必须异于 th64 基线 = 分流真实生效）+ 负向）；cases.json s14a-split（pair split + manual split-verify）。
- 确定性口径：vis buffer u64 `atomicMin` 写入序无关（min 可交换）+ 链路无 RNG → 同后端重拍逐字节确定，bytes 门合法（与 restir 的随机路径统计门形成对照）。
- regression `--only s14a-raster` / `--only s14a-split` 双后端各 2/2 bytes ALL GREEN。kStage 冻结 S8.2 不 bump。

### 5.4 收官账

- tag `s14-softrast`（M14A0~A3 整轨）；MILESTONES A0/A1/A2/A3 四栏补账（A0~A2 欠账一并）。
- 遗留（不阻塞收官）：同深度竞争像素无 bit-exact 期望（Q-A3 拍板口径，已留档）；subgroupAny 宽三角分路（bevy 有、本机换 128 线程常驻口径）如需再评估挂独立项。
