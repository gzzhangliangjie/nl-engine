# STEP7-PLAN — GAME-DEV-ORDER 第7步：最小可玩版（独立 game.exe + 城市真闭环）

> 立项：2026-11-16 用户「继续」→ 开工闸读三源证据 → 两轮拍板定范围/形态/技术路径。
> 编号 M-GAME0，新轨 `game`（feat/game，wt `nl-engine-game`），起点 master `733e28a7`。
> 门槛（GAME-DEV-ORDER §2 第7步）：**能在生成的城市里开一圈、撞了有事件反馈**——本轮取「城市真闭环」档，一次把第5步游戏壳 + 第7步闭环合拢。

## 1. 三处证据（开工闸实读，2026-11-16）

| 源 | 实读结论 |
|---|---|
| `docs/GAME-DEV-ORDER.md` §2 第5/7步 + §3 | 第7步 = 小规模程序化城市 + 载具驾驶闭环，前置 5+6；第6步城市已收官（W轨 + CG轨），**第5步游戏壳"待开工（Q2 定 target 形态）"完全没动**，但 GameClock/GameLoop 轨能力已就绪（tag `gl-m0~m5`）。Q2 此前只停在"建议=独立 game target"，无拍板标记。 |
| `viewer/main.cpp` + 两路探查（core API / 构建回归） | ① 玩家驾驶硬绑 `physicsDebug && physicsScenario==2` 空训练场（`main.cpp:3878`），CG2 城市只有视觉 mesh + AI 交通车，两条路径**互斥**（`:7200-7204`）；② 碰撞事件 `contactAdded/Persisted/Removed` 在 viewer 只有 HUD 数字（`:4632-4635`），无反馈接线；`P8-DRIVE-PLAN.md:174` 明确把"接触点可视化"留到第7步；③ `city.scene.engine` 150 栋楼只有 Model+Transform，**无碰撞体**；`DebugScene` 无 `addStaticBox` 公开 API，唯一建静态体入口是 `QueryScene::registerBody`（query 语义）。 |
| core/renderer 公开 API 复用面 | 现成可直接用：`GameClock::tick` + `TickScheduler`（game_clock.hpp / tick_groups.hpp）、`DebugScene`（World.hpp：build/step/setDriveInput/chassisState/driveState/extractLines/queryHost）、`CameraDirector` + `ChaseCamera`（camera_director.hpp）、`World::deserialise` + `registerComponent<Transform/Model/Light>`（scene/World.hpp）、`ContactEventStream/ContactEventSystem`（physics/ContactEvent*.hpp）、`Window` + `Renderer::create/loadEcsScene/renderFrame`（renderer.hpp）。**需新写胶水**：场景加载/实体→渲染描述/FrameParams/TickGroup 编排/输入映射（约 200 行，viewer 私有逻辑照搬改写）。**需新增 core API**：批量静态盒 + 撞击强度回读。 |

## 2. 拍板记录（本轮 2026-11-16，四项全拍）

| 问 | 结论 | 理由 |
|---|---|---|
| Q-STEP7-1 范围档位 | **城市真闭环**：给 CG2 珠江新城楼体建 Jolt 静态碰撞体，玩家车真在街道里开、撞楼有反馈 | 用户选最高档，满足门槛原意；"最小闭环借场景"+"先补游戏壳"两案被否 |
| Q-STEP7-2（=Q2）承载形态 | **独立 `game.exe`**：新建专门游戏程序，viewer 退回纯展示台 | 架构干净；viewer 加 `--play` 案被否 |
| Q-STEP7-3 楼体碰撞体 | **直接新增 core API**：`DebugScene::addStaticBox` 批量建静态盒 + 楼/车 layer 互通；`DebugScene` 回读撞击强度（冲量/相对速度） | "先复用 query API 验证"+"软碰撞不建体"两案被否。冻结面相应放宽：core **只加不改**（既有签名/导出面/viewer 基线零变动） |
| Q-STEP7-4 撞楼反馈形式 | **震屏 + 音效 + 事件日志+HUD**（三选，multiSelect） | 接触点 debugLines 可视化**不做**（用户未选）；震屏走 CameraSmoother/CameraTrauma（现成默认关需接线），音效走 audio 轨 WAV 播放 API，事件日志+HUD 走 ContactEventStream::drain |

## 3. 冻结面

1. **viewer / renderer 既有源码零改动**（renderer 可加新文件但不动老文件；viewer 一个字不改），kStage=S8.3 不动；
2. **CG0/CG1/CG2 冻结产物零改**（city.scene / top.png / roadnet.json / *.glb / ODbL 署名 sha256 只读消费）；
3. **core 只加不改**：新增公开 API（静态盒批量建 + 撞击回读），既有公开签名、`exports.api`、bindings 一律不变，所有既有 selftest/ctest/回归卡零回归；
4. `city.scene.engine` schema 只加不改；复用 CG2 派生产物，不重写转换链；
5. 输入/相机/物理既有公开头不改语义；`GameClock::kStep` 固定步长不动；
6. 展示卡新增独立版本 `game-m0`，不碰任何既有版本卡。

## 4. 实施方案

### 4.1 里程碑拆分（M-GAME0 单里程碑内三段落码）

