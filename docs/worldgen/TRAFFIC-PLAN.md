# TRAFFIC-PLAN — 交通系统 M-T0（A 类并行轨道 worldgen，2026-11-14 立项）

> **状态：M-T0.0~T0.2 整轨收官（2026-11-14，tag `traffic-m0`）**：双后端 BUILD OK + ctest `m-t0-traffic-*` 9/9 ×2 + `example/t0-traffic/run.bat` 七段 ALL GREEN + 展示台卡 `t0-traffic`；工具归档 `docs/worldgen/TRAFFIC-TOOLS-GUIDE.md`；明细 `docs/MILESTONES.md` T0 节。

> 对应 `docs/GAME-DEV-ORDER.md` 第 8 步「交通系统 M-T0：lane graph 数据结构 + 路网寻路 + 直线跟车」。
> 轨道归属：worldgen（M-W0~W3 已整轨收官，M-W3.0 `CityCar.edgeId` 溯源已为 M-T0 预埋）。
> Q-A（自研立项）已拍（GAME-DEV-ORDER §5）：开源参照 CARLA TrafficManager（BSD，看思路不抄代码）；无成熟 vendored C++ 库对口 → 「无成熟方案自研例外」。
> **命名勘误**：`docs/t0/` 与 tag 前缀 `t0-*` 已被 temporal/post-chain 基建（`docs/t0/T0-PLAN.md`）占用，本轨计划落 `docs/worldgen/`、tag 前缀 `traffic-*`。

## 0. 已拍板结论（2026-11-14 03:00 用户拍板「按默认的来」，四项全按推荐项）

| 编号 | 结论 | 依据 |
|---|---|---|
| Q-T1 落点 | **A1**：逻辑落 `core/include/nl/traffic/` + `core/src/traffic/`（零图形依赖，对齐 `nl/nav` PIMPL 先例）；debug 证据走**独立 `traffic-tools` 纯 CPU 光栅化 PNG**（照 nav-tools 口径，不碰 viewer/renderer） | GAME-DEV-ORDER §3「纯逻辑层、GameClock 60Hz、core 零图形依赖」+ nav 先例（viewer 集成是后置里程碑） |
| Q-T2 车道图 | **B1**：车道级展开图（UE ZoneGraph 口径）：每条 roadnet 有向 edge 按 `lanes_per_dir` 展开成 lane 节点 + 预计算折线点 + lane→lane 显式连接表 | UE `ZoneGraphTypes.h` 实读（`FZoneLaneData` / `FZoneLaneLinkData{DestLaneIndex,Type,Flags}`）；寻路/跟车/红绿灯全在一张图上 |
| Q-T3 跟车 | **C1**：恒速巡航 + 前车安全间距减速（固定 dt=1/60，逐位可复现） | M-T0 验收「直线跟车」够用；IDM 参数面大留后续 |
| Q-T4 命名 | tag 前缀 `traffic-*`；计划文档 `docs/worldgen/TRAFFIC-PLAN.md` | 撞 `docs/t0/` 已占用 |

## 1. 输入契约（冻结，W0-PLAN §1 Q-W2）

`roadnet.json`：有向 edges 双向成对；`lanes_per_dir` / `speed_limit`(m/s) / `width`(车行道总宽)；`phases` 预留空数组（**本轨 M-T0.2 起消费**，信号相位扩展只加不减，破坏性变更 version 递增）；`nodes.kind` 的 `signal` 布尔位本轨消费。实测输入面：seed 12345 / size 6 → 49 nodes / 168 edges（`lanes_per_dir=1`、`width=1.0`）。

## 2. 几何与拓扑口径（B1 固化）

