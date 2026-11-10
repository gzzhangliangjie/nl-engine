# M-D2 决策材料：载具集成层与 PhysicsSystem 的所有权形态（已拍板 A，决策依据快照）

> 2026-11-10 · track `drive` / M-D2 开工前决策前置功课。**拍板结果：方案 A 探针式 fresh world**（用户 2026-11-10），
> 结论已写回 `docs/p8/P8-DRIVE-PLAN.md` §7 Q-D2-1 + §3 M-D2 开工锚点表，历史挂账句已删，开工闸满足后方落码；
> **M-D2 已按 A 收官**（tag `drive-m2`，实测见 `DRIVE-TOOLS-GUIDE.md` §10）。
> 本文件保留为决策依据快照（§4/§5 的「建议 / 非结论」是拍板前的原始措辞，不再代表当前状态）。
> **2026-11-10 M-D3 开工闸勘误**：§4/§5/§6 里「M-D3 届时抽成 `buildVehicle(PhysicsSystem&)`」是
> **承诺而非 M-D2 已实现**；M-D2 收官时装配全内联在 `simulateDrive()`，函数由 M-D3 按 Q-D3-1 A 真抽出，
> 见 `D3-DRIVE-VIEW-OPTIONS.md` §2.1 与 `P8-DRIVE-PLAN.md` §3 M-D3 锚点表。
> 依据：`AGENTS.md`「决策前置铁律」+「开工闸」（主树第 67 / 128 条，2026-11-10 固化，M9.0 教训）。

## 1. 为什么必须在落码前定

`P8-DRIVE-PLAN.md` §3 只写「`Vehicle.hpp`（新，PIMPL）+ `Vehicle.cpp`」与「帧序号驱动输入脚本」，
**没有写 `Vehicle` 与 `JPH::PhysicsSystem` 的所有权关系**。而 vendored Jolt 强制两步注册：

- `VehicleConstraint.h:44` 注释明确 "Don't forget to register the constraint as a StepListener"；
  `:65` `VehicleConstraint : Constraint, PhysicsStepListener`。
- `PhysicsSystem.h:128` `AddConstraint(Constraint*)` + `:151` `AddStepListener(PhysicsStepListener*)`；
  `PhysicsSystem.cpp:649` 是 `OnStep` 唯一驱动入口；`VehicleConstraint.cpp:219/232` 直接解引用
  `mVehicleCollisionTester`（默认空、构造不赋值），不注册 = 轮子无接触、无悬挂、无驱动。
- 本项目 `World.cpp` **有多套独立 `PhysicsSystem` 实例**（`:261/400/679/803/970/1267`），
  `World::Impl`（`:154`）不持有 physicsSystem，探针一律 fresh world per call；
  `DebugScene::Impl`（`:1253`）才是长生命周期 world（build / step / extractLines）。
- M-D3 计划把车辆装进 `DebugScene` scenario 2 训练场。**M-D2 选错所有权形态，M-D3 就要返工**：
  车与墙不在同一个 `PhysicsSystem` 时，M-D1 的「车撞墙事件消费 demo」物理上不成立。

第一行装配代码（`AddConstraint` / `AddStepListener` 挂到哪个实例）就等于把这个判定隐式拍死，
按开工闸必须先摆方案交用户拍板。

## 2. 三处实读（本里程碑点名面，未复用 M-D0/D1 证据）

`docs/ROADMAP.md`「步骤 → 本地代码对照总表」**无 M-D2 行**（全表 grep 载具 / Vehicle / M-D2 零命中），
按 AGENTS.md「查不到对应行时按功能就近读同类模块」，本轮实读如下：

