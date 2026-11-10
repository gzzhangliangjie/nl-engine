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

## 2. 三处参考口径（2026-11-10 M-D0 开工首日实读实证，替换立项预口径；M-D2 开工同款重读）

> 实读留痕（AGENTS.md 决策前置铁律：三处定向读、不漫游，UE 只读不抄）：
> - **Carrot**：`engine/engine/physics/PhysicsSystem.h:73` / `PhysicsSystem.cpp:124,345`、`PhysicsBody.h:53` / `PhysicsBody.cpp:63-94`、`BodyUserData.h`；脚本侧消费者 `engine/engine/scripting/CSharpBindings.cpp:1037,1072`。
> - **bevy**（本地 main / v0.16.0-rc.4）：`crates/bevy_ecs/src/event/mod.rs`（`update()` / `update_drain()` / 两帧保活测试 `:573`、double buffering 注释 `:716`）、`examples/events.rs`；`src/event/` 下仅 `mod.rs` + `trigger.rs`，集合实现 `collections.rs` 本地 sparse clone 未检出，据实标注。
> - **UE 5.8.4**：`Runtime/Engine/Classes/Components/PrimitiveComponent.h:274,276,278,1457,1468,1475`；`Plugins/Experimental/ChaosVehiclesPlugin/Source/ChaosVehicles/Public/ChaosVehicleMovementComponent.h:887,899,903,919`（派生类 `ChaosWheeledVehicleMovementComponent.h:633`）。
> - **开发历史**：`docs/MILESTONES.md`、`docs/carrot-history.md`、`docs/bevy-history.md`、`docs/ROADMAP.md` 步骤→代码对照总表、`.workbuddy/memory/MEMORY.md`、`.workbuddy/memory/2026-11-10.md`、`docs/p8/PHYS-TOOLS-GUIDE.md`、`docs/p8/INPUT-TOOLS-GUIDE.md`、本计划。
> - **vendored 复核**：`thirdparty/jolt/Jolt/Physics/Collision/ContactListener.h:106,119,136`（另见 `:73,101,126-128`）。

### 2.1 事件桥（M-D0/D1）实证口径

| 来源 | 实读结论 | 对本轨的约束 |
|---|---|---|
| **Carrot** | **有接触事件面，且不是通用 Event 总线**：`PhysicsSystem : public JPH::ContactListener`（`PhysicsSystem.h:73`）、`jolt->SetContactListener(this)`（`.cpp:124`），**只实现 `OnContactAdded`**（`.cpp:345`），经 `BodyUserData::ptr` 转成 per-body `BaseBody::onContactAdded(otherBodyId)`。`PhysicsBody.cpp:68` `addContactListener(fn, mainThreadOnly)` 是**双通道**：`false` → `contactAddedCallbacks` 在物理线程立即回调；`true` → 入队 `contactAddedSinceLastPostPhysics`，由 `dispatchEventsPostPhysicsMainThread()`（`.cpp:87-94`）帧后主线程派发并清空 | **「Physics 组产 / PostPhysics 组消费」的既有同构先例**，直接印证 M-D1 的 TickGroup 时序契约。只取「帧内入队 → 帧末派发」节奏，不照抄 per-body 回调粒度 |
| **bevy** | 事件模块实为 `bevy_ecs/src/event/mod.rs`（立项时写的 `event.rs` 路径不成立）；`Events<T>::update()` 换页，测试 `"reader_missed missed events unread after two update() calls"`（`:573`）与 `:716` double buffering 注释，实证「当帧产、次帧读、两帧保活」；`update_drain()`（`:688`）为帧末清页面。**bevy 无官方物理**，碰撞事件属社区 crate（avian / bevy_rapier），core 不背 | `ContactEventStream` 双缓冲 + `drain()` 语义照此锚；**泛型 `Events<T>` 不做**（§4 范围外），具体类型具体队列 |
| **UE 5.8.4** | 玩法面三件套委托签名实锤：`OnComponentHit` 五参（`HitComponent / OtherActor / OtherComp / NormalImpulse / const FHitResult&`，`:274`、成员 `:1457`）、`OnComponentBeginOverlap` 六参（含 `bFromSweep` + `SweepResult`，`:276`、`:1468`）、`OnComponentEndOverlap` 四参（`:278`、`:1475`）；均为 `DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE`，挂组件、立即派发 | `ContactEvent` POD 对齐 `FHitResult` 有用子集；**overlap / 触发器语义不做**（§4）；UE 的立即委托与 Carrot/bevy 的缓冲派发不同，见 §2.3 |
| **Jolt vendored** | `:106/119/136` 三回调签名与立项留痕一致；**新增硬约束**：`:73` / `:101` / `:126-128` 明示 `OnContactRemoved` 期间 body 可能已移除销毁、**禁止访问 body**，要求「在 Added / Persisted 阶段缓存所需数据，removed 只用缓存」；`:101` 亦注明休眠路径的派发时机 | 计划 §3 立项时假设 removed 可回查 owner 映射，**实证后收窄**：owner 对在 Added / Persisted 阶段缓存，`SubShapeIDPair → owner 对` 表由本轨自持，removed 回调不得回查 Jolt body；见 **Q2** |

