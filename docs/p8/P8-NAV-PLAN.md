# P8-NAV-PLAN — 寻路系统（vendored Recast/Detour，A 类并行轨道）

> **2026-11-09 方案变更拍板（覆盖 2026-11-08「从零写」）**：用户拍板寻路**直接接现成 Recast/Detour 方案**——理由：① 工具链成熟（上游 `DebugUtils` 分步可视化 + `RecastDemo` 参数调试）；② **要和编辑器结合**（烘焙参数调参 → 重烘焙 → 可视化闭环）；③ 从零写工作量太大。
> **原「从零写」6 步阶梯作废留痕**：体素栅格 → 开放高度场 → 距离场/区域 → 轮廓 → 多边形 NavMesh → A\* + funnel（旧 M-N0~N5）。Carrot `engine/pathfinding/` 2336 行从零实现不再作实现参照，降级为**验收口径 / 编辑器交互参照**（`debugDraw(DebugDrawType)` 分步可视化与 `BuildParams` 参数集合仍值得抄思路）。
>
> 轨道归属：`NON-RENDER-ROADMAP.md` 候选总表「寻路」行。执行排期**不变**：放到场景整合之后，依赖 P8 E1（ECS-lite 场景：多物体 / 多光源 / 变换层级）与 P9 V3（多物体真实场景）；先立项出计划，执行在 P8 E1 落地后启动。
> 届时按 `AGENTS.md` 并行轨道纪律新建 `feat/pathfinding` worktree（junction 共享 `.workbuddy` + `slang/dist`），验收走**工具版交代**（A 类：不进 `kStage`、不进渲染回归）。

## 性质

- **vendored 库 + 自写集成层**：NavMesh 生成（Recast）与运行时查询（Detour）由上游提供；我们写的是「几何喂给 `rcConfig` → 烘焙产物持久化 → `dtNavMeshQuery` 封装 → 与 `nl/scene` 实体挂接 → 编辑器调参可视化」这一层。
- 上游 Recast/Detour **零依赖**（官方 README：只需 C++98 编译器），与本项目「构建链不依赖任何厂商图形 SDK」铁律天然兼容。
- 编辑器结合是本轮拍板的核心诉求：复用已落地的 `viewer --editor` ImGui 基建（材质图 T1）与 console cvar（M-C2 `installBuiltins` + onChanged 回调），做「调参 → 重烘焙 → 分步可视化」闭环。

## 三处参考口径 + 上游方案（2026-11-08 实读 / 2026-11-09 复核，执行各里程碑时按铁律重读点名文件）

| 来源 | 内容 | 用法 |
|---|---|---|
| **上游 `recastnavigation`**（新增主口径） | **ZLib license**（兼容本项目 MIT）；最新稳定 release **1.6.0（2023-05-21）**；模块：`Recast/`（体素化生成）、`Detour/`（运行时加载/寻路/查询）、`DetourTileCache/`（tiled 流式与增量重烘）、`DetourCrowd/`（群体避障）、`DebugUtils/`（`duDebugDraw*` 调试可视化 API）、`RecastDemo/`（独立 demo）；官方标榜 Industry Standard（Unity/Unreal/Godot/O3DE 均在用）；Recast & Detour 零依赖 C++98 | **实现本体**（vendored）；`Sample_SoloMesh.cpp` = 烘焙入门、`NavMeshTesterTool.cpp` = 查询入门、`Docs/_2_BuildingAndIntegrating.md` = 集成指南；`RecastDemo` 只抄交互思路不 vendored 本体 |
| **UE** `Runtime/Navmesh` | `Recast.tps` 实证 `<Name>Recast v1.4.2</Name>`，`Private/Recast/Recast.cpp` 有 `rcCreateHeightfield`；`Runtime/NavigationSystem` 是引擎侧封装；工业终态 = **直接证明「接现成方案」就是工业做法** | 只读对照（EULA，一行不进 repo）。注意：UE `ThirdParty/Detours` 是 Microsoft Detours（API 钩子），与导航无关，勿混；UE 锁 1.4.2，与上游 1.6.0 的 API/行为差异开工时留痕 |
| **Carrot** `engine/pathfinding/`（2336 行，纯从零） | `SparseVoxelGrid` → `NavMeshBuilder`（`buildOpenHeightField` → `buildDistanceField` → Region → Contour → 多边形网格；`BuildParams{voxelSize, characterHeight/Radius/maxClimbHeight}`）→ `AStar` → `NavPath`；自带 `debugDraw(DebugDrawType)` 分步可视化 | **降级为验收/交互参照**：分步可视化的阶段划分、`BuildParams` 参数集合 → 编辑器面板字段；**实现不参照**（用户已拍板走上游） |
| **bevy** | 无原生寻路 crate（生态 `bevy_pathfinding`） | 据实标注，不作口径 |

