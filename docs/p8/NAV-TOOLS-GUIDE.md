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
| `query <in.nnav> --start x,y,z --end x,y,z [--max-nodes N] [--max-waypoints N] [--half-extents x,y,z] [--png out.png] [--size N]` | M-N1：`findNearestPoly` → `findPath`（走廊）→ `findStraightPath`（string-pulling 折线）；打印 status + reachedLimit + 逐点 waypoints，可叠画路径 PNG | 0=OK / 1=UNREACHABLE·OUT_OF_NODES·BUFFER_TOO_SMALL / 2=INVALID_PARAM·NOT_INITIALIZED·用法错 |
| `resave <in.nnav> <out.nnav>` | load → save roundtrip（容器自检） | 0/1 |
| `debug-stage <in.glb\|.nmesh> <out.png> --stage <voxels\|regions\|distance\|contours\|raw-contours\|polymesh\|navmesh> [--size N] [bake params...]` | M-N2：五阶段叠加图——`bake(keepDebug=true)` 保留 rc 中间产物 → `duDebugDraw` 图元收集 → 俯视软光栅 PNG（TRI 边函数填充 + QUADS 拆两三角 + alpha 混合）；证据图取工具真实输出 | 0/1/2 |
| `tilebake <in.glb\|.nmesh\|.scene> <out.nnav> [--tile-size N] [--max-obstacles N] [--png out.png] [--size N] [bake params...]` | M-N3：`DetourTileCache` tiled 增量烘焙，写 `.nnav` **v2**（基础段同 v1 + 尾部 tiled 扩展段）；`.scene` 走 `nl::scene` World JSON 几何汇集（组件 + 父子世界矩阵复合，按 meshFile 缓存 glb）；`--png` 叠画淡蓝 tile 网格 | 0/1/2 |
| `obstacle <in.nnav> --pos x,y,z --start x,y,z --end x,y,z [--radius F] [--height F] [--png out.png] [--size N]` | M-N3：动态障碍闭环——原路径查询 → `addObstacle` → 被挡查询 → `removeObstacle` → 恢复查询；打印 polys/edges 变化与 `result : PASS`。蓝线=无障碍原路径、红线=障碍后路径、绿圈=圆柱障碍俯视 | 0/1/2 |
| `crowd [--arena \| <in.nnav>] [--spawn x,y,z:x,y,z]... [--preset crossing\|line\|none] [--frames N] [--dt F] [--max-agents N] [--agent-radius F] [--speed F] [--arrival F] [--trace out.ntraj] [--png out.png] [--size N]` | M-N4：`DetourCrowd` 多 Agent 固定步长模拟。`--arena` 用内联合成 20×20 地板 + 中央 2×2×2 障碍（不落盘）；不传则 load `<in.nnav>`（v1/v2 均可）。轨迹写 `.ntraj`（magic `NLTR` + 601×4×3 f32 + fnv64），`--png` 叠画 poly 边界 + 每 agent 一条彩色轨迹折线 | 0/1/2 |
| `selftest [--verbose]` | 合成场景 bake / 确定性 / roundtrip / 负向 / M-N1 查询 / M-N2 调试锚点 / M-N3 tiled·v2·障碍·E1 层级锚点 / M-N4 crowd·`.ntraj`·v1+v2 兼容·七条负向锚点 | 0/1/2 |

- 几何入口：`.glb`（`nl::nav::loadGeometryFromGltf`，M-N2 抽到 core 与 viewer 面板同源）或 `.nmesh`（v3 fallback 尾段；v1 base 无 fallback 会明确报错）。
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

## 4. M-N1 验收记录（2026-11-11）

- **查询封装口径**（`nl/nav/Nav.hpp` `NavMesh::findPath`）：完整链 = 端点吸附（`findNearestPoly`，halfExtents 搜索盒）→ 多边形走廊（`findPath`，maxNodes 节点预算 + maxPathPolys 缓冲）→ string-pulling 折线（`findStraightPath`，即本层「路径平滑」语义；上游 RecastDemo/DetourCrowd 亦只到此为止，不引入 Catmull-Rom 类额外平滑）。
- **明确错误码**（枚举 `QueryStatus`，不静默折算；上游 1.6.0 实据 `DetourNavMeshQuery.cpp:973-1165`）：
  - `OK`：直达终点；若同时带 `OUT_OF_NODES` detail（路径完整但邻居展开受限）→ `OK` + `reachedLimit=true` 单独标记（对齐 UE `SetSearchReachedLimit` 口径）。
  - `UNREACHABLE`：端点吸附不到 poly，或 open list 耗尽不可达（**上游此情形返回 `DT_SUCCESS|DT_PARTIAL_RESULT` 而非 FAILURE**——不可达不是错误，best-effort 前缀折线仍输出）。
  - `OUT_OF_NODES`：partial + 节点池耗尽并存（预算不足以判定可达性）。
  - `BUFFER_TOO_SMALL`：走廊/折线缓冲截断（截断结果仍填充）。
  - `INVALID_PARAM` / `NOT_INITIALIZED`：参数闸 / 未 bake。
  - 对照：Carrot `AStar::findPath` 空 vector 无错误码（本层超越口径）；UE 三态 `ENavigationQueryResult` + `IsPartial()` + partial 折算开关（本层不折算、透传给调用者）。
