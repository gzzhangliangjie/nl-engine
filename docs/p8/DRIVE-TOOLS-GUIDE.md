# DRIVE-TOOLS-GUIDE — 驾驶能力轴工具归档（M-D0：碰撞事件桥）

> 轨道：`feat/drive`（A 类并行轨道，worktree `E:/UE_STUDY_2026/nl-engine-drive`）。
> 计划：`docs/p8/P8-DRIVE-PLAN.md`（M-D0~M-D3）。本文件覆盖 **M-D0**，后续里程碑追加章节。
> 性质：**工具版交代**——不进 `kStage`、不进 `bytes/near` 渲染回归总闸；证据来自 `phys-tools.exe` 的真实输出。

## 1. 定位与边界

| 项 | 口径 |
|---|---|
| 交付 | `core/include/nl/physics/Events.hpp`（Jolt-free POD）+ `World::simulateContactProbe`（`World.cpp` 内 `ContactCollector`）+ phys-tools `contact-trace` / `contact-check` / selftest `contacts` 节。 |
| 层级纪律 | `nl/physics` 头文件零 Jolt 类型；`ContactListener` 及其三回调只出现在 `core/src/physics/World.cpp`（core 内唯一 Jolt 符号面）；`core/` 零图形 API；phys-tools 只链 `nl_core`。 |
| 参考口径 | Carrot `PhysicsSystem.h:73/.cpp:124,345` 单全局 listener + `PhysicsBody.cpp:68/87-94` 帧内入队→帧末主线程派发（**只借时序**）；bevy `event/mod.rs:573,716` 双缓冲两帧保活（**M-D1 才落消费者**）；UE `PrimitiveComponent.h:274` Hit 委托字段子集（**不借立即派发时机**）。三处口径差异与 Q1 拍板见计划 §2.3 / §7。 |
| 硬约束 | vendored `ContactListener.h:123-128`：`OnContactRemoved` 期间 body 可能已销毁，**禁止任何 body 访问**；owner 对、接触点、法线一律在 Added / Persisted 阶段缓存，removed 只读缓存，派发后清（Q2）。 |
| 确定性 | M-P0 契约不变：`JobSystemSingleThreaded` + 固定 1/60 + fresh world；每帧末按 `(minBodyId, maxBodyId, kind)` 稳定排序（池模式回调线程不定时兜底逐位）。两次新建世界事件流逐字段位一致。 |

## 2. 事件 POD 与 TSV 格式

`ContactEvent`（`Events.hpp`，Jolt-free）：

| 字段 | 含义 |
|---|---|
| `frame` | 事件所属物理帧（1..frames） |
| `kind` | 0=Added / 1=Persisted / 2=Removed（冻结为 TSV wire 格式） |
| `ownerA/ownerB` | 引擎侧 u64 id，来自 `Body::GetUserData()` → owner 表；Jolt 保证 A 为低 BodyID 侧 |
| `bodyIdA/bodyIdB` | 原始 `JPH::BodyID` 值（非 Jolt 类型） |
| `worldPos` | 接触点（`ContactManifold::GetWorldSpaceContactPointOn1(0)`） |
| `normalY` | `mWorldSpaceNormal.y`（方向 = 把 body2 推出碰撞，未翻转） |

TSV：`#` 行是 provenance（jolt / job_system / dt / gravity / radius / start_height / blocker_y / restitution / frames / bodies / events / cache_misses）；`E` 行 11 列（tag + 10 字段）；`T` 行 5 列（frame / t / y / vy，探针轨迹）。`contact-check` **只信 E/T 数据行**，摘要行仅作 provenance 与交叉一致性校验。

**缓存 key 口径**：`SubShapeIDPair` 的 **body id 部分**（min/max 归一）作 `unordered_map` key，不取 subshape id —— `ContactListener.h:133-134` 明示 removed 时 subshape id 来自上一帧、shape 变化后可能失效。body id 直接来自回调参数，removed 期间零 Jolt body 访问。

## 3. 子命令与退出码

