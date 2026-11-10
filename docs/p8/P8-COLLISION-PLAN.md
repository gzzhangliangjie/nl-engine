# P8-COLLISION-PLAN — 碰撞事件桥：Jolt ContactListener → ECS 事件（A 类并行轨道，2026-11-10 立项）

> 起因：城市驾驶开发顺序第 2 步（`docs/GAME-DEV-ORDER.md` §2）。review 实证（GAME-DEV-ORDER §5 留痕）：
> core 全目录 grep 零 `ContactListener` / trigger 回调——物理世界（M-P0~P4）只会「算」不会「说」，
> 车撞障碍、轮子蹭地、箱子倒下，玩法逻辑层完全感知不到。写载具（第 3 步）与交通玩法（第 8~9 步）前必须先补这个感知面。
> **性质**：A 类并行轨道（不碰 `kStage`、不进渲染回归），走工具版交代（phys-tools selftest / trace→check / 展示台 tool 卡）。
> **归属**：track `collide`，里程碑 M-CB0~M-CB1（M-C 前缀已被 console 轨 M-C0~C3 占用，避撞取 CB）；tag 名字制（`collide-m0` 起），不占 m0.* 编号。

## 1. 目标

给物理世界接上**事件出口**，分两段、零渲染依赖：

| 段 | 内容 | 落点 |
|---|---|---|
| **事件流（M-CB0）** | Jolt `ContactListener` 三回调（added / persisted / removed）接进 `nl/physics`，事件以 Jolt-free POD 流出，per-frame 可 dump TSV | `core/include/nl/physics/Events.hpp`（新）+ `World.cpp`（唯一 Jolt 符号面内实现 listener） |
| **ECS 消费（M-CB1）** | owner→EntityID 映射 + 事件队列消费 System + TickGroup 时序契约（物理 step 在 Physics 组、消费 System 在 PostPhysics 组） | `core/src/physics/` 队列 + `scene::System` demo 消费者，selftest 验消费节奏 |

**关键设计约束**（历史约定对齐）：
- `nl/physics` 头文件零 Jolt 类型纪律不变（PIMPL / POD 视图，照 `World.hpp` BodyInfo 口径）。
- 确定性契约照 M-P0：JobSystemSingleThreaded 基线逐位；M-J5c 池模式回调线程不定 → 事件帧末**稳定排序**（key = body 对 id + kind）兜底逐位。
- `BodyInfo.owner`（M-P1 已落，`World.hpp:81`，u64 引擎侧 id ↔ BodyID↔userData 双向映射）就是桥的既有地基——事件对直接携带 owner 对，ECS 侧零新增映射结构。
- M-GL5 TickGroup 注册制是时序契约的天然承载面（Physics 组 step → PostPhysics 组消费），不另造调度。

## 2. 三处参考口径（立项预口径；M-CB0 开工首日按铁律实读点名文件后修订留痕）

| 来源 | 预口径 | 用法 |
|---|---|---|
| **Carrot** `engine/physics/` + 事件系统（`Carrot::Event` 通道） | Jolt PhysicsWorld 是否有接触事件面 / trigger 语义，事件走 Async 还是 PassThrough 通道 | 桥的节奏参照；无接触事件面则据实标注（bevy 口径同款处理） |
| **bevy** `crates/bevy_ecs/src/event.rs`（Events<T> 双缓冲） | 事件双缓冲：当帧写、下一 schedule 段读、帧末换页；**bevy 无官方物理**——碰撞事件是社区 crate（avian / bevy_rapier），core 不背 | **消费节奏的锚**：双缓冲「当帧产、次帧读、两帧保活」直接照抄到 ContactEventStream；泛型 Events<T> 框架不做（见 §4） |
| **UE** `Components/PrimitiveComponent.h`（OnComponentHit / OnComponentBeginOverlap / OnComponentEndOverlap 委托） | 玩法面三件套 = Hit（真实碰撞）/ Begin+End Overlap（触发器）；委托挂组件、带 HitResult（位置/法线/相对速度） | **事件字段的锚**：ContactEvent POD 对齐 HitResult 有用子集（位置/法线 y/相对速度）；overlap 触发器语义不做（§4） |

