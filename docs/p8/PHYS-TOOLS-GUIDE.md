# PHYS-TOOLS-GUIDE — 物理轨道工具与 M-P0 验收归档

> 轨道：`feat/physics`（A 类并行轨道，worktree `E:/UE_STUDY_2026/nl-engine-physics`）。
> 计划：`docs/p8/P8-PHYS-PLAN.md`（M-P0~M-P5）。本文件覆盖 **M-P0**，后续里程碑追加章节。
> 性质：**工具版交代**——不进 `kStage`、不进 `bytes/near` 渲染回归总闸；证据来自 `phys-tools.exe` 的真实输出。

## 1. 定位与边界

| 项 | 口径 |
|---|---|
| 求解器 | **不自研**。vendored Jolt Physics **5.3.1**（MIT，纯 CPU），本地 `Carrot/thirdparty/JoltPhysics` 上游副本，**零联网**；来源/裁剪/升级纪律见 `thirdparty/jolt/VENDOR.md`。 |
| 我们写的 | 集成层 `core/include/nl/physics/World.hpp` + `core/src/physics/World.cpp`：进程级 bootstrap、两层碰撞层/宽相层、静态地面 + 动态球、固定步长、逐帧轨迹。**PIMPL 把 Jolt 头挡在 cpp 内**，其他 `nl_core` 消费者零 Jolt 编译负担。 |
| 层级纪律 | `core/` 零图形 API 符号；`phys-tools` 只链 `nl_core`；debug 渲染（M-P3）才挂 viewer 层。 |
| 确定性 | `JPH::JobSystemSingleThreaded`（零 worker 线程）+ 固定 `1/60` 步长 + `collisionSteps=1`。**线程数口径是锁死的契约**：同 `DropConfig` 两次新建世界，样本逐字段位一致。改线程池要另起里程碑重定基线。 |

## 2. 退出码（对齐 `nanite-scene` / 展示台 `expectExit`）

| 码 | 含义 | 触发 |
|---|---|---|
| 0 | PASS | selftest / simulate / check 全过 |
| 1 | 断言或运行失败 | 锚点不符、TSV 打不开或格式坏 |
| 2 | 用法错误 | 无参数、未知子命令、非法参数（`frames`/`dt`/`gravity`/`height`/`radius`/`restitution`） |

## 3. 用法

```
phys-tools selftest [--verbose]      # 23 条机械断言（负向配置 / 自由落体 / 弹跳 / 收敛 / 确定性 / dump）
phys-tools simulate [--out <tsv>] [--frames N] [--dt S] [--gravity G]
                    [--height H] [--radius R] [--restitution E] [--seed N]
phys-tools check <tsv>              # 从 TSV 头读回真实参数，用同一套锚点复验
```

`simulate` 不带 `--out` 时把 TSV 打到 stdout；带 `--out` 落盘并打印 `wrote <path> frames=N bodies=2`。
TSV 头三行是 provenance（版本 / job system / 全部参数 / 体数），数据列 `frame / t / y / vy`。

复现一条命令：`example/phys-p0/run.bat`（重跑四用例 → 重写 `drop.tsv` 与四份 log → 从真实数据重生成两张证据图 → `ALL GREEN` 才 exit 0）。
构建：`scripts/build-m0.2.bat DX12` / `VULKAN`（本机裸 shell 无 nmake 环境，必须走脚本）。

## 4. 五用例实跑归档（展示台 `phys-p0` 卡）

| 用例 | 命令 | expectExit | 真实输出锚 |
|---|---|---|---|
| usage | `phys-tools` | 2 | `phys-tools - nl-engine P8 physics tools (Jolt 5.3.1, headless)` |
| selftest | `phys-tools selftest --verbose` | 0 | `phys-p0 selftest: PASS (Jolt 5.3.1, single-threaded, 240 frames)` |
| simulate | `phys-tools simulate --frames 240` | 0 | `# nl-physics M-P0 drop trajectory (real Jolt 5.3.1 solver output)` |
| check | `phys-tools check {root}/example/phys-p0/drop.tsv` | 0 | `phys-p0 check: PASS (..., 241 samples)` |
| negative | `phys-tools simulate --restitution 1.5` | 2 | `invalid arguments: restitution out of range [0,1]` |