| 命令 | 行为 | 退出码 |
|---|---|---|
| `contact-trace [--out <tsv>] [--frames N]` | 三体世界（floor / static blocker / dropped probe）跑 N 帧，输出事件流 + 轨迹 TSV | 0 / 1（运行失败）/ 2（参数错） |
| `contact-check <tsv>` | 从 E/T 数据行独立复验结构锚 + 实测数值锚 | 0 / 1 / 2 |
| `selftest [--verbose]` | M-P0~M-P4 + M-J5c 既有节 + **新增 contacts 节**（负向配置 / 三类事件 / owner 解析 / 确定性 / 实测锚） | 0 / 1 |

退出码语义对齐 `PHYS-TOOLS-GUIDE.md` §2 与展示台 `expectExit`：0=PASS / 1=断言或运行失败 / 2=用法错误。

负向实测：`contact-trace --frames 0` → 2（`invalid arguments: bad --frames`）；`contact-check <不存在>` → 1；未知子命令 → 2。

## 4. M-D0 实测锚（`contact-trace --frames 240` 真实输出反推，禁手写）

| 量 | 实测 | 判定位置 |
|---|---|---|
| 事件总数 | **41** | selftest contacts / contact-check |
| kind 分布 | Added **6** / Persisted **29** / Removed **6** | 同上 |
| 首个 Added | **f44**（probe 落到 blocker 顶） | 同上 |
| 首个 Removed | **f46** | 同上 |
| 末事件 | **f185**（Removed，睡眠岛） | 同上 |
| 静息尾段 | f186..f240 **零事件**，末 30 帧 quiet | contact-check |
| 接触点 / 法线 | worldPos=(0, **3.0**, 0)（`blockerY + radius`）、normalY=**1.0** | contact-check |
| 探针静息高度 | **3.500000**（blocker 顶 3.0 + 球半径 0.5） | contact-check |
| Q2 缓存 | `cache_misses=0`，6 个 Removed **全部携带非零 owner 对** | selftest / check / provenance |
| 事件时序 | 6 次 Added→Persisted→Removed 循环（f44/96/126/144/155/161 Added；f46/98/127/145/160/185 Removed） | TSV 事件时间线证据图 |

锚由一次性脚本读 `build/DX12/phys-d0-contacts.tsv` 生成后注入 C++，非手写。`contact-check` 对非标准 `--frames` 只跑结构锚、跳过数值锚（事件序列是求解器产物，无法从 provenance 反算）。

## 5. 交代物与一键复现

```
example\drive-m0\run.bat     # 前置：scripts\build-m0.2.bat DX12 / VULKAN
```

五段式：① `selftest --verbose`（7 条 PASS 锚 findstr 断言）→ ② `contact-trace` 重生成 `contacts.tsv` 并 `fc /b` 逐字节自稳 + `contact-check` → ③ DX12/VULKAN trace 逐字节一致（VULKAN 树存在时）→ ④ 负向退出码 2/1 → ⑤ 两张证据图重生成 + `fc /b` 逐字节比对基线 → `ALL GREEN` exit 0。

| 文件 | 来源 |
|---|---|
| `example/drive-m0/contacts.tsv` | `contact-trace --frames 240` 真实输出 |
| `{selftest,contact-trace,contact-check,cross-backend,usage,negative}.log` | run.bat 捕获的真实工具输出 |
| `events.png` | `tools/make-drive-m0-evidence.py` 从 TSV 渲染：事件时间线三行 + f44/f46/f185 标注 + y(t) 曲线 + 接触点标记 + MEASURED 面板 |
| `acceptance.png` | 同一脚本从真实 logs 渲染的终端风格验收板 |

两张图均为固定画布 / 配色 / 字体、无时间戳，重生成后 `fc /b` 逐字节一致；**禁手绘示意**。

展示台：`example/cases.json` `drive-m0` 版本卡 5 用例（usage / selftest / contact-trace / contact-check / negative），`python tools/showcase/server.py --check` exit 0。

## 6. ctest

```
d0-contact-trace      contact-trace --frames 240 --out <build>/phys-d0-contacts.tsv
d0-contact-check      contact-check <上述 TSV>（DEPENDS trace）
d0-contact-negative   contact-trace --frames 0（WILL_FAIL）
```