## 架构约束（2026-11-09 定）

- **vendored 落点**：`thirdparty/recastnavigation/`，取 `Recast/` + `Detour/` + `DebugUtils/`（M-N0）；**M-N3 按 2026-11-11 拍板补入 `DetourTileCache/`（2 头 + 2 .cpp，同一 v1.6.0 锁定 commit `6dc1667f`）**。`DetourCrowd/`（M-N4 可选）与 `RecastDemo/`（依赖 SDL/OpenGL，违反构建链纪律）、`Tests/` 仍不入库。CMake 子目录接入；锁 release tag + commit，升级单独走一轮验收（照 `P8-PHYS-PLAN.md` Jolt 先例）。
- **集成层自写**：`core/include/nl/nav/` + `core/src/nav/`（对齐 `nl/scene` / `nl/anim` 先例），零图形符号（NVRHI/ImGui 零 include）；可视化挂 `viewer/debugDraw` 与编辑器 ImGui 层。
- **编辑器结合**：`rcConfig` 参数（cellSize/cellHeight、walkableSlopeAngle、walkableHeight/Climb/Radius、minRegionArea、maxEdgeLen/maxEdgeError、maxVertsPerPoly、detailSampleDist/MaxError、tileSize）暴露为 console cvar + ImGui 面板；`DebugUtils` 的 `duDebugDraw*` 输出体素/高度场/区域/轮廓/多边形五阶段叠加图；一键重烘焙走同一 `nav-tools bake` 入口（编辑器与命令行共用，证据图取工具真实输出）。
- **不做**（防范围失控）：改动上游内部流水线（vendored 后只调参数与用法）/ 自研体素化 / 重写 A\* / `DetourCrowd` 群体避障 / off-mesh connection（跳跃、传送门）——后两项列为可选升级项，默认不做。

## 里程碑阶梯（旧 6 步作废，新编号沿用 M-N，2026-11-09 重定义）

| 里程碑 | 内容 | 可见验收点 |
|---|---|---|
| **M-N0** | vendored + 最小烘焙闭环：`.nmesh`/glTF 几何 → `rcConfig` → Recast Solo Mesh 流程 → `dtNavMesh` 序列化/加载；`nav-tools bake` / `dump` | 单模型烘焙无报错退出；多边形数/面积统计 dump；NavMesh 线框 PNG（真实工具输出） |
| **M-N1** | Detour 查询封装：`dtNavMeshQuery::findPath` + `findStraightPath`、路径平滑、负向语义（不可达 / 超出节点预算 → 明确错误码） | 两点寻路折线 → 平滑路径；绕障用例；不可达返回明确失败；selftest 确定性逐点一致 |
| **M-N2** | **编辑器结合（本轮拍板核心）**：烘焙参数 cvar + ImGui 面板 + `DebugUtils` 五阶段可视化 + 调参重烘焙闭环 | `viewer --editor` 内改参数 → 重烘焙 → 各阶段叠加图；console `cvar.set` 改参数生效；输出 PNG 证据图亲眼核验 |
| **M-N3** | 场景集成：E1 多物体场景烘焙；动态物体走 `DetourTileCache` tiled 增量重烘（**2026-11-11 拍板：方案 A，不再保留「或全量重烘」备选**；`.nnav` 容器按 **A-1** 语义升级 v2，v1 向后兼容） | E1 场景（50 物体）NavMesh 正确；动态障碍增量更新无洞；展示台 `kind:"tool"` 卡（usage / 正向 / 负向）全绿 |
| **M-N4**（可选，默认不做） | `DetourCrowd` 群体避障 / off-mesh connection | 视后续游戏需求另立项，不在本轨道交付范围 |

## 验收纪律