- **确定性硬判据**：同 mesh 同参数同端点，waypoints 逐点一致（selftest 断言）+ 折线 PNG `fc /b` 逐字节一致（run.bat 断言）。
- **绕障用例**（selftest 合成场景）：直线穿过中央障碍 → 折线 > 2 点、总长 > 直线距、中段避开障碍中心区（机器断言三条）。
- **证据图**：`example/nav-n1/query-path.png` = `nav-tools query --png` 真实输出（512×512 俯视正交，蓝十字折线起点 + 红折线 + 绿十字终点落在箱顶轮廓内）。
- **ctest**：`m-n1-nav-selftest` / `m-n1-nav-bake` / `m-n1-nav-query-ok` / `m-n1-nav-query-unreachable`（WILL_FAIL）/ `m-n1-nav-query-usage`（WILL_FAIL），与 m-n0 共 9/9。
- **交代物**：`example/nav-n1/`（run.bat ALL GREEN + hiz-a.nnav + query-path.png + selftest/bake/query-ok/query-unreachable/query-usage/usage 六份日志）+ 展示台 `nav-n1` 卡六用例（expectExit 2/0/0/0/1/2）。

## 5. M-N1 踩坑（写实）

1. **⚠️ 上游真 bug：`maxNodes < 4` = 段错误（本轮最重坑）**：`dtNavMeshQuery::init` 以 `dtNextPow2(maxNodes/4)` 作 `dtNodePool` hash 桶数，`maxNodes<4` 时该值为 0（`dtNextPow2` 的 `v--` 对 0 下溢回绕）→ `m_first` 0 字节分配 → `getNode` 哈希索引 `m_first[hash & (0-1)]` 越界。RecastDemo 默认 4096 从不触发；selftest `maxNodes=3` 预算用例触发（26 项 ok 后崩，无缓冲 stdout 定位）。修复 = 封装层硬闸 `[4, 65535]`（`INVALID_PARAM`），CLI 同步拦截；selftest 预算用例改 `maxNodes=4`（仍展不开全图走廊，OUT_OF_NODES 语义不变）+ 新增 `maxNodes=1` 负向锚点防回归。
2. **「路径平滑」口径**：`findStraightPath` string-pulling 即计划中的「平滑」（相对多边形走廊口字折线）；不引入额外平滑算法，防范围失控。
3. **HizScene 连通性实测**：4 箱顶 poly 高差超 climb 互不连通——跨区查询天然 UNREACHABLE；连通对（左前区）实测探得 `(-1.9,-2.1)→(-1.0,-1.0)`，ctest 正向用例端点以此固化（expectLog 从实测复制纪律）。
4. **证据图十字标记**：初版蓝十字画原始 start 坐标（吸附点之外的空白区，视觉断开）→ 改标折线两端（吸附后实际起讫点）；原始坐标在 log 的 start/end 行。
5. **run.bat 确定性断言对象**：query log 含 `png : <路径>` 行，两次运行路径不同必不等——确定性断言改用折线 PNG `fc /b`（画布 = f(navmesh, start, end)，比 log 更强）。

## 6. 下一站

- M-N3（**2026-11-11 已收官，tag `nav-n3`**）：方案 A（`DetourTileCache` tiled 增量重烘）+ A-1（`.nnav` v2 同容器语义升级、v1 兼容）已按用户「按建议来」拍板落地，详见 §9/§10。冻结面全部守住：vendored 四文件零编辑、v1 序列化/烘焙老路径一行不改（`hiz-a.nnav` 864 字节逐字节零回归）、M-N2 五阶段 PNG 基线锁定、`kStage` 不 bump、外置编辑器不碰。
- **M-N3 面板补充（2026-11-11 已收官，tag `nav-n3-panel`）**：用户点名解禁「viewer 面板不加障碍交互按钮」的冻结面，双模式分流（`nav.tiled=0` 保 M-N2 / `=1` 走 tiled）+ add/clear + 路径叠加已落地，详见 §11/§11.1。
- 下一站 **M-N4（2026-11-11 已立项并拍板，Q1/Q2/Q3 = A / A-1 / A）**：`DetourCrowd/` 按同一锁定 commit `6dc1667f` 补入（6 头 + 6 源零编辑），`nl::nav::Crowd` + `nav-tools crowd` 工具版交付；`.nnav` 容器不升 v3、渲染基线不碰；固定 dt=1/60 + 固定帧数 + 固定 agent 顺序，`.ntraj` 轨迹 `fc /b` 逐字节可复现。开工闸证据与拍板记录见 `P8-NAV-PLAN.md` 文末，验收记录见本指南 §12。