命令统一带 `--repeat until-pass:3`（便携 cmake 3.31.6 快进程 stdout 捕获竞态，见 `AGENTS.md` 4b）；裸 shell 无 ctest，走全路径 `E:/UE_STUDY_2026/.build-tools/cmake-3.31.6-windows-x86_64/bin/ctest.exe`。

## 7. 踩坑

1. **junction 共享文件挡 merge**：`.workbuddy` 为主树/wt junction 共享 + skip-worktree，主树新日志 commit 使 wt 工作区 blob 领先 index → `git merge master` 报「local changes would be overwritten」。按工作线粘性不在中途 merge，收尾 merge 回 master 自动带上。
2. **缓存 key 不能带 subshape id**：见 §2；按全 pair 哈希做键会在 compound shape 场景静默丢 owner，实测 6/6 removed 命中才算过关。
3. **锚机器反推**：先只写结构锚，实跑后由脚本从真实 TSV 生成 7 条数值锚；避免 M-I0/I1 手写锚两连犯。
4. **selftest 输出演进**：新增 `phys-d0 contacts: PASS` 一行，`phys-p0 selftest: PASS` 等既有锚文本一字未动；M-P0~P4 归档 TSV 基线零改动，phys-* 归档 log 是历史快照不重拍。
5. **行尾**：本树源码 / 文档 / bat 全 CRLF，cases.json 全 LF；Write 工具产出 LF，落盘前按目标文件换行规范转换，写完当场 `git diff --numstat` 核对无 churn。

## 8. 下一步（不属 M-D0）

M-D1：`ContactEventStream` 双缓冲 + owner→Entity 映射 + PostPhysics 消费 System + M-GL5 TickGroup 时序自证（Q1 的落地处）；M-D2：载具无头集成 + 轮接触事件锚；M-D3：viewer 驾驶训练场（本轨唯一渲染接线面）。
**M-D1 已于 2026-11-10 收官，见下方 §9；本节是 M-D0 当时的排期快照。**
## 9. M-D1：ECS 消费契约（2026-11-10 收官，tag `drive-m1`）

### 9.1 定位与边界

| 项 | 口径 |
|---|---|
| 交付 | `core/include/nl/physics/ContactEventStream.hpp`（双缓冲，Jolt-free）+ `ContactEventSystem.hpp`（`scene::System` 消费者）+ 两个 .cpp + `phys-tools` 扩 `ecs-trace` / `ecs-check` / selftest `ecs` 节 + ctest 三条。 |
| 层级纪律 | `nl/physics` 头文件零 Jolt 类型；消费者是 physics→ECS 桥，依赖 `nl/scene/System.hpp` 而非反向；`core/` 零图形 API；零 `kStage`。 |
| 三处实读（M-D1 开工重读，非复用 M-D0 接触事件面） | Carrot `PhysicsBody.cpp:63-94`（帧内入队→帧末主线程派发并 clear）+ `CSharpBindings.cpp:1033-1075`（`mainThreadOnly=true` 消费者，两侧 `entity.exists()` 过滤）；bevy `event/mod.rs:573/680-701/704-719`（`update()` 换页 / `update_drain()` / 两次 update 才 empty）；UE `EngineBaseTypes.h:83-110`（`TG_PostPhysics` 组序语义）。UE 立即委托派发时机明确不借（§2.3 / Q1）。 |
| 口径精确化 | 计划 §5 原写「次帧清」，与 §2.1 bevy 两帧保活、§3「读上一逻辑帧事件」措辞冲突。收口按更细的 §2.1 + §3 执行并改写 §5：**已消费事件次帧不可见；未读事件存活到次帧、第三次 `update()` 才丢**。二者同时成立，不是二选一。 |

### 9.2 双缓冲契约

