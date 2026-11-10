# P8-DRIVE-PLAN — 驾驶能力轴：碰撞事件桥 + 载具物理（A 类并行轨道，2026-11-10 立项）

> 起因：城市驾驶开发顺序第 2、3 步（`docs/GAME-DEV-ORDER.md` §2，1~5 是一条硬链，做完即「空场景能开车」）。
> **合轨拍板（2026-11-10 用户）**：初版按「一步一计划」拆两轨（collide/vehicle），用户复核「为啥不放在一起做」
> 后拍板合一条轨——理由：单人串行、同扩 `phys-tools/main.cpp` + `CMakeLists.txt`（同文件并行改=真冲突面）、
> 事件流的第一个消费者就是载具（车撞墙）、merge 一次收口一次。
> **现状实证**：core 全目录 grep 零 `ContactListener` / trigger 回调（物理只会算不会说）；
> vendored Jolt **全量在库**（`Physics/Vehicle/` 全模块 + `Physics/Collision/ContactListener.h`）——
> 两步都是纯集成面，零 solver 代码。
> **性质**：A 类并行轨道（不碰 `kStage`），走工具版交代（phys-tools selftest / trace→check / 展示台 tool 卡）；
> M-D3 是全轨唯一触碰渲染接线面（`viewer/main.cpp` + `CMakeLists.txt`）的里程碑。
> **归属**：track `drive`，里程碑 M-D0~M-D3；tag 名字制（`drive-m0` 起），不占 m0.* 编号。

## 1. 目标

给引擎接上「碰撞事件感知 + 能开的虚拟轮式车」两块驾驶能力基座，四步递进、零/低渲染依赖：

| 段 | 内容 | 落点 |
|---|---|---|
| **M-D0 物理事件流** | Jolt `ContactListener` 三回调接进 `nl/physics`，事件以 Jolt-free POD 流出，per-frame 可 dump TSV | `core/include/nl/physics/Events.hpp`（新）+ `World.cpp`（唯一 Jolt 符号面内实现 listener） |
| **M-D1 ECS 消费** | owner→EntityID 映射 + 事件双缓冲队列 + 消费 System + TickGroup 时序契约 | `core/src/physics/` 队列 + `scene::System` 消费者 |
| **M-D2 载具无头集成** | `VehicleConstraint` 薄集成：建车 + 驱动输入脚本 + 轨迹/轮态 dump + 三锚场景参数表 | `core/include/nl/physics/Vehicle.hpp`（新，PIMPL）+ `Vehicle.cpp`；phys-tools 扩 selftest |
| **M-D3 驾驶可见** | viewer 训练场：DebugScene 新 scenario + M-I1 驾驶映射实时驱动 + HUD 参数行 + console 实调（车撞事件顺手当 M-D1 消费 demo） | `DebugScene` scenario 2 + viewer 接线 + example 卡 / cases.json |

**关键设计约束**（历史约定对齐）：
- `nl/physics` 头文件零 Jolt 类型纪律不变（PIMPL / POD 视图，照 `World.hpp` BodyInfo 口径）；集成层只做调参/装配/输入/读出，算法全在 vendored Jolt 内。
- 确定性契约照 M-P0：fresh world + 固定步 1/60 + JobSystemSingleThreaded 基线逐位；M-J5c 池模式回调线程不定 → 事件帧末**稳定排序**（key = body 对 id + kind）兜底逐位。
- `BodyInfo.owner`（M-P1 已落，`World.hpp:81`，u64 引擎侧 id ↔ BodyID↔userData 双向映射）是事件桥的既有地基——事件对直接携带 owner 对，ECS 侧零新增映射结构。
- M-GL5 TickGroup 注册制是时序契约的天然承载面（物理 step 在 Physics 组、消费 System 在 PostPhysics 组），不另造调度。
- 载具输入面与 M-I1 精确咬合：`buildDrivingActionMap` 的 W/S=油门±1、A/D=转向±1、Space=手刹 ↔ Jolt `SetDriverInput(forward, right, brake, handBrake)`（`WheeledVehicleController.h:105`，转向是 ±1 左右值非角度）。
- 载具 step 挂 Physics TickGroup（runNormal 物理调试同款位），输入轮询在渲染帧、消费在逻辑帧（GameClock callback）。

## 2. 三处参考口径（立项预口径；M-D0 开工首日按铁律实读点名文件后修订留痕，M-D2 同款重读）

### 事件桥（M-D0/D1）