原始输出落 `example/phys-p0/{selftest,check,usage,negative}.log`，证据图由 `tools/make-phys-evidence.py` 从这些 log 与 `drop.tsv` 渲染（**不许手绘示意**）。

## 5. M-P0 数值锚（默认配置：h=5.0 / r=0.5 / g=9.8 / e=0.6 / dt=1/60 / 240 帧）

| 量 | 实测 | 判据 |
|---|---|---|
| 自由落体段 | 与半隐式欧拉闭式 `y_n = h − ½·g·dt²·n(n+1)` 最大误差 **0.000001** | < 0.05（且逐帧单调下降） |
| 首次触地 | frame 57 / **0.950 s** | 解析 `sqrt(2(h−r)/g)=0.958 s`，差 < 3 步 |
| 反弹峰值 | **2.138943 m** @ frame 92 | `r + e²(h−r) = 2.120 m`；且 > r、< h |
| 静息 | 末帧 y = **0.500000**（= 球心高度） | 末 10 帧极差 **0.000000**；30 帧尾段极差 0.038218（第 3/4 次小弹跳，能量衰减非抖动） |
| 世界体数 | 2（静态地面 + 动态球） | `PhysicsSystem::GetNumBodies()` |
| 确定性 | 两次新建世界 241 样本逐字段相等 | 硬判据，逐位比对 |
| 双后端 | DX12 + VULKAN 全树 BUILD OK、viewer selftest 16/16 ×2、`phys-tools selftest` 两树 PASS | A 类不产 GPU 画面，无 `bytes/near` 分档 |

证据图：`example/phys-p0/trajectory.png`（y(t) 真实曲线 + 解析自由落体 + 触地/反弹/静息标注 + 实测面板）、`acceptance.png`（四份真实日志终端风格渲染）。两张均肉眼核验过。

## 6. 踩坑（M-P0 实踩，写进 canonical 防复发）

1. **CRT 混链**：Jolt 上游 `USE_STATIC_MSVC_RUNTIME_LIBRARY` 默认 ON（`/MT`），而 nl-engine 走 `/MD`；混链会 LNK4098/运行时堆不一致。接入前必须 `set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)`（Carrot 同款处理），并统一 `CPP_RTTI_ENABLED` / `CPP_EXCEPTIONS_ENABLED`、关掉 IPO/install/shared/asserts/FP exceptions。
2. **`AssertFailed` 回调返回 true 会 `JPH_BREAKPOINT`**：Jolt 的 assert 宏把返回值当「是否断进调试器」，headless 工具里返回 true 等于主动 int3。我们只打日志并返回 false。
3. **裸 shell 无 nmake**：`cmake --build build/DX12 --target phys-tools` 直跑报 `execution of make failed`；构建一律走 `scripts/build-m0.2.bat`（显式 INCLUDE/LIB/PATH）。
4. **收敛判据不能只看尾部极差**：e=0.6 时最后 30 帧仍有第 3/4 次弹跳（极差 0.038），首版阈值 0.03 误判 FAIL。拆成「30 帧衰减 < 0.05」+「末 10 帧严格平坦 < 0.005」两条，物理正确且仍严格。
5. **`check` 不能假定默认参数**：TSV 头带完整 provenance，`check` 从 `# jolt=... dt=... gravity=...` 读回参数再算锚点，否则自定义 dump 会拿默认 h/g/e 去验，假绿。
6. **UE 口径勘误**：立项文档原写「UE 5.x 官方提供 Jolt 后端」——2026-11-09 实读本机 UE 5.8.4 源码树，只有自研 Chaos（`Runtime/Experimental/Chaos*` + `PhysicsCore/` + `Engine/Classes/PhysicsEngine/`），**无 Jolt 后端**。结论「不自研求解器」不变，依据已改；差异落 `docs/s5/UE-DIFFS.md` **D10**。
7. **证据图标注重叠**：首版 trajectory.png 的「floor top + radius」与「contact」标注在同一水平带撞车；把 floor 标签移到红线下方后复验通过。工具版证据图同样要肉眼核验，不是生成完就算数。

