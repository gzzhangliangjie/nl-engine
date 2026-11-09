# P8-GAMELOOP-PLAN — 游戏循环 / 双帧模型（渲染帧 vs 逻辑帧，A 类，2026-11-09 立项登记）

> 起因：2026-11-09 用户问「支持游戏暂停，现有框架能做到么」→ 讨论中拍板**需要有渲染帧和逻辑帧的概念**，三点方案（双帧模型 / 起步不插值 / 与 M-S3 合并落点）用户全部同意，本文档即「固化登记」产物。
> **性质**：A 类并行轨道（不碰 `kStage`、不进渲染回归），走工具版交代（selftest / console 命令 / 展示台 tool 卡）。
> **Gate**：E1b 链 merge 回 master 之后才动工；M-GL2（物理/音频/ECS 接逻辑帧）与 **M-S3 场景集成合并收口**（见 `P8-AUDIO-PLAN.md` M-S3 行，已互相挂前置）。

## 1. 目标

给引擎引入**双帧模型**，作为 game loop 层第一次落地：

| 帧 | 驱动 | 步长 | 职责 |
|---|---|---|---|
| **逻辑帧（sim tick）** | accumulator | **固定 60Hz**（`1.0/60.0`） | input → 脚本/动作 → 物理（Jolt step）→ ECS transform 同步 → 音频 listener/声源同步；暂停 = 逻辑帧时钟停摆 |
| **渲染帧** | vsync / 主循环 | 变步长 | present、相机漫游、HUD/console、GPU 资源上传；暂停时**继续跑** |

**暂停语义**：`paused=true` → 逻辑帧不推进（simTime 不变），渲染帧照常——画面可见、可漫游、可调 console，游戏状态冻结。这就是用户原始问题「游戏暂停」的实现载体。

## 2. 三处参考口径（2026-11-09 实读；各里程碑开工按铁律重读点名文件）

| 来源 | 实读证据 | 用法 |
|---|---|---|
| **Carrot** `engine/engine/physics/PhysicsSystem.{h,cpp}`（实读） | `TimeStep = 1.0/60.0` 常量；`accumulator += deltaTime; while(accumulator >= TimeStep) { jolt->Update(TimeStep, collisionSteps, …); accumulator -= TimeStep; }`（cpp:130-138） | **accumulator 模式直接同构**：固定步长 + while 循环；⚠️ 它只在物理侧用，本项目推广为全局 sim clock（引擎级而非单系统级） |
| **bevy** `bevy_time/src/virt.rs`（实读） | `Time<Fixed>` 与 `Time<Virtual>` 分离；`DEFAULT_MAX_DELTA = 250ms` 防死亡螺旋（`max_delta` 双重用途：大 delta 截断 + 长卡顿追赶上限）；`FixedUpdate` 调度与渲染 `Update` 解耦；暂停/倍速走 Virtual 时钟 | **max_delta=250ms 口径照抄**；Virtual 时钟的 `paused`/`relative_speed` 语义即本项目 timeSource 设计的蓝本 |
| **UE** `Runtime/Engine/Private/`（**本轮未实读**，按已知结构：Tick 每帧变 dt + TickGroup 排序 + 物理异步 substepping 固定步长补；`FApp::SetFixedFrameRate` 用于固定帧率模式） | 工业终态走「渲染帧为主 + 物理 substepping」，与 Carrot/bevy 的「逻辑帧为主」是两种流派 | M-GL0 开工时按铁律实读 `GameEngine.cpp` tick 与 `FApp` 相关文件再定论；当前拍板**选逻辑帧为主**（与确定性回放契约同向，见 §4） |

**历史决策对齐**：本项目「确定性回放 = 固定样本 + 固定 dt，PNG MD5 与基线逐字节对上」（viewer/main.cpp:843 注释）——逻辑帧恒定步长**强化**该契约而非冲突；Jolt「逐样本位一致」契约（M-P0）在固定步长下天然满足，不动。

## 3. 设计

- **`nl::core::GameClock`**（薄层，纯逻辑，零依赖）：`simTime / frameDt(缩放后) / timeScale / paused / accumulator`；`tick(realDtMs)` 内部做 `scaledDt → 截断 maxDelta(250ms) → accumulator 累加 → while 跑 logicTick(step)`。逻辑帧步长常量 `1.0/60.0`（对齐 Carrot）。
- **主循环改造**（viewer/main.cpp）：渲染帧开头 `clock.tick(realDt)`，`skinTime` 改读 `clock.simTime()`；`--anim-time` 固定值模式语义不变（等价 paused 固定 simTime）。
- **console 命令**（复用 M-C0~C3 基建）：`game.pause` / `game.timeScale`（CVar，`setOnChanged` 生效）。
- **逻辑帧 tick 顺序**（对齐 UE TickGroup 思想做简化，单线程起步，Scheduler 池化不变）：input → script/action → physics → ECS transform sync → audio sync。
- **起步不插值**（2026-11-09 用户拍板，最小必要方案）：渲染帧直接取最新逻辑帧状态；60Hz 逻辑 + 高刷屏的 judder 可接受。bevy 有 interpolator、Carrot/UE 不插值——将来要了再补 `alpha = accumulator/step` 的 transform 插值，GameClock 已预留 alpha 输出。

## 4. 不在范围