| 调用 | 时机 | 语义 |
|---|---|---|
| `update()` | Physics 组帧首，publish 之前 | 丢最老页的未读事件（计数供两帧保活自证），把它回收为新写页；上一帧写页变为可读旧页 |
| `publish(e)` / `publish(vector)` | Physics 组，Jolt step 之后 | 追加到当前写页 |
| `drain()` | PostPhysics 组，消费者 `tick` 内 | 按「旧页→当前页」时间序取出全部未读事件并推进每页 cursor；同帧第二次 drain 返回空 |
| `readableCount()` / `pendingCount()` / `*Total()` | 诊断 / TSV provenance | 只读，无隐藏状态 |

消费者 `ContactEventSystem` 直接继承 `scene::System`（不经过 `SignedSystem<>`）→ 读写集为空 = 自足系统，`World::tickParallel` 契约 2 允许与其它系统并行；`tick` 只调 `World::exists()`（const 名字表查询）与 `stream_.drain()`，**不做结构变更**。`ownerA/B`（u64，来自 `BodyInfo.owner`）直接当 `scene::EntityID`，零新增映射表；未映射 owner 只累加 `unmapped*`，事件仍留在 `lastEvents()` 里，不丢。

### 9.3 子命令与退出码

| 命令 | 行为 | 退出码 |
|---|---|---|
| `ecs-trace [--out <tsv>] [--frames N]` | 跑 M-D0 三体探针取真实事件 → 逐帧 `TickScheduler::runTick`（Physics 组 publish / PostPhysics 组 ecs.tick / PostUpdateWork 组断言组序）→ 输出 F/E TSV | 0 / 1（运行失败）/ 2（参数错） |
| `ecs-check <tsv>` | 只从 F/E 数据行独立复验结构锚 + 实测数值锚 | 0 / 1 / 2 |
| `selftest [--verbose]` | 既有节 + **新增 ecs 节**（双缓冲单元语义 / 组序 / owner 过滤 / `tickParallel` 零违约 / 真实流 41 事件） | 0 / 1 |

TSV：`#` provenance；`F` 行 11 列（frame / published / consumed / mappedA / mappedB / unmappedA / unmappedB / readable_before / readable_after / order_ok）；`E` 行 9 列（frame / kind / ownerA / ownerB / bodyIdA / bodyIdB / mappedA / mappedB）。`ecs-check` 交叉校验「E 行 mapped 汇总 == F 行 System 计数」，防两份计算各自假绿。

退出码语义同 §3：0=PASS / 1=断言或运行失败 / 2=用法错误。负向实测：`ecs-trace --frames 0` → 2；`ecs-check <不存在>` → 1。

### 9.4 M-D1 实测锚（`ecs-trace --frames 240` 真实输出反推，禁手写）

| 量 | 实测 | 判定位置 |
|---|---|---|
| F / E 行数 | **240** / **41** | selftest ecs / ecs-check |
| published / drained | **41** / **41**（每帧 published==consumed） | 同上 |
| kind 分布 | Added **6** / Persisted **29** / Removed **6** | 同上 |
| 首个 Added / 首个 Removed / 末事件 | **f44** / **f46** / **f185** | 同上 |
| 有事件的帧 | **41**（每帧恰好 1 事件） | ecs-check |
| owner 过滤 | mappedA **0** / mappedB **41** / unmappedA **41** / unmappedB **0** | selftest / check |
| 换页 / 丢弃 / 组序 | updates **240** / dropped **0** / order_ok **240/240** | TSV provenance / F 行 |
| 次帧清 | readable_before / readable_after 全帧恒 **0** | ecs-check |

**owner 方向说明**：M-D0 事件 A 侧恒为 `blockerOwner`（…842）、B 侧恒为 `probeOwner`（…841）。`ecs-trace` 只给 probe 建实体，故 A 侧 41 个全未映射、B 侧 41 个全映射——「未映射原样透传」有真实数据，不是全命中假绿。

### 9.5 交代物与一键复现

```
example\drive-m1\run.bat     # 前置：scripts\build-m0.2.bat DX12 / VULKAN
```

