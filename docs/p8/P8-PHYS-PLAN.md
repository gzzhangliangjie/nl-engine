# P8-PHYS-PLAN — 物理系统（Jolt vendored 集成，A 类并行轨道，2026-11-08 立项）

> 轨道归属：`NON-RENDER-ROADMAP.md` 候选总表「物理（Jolt 集成）」行（★★★）。用户拍板（2026-11-08）：**vendored Jolt（MIT）不自研**——总原则的首例例外；三处证据（Carrot physics 即 Jolt 集成层 / UE 自研 Chaos 工业终态 / bevy 本体无物理、生态 avian），三条路径均不要求我们自己写求解器。
> **2026-11-09 勘误**：原写「UE 5.x 官方提供 Jolt 后端」不成立——实读本机 UE 5.8.4 源码树只有 Chaos（`Runtime/Experimental/Chaos*` / `PhysicsCore/` / `Engine/Classes/PhysicsEngine/`），无 Jolt 后端；结论不变，差异已落 `docs/s5/UE-DIFFS.md` D10。
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
| **M-P1** | 体封装 + 碰撞层（引擎对象 ↔ Jolt 双向关联） | 层过滤矩阵用例：同层碰撞 / 异层穿透，dump 可验 |
| **M-P2** | 堆叠 + 地面（静态体） | 箱体堆叠稳定不抖（长时间步进位移收敛），dump 证据 |
| **M-P3** | debug 渲染接 viewer（Jolt DebugRendering → ImGui/线框） | 场景内实时看碰撞形状；交互模式演示 |
| **M-P4** | 角色胶囊（CharacterMovement 式：扫掠 + 地面检测 + 斜坡） | 胶囊走斜坡/台阶轨迹 dump；与动作系统（M-A 轨道）动画衔接预留接口 |
| **M-P5** | 场景集成：E1/E3 场景物体挂碰撞体 + 序列化 | E1 场景（50 物体）物理正确；Inspector 可编辑碰撞参数 |

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
