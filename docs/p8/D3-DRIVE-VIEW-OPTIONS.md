# M-D3 决策材料：驾驶可见的挂载 / 输入 / 基线边界（已拍板，决策依据快照）

> 2026-11-10 · track `drive` / M-D3 开工前决策前置功课。**拍板结果（2026-11-10 用户）：
> Q-D3-1 A / Q-D3-2 A / Q-D3-3 A / Q-D3-4「折中留钩子」**（A 的变体，见 §3 Q-D3-4）。
> 结论已写回 `P8-DRIVE-PLAN.md` §3 M-D3 开工锚点表 / §6 留痕 / §7 决策记录，
> 失真承诺与「待拍板」挂账句已同步清理，开工闸满足：**待拍板架构项 0**，方落码。
> 本文件保留为决策依据快照：§3 各方案矩阵与 §4 原文「建议组合」是拍板前措辞，不再代表当前状态。
> 按 `AGENTS.md` 决策前置铁律与「开工闸」出具：计划 §3 M-D3 只有三条粗线条，且 M-D2 拍板 A
> 明确写了「不提前定 M-D3 挂载契约」，第一行 viewer 接线代码会把 4 个架构判定隐式拍死，故先摊方案交用户拍板。

## 1. 三处点名面实读（本轮重读，未复用 M-D0/D1/D2 证据）

| 参考 | 本轮实读面 | 结论 |
|---|---|---|
| **Carrot** | `engine/engine/io/actions/Action.hpp:55-77,245-272` / `ActionSet.cpp:220-330` 输入→动作映射；`engine/engine/scene/SceneManager.h:24-55` + `samples/testbed/` 场景注册；`console/Console.cpp:39-54` + `RuntimeOption.hpp:11-16` 运行时开关；`io/actions/ActionDebug.cpp:11-39` ImGui 动作值面板 | 输入映射 / 场景注册 / 调试面板有对应物；**载具无对应物**（physics 16 文件零 Vehicle），console 只有 bool 开关型 option，不是数值 cvar；HUD 一律走 ImGui 窗口，无 AHUD 式类 |
| **bevy** | `bevy_input/src/button_input.rs:19-30,125-252` 三态；`bevy_gizmos/src/lib.rs:7-16` / `bevy_dev_tools/` 调试可视化；`bevy_app/src/main_schedule.rs:94-163,355-363` `FixedUpdate`；`examples/movement/physics_in_fixed_timestep.rs:66-83,110-129,291-319` 输入累积 → 固定步消费 | **无官方物理 / 载具**（唯一 vehicle 命中是 `bevy_ecs/src/component/mod.rs:52` 教学示例）。`physics_in_fixed_timestep.rs` 是「渲染帧累积输入 → FixedUpdate 消费」的官方同构先例，正好印证本项目双帧模型 |
| **UE 5.8.4** | `WheeledVehiclePawn.h:12-20` / `.cpp:13-28`（类名是 `AWheeledVehiclePawn`，非 `AChaos*`）；`ChaosVehicleMovementComponent.h:887/899/903/919` 四输入 `BlueprintCallable`；`GameFramework/HUD.h:503` `DrawHUD()`、`Engine.h:2200` `AddOnScreenDebugMessage`；`ShowFlags.cpp:1114-1123` `ShowFlag.%s` cvar 注册 | 插件全树 Grep `SetupPlayerInputComponent|BindAction|BindAxis` **零命中**、`SetThrottleInput(` 仅声明+定义**无调用点** → **引擎 Pawn 不绑输入，绑定在游戏层**；HUD / 屏幕调试消息 / showflag cvar 三类调试惯例齐备。UE 只读不抄 |
| **历史约定** | `INPUT-TOOLS-GUIDE.md:76-83`（M-I2 单消费者铁律 + `--input-demo`）、`GAMELOOP-TOOLS-GUIDE.md:57-59,84,98-107`（双帧 / 组注册先于 console-exec）、`P8-CAMERA-PLAN.md:9-10,12-14,135,149-151,160-174`（viewer b 段排 M-D3 之后、复用 M-D3 输入管线）、`GAME-DEV-ORDER.md:29-30,41`（第 3 步 console 实调 / 1~5 硬链）、`PARALLEL-TRACKS.md:19,67,148-150`（A 类不进 kStage、工具版交代） | 见 §2 硬约束 |

