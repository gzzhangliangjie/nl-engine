# P8-NAV-PLAN — 寻路系统（NavMesh + A*，A 类并行轨道，2026-11-08 立项）

> 轨道归属：`NON-RENDER-ROADMAP.md` 候选总表「寻路」行（★★★★）。用户拍板（2026-11-08）：**从零写**（总原则「有参考的内容就考虑从头写」，Carrot 从零参照现成）。
> **执行排期：放到场景整合之后**——依赖 P8 E1（ECS-lite 场景：多物体 / 多光源 / 变换层级）与 P9 V3（多物体真实场景）。先立项出计划，执行在 P8 E1 落地后启动。
> 届时按 `AGENTS.md` 并行轨道纪律新建 `feat/pathfinding` worktree（junction 共享 `.workbuddy` + `slang/dist`），验收走**工具版交代**（A 类：不进 `kStage`、不进渲染回归）。

## 性质

- NavMesh **生成是离线管线**（体素化 → 高度场 → 区域 → 轮廓 → 多边形网格），与 `.nmesh` 离线链路同源，可与 Job 系统（`docs/p8/P8-JOB-PLAN.md`）合流并行化。
- 寻路查询（A*）与路径平滑是**运行时纯 CPU 逻辑**，确定性可单测。
- 可视化验收：NavMesh 各生成阶段分步 debugDraw（对照 Carrot `NavMeshBuilder::debugDraw(DebugDrawType)` 设计），寻路结果画路径线。

## 三处参考口径（2026-11-08 实读，执行各里程碑时按铁律重读本里程碑点名文件）

| 来源 | 内容 | 用法 |
|---|---|---|
| **Carrot** `engine/pathfinding/`（2336 行，纯从零） | `SparseVoxelGrid`（稀疏体素栅格，85+84 行）→ `NavMeshBuilder`（`buildOpenHeightField` → `buildDistanceField` → Region → Contour → 多边形网格；`BuildParams{voxelSize=0.5, characterHeight/Radius/maxClimbHeight}`）→ `NavMesh` → `AStar`/`AStarImpl` → `NavPath`；自带 `debugDraw(DebugDrawType)` 分步可视化 | **节奏参照首选**，抄思路不抄代码 |
| **UE** `Runtime/Navmesh`（Recast v1.4.2 + Detour，license 已确认） | 工业终态：体素化 → filter → region → contour → poly mesh → detail mesh；`Runtime/NavigationSystem` 封装 | 只读对照（EULA，一行不进 repo）。注意：UE `ThirdParty/Detours` 是 Microsoft Detours（API 钩子），与导航无关，勿混 |
| **bevy** | 无原生寻路 crate（生态 bevy_pathfinding） | 据实标注，不作口径 |

## 里程碑阶梯（每步可见验收；编号 M-N，执行时按实际情况细化）

| 里程碑 | 内容 | 可见验收点 |
|---|---|---|
| **M-N0** | 稀疏体素栅格（场景几何 → voxel span） | 体素切片可视化（某高度层 voxel 图导出 PNG） |
| **M-N1** | 开放高度场 + 过滤（角色半径/攀爬高度/高度） | 高度场着色图（可行走 span 着色） |
| **M-N2** | 距离场 + 区域划分（watershed/泛洪） | 区域 ID 着色图，区域数与手工预估一致 |
| **M-N3** | 轮廓提取 + 简化 → 多边形 NavMesh | NavMesh 线框叠加在场景上；多边形数 / 面积统计 |
| **M-N4** | A* 寻路 + 路径平滑（string pulling / funnel） | 两点寻路画折线 → 平滑曲线；绕障用例；负向（不可达 → 明确返回） |
| **M-N5** | 与场景集成：多物体场景重建 NavMesh + 动态物体增量处理 | E1 场景（50 物体）NavMesh 正确；展示台 tool 卡全绿 |

## 验收纪律

- 工具版交代：`nav-tools`（生成 / 查询 / dump）+ 展示台 `kind:"tool"` 用例（usage / 正向 / 负向）+ `NAV-TOOLS-GUIDE.md` 归档；证据图从工具真实输出生成，不许手绘。
- **确定性是硬判据**：同场景同参数的 NavMesh 产物逐字节（MD5）一致；A* 结果路径逐点一致。
- 生成离线部分可与 Job 系统合流（`parallel_for` 分块体素化），并行产物与单线程 MD5 一致。
- `core/nav/`（或届时 `nl/nav/`）零图形符号；可视化挂 viewer/debugDraw 层。
- merge 回 master 前过 §7.2 文档闸 + §7.5 关联文档同步。

## 排期约束（拍板留痕）

**执行不提前**：NavMesh 生成吃场景几何（P8 E1 变换层级 / 多物体），提前做只能在单一合成网格上验证，验收价值打折。启动时机 = P8 E1 验收通过之后；届时新建 worktree 并更新本文件与注册表。
