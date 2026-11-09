# P8-PHYS-PLAN — 物理系统（Jolt vendored 集成，A 类并行轨道，2026-11-08 立项）

> 轨道归属：`NON-RENDER-ROADMAP.md` 候选总表「物理（Jolt 集成）」行（★★★）。用户拍板（2026-11-08）：**vendored Jolt（MIT）不自研**——当时记为「总原则的首例例外」；**2026-11-09 四修后按 `AGENTS.md`「自研边界与第三方库选型闸」更正为通例**（关联系统优先成熟库，手写预算留给 nanite/lumen），「例外 / 首例」旧表述作废（2026-11-09 寻路跟进同类拍板，改 vendored Recast/Detour，见 `P8-NAV-PLAN.md`），三处证据（Carrot physics 即 Jolt 集成层 / UE 自研 Chaos 工业终态 / bevy 本体无物理、生态 avian），三条路径均不要求我们自己写求解器。**2026-11-09 勘误**：原写「UE 5.x 官方提供 Jolt 后端」不成立——本机 UE 5.8.4 `Engine/Source` + `Engine/Plugins` 递归扫描无任何 Jolt 文件，官方物理只有 Chaos；差异已落 `docs/s5/UE-DIFFS.md` D10，轨道内 M-P0 已收口（tag `phys-p0`）。
> **执行排期：放到场景整合之后**——物理体 / 碰撞层吃 P8 E1（ECS-lite 场景：多物体 / 变换层级）与 E3（序列化 + Inspector，碰撞体参数要可编辑）。先立项出计划，执行在 P8 E1/E3 落地后启动。
> 届时按 `AGENTS.md` 并行轨道纪律新建 `feat/physics` worktree（junction 共享），验收走**工具版交代**（A 类：不进 `kStage`、不进渲染回归）。

## 性质与路线

- **集成层，不写求解器**：Jolt 提供求解（宽相 / 窄相 / 约束求解 / 积分），我们写的是引擎侧集成——体封装、碰撞层、场景联动、debug 渲染。
- vendored 方式：Jolt 源码进 `thirdparty/jolt/`（MIT，兼容本项目 MIT），构建走 CMake 子目录，遵守「构建链不依赖厂商图形 SDK」约束（Jolt 纯 CPU，无此风险）。
- 参照 Carrot `engine/physics/`（2902 行）的集成切面：`PhysicsSystem`（世界 + 生命周期）/ `PhysicsBody`/`RigidBody`（体封装）/ `Colliders`（形状）/ `CollisionLayers`（层过滤）/ `Character`（角色胶囊）/ `JoltDebugRenderer`（调试可视化）。

## 三处参考口径（2026-11-08 实读，执行各里程碑时按铁律重读本里程碑点名文件）

| 来源 | 内容 | 用法 |
|---|---|---|
| **Carrot** `engine/physics/`（2902 行，Jolt 集成层） | 上述切面结构 + `BodyUserData`（引擎对象 ↔ Jolt 体关联） | **集成方式首选参照**，抄思路不抄代码 |
| **UE** `Runtime/Experimental/Chaos*` + `PhysicsCore/` + `Engine/Classes/PhysicsEngine/` | 工业终态 = **自研 Chaos 求解器**（只读）；2026-11-09 实读全树**无 Jolt 后端**，此前「官方提供 Jolt 后端」为误记已勘误 | 只读对照；Chaos 的耦合面（Scene/Actor + `FBodyInstance`）作集成边界参考；差异见 `UE-DIFFS.md` D10 |
| **bevy** | 本体无物理，生态 avian（parry/collision 库系） | 据实标注，不作口径 |

## 里程碑阶梯（每步可见验收；编号 M-P，执行时按实际情况细化）