## 2. 本地现状的 7 条硬约束（本次实读实证，逐条带文件:行号）

1. **`buildVehicle(PhysicsSystem&)` 根本不存在**。`Vehicle.hpp:8` / `P8-DRIVE-PLAN.md:121,224` / `D2-VEHICLE-OWNERSHIP-OPTIONS.md:54,72` 共 5 处全是注释/文档承诺；`Vehicle.cpp` 全文零该函数，装配逻辑整体内联在 `simulateDrive()` 的 `:129-222`。M-D3 按注释直接「复用」会扑空 —— 必须先真抽函数，这是 M-D2 拍板 A 留下的未兑现项。
2. **层过滤冲突（会让所有轮子 miss）**。`Vehicle.cpp:44-49,50-69` 三个 filter 全层对 `ShouldCollide=true`，注释实证 M-P0 式 `floor/floor` 禁用会让 `VehicleCollisionTesterRay` 全轮 miss（M-D2 踩过、`events=0`）；而 `DebugScene` scenario 0 在 `World.cpp:1310` 正是 `I.matrix.set(kFloor,kFloor,false)`。**车挂进 DebugScene 现有 world 前必须解掉这对差异**。
3. **M-D2 的 fresh world 与 DebugScene 的 world 是两套互不相干的世界**。`Vehicle.cpp:122` 是每次调用新建的栈上 `PhysicsSystem`；`World.cpp:1253-1281` `DebugScene::Impl` 自带 `physicsSystem`（scenario 0 用）+ `charH`（scenario 1 全接管、另带一套）。`Impl` 当前无任何 `VehicleConstraint` / controller / 车体 BodyID 成员。
4. **单消费者铁律与「实时键鼠」正面冲突**。`input.hpp:21-23` + `viewer/main.cpp:857-858` 明写「viewer 绝不调 `updateFromWindow`，现有热键/ImGui 键盘通道零触碰」；`INPUT-TOOLS-GUIDE.md:81` 同款。而计划 §3 M-D3 写「M-I1 ActionMap 实时轮询（渲染帧）」。且 `P8-CAMERA-PLAN.md:41,173-174` 规定 camera b 段要**复用 M-D3 落的这条输入管线** —— 该形态会成为下游契约。
5. **HUD 常驻新增行会波及 35 个既有用例 × 2 后端 = 70 张基线**。`tools/reshot-hud-stage.py:31,36-52` 安全闸签名 `HUD_LINE_MAX_PIXELS=800`；`hud.cpp:251` 面板上限 560。零波及的唯一既有范式是 `hud.cpp:449-457`「独立兄弟窗口 + 默认关闸门」（M-I2 inputDebug / M-S2 audioDebug 先例，注释明写「with the gate off it draws nothing, so no existing HUD baseline moves」）。
6. **`kStage` 仍 `S8.2`，`server.py --check` 只校验 `s8.2` 卡**。`renderer/src/renderer.cpp:16-17`；`tools/showcase/server.py:214-216,230-245` 由 `kStage` 前缀推 `version_id`。新增 `drive-m3` image 卡不会被 `--check` 覆盖；M9.0 刚立的口径是「新卡齐备由 `run.bat verify` + `--only <version>` 双验」。A 类轨道纪律「不碰 kStage」与本里程碑首次落渲染接线面在此交汇。
7. **viewer 现状与 camera 排期**。`viewer/main.cpp` 唯一相机是 `OrbitCamera`（`:735` / `:1820`），`ChaseCamera` / `CameraDirector` 只在 core 侧 a 段与 cam-tools 无头验收，viewer 全文件 Grep `Chase|CameraDirector|readFreeInputs` 零命中。`P8-CAMERA-PLAN.md:9-10,135,149-151` 把 viewer b 段排在 **M-D3 之后串行**，M-CAM2 硬闸 = M-D3 已收口。另：drive wt 的 `viewer/main.cpp` 落后 master `dd43bc7` 一个 M9.0 `ViewMode::Shadow` 改动（4 处差异，与 camera/drive 无关）。

