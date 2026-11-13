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

---

# M-T1 — 调度器迭代：红绿灯相位 + 路口占用锁 + 避撞（2026-11-14 立项）

## 6. 已拍板结论（2026-11-14 04:34 用户拍板「A2 B1 C1」）

| 问 | 拍板 | 内容 |
|---|---|---|
| Q-T5 | （默认） | 续 worldgen 轨 **M-T1**，tag `traffic-m1`，与 M-T0 同 ladder |
| Q-T6 | **A2** | 红绿灯 = 固定周期两相**含黄灯**：绿→黄→全红清空→对向绿…；黄灯语义 = 不放行新进入（持锁在穿的车继续穿完）；dt=1/60 整帧计数时钟，逐位确定 |
| Q-T7 | **B1** | 路口让行 + 避撞 = **路口占用锁（reservation）**：进入路口区（nodeClearRadius）前按到达序 FIFO 申请，全路口互斥（一车一锁，同绿相也串行——保守但确定性易锚）；无信号路口同机制（lock-only）；死锁不可能（单资源、持锁即穿、无跨锁等待） |
| Q-T8 | **C1** | 验收 = traffic-tools sim 扩展：跨车道最小间距锚（不同 lane 车对中心距下界）+ 无进度车检出 + 到达数/最大滞留统计 + 终帧 PNG（信号路口状态着色）；同 lane 间距归跟车律锚管辖（M-T0 已锚），不入几何互斥检查 |

## 7. 机制口径（固化）

- **契约扩展（只加不减，W0-PLAN §1）**：`nodes[].signal=true` 的节点消费 `phases` 顶层数组可选条目 `{"node":<id>,"green_s":..,"yellow_s":..,"clear_s":..}`；缺省 = 自动两相（进入边按行进主轴分组：x 轴向组 0 / z 轴向组 1，edge id 升序）+ 默认时序 8/2/1 s。citygen 不填 → 全默认。
- **相位时钟**：周期 = 2*(g+y+c)；组 0 活跃于 [0, g+y+c)，组 1 于其后半；tt = fmod(t, g+y+c)，tt<g=绿、<g+y=黄、否则全红。帧计数整数 × dt，无浮点累加漂移。
- **停车线**：sStop = lane.length − entryStop（默认 2.5 m，留穿行车间距余量：垂直停止线两车最小中心距 ≥ ~3.9 m > 检查下界）；未获准硬钳 s ≤ sStop（backstop，防离散超调闯入）；approach 减速目标 = sqrt(2·brake·max(0, sStop−s))。
- **锁生命周期**：前瞻申请（无同 lane 前车且剩余 ≤ 刹车距离+minGap，保绿波直行）或到线申请（有前车只许到线——**死锁闸**：锁只被「当前能流动的车」持有，前瞻拿锁后被前车堵住 = 循环等待）；信号绿（或 lock-only）才受理，FIFO 授予；持锁穿至「离节点 > nodeClearRadius（默认 3 m）」释放；route 末 lane 终点为受控节点时停车线 = sStop（不进路口区停靠）。
- **碰撞检查口径（C1）**：只查**不同 lane** 车对（同 lane 归跟车律锚，其离散 dip 已有 [1.0, 2.5] 锚不重复管）；下界默认 1.0 m（跨车道几何下界 ~2.7 m，留离散余量）；违反 = 退出 1。
- **多车道限制（记录在案）**：当前 city 契约 lanes_per_dir=1；多车道同边并行车道不做车道内避撞（不同 lane 车对几何检查会误报并排行驶）→ M-T1 范围外，独立后续项。
- **碰撞检查口径补充（实测修正）**：互斥扫描只查**不同路段**（无序节点对不同）车对——同路段对向车道会车（横距 = 两 lane offset 和 ≈0.5m）是正常交通；下界 1.0m 实测余量 35%（城市实测 min 1.35m）。
- **自适应控制几何（M-T1 落码期根修，2026-11-14）**：固定常数（entryStop 2.5 / clearRadius 3.0）按 10m 夹具标定，W0 城 ~4m lane 上几何不相容——停车线+排队间距吃满 lane，排队车永远压在已穿节点释放圈内持锁不放 = 系统性 gridlock（实测终态 20 锁未放）。根修三件套：① `stopS = length − min(entryStop, 0.6·length)`；② 每节点 `clearEff = min(clearRadius, 0.3·最小邻接边长)`；③ 出口闸阈值 = `minGap + clearEff`（保证获准进入后最坏停点仍在释放圈外）。防溢出闸（don't-block-the-box）+ 无前车前瞻闸 + 自适应几何三件合验：城市 32/16 车终态 `locked end 0`、夹具 10m 锚全部退化原值不重拍。

## 8. 里程碑阶梯

### M-T1.0 — TrafficControl 模块（信号时钟 + 占用锁）
- **任务**：`nl/traffic/TrafficControl.hpp/.cpp`（roadnet.json 节点/进入边分组 + 相位时钟 + FIFO 锁，零图形依赖）；selftest ⑨ 纯时钟锚（相位序列逐帧）+ 锁单元锚（FIFO 授予序 / 持锁互斥 / 释放后再授）。
- **验收判据**：selftest 全绿 + 双后端编译。

### M-T1.1 — TrafficSim 集成
- **任务**：TrafficSim 可选 `control` 挂接（停车线减速/硬钳/acquire/release/route 末停车线）；selftest ⑨ 补仿真锚（红灯刹停在 sStop 逐位、冲突双车串行过锁、控制全开 1200 帧跨车道零碰撞、双跑逐位一致）。
- **验收判据**：selftest 全绿 + M-T0 无 control 路径锚不回归（⑧ 原样全绿）。

### M-T1.2 — 证据 + 收口
- **任务**：sim 输出扩展（signals/arrived/min gap/max halt）+ 信号状态着色 PNG；真实城市 32 车冒烟；卡与 GUIDE 增补；MILESTONES 台账；close 闸 + tag `traffic-m1`。
- **验收判据**：arrived>0、跨车道零碰撞、无 >60s 滞留未完成车；`--only t0-traffic` 保形 + 新锚全绿。

## 9. 冻结面（M-T1）

- M-T0 冻结面全部继承（renderer/viewer/kStage/city-tools 产物/nl/nav）。
- `LaneGraph` / `LanePath` / M-T0 TrafficSim 既有语义与锚**不改不重拍**（control 为可选挂接，nullptr = 原行为）。
- `dumpLaneGraphJson` 字节契约不动（控制面不进 lane graph dump）。