| 来源 | 文件 / 行号 | 结论 |
|---|---|---|
| Carrot | `engine/engine/physics/` 全目录 16 文件 | 对 `[Vv]ehicle / StepListener / Wheeled` **零命中**；只有通用 raycast（`PhysicsSystem.cpp:142-153`）与 SixDOFConstraint（`RigidBody.cpp:410-451`）。只能作「Jolt 生命周期接管」参照，**不可作载具装配参照** |
| bevy | `crates/` 65 官方 crate | 无 `VehicleController`、无 raycast vehicle、无官方物理；唯一命中是 `bevy_ecs/src/component/mod.rs:52` 的教学示例 `struct VehiclePerformance`。**据实标注无对应物** |
| UE 5.8.4（只读不抄） | `ChaosVehicleMovementComponent.h:887/899/903/919`；`ChaosWheeledVehicleMovementComponent.h:633`、`:78` `FWheelStatus`、`:538` `FWheelState`、`:716` `GetWheelState(int)`、`:955` `WheelStatus` 数组 | 四输入分立（油门 / 刹车 / 转向 / 手刹 bool）+「帧内缓存 `TArray<FWheelStatus>` + 按索引回读」。**只取输入分立与状态回读思想**，字段名 / 归一化量不照搬（Jolt 侧无对应 getter） |
| vendored Jolt | `Physics/Vehicle/` 24 文件；`WheeledVehicleController.h:105` `SetDriverInput(forward,right,brake,handBrake)`、`:19` `WheelSettingsWV`、`:160` ConstructWheel 断言；`VehicleConstraint.h:69/79/127/130/136/142`；`Wheel.h:64/70/76/79/100`；`VehicleCollisionTester.h:84` `VehicleCollisionTesterRay`；`VehicleDifferential.h:31-32` 默认 -1；`PhysicsSystem.h:128/151/162` | **不含 Jolt Samples / VehicleTest**（三重确认零命中），装配序列只能按头签名推导；`GetController()` 返回基类，须向下转换；每个 differential 的左右轮索引必须显式填；手刹是 float 0~1 非 bool |
| 本项目 | `World.hpp:212-253`（fresh world per call 不变量）、`World.cpp:528` `ContactCollector` / `:678` `simulateContactProbe`、`DebugScene::Impl:1253` | M-D0 collector 是 `World.cpp` 内局部持有；M-D2 若产轮接触事件须把同类 collector 挂到载具 world（不改 M-D0 实现、contacts 节逐字节不变） |

## 3. 历史约定（不得违反）

1. `AGENTS.md` 决策前置铁律：三处口径不一致 / 触碰架构所有权 → 必须摆证据让用户定。
2. `P8-DRIVE-PLAN.md` §1：`nl/physics` 公开头零 Jolt 类型，PIMPL / POD 视图；集成层只做装配 / 输入 / 读出。
3. 同 §1 + `PHYS-TOOLS-GUIDE.md`：确定性 = fresh world + 固定 1/60 + `JobSystemSingleThreaded`；trace→check 锚从真实 TSV 反推，禁手写。
4. `INPUT-TOOLS-GUIDE.md:82`：M-I2 `--input-demo` 帧序号驱动注入脚本先例（W / D / Space 按帧注入）。
5. `P8-DRIVE-PLAN.md` §3 / §5：M-D2 三锚场景（直线加速 / 定角转向 / 手刹急停）+ 轮接触事件锚 + 「M-D0 契约零回归（contacts 节逐字节不变）」；M-D3 才碰 `viewer/main.cpp` / `DebugScene` scenario 2。
6. A 类轨道：零 `kStage`、零图形、双后端 ctest、工具版交代。

## 4. 三个方案与代价矩阵

| 方案 | 公开面 | M-D2 怎么跑 | M-D3 怎么接 |
|---|---|---|---|
| **A 探针式 fresh world**（建议） | `DriveConfig` / `DriveResult` / `simulateDrive(cfg)`（或挂 `World` 方法），POD 全在公开头；装配函数 `buildVehicle(JPH::PhysicsSystem&)` 只活在 `Vehicle.cpp` | 每次调用自建 world + floor + chassis + constraint，帧序号脚本一次跑完，dump 轨迹 / 轮态 / 接触事件 | `DebugScene` scenario 2 复用同一个内部装配函数把车挂进自己的 world；实时输入另开逐帧接口 |
| **B 长生命周期对象** | `Vehicle` 类自持 `PhysicsSystem`（Impl 内），`setInput()` / `step()` / `snapshot()` | 逐帧喂脚本、逐帧回读 | viewer 直接持有该对象；但训练场在 `DebugScene` 的**另一套** world 里 → 车与墙不互撞 |
| **C 薄层 attach 外部 world** | `Vehicle` 只做装配 / 输入 / 回读，构造接收外部 `JPH::PhysicsSystem&`；公开头零 Jolt 需不透明句柄或跨类 friend | phys-tools 自建 world 传入 | `DebugScene` scenario 2 传自己的 world，车与墙同世界 |

