# S4 详细开发路线图：硬件光栅 + GPU 视锥剔除 + Indirect Draw

> 对应总路线 `docs/ROADMAP.md` 的 S4。预计 3 个里程碑、业余时间 1~1.5 周。
> 从本步起进入 **GPU-driven**：CPU 不再逐簇提交，而是 compute 剔除 → 写 indirect 命令 → 一次 draw 画全部可见簇。这是 S5（DAG 遍历 cut）/ S6（Hi-Z 遮挡）的骨架。
>
> **2026-11-08 三家实读修订**（M4.0 开工前按铁律复读，修订处均注明来源；原 S0 版骨架已按实读结论校正）。

---

## 三家参考实读定稿（2026-11-08，M4.0 开工前）

| 来源 | 位置 | 对 S4 的结论 |
|---|---|---|
| **bevy 0.15**（直接蓝本） | `meshlet/cull_clusters.wesl:29-92`：每线程 1 簇、wg=128，`aabb_in_frustum`（5 平面）+ atomicAdd append 到 raster 桶并写 `hardware_raster_indirect_args.instance_count`；`fill_counts.wesl:24-33`（单线程填计数）；`visibility_buffer_hardware_raster.wesl:27-58`（**vertex pulling**：一次 `draw_indirect`，`instance_index → 簇槽位 → SSBO 读顶点`，超界产 dummy NaN 顶点）；`visibility_buffer_raster_node.rs:592`（`pass.draw_indirect`） | 结构照抄：cull compute（append + 写 instanceCount）→ 一次 indirect draw（vertex pulling）。bevy 的软件光栅桶/两 pass 遮挡队列是 S7/S6 的东西，S4 不搬 |
| bevy 视锥公式 | `cull_shared.wesl:44-64`（Gribb-Hartmann 行抽取 5 平面：`row[3]±row[0]`、`row[3]±row[1]`、`row[2]`；AABB `half_extent*sign(plane.xyz)` 投影）+ `:84-121`（project_aabb，近平面后不剔除） | **5 平面不是 6**：近平面跳过，防相机贴脸/球心在近平面后的过剔除（bevy 57e58ef997 修过该 bug）。我们用簇**包围球**（.nmesh 52B 头自带 sphere[4]）代替 AABB |
| Carrot | `render/ClusterManager.cpp:572-588`（mesh task shader `drawMeshTasks`，组判定在 `draw_virtual_geometry.slang:105`） | 走 `VK_EXT_mesh_shader`——NVRHI 2021 代无 meshlet/mesh pipeline 对外承诺，**不适用**；其组原子双阈值判定留给 S5 |
| UE 5.8 | `Renderer/Private/Nanite/NaniteCullRaster.cpp`（主可见性 pass 全 compute 软光栅；**2026-11-08 用户追问后复读修正**：CVar "triangle edge length in pixels at which Nanite starts using the hardware rasterizer" + `FHWRasterizeVS/MS/PS` 一套 material shader——**大三角形走硬件光栅的补充路径存在**，且硬光栅 VS 同样是 pulling 式读 GPU 簇数据、变体还用 mesh shader `SF_Mesh`）+ `NaniteVisibility.cpp` | UE 硬件光栅只是「软光栅为主 + 大三角硬光栅补画」的**补充**，与 S4「硬件光栅为主路径」方向相反；其视锥剔除也不独立成 pass，全部融进 GPU-driven 层级遍历流水线。**S4 在 UE 里没有可拆的结构对应物**（能力有、结构没有）——S4 蓝本只有 bevy；UE 的 compute 全光栅是 S7 参考、层级剔除遍历是 S5 参考 |

**NVRHI 可行性（vendored 2021.3 实读确认）**：
- `ICommandList::drawIndexedIndirect(offsetBytes, drawCount)` 两后端均实现：`src/d3d12/d3d12-graphics.cpp:586`（ExecuteIndirect + CommandSignature）、`src/vulkan/vulkan-graphics.cpp:721`（vkCmdDrawIndexedIndirect，stride=`sizeof(DrawIndexedIndirectArguments)`）。
- `BufferDesc::setIsDrawIndirectArgs(true)`（nvrhi.h:807）声明 indirect 参数 buffer。
- args 布局 `DrawIndexedIndirectArguments`（nvrhi.h:2834，4×u32：vertexCount/instanceCount/firstVertex/firstInstance——**indexed 版本注意与 Vulkan `VkDrawIndexedIndirectCommand` 的 indexCount 字段对位**，实现时对照两后端 CommandSignature/stride 源码）。
- CPU readback：`createBuffer(cpuAccess=Read)` + `copyBuffer` + `mapBuffer/unmapBuffer`（nvrhi.h:3869）。

