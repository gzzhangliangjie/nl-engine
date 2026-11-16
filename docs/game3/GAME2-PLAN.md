# M-GAME2 PLAN — AI 车体物理化（kinematic body + 玩家撞击事件）

> 轨道：game（feat/game 继续，wt `E:/UE_STUDY_2026/nl-engine-game`）。
> 前置：M-GAME0（tag `game-m0`）/ M-GAME1（tag `game-m1`）均已 RELEASED。

## 1. 三处证据（2026-11-17 实读）

1. **GAME-DEV-ORDER / 用户拍板**：三候选（A 车体物理化 / B 尾灯自发光 / C 手感实调）→
   用户拍板 **A. AI 车体物理化**（B/C 顺延 M-GAME3+）。
2. **core/src/physics/World.cpp**：
   - `SceneContactCounter`（L1260） additive 状态 = `cityOwners` + `chassisRaw` 白名单；
     撞击判定要求"city box ↔ chassis"，非 box 对在 `if (!box || !car) return;` 丢弃 →
     **AI 车接触当前不可见**。
   - `addCityBoxes`（L~1850）模式：批量 CreateAndAddBody + owners map + OptimizeBroadPhase；
     `clearCityBoxes` = RemoveBody+DestroyBody（本 Jolt 无 IsBodyAdded 守卫）。
   - `addSlot`（L1401）：chassis 由 `buildVehicle` 创建（dynamic、有 VehicleTuning）。
3. **core/src/traffic/TrafficCarSystem.cpp**：`tick → sim_.step → writeBack`：
   `sim_.carPose(ci,&p,&yaw)` 写 TransformComponent（position.y=0，rotation=yaw 四元数）
   + 灯 tint。game 逐帧读 ECS transform 即位姿源，无需改 traffic。

## 2. 冻结面与拍板

- **core 只加不改**（M-GAME0 放宽条款延续）：新增 additive API + counter additive 分支，
  不动既有 struct 布局语义（ImpactState 不加字段——AI 撞击复用同一事件通道）。
- **traffic 轨零改**：TrafficCarSystem/TrafficSim 一行不动。
- **viewer 基线零变动**；city.scene/roadnet schema 零改。
- 拍板沿用：AI 车为 **Jolt kinematic body**（EMotionType::Kinematic，无限质量，
  不被玩家推走，玩家撞上去有接触事件+反弹）；物理化只在 game 轨消费，viewer 不接。

## 3. 设计

### 3.1 core additive API（World.hpp / World.cpp，全 additive）

```cpp
// World.hpp DebugScene 新增（addCityBoxes 同款批量口径）：
int  addKinematicCars(const float positions[][3], const float yaws[],
                      const float halfExtents[][3], const std::uint64_t owners[],
                      int count, int layer);
void updateKinematicCarPoses(const float positions[][3], const float yaws[], int count);
void clearKinematicCars();
int  kinematicCarCount() const;
```

- 创建：`EMotionType::Kinematic` + box shape + `EActivation::Activate`；bodyId→owner
  进 counter 新 map `aiOwners`；body raw id 集 `aiCarRaw`（白名单语义同 chassisRaw）。
- 逐帧驱动：`bi.MoveKinematic(id, targetPos, targetRot, dt)`（Jolt kinematic 官方口径，
  速度 = (target-cur)/dt，单步精确到达 → 确定性成立）；dt = GameClock::kStep。
- 清理：shutdown 清单（M-CRASH0 UAF 纪律）——cityBoxIds 同款，先 Remove 后 Destroy，
  析构顺序在 physicsSystem 之前。

### 3.2 撞击判定 additive 分支（SceneContactCounter::OnContactAdded）

在 `if (!box || !car) return;` 之前插入：一方 ∈ chassisRaw 且另一方 ∈ aiCarRaw →
记入既有 cityHit 通道（owner=AI 车 entity id、strength=玩家底盘水平速度、
impulse=speed/invMass 同款代理）。AI↔AI、AI↔city box 不记（玩家不在场）。

### 3.3 game.exe additive（game/main.cpp）

- `--kinematic 1`（缺省 0）：显式激活，M-GAME0/M-GAME1 基线字节保形。
- setupTraffic 之后：读 car_* TransformComponent 位姿 → `addKinematicCars`
  （halfExtents ≈ (1.1, 0.8, 2.4) sedan 包围盒口径，layer 与 city box 同层）。
- PostPhysics `world.tick(kStep)` 之后：重读 transforms → `updateKinematicCarPoses`。
- shutdown/退出路径：`clearKinematicCars()`。
- 日志锚：`game-m2: kinematic cars=8 ... impacts=N`；GAME_IMPACT 行 owner=<AI entity id>。

### 3.4 ObjectLayer

kinematic 车与 city box 同 layer（M-GAME0 传参先例）——玩家 chassis/城市盒/AI 车同层
互撞，BP 矩阵无需扩层。落码时以 World.cpp ObjectLayers 实况为准核对。

## 4. 验收判据

1. selftest（core 级，受控场景）：z=8 玩家撞 kinematic 盒 → impact owner=<AI id>、
   strength>0、clear 归零、双跑逐位；既有 wall/city 场景锚零回归。
2. phys-tools：新增 `checkAiImpact`（建 kinematic 盒 + 全油门撞）PASS 双后端。
3. game 基线：`--traffic 1 --kinematic 1` 长跑，找到玩家撞 AI 车的 seed/步数组合
   （AI 车在 lane 上巡游，路径与玩家路径交叉）→ GAME_IMPACT owner=<AI entity id>，
   双后端逐位一致 + 基线 PNG 双后端 sha256 相同。
4. `--kinematic 0`（缺省）：M-GAME0 / M-GAME1 卡 bytes 全部不回归。
5. ctest 双后端全绿（game-m2-selftest 新增进 game/CMakeLists.txt）。
6. 负向：`--kinematic 5` → exit 2（同 --traffic 口径）。

## 5. 环境坑清单（跨里程碑有效，开工必读）

1. **长任务硬闸**：build/ctest/regression 一律后台 + `bg-last.log` 结果锚；完成通知到达
   立即查数报数。每条命令 `cd <树> && pwd` 开头。
2. **NvOptimus 勿删**：game.exe 头部双 GPU 独显导出是 M-GAME0 铁律，任何重构保留。
3. **改 World.hpp 布局类头**：本树全量重编（消费者含 game/main.cpp、phys-tools；
   增量 build 同树自洽，跨树/跨轨不共享 obj）。
4. **cases.json 配方**：python `indent=1` + 尾换行 + `newline='\n'`；expectLog 锚以实测为准。
5. **push 前核对代理端口**：`echo $http_proxy`，重写 `core.sshCommand`（端口每次会话漂移）。
6. **Jolt API 偏差记录**（M-GAME0 实测）：BodyID raw = `GetIndexAndSequenceNumber()`；
   删 body = 直接 `RemoveBody`+`DestroyBody`（无 IsBodyAdded）；invMass 反数=质量。
7. **两套中位数口径**：roadnet 系坐标渲染要减 cityCenter（M-GAME1 踩坑①）；
   kinematic 车的物理位姿是 **物理系**（cityCenter 平移后），与 game 渲染 car 矩阵
   （roadnet 系减 cityCenter）同源——落码时以「物理 = 渲染矩阵的平移部分」对齐。
8. **收尾链**：MILESTONES 四栏 → cases.json 卡 → run.bat 六段 → regression
   `--only game-m2`（+ m0/m1 保形抽查）→ commit/tag `game-m2` → push → close 闸
   → release --done → master merge → memory 沉淀。
