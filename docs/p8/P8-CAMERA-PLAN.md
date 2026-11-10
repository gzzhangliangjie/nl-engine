# P8-CAMERA-PLAN — 玩法相机族：Chase / 自由 / 轨迹（A 类并行轨道，2026-11-10 立项）

> 起因：城市驾驶开发顺序第 4 步（`docs/GAME-DEV-ORDER.md` §2）。**范围拍板（2026-11-10 用户）**：
> 初版只立项 Chase 相机，用户复核「不止这两种相机吧——比如纯自由相机、或者轨迹文件控制的相机」
> 后扩为**相机族**三件套：跟车 Chase + 纯自由 Free + 轨迹文件 Cinematic。
> **现状实证**：core 相机面只有 `OrbitCamera`（M0.4，纯 CPU 轨道相机，带 `cameraSelfTest` +
> console cvar `cam.fov`/`cam.dist`）——零跟随/自由/轨迹相机；grep 全树无任何 chase/fly/track 语义。
> **性质**：A 类并行轨道（不碰 `kStage`）。M-CAM0 全程零渲染（纯 CPU 逻辑 + 新工具 exe）；
> M-CAM1~CAM3 触碰渲染接线面（`viewer/main.cpp`），统一排在 drive M-D3 之后串行做（同文件冲突面一次清）。
> **归属**：track `camera`，里程碑 M-CAM0~M-CAM3；tag 名字制（`cam-m0` 起），不占 m0.* 编号。
> **与 drive 轨的关系**：M-CAM0 不依赖 drive（Chase 用脚本轨迹当假目标）；M-CAM2 强依赖
> drive M-D3（训练场 + 真车回读）；M-CAM1/M-CAM3 与 drive 无数据依赖，但 viewer 接线共用
> M-D3 落的输入管线（窗口 → InputState → ActionMap），一并串行。

## 1. 目标

给引擎接上「玩法相机族」，四步递进——三件套共用同一套位姿/矩阵底盘，互为兄弟类（不提前抽象框架）：

| 相机 | 一句话 | 主要用途 |
|---|---|---|
| **FreeCamera 纯自由** | WASD 平移 + 鼠标 look 的飞行观察相机（ spectator / fly-through） | 城市大场景调试巡视、截图找机位；**零数据依赖，最先能用** |
| **ChaseCamera 跟车** | 目标位姿输入 + 帧率无关阻尼跟随 + boom 装配 + C 切 orbit 环绕；**倒车自动切后视 / 回头键**两个低成本变体 | 玩家驾驶视角（第 4 步原始定义） |
| **CinematicCamera 轨迹** | keyframe 轨迹文件（TSV）驱动 + 确定性插值回放；**静态机位 = 单 keyframe loop 退化免费** | 运镜演示、展示台片段、交通监控视角、**双后端截图基线的机位工具**（轨迹本身确定性 → 证据管线直接受益） |

三件套共用**轻量 CameraRig 基座**（位姿/fov/aspect/Matrix4 输出口径共享）——族已 3+ 类，
纯复制三份口径会漂；但**不做** CameraManager 框架（场景模式开关够用，第五种相机出现再抽象）。

| 段 | 内容 | 落点 |
|---|---|---|
| **M-CAM0 无头核心** | `ChaseCamera` + `FreeCamera` 两个纯 CPU 类（阻尼跟随/boom rig/look-ahead/模式切换；位置+yaw/pitch 飞行+速度档）；`cam-tools` 新工具 selftest 锚场景 + trace→check 闭环 | `core/include/nl/core/chase_camera.hpp` + `free_camera.hpp`（新）+ `core/src/` 同名 cpp + `cam-tools/`（新工具，照 input-tools 三步骨架） |
| **M-CAM1 自由相机可见** | viewer 接线 FreeCamera（输入管线消费 + HUD 相机行 + console cvar），调试任意场景可立即飞行 | viewer 接线 + example 卡 / cases.json |
| **M-CAM2 跟车可见** | viewer 接线 ChaseCamera：drive M-D3 训练场实时跟车（C 切模式 / 鼠标 look / auto-return）+ `--drive-demo` 截图基线扩展 | viewer 接线 + example 卡 / cases.json |
| **M-CAM3 轨迹相机** | `CinematicCamera`：轨迹文件加载（TSV keyframe）+ 插值（线性 / smoothstep，逐位确定）+ 播放控制（play/pause/loop/倍速）+ cam-tools 锚 + viewer 播放与截图锚帧 | `cinematic_camera.hpp/.cpp`（新）+ cam-tools 扩节 + viewer 接线 + example 卡 |

