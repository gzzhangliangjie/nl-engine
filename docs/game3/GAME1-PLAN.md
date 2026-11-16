# GAME3 GAME1-PLAN — M-GAME1：AI 车流进 game.exe（2026-11-17 立项）

> **交接文档**：本文档自包含，新对话零上下文可直接落码。工作树
> `E:/UE_STUDY_2026/nl-engine-game`（分支 `feat/game`），里程碑 M-GAME0 已收官
> （tag `game-m0` @ `d3d1743c`，已推远端；主仓 master merge `d66c2648`）。
> 轨道纪律见根 `AGENTS.md`；台账 `docs/MILESTONES.md`；wt_pool `tools/wt_pool.py`
> （game 轨 active，wt 不移除，M-GAME1 接着跑）。

## 0. 已拍板（2026-11-17 AskUserQuestion，开工闸已清）

| 问 | 结论 |
|---|---|
| 第 8 步实质方向 | **A. AI 车流进 game.exe**（原 GAME-DEV-ORDER 第 8 步交通 M-T0~T4 已收官，状态列已勘误；本步 = 交通轨成果接进驾驶闭环） |
| 轨道归属 | **feat/game 继续**（wt 不移除，M-GAME1 在 game.exe 上增量） |
| AI 车碰撞边界 | **AI 车为运动学 sim（不进 Jolt），M-GAME1 内玩家车与 AI 车不互撞（穿过）**；AI 车体物理化挂 M-GAME2+ |
| 范围 | viewer `--traffic-city` 同口径全套：car_* 挂组件 + TrafficCarSystem + 红绿灯 initControl + SignalLamp 摆件 |

## 1. 落码坐标（全部实证过，行号为当前 HEAD `b6ed5d6b`）

### 1.1 viewer 参照段（照抄口径，勿改 viewer）

- `viewer/main.cpp` L5523~L5645：`--traffic-city` 初始化——car_* 前缀实体挂
  `nl::scene::TrafficCarComponent` → `world.flushEntityCreationAndRemoval()` →
  `registerSystem(TrafficCarSystem)` → `findSystem(kName)` → 读 `roadnet.json` →
  `buildLaneGraphFromRoadnetJson` → `init(laneGraph, simParams.loop=true, seed)` →
  `initControl(json, true)`（红绿灯相位，失败 exit 2）→ SignalLampComponent 红绿灯
  摆件（停车线右侧路缘外 1.2m，+Z 面板向来车，`atan2(-fx,-fz)*0.5f` 四元数 yaw）。
- `viewer/main.cpp` L6395~6405：确定性钉步——`--traffic-steps N` = 每渲染帧强制
  `gameClock.tick(GameClock::kStep*1000.0)` N 次（真实 dt 抖动会产出不定步数，
  双跑 PNG 无法逐字节对拍）。
- `viewer/main.cpp` L6406~6416：traffic 日志锚逐帧行
  `traffic-city: frame=%llu ticks=%llu odometer=%.2f m arrived=%d`。

### 1.2 game/main.cpp 现状结构（本步全部 additive，不动既有函数）

```
L64   struct Options        # 加 bool traffic = true（--traffic 0|1）
L101  parseArgs             # 加 --traffic 解析（atoi，非 0/1 报 exit 2）
L158  runSelftest           # 不动（M-GAME0 锚保形）
L222  struct CityScene      # world/ECS 在这里；加 std::vector<int> carObjects
                            #  （car_* 实体 → objects 索引映射，照 playerObject 先例）
L233  loadCity              # 收集 objects 时记录 car 实体 id→索引
L315  addCityCollision      # 不动
L393  struct Feedback       # 不动
L432  runGame               # 主接线点
L560  GameClock+TickScheduler（4 组 lambda：PrePhysics=输入 / Physics=phys.step /
      PostPhysics=撞击+相机 / PostUpdateWork=trauma）——【注意】game 无 world.tick()，
      ECS 是启动快照口径，车流必须显式驱动（见 §2 方案）
L605~ 渲染循环：每帧玩家车直接改 objects[playerObject].world + updateEcsObjects 全量上传
      （AI 车矩阵同步照同款，见 §2）
```

### 1.3 TrafficCarSystem 接口（core/include/nl/traffic/）

- `TrafficCarSystem.hpp`：`init(laneGraph, TrafficSimParams, seed, &err)` /
  `initControl(json, jsonLen, createLamps, &err)` / `tick(double dt) override` /
  统计 `stepCount()` `totalOdometer()` `arrivedCount()`（loop=true 恒 0）。