- 工具版交代：`nav-tools`（bake / query / dump）+ 展示台 `kind:"tool"` 用例（usage / 正向 / 负向）+ `NAV-TOOLS-GUIDE.md` 归档；证据图从工具真实输出生成，不许手绘。
- **确定性是硬判据**：同场景同参数的 NavMesh 产物逐字节（MD5）一致；`findPath`/`findStraightPath` 结果路径逐点一致。上游 `rcBuild*` 为单线程流水线，**线程数口径锁死**，不引入上游内部并行。
- **Job 系统合流口径修订**：原「`parallel_for` 分块体素化」作废（vendored 后不改上游内部流程）；合流仅用于 **多 tile 并行烘焙**（每 tile 一次 `rcBuild`，tile 间独立），逐 tile 产物与单线程 MD5 一致。
- `nl/nav/` 零图形符号；可视化挂 viewer/debugDraw 与编辑器层；A 类轨道不碰 `kStage`、不进渲染回归。
- vendored 纪律：锁版本、不依赖图形 SDK、排除 `RecastDemo`、上游升级单独一轮验收；UE 1.4.2 与上游 1.6.0 差异留痕。
- merge 回 master 前过 §7.2 文档闸 + §7.5 关联文档同步。

## 排期约束（拍板留痕）

**执行不提前**：NavMesh 烘焙吃场景几何（P8 E1 变换层级 / 多物体），提前做只能在单一合成网格上验证，验收价值打折。启动时机 = P8 E1 验收通过之后；届时新建 worktree 并更新本文件与注册表（2026-11-09 方案变更**未改排期**）。
**前置就绪现状**：编辑器结合所需的 `viewer --editor` ImGui 基建（材质图 T1，已合入 master）与 console cvar（M-C2，已合入 master）均已就位；E1a 内核已合入（`2d4ad49`），E1b 渲染集成进行中。

> **2026-11-10 开工登记（排期闸解除）**：E1a+E1b 链已全量 merge 回 master（2026-11-09，tags `ecs-e0`~`ecs-e1b.5`），启动条件满足。用户指令开工，worktree `E:/UE_STUDY_2026/nl-engine-pathfinding`（分支 `feat/pathfinding`，起点 master `7fd867b`）已建，junction 共享 `.workbuddy` / `slang dist`；wt_pool 已 claim **M-N0**。本条更新后，上方「E1b 渲染集成进行中」为过时留痕，不再修改。

> **2026-11-10 M-N0 收口 + M-N1 延后拍板**：M-N0（vendored Recast/Detour 1.6.0 + nl/nav 烘焙闭环 + nav-tools 五子命令）已在 `feat/pathfinding` 收口（tag `nav-m0`，commit `bade82f`；merge 回 master 待 M8.1 会话提交后补做）。用户拍板：**M-N1（findPath/findStraightPath 封装）延后**，不单独排期——查询 API 本身不依赖大场景（合成网格即可跑通），等 M-N2 编辑器可视化或 M-N3 E1 场景集成需要查询能力时**按需插入**，收益最高。阶梯顺序变为 M-N0 →（M-N2 / M-N3 按场景就绪节奏推进，M-N1 随需补）。

> **2026-11-11 M-N1 启动（延后解除）**：用户指令「开启pathfinding M-N1」= 解除上方延后拍板，M-N1 按原阶梯行执行（Detour 查询封装 + 负向语义 + 绕障用例）。worktree 重建（ff 同步 master `991e36f`）+ wt_pool claim M-N1 已登记；M-N0 已确认 merge 回 master（`core/src/nav/` 在 master）。实现口径：`findStraightPath` string-pulling 即计划「路径平滑」语义（不引入 Catmull-Rom 类额外平滑）；`DT_PARTIAL_RESULT` 不静默折算、透传调用者（`QueryStatus::UNREACHABLE`/`OUT_OF_NODES`）。收口 tag `nav-n1`。

> **2026-11-11 M-N2 启动（口径拍板）**：用户拍板「先不外置，直接开工」——M-N2「编辑器结合」维持 **viewer `--editor` 内嵌 ImGui 面板口径，不挂 feat/editor 外置编辑器**（nl/nav 零图形符号纪律保证后续外置编辑器可直接迁移消费）。呈现形态按计划语义内落地：viewer 面板 = ImDrawList 2D 俯视画布（材质图编辑器同基建）；PNG 证据 = nav-tools `debug-stage` 软光栅（「证据图取工具真实输出」既定口径）。收口 tag `nav-n2`（详见 NAV-TOOLS-GUIDE §7/§8）。

