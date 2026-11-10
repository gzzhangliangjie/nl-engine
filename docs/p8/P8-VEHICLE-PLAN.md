# P8-VEHICLE-PLAN — 载具物理：Jolt VehicleConstraint 薄集成层（A 类并行轨道，2026-11-10 立项）

> 起因：城市驾驶开发顺序第 3 步（`docs/GAME-DEV-ORDER.md` §2，「1~5 是一条硬链」——做完即「空场景能开车」）。
> Jolt 5.3.1 vendored **全量在库**（本轮实证：`thirdparty/jolt/Jolt/Physics/Vehicle/` 含 VehicleConstraint /
> WheeledVehicleController / VehicleEngine / VehicleTransmission / VehicleDifferential / Wheel / AntiRollBar
> 及 Motorcycle / Tracked 控制器），只差 nl 集成面。
> **性质**：A 类并行轨道（不碰 `kStage`），验收照 GAME-DEV-ORDER 口径：phys-tools 式参数表 + console 实调。
> **归属**：track `vehicle`，里程碑 M-V0~M-V1；tag 名字制（`vehicle-m0` 起），不占 m0.* 编号。
> **前置**：GAME-DEV-ORDER 排第 2 步（碰撞事件桥）之后。**技术实情**：`VehicleConstraint` 轮子走
> raycast 探地，不依赖接触事件——该前置是玩法面（撞了要有反馈）而非接口面，两轨可并行开 wt（文件重叠仅 CMakeLists.txt）。

## 1. 目标

给引擎一个**能开的虚拟轮式车**，分两段：

| 段 | 内容 | 落点 |
|---|---|---|
| **无头集成（M-V0）** | `VehicleConstraint` 薄集成：建车（底盘 + 4 轮悬挂/引擎/传动）+ 驱动输入脚本 + 轨迹 dump + 参数锚 | `core/include/nl/physics/Vehicle.hpp`（新，PIMPL）+ `Vehicle.cpp`；phys-tools 扩 selftest / trace→check |
| **驾驶可见（M-V1）** | viewer 训练场：DebugScene 新 scenario + M-I1 驾驶映射实时驱动 + HUD 参数行 + console 实调 | `DebugScene` scenario 2 + viewer 接线 + example 卡 / cases.json（唯一触碰渲染处，只加不改） |

**关键设计约束**（历史约定对齐）：
- `nl/physics` 集成层纪律照 phys 全轨：**零 solver 代码、零图形符号**——调参、装配、输入灌入、状态读出，算法全在 Jolt vendored 内。
- 头文件零 Jolt 类型（PIMPL），确定性契约照 M-P0：fresh world + 固定步 1/60 + 单线程 JobSystem 基线逐位。
- 输入面与 M-I1 精确咬合：`buildDrivingActionMap` 的 W/S=油门±1、A/D=转向±1、Space=手刹 ↔ Jolt `SetDriverInput(forward, right, brake, handBrake)`（`WheeledVehicleController.h:105`，转向是 ±1 的左右值非角度）——映射口径直接落注释。
- 载具 step 挂 M-GL5 Physics TickGroup（runNormal 物理调试同款位），输入轮询在渲染帧、消费在逻辑帧（GameClock callback）。

## 2. 三处参考口径（立项预口径；M-V0 开工首日按铁律实读点名文件后修订留痕）

| 来源 | 预口径 | 用法 |
|---|---|---|
| **Jolt vendored**（`Physics/Vehicle/` 实读 + 上游 JoltPhysics Samples 未 vendored，gh-proxy 浅读 `Samples/WheeledVehicle*` 补装配口径） | 装配路径：chassis Body（BoxShape）→ `WheeledVehicleControllerSettings`（每轮 WheelSettings：半径/宽/悬挂 stiffness/damping/travel/maxSteerAngle）→ differential → constraint 加进 body → controller 驱动 | **集成层的唯一权威锚**：一切 API 以 vendored 头为准（行号开工实读留痕） |
| **UE** Chaos 载具（`ChaosVehicles` 插件 ChaosWheeledVehicleMovementComponent） | 引擎/传动/差速/四轮分块 + 输入映射面（throttle/steering/brake/handbrake 分立）+ 轮子状态回读给渲染 | 工业终态**只取思想**：分块结构对齐、输入分立对齐；WheeledVehiclePawn/动画轮同步等渲染侧不做 |
| **bevy / Carrot** | bevy 无官方载具（社区 avian 亦无 vehicle controller）；Carrot 预判无 vehicle 模块 | 据实标注无对应物（铁律：标了就要开工实读确认） |