**关键设计约束**（历史约定对齐）：
- **`OrbitCamera` 零改动零回归**：三个新类是兄弟类（同款纯 CPU / Matrix4 列主序 / `view()` 同源口径），viewer 既有轨道相机路径（`--selftest`、orbit 拖拽、cvar 同步）一字不动。
- 纯 CPU core 层纪律同 `camera.hpp` 头注：不接触任何图形 API / NVRHI 类型；同一份矩阵数值喂 DXIL/SPIR-V 双目标。
- **确定性契约**：固定 dt（与固定轨迹文件）下相机位姿逐位可复现——阻尼公式只吃 dt 与输入位姿、轨迹插值只吃 keyframe 数值，无 wall-clock 无随机；跨 dt 场景是**收敛性锚**（阈值）非逐位锚，两者在用例里显式分档。
- **输入面**：M-I1 driving set 已备 `camToggle`（C 边沿）+ `look`（Vec2 additive）；FreeCamera 的移动键（W/S/A/D + 升降 + 速度档）复用同一 driving set 既有语义 + 新增绑定走 `buildSpectatorActionMap` 小增量（新 SetId，driving set 零改动；具体键位表 M-CAM1 开工实读 `InputState` 键面后定）。viewer 输入管线（窗口 → InputState → ActionMap 实时轮询）由 drive M-D3 落，本轨只消费。
- **目标位姿数据源**：Chase 的 viewer 数据源 = M-D2 载具回读（chassis pos / yaw / velocity per-frame）；headless 侧用帧序号脚本轨迹（照 M-I2 `--input-demo` / M-D2 `DriveConfig` 先例）造假目标，相机对数据来源无感知。轨迹相机的数据源 = 离线 TSV 文件（与 trace/check 的 TSV 格式同族，工具链自洽）。
- 时钟：渲染帧轮询输入、GameClock 逻辑帧步进（M-GL0 dt），与 drive 载具输入轮询节奏同款；轨迹相机按逻辑帧时间轴播放。

## 2. 参考口径（立项预口径；M-CAM0 开工首日按铁律实读点名文件后修订留痕）

| 来源 | 预口径 | 用法 |
|---|---|---|
| **UE** SpringArm + CameraActor + Sequencer | Chase 锚：位置/旋转分离 lag（EnableCameraLag/RotationLag + LagSpeed）+ arm length/TargetOffset + bDoCollisionTest（探针）；**轨迹锚**：Sequencer keyframe track（时间轴 + 每通道 keyframe + 插值模式 Linear/Cubic） | 阻尼与 rig 的锚 / 轨迹文件格式与插值语义的锚；碰撞探针不做（§4） |
| **UE** DefaultFlyCamera / Editor 视口相机（PIE spectator） | Free 锚：WASD 平移 + 鼠标 look + 滚轮/Shift 速度档；无阻尼（调试相机要跟手） | FreeCamera 键位与手感的锚 |
| **Jolt vendored** | 不适用——相机不在物理层（Chase 所需 chassis pose 走 M-D2 回读，不 raycast） | 据实标注：本轨唯一 Jolt 关联面 = M-CAM2 的目标位姿来自 M-D2 回读 API |
| **Carrot / bevy** | Carrot 预判无 spring arm / 轨迹回放抽象；bevy 无官方相机跟随与 cinematic 轨迹（社区插件，core 不背） | 据实标注无对应物（铁律：标了就要开工实读确认） |
| **内部先例** | `OrbitCamera`（clamp 族 / gain 常量 / selftest 机械断言）、viewer cvar 注册（onChanged 回写）、trace→check TSV 闭环（input-tools） | 代码风格与验收形态的锚 |