- **Y-up 右手系、右行交通**：行进方向 `f=(fx,fz)`（XZ 面），右侧 `right=(fz,−fx)`（`up×f`，f=+Z 时 right=+X 实证）。
- **车道横向偏移**：`halfW=width/2`，`laneW=halfW/lanes_per_dir`；车道 i（0=最内贴中线）偏移 `off_i = halfW − (i+0.5)·laneW`，沿 right 施加。width=1.0 / 1 车道 → ±0.25m。
- **折线**：城市网格边为直线段 → 车道折线 = `[from + right·off, to + right·off]`，`length = |to−from|`。折线点序 = 行进方向（起点→终点）。
- **连接表（禁掉头）**：lane（edge e：u→v）的 outgoing = 所有 `e'.from==v && e'.to != u` 的边展开车道的 lane 序号（edges 文件序 × laneIndex 升序 = 确定性遍历序）；incoming 对称。掉头链路不建（UE Flags 口径可后续扩展）。
- **确定性**：构建零 RNG，同输入逐字节同输出；`dumpLaneGraphJson` 产物作字节级对拍锚（W1.0 同路径双跑先例）。

## 3. 里程碑阶梯

### M-T0.0 — lane graph 构建 + 确定性 selftest
- **任务**：`nl/traffic/LaneGraph.hpp/.cpp`（roadnet.json 解析用 `thirdparty/tinygltf/json.hpp` 同款 include，nav/SceneGeo.cpp 先例）+ `traffic-tools`（build/dump/selftest CLI，链接 nl_core，退出码 0/1/2 对齐 nav-tools）。
- **验收判据**：① selftest 数值锚（车道数网格公式 / 偏移几何 ±0.25 / 长度 / 连接闭合 / 禁掉头 / 双跑 dump 逐字节一致 / 负向：断引用与坏 version 显式报错）；② ctest 用例齐（selftest / build 正向 / 负向）；③ 双后端编译通过（纯 CPU，零图形 API）。

### M-T0.1 — 车道图 A* 寻路
- **任务**：lane graph 上 A*（自研，确定性 tie-break：g 值 + lane 序号）；起点/终点 lane 内弧长参数化；负向语义（不可达 / 起终点同 lane / 越界 → 明确错误码）。
- **验收判据**：selftest 折线路径逐点一致 + 转弯数锚（直行优先口径）+ 不可达显式失败。

### M-T0.2 — 直线跟车 + 车道线 debug 收口
- **任务**：恒速巡航 + 前车安全间距减速（固定 dt=1/60，固定 agent 序）；`traffic-tools lanes` 纯 CPU 光栅化画车道线 + 车流位置 PNG（工具真实输出，禁手绘）；车流实体落 `CityCar.edgeId` 预埋通道（`phases`/`signal` 信号相位**不在本轨**，GAME-DEV-ORDER 第 9 步）。
- **验收判据**：selftest 轨迹逐位可复现（同 seed 同帧同位置）+ 车道线/车流 PNG 证据图 + cases.json `kind:"tool"` 卡（usage/正向/负向）+ 收口（tag `traffic-m0` + MILESTONES 四栏 + close 闸）。

## 4. 冻结面

- `renderer/` / `viewer/` 一行不碰；`kStage` 不动；渲染回归卡不新增。
- `city-tools` 产物格式只消费不修改；roadnet.json 契约扩展（phases/signal）只加不减。
- `.nmesh` / `city.scene` / nanite-tools / asset-ingest 接口只复用不修改。
- `nl/nav`（Recast 口径）与交通（车道图口径）互不替代——行人 NPC 走 nav，车辆走 lane graph（GAME-DEV-ORDER §3 定档）。

## 5. 开工登记

- track `worldgen` / milestone `M-T0` claim 在位（wt `E:/UE_STUDY_2026/nl-engine-worldgen`，分支 `feat/worldgen`）。
- 三处实读留痕（2026-11-14）：roadnet.json 实测产物 + W0-PLAN §1；UE `ZoneGraphTypes.h`（车道级图 + 连接类型）只读对照；nav-tools / city-tools 交接先例（退出码、ctest、确定性 selftest 口径）。Carrot / bevy 无交通系统，据实标注不作口径。