| 来源 | 预口径 | 用法 |
|---|---|---|
| **Carrot** `engine/physics/` + 事件系统（`Carrot::Event` 通道） | Jolt PhysicsWorld 是否有接触事件面 / trigger 语义，事件走 Async 还是 PassThrough 通道 | 桥的节奏参照；无接触事件面则据实标注（bevy 口径同款处理） |
| **bevy** `crates/bevy_ecs/src/event.rs`（Events<T> 双缓冲） | 事件双缓冲：当帧写、下一 schedule 段读、帧末换页；**bevy 无官方物理**——碰撞事件是社区 crate（avian / bevy_rapier），core 不背 | **消费节奏的锚**：双缓冲「当帧产、次帧读、两帧保活」照抄到 ContactEventStream；泛型框架不做（§4） |
| **UE** `Components/PrimitiveComponent.h`（OnComponentHit / BeginOverlap / EndOverlap 委托） | 玩法面三件套 = Hit（真实碰撞）/ Begin+End Overlap（触发器）；委托挂组件、带 HitResult（位置/法线/相对速度） | **事件字段的锚**：ContactEvent POD 对齐 HitResult 有用子集；overlap 触发器语义不做（§4） |

### 载具（M-D2/D3）

| 来源 | 预口径 | 用法 |
|---|---|---|
| **Jolt vendored**（`Physics/Vehicle/` 实读 + 上游 JoltPhysics Samples 未 vendored，gh-proxy 浅读 `Samples/WheeledVehicle*` 补装配口径） | 装配路径：chassis Body（BoxShape）→ `WheeledVehicleControllerSettings`（每轮 WheelSettings：半径/宽/悬挂 stiffness/damping/travel/maxSteerAngle）→ differential → constraint 加进 body → controller 驱动 | **集成层的唯一权威锚**：一切 API 以 vendored 头为准（行号开工实读留痕） |
| **UE** Chaos 载具（`ChaosVehicles` 插件 ChaosWheeledVehicleMovementComponent） | 引擎/传动/差速/四轮分块 + 输入映射面（throttle/steering/brake/handbrake 分立）+ 轮子状态回读给渲染 | 工业终态**只取思想**：分块结构对齐、输入分立对齐；WheeledVehiclePawn/动画轮同步等渲染侧不做 |
| **bevy / Carrot** | bevy 无官方载具（社区 avian 亦无 vehicle controller）；Carrot 预判无 vehicle 模块 | 据实标注无对应物（铁律：标了就要开工实读确认） |

## 3. 设计（逐里程碑细化，开工前补全锚点表）

### M-D0 物理事件流

- `ContactEvent` POD（Jolt-free）：`{frame, kind(Added=0/Persisted=1/Removed=2), ownerA, ownerB, bodyIdA, bodyIdB, worldPos(xyz), normalY}`——对齐 UE HitResult 有用子集；removed 回调 Jolt 只给 SubShapeIDPair（`ContactListener.h:136`），owner 从既有映射表回查。
- `World` 新探针 `simulateContactProbe(ContactProbeConfig)`：照 `simulateLayerProbe`（M-P1）先例——floor + 静态 blocker + 掉落 probe 三体世界，逐帧事件流随轨迹一起 dump。listener 实现在 `World.cpp`（PIMPL 内），三回调原样转 POD。
- **确定性**：单线程 JobSystem 基线回调内联于 step（帧序即回调序）；池模式回调线程不定 → 帧末按 `(minBodyId,maxBodyId,kind)` 稳定排序后逐位。OnContactRemoved 的派发时机（Jolt 口径：解激活/移除路径，下次 Update 派发）开工实读 `ContactListener.h` + `PhysicsSystem` 确认后写死注释。
- phys-tools 扩 selftest `contacts` 节 + `contact-trace` / `contact-check` 子命令：掉落 → 首次接触帧（added）→ 静置段 persisted → 反弹段 removed→added 重触发 → 静息段无新 added；锚从实测 log 反推（禁手写锚，M-I0/I1 两连犯教训入册）。

### M-D1 ECS 消费

- `ContactEventStream`：双缓冲队列（当帧写 / 次帧读 / 两帧保活，bevy 口径），`drain()` 语义固化。
- demo `scene::System` 消费者：tick 内读上一逻辑帧事件，owner→Entity 走 `scene::World::exists` 过滤（未映射的纯物理 owner 原样透传）。
- 时序契约自证：GameClock Physics 组跑 step → PostPhysics 组跑消费 System（M-GL5 注册制）——selftest 断言「step 同帧产的事件，消费 System 在同帧 PostPhysics 段可见、下一帧不可见」。

### M-D2 载具无头集成

- `VehicleTuning` POD：底盘（halfExtents / mass / 质心高）+ 4 轮（radius / width / 悬挂 stiffness / damping / travel / maxSteerAngle 前轴）+ 传动（auto，engine 参数缺省取 Jolt 默认，测出行为再调）。
- `DriveConfig`：帧数 + **帧序号驱动输入脚本**（照 M-I2 `--input-demo` 先例：f∈[a,b) forward=1 等），输出 per-frame 轨迹（chassis pos / yaw / velocity）+ per-wheel 状态（contact flag / suspensionLength / rotationAngle）——轮子状态回读口径对 UE「动画轮同步」预留。
- 锚场景（参数表三行，GAME-DEV-ORDER 验收口径）：① 直线加速（0→达速帧 + 末速锚）；② 定角转向（转弯半径 / 角速度锚）；③ 手刹急停（刹停帧 + 横摆角锚）。
- phys-tools 扩 selftest `vehicle` 节 + `drive-trace` / `drive-check` 子命令（TSV + 锚闭环）。
- **与 M-D0/D1 的衔接**：载具探针的轮-地接触天然产 added/persisted 事件流——selftest 加一条「车辆行驶段轮接触事件序列」锚，M-D1 的消费契约第一次消费真实玩法数据。

