# TRAFFIC-TOOLS-GUIDE — 交通 lane graph 工具归档（M-T0.0~T0.2，tag `traffic-m0`）

> 轨道：worldgen（feat/worldgen）。计划 `docs/worldgen/TRAFFIC-PLAN.md`；输入契约 `docs/worldgen/W0-PLAN.md §1`（roadnet.json）。
> 工具版交代：不进 `kStage` / renderer / 渲染回归总闸；cases.json 卡 `t0-traffic`（131 号）。

## 1. 定位与拍板

| 项 | 拍板 |
|---|---|
| Q-T1 落点 | 逻辑 `core/include/nl/traffic/`（零图形，nl/nav PIMPL 先例）+ `traffic-tools`（只链 nl_core） |
| Q-T2 车道图 | B1 车道级展开图（UE `ZoneGraphTypes.h` 只读对照口径：lane 折线预计算 + lane→lane 连接表） |
| Q-T3 跟车 | C1 恒速巡航 + 跟车律（IDM 留后续） |
| Q-T4 命名 | tag 前缀 `traffic-*`（`docs/t0/` 与 `t0-*` 已被 temporal 基建占用） |

## 2. 子命令（退出码 0=PASS / 1=断言失败 / 2=用法错误）

```
traffic-tools build <roadnet.json> [--dump out.json] [--stats]   # lane graph 构建 + 确定性 dump
traffic-tools path <roadnet.json> --from <edgeId> --to <edgeId>  # A* 寻路（edgeId = CityCar.edgeId 溯源语义）
traffic-tools lanes <roadnet.json> --png out.png [--size WxH]    # 车道线 PNG（纯 CPU 光栅化）
traffic-tools sim <roadnet.json> [--cars N] [--frames F] [--png out.png]  # 跟车仿真 + 车流终帧 PNG
traffic-tools selftest [--emit-fixture path] [--verbose]         # 3693 断言数值锚
```

## 3. 数据结构与几何口径（TRAFFIC-PLAN §2 固化）

- **展开**：每条有向 edge 按 `lanes_per_dir` 展开 lane；edges 文件序 × laneIndex 升序 = lane 下标序（确定性遍历序契约）。
- **几何**：Y-up 右手、右行；`right=(fz,−fx)`；车道 i 横向偏移 `halfW − (i+0.5)·laneW`；折线点序 = 行进方向。
- **连接**：lane(u→v) outgoing = `e'.from==v && e'.to!=u` 全部车道（**禁掉头**）；incoming 对称。
- **确定性**：构建/寻路/仿真全链零 RNG、零堆序依赖（A* open 表线性扫最小，禁 priority_queue）；同输入逐位同输出，`build --dump` 产物 = 字节级对拍锚。

## 4. 跟车律（C1 落码口径）

- 目标速度 = min(lane 限速, **sqrt(brake·max(0, gap−minGap))**)（以 brake 减速度恰好停在 minGap 跟随点；「匹配前车速度」式在前车停车曲线 + 后车自由段再加速下形成 limit cycle，已否）。
- route 末 lane 走停车曲线 `sqrt(2·brake·remain)`（瞬钳 = 无限 jerk，后车物理刹不住）。
- 固定 dt=1/60、固定车序（下标序）、bang-bang 限幅（accel/brake）；s 超长推进 route 下一条 lane，route 走完停车。
- selftest 锚：3 车混合路径 300 帧双跑 `TrafficCarState` memcmp 逐位一致；停车跟随末间距 ∈[1.0,2.5]m 永不穿透；全帧限速帽。

## 5. 验收记录（2026-11-14）

- 双后端 BUILD OK；ctest `m-t0-traffic-{selftest,fixture,build,negative,path,path-negative,sim,lanes,sim-negative}` **9/9 ×2**。
- 真实城市（seed 12345，49 nodes/168 edges）：168 lanes / 428 links / 798m；dump 双跑逐字节一致；e0→e60 跨城 8 hops 36.5m；32 车 30s 行驶 967.4m（`city-sim.png` / `city-lanes.png` = 工具真实输出）。
- `example/t0-traffic/run.bat` 七段 ALL GREEN；cases.json `t0-traffic` 卡 7 用例。

## 6. 边界与后续

- 行人 NPC 走 `nl/nav`（Recast），车辆走 `nl/traffic`（车道图）——互不替代（GAME-DEV-ORDER §3 定档）。
- `phases`/`signal` 信号相位、红绿灯、路口让行、避撞 = GAME-DEV-ORDER 第 9 步（调度器迭代），本轨只留了 roadnet 契约扩展位。
- route 走完停车；循环 demo 旗标、`CityCar` 实体接入、GameClock/ECS System 化 = 后续独立拍板项。