## 3. 四项决策与代价矩阵（拍板前原始矩阵，结论见 §4）

### Q-D3-1 载具运行时所有权 / 挂载形态（M-D2 遗留，最关键）

| 方案 | 形态 | 代价 | 风险 |
|---|---|---|---|
| **A 挂进 DebugScene scenario 2**（建议） | 真抽出 `buildVehicle(JPH::PhysicsSystem&)`，`DebugScene::Impl` 持有长生命周期 `Ref<VehicleConstraint>` / controller / 车体 id / floor；`build(2)` 复用 `Impl::physicsSystem`，`step()` 每逻辑帧 `SetDriverInput` + `PhysicsSystem::Update`；公开头只加 `setDriveInput()` / `driveState()` POD | 动 `core/src/physics/World.cpp`（M-D2 曾冻结「不碰」）+ 新增零 Jolt 公开接口；必须解 §2.2 层过滤冲突 | 低。车与墙/地面同一 world → 底盘撞墙天然产 `ContactListener` 事件，M-D1 消费 demo 有真实载体；与 §1 bevy 固定步同构 |
| B 独立 `DriveWorld` 类 | `core/physics/` 新建类自持 world + vehicle，viewer 直接驱动；DebugScene 只画静态训练场 | 两套 world 分离，训练场地形与车体不在同一物理世界 | 中高。撞墙要跨世界合成事件，M-D1 demo 变成假数据；`extractLines()` 要合并两套线框 |
| C 每帧重跑 `simulateDrive` | 复用 fresh world，逐帧把脚本跑到当前帧 | 零新架构 | 不可接受：O(N²)、无法实时响应输入、与「长生命周期」语义相反 |

### Q-D3-2 实时输入管线（单消费者铁律 + camera b 段契约）

| 方案 | 形态 | 代价 | 风险 |
|---|---|---|---|
| **A 注入管线扩展 + 窗口只读查询**（建议） | 扩展 M-I2 注入通道：新增 `--drive-demo` 帧序号脚本（确定性回放，照 `--input-demo` 先例）作截图基线源；实时键鼠用 window 的**只读 `isDown` / 鼠标增量查询**（不消费事件队列）→ `feedKey` 喂 `InputState` → `ActionMap::update` → Physics 组灌输入。viewer 仍不调 `updateFromWindow` | 需要 window 暴露只读键态查询；输入语义要写清「渲染帧累积 → 逻辑帧消费」 | 低。不破铁律、ImGui/console 键盘通道零触碰；camera b 段可直接复用同一管线，满足 `P8-CAMERA-PLAN.md:41,173-174` |
| B 解除铁律 | viewer 直接 `updateFromWindow`，把 ImGui 键盘通道改成 feed 转发 | 改动既有热键 / ImGui / console 三条通路；35 个含 `--hud` 用例可能全变 | 高。与 M-I2 立下的纪律冲突，且会扰动大量既有基线 |
| C 只做脚本回放 | M-D3 仅 `--drive-demo` 确定性脚本，实时键鼠留游戏壳 | 最小改动 | 不满足 §5「键鼠实时驾驶肉眼可验」与 `GAME-DEV-ORDER.md` 第 3 步「console 实调」，验收缺口 |

### Q-D3-3 kStage / HUD / 基线口径（A 类轨道首次落渲染面）

