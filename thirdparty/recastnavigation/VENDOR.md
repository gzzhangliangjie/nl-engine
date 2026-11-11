# Vendored third-party: Recast & Detour (recastnavigation) 1.6.0

## Identity

| item | value |
|---|---|
| upstream | recastnavigation/recastnavigation (https://github.com/recastnavigation/recastnavigation) |
| version | **v1.6.0**（tag，2023-05-21 上游 release；锁定 commit `6dc1667f580357e8a2154c28b7867bea7e8ad3a7`） |
| license | **ZLib**（`License.txt`，Copyright (c) 2009 Mikko Mononen）——允许商用/修改/再分发，与 nl-engine MIT 兼容；ZLib 无显式专利授权条款（与 MIT 同），如实记录，不宣称「绝对零风险」 |
| vendored on | 2026-11-10, P8 M-N0 |
| obtained | `git clone --depth 1 --branch v1.6.0 --filter=blob:none https://gh-proxy.com/https://github.com/recastnavigation/recastnavigation`（GitHub 直连断，走 gh-proxy 镜像拉取，clone 后校验 HEAD == 锁定 commit） |
| 上游地位 | 官方标榜 Industry Standard（Unity/Unreal/Godot/O3DE 均在用）；零依赖 C++98，符合「构建链不依赖任何厂商图形 SDK」铁律 |

## What is vendored

- `Recast/{Include,Source}` — NavMesh 生成（体素化 → 高度场 → 区域 → 轮廓 → 多边形网格），10 个 .cpp + 头，无编辑。
- `Detour/{Include,Source}` — 运行时加载 / 寻路 / 查询（dtNavMesh / dtNavMeshQuery），7 个 .cpp + 头，无编辑。
- `DebugUtils/{Include,Source}` — `duDebugDraw*` 调试可视化 API（M-N2 五阶段叠加图消费），4 个 .cpp + 头，无编辑。
  - **例外：`DebugUtils/Source/DetourDebugDraw.cpp` 仍未编入**（文件已入库但排除在构建外）：M-N0 剔除理由 = 头依赖 `DetourTileCacheBuilder.h` 而当时 TileCache 未入库；**M-N3 TileCache 已入库、该依赖已满足，但重评结论 = 继续不启用**——M-N2 NavMesh 阶段自绘 PNG 基线字节已锁定，启用上游 TU 会改动 NavMesh 叠加图输出。若将来确需 `duDebugDrawNavMesh*` 全套，单独一轮验收 + 重拍基线（升级纪律同款流程）。
- `DetourTileCache/{Include,Source}` — **M-N3 补入**（2026-11-11 用户拍板方案 A）：`DetourTileCache.h` / `DetourTileCacheBuilder.h` / `DetourTileCache.cpp` / `DetourTileCacheBuilder.cpp` 共 4 文件，取自同一锁定 commit `6dc1667f`（本地只读 clone `E:/UE_STUDY_2026/_tmp_recast` 实证 HEAD），**无编辑**，ZLib 同一 `License.txt` 覆盖。用途 = tiled 增量重烘 + 动态障碍（`addObstacle`/`removeObstacle`/`update`）。
- `DetourCrowd/{Include,Source}` — **M-N4 补入**（2026-11-11 开工闸 Q1=A 拍板）：`DetourCrowd.h` / `DetourLocalBoundary.h` / `DetourObstacleAvoidance.h` / `DetourPathCorridor.h` / `DetourPathQueue.h` / `DetourProximityGrid.h` 共 6 头 + 同名 6 个 .cpp 共 **12 文件**，取自同一锁定 commit `6dc1667f`（本地只读 clone 实证 HEAD），**无编辑**，ZLib 同一 `License.txt` 覆盖。用途 = `dtCrowd` 多 Agent 群体避障（路径走廊 + 局部边界 + RVO 速度避障 + 邻近网格 + 异步路径队列）。**零新依赖**：逐个 TU 核 `#include` 只指向已入库 `Detour/Include`（`DetourNavMesh.h`/`DetourNavMeshQuery.h`/`DetourCommon.h`/`DetourMath.h`/`DetourAssert.h`/`DetourAlloc.h`）与 C 标准头（`string.h`/`float.h`/`stdlib.h`/`new`），无 SDL/OpenGL/厂商 SDK。
- `License.txt` — 上游许可证原文，无编辑。
- `CMakeLists.txt` — **nl-engine 自写薄脚本**（唯一非上游文件）：单静态目标 `nl_recast`，源文件显式枚举。不直接 `add_subdirectory` 上游模块脚本的原因：上游脚本带 SOVERSION/LIB_VERSION（依赖仓库根变量）、install() 导出，且 DebugUtils 链接 `DetourTileCache`（M-N0 时未入库；M-N3 已补入但仍维持薄脚本，避免引入上游 install/导出链）。

Intentionally **not** vendored：
- `RecastDemo/` — 依赖 SDL/OpenGL，违反构建链纪律；`Sample_SoloMesh.cpp`（烘焙流水线样板）与 `NavMeshTesterTool.cpp`（查询样板）**只读作参照，不抄代码**。
- `Tests/`、`Docs/`（doxygen 工程）、`.github/`、根 CMakeLists 等仓库管理文件。

## Build integration (root `CMakeLists.txt`)

- `add_subdirectory(thirdparty/recastnavigation EXCLUDE_FROM_ALL)` → 静态目标 `nl_recast`；`nl_core` 以 PRIVATE 链接（对齐 Jolt 先例），`nav-tools` 显式链 `nl_recast`。
- 纯 CPU、无厂商 SDK、无网络、无新运行时 DLL；MSVC 14.38 / C++17 编译通过（上游 C++98 源码向下兼容）。
- **新 checkout 必验 configure**（jolt `.gitignore` 撞 `build/` 教训推广）：M-N0 在 `nl-engine-pathfinding` wt 全新 configure + build 验证。

## 与 UE vendored 版本的口径留痕（P8-NAV-PLAN 纪律）

- UE 5.8.4 `Runtime/Navmesh/Recast.tps` 实证锁 **Recast v1.4.2**；上游已到 1.6.0。
- 本轨道集成层只使用两版同名同签的稳定 API（实读核对）：Solo Mesh 流水线（`rcCreateHeightfield` / `rcRasterizeTriangles` / `rcBuildCompactHeightfield` / `rcErodeWalkableArea` / `rcBuildDistanceField` / `rcBuildRegions` / `rcBuildContours` / `rcBuildPolyMesh` / `rcBuildPolyMeshDetail`）与 Detour（`dtCreateNavMeshData` / `dtNavMesh::init` / `addTile` / `dtNavMeshQuery`）。`dtCreateNavMeshData` 在 UE 1.4.2 `DetourNavMeshBuilder.h:166` 与上游 1.6.0 同签名。
- 1.4.2 → 1.6.0 间的上游修复/行为变化不在本项目背书范围：我们只以 vendored 1.6.0 自身产物为准，不引用 UE 行为做验收口径。

## Determinism contract

- 上游 `rcBuild*` 为单线程流水线；集成层**锁单线程**（不引入上游内部并行），线程数口径进冻结契约。
- 同场景同参数：NavMesh 产物（tile 原始字节）逐字节 MD5 一致；`findPath`/`findStraightPath` 结果逐点一致（M-N1）。
- Job 系统合流只做**多 tile 并行烘焙**（tile 间独立，逐 tile 产物与单线程 MD5 一致），不并入 M-N0。

## Upgrade discipline

版本**锁死 1.6.0**。升级单独走一轮验收（照 Jolt 先例）：重 vendored → `nav-tools selftest` → 重出 bake/dump 产物 MD5 + 证据 PNG → 展示台卡复核。绝不静默 bump。