## 7. M-N2 验收记录（2026-11-11）

- **交付面**：nl/nav 五阶段调试层（`bake(..., keepDebug)` 保留 `rcCompactHeightfield` / `rcContourSet` / `rcPolyMesh` / `rcPolyMeshDetail` + `DebugPrims` POD 图元收集器实现 `duDebugDraw` + `debugDrawStage` 七阶段导出 + `loadGeometryFromGltf` 几何汇集共享层）；nav-tools `debug-stage` 子命令（俯视软光栅 PNG）；renderer `NavPanel`（ImGui 面板 + ImDrawList 画布，NodeEditor 同款封装纪律）；viewer `--nav-panel`（`nav.*` cvar 13 项 + `nav.bake` 命令 + 面板重烘焙闭环）。
- **验收数据**：双后端 ctest `m-n[012]-nav` 15/15；selftest 新增 M-N2 锚点约 20 项全绿（七阶段 roundtrip / 图元流确定性逐字段一致 / 无中间产物负向 / 未装载负向）；HizScene bake 输出与 M-N1 基线 864 字节逐字节一致（几何汇集抽共享层零回归实锤）；regression nav-m0 10/10 + nav-n1 12/12 + nav-n2 10/10；`server.py --check` exit 0；example/nav-n2 run.bat 双后端 ALL GREEN（五阶段 PNG + `fc /b` 确定性 + bogus 负向）。
- **证据图**：`example/nav-n2/stage-{voxels,regions,contours,polymesh,navmesh}.png`（nav-tools debug-stage 真实输出，768px 俯视正交：voxels 灰环=agent 半径腐蚀带、青=可走核心；polymesh/navmesh 彩色 poly + 深蓝边界）+ `nav-panel.png`（viewer 闭环截图：`--console-exec "nav.bake" "nav.stage 6"` 驱动，面板 stage 下拉 navmesh + 状态行 + 画布 poly）。
- **呈现形态口径（开工时声明，用户可否决）**：viewer 面板 = ImDrawList 2D 俯视画布（与材质图编辑器画布同基建，零新 NVRHI 管线、零纹理上传改动）；PNG 证据 = 工具软光栅（P8-NAV-PLAN「证据图取工具真实输出」既定口径）。2026-11-11 用户拍板「不挂外置编辑器，viewer 内嵌面板口径」。

## 8. M-N2 踩坑（写实）

1. **HudFrame POD 每帧重建 vs 面板写回**：`HudFrame hud{}` 每帧值初始化，面板改的 `st.stage` 下一帧归零——viewer 侧「st.stage != navStage 就采纳面板」的判定把 POD 归零当成面板改动，反过来把 cvar `nav.stage` 踩回 0（实测首帧 bake 成功、第二帧阶段被重置）。**铁律：HudFrame 双向字段必须走边沿标志（`stageDirty`/`paramsChanged`/`bakeRequested`），显示状态（status/prims/bounds）由 viewer 持久变量每帧无条件回填**——与 cull/color 既有双向通道同款纪律，但「检测差异」不能建立在 POD 旧值上。
2. **`DetourDebugDraw.cpp` 不入库的下游影响**：M-N0 因 1.6.0 头依赖 `DetourTileCacheBuilder.h` 剔除该 TU，`duDebugDrawNavMesh*` 不可用——M-N2 NavMesh 阶段自绘（`dtMeshTile` poly fan TRIS + 边界 LINES，`duIntToCol` 配色），与 M-N0 CMake 留痕口径一致。**M-N3 勘误（2026-11-11）**：TileCache 已按方案 A 入库，头依赖已满足；重评结论仍是**不启用**该 TU（启用会改 M-N2 自绘 PNG 基线字节），此处不再留「届时重评」挂账，需要上游全套 `duDebugDrawNavMesh*` 时另起一轮验收 + 重拍基线。
3. **cmdDebugStage 参数回退错位**：自定义参数循环里 `--i` 回退后把 `--size` 的值（768）当选项名喂给 `parseBakeArgs`。修复 = 不回退、直接让 `parseBakeArgs` 从当前位置吃。
4. **regression `--only` 单值语义再次确认**：一次传多个 `--only` 只生效最后一个（nav-n2 10/10 而非三卡 32 条）——三卡零回归须分开跑（M-N1 已踩过逗号列表跑 0 条，本轮是变体）。