## 3. 设计（逐里程碑细化，开工前补全锚点表）

### M-CAM0 无头核心

- **CameraRig 轻量基座**（`nl::core`）：位姿（eye/lookAt/yaw/pitch）+ fov/aspect + Matrix4 列主序输出口径三兄弟类共享（`viewProjection()` / `view()` / `eyePosition()` 同 OrbitCamera 只读口径）；只共享数据口径，不共享更新逻辑（各相机 update 各写）。OrbitCamera 不动不迁移（零回归）。
- **ChaseCamera**（`nl::core`）：
  - 输入 `update(dt, targetPos[3], targetYawDeg, targetVel[3])`；输出 `viewProjection()` / `view()` / `eyePosition()`（同 OrbitCamera 只读口径）。
  - rig：`boomYawOffset`（鼠标 look 累计，rad clamp）+ `pitch`（固定缺省 + 可调 clamp）+ `distance` + `heightOffset`；期望 eye = targetPos + height + boom(yaw+boomYawOffset, pitch) * distance。
  - 阻尼：帧率无关指数阻尼 `factor = exp(-k*dt)`（k = stiffness），位置与 boomYaw 独立系数（UE 位置/旋转 lag 分离口径）；`boomYawOffset` 带 auto-return（无鼠标输入按 rate 衰减回 0；orbit 模式不衰减）。
  - look-ahead：观察点 = targetPos + targetVel * leadTime（clamp 上限）。
  - 模式 `enum Mode { Chase, Orbit }`：C 边沿切换，切换帧位姿**连续无跳变**（锚）。
  - **驾驶变体**（低成本，驾驶游戏标配）：`reverseView` 开关（targetVel.yaw 反向段自动切后视，实现 = 视角 yaw +180）+ 回头键（按住瞬切后视、松开回正，同款机制）——viewer 侧接线放 M-CAM2。
- **FreeCamera**（`nl::core`）：
  - 状态：eye 位置 + yaw/pitch（欧拉，clamp 俯仰）+ fov；无阻尼直出（调试相机跟手优先）。
  - 输入 `update(dt, moveInput[3](xyz 平移轴 ±1), lookDelta[2](yaw/pitch 增量), boost)`——键位语义由外部映射层翻译，相机只吃归一化轴。
  - 移动 = eye 前向/右向基底 × 平移轴 × speed×(boost?kBoost:1)；speed 档位 console 可调。
- `cam-tools` 新工具 exe（照 input-tools 三步：main.cpp → 目录 CMakeLists → 根 add_subdirectory 一行；只链 nl_core glfw 同款口径）：
  - selftest 节：机械断言（初始化/clamp，照 cameraSelfTest 口径）+ Chase 锚场景 A~D + Free 锚场景 E（见 §5 表）。
  - `cam-trace` / `cam-check` 子命令：脚本输入 → per-frame 位姿 TSV dump → 数值锚重验闭环。**锚从实测 log 反推（禁手写锚，M-I0/I1 两连犯教训）**。

### M-CAM1 自由相机可见

- viewer 接线：M-D3 落的输入管线消费（spectator set：移动键轴 + look 增量 + boost）→ GameClock 逻辑帧 `freeCamera.update(...)` → 相机矩阵替换渲染投影（OrbitCamera 路径不动，开关位随场景模式）。
- HUD 相机行（ASCII，面板高 ≤560 纪律）：`CAM free eye=(x,y,z) yaw=NN.N spd=N.N`。
- console cvar：`cam.free.speed` / `cam.free.boost`（照 `cam.fov` onChanged 回写先例）。
- example 卡 + cases.json（脚本注入飞行路径 → 截图锚帧，pair 双后端 PNG 基线 + `fc /b`，照 input-m2 先例）。