**范围边界（明确不做，防蔓延）**：遮挡剔除（S6 Hi-Z）、屏幕误差选层（S5）、BVH 加速剔除（bevy `57e58ef997` 是 2025-06 才加的 S13 级优化；S4 簇数 ≤ 万级线性遍历够用）、软件光栅（S7）、多实例剔除（bevy `cull_instances.wesl` 管实例级，我们单物体）。**剔除必须无状态可重复**（每帧从头算，S5 会替换它——接口今天留好）。

**开发历史教训（carrot-history.md:22）**：「剔除是后补的——先画出来再剔除，别一开始就上 GPU-driven 全家桶」→ M4.0 先通 pulling 渲染路径并对照既有模式回归，剔除在 M4.1 才上。

---

## M4.0 GPU 簇数据通路 + vertex pulling 渲染（2~3 天）

- [ ] **簇元数据 SSBO**：.nmesh 加载后把每簇紧凑记录（包围球 sphere[4] + vertexOffset/vertexCount/triangleOffset/triangleCount + 簇色）上传 `StructuredBuffer`（compute 与 VS 都读）。几何不改：现有窗口展开 VB 照旧可复用为顶点池，索引展开为索引池 SSBO（VS 按 pulling 读）
- [ ] **vertex pulling VS**：一次 draw，`instance_index → 可见簇列表槽位 → 簇元数据 → SSBO 读顶点/索引`；`vertex_index/3` 超出本簇三角数 → dummy 顶点（NaN clip position，光栅化自动丢弃——bevy 同式）
- [ ] **draw 路线（2026-11-08 用户拍板，方案 A）**：**单 draw + vertex pulling**——`drawIndexedIndirect`，`instanceCount = 可见簇数（GPU 写）`。bevy/UE 同款，S5 遍历 cut 时只换 instanceCount 语义，零返工；VS 多一次 SSBO 间接寻址。~~方案 B（per-cluster 多条 args + drawIndexedIndirectCount）已否决~~：两处参考无一采用（与 kb:475 全局池警告同源教训），不引入
- [ ] **零剔除回归基线**：M4.0 阶段 instanceCount 恒 = 总簇数（全画），验证 pulling 路径本身正确

**自检**：`--mode cull --cull off`（全画）与 `--mode cluster` 同机位同 shading 截图**逐像素一致**（对照对象是 cluster 模式——同一份 .nmesh 几何；旧版写 normal 是错的，normal 走 GLB 索引路径无法逐像素对照）。双后端 PNG 各自一致。tag：`s4-indirect`。

⚠️ 常见坑：
- Slang `StructuredBuffer` 在两后端的绑定槽（SRV/UAV、t/u shift）——按 `docs/SHADER-CONVENTIONS.md` 既有 shift 约定核，实现时回 vendored nvrhi.h 核 `ResourceType`
- pulling 的顶点池就是现有展开 VB（32B ClusterVertex，已带簇色）——别再建第二份顶点数据
- dummy 顶点 NaN 位置在两后端的光栅化剔除行为要实测（退化三角形应被丢弃，不产生像素）

## M4.1 compute 视锥剔除 + indirect draw（2 天）

- [ ] **cull.slang compute pass**：每线程 1 簇（wg=128），静态 dispatch（`ceil(簇数/128)`——输入是全量簇，CPU 已知，无需 indirect dispatch；bevy 的 cull queue 是 two-pass 遮挡才需要的动态结构，S4 不上）；5 平面球体测试（球心投影 + 半径，`row[3]±row[0]`、`row[3]±row[1]`、`row[2]`，近平面跳过）
- [ ] 通过 → `atomicAdd(instanceCount,1)` + append 簇号到可见列表 SSBO；indirect args 的 vertexCount/firstIndex 等静态字段由 CPU 初始化一次（不变字段），GPU 每帧只写 instanceCount
- [ ] **barrier**：compute 写 indirect args → graphics 读，NVRHI 状态跟踪通常覆盖；DX12 的 UAV→INDIRECT_ARGUMENT 显式状态在实现时实测确认（Donut `vertex_buffer` 示例 + nvrhi src/d3d12 有现成写法）
- [ ] **离线对照工具**：`tools/frustum_check.py`——读 .nmesh 簇球 + 相机参数（--yaw/pitch/dist 同公式）离线算可见簇集合，与 GPU HUD 数字逐个对照（机械判据，防"数字看起来合理"假证据）