## 9. M-N3 验收记录（2026-11-11，tag `nav-n3`）

- **拍板口径**：用户「按建议来」= ① **方案 A** `DetourTileCache` tiled 增量重烘（不走「全量重烘」备选，实测 4 物体约 500ms/次、50 物体外推 2~4s/次不可用）；② **A-1** `.nnav` 同一容器语义升级 v2，v1 向后兼容，不新增 `.ntcache`。三处实读：上游 `DetourTileCache.h`（`addTile`/`addObstacle`/`removeObstacle`/`update`/`buildNavMeshTilesAt`）+ `RecastDemo/Source/Sample_TempObstacles.cpp`（`rasterizeTileLayers` 逐 tile 切片 + tcparams）、UE `RecastNavMeshGenerator.cpp:1558/1655/2400`（fork 的同套 dtTileCache 接口 + `FRecastTileGenerator`/`FTileCacheCompressor`/`TileSizeUU`）、Carrot 自研静态 AStar 无动态导航不作口径。
- **交付面**：`thirdparty/recastnavigation/DetourTileCache/` 4 文件入库（2 头 + 2 .cpp，同一锁定 commit `6dc1667f`，**零编辑**）；`nl_recast` CMake 接 sources/include；`Nav.hpp` 新增 `kNavVersionTiled=2` / `TileBuildParams` / `bakeTiled` / `isTiled` / `compressedTileCount` / `obstacleCount` / `addObstacle` / `removeObstacle` / `updateObstacles` / `geometryEntityCount` / `loadGeometryFromScene`；`Nav.cpp` 自写三个注入实现（`TileLinearAllocator` 1 MiB arena 16B 对齐 + `reset()` 回收、`TilePassthroughCompressor` 4B 长度头零新依赖、`TileWalkableMeshProcess` 按 `RC_NULL_AREA` 设 `polyFlags=1`）；`core/src/nav/SceneGeo.cpp` 新建（E1 World JSON → Geometry，组件注册 + `getWorldMatrix()` 父子复合 + meshFile 白名单 + 按 meshFile 缓存 glb）；`nav-tools` 新增 `tilebake` / `obstacle` 子命令 + selftest 锚点 + ctest 9 条。
- **v2 容器**：magic `NLNV` 与基础段布局同 v1，仅 `version=2`；尾部追加 tiled 扩展段（几何 bbox、`dtTileCacheParams`、`TileBuildParams`/`BuildParams` 逐字段、compressed tile 字节 + `tx/ty/tlayer`）。`save()` 按 `isTiled` 分流，`load()` 按 version 分流；v1 老路径一行不改。禁整块 memcpy struct（padding 破坏确定性），逐字段 `wrScalar`/`rdScalar`。
- **验收数据**：DX12/VULKAN 双后端 BUILD OK + viewer selftest 16/16×2；nav-tools selftest 全绿（M-N0/M-N1/M-N2 锚点零回归 + M-N3 新增 21 项）；ctest `m-n[0123]-nav` **24/24 ×2 后端**（`--repeat until-pass:3`）；VULKAN 树 selftest fnv64 `e7349583a994928d` 与 DX12 一致（跨树确定性）；`regression` nav-n3 22/22 + nav-m0 10/10 + nav-n1 12/12 + nav-n2 10/10 全 ALL GREEN；`server.py --check` exit 0（kStage 仍 S8.2，A 类轨道不 bump）；`example/nav-n3/run.bat` ALL GREEN（10 段机器断言）。
- **零回归硬判据**：solo bake `hiz-v1.nnav` 与 `example/nav-m0/hiz-a.nnav` **864 字节逐字节一致**；v2 两次 `tilebake` `fc /b` 一致；v2 `resave` 后 `fc /b` 一致。
- **动态障碍闭环**：`polysBefore 4 → polysBlocked 3`，`edgesBlocked 12`（3 quads × 4 边，可视化与统计口径一致），`before : OK (2 wp) → blocked : UNREACHABLE (0 wp) → after : OK (2 wp)`，`restored=1`、障碍池清零、`result : PASS`。障碍参数 `--pos -0.35,-1,-1.70 --radius 0.5 --height 3`。
- **E1 场景**：`example/nav-n3/nav-n3.scene`（3 个 Model entity，`nav-block-child` 挂在位移 `(8,0,0)` 的父下）→ `entities : 3` / `verts/tris : 3972 / 6948` / `tiles : 8` / `polys : 12` / `area 1.50 m²`；selftest 另用两份内联 World JSON fixture 断言「父世界矩阵必须复合进子几何」及 `meshFile` 路径穿越拒绝。规模锚 `example/ecs-e1b/ecs-e1b-scale.scene` 50 个 Model entity / 3970980 verts / 6645600 tris 汇集约 0.6s。
- **证据图**（均为 nav-tools 软光栅真实输出，768px 俯视正交，非手绘）：`tiles.png`（淡蓝 tile 网格 + 黑 poly 边界）、`obstacle.png`（蓝=无障碍原路径、红=障碍后路径（本例 0 wp 故不可见）、绿圈=圆柱障碍俯视、黑=挖洞后剩余 poly）、`scene-tiles.png`（3 组 HizScene 箱顶 + 8 tile 网格，可见父子层级把第三组推到 x≈8 / z≈4）。
- **交代物**：`example/nav-n3/`（run.bat + nav-n3.scene + 三张 PNG + `hiz-tiled.nnav` / `hiz-v1.nnav` / `scene-tiled.nnav` 三份基线 + 12 份日志）；cases.json `nav-n3` 卡 11 用例（usage / selftest / tilebake / scene / 50 物体 / dump-v2 / wire-v2 / obstacle / obstacle-v1 / 两条 usage 负向）；`P8-NAV-PLAN.md` 收官留痕 + `MILESTONES.md` 四栏 + `VENDOR.md` 入库子集。