五段式：① `selftest --verbose`（8 条 PASS 锚 findstr 断言，含 `phys-d1 ecs: PASS`）→ ② `ecs-trace` 重生成 `ecs.tsv` 并 `fc /b` 逐字节自稳 + `ecs-check` → ③ DX12/VULKAN trace 逐字节一致 → ④ 负向退出码 2/1 → ⑤ 两张证据图重生成 + `fc /b` 比对 → `DRIVE-M1 ACCEPTANCE: ALL GREEN` exit 0。首次运行会先生成 PNG 基线并在第 ⑤ 段报一次「找不到 .base.png」（与 M-D0 同款），第二次起稳定全绿。

| 文件 | 来源 |
|---|---|
| `example/drive-m1/ecs.tsv` | `ecs-trace --frames 240` 真实输出 |
| `{selftest,ecs-trace,ecs-check,cross-backend,usage,negative}.log` | run.bat 捕获的真实工具输出 |
| `timeline.png` | `tools/make-drive-m1-evidence.py` 从 TSV 渲染：消费事件时间线 + 累计 publish/drain 曲线 + 组序条带 + MEASURED 面板 |
| `acceptance.png` | 同一脚本从真实 logs 渲染的终端风格验收板 |

展示台：`example/cases.json` `drive-m1` 版本卡 5 用例（usage / selftest / ecs-trace / ecs-check / negative），`python tools/showcase/server.py --check` exit 0。

### 9.6 ctest

```
d1-ecs-trace      ecs-trace --frames 240 --out <build>/phys-d1-ecs.tsv
d1-ecs-check      ecs-check <上述 TSV>（DEPENDS trace）
d1-ecs-negative   ecs-trace --frames 0（WILL_FAIL）
```

命令统一带 `--repeat until-pass:3`（见 §6）；裸 shell 无 ctest，走全路径 `E:/UE_STUDY_2026/.build-tools/cmake-3.31.6-windows-x86_64/bin/ctest.exe`。双后端各 7/7（d1 三条 + d0 三条 + p0 selftest）。

### 9.7 踩坑

1. **「次帧清」不是「所有事件次帧清」**：计划 §5 简写与 §2.1 两帧保活表面冲突。实现用逐页 cursor 同时满足两者——drain 推进 cursor 使已消费事件次帧不可见，未读事件留到第三次 `update()` 才丢；selftest 两条分别断言。收口时把 §5 措辞改精确，避免下轮又捡回旧表述。
2. **证据图不能含手绘/占位数据**：acceptance 板每一节都必须读真实 log 文件；曾想直接写死「ALL GREEN」两行，改为从 `ecs-check.log` / `cross-backend.log` 取真实行。
3. **排版压字**：`timeline.png` 首版 f44 标注贴住面板标题、累计曲线平台与说明文字挤在一起 → 标注下移、曲线 scale 165→110、说明文字上移；`acceptance.png` 画布 660→600 收掉下方留白。修完重生成并复跑 run.bat `fc /b` 逐字节一致。
4. **首次 run.bat 必挂一次**：PNG 基线不存在时 `copy /b` 造不出 `.base.png`，第 ⑤ 段 fc 报 ERROR；第二次起 ALL GREEN（M-D0 同款，属预期冷启动行为）。
5. **selftest 变量遮蔽**：新增 ecs 节的局部 `st2` 遮蔽了既有 stack 节的 `st2`（/W4 C4456）→ 改名 `ecsStream2`。文件里其余 4 条同类 warning 是 M-D0 之前既有，未动。

## 10. M-D2：无头载具集成（2026-11-10 收官，tag `drive-m2`）

### 10.1 定位与边界

用户拍板 Q-D2-1 = **方案 A 探针式 fresh world**（三处证据 + A/B/C 代价矩阵见 `docs/p8/D2-VEHICLE-OWNERSHIP-OPTIONS.md`）：`Vehicle.hpp` 公开面全是 POD、零 Jolt 类型；每次 `simulateDrive(cfg)` 自建 `JPH::PhysicsSystem`、跑完帧序号输入脚本后只返回 POD 视图。不引入长生命周期 `Vehicle` 对象、不提前定 M-D3 挂载契约；`World.hpp:226-228`「fresh world per call ⇒ 逐位一致」不变量保持。零图形、零 `kStage`、不碰 `World.cpp` / `viewer` / `DebugScene`。

