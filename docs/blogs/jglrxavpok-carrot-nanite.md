# jglrxavpok 博客 × Carrot 源码：Recreating Nanite 全系列逐篇实录

> 来源：jglrxavpok《Recreating Nanite》9 篇 + 《We have GI at home》，逐篇对照本机 Carrot 源码（`E:\UE_STUDY_2026\Carrot`，已核实的落点见各篇"源码落点"）。
> 用途：S1~S5（虚拟几何）、S7/S8（RT 接口）、S10~S12（GI）的编年史教材。原博客见 `docs/LINKS.md`；git 提交时间线另见 `docs/carrot-history.md`（本文是内容级，那边是提交级）。
> 整理日期 2026-11-07。注意：博客发布后 Carrot 仓库有目录重组，下表"源码落点"已按当前仓库实况核对。

## 源头说明与链接总表

**博客原文**（blog.jglrxavpok.eu，全部逐篇核对于 2026-11-07）：

| 篇 | 原文链接 |
|----|---------|
| The Plan | <https://blog.jglrxavpok.eu/2023/11/12/recreating-nanite-the-plan.html> |
| Visibility buffer | <https://blog.jglrxavpok.eu/2023/11/26/recreating-nanite-visibility-buffer.html> |
| Cluster rendering | <https://blog.jglrxavpok.eu/2023/12/07/recreating-nanite-cluster-rendering.html> |
| LOD generation | <https://blog.jglrxavpok.eu/2024/01/19/recreating-nanite-lod-generation.html> |
| A basic material pass | <https://blog.jglrxavpok.eu/2024/02/06/recreating-nanite-material-pass.html> |
| faster, better, simpler | <https://blog.jglrxavpok.eu/2024/03/12/recreating-nanite-faster-lod-generation.html> |
| Runtime LOD selection | <https://blog.jglrxavpok.eu/2024/04/02/recreating-nanite-runtime-lod-selection.html> |
| Mesh shader time | <https://blog.jglrxavpok.eu/2024/05/13/recreating-nanite-mesh-shader-time.html> |
| Raytracing | <https://blog.jglrxavpok.eu/2024/08/21/recreating-nanite-raytracing.html> |
| We have GI at home | <https://blog.jglrxavpok.eu/2025/03/18/gi-at-home.html> |
| Carrot 引擎起点 | <https://blog.jglrxavpok.eu/2023/11/08/carrot-engine.html> |
| Linux 移植 | <https://blog.jglrxavpok.eu/2025/04/26/engine-linux-port.html> |

**源码**：
- 在线仓库：<https://github.com/jglrxavpok/Carrot>（提交链接格式 `https://github.com/jglrxavpok/Carrot/commit/<sha>`）
- 本机完整 clone：`E:\UE_STUDY_2026\Carrot`（含 `.git`，可直接 `git show <sha>` 对照博客提到的每个提交）

**关键提交速查**（博客正文引用过的）：