## 7. 下一步（不属 M-P0）

M-P1 体封装 + 碰撞层矩阵（引擎对象 ↔ Jolt 双向关联，`BodyUserData` 思路）/ M-P2 堆叠稳定 / M-P3 debug 渲染接 viewer / M-P4 角色胶囊 / **M-P5 场景集成仍 gate 在 P8 E3（序列化 + Inspector）之后**，未按本轮提前解锁。


## 8. M-P1：碰撞层矩阵与体关联（2026-11-09，tag `phys-p1`）

### 交付物
- `example/phys-p1/run.bat`：重跑 selftest / layers / check-layers / M-P0 逐字节回归 / usage / 负向，再重生成两张证据图，全绿才退出 0。
- `example/phys-p1/layers.tsv`：4 用例 × 181 帧真实 Jolt 输出；`# jolt=` 行是 provenance（blocker_y / radius / frames / dt / gravity），`# case` 行记录每个用例的矩阵、末帧高度、blocked、owner_ok、体数。
- `example/phys-p1/{trajectory,acceptance}.png`：`tools/make-phys-p1-evidence.py` 从真实 TSV 与真实日志渲染，无手绘曲线。
- `tools/check-phys-p0-regression.py`：M-P0 基线逐字节 + MD5 比对，输出纯 ASCII（中文 Windows 的 `fc /b` 会吐 GBK，不能进证据图）。

### 内核约定（写回 `P8-PHYS-PLAN.md`）
- **层级**：`kLayerCount=4`（FLOOR=0 / DEFAULT=1 / DEBRIS=2 / GHOST=3），对象层与宽相层 **1:1 映射**，同一个 `LayerMatrix` 同时驱动 `ObjectLayerPairFilter` 与 `ObjectVsBroadPhaseLayerFilter`——只改一处过滤器会让宽相层先把碰撞对筛掉，矩阵看起来「没生效」。
- **矩阵**：`rows[a] bit b` 对称 bool 掩码，`set()` 强制双向写入；`validateLayerProbe` 拒绝非对称矩阵。UE 用 `FCollisionResponseContainer` 的 Ignore/Overlap/Block 三态，我们简化的差异见 `UE-DIFFS.md` D13。
- **双向关联**：Jolt `Body::SetUserData` 存的是 `bodies` 向量的**索引**，不是对象裸指针（避免 move/销毁后悬垂）；owner -> BodyID 走 `unordered_map`，BodyID -> userData(index) -> owner 走表反查，两个方向都由 `ownerOk()` 在 TSV 里留证。
- **M-P0 兼容**：`simulateDrop()` 用「全开但 (floor,floor) 关」的矩阵，过滤行为与原固定两层版本逐位一致；`run.bat` 用 MD5 逐字节回归把这条锁死。
- **确定性契约不变**：`JobSystemSingleThreaded` + 1/60 + collisionSteps 1；`disabled_pair` 两次新建世界逐样本位一致。
- **收敛判据**：穿透用例末帧仍在地板上余弹（实测 0.842475），判据按物理本质写成「低于挡板接触线且未穿透地板」，不要求精确静息高度（M-P0 同款教训）；`check-layers` 从 provenance 读真实参数，不硬编码 3.5/0.5。

### CLI
| 命令 | 行为 | 退出码 |
|---|---|---|
| `phys-tools layers [--out <tsv>] [--frames N] [--verbose]` | 跑 4 个层矩阵用例，输出逐帧 TSV | 0 / 1 / 2 |
| `phys-tools check-layers <tsv>` | 从归档 TSV 复验矩阵结局与双向关联 | 0 / 1 / 2 |

### 下一步
M-P2 堆叠稳定 / M-P3 debug 渲染接 viewer / M-P4 角色胶囊；**M-P5 仍 gate 在 P8 E3 之后**。


## 9. M-P2：堆叠稳定与休眠岛（2026-11-09，tag `phys-p2`）