### 2.2 载具（M-D2/D3）实证口径

| 来源 | 实读结论 | 对本轨的约束 |
|---|---|---|
| **UE 5.8.4 ChaosVehicles** | 派生类 `UChaosWheeledVehicleMovementComponent`（`ChaosWheeledVehicleMovementComponent.h:633`）；输入面四分立 API 在基类 `ChaosVehicleMovementComponent.h`：`SetThrottleInput(float)` `:887` / `SetBrakeInput(float)` `:899` / `SetSteeringInput(float)` `:903` / `SetHandbrakeInput(bool)` `:919` | 与 M-I1 `buildDrivingActionMap`（W/S 油门、A/D 转向、Space 手刹）逐项对齐；**只取分块 / 输入分立思想**，WheeledVehiclePawn、动画轮同步等渲染侧不做 |
| **Jolt vendored** | `Physics/Vehicle/` 全模块在库（立项已实证）；`WheeledVehicleController.h:105-120` `SetDriverInput(forward, right, brake, handBrake)`，转向是 ±1 right 值非角度 | 集成层唯一权威锚，一切 API 以 vendored 头为准；M-D2 开工按行号再实读留痕 |
| **bevy / Carrot** | bevy 无官方载具（社区 avian 亦无 vehicle controller）；Carrot `engine/engine/physics/` 全目录 16 文件 grep 零 `Vehicle` 模块——立项预口径「预判无」**实锤成立** | 据实标注无对应物，不强行找参照 |

### 2.3 三端口径差异（需用户拍板，见 Q1）

| 来源 | 派发时机 | 消费者粒度 | 本轨取舍 |
|---|---|---|---|
| UE | 委托立即派发（挂组件） | 组件 / Actor | 只借 POD 字段，不借派发时机 |
| Carrot | 帧内入队 → `dispatchEventsPostPhysicsMainThread()` 帧末主线程派发 | per-body 回调 | 借时序（Physics 产 / PostPhysics 消费），不借 per-body 粒度 |
| bevy | `Events<T>` 双缓冲换页，两帧保活 | 任意 System + Cursor | 借缓冲与 `drain()` 语义 |

> 三者口径不一致。计划既定路线是「bevy 双缓冲 + M-GL5 TickGroup 时序」，Carrot 实读**只作同构佐证、不改既定路线**；按 AGENTS.md「不许擅做决定」，此综合口径列为 **Q1** 待确认。

## 3. 设计（逐里程碑细化，开工前补全锚点表）

### M-D0 物理事件流

- `ContactEvent` POD（Jolt-free）：`{frame, kind(Added=0/Persisted=1/Removed=2), ownerA, ownerB, bodyIdA, bodyIdB, worldPos(xyz), normalY}`——对齐 UE HitResult 有用子集；removed 回调 Jolt 只给 SubShapeIDPair（`ContactListener.h:136`），owner 对由 Added / Persisted 阶段缓存的 `SubShapeIDPair → owner` 表提供（`ContactListener.h:126-128`：removed 期间禁访问 body），见下方开工锚点表与 Q2。
- `World` 新探针 `simulateContactProbe(ContactProbeConfig)`：照 `simulateLayerProbe`（M-P1）先例——floor + 静态 blocker + 掉落 probe 三体世界，逐帧事件流随轨迹一起 dump。listener 实现在 `World.cpp`（PIMPL 内），三回调原样转 POD。
- **确定性**：单线程 JobSystem 基线回调内联于 step（帧序即回调序）；池模式回调线程不定 → 帧末按 `(minBodyId,maxBodyId,kind)` 稳定排序后逐位。OnContactRemoved 的派发时机已实读 `ContactListener.h:101,126-128`（解激活 / 移除路径，下次 Update 派发；回调期间 body 可能已销毁）——落码时写死进 `Events.hpp` 头注释。
- phys-tools 扩 selftest `contacts` 节 + `contact-trace` / `contact-check` 子命令：掉落 → 首次接触帧（added）→ 静置段 persisted → 反弹段 removed→added 重触发 → 静息段无新 added；锚从实测 log 反推（禁手写锚，M-I0/I1 两连犯教训入册）。