### M-CAM2 跟车可见

- viewer 接线（drive M-D3 训练场之上）：渲染帧轮询 M-I1 映射（`camToggle` 边沿 / `look` Vec2）→ GameClock 逻辑帧 `chaseCamera.update(dt, chassisPos, yaw, vel)`（M-D2 回读）→ 训练场模式渲染投影走 ChaseCamera。
- HUD 行：`CAM chase eye=(x,y,z) yaw=NN.N d=NN.N`；console cvar：`cam.chase.dist` / `cam.chase.height` / `cam.chase.stiffness` / `cam.chase.leadTime`。
- example 卡 + cases.json：`--drive-demo` 帧序号脚本扩展相机断言帧（pair 双后端基线）。

### M-CAM3 轨迹相机

- **轨迹文件格式**（TSV，与 trace/check 同族自洽）：首行 `# t x y z yaw pitch [fov]` 注释头声明列与帧率无关的时间轴；keyframe 逐行时间升序；`# loop` / `# interp linear|smooth` 指令行声明播放语义。
- **静态机位免费**：单 keyframe + loop = 固定机位（交通路口监控视角、展示台定点镜头），零新代码——交通调度演示（GAME-DEV-ORDER 第 8 步）直接受益。
- **`CinematicCamera`**：文件加载 + keyframe 二分查找 + 分段插值（Linear / smoothstep 起步，逐位确定；Catmull-Rom 样条 §4 后置）+ 播放控制（play/pause/loop/倍速，逻辑帧时间轴推进）；到端点行为（clamp / wrap）显式模式。
- cam-tools 扩节：keyframe 端点精确复现锚（t=keyframe 时刻位姿逐位 = keyframe 值）、中段插值逐位锚、loop wrap 连续性锚、pause/倍速时序锚；`cam-track-trace/check` 子命令。
- viewer 接线：`--camera-track <file>` 播放 + HUD 行 + **截图锚帧**（轨迹确定性 → 双后端基线直接受益；展示台后续运镜片段的机位工具）。
- example 卡 + cases.json：轨迹文件 + pair 双后端基线。

## 4. 候选池（盘过面、给推荐处置、待拍板）

| 项 | 判断 | 推荐处置 |
|---|---|---|
| **Hood / 驾驶舱第一人称** | Forza/GTA 标配，但 demo 阶段没有车壳资产——第一人称里只有引擎盖线条，体验空心 | **后置**到车壳资产落地（第 5 步游戏壳之后）；实现上就是 ChaseCamera 的零距离退化 + 贴车刚体位姿，届时是小活 |
| **Replay / ghost 回放** | 是**状态录制系统**不是相机（相机只是消费端）——录全局状态快照 + 时间轴倒退，成本一个量级 | 不进本轨；第 11 步后视需求（圈速 ghost 有真实需求再立项） |
| **后视镜（真实镜像 viewport）** | 镜像渲染 = 第二个 render pass，渲染管线成本翻倍级 | **不做**；HUD 小画面示意（不渲染镜像）够用且便宜，真要做随第 11 步打磨波 |
| **Photo mode** | 不是新相机——= FreeCamera + 世界暂停语义 + 可选机位参数面板 | 等第 5 步游戏壳 pause 语义落地后是**整合活不是开发活**；暂不立项 |

## 5. 不在范围