三处本里程碑点名面实读（**未复用 M-D0/D1 证据**，AGENTS.md M5.1 教训）：Carrot `engine/engine/physics/` 16 文件零 `Vehicle` / `StepListener` / `Wheeled` 命中；bevy `crates/` 65 官方 crate 无官方物理，唯一 vehicle 命中是 `bevy_ecs/src/component/mod.rs:52` 教学 Component 示例（如实记「无对应物」）；UE 5.8.4 ChaosVehicles 四输入 API（`ChaosVehicleMovementComponent.h:887/899/903/919`）+ `FWheelStatus[]` 逐帧缓存回读（`:716/:955`），只借「输入分开 + 状态按索引回读」思路。另逐行核 vendored Jolt `Physics/Vehicle/` 24 文件与 `PhysicsSystem.h:128/151/162`（vendored 不含 Samples / VehicleTest，装配序列只能从头文件推导）。

### 10.2 装配契约

| 顺序 | 调用 | 依据 / 踩坑 |
|---|---|---|
| 1 | `PhysicsSystem::Init` + `SetGravity` + 4 层 / 1:1 broadphase filter（本 TU 内，不碰 `World.cpp`） | 对象层对过滤**必须全 `true`**：M-P0 禁用 `floor/floor` 会让 `VehicleCollisionTesterRay` 用 floor 层 cast floor body 全 miss → `events=0`、车身坐地 |
| 2 | static floor（`BoxShape` 半高 1、中心 y=-1，顶面 y=0）+ dynamic chassis（`mDensity = mass / volume`） | Jolt 由密度推质量 |
| 3 | `VehicleConstraintSettings`：`mUp=(0,1,0)`、`mForward=(0,0,1)`、`mMaxPitchRollAngle=60°` | `VehicleConstraint.cpp:87-88` 断言归一化 |
| 4 | `WheeledVehicleControllerSettings` + `VehicleDifferentialSettings{mLeftWheel=2, mRightWheel=3, mEngineTorqueRatio=1}` | 差速器轮索引默认 **-1**，不显式填则动力不传轮 |
| 5 | 4 个 `WheelSettingsWV`（不是基类 `WheelSettings`）：位置 / 悬挂方向 / `mSuspensionMin,MaxLength` / `mRadius` / `mWidth` / 前轴 `mMaxSteerAngle` | `WheeledVehicleController.h:160` `ConstructWheel` 断言 `IsKindOf(WheelSettingsWV)` |
| 6 | `new VehicleConstraint(body, settings)` → `SetVehicleCollisionTester(new VehicleCollisionTesterRay(kFloor))` | `VehicleConstraint.cpp:219/232` 对 tester 无空检查直接解引用 |
| 7 | `AddConstraint` → `AddStepListener` → `OptimizeBroadPhase` | `VehicleConstraint.h:44` 明确要求 StepListener；`PhysicsSystem.cpp:649` 是唯一 `OnStep` 驱动入口 |
| 8 | 每帧 `static_cast<WheeledVehicleController*>(constraint->GetController())->SetDriverInput(f,r,b,hb)` → `Update(dt,1,...)` | `GetController()` 返回**基类**指针；手刹是 float 0~1，不是 UE 的 bool |
| 9 | 读 `HasContact()` / `GetSuspensionLength()` / `GetRotationAngle()` / `GetSteerAngle()` | 接触类 getter 必须先 `HasContact()`（`Wheel.h:85-97` JPH_ASSERT） |
| 10 | `RemoveStepListener` → `RemoveConstraint` 再析构 | StepListener 在 Jolt 内是裸指针 |

**事件桥（重要）**：Jolt 轮是 raycast query、**不是刚体**，天然不触发 `ContactListener`。M-D2 的「轮-地接触事件」由逐轮 `HasContact()` 边沿合成 `ContactEvent` POD：`false→true` Added / `true→true` Persisted / `true→false` Removed；`bodyIdA = kDriveWheelSlotBase + wheelIndex`（稳定 synthetic id）、`bodyIdB = floorId`、`worldPos` 留零、`normalY=1`。事件在同一逻辑帧走 M-D1 `stream.update() → publish() → drain()`。M-D0 的 `ContactCollector` / `simulateContactProbe` 一字未动。