### M-D3 驾驶可见

- `DebugScene` 新 scenario 2 = 车辆训练场（平地 + 直道 + 定角弯道标记物 + 一面撞墙——墙就是 M-D1 事件消费的 demo 载体；亮色车底线框——VehicleConstraint 有真实体，DebugRenderer 自动画）。
- viewer 接线：M-I1 ActionMap 实时轮询（渲染帧）→ GameClock Physics 组 step 前灌 `SetDriverInput`（逻辑帧）→ HUD 追加参数行（speed / gear / 四轮 grounded / 最近碰撞事件，ASCII 口径）→ console cvar 实调（`vehicle.throttleScale` 等，GAME-DEV-ORDER「console 实调」验收口径）。
- example 卡 + cases.json（image 用例 pair：`--drive-demo` 帧序号脚本注入 → 截图帧断言链，照 input-m2 双后端 PNG 基线先例）。

## 4. 不在范围

- 通用 `Events<T>` 事件框架（bevy 泛型 / UE GameplayMessageRouter）——具体类型具体队列，真有第二类事件再抽象。
- 触发器 / sensor 体积语义（Ghost 层 overlap 事件）、`ContactSettings` 响应面修改（回调里改弹性/摩擦）——先只读。
- 多线程事件零锁化、Jolt `BodyActivationListener`（激活/休眠事件，按需后置）。
- 自定义轮胎模型 / 离合器细分调参（Jolt 内建 RayCast 轮胎 + 传动够起步）；多车 / Tracked（履带）/ Motorcycle；载具存档序列化。
- 车轮视觉网格 / 车壳模型（渲染资产留给第 5 步游戏壳之后）；引擎音效（第 11 步打磨波，音频轨已备 AudioWorld）。
- 事件流渲染可视化（debugLines 画接触点）——M-D0~D2 纯逻辑零图形；要看得见留给第 7 步最小可玩版。
- 行人 NPC 与载具交互（Recast 接，第 11 步）。

## 5. 里程碑草案

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M-D0** | `core/physics` 事件流（listener + ContactEvent POD + contactProbe）+ phys-tools `contacts` 节 + trace→check 闭环 + ctest | selftest 数值锚全绿；事件时序逐位（单线程基线 + 池模式排序兜底）；负向退出码 0/1/2；双后端 spot 零回归；tool 卡 |
| **M-D1** | `ContactEventStream` 双缓冲 + demo System 消费者 + TickGroup 时序契约自证 + ECS selftest | 消费节奏锚全绿（当帧产、同帧 PostPhysics 可见、次帧清）；并行 tick 下消费 System 只读零违约；ctest 双后端 |
| **M-D2** | `core/physics/Vehicle` PIMPL 集成 + DriveConfig 输入脚本 + phys-tools 参数表三锚场景 + 轮接触事件锚 + trace→check + ctest | selftest 数值锚全绿（加速/转向/手刹三场景逐位）；轮子状态回读自洽（contact 数 = 着地轮数）；M-D0 契约零回归（contacts 节逐字节不变）；tool 卡 |
| **M-D3** | DebugScene scenario 2 + viewer M-I1 映射驱动 + HUD 参数行 + console cvar 实调 + example 卡 + cases.json + 收口（close 硬闸） | 截图基线双后端 bytes；键鼠实时驾驶肉眼可验；车撞墙事件反馈可见（HUD 行）；console 实调参数生效锚落盘；`server.py --check` exit 0 |

## 6. 留痕

- 2026-11-10：立项出计划。初版拆两轨（`P8-COLLISION-PLAN.md` + `P8-VEHICLE-PLAN.md`，commit `6221620`），
  用户复核「为啥不放在一起做」后拍板合轨 → 本文合并两份内容（M-CB0/CB1 → M-D0/D1，M-V0/V1 → M-D2/D3），
  增补「载具轮接触事件当 M-D1 消费 demo」衔接设计；旧两份文档撤档。
- vendored API 实证（本日 grep 留痕）：`ContactListener.h:106/119/136`（三回调签名）、
  `WheeledVehicleController.h:105-120`（SetDriverInput/SetForwardInput/SetRightInput/SetBrakeInput/SetHandBrakeInput）、
  `Physics/Vehicle/` 全模块在库（含 Wheel.h / VehicleEngine / VehicleTransmission / VehicleDifferential / AntiRollBar）、
  `World.hpp:79-85`（BodyInfo.owner 既有地基）、layers 表（World.hpp:60-65）。
- 冲突面预告：M-D3 是本轨唯一触碰 `viewer/main.cpp` + `CMakeLists.txt` 渲染接线面的里程碑；
  M-D0~D2 只动 core/physics + phys-tools + CMake（工具 target）。