| sha | 内容 | 提及于 |
|-----|------|--------|
| [`737dbf72`](https://github.com/jglrxavpok/Carrot/commit/737dbf72dbae8d19b211d61a146af424a0856c3b) / [`22c8c808`](https://github.com/jglrxavpok/Carrot/commit/22c8c80829133ac2f5d2952653f2946a4c098056) | visbuffer pass（no-op）/ 填充 visbuffer | Visibility buffer |
| [`4c100761`](https://github.com/jglrxavpok/Carrot/commit/4c100761c442593c66b647c938adfec22fda33e6) / [`8db880c8`](https://github.com/jglrxavpok/Carrot/commit/8db880c80b4111cadbc15e8a46becf284574958f) | Meshlet support（+meshoptimizer 同提交）/ cluster lifetime | Cluster rendering |
| [`17cb961a`](https://github.com/jglrxavpok/Carrot/commit/17cb961a9ef6ed4c6a3afd4d2ab4143e3f31c2b0) / [`0c2b2921`](https://github.com/jglrxavpok/Carrot/commit/0c2b2921dd4bdd79438920ec3f7d35a10b1f1693) / [`8d221447`](https://github.com/jglrxavpok/Carrot/commit/8d22144765c374d65cf79c24e6b0beeed4b9b330) / [`f7d1be41`](https://github.com/jglrxavpok/Carrot/commit/f7d1be41eb69122cbe1f28a6de9ffd175d77ed2c) / [`83e77514`](https://github.com/jglrxavpok/Carrot/commit/83e7751428614da34420cd682191f00b520ce924) | glTF LOD / 第一版 LOD 簇 / 顶点焊接+边权 / k-d 树 / 每 LOD 重建 k-d 树 | LOD generation |
| [`8f8bfe22`](https://github.com/jglrxavpok/Carrot/commit/8f8bfe22c0a68cc55e74f04543c611f1120e06e5) / [`205a10cc`](https://github.com/jglrxavpok/Carrot/commit/205a10cc85808df30495802667b262b5206cb230) / [`ff8e5ae9`](https://github.com/jglrxavpok/Carrot/commit/ff8e5ae9c53f305956deb830f9db5cf377b91ad3) + [`816b25ff`](https://github.com/jglrxavpok/Carrot/commit/816b25ff60c6bf580afe51dd295ce114fadca6ad) | 小顶点缓冲简化 / 三项减负 / 边界顶点不焊接（含逻辑反了的重修） | faster, better, simpler |
| [`f1513dd3`](https://github.com/jglrxavpok/Carrot/commit/f1513dd3d9043ef957cad2d806f1958bcf97c0f8) / [`69d1413d`](https://github.com/jglrxavpok/Carrot/commit/69d1413d09465ea8e00982d64e894372b4e48af4) / [`08e501a8`](https://github.com/jglrxavpok/Carrot/commit/08e501a8704c658e20259c3832d3d10482464357) | 运行时 LOD 选择 v1 / 投影相机位置修复 / 包围球生成修复 | Runtime LOD selection |
| [`2643842`](https://github.com/jglrxavpok/Carrot/commit/2643842ebe938ce7a84b9ddf8eca3ca35f6dbb4e) / [`3891eed`](https://github.com/jglrxavpok/Carrot/commit/3891eed655942b81677dd098c616be229761c471) / [`9167d1c`](https://github.com/jglrxavpok/Carrot/commit/9167d1c24b188bec198f8b174e80df895b9bc0f4) | cluster instance buffer / Material pass v1 / motion vectors | Material pass |
| [`e7a88a0`](https://github.com/jglrxavpok/Carrot/commit/e7a88a0) / [`1fd0633`](https://github.com/jglrxavpok/Carrot/commit/1fd0633) / [`cb0c2a2`](https://github.com/jglrxavpok/Carrot/commit/cb0c2a2) / [`bff3763`](https://github.com/jglrxavpok/Carrot/commit/bff3763) | mesh shader 支持 / LOD 选择进 shader / task shader / 调试开关 | Mesh shader time |
| [`06547be`](https://github.com/jglrxavpok/Carrot/commit/06547be) | RT 组加载（文中源码链接的锚点提交） | Raytracing |

**他引参考**：[Karis Nanite PDF](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) · [Traverse Research：Creating a DAG from a Mesh](https://blog.traverseresearch.nl/creating-a-directed-acyclic-graph-from-a-mesh-1329e57286e5) · [AMD GI-1.0 论文](https://gpuopen.com/download/publications/GPUOpen2022_GI1_0.pdf) · [METIS](https://github.com/KarypisLab/METIS)（[作者 fork](https://github.com/jglrxavpok/METIS/)）· [meshoptimizer](https://github.com/zeux/meshoptimizer) · [Shadertoy 屏幕数字](https://www.shadertoy.com/view/4sBSWW/) · [Happy Buddha 模型](http://graphics.stanford.edu/data/3dscanrep/)

## 系列总览（12 篇时间线）

| # | 日期 | 篇 | 一句话 | 对应我们 |
|---|------|-----|--------|---------|
| 1 | 2023-11-12 | The Plan | 总规划：visbuffer → 建簇 → LOD → 选择 → 软光栅 | ROADMAP 全局 |
| 2 | 2023-11-26 | Visibility buffer | 64 位深度+ID atomicMax visbuffer | S5 前置 |
| 3 | 2023-12-07 | Cluster rendering | meshlet→cluster、无 VBO 绘制 | **S1** |
| 4 | 2024-01-19 | LOD generation | METIS 分组 + 锁边界简化 + 顶点焊接 | **S3** |
| 5 | 2024-02-06 | A basic material pass | visbuffer→GBuffer、重心坐标恢复 | S5 |
| 6 | 2024-03-12 | faster, better, simpler | LOD 生成 4min→40s + 边界不焊死 | **S3** |
| 7 | 2024-04-02 | Runtime LOD selection | 误差投影像素空间、CPU 遍历 cut | **S3/S4** |
| 8 | 2024-05-13 | Mesh shader time | task/mesh shader 化剔除，8.65→0.8ms | S4 对照 |
| 9 | 2024-08-21 | Raytracing | 组级 BLAS + 预计算 + 批建 | **S7/S8** |
| 10 | 2025-03-18 | We have GI at home | GI-1.0 两级辐射缓存 | **S10~S12** |

另有《Carrot engine》（引擎起点）与《engine-linux-port》《one-year-of-linux》两篇移植记。

## 1. The Plan（先看蓝图再动手）

- 对 Nanite 的理解：**LOD 簇 + 软光栅**；层级构建 = 每 ~128 三角形一簇 → 4 簇一组简化 → 再分裂成 2 簇（避免边界锁死全层级）→ 收敛到单簇。
- 计划六步：visibility buffer → cluster generation（先用 meshoptimizer，必要时 METIS）→ LOD generation → LOD selection（cut）→ 软光栅（maybe）→ 更远（细粒度遮挡）。
- 对我们的启示：这个顺序和 `docs/ROADMAP.md` 的 S1→S5 几乎同构，且他明确"软光栅 may be"——和 JMS55"软光栅放最后"互相印证。

## 2. Visibility buffer（64 位 atomicMax 深度测试）

- 格式 `r64ui` 存储图像；**高 32 位 = 深度、低 32 位 = ID**；写入 `imageAtomicMax`。
- **深度反转编码**：`depth = 0xFFFFFFFF - uint(ndc.z * 0xFFFFFFFF)`——近处值大，Max 天然保留最近三角形。`ndc.z < 0` discard。
- pipeline：无剔除无混合、**开 depth write/test**；先 compute 清零 + `vkMemoryBarrier2`（ComputeWrite→FragmentRead）再光栅。
- **踩坑**：初版直接 `gl_PrimitiveID` atomicMax 没编码深度 → 绘制顺序决定前后（"Depth testing is not working! 😱"）；U64 无法从 fragment 输出（GLSL 限制），只能经 image 原子写；U64 图不可直接看，**专门写了 debug pass 把低 32 位当索引用 25 色调色板上色**。
- 源码落点（现行仓库）：`engine/resources/pipelines/visibility-buffer.json`、`engine/resources/shaders/includes/visibility-buffer-common.glsl`。

## 3. Cluster rendering（meshlet → cluster，S1 最核心一篇）

- **meshlet vs cluster**：meshlet 只是原网格的子索引引用；**cluster 拥有独立顶点+索引数据**（GPU 地址直引）。运行时以 cluster 绘制。
- 生成参数：`meshopt_buildMeshlets`，maxVertices=64 / maxTriangles=128 / coneWeight=0（遮挡剔除预留）。
- shader 侧 `buffer_reference`：`Cluster { VertexBuffer vertices; IndexBuffer indices; uint8 triangleCount; }`，无 VBO/IBO，`gl_VertexIndex` 手动重索引。
- **cluster ID 传递**：比较 push constant / per-instance / per-draw 后选 **per-drawcall**（drawIndirect + dynamic offset 取 uuid0）。
- visbuffer 编码升级：低 32 位 = 高 8 位 instance ID（当时 TODO）+ 低 24 位 triangle index（+1，0 保留为空）。
- 资产：glTF 自定义扩展 `CARROT_meshlets`（3 个 accessor：meshlets 结构体 / 顶点索引 / 索引）。
- 调试技巧：把低 24 位换成 cluster ID 着色，一簇一色直观验簇。
- 源码落点：`asset_tools/fertilizer/models/GLTFWriter.cpp`（扩展写入）、`engine/engine/render/ClusterManager.cpp`（簇管理）。提交 `4c100761`（Meshlet support，**meshoptimizer + meshlet 同一个提交进的**，见 carrot-history 教训）、`8db880c`（cluster lifetime）。

## 4. LOD generation（S3 第一课：分组/简化/焊接）

五步循环：LOD0 建簇 → 连通性分组（每组 4）→ 组内拼索引 `meshopt_simplify(50%, SimplifyLockBorder)` → 再建簇（复用 LOD0 代码）→ 循环到 ≤1 簇（maxLOD=25 保险）。

- **METIS 参数**：`ncon=1`、`OBJTYPE_CUT`、`CCORDER=1`；meshlet 图边权=共享边数。
- **误差**：`targetError = 0.9*tLod + 0.01*(1-tLod)`（高 LOD 允许 1% 形变、低 LOD 放到 90%）。
- **顶点焊接**（faceted 网格必做）：空间距离 ≤(1%~10%)*simplifyScale 且 UV 距离 ≤(1px~半张纹理) 才允许合并；**每个 LOD 重建 k-d 树**（顶点跨 LOD 不稳定）。
- **踩坑**：METIS 存储大小错 → segfault 难查（`xadj.size()==n+1` 要 verify）；**焊接必须查 UV**（否则纹理搅乱），但 Damaged Helmet 面罩 UV 差太大焊接失败 → 低 LOD 空洞（作者怀疑"忽略 normal"的决定）；glTF 塞字节数组必须显式 `byteStride`。
- 源码落点：`asset_tools/fertilizer/models/ModelProcessing.cpp`（建簇/简化现行所在）。

## 5. faster, better, simpler（S3 第二课：4min→40s）

- **k-d 树建树 O(n log n) 化**：平衡树要中位数 → 全量排序贵；用**大数定律采样 512 个点取中位数近似**（Wikipedia k-d tree 建法）。
- **少给 meshoptimizer 干活**：简化器会遍历顶点缓冲全部顶点（含未用的）——**把组内用到的顶点 remap 进小缓冲再简化，完事 remap 回去**，单此一项大降耗时（作者原话 massively reduced）。
- 去重边/连接（vector→set）、退化三角形跳过、并行找顶点邻居。
- **修质量（fixing quality）**：第一版一边讲"边界要匹配"一边焊接时把边界顶点也焊了 → 裂缝逐级扩大。修法：`findBoundaryVertices`（只被一个 meshlet 使用的边 = 边界），**边界顶点禁止合并**。
- 结果：Happy Buddha 4min → 40s，LOD 终于"像原模型"。

## 6. Runtime LOD selection（S4 的误差度量同源篇）

- `meshSpaceError = simplificationError * simplifyScale`，**再累加子级最大误差**——保证误差随 LOD 单调递增、各簇独立可并行判定。
- cut 判定：`clusterError <= 阈值 && parentError > 阈值`（阈值默认 1 像素），投影公式与我们一致（`screenHeight/2 * cot(fov/2) * r / sqrt(d²-r²)`）。
- **同组簇必须共享 error/bounds**，否则簇缺失/重叠伪影。
- 实现为 CPU 每帧顺序遍历（作者自评 "probably not performant"）——下一篇就推翻重来了。
- **四个调试视图是他自称的必备品**：Triangle / Cluster / LOD / Projected screen error——`engine/resources/shaders/post-process/visibility-buffer-debug/` 下四个 shader 就是（triangles / clusters / lods / screen-error）。我们 S3/S4 的调试视图照此四件套验收。

## 7. Mesh shader time（CPU 遍历的 GPU 化，S4 对照篇）

- 动机：CPU 遍历 + 每帧分配太慢；compute+indirect 要按最坏分配 & 同步；mesh/task shader 天生"抽象对象→draw command"。
- 结构：**task shader**（LOD 剔除 + 视锥 6 平面，`atomicAdd` 攒活跃簇 ID 进 payload，invocation 0 一次 `EmitMeshTasksEXT`）→ **mesh shader**（每簇一 workgroup，`SetMeshOutputsEXT(0,0)` 提前剔除）。fragment 完全不用改。
- **occupancy 调优**：mesh workgroup 1→64、task 1→32 后，mesh/task 占用率变化不大，但 **fragment occupancy 涨 14 倍**——`drawMeshTasks` 8.65ms → 0.8ms；Bistro 约 80 FPS。
- **踩坑**：调试用三角形计数 feedback buffer（写 CPU 可见内存）导致 `vkCmdMeshDrawTasksEXT` 大量 PCIe-to-BAR 请求、占用率骤降——**调试设施本身要能用编译开关关掉**。

## 8. Raytracing（virtual geometry × RT，S7/S8 主参考）

- 难点：Nanite 模型没有整体 LOD，不能按相机建一把 BLAS。
- 方案：**以 group（簇组）为单位**——同组簇共享 LOD 决策，整组合并成一个 BLAS；GPU compute 算"appropriate groups"（与光栅同一判定、去掉视锥只看位置）→ readback 回 CPU（3 帧延迟）→ 建/拷 BLAS → TLAS。
- **BLAS 预计算进资产**：asset pipeline 里 `vkCmdCopyAccelerationStructureToMemoryKHR` 序列化，头部 32 字节 UUID 校验驱动兼容后直接 copy（构建慢但可移植 vs 拷贝极快但不可移植）。
- 性能工程（文章重点）：每帧限建/拷 1000 个 BLAS（Bistro 11k 组 11 帧就绪）；32 桶批量命令录制 + 多线程；WeakPool 锁竞争 40ms+ → 读写锁 + atomic counter + 稀疏数组 8ms。
- **踩坑**：per-cluster BLAS = 30k 个 BLAS 4GB+ 直接爆显存；asset pipeline 与 runtime 的 geometry index 不一致 → 阴影空洞（不崩但错）；`vkGetAccelerationStructureBuildSizesKHR` 无 batch、驱动内反复 stall。
- 源码落点：`engine/engine/render/ClusterManager.cpp`（组加载）、`engine/engine/render/raytracing/RaytracingScene.cpp`（AS 构建现行所在；博客写的 `ASBuilder.cpp` 已并入）、`asset_tools/fertilizer/models/ModelProcessing.cpp`（预计算）。

## 9. We have GI at home（S10~S12 参考）

- 动机：全光追光照噪、丑、贵 → 实现 AMD **GI-1.0**（GPUOpen 论文）的两级辐射缓存。
- **屏幕空间缓存**：8×8 像素 tile = 探针；GBuffer + motion vector 重投影；论文技巧——每帧只 spawn 1/4 探针（4 帧轮满），时间复用下不可见。
- **世界空间缓存**：量化(位置,方向) 做 key 的 **GPU hashmap**，查询与插入都原子；射线命中处查缓存+写回，跨帧累积。
- **光线偷取**：重投影失败的探针（disocclusion 区）从成功探针处"偷"射线预算——作者实现用顺序偷（随机偷会互相"吸血"）。
- pass 链：`reuse-gi`(vkCmdCopyBuffer) → `decay-gi`(60 帧未动即删) → `spawn-screen-probes` → `gi-reorder-rays` → `gi-spawn-rays` → `gi-trace-rays` → `gi`(插值+时空降噪)。
- **最有价值的忠告**：① 他的引擎内做了 **render graph 实时 buffer 可视化调试器**，"absolutely instrumental"——对应我们 S6+ 的调试视图投入；② 6 个月晚间工程，差点 burnout，靠"做无关小任务"续命——个人项目节奏管理的活教材。

## 10. 对我们路线的对照结论

| 维度 | Carrot | 我们 | 取舍 |
|------|--------|------|------|
| API/着色 | Vulkan + GLSL 单后端 | NVRHI + Slang 双后端 | 我们多一层抽象但验收更严 |
| visbuffer | r64ui 存储 image + imageAtomicMax | NVRHI 下同思路（format 支持 U64 atomic） | 直接可抄 |
| cluster 传递 | per-draw uuid0 | 待 S1 定（参考 per-draw） | — |
| LOD 选择 | CPU 遍历 → mesh shader | compute 两遍法（更贴 bevy/Nanite） | 不走 mesh shader |
| meshlet 进仓库 | 一个大提交 `4c100761` | 拆 M0.x 版本逐步进 | 已在 carrot-history 记教训 |
| 调试视图 | 4 件套 + render graph 实时可视化 | L0 截图 + example + 展示台，S3 起加四视图 | 四视图照搬 |
