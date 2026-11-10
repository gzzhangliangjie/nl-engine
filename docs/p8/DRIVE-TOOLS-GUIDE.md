# DRIVE-TOOLS-GUIDE — 驾驶能力轴工具归档（M-D0：碰撞事件桥）

> 轨道：`feat/drive`（A 类并行轨道，worktree `E:/UE_STUDY_2026/nl-engine-drive`）。
> 计划：`docs/p8/P8-DRIVE-PLAN.md`（M-D0~M-D3）。本文件覆盖 **M-D0**，后续里程碑追加章节。
> 性质：**工具版交代**——不进 `kStage`、不进 `bytes/near` 渲染回归总闸；证据来自 `phys-tools.exe` 的真实输出。

## 1. 定位与边界

| 项 | 口径 |
|---|---|
| 交付 | `core/include/nl/physics/Events.hpp`（Jolt-free POD）+ `World::simulateContactProbe`（`World.cpp` 内 `ContactCollector`）+ phys-tools `contact-trace` / `contact-check` / selftest `contacts` 节。 |
| 层级纪律 | `nl/physics` 头文件零 Jolt 类型；`ContactListener` 及其三回调只出现在 `core/src/physics/World.cpp`（core 内唯一 Jolt 符号面）；`core/` 零图形 API；phys-tools 只链 `nl_core`。 |
| 参考口径 | Carrot `PhysicsSystem.h:73/.cpp:124,345` 单全局 listener + `PhysicsBody.cpp:68/87-94` 帧内入队→帧末主线程派发（**只借时序**）；bevy `event/mod.rs:573,716` 双缓冲两帧保活（**M-D1 才落消费者**）；UE `PrimitiveComponent.h:274` Hit 委托字段子集（**不借立即派发时机**）。三处口径差异与 Q1 拍板见计划 §2.3 / §7。 |
| 硬约束 | vendored `ContactListener.h:123-128`：`OnContactRemoved` 期间 body 可能已销毁，**禁止任何 body 访问**；owner 对、接触点、法线一律在 Added / Persisted 阶段缓存，removed 只读缓存，派发后清（Q2）。 |
| 确定性 | M-P0 契约不变：`JobSystemSingleThreaded` + 固定 1/60 + fresh world；每帧末按 `(minBodyId, maxBodyId, kind)` 稳定排序（池模式回调线程不定时兜底逐位）。两次新建世界事件流逐字段位一致。 |

## 2. 事件 POD 与 TSV 格式

`ContactEvent`（`Events.hpp`，Jolt-free）：

| 字段 | 含义 |
|---|---|
| `frame` | 事件所属物理帧（1..frames） |
| `kind` | 0=Added / 1=Persisted / 2=Removed（冻结为 TSV wire 格式） |
| `ownerA/ownerB` | 引擎侧 u64 id，来自 `Body::GetUserData()` → owner 表；Jolt 保证 A 为低 BodyID 侧 |
| `bodyIdA/bodyIdB` | 原始 `JPH::BodyID` 值（非 Jolt 类型） |
| `worldPos` | 接触点（`ContactManifold::GetWorldSpaceContactPointOn1(0)`） |
| `normalY` | `mWorldSpaceNormal.y`（方向 = 把 body2 推出碰撞，未翻转） |

TSV：`#` 行是 provenance（jolt / job_system / dt / gravity / radius / start_height / blocker_y / restitution / frames / bodies / events / cache_misses）；`E` 行 11 列（tag + 10 字段）；`T` 行 5 列（frame / t / y / vy，探针轨迹）。`contact-check` **只信 E/T 数据行**，摘要行仅作 provenance 与交叉一致性校验。

**缓存 key 口径**：`SubShapeIDPair` 的 **body id 部分**（min/max 归一）作 `unordered_map` key，不取 subshape id —— `ContactListener.h:133-134` 明示 removed 时 subshape id 来自上一帧、shape 变化后可能失效。body id 直接来自回调参数，removed 期间零 Jolt body 访问。

## 3. 子命令与退出码

| 命令 | 行为 | 退出码 |
|---|---|---|
| `contact-trace [--out <tsv>] [--frames N]` | 三体世界（floor / static blocker / dropped probe）跑 N 帧，输出事件流 + 轨迹 TSV | 0 / 1（运行失败）/ 2（参数错） |
| `contact-check <tsv>` | 从 E/T 数据行独立复验结构锚 + 实测数值锚 | 0 / 1 / 2 |
| `selftest [--verbose]` | M-P0~M-P4 + M-J5c 既有节 + **新增 contacts 节**（负向配置 / 三类事件 / owner 解析 / 确定性 / 实测锚） | 0 / 1 |

退出码语义对齐 `PHYS-TOOLS-GUIDE.md` §2 与展示台 `expectExit`：0=PASS / 1=断言或运行失败 / 2=用法错误。

负向实测：`contact-trace --frames 0` → 2（`invalid arguments: bad --frames`）；`contact-check <不存在>` → 1；未知子命令 → 2。

## 4. M-D0 实测锚（`contact-trace --frames 240` 真实输出反推，禁手写）