## 10. M-N3 踩坑（写实）

1. **上游 `void` 返回函数被写成布尔判定（C2171 / C2451）**：`rcMarkWalkableTriangles` / `rcFilterLowHangingWalkableObstacles` / `rcFilterLedgeSpans` / `rcFilterWalkableLowHeightSpans` 在 1.6.0 均返回 `void`，照 Solo Mesh 直觉写 `if (!fn(...))` 直接编译失败。修复 = 照 M-N0 bake 同款直接调用，**NVRHI/上游每处调用回 vendored 头核签名**纪律再次生效。
2. **动态障碍首版完全不影响路径（最重坑）**：`--pos -1.45,0,-1.55 --height 2` 时 before/blocked/after 全 `OK (2 wp)`。先加 `polysBefore/polysBlocked` 诊断确认 tile 确实在重建（4→3），再逐点 dump waypoint 才发现真实吸附折线在 `(-0.35,-0.35,-1.90)→(-0.35,-0.35,-1.50)`，障碍放错箱顶/高度域。**铁律：TileCache 圆柱障碍的 y 区间必须跨过 tile 高度域下界**（HizScene 下界约 -1.35），改 `--pos -0.35,-1,-1.70 --height 3` 后闭环 PASS。诊断用 `dumpWp` 在验收前已清理，正式日志只保留 polys/edges 两行证据。
3. **v2 load 的 compressed tile 内存所有权**：`dtTileCache::addTile(..., DT_COMPRESSEDTILE_FREE_DATA)` 会让 Detour 用 `dtFree` 释放数据；若直接把 `std::vector` 的 `data()` 交出去，就重演 M-N0 的 `new`/malloc 分配器不匹配 UAF。修复 = 读出的 tile 字节用 `dtAlloc` 分配并 memcpy 后再移交；solo v1 路径继续由 `Impl::ownedTiles` 持有（flags=0）。
4. **「50 物体锚」的语义陷阱**：现成 E1 场景（`ecs-e1b-scale.scene` 等）全是 Suzanne / IcoSphere / TorusKnot 曲面装饰模型，无水平可走面 → `tiles=0 / polys=0 / area=0` 是**几何语义结果，不是管线故障**（与 M-N0 踩坑 5「小资产 + 粗网格腐蚀吃光」同类）。因此必须另造 `nav-n3.scene` 用 HizScene.glb（含箱顶可走面）验证 NavMesh 正确性，两条证据分开写清，不能用「tiles=0」冒充失败、也不能拿它证明正确。
5. **`polyEdges()` 与 `stats().polyCount` 的视觉误读**：障碍后 PNG 上黑色方框看着像 4 个，而日志 `polysBlocked : 3`。加 `edgesBlocked : 12` 诊断行后确认 = 3 quads × 4 边，两者口径一致；把 `edgesBlocked: 12` 一并做成 run.bat 机器断言，防止后续看图误判为可视化漏绘。
6. **展示台 tool 卡 `{out}` 单占位符 + 只读纪律**：`server.py`/`regression.py` 只把**精确等于** `{out}` 的 token 换成临时 `.toolout` 路径，`{out}.png` 这类拼接会抛 `unresolved placeholder`。而 `tilebake` 同时需要 `.nnav` 与 PNG 两个输出，若把 `.nnav` 写成 `{root}/example/nav-n3/hiz-tiled.nnav` 就等于回归时重写仓库归档基线。修复 = 归档基线只读，证据图卡改用 `wire <归档 v2 基线> {out}`，PNG 走临时路径跑完即删。

