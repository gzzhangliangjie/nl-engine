# bevy 开发过程导读：用 meshlet / solari 当「同代实证」

> bevy 是 **2024 起从零长出来的同代 Nanite×GI 实现**，和我们的研究主线几乎同岁。AGENTS.md 给它的定位是 **「同代实证」**——不是工业终态（那归 UE5），也不是纯粹节奏参照（那归 Carrot），而是 **一个和我们处在同一技术水位、用开源社区方式把同一套概念落地的活样本**。它比 Carrot 更"现代"（GPU-driven + 持久缓冲 + atomics + BVH 剔除 + Solari ReSTIR GI），但语言是 WESL/WebGPU，和我们的 Slang/NVRHI 不同源——所以天生就是"抄思路不抄代码"。
> 本导读把 bevy meshlet / solari 的关键提交抽成时间线，对齐到我们的 S1~S14 与 P9，重点标出 **和我们决策同构、或能提前替我们踩坑** 的点。

## 怎么挖（三条命令就够）

```bash
cd E:/UE_STUDY_2026/bevy
git log --all --oneline -i --grep="meshlet"        # 按关键词看 meshlet 时间线
git show 57e58ef997                                # 看某次提交的完整 diff（重点功能）
git log --oneline --follow -- crates/bevy_pbr/src/meshlet/from_mesh.rs  # 单文件演进史
```