### 10.3 子命令、TSV 与退出码

| 子命令 | 作用 |
|---|---|
| `drive-trace [--out <tsv>] [--frames N] [--accel-end N] [--turn-end N]` | 跑 fresh world 480 帧输出 TSV；不传 `--out` 走 stdout |
| `drive-check <tsv>` | 只信 F/W/E 数据行独立复验；`#` 头仅作 provenance 与行数交叉校验 |
| `selftest --verbose` | 新增 `vehicle` 节：负向 config 六例 / 两次 fresh world 逐位一致 / 四轮射线接触 / 流每帧 publish==consume / 加速·转向·手刹三类行为 |

TSV 列（tab 分隔）：

```
F frame phase t forward right brake handbrake px py pz vx vy vz yaw speed grounded published consumed dropped   (20 列，481 行)
W frame wheel contact suspensionLength rotationAngle steerAngle                                                (7 列，1924 行)
E frame kind ownerA ownerB bodyIdA bodyIdB normalY                                                              (8 列，1920 行)
```

`drive-check` 共 25 条结构 / 物理断言 + 6 组实测数值锚：行序与时间戳、phase 与输入脚本、`speed == hypot(vx,vz)`、每帧 `published==consumed==grounded` 且 `dropped==0`、底盘 `py>0.5`（没坐地）、每帧 4 个 W 行且 contact 求和等于 F、前轴转向 / 后轴恒 0、悬挂长度在范围内、E 行 owner / synthetic id / normal / 帧序 / 每帧条数、加速到 >5 m/s、转向 |Δyaw|>0.3 且半径 5~60 m、手刹减速且窗口内停住、四轮接触覆盖 >3/4 帧。

退出码：`0` PASS / `1` 断言或运行失败 / `2` 用法错误。

### 10.4 M-D2 实测锚（`drive-trace --frames 480` 真实输出反推，禁手写）

全部由 `python tools/make-drive-m2-evidence.py --anchors` 从归档 `drive.tsv` 的 F/W/E 行重算，再注入 `drive-check`；非 480 帧只跑结构 / 物理断言，跳过数值锚。

| 量 | 实测 | 判定位置 |
|---|---|---|
| F / W / E 行数 | **481** / **1924** / **1920** | drive-check / 证据图 |
| kinds | Added **4** / Persisted **1916** / Removed **0** | drive-check |
| 首个接触 / 末事件 | **f1** / **f480** | drive-check |
| 全接地帧 / published | **480** / **1920**（consumed 同、dropped 0） | drive-check |
| 加速 | reach 1 m/s **f25** / top **9.298565** / 出段 f150 **6.261256** m/s | drive-check |
| 转向 | yaw **-0.838077** rad / 位移 15.091 m / 半径 **18.007170** m / 前轮 0.5236 rad 后轮 0 | drive-check / W 行 |
| 手刹 | f270 **9.298565** → f480 **0.001434** m/s / 刹停 **f402** | drive-check |
| 双后端 | DX12/VULKAN TSV 逐字节一致 **269658 B** | run.bat 第 ③ 段 |
| 零回归 | M-D0 `contacts.tsv` / M-D1 `ecs.tsv` 双后端逐字节不变 | 本轮对拍 |

`Removed=0` 是真实物理：四轮射线全程贴地、没有任何离地边沿，不是漏采事件。

### 10.5 交代物与一键复现

```
example\drive-m2\run.bat     # 前置：scripts\build-m0.2.bat DX12 / VULKAN
```