## 11. M-N3 面板补充验收记录（2026-11-11，tag `nav-n3-panel`）

> 本节记录 M-N3 收官时冻结的「viewer 面板不加障碍交互按钮」被用户点名解禁后的补充交付。本节全部是 **viewer / renderer 侧**，nav-tools 与 `nl::nav` 一行未改，因此 §2 子命令表不变、ctest 沿用既有 24 条。

- **拍板口径**：用户「继续做」点名两项 → ① viewer `NavPanel` 加障碍交互按钮（**冻结面解禁**）；② M-N4 `DetourCrowd` 立项（见 `P8-NAV-PLAN.md` 文末，架构判定未拍板前零落码）。本轮只交付 ①。
- **绕不开的前提**：M-N2 面板 bake 走 `bake()` Solo Mesh（`.nnav` v1），**没有 `dtTileCache`**，障碍 API 一律拒绝。因此做双模式分流：`nav.tiled=0`（缺省）保持 M-N2 全部行为；`nav.tiled=1` 时 bake 改走 `bakeTiled()`（v2），障碍控件才启用。tiled 不保留 rc 中间产物，五阶段只 NavMesh 可用，viewer 把 `nav.stage` 钉到 6（cvar 与持久变量同改，防下一帧被 cvar 差异拉回）。
- **接口**：新增 cvar `nav.tiled` / `nav.tileSize` / `nav.obsX` / `nav.obsZ` / `nav.obsRadius` / `nav.obsHeight` / `nav.obsPanel`，命令 `nav.obsAdd` / `nav.obsClear`；新增 `--nav-model <name>` 把**面板烘焙输入**与**渲染模型**解耦（`HizScene.glb` 无 baseColor，不能作 `--model`：实测 `renderer init failed: HizScene.glb has no base color texture`），缺省仍等于 `--model`，M-N2 零回归。障碍 y 下界固定 `-1`，沿用 M-N3「必须跨过 tile 高度域下界」铁律。
- **面板**：`dynamic obstacles (tiled)` 折叠头（默认收起 → M-N2 首屏只多一行）+ `tiled bake (v2)` checkbox + 四条滑条 + `add obstacle` / `clear obstacles` 按钮 + 障碍数/路径状态行；画布在 NavMesh 图元之上叠加红色障碍圆与蓝色 `findPath` 折线。`nav.obsPanel 1` 用 `ImGuiCond_Once` 展开该区、收起 build params 并把窗口调到 700px，专供证据脚本截图（默认 0 时严格是 M-N2 的 460×600）。
- **验收数据**：DX12/VULKAN 双后端 BUILD OK + viewer selftest 16/16×2；`example/nav-n3-panel/run.bat` 双后端 **A~E 五段 ALL GREEN**（含两次 `--screenshot` `fc /b` 逐字节一致）；ctest `m-n[0123]-nav` 24/24 ×2；`regression --only nav-n3-panel` **8/8 ALL GREEN**（双后端文本断言），四张老卡 nav-n3 22/22 + nav-n2 10/10 + nav-n1 12/12 + nav-m0 10/10；`server.py --check` exit 0（kStage 仍 S8.2）；`check-bat-encoding` 1 文件 0 违规。全程 `deviceErrors=0`。
- **闭环锚点**：`bake ok mode=tiled polys=4 tiles=2` → `path OK (2 wp) polys=4 obstacles=0` → `obstacle added ref=65536 polys=3` → `path UNREACHABLE (0 wp) polys=3 obstacles=1` → `obstacles cleared polys=4 left=0` → `path OK (2 wp) polys=4 obstacles=0`。负向：Solo Mesh 上 `nav.obsAdd` 打 `nav-panel: obstacle rejected`（明确拒绝，不静默 no-op）。零回归锚：`bake ok mode=solo` + `nav.stage 4` 且日志无 `debug stage failed`。
- **证据图**（viewer `--screenshot` 真实 GPU 回读，1280×720，非手绘）：`obstacle-panel.png`（展开的障碍区 + 红色障碍圆 + 3 个剩余 poly + `obstacles: 1 / path: UNREACHABLE (0 wp)`）、`cleared-panel.png`（恢复后的蓝色折线，无红圆）、`solo-panel.png`（M-N2 默认布局未变）。另 `reject.png` 为负向用例截图。
- **交代物**：`example/nav-n3-panel/`（run.bat + 4 张 PNG + 4 份日志）、cases.json `nav-n3-panel` 卡 4 用例（`exe` 指向 `viewer.exe`、`kind:"tool"` 只做文本断言、`{out}` 走临时路径不写仓库）、`P8-NAV-PLAN.md` / `MILESTONES.md` 留痕。

