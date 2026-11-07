# P9 真实资产落地验收 · 详细计划（可照敲）

> 对应总路线 `docs/ROADMAP.md` 的「阶段 P9」。S0~S14 + P8 之后、按需推进的**收尾验收关，也是项目立身之本**。
> **不阻塞主线**：S0~S14 与 P8 各自验收通过后才进本阶段；真实资产由用户提供，资产到位后单独走 V1→V4。
> 目录说明：本计划放 `docs/p9/`，**不进 `docs/s9/`**——`docs/s9/` 已被 S9 步骤（GBuffer+直光+阴影射线）占用，P（阶段）与 S（步骤）在路线图里前缀不同，但文件系统不能撞。

---

## 顶层硬约束（用户 2026-11-08 原话）

> "如果最终不能加载大场景，试一试做这个还有什么意思"

→ **V2 超大场景首版即须跑通（不以流式为前提）；V4 把流式做实到超显存 N 倍场景。两者都非可选、非推远。**

---

## 全局不变量（V1 起就必须满足，为 V4 流式预埋）

- **INV-1 顶点按簇自包含、可分页存储**：绝不为「全局共享顶点池」焊死结构。
  - 证据：Carrot `ClusterManager.cpp:144-150`（每簇 `vertices.resize(firstVertexIndex+meshlet.vertexCount)` 独立拷贝，buffer 命名 "Virtual geometry vertex buffer %llu meshlets"）；bevy `aa626e4f0b`（per-meshlet 顶点自包含 + 位流压缩，Objective 第一行即 "Prepare for streaming"）；`kb/index.html:475`（"簇共享顶点池会让按簇加载/流式变得很麻烦"）。
- **INV-2 `.nmesh` 分页友好格式**：cluster 自包含、支持按簇/页独立加载。字段级草案 **2026-11-08 已拍板（5 点全按推荐来）**：`docs/p9/NMESH-PAGING-DRAFT.md`（状态 DECIDED，待 V1 落地；§8 落地步骤已列）。
- **INV-3 renderer 预留 streaming 抽象接口**（asset residency / page manager）：V1~V3 实现为「全常驻」，V4 替换为「分页 residency」。接口签名现在定，实现后换。

---

## V1 真实资产接入离线管线

- **目标**：单 / 少数真实模型（多材质 / PBR 贴图 / 可能含破面·孤立顶点·法线缺失）过完整离线管线 `cluster → simplify → dag → fallback`，产出 `.nmesh` + `viewer` 全模式可加载。
- **可运行用例（资产到位后）**：
  - `nanite-tools` 全链（cluster / simplify / dag / fallback）三阶段退出 0。
  - `viewer` 全模式（textured / normal / uv / tri-id + lod / dag-top / wireframe）加载真实资产不崩。
  - 分层画廊（lod0..N）无黑斑无裂缝无洞。
- **验收判据（沿用 M2.3a 口径）**：
  - 离线三阶段无报错退出；`nanite-tools dag` 六项校验全 PASS；fallback 三角守恒 + weld 收敛 + 边流形每边 ≤ 2。
  - 分层画廊无黑斑无裂缝无洞（M2.3a 验收口径）。
  - 双后端画面 `bytes` / `near`（AGENTS.md 双后端纪律，禁止笼统写「一致」）。
- **三处证据**：
  - UE5 `Developer/NaniteBuilder/`（Cluster.cpp / ClusterDAG.cpp / GraphPartitioner.cpp / NaniteAssemblyBuild.cpp + `Developer/MeshSimplifier/MeshSimplify.cpp` 翻面判定）——工业级离线构建口径对照。
  - bevy `crates/bevy_pbr/src/meshlet/from_mesh.rs`（分组/简化/误差/顶点压缩单 TU 集中）——同代实证。
  - Carrot `ClusterManager.cpp`（簇顶点自包含，INV-1 印证）。
- **不变量**：INV-1/2/3 落地；资产脏数据（非流形/破面/孤立顶点/法线缺失）由离线管线 cleanup / weld / 法线继承消化（S2 `rebuildMeshlets` 口径），不另立资产清洗工具。

---

## V2 超大场景全链路渲染（首版即须跑通）

- **目标**：剔除 / LOD / DAG 遍历 / Hi-Z / visbuf / PBR / 直光 / GI 在**超大场景**上正确，稳定 60fps。
- **关键**：**不以流式为前提**——整模型常驻或分块加载即可跑通。S4 百万面硬件剔除 / S5 DAG 屏幕误差 LOD / S6 Hi-Z 遮挡本就为大场景设计，能力已具备。
- **可运行用例**：`viewer` 实跑用户提供的超大场景，各模式（含 Lumen GI、阴影）结果正确，HUD 统计合理，稳定 60fps。
- **验收判据（沿用 S4 性能基准）**：
  - 各模式结果与参考 / 对照组一致；超大场景稳定 60fps。
  - 双后端画面 `bytes` / `near`。