## 3. 设计（逐里程碑细化，开工前补全锚点表）

### M-V0 无头集成

- `VehicleTuning` POD：底盘（halfExtents / mass / 质心高）+ 4 轮（radius / width / 悬挂 stiffness / damping / travel / maxSteerAngle 前轴）+ 传动（auto，engine 参数缺省取 Jolt 默认，测出行为再调）。
- `DriveConfig`：帧数 + **帧序号驱动输入脚本**（照 M-I2 `--input-demo` 先例：f∈[a,b) forward=1 等），输出 per-frame 轨迹（chassis pos / yaw / velocity）+ per-wheel 状态（contact flag / suspensionLength / rotationAngle）——轮子状态回读口径对 UE「动画轮同步」预留。
- 锚场景（参数表三行，GAME-DEV-ORDER 验收口径）：① 直线加速（0→达速帧 + 末速锚）；② 定角转向（转弯半径 / 角速度锚）；③ 手刹急停（刹停帧 + 横摆角锚）。
- phys-tools 扩 selftest `vehicle` 节 + `drive-trace` / `drive-check` 子命令（TSV + 锚闭环，锚从实测反推——M-I0/I1 禁手写锚纪律）。

### M-V1 驾驶可见

- `DebugScene` 新 scenario 2 = 车辆训练场（平地 + 直道 + 定角弯道标记物；亮色车底线框——VehicleConstraint 有真实体，DebugRenderer 自动画）。
- viewer 接线：M-I1 ActionMap 实时轮询（渲染帧）→ GameClock Physics 组 step 前灌 `SetDriverInput`（逻辑帧）→ HUD 追加参数行（speed / gear / 四轮 grounded，ASCII 口径）→ console cvar 实调（`vehicle.throttleScale` 等，GAME-DEV-ORDER「console 实调」验收口径）。
- example 卡 + cases.json（image 用例 pair：`--drive-demo` 帧序号脚本注入 → 截图帧断言链，照 input-m2 双后端 PNG 基线先例）。

## 4. 不在范围

- 自定义轮胎模型 / 离合器细分调参（Jolt 内建 RayCast 轮胎 + 传动够起步，行为不够再进 `VehicleEngine` 参数面）。
- 载具间 / 载具-城市场景碰撞事件玩法（M-CB0 收口后第 7 步一起）。
- 多车 / Tracked（履带）/ Motorcycle（Jolt 有但起步不做）；载具存档序列化。
- 车轮视觉网格 / 车壳模型（渲染资产留给第 5 步游戏壳之后）；引擎音效（第 11 步打磨波，音频轨已备 AudioWorld）。
- 行人 NPC 与载具交互（Recast 接，第 11 步）。

## 5. 里程碑草案

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M-V0** | `core/physics/Vehicle` PIMPL 集成 + DriveConfig 输入脚本 + phys-tools 参数表三锚场景 + trace→check + ctest | selftest 数值锚全绿（加速/转向/手刹三场景逐位）；轮子状态回读自洽（contact 数 = 着地轮数）；双后端 spot 零回归；tool 卡 |
| **M-V1** | DebugScene scenario 2 + viewer M-I1 映射驱动 + HUD 参数行 + console cvar 实调 + example 卡 + cases.json | 截图基线双后端 bytes；键鼠实时驾驶肉眼可验；console 实调参数生效锚落盘；`server.py --check` exit 0 |

## 6. 留痕

- 2026-11-10：立项出计划（用户指令「碰撞事件桥和载具物理先出文档」）。vendored 实证：`Physics/Vehicle/` 全模块在库（含 `Wheel.h` / `WheeledVehicleController.h:105-120` SetDriverInput/SetForwardInput/SetRightInput/SetBrakeInput/SetHandBrakeInput）；`ContactListener.h` 在 `Physics/Collision/`。与碰撞轨道（`P8-COLLISION-PLAN.md`）串并行取舍见该文件 §6；本轨 M-V1 是两轨中唯一触碰 `viewer/main.cpp` + `CMakeLists.txt` 渲染接线面的里程碑（merge 冲突面预告）。