#### M-D0 开工锚点表（2026-11-10 实读后补全）

| 项 | 落点 / 口径 | 依据 |
|---|---|---|
| `ContactEvent` POD | `{frame, kind(Added=0/Persisted=1/Removed=2), ownerA, ownerB, bodyIdA, bodyIdB, worldPos(xyz), normalY}`，Jolt-free | UE `FHitResult` 有用子集 + `World.hpp:79-85` `BodyInfo.owner` 既有地基 |
| Removed 的 owner 来源 | **Added / Persisted 阶段缓存 `SubShapeIDPair → owner 对`**，removed 只读缓存、禁访问 Jolt body；缓存表 **removed 派发后清**（Q2 已按建议口径执行） | `ContactListener.h:126-128`（新增实证硬约束）；立项假设据此收窄 |
| 稳定排序 | 帧末按 `(minBodyId, maxBodyId, kind)` 排序；单线程基线回调内联于 step（帧序即回调序），池模式兜底逐位 | M-P0 确定性契约 + M-J5c 池模式回调线程不定 |
| 探针与 dump | `simulateContactProbe(ContactProbeConfig)` 照 `simulateLayerProbe`（M-P1）三体世界；逐帧事件流随轨迹 dump TSV | `PHYS-TOOLS-GUIDE.md` §1 / §3 |
| 工具面 | phys-tools 扩 `contacts` selftest 节 + `contact-trace` / `contact-check`；**锚一律从实测 log 反推，禁手写** | M-I0 / M-I1 手写锚两连犯教训；`INPUT-TOOLS-GUIDE.md` §2 trace→check 闭环 |
| 退出码 | `0` PASS / `1` 断言或运行失败 / `2` 用法错误（无参、未知子命令、非法参数） | `PHYS-TOOLS-GUIDE.md` §2；展示台 `expectExit` 约定 |
| 改动面 | `core/include/nl/physics/Events.hpp`（新）、`core/src/physics/World.cpp`（唯一 Jolt 符号面）、`phys-tools/main.cpp` + `CMakeLists.txt`；**零图形、零 `kStage`** | 本计划 §1；A 类轨道纪律 |
| 时序契约 | 事件在 Physics 组 step 内产出入队、PostPhysics 组消费；M-D1 由 M-GL5 注册制承载 | Carrot `dispatchEventsPostPhysicsMainThread` 同构 + 本计划 §1 |

### M-D1 ECS 消费（2026-11-10 收官，开工锚点表见下）

- `ContactEventStream`（`core/include/nl/physics/ContactEventStream.hpp`，Jolt-free）：双缓冲两页 + 逐页 read cursor，`update()` 帧首换页丢最老页、`publish()` Physics 组写当前页、`drain()` PostPhysics 组按「旧页→当前页」时间序取出未读事件并推进 cursor。bevy `event/mod.rs:573/715-718` 两帧保活与 `update_drain()` 口径，泛型 `Events<T>` 不做（§4）。
- demo `scene::System` 消费者 `ContactEventSystem`：直接继承 `System`（读写集空 = 自足，可并行，`System.hpp` 契约 2）；`tick` 内 `drain()`，owner→Entity 走 `scene::World::exists` 过滤，**未映射的纯物理 owner 只计数不丢事件**（Carrot `CSharpBindings.cpp:1037-1056` 双 exists 过滤同构）。
- 时序契约自证：M-GL5 `TickScheduler` 四组枚举序即执行序，**故意乱序注册**验证 `runTick` 仍按枚举序跑；Physics 组 `update()` + `publish()` 真实 M-D0 事件，PostPhysics 组 `scene::World::tick` 触发消费者，PostUpdateWork 组断言组序。
- 工具面照 M-D0 trace→check：`ecs-trace` 输出 F/E 两类数据行（F = 每逻辑帧 publish/consume/owner 统计，E = 每消费事件明细），`ecs-check` 只信 F/E 行独立复验，锚从真实 TSV 反推。