### 11.1 踩坑（写实）

1. **`--console-exec` 一次只吃一个参数**：`--console-exec "nav.tiled 1" "nav.bake" "nav.obsAdd"` 实测只有第一条进日志（后两条被当位置参数丢弃），首版假绿（只有 `nav.tiled=1`、无 bake）。修复 = 每条命令重复一次 flag：`--console-exec "nav.tiled 1" --console-exec "nav.bake" --console-exec "nav.obsAdd"`。M-N2 GUIDE §7 写的「`"nav.bake" "nav.stage 6"`」是简写，脚本里必须按 flag 重复写。
2. **面板默认 bake 输入是渲染模型**：`o.modelFile` 缺省 `Suzanne.glb`，直接换 `--model HizScene.glb` 会因无 baseColor 贴图让 renderer init 失败；而继续用 Suzanne 则 tiled 只有 1 poly、路径端点全不可达（`path UNREACHABLE obstacles=0`），障碍也无效。修复 = `--nav-model` 解耦，缺省仍 `--model`。
3. **tiled 画布 bounds 不能用 `stats()` 几何盒**：`statsFromMesh` 的 bbox 是整份几何盒（含大量无可走面的箱子），首版截图里 4 个 poly 缩成几个小黑点。修复 = tiled 时改用 `DebugPrims` 实际顶点 + 障碍圆的 AABB 收紧投影（solo 分支一字不改，保 M-N2 字节）。
4. **展开障碍区后画布被挤成一条缝**：`build params` 默认展开占掉 600px 窗口的大部分。修复 = 证据模式下（`nav.obsPanel=1`）窗口 700px + `SetNextItemOpen(false, Cond_Once)` 收起参数区 + 障碍区 `Cond_Once` 展开；默认 0 时两条逻辑都不触发。
5. **`obstacleCount()` 计数口径**：它只数 `DT_OBSTACLE_PROCESSED` 状态，add 后立即 `updateObstacles()` 才会变 1（首版 Suzanne 场景显示 0 是因为障碍根本不在 tile 覆盖域）。日志锚必须同时断言 `polys` 与 `obstacles` 两个数，单看一个会误判。

## 12. M-N4 验收记录（2026-11-11，tag `nav-n4`）