| 里程碑 | 内容 | 可见验收点 |
|---|---|---|
| **M-P0** ✅ **2026-11-09 完成（tag `phys-p0`）** | vendored Jolt 5.3.1 + 最小世界（落下小球） | `phys-tools` 无头模拟 dump 轨迹 TSV → 证据图；弹跳曲线符合自由落体+恢复系数。实测见 §M-P0 固化与 `PHYS-TOOLS-GUIDE.md` |
| **M-P1** ✅ **2026-11-09 完成（tag `phys-p1`）** | 体封装 + 碰撞层（引擎对象 ↔ Jolt 双向关联） | 层过滤矩阵用例：同层碰撞 / 异层穿透，dump 可验。实测见 §M-P1 固化口径与 `PHYS-TOOLS-GUIDE.md` §8 |
| **M-P2** ✅ **2026-11-09 完成（tag `phys-p2`）** | 堆叠 + 地面（静态体） | 箱体堆叠稳定不抖（长时间步进位移收敛），dump 证据。实测见 §M-P2 固化口径与 `PHYS-TOOLS-GUIDE.md` §9 |
| **M-P3** ✅ **2026-11-09 完成（tag `phys-p3`）** | debug 渲染接 viewer（Jolt DebugRendering → ImGui/线框） | `viewer --physics-debug` 场景内实时看碰撞形状（地板蓝 / 盒绿 / 球橙 / 胶囊品红，depth test 开 / write 关），双后端 `deviceErrors=0`；工具侧 `debug`/`check-debug` 线流度量 TSV。实测见 §M-P3 固化口径与 `PHYS-TOOLS-GUIDE.md` §10；⚠️ §2.4 入口名坑的共享 builder 变体（`SHADER-CONVENTIONS.md` §2.4 已扩充） |
| **M-P4** ✅ **2026-11-09 完成（tag `phys-p4`）** | 角色胶囊（CharacterMovement 式：扫掠 + 地面检测 + 斜坡） | `character`/`check-character` 走查 TSV：地板 → 30° 斜坡 → 3 级台阶 → 60° 斜坡挡住（Jolt CharacterVirtual ExtendedUpdate，重力/输入应用侧合成）；viewer scenario=1 亮黄胶囊线框走查演示（双后端 `deviceErrors=0`）；`CharacterConfig.walkSpeed` 为动作系统衔接预留输入接口。实测见 §M-P4 固化口径与 `PHYS-TOOLS-GUIDE.md` §11 |
| M-P5 | 场景集成（物理对象 ↔ ECS/序列化） | **仍 gate 在 P8 E3 之后**，不随 M-P4 解锁 |
| **M-P5** | 场景集成：E1/E3 场景物体挂碰撞体 + 序列化 | E1 场景（50 物体）物理正确；Inspector 可编辑碰撞参数 |


## 选型评审结论（2026-11-09 二次拍板，权威落点）

**结论：继续走 vendored Jolt 5.3.1，不换库、不自研求解器。**

| 项 | 内容 |
|---|---|
| 用户拍板 | 「物理用现成的，没必要从头写，成本太高」→ 方向 = 现成库；「比较纠结的只是用哪个」→ 唯一剩的问题是选型 |
| 例外理由（改写后） | **成本取舍**：自研求解器成本过高、物理是 A 类并行轨道配角；**不是** UE 口径 / 业界共识（UE 自研 Chaos 恰恰是自己写求解器，原依据自相矛盾已修正，见 `UE-DIFFS.md` D10） |
| Jolt 胜出依据（本机实证） | 本机唯一零联网可得的 C++ 求解器源码（Carrot `thirdparty/JoltPhysics` 上游副本）；MIT 与项目一致；纯 CPU 零图形 SDK；C++17 只依赖 STL；官方把确定性列为一等特性且 M-P0 已实测逐位一致；上游自带 `Renderer/DebugRenderer*`（M-P3）、`Physics/Character/*`（M-P4）、CCD/约束；Carrot `engine/physics/` 2902 行集成层可作本地参照；M-P0 已全绿收口（tag `phys-p0`） |
| 候选落选原因 | **PhysX 5**（BSD-3）：UE4 血统但 UE5.8 `Engine/Source/ThirdParty/` 已无 PhysX SDK（`PhysicsCore/Private/PhysXLibs.cpp` 仅历史 DLL 加载兼容层）、本机无副本、GitHub 直连被封只能逐文件拉镜像、GPU 二进制非源码与 vendored 纪律有摩擦、确定性需额外配置且无本地验证；**Bullet3**（zlib）：本机无副本、一般不承诺逐位确定性，与本项目「轨迹逐点比对」验收冲突；**Rapier / avian**（Apache-2）：Rust，本项目 C++，排除；**Havok**：商业授权，排除 |
| 「贴 UE」这个纠结 | UE5 官方 = 自研 Chaos，Jolt 与 PhysX **都不是 UE5 口径**；拿 PhysX 换 Jolt 只换到 UE4 历史名义，不是 UE5 架构对齐。该维度两者打平，选型回到工程适配 |
| 换库代价（实测） | 摘除 `thirdparty/jolt/` 446 文件 / 4.9MB、重写 `World.cpp` 233 行 PIMPL、`phys-tools` 403 行 Jolt API 调用、作废 Carrot 2902 行本地参照、重定确定性契约、重做 M-P0 全部交代物与双后端构建；无对应口径收益 |
| 值得换的硬判据 | 只有目标变成「复现 UE4 PhysX 时代 Scene/Actor 耦合面」时才值得付代价拉 PhysX；否则换库是纯支出 |