- `LaneGraph.hpp` / `TrafficControl.hpp`（NodeControl.groups[2] 每进入边一盏灯）。
- 头注铁律：必须在「实体已挂 TrafficCarComponent + World flush + registerSystem」之后 init。
- cg2-m0 产物有 **8 个 car_00~car_07 实体**（flat glb，已在 city.scene.engine）。

## 2. 接线方案（game.exe 特有差异，关键决策已定）

**差异**：viewer 的 GameClock tick 内走 `world.tick()`（ECS System 自动驱动）；
game.exe 的 TickScheduler 是自建 4 组 lambda、ECS 仅启动快照、渲染对象 = `city.objects`
数组（每帧只有玩家车直写矩阵 + `updateEcsObjects` 全量上传）。

**方案（已定，勿再拍板）**：
1. runGame 内 loadCity 之后、物理 build 之后：照 viewer L5523 段挂组件 + flush +
   registerSystem + init + initControl（红绿灯摆件 newEntity 的 signal_* 实体**必须在
   loadCity 收集 objects 之前建好**，或收集后追加 SceneObjectDesc——落码时二选一，
   注意 signal_* 无 glb mesh，需查 viewer 灯的渲染路径：SignalLampComponent 如何进
   SceneObjectDesc / 或 viewer 用专用画法；**这是本步唯一需要现场调研的点**）。
2. TickScheduler **PostPhysics 组末尾**追加 `world.tick(nl::core::GameClock::kStep)`——
   TrafficCarSystem 经 ECS tick 驱动（组序仍为枚举序，红绿灯 sim 与玩家物理同帧推进，
   先物理后交通，避免玩家车读到上帧车位姿）。
3. 渲染循环：car 实体位姿同步——逐 carObjects[i] 读
   `TransformComponent::getWorldMatrix()` 写 `objects[idx].world`，与玩家车合并一次
   `updateEcsObjects`（既有调用只上传全量数组，直接扩列即可）。
4. 确定性：`--traffic-seed` 缺省 12345（viewer 同款）；基线跑钉
   `--steps 240 --frames 16`（stepsPerFrame=15 逻辑步/帧），车流 odometer 双后端逐位一致。
5. 日志锚：初始化后 `game: traffic ready: cars=8 nodes=… signal nodes=…`；逐帧
   `traffic: frame=N odometer=… arrived=0`（照 viewer 口径）；尾部汇总行并入
   `game-m1: …`。

## 3. 验收判据

1. 双后端 BUILD OK（`scripts/wt.bat game build`，分钟级**必后台**+尾写 bg-last.log）；
2. `game.exe --city example/cg2-m0 --seed 1`（车流缺省开）双后端 rc=0 逐位一致：
   既有 GAME_IMPACT 锚不回归（strength=4.7787 impulse=7167.9883 owner=113）+ 新
   traffic odometer 逐位同；
3. 基线截图 `example/game-m1/game-street.png` / `-VULKAN.png` 非空且达 viewer 同级可见
   口径（灯色点+楼剪影；CG2 夜景暗城+km 级稀疏车体，"肉眼可见车流"不可达属数据现实——
   **车体可见性立项 M-GAME2**：尾灯自发光/近景光照，本步以灯色点+数值锚验收）；
   双跑 PNG 逐字节一致（python sha256 对拍，Git Bash `fc` 是假绿勿用）；
4. cases.json 追加 **game-m1 卡**（python json `indent=1`+尾换行+`newline='\n'`；
   image 卡 baselines 双后端分文件：DX 无后缀/VK 带 `-VULKAN`；expectLog 锚以实测为准）；
5. `scripts/wt.bat game regression --only game-m1` 双后端 ALL GREEN；
   `--only game-m0` 6/6 不回归（--traffic 0 或缺省口径需保证 M-GAME0 基线字节不动——
   **注意**：M-GAME0 基线跑的是无车流口径，若 --traffic 缺省=1 则 M-GAME0 基线会变；
   处理：M-GAME0 卡的 street case args 显式加 `--traffic 0`，或 game-m1 缺省 0 由
   显式 `--traffic 1` 激活——**推荐后者**，M-GAME0 基线零改）；
6. ctest 双后端全绿（game-m0-selftest 既有用例不动）。

## 4. 环境坑（本仓血泪清单，开工前必读）