进阶：`git log -S "METIS" --oneline`（pickaxe，找「METIS 分组」第一次出现的提交）。仓库已克隆在 `E:\UE_STUDY_2026\bevy\`，HEAD = `main` / v0.16.0-rc.4（2026-11-05），**纯本地，无需联网**。

## 时间线 → 我们的步骤（实测抽取）

| bevy 实况（hash 可直接 show） | 时间 | 对应我们 | 能学到什么 |
|---|---|---|---|
| `4f20faaa43` Meshlet rendering（initial feature） | 2024-03 | S1 | **起点就是 GPU-driven + 可见性缓冲 + 深度下采样**，引用了 Nanite SIGGRAPH 2021 与 two-pass occlusion 文章。和我们"先画出来再剔除"的节奏一致 |
| `6d6810c90d` Meshlet continuous LOD | 2024-04 | S5 | 连续 LOD 切层，单文件 `from_mesh.rs` 一次 +385 行 |
| `aa626e4f0b` Per-meshlet compressed vertex data | 2024-10 | P9 V4 / S1 | ⭐ **明确"为流式做准备，顶点按 meshlet 自包含存"**：位置量化到厘米级 + 相对 meshlet min 的位流压缩，法线 octahedral + snorm2x16 压成 u32。这正是我们 P9 V4「cluster 自包含、不为全局共享顶点池焊死」的同行验证 |
| `9d54fe0370` Meshlet new error projection | 2024-10 | S3 | ⭐ **误差度量直接取自 zeux 的 meshoptimizer `nanite.cpp` demo**（`compute_lod_group_data` / `lod_error_is_imperceptible`）。我们 S3 error chain（父簇误差单调不减、组误差=max）应锚定同一权威源，别自创误差公式 |
| `33365c5b01` fill cluster buffers rewritten | 2024-10 | S1 | cluster 缓冲重写，可见性缓冲光栅/反解一起动 |
| `119e37d2ae` Meshlet builder improvements redux | 2024-10 | S2 | 离线构建器迭代（from_mesh.rs 又 +184 行） |
| `34b94f590f` normal-aware LOD + meshopt 升级 | 2024-11 | S1/S3 | **LOD 选择同时看法线简化误差与位置误差**（meshopt 升 0.22）。和我们 M2.3a 朝向锚（法线随折叠同步更新）同动机——法线跳变=LOD 切错 |
| `fe58993577` **METIS-based meshlet generation** | 2025-01 | S2 | ⭐ **用 METIS 分组是为了"最小化 locked vertices、改善简化质量"，不是为剔除效率**（meshopt 才优先剔除效率）。这直接印证我们 S2 vendored METIS + 按组重切保证父子簇不重叠的决策；作者也提到 126/128 三角的取整遗憾，和我们"按组重切簇粒度取整"同构 |
| `dda97880c4` GPU two-phase occlusion culling | 2025-01 | S4/S6 | 实验性两阶段 GPU 遮挡剔除，下采样逻辑挪到 `mip_generation/` |
| `57e58ef997` **Meshlet BVH Culling** | 2025-06 | S4/S6/S13 | 合并 BVH 加速的 meshlet 剔除（新增 `cull_bvh.wgsl` 110 行，from_mesh.rs +766 行）；修了近平面过剔除、HZB 采样、正交误差度量。**BVH 管 GPU 遍历剔除，Hi-Z 管遮挡，两者配合不是替代** |
| `2d4bf0cfc5` Hi-Z generation properly conservative | 2026-01 | S6 | ⭐ **单 pass 下采样（SPD）只在深度边长为 2 的幂时保守**；非幂次需 3×3 采样或上取整到下一幂次。修了右/下边缘闪烁。我们 S6 做 Hi-Z 必踩这个坑 |
| `98639670f5` many_meshlet_materials 压测示例 | 2026-05 | S7 / P9 V1 | 多材质 meshlet 压力测试——对应真实资产多材质/PBR 的验收场景 |
| `7a21c21ecb` Mesh shader pipelines | 2025 | S1 | mesh shader 管线落地 |
| `bab31e3777` Initial raytraced lighting（bevy_solari） | 2024 | S10/S13 | Solari = bevy 的 RT GI（ReSTIR + world cache），对应我们 Lumen/S10 |
| `bdaf47c72b` **Zero-Day large-scene example** | 2026-09 | P9 V2/V3/V4 | ⭐ Solari 大场景示例（Beeple Zero-Day，NVIDIA ORCA 资产），808 行 main.rs 加载/动画/相机跟随——就是我们 P9 要落地的"超大场景真验"的现成参照 |

## 五个「实况才看得到」的开发教训

1. **METIS 分组的动机是简化质量，不是剔除效率**（fe58993577）：作者原话 "Use METIS to group triangles into meshlets which lets us minimize locked vertices which improves simplification, instead of using meshopt which prioritizes culling efficiency"。这和我们 S2「按组重切→父簇绝不跨组→S5 组原子 cut 才成立」是同一逻辑的两种表述——locked vertices 少=组间边界干净=简化误差可控。
2. **顶点按 meshlet 自包含存 = 为流式预埋**（aa626e4f0b）：bevy 在 2024-10 就把顶点从"按 mesh 共享"改成"按 meshlet 复制 + 位流压缩"，理由写在 Objective 第一行就是 "Prepare for streaming"。这等于别人用实况帮我们验证了 P9 V4 的 `.nmesh` 分页友好设计——**不焊死全局共享顶点池，后期做流式就不返工**。
3. **误差度量别自创，抄 zeux 的 nanite.cpp**（9d54fe0370）：bevy 自己承认 error projection 取自 meshoptimizer 作者的 demo。我们 S3 的 error chain 判据（max error 单调、组 error=max(组简化,成员簇)）应和这一权威源对齐，而不是拍脑袋定公式。
4. **Hi-Z 下采样保守性是真 bug 源**（2d4bf0cfc5）：非 2 的幂深度缓冲单 pass 下采样会漏采样、导致边缘闪烁，修了才"less broken"。我们 S6 若上 Hi-Z，生成阶段就必须按 2 的幂或 3×3 采样，否则 occlusion culling 会误删可见簇。
5. **BVH 与 Hi-Z 是两层剔除，不是二选一**（57e58ef997 + AGENTS.md）：bevy 用 BVH（`cull_bvh.wgsl`）做 meshlet 的 GPU 遍历剔除、Hi-Z 做遮挡剔除；UE 工业终态是 Hi-Z + software raster。我们 S4（硬件剔除）/ S6（Hi-Z 遮挡）的取舍要讲清层级，别混为一谈。

## 文件地图（卡住时去哪看）

| 子系统 | 路径 | 对应我们 |
|---|---|---|
| 离线管线（分组/简化/误差/顶点压缩）×单文件集中 | `crates/bevy_pbr/src/meshlet/from_mesh.rs` | core/ + tools/ 拆开的部分 |
| 运行时 meshlet 管理 | `meshlet_mesh_manager.rs` / `instance_manager.rs` / `resource_manager.rs` / `persistent_buffer*.rs` | renderer/ 资源与持久缓冲 |
| 剔除（GPU 遍历 / 簇 / 实例） | `cull_bvh.wgsl` / `cull_clusters.wgsl` / `cull_instances.wgsl` / `fill_counts.wgsl` | S4/S6 剔除 |
| 可见性缓冲（硬件/软件光栅 + 反解） | `visibility_buffer_*.wesl` | S1/S7 visbuf |
| Hi-Z 生成 | `bevy_core_pipeline/src/experimental/mip_generation/` | S6 |
| RT GI（ReSTIR + world cache） | `crates/bevy_solari/src/realtime/`（restir.wesl / world_cache_*.wesl / initial_path.wesl / atmosphere.wesl） | S10/S13 |
| 大场景示例 | `examples/large_scenes/zero_day/` | P9 V2/V3/V4 |

## 用法约定

- **bevy 离线管线是单 TU 集中实现**（分组、简化、误差、顶点压缩全在 `from_mesh.rs` 一个文件里，单功能 diff 常 100~766 行），我们拆成 `core/` + `tools/` 多模块是为了可测可控——粒度不同但概念一一对齐，不要因为文件结构不同而以为"bevy 没做某步"。
- **语言天然隔源**：bevy 用 WESL（WebGPU Shading Language，已从 wgsl 迁移），我们用 Slang。AGENTS.md「抄思路不抄代码」在这里是物理强制的——看算法结构、误差公式、剔除层级，别搬 shader。
- 卡住某步时：先 `git show <hash>` 看作者当时的 diff 规模，你的实现粒度应和它同量级（通常单功能 100~700 行），超了多半是过度设计。
- 想看「完整功能从无到有」：从 `4f20faaa43`（initial）顺着 `git log --reverse` 翻到 `57e58ef997`（BVH 剔除），比读最终代码高效。
- **P9 必读**：`aa626e4f0b`（流式顶点自包含）+ `bdaf47c72b`（大场景示例）+ `fe58993577`（METIS 分组），这三处是 V2/V4 决策的直接同行证据。