| 方案 | 形态 | 代价 | 风险 |
|---|---|---|---|
| **A 不 bump + 独立窗口 + 工具版为主**（建议） | `kStage` 保持 `S8.2`；HUD 参数行走 `hud.cpp:449-457` 独立兄弟窗口 + 默认关闸门（零波及 70 张基线）；`drive-m3` 版本卡以 tool 卡为主 + 一组 `pair` 截图标；新卡由 `run.bat verify` + `--only drive-m3` 双验（M9.0 口径） | 需写清「`server.py --check` 只校验 s8.2」的例外并落档 | 低。照 ECS E1b / console / M9.0 三次先例；A 类纪律不破 |
| B bump 到 S8.3 | 新建 `s8.3` 版本卡 + 全量重拍 HUD 基线 | `reshot-hud-stage.py` 35 用例 / 70 张图、viewer selftest `:415` 硬编码锚要同步改 | 高。A 类轨道「不进 kStage」纪律被破，且与主线 S9 排期打架 |
| C HUD 常驻行 + `--force` 重拍 | 参数行直接进主面板 | 需人工逐像素确认 70 张图 diff 全在 HUD 面板 | 中高。超 800px 签名要追因放行，纪律上属于「能避则避」 |

### Q-D3-4 相机与 viewer 接线范围

| 方案 | 形态 | 代价 | 风险 |
|---|---|---|---|
| **A OrbitCamera 固定机位 + 训练场有限范围**（建议） | M-D3 不接 Chase，遵守 `P8-CAMERA-PLAN.md` 串行排期；训练场直道/弯道/墙布在固定机位可见范围内 | 车开出画面后的跟车体验留到 camera b 段 | 低。不抢 camera 轨的活，M-CAM2 硬闸顺序不变 |
| B 顺手接 CameraDirector 的 Chase | 复用已落 a 段，viewer 里换投影源 | 等于提前做 camera b 段；且 `fp.mvp` / `fp.viewProj` 在 `viewer/main.cpp:978/999` 直接产出，要换源 | 中。越过串行排期，camera 轨后续 merge 时同文件二次改 |
| C 不 merge master 直接接线 | 基于落后一个 M9.0 commit 的 viewer 落码 | 整轨 merge 时一次解 4 处 Shadow 冲突 | 视 Q-D3-1/2 改动量；属工程操作，可由执行侧按「工作线粘性」决定，不单列拍板 |
| **D 折中留钩子（用户选择）** | 保持 A 的 OrbitCamera 固定机位，但 M-D3 把接口留稳：`DebugScene` 暴露 Jolt-free `ChassisState` POD（`camera_director.hpp:45-50` 已定义）、viewer 加「投影源可切换」钩子且默认仍取 `OrbitCamera::viewProjection()`；不落 Chase/Free 胶水、不拍相机基线、不验相机五锚 | 多一组 POD 接口 + 一个默认分支钩子 | 低。M-D3 不背相机验收；收口后 camera b 段只填钩子，「同文件冲突面一次清」收益仍在 |

> **Q-D3-4 的 C 项不是架构判定**：是否中途 merge master 属工程操作，按既有「工作线粘性」纪律由执行侧决定并在收口留痕，不占拍板项。

## 4. 拍板结果（2026-11-10 用户）

**Q-D3-1 A / Q-D3-2 A / Q-D3-3 A / Q-D3-4 D「折中留钩子」**：

1. 车挂进 `DebugScene` scenario 2 并**真抽** `buildVehicle(PhysicsSystem&)`（M-D2 拍板 A 的未兑现承诺，本轮兑现）；公开头只加 Jolt-free POD 接口；解掉 `floor/floor` 层过滤冲突。
2. 输入走注入管线扩展：新增 `--drive-demo` 帧序号脚本作确定性截图源；实时键鼠用窗口只读键态查询（不消费事件队列）→ `feedKey` → `ActionMap::update` → Physics 组灌输入；viewer 仍不调 `updateFromWindow`。
3. `kStage` 保持 `S8.2`；HUD 参数行走独立兄弟窗口 + 默认关闸门（70 张既有基线零波及）；`drive-m3` 卡以 tool 卡为主 + 一组 pair 截图，由 `run.bat verify` + `--only drive-m3` 双验。
4. 相机保持 OrbitCamera 固定机位；但 M-D3 留两个接口：`DebugScene` 暴露 `ChassisState` POD、viewer 提供投影源可切换钩子（默认 OrbitCamera）。**不接 Chase/Free、不拍相机基线、不验相机五锚** —— b 段仍按 `P8-CAMERA-PLAN.md` 排 M-D3 之后串行。