- **三处证据**：
  - bevy `bdaf47c72b` Zero-Day 大场景示例（Beeple Zero-Day，NVIDIA ORCA 资产）——现成超大场景参照。
  - S4 / S5 / S6 研究主线成果——大场景能力底座。
  - UE5 `Nanite/NaniteCullRaster.cpp` + `NaniteVisibility.cpp`——cut 判定与遮挡两遍真实开销对照。
- **不变量**：大场景首版必须能加载能跑（用户硬约束）；若单卡显存放不下，**降级分块加载**，不以完整流式为验收前提。

---

## V3 多物体真实场景（承接 P8 E1）

- **目标**：真实资产组合成多物体世界（N 物体 M 灯），GI / LOD 在多物体真实场景正确响应。
- **可运行用例**：`viewer` 加载真实多物体场景，GI / LOD 正确响应相机 / 光源变化。
- **验收判据**：GI / LOD 在多物体真实场景正确响应；双后端 `bytes` / `near`；性能随物体数合理。
- **三处证据**：
  - bevy `98639670f5` many_meshlet_materials 多材质 meshlet 压测——多材质真实资产验收场景。
  - P8 E1（ECS-lite 多物体场景）——承接关系。
  - UE5 `Nanite/NaniteDrawList.cpp`——多视图（阴影/反射）复用一次剔除。

---

## V4 完整流式落地

- **目标**：超显存超大场景按页加载 / 卸载，帧时间平稳，无 pop-in 爆闪。
- **可运行用例**：`viewer` 实跑超显存超大场景，按相机移动按需 page in / out，帧时间平稳，无爆闪。
- **验收判据**：
  - 超显存 N 倍场景能流畅加载；帧时间平稳；无 pop-in 爆闪；双后端一致。
- **三处证据（V4 直接对标）**：
  - UE5 `Nanite/NaniteStreamOut.h` 的 `FStreamOutMeshDataHeader`（`VertexBufferOffset / IndexBufferOffset / NumVertices / NumIndices` 偏移寻址）+ `kb/index.html:820` 点名 `NaniteStreamingManager*.cpp` = 分页与驻留策略——工业终态的流式 = 「偏移寻址大 buffer + 独立 residency 管理器」。
  - bevy `aa626e4f0b`（per-meshlet 顶点自包含，"Prepare for streaming"）——格式层预埋验证。
  - Carrot `ClusterManager.cpp:144-150`（每簇顶点自包含）——同动机独立印证。
- **不变量**：INV-1/2/3 收口；renderer streaming 抽象接口由「全常驻」替换为「分页 residency」；**纹理随几何 page 走**（VT 已跳过，见 MEMORY「新增研究模块决议」）。

---

## 验收总闸与交付纪律

- 复用 `example/cases.json`（唯一数据源）+ `tools/regression.py` 总闸。
- 每 V 里程碑「交代」：渲染版 = `example/<版本>/` 基线 PNG + `run.bat`；工具版 = 展示台 `kind:"tool"` 用例 + 归档；tag 在验收后打；提交前 regression 全绿；`docs/MILESTONES.md` 四栏写实。
- 双后端画面一致分档 `bytes` / `near`，禁止笼统写「一致」。
- `.nmesh` 具体格式变更留待 V1 实现时按铁律请用户拍板（触碰核心约定，不擅定）。

---

## 卡住翻哪（三处来源文件地图）

| 问题 | 翻 |
|---|---|
| 离线 cluster / simplify / dag / fallback 口径 | UE5 `Developer/NaniteBuilder/`（Cluster.cpp / ClusterDAG.cpp / GraphPartitioner.cpp / NaniteAssemblyBuild.cpp）+ `Developer/MeshSimplifier/MeshSimplify.cpp`（翻面）；bevy `from_mesh.rs`；Carrot `ClusterManager.cpp` |
| 超大场景 / 流式（V2/V4） | UE5 `Nanite/NaniteStreamOut.h` + `NaniteStreamingManager*.cpp` + `NaniteCullRaster.cpp`；bevy `bdaf47c72b`（大场景示例）+ `aa626e4f0b`（流式顶点预埋）；Carrot `ClusterManager.cpp` |
| 误差度量（S3 error chain 基准） | zeux meshoptimizer `nanite.cpp`（bevy `9d54fe0370` 同源）——别自创误差公式 |
| 大场景示例直接抄结构 | bevy `examples/large_scenes/zero_day/`（808 行 main.rs 加载/动画/相机跟随） |