| 量 | 实测 | 判定位置 |
|---|---|---|
| 事件总数 | **41** | selftest contacts / contact-check |
| kind 分布 | Added **6** / Persisted **29** / Removed **6** | 同上 |
| 首个 Added | **f44**（probe 落到 blocker 顶） | 同上 |
| 首个 Removed | **f46** | 同上 |
| 末事件 | **f185**（Removed，睡眠岛） | 同上 |
| 静息尾段 | f186..f240 **零事件**，末 30 帧 quiet | contact-check |
| 接触点 / 法线 | worldPos=(0, **3.0**, 0)（`blockerY + radius`）、normalY=**1.0** | contact-check |
| 探针静息高度 | **3.500000**（blocker 顶 3.0 + 球半径 0.5） | contact-check |
| Q2 缓存 | `cache_misses=0`，6 个 Removed **全部携带非零 owner 对** | selftest / check / provenance |
| 事件时序 | 6 次 Added→Persisted→Removed 循环（f44/96/126/144/155/161 Added；f46/98/127/145/160/185 Removed） | TSV 事件时间线证据图 |

锚由一次性脚本读 `build/DX12/phys-d0-contacts.tsv` 生成后注入 C++，非手写。`contact-check` 对非标准 `--frames` 只跑结构锚、跳过数值锚（事件序列是求解器产物，无法从 provenance 反算）。

## 5. 交代物与一键复现

```
example\drive-m0\run.bat     # 前置：scripts\build-m0.2.bat DX12 / VULKAN
```

五段式：① `selftest --verbose`（7 条 PASS 锚 findstr 断言）→ ② `contact-trace` 重生成 `contacts.tsv` 并 `fc /b` 逐字节自稳 + `contact-check` → ③ DX12/VULKAN trace 逐字节一致（VULKAN 树存在时）→ ④ 负向退出码 2/1 → ⑤ 两张证据图重生成 + `fc /b` 逐字节比对基线 → `ALL GREEN` exit 0。

| 文件 | 来源 |
|---|---|
| `example/drive-m0/contacts.tsv` | `contact-trace --frames 240` 真实输出 |
| `{selftest,contact-trace,contact-check,cross-backend,usage,negative}.log` | run.bat 捕获的真实工具输出 |
| `events.png` | `tools/make-drive-m0-evidence.py` 从 TSV 渲染：事件时间线三行 + f44/f46/f185 标注 + y(t) 曲线 + 接触点标记 + MEASURED 面板 |
| `acceptance.png` | 同一脚本从真实 logs 渲染的终端风格验收板 |

两张图均为固定画布 / 配色 / 字体、无时间戳，重生成后 `fc /b` 逐字节一致；**禁手绘示意**。

展示台：`example/cases.json` `drive-m0` 版本卡 5 用例（usage / selftest / contact-trace / contact-check / negative），`python tools/showcase/server.py --check` exit 0。

## 6. ctest

```
d0-contact-trace      contact-trace --frames 240 --out <build>/phys-d0-contacts.tsv
d0-contact-check      contact-check <上述 TSV>（DEPENDS trace）
d0-contact-negative   contact-trace --frames 0（WILL_FAIL）
```

命令统一带 `--repeat until-pass:3`（便携 cmake 3.31.6 快进程 stdout 捕获竞态，见 `AGENTS.md` 4b）；裸 shell 无 ctest，走全路径 `E:/UE_STUDY_2026/.build-tools/cmake-3.31.6-windows-x86_64/bin/ctest.exe`。

## 7. 踩坑

1. **junction 共享文件挡 merge**：`.workbuddy` 为主树/wt junction 共享 + skip-worktree，主树新日志 commit 使 wt 工作区 blob 领先 index → `git merge master` 报「local changes would be overwritten」。按工作线粘性不在中途 merge，收尾 merge 回 master 自动带上。
2. **缓存 key 不能带 subshape id**：见 §2；按全 pair 哈希做键会在 compound shape 场景静默丢 owner，实测 6/6 removed 命中才算过关。
3. **锚机器反推**：先只写结构锚，实跑后由脚本从真实 TSV 生成 7 条数值锚；避免 M-I0/I1 手写锚两连犯。
4. **selftest 输出演进**：新增 `phys-d0 contacts: PASS` 一行，`phys-p0 selftest: PASS` 等既有锚文本一字未动；M-P0~P4 归档 TSV 基线零改动，phys-* 归档 log 是历史快照不重拍。
5. **行尾**：本树源码 / 文档 / bat 全 CRLF，cases.json 全 LF；Write 工具产出 LF，落盘前按目标文件换行规范转换，写完当场 `git diff --numstat` 核对无 churn。

## 8. 下一步（不属 M-D0）

M-D1：`ContactEventStream` 双缓冲 + owner→Entity 映射 + PostPhysics 消费 System + M-GL5 TickGroup 时序自证（Q1 的落地处）；M-D2：载具无头集成 + 轮接触事件锚；M-D3：viewer 驾驶训练场（本轨唯一渲染接线面）。