**执行**：`feat/physics` worktree / 分支保留，M-P0 的 tag `phys-p0` 与台账留作历史留痕，代码与交付物不作废；本选型结论同步写进 `NON-RENDER-ROADMAP.md` / `UE-DIFFS.md` D10 / `PARALLEL-TRACKS.md` §7.1e / `AGENTS.md` 例外 3。


### M-P1 固化口径（2026-11-09 验收后写回）

- **选型二次拍板**：物理不自研（成本取舍），继续 Jolt 5.3.1；UE 口径不参与选型，详见 §选型评审结论。
- **层模型**：`kLayerCount=4`（FLOOR/DEFAULT/DEBRIS/GHOST），对象层与宽相层 **1:1 映射**；同一个 `LayerMatrix` 同时驱动 `ObjectLayerPairFilter` 与 `ObjectVsBroadPhaseLayerFilter`。只改一处会因宽相层先筛掉碰撞对而失效。
- **矩阵**：对称 bool 掩码 `rows[a] bit b`，`set()` 强制双向写，`validateLayerProbe` 拒绝非对称；UE 三态响应的简化差异见 `UE-DIFFS.md` D13。
- **双向关联**：`Body::SetUserData` 存 `bodies` 索引（不存裸指针）；owner→BodyID 用 `unordered_map`，BodyID→userData→owner 用表反查；两方向每例在 TSV 的 `owner_ok` 留证。
- **M-P0 兼容**：`simulateDrop()` 用「全开但 (floor,floor) 关」矩阵，M-P0 落球 TSV 逐字节回归（MD5 `075ddfe…a1ed`）锁死；过滤器重构不得改变该基线。
- **验收判据**：允许对停在 `blocker_y + 2r = 3.5`；禁用对「低于挡板接触线且未穿透地板」（末段余弹是物理正常，不要求精确静息）；ghost 全层穿透持续自由落体；`check-layers` 从 TSV provenance 读真实参数，不硬编码阈值。
- **确定性契约不变**：`JobSystemSingleThreaded` + 1/60 + `collisionSteps=1`。
- **交代**：工具版（`example/phys-p1/` + `cases.json` `phys-p1` 卡 + `PHYS-TOOLS-GUIDE.md` §8），不进 `kStage` / 渲染总闸。


### M-P2 固化口径（2026-11-09 验收后写回）

- **场景**：静态地面（顶 y=0）+ `boxes=6` 个 1m 立方，初始 `y=0.5+i*(1.0+gap)`、`gap=0.005`；固定 `1/60` + `collisionSteps=1` + `JobSystemSingleThreaded`，确定性契约不变。
- **三用例对照（权威）**：`baseline`（friction 0.6 / sleep on）、`no_sleep`（同摩擦、关 `mAllowSleeping`）、`no_friction`（friction 0 / sleep on / 顶层 `+x 0.5 m/s`）。前两例证明**休眠只省算力**，第三例证明**摩擦与接触求解器才是稳定来源**；不得把「休眠」写成稳定性来源。
- **休眠口径**：`StackConfig::allowSleeping` 同时写 `JPH::PhysicsSettings::mAllowSleeping` 与 `BodyCreationSettings::mAllowSleeping`；`mTimeBeforeSleep=0.5s` / `mPointVelocitySleepThreshold=0.03` 用上游默认不暴露。只读 `GetNumActiveBodies(EBodyType::RigidBody)` 观测。UE 对应 `IslandManager` 整岛休眠，差异见 `UE-DIFFS.md` D14。
- **稳定性指标**：`maxTailSpread`（末 60 帧全体 `|y(t)-y(end)|` 最大值）、`maxTailSpeed`、`maxDrift`/`topDrift`、`collapsed`、`finalActive`。判据按物理本质写，不要求精确静息高度；`no_sleep` 尾段速度非零属正常持续迭代，只有 baseline 要求速度严格为 0。
- **复验防假绿**：`check-stack` 必须从 25 列数据**独立重算**末态层高、层距、尾段极差、活跃体、漂移，再与 `# case` 摘要行交叉核对；只验摘要不算验收。
- **回归闸**：M-P0 `drop.tsv`（MD5 `075ddfe…a1ed`）与 M-P1 `layers.tsv`（MD5 `36c4b169…111c`）逐字节不变，任何新增世界不得扰动这两条基线。
- **交代**：工具版（`example/phys-p2/` + `cases.json` `phys-p2` 卡 + `PHYS-TOOLS-GUIDE.md` §9），不进 `kStage` / 渲染总闸。