| 维度 | A | B | C |
|---|---|---|---|
| M-D2 落码量 | 小（与 M-P0~M-D4 / M-D0 探针同构） | 中（生命周期 + 逐帧接口 + 销毁次序） | 中（不透明桥 + attach 契约） |
| 确定性基线 | 天然（fresh world per call，逐位复现） | 需额外保证 reset / 重建等价 | 依赖调用方 world 状态 |
| M-D3 复用成本 | 抽一次装配函数（约 1 处） | **高**：要么并两套世界（撞墙 demo 不成立），要么改 `DebugScene` 所有权 | 低，但 M-D2 就得把挂载契约定死 |
| 公开头零 Jolt | 满足 | 满足 | 需 `void*` / `uintptr_t` 或 friend，破坏 POD 风格 |
| 轮接触事件衔接 | 同 `World.cpp` 内 collector，零改 M-D0 | collector 要挂到 Vehicle 私有 world | collector 挂调用方 world |
| M-D0/D1 回归面 | 零（新探针独立） | 零 | 零 |
| 提前定死架构的风险 | 低（所有权留给 M-D3） | 高 | 中高 |

## 5. 建议 A（**已拍板，见文首拍板结果**）

**建议 A**：与 `World.hpp:226-228`「A fresh world per call is what makes repeated runs bit-identical」不变量、
M-P0~M-D4 / M-D0 全部探针先例、以及计划 §3「帧序号驱动输入脚本 + per-frame dump」完全同构；
M-D3 的实时输入是另一个里程碑的问题，届时把 `Vehicle.cpp` 内的装配逻辑抽成 `buildVehicle(PhysicsSystem&)`
供 `DebugScene` scenario 2 复用即可，不需要 M-D2 提前决定「谁持有 PhysicsSystem」。
代价是 M-D3 要抽一次函数并补逐帧输入接口 —— 已列入 M-D3 范围，不是遗漏。
> **兑现状态（M-D3 收官）**：M-D2 收官时该函数**未落**（文档承诺超前于代码）；**M-D3 已按 Q-D3-1 A 真抽出并兑现** —— `core/src/physics/Vehicle.cpp::buildVehicle(JPH::PhysicsSystem&)` 与 `core/src/physics/VehicleInternal.hpp`，`simulateDrive()` 和 `DebugScene::build(2)` 共用同一装配序列，M-D2 归档 `drive.tsv` 逐字节零回归。见 `D3-DRIVE-VIEW-OPTIONS.md` §2.1 / `DRIVE-TOOLS-GUIDE.md` §11.1。

## 6. 拍板后动作清单（M-D2 已执行）

1. 结论写回 `P8-DRIVE-PLAN.md` §7 拍板记录（新增 Q-D2-1 行 + 状态段），并在 §3 M-D2 补「开工锚点表」。
2. 同步删掉历史挂账句：§2.3 标题「需用户拍板，见 Q1」/ 正文「列为 Q1 待确认」、§7 Q1 / Q2 建议栏「确认后 …」——Q1 / Q2 实际已在 M-D0 / M-D1 执行，属文字残留。
3. 之后才写第一行架构代码：`Vehicle.hpp/.cpp` → 根 CMakeLists 两行 → phys-tools `drive-trace` / `drive-check` / selftest `vehicle` 节 → ctest → `example/drive-m2` → cases.json / 证据图 / 指南 → 四栏 → tag `drive-m2` → close 闸 → `release --done` → 主树同步。

## 7. 冻结面（本轮一行架构代码未碰）

- 所有新增 RT / UAV / 绑定槽、`kStage`、渲染回归基线、`renderer/`、`viewer/main.cpp`、`DebugScene` scenario 2（M-D3 面）。
- `World::simulateContactProbe` / `ContactCollector`（M-D0 实现）、`example/drive-m0` 与 `example/drive-m1` 全部归档基线（contacts.tsv / ecs.tsv / 4 张 PNG）逐字节不动。
- `P8-DRIVE-PLAN.md` 除挂账句纠错外不改；主树活跃台账不动（按 §7.2 四栏留轨道分支、整轨 merge 时一次带入）。