### 交付物
- `example/phys-p2/run.bat`：重跑 selftest / stack / check-stack / M-P0+M-P1 双基线逐字节回归 / usage / 负向，再重生成两张证据图，全绿才退出 0。
- `example/phys-p2/stack.tsv`：3 用例 × 601 帧 × 6 体真实 Jolt 输出（25 列）；`# jolt=` 是 provenance（boxes / half_extent / gap / frames / tail），`# case` 记录每例的摩擦、休眠开关、末态活跃体数、尾段极差/速度、最大漂移、顶层高度、是否塌塔。
- `example/phys-p2/{trajectory,acceptance}.png`：`tools/make-phys-p2-evidence.py` 从真实 TSV 与真实日志渲染（六层高度曲线 + 三用例顶层对照 + 末 60 帧收敛放大 + 实测面板；验收图为完整日志，provenance 长行循环折行不截断）。
- `tools/check-phys-regression.py`：M-P0 `drop.tsv` + M-P1 `layers.tsv` 双基线逐字节 + MD5 比对，输出纯 ASCII。

### 内核约定（写回 `P8-PHYS-PLAN.md`）
- **场景**：静态地面（Box 200×2×200，顶 y=0）+ `boxes` 个 1m 立方（halfExtent 0.5），初始中心 `y = 0.5 + i*(1.0+gap)`，`gap=0.005` 留微小下落让压实过程可见。固定 `1/60`、`collisionSteps=1`、`JobSystemSingleThreaded`——确定性契约不变。
- **三用例对照**：`baseline`（friction 0.6 / sleep on / 无扰动）、`no_sleep`（同摩擦，关 `mAllowSleeping`）、`no_friction`（friction 0 / sleep on / 顶层 `+x 0.5 m/s`）。前两例证明「休眠只省算力」，第三例证明「摩擦才是稳定来源」。
- **休眠开关**：`StackConfig::allowSleeping` 同时写 `JPH::PhysicsSettings::mAllowSleeping` 与 `BodyCreationSettings::mAllowSleeping`；`mTimeBeforeSleep=0.5s` / `mPointVelocitySleepThreshold=0.03` 用上游默认不暴露。只读 `GetNumActiveBodies(EBodyType::RigidBody)` 观测，不碰求解器。UE 对应 `IslandManager` 整岛休眠，差异见 `UE-DIFFS.md` D14。
- **稳定性指标**：`maxTailSpread`（末 60 帧全体 |y(t)−y(end)| 最大值）、`maxTailSpeed`、`maxDrift`/`topDrift`（水平位移）、`collapsed`（任一层低于期望槽位半个立方）、`finalActive`。判据按物理本质写，不要求精确静息高度（M-P0/M-P1 同款教训）。
- **复验防假绿**：`check-stack` 从 TSV 数据列**独立重算**末态层高、层距、尾段极差、活跃体、漂移，再与 `# case` 摘要行交叉核对；只验摘要不算验收。
- **回归闸**：M-P0 落球与 M-P1 层矩阵 TSV 的 MD5 必须逐字节不变（`075ddfe…a1ed` / `36c4b169…111c`），证明新增堆叠世界未扰动既有基线。

### CLI
| 命令 | 行为 | 退出码 |
|---|---|---|
| `phys-tools stack [--out <tsv>] [--frames N] [--boxes N] [--gap G]` | 跑 3 个堆叠用例，输出逐体逐帧 TSV | 0 / 1 / 2 |
| `phys-tools check-stack <tsv>` | 从归档 TSV 复验堆叠稳定性与休眠 | 0 / 1 / 2 |

### 下一步
M-P3 debug 渲染接 viewer（Jolt DebugRenderer → ImGui / 线框）/ M-P4 角色胶囊；**M-P5 仍 gate 在 P8 E3 之后**。


## 10. M-P3：碰撞形状调试线框接 viewer（2026-11-09，tag `phys-p3`）