### M-P3 固化口径（2026-11-09 验收后写回）

- **场景**：静态地板薄盒（半高 0.5，顶 y=0，半 extent 5）+ 盒(0.5³)/球(r0.5)/胶囊(r0.5,h0.3) 三落体，x = -2/0/+2 错开、初始 y=4；restitution 0.4 / friction 0.6 / damping 0；固定 `1/60` + `collisionSteps=1` + `JobSystemSingleThreaded`，确定性契约不变。稳态锚：地板线框 y∈[-1,0]、胶囊顶 1.6（半高 0.8）。
- **分色（权威）**：地板蓝 `(60,120,255)`、盒绿 `(80,220,80)`、球橙 `(255,160,40)`、胶囊品红 `(230,80,230)`——`World.cpp extractLines` 与 `phys-tools debug` TSV `# colors` 行必须同源，改一处必须同步另一处。
- **DebugRenderer 接线**：实现方收顶点、引擎只「喊」图元；`DrawGeometry` 取 `mLODs.front()`（最细 LOD）；`RefTargetVirtual::AddRef/Release` **非 const**；`GetType()` 是 broad `EShapeType`，分形用 `GetSubType()`。
- **渲染层级**：`DebugScene`/`DebugLines` POD 全在 `nl/physics`（零 Jolt 类型，PIMPL）；线框管线（LINE_LIST / depth test 开 / write 关 / 固定容量动态 VB）在 `renderer/`，`ensurePipelines` 无条件 ensure、关闭时零开销（不 bump `kStage`，M-P0~P2 渲染基线不进渲染总闸口径不变）。
- **⚠️ §2.4 共享 builder 变体**：`DebugLines::ensure` 硬编码 `"vsMain"/"psMain"` → Vulkan `vkCreateGraphicsPipelines` 每帧 `VK_ERROR_UNKNOWN`（VVL 对管线零报错）。共享 builder 一律传 `"main"`；详 `SHADER-CONVENTIONS.md` §2.4。
- **复验防假绿**：`check-debug` 只信数据列——拓扑锚（顶点数/分色桶逐帧恒定，桶和=顶点数）、物理锚（包围盒列独立复算：初始顶 4.8 / 静止顶 1.6 / 地板底 -1 / 地板 ±5）、静止锚（末 30 帧 apex 极差 < 1e-4）。
- **回归闸**：`check-phys-regression.py` 扩到四基线（M-P0 drop / M-P1 layers / M-P2 stack / M-P3 debug）逐字节 + MD5。
- **双后端互比**：线框截图 599px / maxDelta 242，薄线覆盖噪声按 `scene`/`scale` 先例如实记 FAIL 档，各自对本后端基线 bytes。
- **交代**：工具版 + 渲染证据双轨（`example/phys-p3/`：debug.tsv + 双后端 viewer 截图 + 2 张证据图 + `cases.json` `phys-p3` 卡 + `PHYS-TOOLS-GUIDE.md` §10），不进 `kStage` / 渲染总闸。

### M-P4 固化口径（2026-11-09 验收后写回）