- **相机碰撞探针**（SpringArm bDoCollisionTest：boom raycast 防穿墙）——城市生成（第 6 步）前没有楼可穿，后置到城市落地后（届时消费 `nl/physics` raycast 面）。
- Chase 之外的自由相机阻尼（Free 要跟手，无 lag 是特性不是缺陷）；平滑高级模型（critically damped spring 二阶、Snap 语义）——指数阻尼够第 4 步验收，实测不够再升。
- 轨迹样条插值（Catmull-Rom / Bezier）、easing 曲线库、轨道编辑器（只做文本 keyframe 手写 + trace 工具校验）。
- 震动（screen shake）、fov 动态（速度感 zoom，第 11 步打磨波）。
- look-ahead 高级形状（速度圆弧/漂移侧偏——Forza 风格），一阶线性 lookAhead 够第 4 步验收。
- CameraManager 框架（CameraRig 基座共享数据口径即可；场景模式开关够用）。

## 6. 里程碑草案

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M-CAM0** | `ChaseCamera` + `FreeCamera` 类 + `cam-tools` selftest 五锚场景 + trace→check 闭环 + ctest | 五锚全绿：**A 直线跟随**（目标匀速直线脚本，稳态 eye 距离/高度/偏航误差 ≤ ε）；**B 急转衰减**（boomYaw/视线残差 per-frame 衰减序列）；**C 模式切换**（C 边沿位姿连续零跳变 + orbit clamp）；**D 帧率无关**（dt=1/60 vs 1/30 各自逐位，跨 dt N 秒后 ≤ ε 阈值——分档显式）；**E 自由飞行**（脚本移动轴/look 增量 → eye/yaw/pitch 逐位序列 + clamp 断言）；负向退出码 0/1/2；双后端 spot 零回归（cameraSelfTest 不动）；tool 卡 |
| **M-CAM1** | viewer FreeCamera 接线 + spectator set 增量 + HUD 行 + cvar + example 卡 + cases.json | 截图基线双后端 bytes；键鼠飞行肉眼可验；`OrbitCamera` 路径零回归（spot 不动）；`server.py --check` exit 0 |
| **M-CAM2** | viewer 训练场 ChaseCamera 接线 + C/鼠标交互 + HUD 行 + cvar + `--drive-demo` 扩展 + 收口（close 硬闸） | 截图基线双后端 bytes；跟车/切模式肉眼可验；console 实调参数生效锚落盘；**前置硬闸 = drive M-D3 已收口** |
| **M-CAM3** | `CinematicCamera` + 轨迹文件格式 + cam-tools 锚节 + viewer 播放 + 截图锚帧 + 收口 | 轨迹锚全绿（端点逐位/插值逐位/wrap 连续/pause 倍速时序）；轨迹文件驱动双后端截图基线 bytes 一致；`--camera-track` demo 卡落 cases.json |

## 7. 留痕

- 2026-11-10：立项出计划（用户指令「玩法相机的也出个文档」）；初版只有 Chase 双里程碑，
  用户复核「不止这两种相机吧——比如纯自由相机、或者轨迹文件控制的相机」后扩为相机族三件套；
  用户再复核「还有其他的么 不要老是我说啥就是啥」→ 主动盘全景：**增量采纳**（倒车/回头变体
  进 M-CAM2、静态机位免费进 M-CAM3、CameraRig 轻量基座替代纯复制）、**候选池四项**（Hood 后置 /
  Replay 不进本轨 / 后视镜不做 / Photo mode 整合活）。现状实证 grep 留痕：
  core 相机面仅 `camera.hpp` OrbitCamera；M-I1 `action_map.hpp:126/131/132` 已备 `camToggle`
  （C 边沿）+ `look`（Vec2 additive）；M-D2 回读（chassis pos/yaw/velocity）是 Chase 的
  viewer 数据源；轨迹相机 TSV 与 trace/check 工具链同族自洽。
- 开工排序：M-CAM0 零依赖可先行/与 drive 并行；M-CAM1~CAM3 统一在 drive M-D3 之后串行
  （viewer/main.cpp 同文件冲突面一次清 + 输入管线复用 M-D3 落的接线）。track `camera`
  四里程碑，merge 一次收口。
