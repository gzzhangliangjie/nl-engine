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

---

## 7. M-T1 调度器（traffic-m1，2026-11-14）

### 7.1 机制（TRAFFIC-PLAN §6 拍板 A2/B1/C1 + §7 固化）

- **TrafficControl**（`nl/traffic/TrafficControl.hpp/.cpp`）：受控节点 = 进入边 ≥2；进入边按行进主轴分组（x 轴向组 0 / z 轴向组 1，对角 tie 归组 0，edge id 升序）。信号时钟 = 整数帧 ×dt，周期 `2*(g+y+c)`，组 0 活跃前半、组 1 后半，`tt<g` 绿 / `<g+y` 黄 / 否则全红；黄灯不放行新进入、持锁穿完。`phases` 契约只加不减：可选 `{"node":<id>,"green_s":..,"yellow_s":..,"clear_s":..}` 覆盖，缺省 8/2/1。无信号节点 = lock-only（信号恒绿）。
- **占用锁**：FIFO（队首优先授予），持锁越 `clearEff` 圈释放。**三重死锁根修**（城市冒烟实测 20 锁泄漏驱动）：① 同 lane 有前车不允许前瞻申请（锁只归「当前能流动的车」）；② 出口 lane 起步区被占（`s < minGap + clearEff`）不申请（don't-block-the-box，等在停车线不持锁）；③ 自适应几何 `stopS = length − min(entryStop, 0.6·length)`、`clearEff = min(clearRadius, 0.3·最小邻接边长)`——固定常数按 10m 夹具标定，在 ~4m 城市路网上停车线+排队间距吃满 lane，排队车永远压在上游节点圈内。
- **互斥扫描**：只查不同路段（无序节点对不同）车对——同路段对向会车（横距 ≈0.5m）是正常交通；下界 1.0m，违反退出 1；`locked end != 0`（死锁）退出 1 并 dump LOCK 诊断行。

### 7.2 sim 输出契约（M-T1 扩展，既有四行不动）

```
signals     : <受控节点数> (lock-only <L> / signal <S>)
arrived     : <到达停靠帽车数> / <总车数>
min gap     : <跨路段全程最小中心距> m (cross-road)
locked end  : <终态持锁节点数>（0 = 无死锁）
max halt    : <最大停滞时长> s
```

PNG：信号节点 5px 方块按终帧相位着色（绿/黄/红，lock-only 灰）；车点 3px 行驶红 / 滞留琥珀。

### 7.3 验收记录

- selftest **4657 断言**全绿（⑨ 调度器段：时钟帧锚 0/30/42/48/78/96、FIFO 锁单元链、红灯 7.5m 逐位钳停、冲突串行过锁、跨路段互斥、控制全开双跑 memcmp）；ctest `m-t0-traffic-*` 9/9 ×2 双后端。
- 城市 32 车（49 lock-only 节点）：`locked end 0`（根修前 20）、min gap 1.35m、475.5m / 30s；信号化夹具 6 车：3/6 到达、锁 0。
- `example/t1-traffic/run.bat` 五段 ALL GREEN（含 signals 行与 locked-end 行的 findstr 锚）。

---

## 8. M-T2 调度器增强（traffic-m2，2026-11-14）

### 8.1 机制（TRAFFIC-PLAN §10 拍板 Q-T9/Q-T10/Q-T11）

- **锁模型（Q-T9）**：节点锁从单持有者升级为「组共享 + 转弯独占」——直行车申请所属组（组 0 x 轴 / 组 1 z 轴）共享通行权（同组多车并行：对向/同向直行右行车道分离不冲突）；转弯车（当前 lane × next lane 方向 cos < 0.7071，偏转 ≥45°）申请节点独占（1 车道路网左转穿对向直行）。授予 = 队首裁决：组空即授（M-T1 单持有语义兼容）、组非空仅 axisPure 节点共享；跨组互斥（交叉冲突）；FIFO 队序天然防转弯饿死（turn 前的组授完、turn 排到队首后其后的组全停）。**axisPure 守卫**：进入边含斜边（dx/dz 皆非 0）的节点组锁退化单持有者——「同组=不冲突」假设只在严格正交网格成立。
- **gap-out（Q-T10）**：绿相活跃组本帧零在线等待车持续 `gap_out_s` → frame 跳到本相黄灯起点（stateForEdge 纯函数自动跟进，整数帧确定）；最短绿时防抖 `min(greenS, 2s)` 窗口内不触发；demand 由 sim 逐帧对在线等待车 `reportWaiting` 上报（advanceFrame 用上帧 demand 判定）。**缺省 0 = 关（M-T1 固定周期逐位行为冻结）**；开启位 = phases 契约条目 `"gap_out_s"` 或 tool `--actuated`（全信号节点 2s）。
- **loop（Q-T11）**：`TrafficParams.loop` + 环闭合判定（末 lane toNode == 首 lane fromNode）→ routeIdx 回绕续行；tool `--loop` 构造环 route（start→goal ⊕ goal→start 回程 path 去首尾衔接），不可环回落单程。

### 8.2 死锁判定口径修正（M-T2 落码期实测）

`locked end` 行 = **终态持锁节点数**（含穿越中）；死锁退出条件 = 持锁车**停滞在圈内**（speed==0 且距节点 ≤ clearEff）——loop 环上车流不消散，终帧持锁穿越车是正常交通。诊断行（LOCK dump）保留，触发条件同步改为停滞。**t1 卡 city 段锚随此语义重拍**（locked end 0 → distance 646.9m 确定性值）。

### 8.3 对向豁免口径升级（城市实测 min gap 0.557m 误报驱动）