## 3. 设计（逐里程碑细化，开工前补全锚点表）

### M-CB0 事件流

- `ContactEvent` POD（Jolt-free）：`{frame, kind(Added=0/Persisted=1/Removed=2), ownerA, ownerB, bodyIdA, bodyIdB, worldPos(xyz), normalY}`——对齐 UE HitResult 有用子集；removed 回调 Jolt 只给 SubShapeIDPair（`ContactListener.h:136`），owner 从既有映射表回查。
- `World` 新探针 `simulateContactProbe(ContactProbeConfig)`：照 `simulateLayerProbe`（M-P1）先例——floor + 静态 blocker + 掉落 probe 三体世界，逐帧事件流随轨迹一起 dump。listener 实现在 `World.cpp`（PIMPL 内），三回调原样转 POD。
- **确定性**：单线程 JobSystem 基线回调内联于 step（帧序即回调序）；池模式回调线程不定 → 帧末按 `(minBodyId,maxBodyId,kind)` 稳定排序后逐位。OnContactRemoved 的派发时机（Jolt 口径：解激活/移除路径，下次 Update 派发）开工实读 `ContactListener.h` + `PhysicsSystem` 确认后写死注释。
- phys-tools 扩 selftest `contacts` 节 + `contact-trace` / `contact-check` 子命令：掉落 → 首次接触帧（added）→ 静置段 persisted → 反弹段 removed→added 重触发 → 静息段无新 added；锚从实测 log 反推（禁手写锚，M-I0/I1 两连犯教训入册）。

### M-CB1 ECS 消费

- `ContactEventStream`：双缓冲队列（当帧写 / 次帧读 / 两帧保活，bevy 口径），`drain()` 语义固化。
- demo `scene::System` 消费者：tick 内读上一逻辑帧事件，owner→Entity 走 `scene::World::exists` 过滤（未映射的纯物理 owner 原样透传）。
- 时序契约自证：GameClock Physics 组跑 step → PostPhysics 组跑消费 System（M-GL5 注册制）——selftest 断言「step 同帧产的事件，消费 System 在同帧 PostPhysics 段可见、下一帧不可见」。

## 4. 不在范围

- 通用 `Events<T>` 事件框架（bevy 泛型 / UE GameplayMessageRouter）——具体类型具体队列，真有第二类事件再抽象。
- 触发器 / sensor 体积语义（Ghost 层 overlap 事件）——载具和城市玩法起步用真实碰撞够用，按需后置。
- `ContactSettings` 响应面修改（回调里改弹性/摩擦）——先只读。
- 多线程零锁化、Jolt `BodyActivationListener`（激活/休眠事件，按需后置）。
- viewer 渲染侧可视化（debugLines 画接触点）——纯逻辑轨道零图形；要看得见留给第 7 步最小可玩版。

## 5. 里程碑草案

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M-CB0** | `core/physics` 事件流（listener + ContactEvent POD + contactProbe）+ phys-tools `contacts` 节 + trace→check 闭环 + ctest | selftest 数值锚全绿；事件时序逐位（单线程基线 + 池模式排序兜底）；负向退出码 0/1/2；双后端 spot 零回归；tool 卡 |
| **M-CB1** | `ContactEventStream` 双缓冲 + demo System 消费者 + TickGroup 时序契约自证 + ECS selftest | 消费节奏锚全绿（当帧产、同帧 PostPhysics 可见、次帧清）；并行 tick 下消费 System 只读零违约；ctest 双后端 |

## 6. 留痕

- 2026-11-10：立项出计划（用户指令「碰撞事件桥和载具物理先出文档」）。vendored API 实证：`ContactListener.h:106/119/136`（三回调签名）、`World.hpp:79-85`（BodyInfo.owner 既有地基）、layers 表（World.hpp:60-65）。与载具轨道（`P8-VEHICLE-PLAN.md`）的文件重叠面 = 仅 CMakeLists.txt（phys-tools/main.cpp 各扩各的节，串行收口即可避）；本轨全程零 viewer 触碰。