- **构建/回归只准后台**：分钟级命令一律 `run_in_background` + 尾写 `bg-last.log`，
  完成通知到达立即查结果报数；每条 shell 命令开头 `cd <树> && pwd`（cwd 不跨命令）。
- **NvOptimus 导出已带**（game/main.cpp 头部 L1~L20 区），勿删——删了 exe 又落 Intel
  核显崩（nvrhi vk dispatcher 空槽 +0x5b8）。
- **改 .slang 不涉及**（本步零 shader 改动）。
- **混合 obj 布局**：本步不改任何 struct 布局类头文件（core 只加 TrafficCarSystem 接线，
  不动 World.hpp/ImpactState）；若意外动了布局头，必删 build 下全部 .obj 全量重编。
- **cg1-m0 fixture `buildings/box.obj` 是 untracked 本地产物**：若 cg2-m0 selftest 报
  `--src missing buildings/box.obj`，重跑
  `build/DX12/citygen.exe export --input modules/citygen/test/zjx-sample.json --buildings
  modules/citygen/test/zjx-buildings-sample.json --bbox 23.108,113.316,23.130,113.342
  --landmarks assets/citygen/landmark_whitelist.json --out example/cg1-m0` 即愈
  （锚：buildings=150 landmarks=1 suppressed=0 outside=1 / bldPx=263673 / lmPx=256）。
- **push 前代理端口漂移**：`echo $http_proxy` 核对端口，重写
  `git config core.sshCommand "ssh -p 443 -o ProxyCommand='C:/Users/zlj10/bin/connect.exe -H 127.0.0.1:<PORT> %h %p'"`。
- **cases.json 追加配方**：python json `indent=1` + 尾换行 + `newline='\n'`（勿手编）。
- 负向锚口径：scene 不存在 = resolve 期 exit 2（`game: scene not found`）。

## 5. 收尾链（落码+验证全绿后）

1. `docs/game3/GAME1-PLAN.md` 补收官证据段；
2. `docs/MILESTONES.md` 追加 M-GAME1 四栏台账；
3. commit（feat/game）→ tag `game-m1` → push（分支+tag）；
4. `python tools/wt_pool.py close --track game --milestone M-GAME1 --tag game-m1 --version game-m1`
   → 填 close report 待填格 → `release --done`；
5. 主仓 `E:/UE_STUDY_2026/nl-engine` master merge feat/game + push。

## 6. 收官证据（2026-11-17 补录）

- **落点**：`game/main.cpp` 全部 additive——Options(--traffic/--traffic-seed/--cam/--cam-at)、
  CityScene(carEntities/carObjects/lampEntities/lampObjects)、loadCity(meshFiles.reserve(256)
  防悬垂 + car_* 实体映射)、setupTraffic(挂组件/flush/registerSystem/roadnet→lane graph/
  init loop=true/initControl/401 盏灯 newEntity+registerLamp/灯 desc 收集)、
  PostPhysics 末尾 world.tick(kStep)、渲染循环 car 矩阵减 cityCenter 同步 + lamp tint
  同步 + updateEcsObjects、traffic ready/逐帧/尾行三段日志锚。
- **构建**：双后端 BUILD OK；ctest 184/184 双后端。
- **双后端基线逐位一致**：`traffic ready: cars=8 lanes=500 signal lamps=401 seed=12345`；
  `GAME_IMPACT strength=4.7787 impulse=7167.9883 owner=113`（M-GAME0 锚零回归）；
  `game-m1: traffic cars=8 lamps=401 ticks=240 odometer=128.53 arrived=0`；
  基线 PNG dx==vk==dup sha256 相同 = `4918ff3669fb98fb…`。
- **run.bat 六段** ALL GREEN（selftest/DX/VK/dup/负向/M-GAME0 无车流回归锚
  `objects=160 boxes=150`）；regression `--only game-m1` 6/6 + `--only game-m0` 6/6
  bytes ALL GREEN；cases.json 173 卡（game-m1 卡 selftest/street-traffic/negative）。
- **实现差异记录**：验收判据 3 勘误——CG2 夜景暗城 + km 级稀疏车体，"肉眼可见车流"
  不可达，改为"灯色点可见（cg2-m0 viewer 同级口径）+ 数值锚"；车体可见性（尾灯
  自发光/近景光照）立项 M-GAME2。--cam free/--cam-at 为本步新增验证取景 CLI
  （缺省 chase 保 M-GAME0 基线字节）。