五段式：① `selftest --verbose`（9 条 PASS 锚 findstr 断言，含 `phys-d2 vehicle: PASS`）→ ② `drive-trace --frames 480` 重生成 `drive.tsv` 并 `fc /b` 逐字节自稳 + `drive-check` → ③ DX12/VULKAN trace 逐字节一致 → ④ 负向退出码 2/1 → ⑤ 两张证据图重生成 + `fc /b` 比对 → `DRIVE-M2 ACCEPTANCE: ALL GREEN` exit 0。**M-D0/D1 的「首次必挂一次」冷启动问题在 M-D2 已修**：第 ⑤ 段检测 `timeline.png` 不存在时先生成基线、跳过比对并落 `[INFO]` 到 `evidence.log`，首次即全绿。

| 文件 | 来源 |
|---|---|
| `example/drive-m2/drive.tsv` | `drive-trace --frames 480` 真实输出（269658 B） |
| `{selftest,drive-trace,drive-check,cross-backend,usage,negative,evidence}.log` | run.bat 捕获的真实工具输出 |
| `timeline.png` | `tools/make-drive-m2-evidence.py` 从 TSV 渲染：速度曲线 + 三段相位色带 + yaw/四轮接触条带 + 累计 publish/drain + MEASURED 面板 |
| `acceptance.png` | 同一脚本从真实 logs 渲染的终端风格验收板 |

展示台：`example/cases.json` `drive-m2` 版本卡 5 用例（usage / selftest / drive-trace / drive-check / negative），`python tools/showcase/server.py --check` exit 0。

### 10.6 ctest

```
d2-drive-trace      drive-trace --frames 480 --out <build>/phys-d2-drive.tsv
d2-drive-check      drive-check <上述 TSV>（DEPENDS trace）
d2-drive-negative   drive-trace --frames 0（WILL_FAIL）
```

双后端各 **10/10**（d2 三条 + d1 三条 + d0 三条 + p0 selftest），命令统一带 `--repeat until-pass:3`；裸 shell 无 ctest，走全路径便携 cmake 3.31.6 的 ctest.exe。

### 10.7 踩坑

1. **对象层对过滤不能沿用 M-P0**：`ObjectLayerPairFilterImpl::ShouldCollide` 初版照抄 M-P0 禁用 `floor/floor`，导致 `VehicleCollisionTesterRay` 用 floor 层去 cast floor body 全部 miss → 首跑 `events=0`、四轮 `contact=0`、车身坐地。改成所有层对 `return true`（只在本 TU 内，`World.cpp` 保持逐字节不变）后 `events=1440`（360 帧口径）。
2. **`--frames` 三段等比缩放是陷阱**：初版按 `N*5/12` / `N*3/4` 同时缩放三段，实测 N=360/480/600/720 的 `stop_frame` **恒 -1** —— 加速段一起变长，车速涨得比 1/4 长度的手刹窗口快。改为「`accelEnd=150` / `turnEnd=270` 固定，额外帧只延长 phase 2」+ 新增 `--accel-end` / `--turn-end`，默认 `frames=480`，实测 f402 停住、末速 0.001434 m/s。
3. **差速器默认 -1**：`VehicleDifferentialSettings::mLeftWheel/mRightWheel` 默认 -1，不显式填轮索引 = 动力不传轮、车不动。
4. **轮接触没有 ContactListener 事件**：立项口径写「天然产 added/persisted 事件流」不成立；Jolt 轮是 raycast query。改由 `HasContact()` 边沿合成，并把计划 §3 衔接段 / 锚点表 / `Vehicle.hpp` 头注释全部同步更正（避免下轮捡回旧表述）。
5. **selftest 新节变量遮蔽**：初版 `r1`/`r2` 触发 /W4 C4456 → 改名 `veh1`/`veh2`，warning 回落到既有 4 条。
6. **证据图不能读正在写的日志**：`acceptance.png` 若读 `evidence.log`（python 自己的 stdout 正被重定向到该文件），冷启动与二次运行会产出不同 PNG 导致 `fc /b` 挂。改成只读第 ④ 段前已写完的 `drive-check.log` / `cross-backend.log`。
7. **锚一律机器反推**：先只写结构 / 物理断言 + 一条故意失败的占位（防忘注入），实跑后由 `--anchors` 从真实 TSV 读出 6 组数值注入 `drive-check`（M-I0/I1 手写锚两连犯教训）。