**自检**：~~相机背对场景 HUD 可见簇数 ≈0~~ **2026-11-08 实测修正：轨道相机永远 look at 原点，背对时整模仍在视锥内（dist4.2 视锥半高≈1.74 > 模型半径≈1.3），「背对」剔除数仍是全量——本前提对轨道相机不成立**。真剔除维度 = 近平面：相机推进模型内部（dist 0.45）外围簇被近平面切掉，Suzanne 640x720 下 237→30。**验收姿态（正对全可见 / 进模真剔除）的 GPU 数字 == frustum_check.py 离线值**。tag：`s4-cull`。

⚠️ 常见坑：
- 视锥平面从**视图投影矩阵**提取（Gribb-Hartmann 行抽取），别对错行列；我们的 MVP 矩阵符号约定先核 `scene.cpp` 现有用法再抄 bevy 公式（行列序差异）
- atomicAdd 返回序两后端可能不同 → 可见列表顺序不同 → **不透明 + 深度测试画面应逐像素一致**（顺序无关）；若 PNG 出现差异按三档口径（bytes/near/FAIL）如实记录并查因，禁止笼统写"一致"
- append 槽位从 0 起 vs bevy 的 rightmost_slot 倒序——我们单向 append 即可（bevy 倒序是软件/硬件桶同 buffer 对齐的产物，S4 单桶不需要）

## M4.2 HUD 统计 + 1M 场景 + 性能验收（1~2 天）

- [x] **剔除统计回读**：可见簇数/总簇数/剔除率，readback buffer 延迟 1~2 帧（M4.1 已交付）
- [x] **GPU 计时**：cull pass 接入第二个 `GpuTimer` 实例（M4.2 落地：[frame]/[cull] resolve 日志 + HUD cull GPU 行/曲线；轮询必须在 hud_.build() 之前，踩坑见 MILESTONES）
- [x] **1M+ 三角资产（2026-11-08 用户拍板）**：IcoSphereDense1M 入仓——subdiv 8 = 1,310,720 tri / **19,718 簇**（PLAN 预估 6554，meshoptimizer 按簇上限切得更细，如实修正）/ .nmesh 33.5MB
- [x] **验收姿态**：正对 640x360 全可见 19718/19718 pass-through bytes（跨后端也一致）/ 进模 1932/19718 剔除 90.2% == 离线；CPU avg 4.9~5.5ms、GPU 整帧 0.96~1.02ms / cull pass ~0.010ms（判据 16.6ms 余两数量级）
- [x] **展示台交代**：cases.json s4.2 + s4.2-tools 卡 + example/s4-done 基线 + run.bat + S4-TOOLS-GUIDE M4.2 节；kStage=S4.2

**自检（S4 验收）**：进模姿态 HUD 绘制簇数骤降（近平面真剔除）；1M+ 场景稳定 60fps；双后端两姿态数字与离线工具一致。tag：`s4-done`，截图 `docs/screenshots/s4.png`（两张：正对全可见 / 进模高剔除 HUD 数字对比）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag | kStage |
|---|---|---|---|
| M4.0 pulling 通路 | 2~3 天 | `s4-indirect` | S4.0 |
| M4.1 视锥剔除 | 2 天 | `s4-cull` | S4.1 |
| M4.2 统计+1M+验收 | 1~2 天 | `s4-done` | S4.2 |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 剔除 pass 结构（每线程 1 簇、append、fill counts） | `bevy/.../meshlet/cull_clusters.wesl:29-92` + `fill_counts.wesl`（结构照抄，去掉两 pass 队列与软/硬桶） |
| vertex pulling VS + dummy 顶点 | `visibility_buffer_hardware_raster.wesl:27-58` |
| 视锥 5 平面公式 | `cull_shared.wesl:44-64`（Gribb-Hartmann）；实现前先核我们 MVP 的行列序 |
| NVRHI indirect / readback | vendored `nvrhi.h:2834/3522/3533/3869`；`src/d3d12/d3d12-graphics.cpp:586`、`src/vulkan/vulkan-graphics.cpp:721`；donut `vertex_buffer` 示例 |
| 簇包围球字段 | `core/include/nl/core/meshlet_io.hpp`（52B Meshlet 头 sphere[4]，M1.0 落盘口径） |
| 概念 | kb/index.html#nanite（01 章 GPU-driven 节） |

## 底线

- 剔除 compute 必须是**无状态可重复**的（每帧从头算，不依赖上帧结果）——S5 的 DAG 遍历会替换它，接口今天留好
- readback 一律延迟帧，主循环内禁止等待型 map
- 每帧剔除结果只影响 instanceCount 与可见列表，**不重建任何 buffer**（所有 GPU 资源 M4.0 一次建好、帧间复用）
- 双后端画面一致性按三档口径记录，剔除集合相同前提下 pulling 顶点序差异不应产生可见差异
