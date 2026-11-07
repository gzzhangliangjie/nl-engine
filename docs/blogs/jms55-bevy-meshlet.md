# JMS55 博客 × bevy 源码：Bevy 虚拟几何（meshlet）开发实录

> 来源：JMS55《Virtual Geometry in Bevy 0.14 / 0.15 / 0.16》三篇 + bevy 主仓 `crates/bevy_pbr/src/meshlet/` 实码（本机探针克隆 `/tmp/bevy-meshlet-probe`，可对照）。
> 用途：S1（切簇）/ S3（LOD 层级）/ S4（剔除与可见性）的配套实录。原博客见 `docs/LINKS.md`。
> 本文 2026-11-07 整理，内容以博客原文 + 当日 bevy main 源码为准。

## 源头说明与链接总表

**博客原文**（jms55.github.io）：
- [Virtual Geometry in Bevy 0.14](https://jms55.github.io/posts/2024-06-09-virtual-geometry-bevy-0-14/)（2024-06-09）
- [Virtual Geometry in Bevy 0.15](https://jms55.github.io/posts/2024-11-14-virtual-geometry-bevy-0-15/)（2024-11-14）
- [Virtual Geometry in Bevy 0.16](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16/)（2025-03-27）
- 相关：[Bevy's Fourth Birthday - A Year of Meshlets](https://jms55.github.io/posts/2024-08-30-bevy-fourth-birthday/)（2024-08-30）

**源码**：
- 在线：<https://github.com/bevyengine/bevy/tree/main/crates/bevy_pbr/src/meshlet>
- 本机探针克隆：`/tmp/bevy-meshlet-probe`（sparse，`git clone --depth 1 --filter=blob:none --no-checkout --sparse` 经 gh-proxy，2026-11-07）
- meshoptimizer：[仓库](https://github.com/zeux/meshoptimizer) · [nanite.cpp demo](https://github.com/zeux/meshoptimizer/blob/master/demo/nanite.cpp)（0.15 LOD 选择改进的出处） · [Meshlets Merging 讨论](https://github.com/zeux/meshoptimizer/discussions/569)

**文中提到的 PR 索引**（全部 github.com/bevyengine/bevy/pull/NNNNN）：

| PR | 主题 | 提及于 |
|----|------|--------|
| [#13431](https://github.com/bevyengine/bevy/pull/13431) | 资产处理器（背景） | 0.14 |
| [#13904](https://github.com/bevyengine/bevy/pull/13904) / [#13913](https://github.com/bevyengine/bevy/pull/13913) / [#14038](https://github.com/bevyengine/bevy/pull/14038) / [#14042](https://github.com/bevyengine/bevy/pull/14042) | Kapoulkine：转换器提速/确定性 + mip 层 bug 修复 | 0.15 |
| [#14193](https://github.com/bevyengine/bevy/pull/14193) | 加载 102ms→12ms（bytemuck） | 0.15 |
| [#14623](https://github.com/bevyengine/bevy/pull/14623) | 软光栅（重写全库） | 0.15 |
| [#15023](https://github.com/bevyengine/bevy/pull/15023) | 64v:64t → 255v:128t | 0.15 |
| [#15084](https://github.com/bevyengine/bevy/pull/15084) | 屏幕空间隐式切线 | 0.15 |
| [#15643](https://github.com/bevyengine/bevy/pull/15643) | 顶点压缩（显存 -42%） | 0.15 |
| [#15846](https://github.com/bevyengine/bevy/pull/15846) | LOD 选择改进（抄 nanite.cpp） | 0.15 |
| [#15886](https://github.com/bevyengine/bevy/pull/15886) | 转换改进（8 簇/组、手动锁边界） | 0.15 |
| [#15955](https://github.com/bevyengine/bevy/pull/15955) | fill cluster buffers 重写（含 ID 溢出坑） | 0.15 |
| [#16049](https://github.com/bevyengine/bevy/pull/16049) | 软光栅 stray pixels 修复 | 0.15 |
| [#16111](https://github.com/bevyengine/bevy/pull/16111) | 法线感知简化 | 0.15 |
| [#16947](https://github.com/bevyengine/bevy/pull/16947) | METIS 直接聚三角形（DAG 秘方） | 0.16 |
| [#17765](https://github.com/bevyengine/bevy/pull/17765) | texture atomics | 0.16 |

**其他参考**：[Nanite Deep Dive（Karis SIGGRAPH 2021）](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)、[NVIDIA Mesh Shader 介绍](https://developer.nvidia.com/blog/introduction-turing-mesh-shaders/)、[投影球半径（StackOverflow）](https://stackoverflow.com/questions/21648630/radius-of-projected-sphere-in-screen-space)

## 0. 系列速览（先读这行再翻细节）

| 版本 | 发布 | 一句话 | 性能（Bunny 3375 实例，3080） |
|------|------|--------|------------------------------|
| 0.14 | 2024-06 | 完整管线首版：离线 LOD 树 + 两遍可见性光栅 | 4.97 ms/帧 |
| 0.15 | 2024-11 | 软光栅 + 压缩 + DAG 收敛，5 倍提速 | 0.93 ms/帧 |
| 0.16 | 2025-03 | METIS 直接聚三角形（DAG 质量秘方）+ texture atomics | 再快 0.42 ms |

**重要口径纠正**（此前 `docs/s3/S3-PLAN.md` / `docs/ROADMAP.md` 写错过）：博客期（0.14~0.16）bevy **一直保留 DAG**，没有"弃 DAG 改 BVH"。0.15 是 DAG 质量大改进（Bunny 从 7 层 19 簇收敛 → 12 层收敛到 1 簇）；0.16 博文里 BVH 剔除只是 roadmap（作者因 burnout 未完成）；**bevy main 现在确实有 `cull_bvh.wesl`**（BVH 遍历剔除是 0.16 之后实现的），DAG 负责 LOD 层级、BVH 负责 GPU 遍历剔除——两者是配合不是替代。

## 1. 0.14 篇：完整管线首版

### 1.1 离线构建（`from_mesh.rs`，"基本抄 Nanite，Nanite 做得更好"）

1. **LOD 0 建簇**：`meshopt build_meshlets`（64 顶点/64 三角形上限），无简化。
2. **三种包围球**：`self_culling`（剔除用，`compute_meshlet_bounds`）、`self_lod`（LOD0 = 0）、`parent_lod`（无父 = f32::MAX）——**误差以球半径编码**。
3. **找边**：顶点对 `(min,max)` 归一化进 hashset。
4. **找连通**：共享 ≥1 条边即连通，共享边数当权重。
5. **METIS 分组**：图划分，目标 4 簇/组。
6. **组内简化**（最难步）：拼接索引 → `meshopt simplify` 到 50% → QEM 误差经 `simplify_scale()` 转物体空间 → **累加子级最大误差**（`group_error = max(group_error, 子 self_lod)`）→ 组包围球半径 = 组误差，写进每个子簇的 `parent_lod`。
7. **再切簇**：简化网格重新 buildMeshlets → 新簇 `self_lod` = 组球，`parent_lod` 重置 MAX。循环 3~7 直至无可简化（实际多数网格收敛不到单根）。

### 1.2 运行时 9 pass（2.78ms）

fill cluster buffers 0.22 → 剔除① 0.49 → 光栅 visbuffer① 1.85 → 深度金字塔 0.03 → 剔除② 0.11 → 光栅② <0.01 → copy material depth 0.04 → 材质着色 → 金字塔(供下帧) 0.03。

关键实现点：
- **fill cluster buffers**：每帧按 instance 上传前缀和（8B/instance，不是 8B/cluster 的 122MB！），compute 里二分查找 instance。
- **剔除 pass**（作者重写多次）：instance 位掩码 → 视锥 6 平面 → **LOD 选择**（本簇误差投影 <1px 且父簇 ≥1px）→ 遮挡（**上一帧**深度金字塔，球投影 AABB，**mip 层必须 ceil 不能 floor**，2x2 textureLoad 取 min）→ 通过者 atomicAdd 进 indirect args + 写 `draw_triangle_buffer`（**u32 = 26bit cluster ID | 6bit 三角形 ID**）。
- **材质着色**：每材质一次全屏三角形，深度比较 equals 只着匹配像素；片元从 visbuffer 恢复簇/三角形，**手动算重心坐标与屏幕空间导数**（借 The Forge）。
- LOD 误差投影公式（和我们 S3 用同一个）：`clip_from_view[0][0] * r / sqrt(d²-r²) * view_size < 1.0`。

### 1.3 "what didn't work"（0.14 篇精华，S4 前必读）

| 尝试 | 结局 | 教训 |
|------|------|------|
| CPU 写全量 cluster ID buffer | 122MB，7.63ms = 46% 帧预算 | GPU 端生成 + 每 instance 极小上传 |
| single draw indexed indirect | 快 10-20% 但索引 buffer 按最坏分配，撞 2GB 上限 | 用 4B/三角形的 triangle buffer |
| NaN 填充 dummy 三角形 | 4B/cluster 最省，但 GPU fixed-function 被垃圾三角形拖死 | 别骗光栅器 |
| multi draw indirect（每簇一 sub-draw） | GPU 命令处理器海量子命令，瓶颈没在光栅上 | — |
| mesh shader | wgpu 不支持，出局 | 我们 D3D12/Vulkan **可以用**，但默认 compute |
| 软光栅 | 需要 64 位纹理原子（当时 wgpu 没有） | 0.15 就做上了，见下 |
| 跨帧存每簇可见性位掩码 | 与 LOD 冲突（上帧可见簇本帧可能不在 cut 上） | 用上一帧深度金字塔 |

## 2. 0.15 篇：5 倍提速（4.97 → 0.93ms）

按 PR 顺序，每条都有可抄的实底：

- **加载**（#14193）：弃 bincode（77ms 反序列化）改 `bytemuck::cast_slice` 直读 Pod → **102ms→12ms**。
- **软光栅**（#14623，几乎重写全库）：
  - 启发式：簇屏幕 AABB 两轴 <64px → 软光栅；**且不与近裁剪面相交**（`dot(frustum[4], center) > radius`），否则硬光栅（GPU 自动处理近裁剪，两行代码免裁剪数学）。
  - Nanite 技巧：软/硬共用一个簇缓冲，软从左端 atomicAdd、硬从右端倒序追加。
  - 硬光栅 fragment 用 **64 位 atomicMax**（`(depth<<32)|packed_ids`）写 visbuffer。
  - 软光栅每 workgroup 一簇：先各变换 1 顶点进共享内存，再每线程一三角形，按包围盒选逐像素/逐扫描线（扫描线要逐像素做"中心在三角形内"检查，Nanite 幻灯片没画这步）。
  - 作者忠告：**软光栅放最后做**（只带来 10-20%）；先把剔除、DAG 构建做好。
- **簇尺寸**（#15023）：64v:64t → **255v:128t**。发现 `v:t ≤ 1` 导致多数簇不满，`2:1` 比例满簇多。
- **顶点压缩**（#15643）：每簇自带顶点副本（为流式铺路）；位置量化 1/16cm + 每轴 min/max 重映射 + `ceil(log2(range))` 位打包；法线八面体 + `pack2x16snorm`（试过 bitstream，不如它）；显存 110MB→64MB（-42%）。**量化因子必须全网格统一，否则簇间裂缝**。
- **LOD 选择改进**（#15846）：照抄 meshoptimizer `nanite.cpp`——组包围球 = 子球最小包围球（自底向上），误差单独 f16 存，两者都单调，切换无缝，转换器不再崩。
- **转换改进**（#15886）：分组只看顶点位置（忽略 UV 缝）；METIS 传 seed 完全确定；**分组目标 4→8**（Nanite 用 8-32，简化质量大涨）；**手动只锁组边界顶点**而非 `LOCK_BORDER` 锁全网——最高 LOD 才能缩到 1 簇。
- **fill cluster buffers**（#15955）：每簇一线程+二分 → **每实例一个 workgroup(1024)**，首线程 atomicAdd 预留 + `workgroupUniformLoad` 广播，NSight 显示 95% VRAM 吞吐（已带宽封顶）。
- **法线感知简化**（#16111）：`meshopt_simplifyWithAttributes` 把法线传进简化器。

### 0.15 的重大踩坑：cluster ID 溢出

1041 个实例正常、1042 起闪烁且非确定——visbuffer 里 `25bit cluster ID | 7bit tri ID`，`1042 × 32217 > 2^25` 溢出。且新 fill shader 让 RenderDoc 回放每次都变，没法稳定调试。正确解法（只存可见簇）留到 0.16+。

## 3. 0.16 篇：DAG 质量 + texture atomics

- **METIS 直接聚三角形**（#16947，"DAG 质量是 virtual geometry 最重要也最难的部分"）：
  - 旧路：`meshopt_buildMeshlets` 聚三角形（为 culling/顶点复用优化）——但 virtual geometry 要的是**簇间共享顶点尽可能少**（共享少 → 锁定顶点少 → 简化自由度大 → DAG 好）。
  - 新路：三角形聚类和簇分组是**同一个问题**——METIS 图划分，节点=三角形、边=相邻、边权=共享顶点数。
  - **调参秘方**：`UFactor=1`；划分数故意少算（128 簇上限用 `div_ceil(126)`），否则 METIS 超发导致簇过大。
  - 效果：旧 DAG 常有 <10 三角形的"卡住"小簇，新 DAG 均匀、向高 LOD 顺畅收敛。
- **texture atomics**（#17765）：visbuffer 改 R64Uint/R32Uint storage texture + 纹理原子，缓存行为更好。
- **一行 discard 翻倍**：material depth resolve 里 `if depth==0 { discard; }`（背景像素直接弃）——**一行代码 resolve 两个 pass 性能翻倍**。
- **clear_texture 的坑**：`clear_texture()` 对不支持 RENDER_ATTACHMENT 的 R64 纹理走"全零 buffer + vkCmdCopyBufferToImage"，单条命令 **0.68ms**（buffer 清零才 0.01ms）；最优是 vkClearColorImage 但 wgpu 不暴露 → 作者手写 compute 清零。**教训：清屏路径也要 profile**。

## 4. bevy main 源码落点（2026-11-07 实码核对）

| 文件 | 对应内容 | 我们用在哪 |
|------|----------|-----------|
| [`from_mesh.rs`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/from_mesh.rs)（1099 行） | 离线全流程；`TARGET_MESHLETS_PER_GROUP=8`、`SIMPLIFICATION_FAILURE_PERCENTAGE=0.60`（简化不达标宁多留三角形不加簇）、量化因子 4（1/16cm） | S1/S3 工具实现对照 |
| [`asset.rs`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/asset.rs)（319 行） | `Meshlet`/`MeshletBoundingSphere`/`BvhNode`/`MeshletAabb` 资产结构 | S1 资产格式设计 |
| [`cull_bvh.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/cull_bvh.wesl)（109 行） | BVH 遍历剔除：workgroup 128 = 16 节点 × 8 线程，early/late 两级 dispatch 队列 + second pass | S3/S4 剔除方案对照 |
| [`cull_clusters.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/cull_clusters.wesl) / [`cull_instances.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/cull_instances.wesl) / [`cull_shared.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/cull_shared.wesl) | 簇/实例剔除 + `lod_error_is_imperceptible` 等共享函数 | S4 误差投影公式可直接对照 |
| [`visibility_buffer_hardware_raster.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/visibility_buffer_hardware_raster.wesl) / [`visibility_buffer_software_raster.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/visibility_buffer_software_raster.wesl)（189 行软光栅） | 双路光栅 + visbuffer 写入 | S5/S14 |
| [`resolve_render_targets.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/resolve_render_targets.wesl) / [`clear_visibility_buffer.wesl`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/meshlet/clear_visibility_buffer.wesl) | visbuffer resolve 与清零 | S5 |

## 5. 对我们 S1~S5 的直接启示

1. **S1 高模资产**（M0.7）：bevy 要求输入"带索引 TriangleList + {POSITION,NORMAL,UV_0}"——我们的高模生成器照此出。
2. **S1 建簇**：第一步用 `meshopt_buildMeshlets` 没问题（Carrot 同款）；但要知道天花板：DAG 质量取决于簇间共享顶点，进 S3 时考虑 METIS 直接聚三角形（0.16 秘方可整段借鉴）。
3. **S3 分组**：目标 8 簇/组（不是 4）；**手动锁组边界顶点**是收敛到单根的关键。
4. **S3 误差**：误差=球半径编码、逐级 `max` 累加、投影公式与 bevy `lod_error_is_imperceptible` 同源——我们的 `--mode cluster` 调试视图直接对齐它的 cluster/LOD/screen-error 四视图。
5. **S4 剔除**：两遍法 + 上一帧深度金字塔（mip ceil！）；「CPU/GPU 写什么 buffer、多大」的 5 个反例直接避开。
6. **别急着做软光栅**：JMS55 与 Carrot 都验证它只值 10-20%，放最后。
7. **wgpu 的限制不是我们的**：64 位原子（D3D12/Vulkan 都有）、mesh shader（可选）我们都可用，但按博客经验默认 compute 路线性价比最高。