> **2026-11-11 M-N3 启动（开工闸拍板，方案 A + A-1）**：计划 M-N3 行原挂账「或全量重烘，开工前按上游口径定」属未拍板架构判定，已按三处实读 + 基准数据摆方案交用户拍板，用户指令「按建议来」= ① **方案 A：DetourTileCache tiled 增量重烘**（上游 1.6.0 `Sample_TempObstacles.cpp` 官方样板 + UE `FRecastTileGenerator`/`FTileCacheCompressor` 实锤同方案；全量重烘实测 4 物体 HizScene ≈500ms/次，50 物体外推 2~4s/次不可用）；② **A-1：`.nnav` 容器语义升级 v2**，magic 与 version 字段分流，loader 兼容 v1（M-N0~M-N2 老基线字节零回归）。三处证据：上游 `DetourTileCache.h`（`addTile`/`addObstacle`/`removeObstacle`/`update`/`buildNavMeshTilesAt`）+ `Sample_TempObstacles.cpp`（`rasterizeTileLayers` 逐 tile 切片 + tcparams）、UE `RecastNavMeshGenerator.cpp:1558/1655/2400`（fork 的同套 dtTileCache 接口）、Carrot 自研静态 AStar 无动态导航不作口径。交付边界（计划语义内）：E1 场景烘焙 = `nl::scene` World JSON → nav 几何汇集（吃组件 + 层级变换，50 物体锚）；动态障碍演示 = selftest（加障碍→查询被挡→移除→恢复逐点断言）+ 工具卡；**viewer 面板本轮不加障碍交互按钮**（M-N2 面板闭环已收官，属范围扩展，留用户点名再做）。冻结面：上游 vendored 四个新文件零编辑；v1 序列化/烘焙老路径一行不改；M-N2 五阶段自绘 PNG 基线锁定（`DetourDebugDraw.cpp` 依赖虽已满足仍不启用，避免动 M-N2 字节）；kStage 不 bump、外置编辑器不碰。收口 tag `nav-n3`。

> **2026-11-11 M-N3 收官（tag `nav-n3`）**：开工闸拍板的交付边界与冻结面逐条兑现。① 方案 A：`DetourTileCache/` 4 文件按锁定 commit `6dc1667f` 零编辑入库，逐 tile `rcBuildHeightfieldLayers` → `dtBuildTileCacheLayer` → `dtTileCache::addTile` → `buildNavMeshTilesAt`；三个注入接口自写，零新依赖。② A-1：`.nnav` magic/基础段不变、`version=2` 尾部追加 tiled 扩展段，loader 兼容 v1；**solo bake 产物与 `example/nav-m0/hiz-a.nnav` 864 字节逐字节一致**（v1 老路径一行不改实锤）。③ E1 场景：`SceneGeo.cpp` 吃 World JSON 组件 + `getWorldMatrix()` 父子复合，自造含可走面的 `nav-n3.scene`（3 entity / 8 tiles / 12 polys）+ 现成 `ecs-e1b-scale.scene` 50 entity 规模锚（曲面模型 `tiles=0` 属几何语义，非故障）。④ 动态障碍：selftest + `obstacle` 工具卡闭环（polys 4→3 → UNREACHABLE → 移除后折线逐点恢复），viewer 面板按拍板边界不加交互按钮。⑤ 冻结面：vendored 零编辑、M-N2 五阶段 PNG 基线锁定、`DetourDebugDraw.cpp` 继续不启用、`kStage` 仍 S8.2、外置编辑器不碰。验收：双后端 BUILD OK + viewer selftest 16/16×2、nav-tools selftest 全绿（新增 21 项锚点）、ctest `m-n[0123]-nav` 24/24 ×2、`regression` nav-n3 22/22 + 三张老卡 10/10·12/12·10/10、`server.py --check` exit 0、`example/nav-n3/run.bat` ALL GREEN、cases.json `nav-n3` 11 卡。详见 `NAV-TOOLS-GUIDE.md` §9/§10 与 `MILESTONES.md` M-N3 四栏。下一站 M-N4（`DetourCrowd/` 多 Agent）为可选项，若要启动须另起开工闸（vendored 子集 + 三处实读 + 用户拍板）。