- 多逻辑帧速率分档（TickGroup 精细化）、帧间插值、回滚 netcode、多线程逻辑帧并行（Scheduler 已有 DAG，将来 M-GL2 再评估）。

## 5. 里程碑草案（不排期，gate E1b merge 后开工）

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M-GL0** | `GameClock` 纯逻辑 + selftest（累加/截断/暂停/倍速/追帧上限） | selftest 全绿；tool 卡 usage/正向/负向 + 证据图 |
| **M-GL1** | viewer 主循环接双帧 + `game.pause`/`game.timeScale` + `--fixed-dt` 兼容 | 截图基线：暂停帧 simTime 不变、渲染帧 HUD 活；双后端对拍 |
| **M-GL2** | 物理/音频/ECS 接逻辑帧 tick（与 **M-S3 合并收口**） | 声源随物体移动且暂停即停；全量回归 + 收口硬闸 |

## 6. 留痕

- 2026-11-09：立项登记（用户拍板三点：双帧模型 / 起步不插值 / 与 M-S3 合并落点）。点光源阴影同日拍板**不排期**（见 `.workbuddy/memory/2026-11-09.md`）。
- 2026-11-10：**M-GL0 收口（tag `gl-m0`，feat/gameloop 轨道首里程碑）**。UE 侧按铁律补实读：`UnrealEngine.cpp FEngineLoop::Tick`（3020-3160）——证实「渲染帧为主」流派，维持逻辑帧为主拍板。落码 `nl::core::GameClock`（kStep=1/60 / kMaxDelta=250ms / 暂停=bevy 时钟停摆语义——**拍板记录**：非 Carrot 烧 accumulator，语义是「时钟停摆」非「丢帧」/ timeScale∈[0,1024] / alpha 预留）+ `gl-tools`（selftest 30 锚点 + trace TSV）+ verify.py 独立 python 孪生逐行对拍全等 + example/gl0 工具版交代全套。详见 MILESTONES M-GL0 四栏与 `GAMELOOP-TOOLS-GUIDE.md`。
- 2026-11-10：**M-GL1 收口（tag `gl-m1`）**。viewer 主循环接双帧：runNormal skin 模式非 `--anim-time` 路径改走 `GameClock::tick`（渲染帧喂 dt → 逻辑帧 60Hz 消费 → simTime 量化不插值）；`--anim-time` 兼容路径原样保留（绕过时钟，既有基线逐字节零风险）。控制台命令 `game.pause`（无参翻转；暂停=bevy 时钟停摆——画面冻结、HUD/相机照常）/ `game.timeScale`（[0,1024] 钳制）/ `game.status`（截图日志锚）；新增 `--fixed-dt <ms>` 钉渲染帧 delta（对拍确定性）。验收：暂停冻结三重证（pause120==pause60==anim0，双后端逐字节）+ 倍速轨迹等价证（16.7×2 与 33.4×1 每帧馈入逐位相同→PNG 逐字节相等）。**拍板记录**：ECS scene tick / 物理叠加层 `step(1/60)` 统一迁入 GameClock 属 **M-GL2**（本站不动）——迁移路径：受影响用例加 `--fixed-dt 16.7` 后总 tick 数不变（floor(240×16.7/16.667)=240）、终态轨迹一致、基线大概率零重拍。
- 2026-11-10：**新知沉淀（viewer 截图捕获口径）**：capturePath 判定（`frame + 1 == o.frames`）在 `++frame` 之前求值、持上一迭代值 → PNG 实际落在**最后一个迭代**（`--frames N` 跑 N 次迭代、第 N 次出图）；终帧日志必须用 `frame == o.frames`（自增后语境）才能与 PNG 同帧。另：**240 tick = 4.0s 恰好 wrap 回 t0 姿态**（CesiumMan Walk 2.0s 循环 + 末键≈首键）——倍速用例取 100 帧（200 tick = 3.3333s 落周期中部）避开巧合等值。
- 2026-11-10：**M-GL2 收口（tag `gl-m2`）**。物理调试 step / ECS world.tick / 音频场景同步全部收进 GameClock 逻辑帧 callback（runNormal 与 runScene 同构）；runScene 补落 `--fixed-dt`；新增 `audio.play <wav> <entity>`（M-S3：声源挂 ECS 实体、listener 随相机、逻辑帧同步、暂停即停）。**拍板记录（用户选 B）**：`game.pause` 连音频播放一起冻（UE 流派，复用 audio.freeze 机制）；bevy「只停同步」流派被否。迁移零重拍：受影响用例加 `--fixed-dt 16.7` 后总 tick 数不变，ecs-parallel/phys-p3/phys-p4/gl1 基线全部逐字节复现。验收证据：gl2-audio.scene 轨道子实体（E1a 层级）上声源读回 == 实体世界位置（轨道半径不变量 + movement + freeze + 跨后端逐位，verify.py 独立孪生 12 检查 × 2 后端）。游戏循环双帧模型至此完整落地（全消费方接入）。
- 2026-11-10：**新知（viewer 截图捕获口径补遗）**：phys-debug 的 settled 日志此前沿用 `frame + 1 == o.frames`（pre-increment 语境残留），与 PNG 实际落点差一迭代，M-GL2 起统一为 `frame == o.frames`（自增后语境）。另：场景 deserialise 严格拒绝重复实体 id（gl2 场景 id 7 撞 spin 场景即被拒）——手工拼场景 JSON 时 nextID 必须与已用 id 对账。