> 拍板前本节原文为「**建议组合 A / A / A / A**」，理由「四项都取改动面最小 + 不破既有纪律 + 给下游留可复用接口」。用户把第 4 项从纯 A 升级为折中留钩子：不越排期，但把 M-D3 作为管线提供方的接口留稳。

## 5. 拍板后动作清单（全部已执行，M-D3 收官）

1. ✅ 结论写回 `P8-DRIVE-PLAN.md`：§3 增「M-D3 开工锚点表」、§7 增 Q-D3-1~4 状态段与表格行、§6 留痕。
2. ✅ 删掉 §3 M-D3 三条粗线条里与拍板结论冲突的措辞，并**更正 §3 M-D2 锚点表 / §7 Q-D2-1 行 / `Vehicle.hpp:8` / `D2-VEHICLE-OWNERSHIP-OPTIONS.md` 中「`buildVehicle` 已存在」的失真承诺**（改为「M-D3 抽出」）。
3. ✅ 同步扫全轨旧口径（`P8-DRIVE-PLAN.md`、`D2-VEHICLE-OWNERSHIP-OPTIONS.md`、`DRIVE-TOOLS-GUIDE.md`（无命中）、`Vehicle.hpp`），本文件顶部状态与「待拍板」挂账句一并清掉。
4. ✅ 落码顺序全部兑现：抽 `buildVehicle` → `DebugScene` scenario 2 + POD 接口 → viewer 输入/分时接线 → cvar → HUD 独立窗口 → `--drive-demo` 确定性 → example/cases.json → 双后端基线 → 文档 → tag `drive-m3` → close 闸。
5. ✅ 「`server.py --check` 只校验 `s8.2`」的例外与 `run.bat` / `--only drive-m3` 双验已写进 `DRIVE-TOOLS-GUIDE.md` §11.4。

> **兑现实测（2026-11-10 收官）**：`--only drive-m3` **12/12 bytes ALL GREEN**，`server.py --check` exit 0，run.bat 连续两轮 ALL GREEN，双后端截图各自逐字节稳定，M-D0/D1/D2 归档 TSV 六组零回归，`kStage` 保持 `S8.2`。**一处冻结面内的实测勘误**：§4 第 4 条 / §6 冻结面写「相机默认固定机位」无误，但「训练场布在可见范围」的预想不成立（默认机位只框 Suzanne）；M-D3 没有改任何相机代码，改由截图卡命令行 `--dist 45 --yaw 90 --pitch 40` 取景，Q-D3-4 冻结面完整保住。

## 6. 冻结面（本轮拍板不涉及、落码不得顺手改）

- `kStage` 字符串、`modes.hpp` 的 `ViewMode` 枚举 / `kModeNames`、viewer selftest 的 `S8.2` 硬编码锚。
- `World.cpp` 的 M-D0 `ContactCollector` / `simulateContactProbe`、`example/drive-m0/contacts.tsv`。
- M-D1 `ContactEventStream` / `ContactEventSystem`、`example/drive-m1/ecs.tsv`。
- M-D2 `simulateDrive()` 的公开签名与 `example/drive-m2/drive.tsv`（481F/1924W/1920E 逐字节冻结）。
- 既有 35 个含 `--hud` 用例的双后端基线 PNG、`cases.json` 中 `s8.2` 及更早版本卡。
- `core/src/camera_director.cpp` / `chase_camera.cpp` 等 camera a 段产物（camera 轨所有物）。
- 主树 `AGENTS.md` / `example/cases.json` 的并行会话改动。