- **路线**：Jolt `CharacterVirtual`（虚体走查），不是 Carrot 的 `JPH::Character`（真实体 kinematic）——M-P4 计划口径「CharacterMovement 式扫掠」与 UE `MoveAlongFloor/FindFloor/MaxSlopeAngle` 同型，Carrot 无 CharacterVirtual 参考。
- **mPosition 语义（源码钉死）**：胶囊（COM=0、offset=0）shape 原点 = `mPosition + padding*up` → mPosition 是胶囊几何中心；脚底 = py − 1.08；贴地 py = top + 1.10（padding 悬距 0.02）。锚点全部按此口径。
- **步进节奏**：重力/输入应用侧每帧合成（OnGround: `(walk, −g·dt, 0)` 贴地压紧；空中: `(walk, vy−g·dt, 0)`）；`ExtendedUpdate` 用 Jolt 默认 stickDown 0.5 / stepUp 0.4（显式写出）。
- **场地（中心化 x−13）**：地板顶 y=0 → 30° 斜坡 x∈[−7,−3]（顶 2.3094）→ 平台1 → 3 级台阶 x∈[5,6.8]（每级 0.3，顶 3.2094，WalkStairs）→ 平台2 → 60° 斜坡 x∈[11,13]（maxSlope 50° 挡住）→ 保险墙 x=17。中心化原因：OrbitCamera pivot 固定原点，viewer scenario=1 固定机位 `(yaw 0, pitch 35°, dist 19)`。
- **台阶锚窗口**：[4.3, 7.2] 而非几何区间 [5, 6.8]——胶囊半径 0.5 前缘提前 0.5m 接触立面。
- **确定性**：与 M-P0 同契约（fresh world + 固定步 + 单线程 JobSystem），两次模拟逐位一致；五基线逐字节回归闸（drop/layers/stack/debug/character）。
- **viewer 接线**：scenario=1 时 `DebugScene::Impl.charH` 全接管；CharacterVirtual 无真实体，extractLines 手动 `DrawCapsule` 画亮黄 (255,230,60) 胶囊；`bodyInterface` guard 放行条件改为 `!bodyInterface && !charH`。
- **动作系统衔接**：`CharacterConfig.walkSpeed` = 输入接口演示驱动；`groundState`（EGroundState 枚举序 0..3，TSV 直接可读）与 `GetGroundVelocity` 可直接喂动画状态机——M-P5/E 系集成时按此接。

## 验收纪律

- 工具版交代：`phys-tools`（无头步进 / dump / 层过滤用例）+ 展示台 `kind:"tool"` 用例（usage / 正向 / 负向）+ `PHYS-TOOLS-GUIDE.md` 归档；证据图从 dump 真实输出生成。
- **确定性**：固定随机种子下同场景步进轨迹逐点一致（Jolt 本身确定性模拟，需锁线程数口径）。
- Jolt 更新频度锁死 vendored 版本，升级单独走一轮验收。
- `core/physics/` 零图形符号（Jolt 本身是 CPU 库，符合层级纪律）；debug 渲染挂 viewer 层。
- merge 回 master 前过 §7.2 文档闸 + §7.5 关联文档同步。

## 学习支线（可选，不排期，不进引擎）

想学求解器原理时另开 Box2D-Lite 风格 mini 求解器支线（AABB 宽相 → 冲量求解 → 堆叠，几百行），对照 UE Chaos 只读——产出是理解，反哺集成层调参，不作为引擎正式物理。

## 排期约束（拍板留痕）

**执行不提前**：物理体吃场景（P8 E1 变换层级 / 多物体，E3 可编辑碰撞参数），提前做只能合成场景自嗨。启动时机 = P8 E1（M-P0~M-P3 可与 E1 并行启动）+ E3（M-P5 依赖）验收通过之后；届时新建 worktree 并更新本文件与注册表。

### 排期闸实际执行（2026-11-09 用户指令）

用户「建 feat/physics 开发物理模块内容」= 提前解锁 **M-P0~M-P3**（括号内口径本就允许与 E1 并行，且这几步只吃 Jolt 世界 + 体封装，不依赖场景整合）。已按 §7.1b 建 `feat/physics` worktree（junction 共享 + skip-worktree）并注册进 AGENTS.md / PARALLEL-TRACKS §7.1e / wt_pool TRACKS。**M-P5 场景集成不解锁**，仍 gate 在 P8 E3（序列化 + Inspector）之后。

### M-P0 固化口径（2026-11-09 验收后写回）

- **版本锁死 Jolt 5.3.1**（本地 Carrot 上游副本，MIT，零联网），升级单独走一轮 M-P 验收；来源/裁剪见 `thirdparty/jolt/VENDOR.md`。
- **确定性契约** = `JobSystemSingleThreaded`（零 worker 线程）+ 固定 `1/60` + `collisionSteps=1`；同 `DropConfig` 两次新建世界样本逐位一致。改线程池属新里程碑，须重定基线。
- **构建接入**：`add_subdirectory(thirdparty/jolt/Build EXCLUDE_FROM_ALL)`，FORCE 关 `/MT` 静态 CRT、统一 RTTI/异常、关 IPO/install/shared/asserts；纯 CPU，零图形 SDK、零网络。
- **层级**：`nl/physics` 用 PIMPL 把 Jolt 头挡在 `core/src/physics/World.cpp`；`core/` 零图形符号，debug 渲染（M-P3）才挂 viewer。
- **交代**：工具版（`example/phys-p0/` + `cases.json` `phys-p0` 卡 + `PHYS-TOOLS-GUIDE.md`），不进 `kStage` / 渲染 `bytes-near` 总闸。