sameRoad（无序节点对相同）不覆盖**共线相邻路段**：同一条水平街 e4(+X)/e7(-X) 在共用路口节点两侧，对向车道横距 0.5m = lane offset 设计值。升级为几何判据：方向精确相反（dot ≤ -0.999）且共线（|叉积|/|a| < 0.75m = offset 和 0.5 + 余量；最小平行街距 4.0m 不受影响）。修正后城市 min gap 回 1.35m 基线。

### 8.4 验收记录

- selftest **4685 断言**全绿（⑩ 段：gap-out 帧跳变 481 / 需求反锚 / 防抖 / loop 环 odometer>50m+双跑 memcmp+单程对照 / 转弯独占串行+heldNodeCount 0+双跑）；ctest `m-t0-traffic-*` 9/9 ×2 双后端。
- 城市 32 车（49 lock-only 节点）：**arrived 17/32 / 646.9m（M-T1 基线 7/32 / 475.5m，组锁并行 +143%）**、min gap 1.35m、rc 0；gap-out 夹具 6 车 locked 0；loop 4/4 闭合环。
- `example/t2-traffic/run.bat` 六段 ALL GREEN；regression `--only t2-traffic` + `t1-traffic`/`t0-traffic` 保形。

## 9. M-T3 CityCar 接入 viewer（traffic-m3，2026-11-14）

拍板与证据链见 `TRAFFIC-PLAN.md` §11（Q-T12=A ECS 系统 / Q-T13=A `--traffic-city` / Q-T14=最小集）。三处参考：UE ZoneGraph 本体只有「图数据+查询」（`CalculateLocationAlongLane` ≈ `TrafficSim::carPosition` 同构 API），移动由独立消费方驱动；bevy FixedMain / Carrot World::tick 口径已在 `tick_groups.hpp`/`System.hpp` 留痕复核；`NavAgentSystem`（M-N6）= ECS 固定步长系统同构先例。

### 9.1 新增交付物（core）

| 文件 | 内容 |
|---|---|
| `nl/scene/TrafficCarComponent.hpp` + `core/src/scene/TrafficCarComponent.cpp` | POD 组件（route/s/speed/variant/enabled），**零 nl/traffic 依赖**（scene→traffic 反向依赖禁令，NavAgent 同款）；serialise roundtrip |
| `nl/traffic/TrafficCarSystem.hpp` + `core/src/traffic/TrafficCarSystem.cpp` | `SignedSystem<TrafficCar, Transform>`；内持 LaneGraph 值拷 + TrafficSim + 固定步长累加器（dt=1/60、卡顿帧防爆 `kMaxStepsPerTick=8`）；`carPose` 写回位置 {x,0,z} + yaw 四元数 |
| `TrafficSim::carPose`（纯新增） | carPosition 插值逻辑 + 所在段行进方向 `atan2(dx,dz)`；既有函数/锚零触碰 |

### 9.2 viewer 旗标（runScene 可选分支，不指定时逐位走原路径）

- `--traffic-city <DIR>`：DIR 含 `city.scene.engine` + `roadnet.json`（citygen 接口铁律产物，t3 资产 = w3-m1 场景 + w0-m3 roadnet，同 seed 12345 size 6 几何同源）。隐含 `--tick serial`；Spin/ReadCount 不注册（Write<Transform> 冲突，车位姿由 sim 独占）。`car_*` 前缀实体挂 TrafficCarComponent；roadnet 缺失/init 失败 = exit 2。
- `--traffic-steps <N>`：每渲染帧强制 N 个逻辑 tick（nav-crowd-steps 同款；0 = GameClock 自然节奏）。截图口径 8 帧×75 = 600 tick ≈ 10s 车流。
- `--traffic-seed <N>`：route 生成的 fnv1a64 种子（默认 12345；与 city-tools `assetVariantHash` 同常数同字节序）。

### 9.3 route 生成（零 RNG）

起点 lane = `fnv1a64(seed,"rout",carIdx) % laneN`；终点 = 闭合候选集（`toNode == L0.fromNode` 且 `fromNode != L0.toNode`，排除直接掉头回环）中 hash 选取，`findPathOnLanes` 连路 → 环闭合 + `loop=true` 永续行驶；闭合集空回落单程。起步弧长 hash 散布 `[0.1,0.5]·len`（42 车不同点叠置）。车序 = EntityID 升序。

### 9.4 selftest ⑪（traffic-tools）

ECS 写回（init 车数/route 落表/N tick Transform 落 lane 几何 + yaw 单位四元数）／确定性（同 seed 双 World memcmp 逐位）／serialise roundtrip 无损／卡顿帧防爆（tick 10/60 恰补 8 步）／负向（无 enabled 实体 init 显式失败）。ctest `m-t0-traffic-*` 既有用例覆盖（selftest 单命令全段执行）。

### 9.5 验收记录

- selftest **4740 断言**全绿（⑪ 段 55 断言：ECS 写回 init/route 落表/Transform 落 lane 几何+yaw 单位四元数；同 seed 双 World memcmp 逐位；serialise roundtrip 无损；卡顿帧防爆 tick(10/60) 恰补 8 步；无 enabled 实体 init 显式失败）；ctest `m-t0-traffic-*` 双后端全绿。
- `example/t3-traffic-city/run.bat` 六段 ALL GREEN（selftest / top+street 双后端 4 图 + register/odometer 锚 / det 双跑 PNG fc 逐字节 / negative exit 2 / usage exit 2）。
- 回归 `--only t3-traffic-city` 4/4 + `t2-traffic` 12/12 + `t0-traffic` 12/12 全 bytes；cases.json 卡 134（selftest 断言数锚 4685→4740 七处同步，t2 run.bat 锚同步）。
- 车流证据：600 逻辑 tick odometer 3437.15m 单调；街景机位车流位移/朝向可辨（w3-m1 静态基线同机位对照无渲染回归）。