- **G0.1 core 只加不改**：`core/include/nl/physics/World.hpp` 新增 `addStaticBox(pos, halfExtents, layer, owner)` / `clearStaticBoxes()` / `buildCityCollisionFromScene(json)`（或等价批量入口）+ `DriveViewState` 增补撞击强度字段（additive，不改既有字段布局语义）；`core/src/physics/World.cpp` 落 Jolt 静态体（layer 矩阵确保楼体层与车辆层互通）。配 `phys-tools` selftest 段 + ctest（撞墙对照：无静态体车穿过 / 有静态体车停住 + 事件带 owner）。
- **G0.2 独立 game.exe**：新建 `modules/game/main.cpp` + `modules/game/CMakeLists.txt`（照 viewer/CMakeLists.txt 渲染型骨架：`add_executable(game ...)` + link `nl_core nl_renderer nl_profiler glfw` + `NL_BACKEND_NAME` 宏 + `add_dependencies(game nl_shaders)`），根 `CMakeLists.txt` L52-55 `NL_ALL_MODULES` 加 `game`。main.cpp 复用清单：
  - 装载 CG2 `example/cg2-m0/city.scene.engine`（`nlohmann::json` + `World::deserialise` + 三组件注册），实体→`SceneObjectDesc`/`SceneLightDesc` 收集；
  - 从同一 scene JSON 的 150 栋 box Transform 调 G0.1 批量静态盒 API 建碰撞；
  - 玩家车：`DebugScene::build(2,...)` 训练场里取车体，`addVehicle` 放到城市街道起点（或复用 scenario2 车体 + teleport 到城市坐标）；`setDriveInput(Window::rawKeyDown WASD)`；
  - `CameraDirector` Chase 模式喂 `chassisState()`，`viewProjection()` 填 `FrameParams`；
  - `GameClock` 四 TickGroup 编排：PrePhysics=输入、Physics=`stream.update()+physicsScene.step()+publish`、PostPhysics=`world.tick()+ContactEventSystem.drain()`、PostUpdateWork=audio 同步；
  - 撞楼反馈三路：事件日志（结构化 `impact` 行：frame/owner/pos/强度）+ HUD 撞击计数/最近位置（`HudFrame`，ASCII）+ 音效（audio 轨 WAV）+ 震屏（`CameraTrauma` 接线到相机 offset）。
- **G0.3 确定性验收**：`--script` 帧序号输入脚本（照 `--drive-demo` 口径，可复现），`--screenshot` 程序内截图，双后端 PNG 基线；selftest 数值锚（车前进位移 >0 / 撞墙后位移收敛 / 事件计数 >0 / 三段反馈各触发一次）。

### 4.2 CLI 契约

```
game.exe --selftest                       # 无头结构+数值锚
game.exe --city DIR --play-script FILE --screenshot PATH --frames N --size WxH [--seed N] [--no-hud]
exit: 0 ok / 1 runtime / 2 usage
```
缺省 `--city` = `example/cg2-m0`，`--play-script` 缺省内置帧序号脚本（前进→撞楼→回头）。

### 4.3 展示卡 `game-m0`

- exe 卡：`exe={dx12:"build/DX12/game.exe",vulkan:"build/VULKAN/game.exe"}`，case 级 cwd=`{root}`；
- 用例：usage（exit 2）/ selftest / 城市驾驶截图（双后端 PNG）/ 撞击反馈日志锚；
- `preview = example/game-m0/game-street.png`。

## 5. 验收判据

1. 双后端 `BUILD OK`（errors=0）；`game.exe --selftest` PASS（新增数值锚：位移/撞停/事件/反馈三路）；
2. ctest 双后端全绿（新增 game selftest 用例 + 既有全部零回归，物理 selftest 段扩项通过）；
3. `game.exe --city example/cg2-m0` 双后端 rc=0 + 程序内截图非空，肉眼可见珠江新城街景 + 玩家车 + 楼体 + HUD 撞击反馈；
4. 撞楼实证：撞击日志行含 owner=楼体、强度 >0；音效/震屏/HUD 三路各至少触发一次（日志锚可机器验）；
5. `--only game-m0` 双后端 bytes ALL GREEN；既有 `--only cg0-m0`/`cg1-m0`/`cg2-m0` 及其余全量卡零回归（viewer 未动实证）；
6. `showcase/server.py --check` exit 0；close 闸 PASS + `release --done` + annotated tag `game-m0` + master ff + push。

## 6. 范围边界

- 只做**单机玩家驾驶闭环**：无交通调度（M-T 轨已收官但本轮不接 AI 车流的玩家交互）、无换道/多车道、无行人 navmesh、无全城 V4 流送；
- 楼体碰撞体取 CG2 box 的 Transform（150 栋），不做逐三角 mesh 碰撞；地标塔（lm_0）可建粗盒或按范围外处理（本轮可选，不强行）；
- 反馈三路按拍板：震屏 + 音效 + 事件日志+HUD；接触点 debugLines 可视化不做、不追加；
- 撞击为物理接触（真挡车），不做软碰撞探测；
- 输入为键盘 WASD（复用 `rawKeyDown` 只读口径，不接 `updateFromWindow`）；本轮不做手柄。
