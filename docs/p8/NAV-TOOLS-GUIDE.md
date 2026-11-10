# NAV-TOOLS-GUIDE — nav-tools 工具归档（P8 寻路轨道）

> 轨道：`feat/pathfinding`（A 类并行轨道）。计划：`docs/p8/P8-NAV-PLAN.md`。
> vendored 溯源：`thirdparty/recastnavigation/VENDOR.md`（Recast/Detour 1.6.0，ZLib，锁 commit `6dc1667f…`）。

## 1. 定位与纪律

- `nav-tools` = 寻路系统无头验收工具（A 类轨道工具版交代）。只链 `nl_core`（`nl/nav` PIMPL 把 Recast/Detour 头挡在 `core/src/nav/Nav.cpp` 内），零图形 API，不进 `kStage`、不进渲染回归。
- 退出码契约（对齐 phys-tools / 展示台 `expectExit`）：**0 = PASS / 1 = 断言或运行失败 / 2 = 用法错误**。
- 确定性硬判据：上游流水线锁单线程；同几何同参数产物**逐字节一致**（run.bat 用 `fc /b` 机器断言，产物指纹 fnv64 打进 dump 输出）。
- 证据规则：`example/nav-m0/nav-wire.png` 由 `nav-tools wire` 从真实烘焙产物生成，不许手绘。

## 2. 子命令

| 命令 | 作用 | 退出码 |
|---|---|---|
| `bake <in.glb\|.nmesh> <out.nnav> [--cell-size F] [--cell-height F] [--slope F] [--agent-height F] [--agent-radius F] [--agent-climb F] [--min-region F] [--merge-region F]` | 几何 → rcConfig → Recast Solo Mesh → `dtNavMesh`，写 NLNV 容器 | 0/1/2 |
| `dump <in.nnav>` | 读回容器 + 统计（polys/verts/detail tris/area/bounds/fnv64） | 0/1 |
| `wire <in.nnav> <out.png> [--size N]` | 俯视正交多边形边界线框 PNG（证据图生成器） | 0/1/2 |
| `resave <in.nnav> <out.nnav>` | load → save roundtrip（容器自检） | 0/1 |
| `selftest [--verbose]` | 合成场景 bake / 确定性 / roundtrip / 负向断言 | 0/1/2 |

- 几何入口：`.glb`（`loadGltfScene` 全 primitive 拼接）或 `.nmesh`（v3 fallback 尾段；v1 base 无 fallback 会明确报错）。
- 失败诊断链：bake 失败时打印 `walkableTris / grid WxH / rasterSpans / filteredSpans / compactSpans / regionCount`，逐级定位死点（M-N0 调参实测：HizScene 在 cs=0.3 下 erosion 半径吃光箱顶，cs=0.1 + radius 0.2 才有产物）。

## 3. M-N0 验收记录（2026-11-10）

- **资产口径**：HizScene.glb（真实场景资产）`--cell-size 0.1 --agent-radius 0.2` → 4 多边形 / 16 顶点 / 0.50 m²（1m 箱顶可站面，0.5m 半径 Agent 站不上 = 正确腐蚀）。
- **确定性**：两次 bake `fc /b` 逐字节一致，fnv64 `5e70947ba37dcd35`。
- **roundtrip**：`resave` 产物与原文件逐字节一致。
- **证据图**：`nav-wire.png` 512×512 俯视正交，4 个箱顶多边形轮廓与 polys=4 一致（肉眼核验）。
- **交代物**：`example/nav-m0/`（run.bat ALL GREEN + hiz-a.nnav + nav-wire.png + selftest/bake/dump/wire/usage/negative 六份日志）+ 展示台 `nav-m0` 卡五用例（usage/selftest/bake/dump/negative，expectExit 2/0/0/0/1）。
- **ctest**：`m-n0-nav-selftest` / `m-n0-nav-bake` / `m-n0-nav-dump` / `m-n0-nav-negative`（WILL_FAIL）。

## 4. M-N0 踩坑（写实）

1. **上游 1.6.0 `dtNavMesh::getTile(int)` 非 const 重载是 private**——非 const 指针调用命中 private 版直接 C2248；读侧必须走 const 指针（公开的 `getTile(int) const`）。`getParams()` 返回指针非出参。
2. **上游 1.6.0 `DebugUtils/DetourDebugDraw.cpp` 头依赖 `DetourTileCacheBuilder.h`**——TileCache 未入库该 TU 编不过；已从构建剔除（VENDOR.md 留痕），M-N2 NavMesh 叠加自绘。
3. **tile 内存所有权铁律（本轮最重坑）**：`load()` 把 `std::vector` 内存用 `DT_TILE_FREE_DATA` 移交 Detour → Detour 析构 `dtFree`（malloc 语义）对 `operator new` 内存 → 分配器不匹配 + UAF，症状 = resave 确定性 0xC0000409 秒退 + selftest roundtrip 字节差。修复 = tile 缓冲由 `Impl::ownedTiles` 持有、addTile flags=0；bake 路径 `dtCreateNavMeshData` 产物本就是 `dtAlloc` 的，`DT_TILE_FREE_DATA` 移交合法。
4. **minRegionArea 口径**：RecastDemo 默认 `regionMinSize=8`（64 格²）假设几十米大地图；本项目 2~3m 测试资产的可走区全被丢光（"no walkable contours"）。默认下调为 2/6 并 CLI 暴露。
5. **网格分辨率 vs 腐蚀半径**：cs=0.3 时 2m 球的 7×7 网格被 radius-2 腐蚀清空；cs=0.1 直接通过。粗网格 + 大腐蚀半径 = 小资产必死，不是管线 bug（诊断链定位）。
6. **本机 exe 秒退家族再+1 佐证**：resave 曾 5 连 127 零输出——先按 flake 重跑，5/5 复现后定性为真崩溃（UAF），「先重跑再定性」纪律双向都用上了。
7. **存量债修复**：`viewer/main.cpp` selftest 锚点硬编码 "S7.2"，M8.0 bump kStage 后从未重跑（s8-blas 收口漏网）→ 修为跟随 "S8.0"；**每次 kStage bump 必须同步改该锚点并重跑 `viewer --selftest`**。

## 5. 下一站

- M-N1：`dtNavMeshQuery::findPath` + `findStraightPath` 封装、负向语义（不可达/超节点预算错误码）、绕障用例、确定性逐点一致。
- M-N2：烘焙参数 cvar + ImGui 面板 + 五阶段可视化 + 调参重烘焙闭环（本轮拍板核心）。
- M-N3：E1 多物体场景集成；动态物体走 `DetourTileCache`（届时连 `DetourDebugDraw` 一起重评）。