#### M-D1 开工锚点表（2026-11-10 实读后补全）

| 项 | 落点 / 口径 | 依据 |
|---|---|---|
| 换页语义 | `update()` 在 Physics 组帧首调用；丢最老页未读事件并回收为写页 | bevy `event/mod.rs:680-701` `update_drain()` + `:704-719` 两次 update 才 empty |
| 同帧可见 | `drain()` 同时读旧页与当前写页，故帧 N publish 的事件在帧 N PostPhysics 可见 | §5 验收判据 |
| 次帧清 | 已 drain 的事件 cursor 已推进 → 帧 N+1 不可见；**未读**事件存活到帧 N+1、第三次 `update()` 才丢（两帧保活） | §2.1 bevy 口径与 §5 简写的精确化，二者不矛盾 |
| 组序 | `TickGroup` 枚举序（PrePhysics→Physics→PostPhysics→PostUpdateWork）为唯一顺序源，乱序注册不改执行序 | `tick_groups.hpp` 确定性纪律；UE `EngineBaseTypes.h:83-110` |
| owner 映射 | `ContactEvent.ownerA/B`（u64）直接当 `scene::EntityID`；零新增映射表 | §1 `BodyInfo.owner` 既有地基 |
| 并行契约 | 消费者不声明读写集 → 自足系统，`tickParallel` 零结构变更违约 | `System.hpp` 契约 2 |
| 改动面 | `core/{include/nl/physics,src/physics}` 新 4 文件 + 根 CMakeLists 两行 + `phys-tools`；零图形、零 `kStage` | A 类轨道纪律 |

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
| **M-D1** | `ContactEventStream` 双缓冲 + demo System 消费者 + TickGroup 时序契约自证 + ECS selftest | 消费节奏锚全绿（同帧 PostPhysics 可见、**已消费**事件次帧不可见、**未读**事件两帧保活、第三次 `update()` 清）；并行 tick 下自足消费 System 零违约；ctest 双后端 |
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

- 2026-11-10（**M-D0 开轨**）：用户指令「开新 wt 做 P8-DRIVE-PLAN.md」。主树注册提交 `b4ab3b0`——
  `tools/wt_pool.py` TRACKS 新增 `drive`、`docs/PARALLEL-TRACKS.md` §7.1 新增轨道行、
  `docs/GAME-DEV-ORDER.md` 第 2/3 步状态改「在轨」。相机轨收官会话的未提交改动由其自行提交
  `4ff5551` + `351d216` 进入父链，本轮未代提交（定点暂存避开同文件改动）。
  worktree `E:/UE_STUDY_2026/nl-engine-drive`（分支 `feat/drive`，起点 `b4ab3b0`，
  **已含 camera merge `7b0f24a` 全部内容**：`git merge master` 实测 `Already up to date`、
  `rev-list --count HEAD..master` = 0）；junction 共享 `.workbuddy` / `thirdparty/slang/dist`
  + 6 文件 skip-worktree；`wt_pool claim --track drive --milestone M-D0` 在册。
- 2026-11-10（同日）：M-D0 开工首日按 AGENTS.md 铁律实读三处点名文件，§2 由「立项预口径」升级为实证口径，
  §3 补 M-D0 开工锚点表。两处实质修订：① **Carrot 确有接触事件面且为双通道**（立项预口径「是否有」偏差最大）；
  ② **Jolt `OnContactRemoved` 禁访问 body**，owner 对须前置缓存。另修正 bevy 事件模块路径
  `event.rs` → `event/mod.rs`。待拍板 Q1~Q4 见 §7。

- 2026-11-10（**M-D0 收官**）：用户指令「开工」，Q1/Q2 按 §7 建议口径执行（Q1 维持 bevy 双缓冲 +
  M-GL5 TickGroup、Carrot 只作同构佐证；Q2 缓存 removed 派发后清），Q3 边界 = 完整 M-D0 落码收口，
  Q4 编号 `track drive / M-D0` 不变。落码：`core/include/nl/physics/Events.hpp`（新，Jolt-free POD）、
  `World.hpp` 新增 `simulateContactProbe`、`World.cpp` 内 `ContactCollector`（唯一 Jolt 符号面，三回调）、
  `phys-tools` 扩 `contact-trace` / `contact-check` / selftest `contacts` 节 + ctest 三条。
  **实测锚（从真实 TSV 反推，非手写）**：240 帧 41 事件 = Added 6 / Persisted 29 / Removed 6；
  首个 Added f44、首个 Removed f46、末事件 f185、其后 55 帧静息；接触点 (0,3,0)、normalY=1.0、
  探针静息 y=3.5；`cache_misses=0` —— 6 个 Removed 全部命中缓存并携带 owner 对，Q2 口径实证成立。
  **实现口径细化（有 vendored 依据，不改架构）**：缓存 key 取 `SubShapeIDPair` 的 **body id 部分**
  （min/max 归一），不取 subshape id —— `ContactListener.h:133-134` 明示 subshape id 来自上一帧、
  shape 变化后可能失效；body id 直接来自回调参数，removed 期间零 Jolt body 访问。
  M-D1 双缓冲消费者 / M-D2 载具仍按 §5 排期，未提前落码。