### 交付物
- `example/phys-p3/run.bat`：重跑 selftest / debug / check-debug / M-P0+M-P1+M-P2 三基线逐字节回归 / **双后端 viewer 截图（`deviceErrors=0` 硬闸）** / usage / 负向，再重生成两张证据图，全绿才退出 0。
- `example/phys-p3/debug.tsv`：241 帧 × 13 列真实 Jolt DebugRenderer 线流汇总（`frame t vertices min/max xyz box_v sphere_v capsule_v floor_v`）；`# colors` 行记录四分色参考值，与 `World.cpp extractLines` 同源。
- `example/phys-p3/{viewer-dx12,viewer-vulkan}.png`：viewer `--physics-debug` 真实 GPU 截图（DX12/Vulkan 各一，240 帧，`deviceErrors=0`）。绿盒 / 橙经纬球 / 品红胶囊 / 蓝地板网格，被 Suzanne 实体近面正确深度遮挡（depth test 开 / write 关）。
- `example/phys-p3/{wireframe,acceptance}.png`：`tools/make-phys-p3-evidence.py` 从真实 TSV 与真实日志渲染（落下-回弹-静止曲线 + 四分色桶面板 + 末 30 帧冻结放大 + 实测面板；验收图为完整日志）。
- `tools/check-phys-regression.py` 扩到四基线：M-P0 drop / M-P1 layers / M-P2 stack / M-P3 debug 全部逐字节 + MD5。

### 内核与渲染接线（写回 canonical）
- **`nl::physics::DebugScene`**（`World.hpp` 零 Jolt 类型 POD，PIMPL 挡头）：`build(scenario, seed, err)` 建世界（地板薄盒 + 盒/球/胶囊三落体，x 错开，restitution 0.4 / friction 0.6）；每帧 `step(1/60)` + `extractLines()`。`DebugLines{data, vertexCount}` 指向内部缓冲，**单帧消费模型**（下次 extractLines 即失效）。
- **Jolt DebugRenderer 子类**（`World.cpp` 匿名命名空间）：实现 `DrawLine / DrawTriangle / DrawText3D / CreateTriangleBatch×2 / DrawGeometry`；batch 的 `AddRef/Release` **非 const**（`RefTargetVirtual` 纯虚签名），引用归零 `delete this`。`DrawGeometry` 取 `mLODs.front()`（最细 LOD；取 `back()` 球/胶囊呈星状）。`Shape::GetType()` 是 broad `EShapeType`（三者皆 `Convex`），分形要看 `GetSubType()`。
- **renderer 侧 `DebugLines`**（`scene.hpp/.cpp`）：固定容量动态 VB（262144 顶点）、LINE_LIST、depth test 开 / depth write 关、volatile CB b0。`ensurePipelines` **无条件** ensure（默认不画、关闭时零开销）；`invalidate` 随主管线重建。
- **⚠️ §2.4 实测扩充**：共享管线 builder **硬编码入口名**也会中招——`DebugLines::ensure` 曾写死 `"vsMain"/"psMain"`，DX12 忽略入口名蒙混过关，Vulkan `vkCreateGraphicsPipelines` 每帧 `VK_ERROR_UNKNOWN`（验证层对管线零报错，极难定位）。修复：一律传 `"main"`（compute 同款先例）。详 `SHADER-CONVENTIONS.md` §2.4。
- **双后端互比**：分色线框截图 599px 差 / maxDelta 242——薄线光栅化覆盖噪声，按 `scene`/`scale` 先例口径如实记 **FAIL 档**，各自对本后端基线 bytes；三次尝试消不掉，不再折腾。

### 工具 CLI
| 命令 | 行为 | 退出码 |
|---|---|---|
| `phys-tools debug [--out <tsv>] [--frames N]` | 与 viewer 共用 DebugScene，逐帧汇总线流几何度量成 TSV | 0 / 1 / 2 |
| `phys-tools check-debug <tsv>` | 从数据列独立复验：拓扑恒定、四桶划分、物理锚（初始顶 4.8 / 静止顶 1.6 / 地板底 -1 / 地板 ±5）、末 30 帧冻结 | 0 / 1 / 2 |

### 复验防假绿
`check-debug` 只信数据列：结构锚（顶点数与分色桶逐帧恒定、四桶之和=顶点数）+ 物理锚（从包围盒列独立复算，不信头注释）+ 静止锚（末 30 帧 apex 极差 < 1e-4，睡眠岛逐位冻结）。摘要行（`# jolt=` 头）只作 provenance。

### 下一步
M-P4 角色胶囊（Jolt `CharacterVirtual`）；**M-P5 场景集成仍 gate 在 P8 E3 之后**。