- **开工闸（拍板先于落码）**：用户点名「DetourCrowd 要做」解除 `P8-NAV-PLAN.md` 原「默认不做 / 不在本轨道交付范围」范围闸 → 三处实读（上游 `DetourCrowd/` 6 头 + 6 源零新依赖、UE `UCrowdManager` Tick 四步、Carrot 静态 A\* 与 bevy 无原生 navmesh 据实标注不作口径）+ 上游 `DetourCrowd.cpp` 无 RNG 取证 → 用户「按推荐来」拍板 **Q1=A（A 类工具版）/ Q2=A-1（`.nnav` 不升 v3）/ Q3=A（固定 dt + 固定帧数 + 固定顺序）**，写回计划并删「拍板前不许写第一行代码」挂账句后才落第一行代码。
- **交付面**：vendored `DetourCrowd/` 12 文件（锁定 commit `6dc1667f`，逐文件 md5 一致、零编辑）+ `nl_recast` CMake 显式枚举 6 TU 与 include 目录；`core/include/nl/nav/Crowd.hpp`（POD 配置 / 轨迹 / 类，零第三方符号）+ `core/src/nav/Crowd.cpp`（唯一 DetourCrowd 符号面，PIMPL）；`NavMesh::nativeHandle()` 以 opaque `const void*` 桥接 `dtNavMesh*`；`nav-tools crowd` 子命令 + `.ntraj` 容器（magic `NLTR` / version 1 / agentCount / frames / dt / targets / samples / fnv64）+ 俯视轨迹 PNG + 25 项 selftest 锚点 + 8 条 ctest + cases.json `nav-n4` 11 卡。`viewer` / `scene` / `renderer` / `.nnav` 序列化 / `kStage` 一行未改。
- **验收数据**：DX12/VULKAN 双后端 BUILD OK + viewer selftest 16/16×2；nav-tools selftest 全绿（M-N0~M-N3 老锚点零回归 + M-N4 新增 25 项）；ctest `m-n[01234]-nav` **32/32 ×2**（`--repeat until-pass:3`）；`regression --only nav-n4` 22/22 + 五张老卡全绿；`server.py --check` exit 0；`example/nav-n4/run.bat` 双后端 **A~F ALL GREEN**；`check-bat-encoding` 0 违规。**Q3=A 硬判据**：同参数两次 run 的 `.ntraj`（28924 字节 / fnv64 `5cbf9ce63eb1b273`）与 PNG 分别 `fc /b` 逐字节一致；`arrived 4/4`、`minPairDist 0.6003 m`（≥ 2×radius=0.6，RVO 生效）。**Q2=A-1 硬判据**：`example/nav-n3/hiz-v1.nnav`（864 B）与 `hiz-tiled.nnav`（7276 B）直接被 `Crowd` 消费且单 agent 抵达，两个基线字节未动。
- **证据图**：`example/nav-n4/crowd-arena.png`（768×768，nav-tools 软光栅真实输出，非手绘）——黑色 = dtNavMesh poly 边界（20×20 地板 + 中央障碍 + 孤立岛），四条彩色折线 = 两对对角穿越的 600 帧轨迹，可清楚看到绕开中央障碍与中心区的避让弯曲，起终点十字标记。
- **交代物**：`example/nav-n4/`（run.bat + `crowd-arena.ntraj` + `crowd-arena.png` + 8 份日志）、cases.json `nav-n4` 11 卡（`{out}` 走临时路径不写仓库）、`VENDOR.md` 入库子集、`P8-NAV-PLAN.md` 拍板/收官留痕、`MILESTONES.md` M-N4 四栏。

### 12.1 踩坑（写实）

1. **180 帧不够跑完 20 m 对角**：首版 `frames=180`（3 s）时 `arrived 0/4`、`result=FAIL`，是帧数不足不是 crowd 失效。3.5 m/s × 绕行 + 加速段实测约需 600 帧（10 s），故把 `CrowdConfig::frames` 默认提到 600，并在 `run.bat` / ctest / 展示台卡全部显式写死（Q3=A「固定帧数」）。
2. **`targetHalfExtents` y 半宽 4 m 把终点吸附到错误高度层**：HizScene 箱顶 poly y=1.25、下层 poly y=-0.35 相差 1.6 m；`findNearestPoly` 用 `{2,4,2}` 时 target 被吸到 y=-0.35，agent 留在箱顶跨层不可达（240 帧末距目标仍 0.857 m）。修复 = 收紧到 `{2,1,2}`，与 M-N1 `QueryParams::halfExtents` 默认口径一致；改后 v1/v2 单 agent 60 帧均 `arrived 1/1`。**arena 单高度地面不受影响，fnv64 未变。**
3. **「v1/v2 轨迹逐点相同」是错误期望**：solo 与 tiled 的多边形划分不同（tile 切边），同一几何的走廊/局部边界不同，轨迹本就不该一致。A-1 的正确断言 = 两种容器**都能被消费并产出完整同形状轨迹**（`samples.size()` / `agentCount` / `frames` 一致），不比较逐点值。
4. **`dtCrowd::addAgent` 会静默接受无路径的 agent**：上游 `addAgent` 内部 `findNearestPoly` 失败时仅把 `ref=0`、`nearest=pos`，仍返回有效 idx。封装层必须**先用自己的 `dtNavMeshQuery` 显式校验 startRef / targetRef**，任一 `ref==0` 或 `dtStatusFailed` 立即返回 false，否则会把「网格外 spawn」变成假绿。
5. **上游路径队列一帧最多 8 个请求**：`dtPathQueue::update` 每帧处理上限 8，agent 数超过 8 时建路径要跨帧；默认 `crossing`/`line` 均 4 个，一帧建完。固定帧数须给足，否则前几帧的位置只反映加速段。
6. **vendored md5 核对要排除上游 CMakeLists.txt**：`DetourCrowd/` 上游自带 `CMakeLists.txt`，本项目按薄脚本纪律**故意不入库**；首版核对脚本遍历整个目录把该文件标成 `DIFF`（假报），改为只比 12 个 `.h`/`.cpp` 后 `ALL_BYTE_IDENTICAL=True`。