- 2026-11-10（**M-D1 收官**）：用户指令「下一步」= 完整 M-D1 落码收口。开工按铁律重读三处**本里程碑点名面**（Carrot `PhysicsBody.cpp:63-94` + `CSharpBindings.cpp:1033-1075` 双 exists 过滤；bevy `event/mod.rs:573/680-701/704-719` 两帧保活与 `update_drain()`；UE `EngineBaseTypes.h:83-110` `TG_PostPhysics` 组序），**未复用 M-D0 的接触事件面证据**。落码：`ContactEventStream`（双缓冲 + cursor + update/publish/drain）+ `ContactEventSystem`（自足 `scene::System` 消费者） + `TickScheduler` 帧循环 + phys-tools `ecs-trace` / `ecs-check` / selftest `ecs` 节 + ctest 三条。**实测锚（真实 TSV 反推）**：240 F 行 / 41 E 行，published=consumed=41，kinds 6/29/6，f44/f46/f185，mappedA=0 / mappedB=41 / unmappedA=41 / unmappedB=0（A=blocker 未映射、B=probe 映射，owner 过滤双向有真实数据），updates=240、dropped=0、order_ok=240/240。**口径精确化**：§5「次帧清」指已消费事件；未读事件照 §2.1 两帧保活，表述已改（§9.1 归档）。M-D2 载具 / M-D3 驾驶可见未提前落码。

## 7. 开工待确认（Q 编号，决策前置）

> **状态（2026-11-10 用户「开工」）**：Q1 / Q2 按本表建议口径执行（未另行指定即视为采纳建议），Q3 = 完整 M-D0 落码收口，Q4 编号维持 `track drive / M-D0`。Q1 的「bevy 双缓冲 + M-GL5 TickGroup」落到 **M-D1**，本轮 M-D0 只产 POD 流与帧末排序；Q2 的清理时机已写进 `Events.hpp` 头注释与 §3 锚点表。
>
> **状态（2026-11-10 用户「下一步」）**：M-D1 已收官（tag `drive-m1`），Q1 的「bevy 双缓冲 + M-GL5 TickGroup」在 M-D1 落地并自证；M-D2 / M-D3 未开工。上一段为 M-D0 收官时的历史快照。

| 编号 | 问题 | 建议 / 拿到答案改哪里 |
|---|---|---|
| **Q1** | 三处消费口径不一致（UE 立即委托 / Carrot 帧末主线程队列 / bevy 双缓冲两帧保活）：是否维持既定「bevy 双缓冲 + M-GL5 TickGroup Physics→PostPhysics」，Carrot 只作同构佐证？ | **建议维持**，不扩范围。确认后 §2.3 标「已拍板」，M-D1 按此落码 |
| **Q2** | `OnContactRemoved` 期间 Jolt body 可能已销毁（`:126-128`）。owner 对改为 Added / Persisted 阶段缓存 + 本轨自持 `SubShapeIDPair → owner 对` 表；该缓存表清理时机是否定为「removed 派发后」？ | **建议 removed 派发后清**，避免 owner 查不到。确认后写进 §3 锚点表与 `Events.hpp` 头注释 |
| **Q3** | 本轮交付边界：停在「开轨 + 计划做实」，还是继续 M-D0 落码（Events.hpp / World.cpp listener / phys-tools contacts / trace→check / 展示台 tool 卡 / 双后端 ctest / example / tag / close 闸）？ | **已执行**：用户「开工」= 完整 M-D0 落码收口 |
| **Q4** | 归属编号：本对话按 `track drive` / `M-D0` 登记（四里程碑中唯一可开工起点）。 | 若要改编号，同步改 claim 锁与本文状态行 |

