# P8-ECS-PLAN — ECS-lite 场景系统（P8 E1 拆分段）

> A 类并行轨道（非渲染 / 引擎化），固定 worktree `feat/ecs`（`E:/UE_STUDY_2026/nl-engine-ecs`，已建，junction 共享）。
> 2026-11-09 用户拍板立项：**Carrot 式 OOP 组件路线**；E1a（纯逻辑内核）先行开 worktree，E1b（接 viewer 渲染）后挂等 S6 收官。
> 动机：寻路 / 物理 / 音频三条已立项轨道全部依赖场景整合（P8 E1），E1 是下游瓶颈，提前做 = 提前解锁三条轨（2026-11-08 排期拍板只把三条下游排在 E1 **之后**，未禁止 E1 提前）。
> 对应总路线 `docs/ROADMAP.md` 阶段 P8 表 E1 行；并行纪律见 `AGENTS.md`「并行轨道 worktree 纪律」与 `docs/PARALLEL-TRACKS.md` §7。

## 里程碑编号约定

- 本计划 **M-E0~E3 = ROADMAP「P8 E1」的 E1a 拆分段**（纯逻辑内核，A 类）；E1b（渲染集成）为后挂段，里程碑届时另编号。
- 任务号 = 里程碑编号（wt_pool 占坑闸口径）：`python tools/wt_pool.py claim --track ecs --milestone M-E0`。

## 目标

给引擎一个 **Carrot 式 OOP 组件场景内核**：`World`（实体注册表 + 组件存储 + 层级）+ `Entity`（轻量引用）+ `Component`（继承基类 + 工厂注册）+ `System`（按 Signature 匹配实体列表，tick 生命周期）+ 场景 JSON 序列化。**50 物体 + 8 灯规模，不追 bevy archetype 级完备度。**

## 三处参考口径（2026-11-09 精读留痕）

| 来源 | 精读文件 | 怎么做 | 性质 |
|---|---|---|---|
| **Carrot**（节奏参照首选，用户拍板按它来） | `engine/ecs/` 全文 2006 行（`World.h/.cpp` 239+931、`EntityTypes.h` 195、`Component.h` 111、`TransformComponent.h`、`System.h` 143、`math/Transform.h` 42）+ `engine/scene/Scene.h` | OOP 组件式：`World::entityComponents` = `unordered_map<EntityID, unordered_map<ComponentID, unique_ptr<Component>>>`；`Entity` = {UUID, World*} 轻壳不持组件；Component 基类 virtual `serialise/deserialise/duplicate/reload/unload` + `IdentifiableComponent<Self>` CRTP 注册类型 ID；`ComponentLibrary` 字符串 ID→工厂；Signature（组件位集）匹配 + 查询缓存 + `invalidateQueries`；`SignedSystem<Required...>` 模板 + Logic/Render 两类 + `tick/prePhysics/postPhysics/onFrame/swapBuffers` 生命周期；延迟增删 `entitiesToAdd/Remove` + `flushEntityCreationAndRemoval`；层级 = `entityParents`/`entityChildren` 双 map + `setParent/reparent`（reparent 保世界变换）；`Transform` = pos+scale+quat，`operator*` 复合，`toTransformMatrix(parent)` | **从零节奏参照**；Transform/Component/World 三层逐条对标 |
| **bevy_ecs**（对照，不对标完备度） | `src/` 模块清单 + `hierarchy.rs`（ChildOf/Children relationship）+ `entity_disabling.rs` | archetype 全功能 ECS——**只借语义不抄结构**：① 父删递归删子（ChildOf 语义，Carrot duplicate 递归同向）；② Disabled 是组件不是删除（查询默认过滤）；③ 层级 source of truth 是「子持父引用」（我们走 Carrot 双 map，效果等价） | 反向印证别过度设计；借两条语义 |
| **UE GameFramework**（只读） | `Actor.h`（RootComponent / GetAttachParent / GetComponentTransform 口径） | Actor 变换层级 = USceneComponent 挂接树；local-to-world 逐级乘同 Carrot `computeGlobalXxx` | 工业终态口径确认，只读不抄 |

**历史拍板**：2026-11-08 排期拍板（下游三轨排 E1 后）+ M-A1 自写最小数学不引 glm（anim_math.hpp 文件头）+ 2026-11-09 本轮拍板（Carrot 式 OOP / E1a 先行 / worktree 开）。

## 架构约束

- **模块落点**：`core/include/nl/scene/`（hpp）+ `core/src/scene/`（cpp），对齐 `nl/anim` 先例；零图形符号（NVRHI/ImGui 零 include），不碰 kStage、不进渲染回归。
- **数学（已执行，2026-11-09 拍板）**：通用数学（Vec3/Quat 及函数）已上移 `core/include/nl/math/math.hpp`（namespace `nl::math`）；`anim_math.hpp` 已删除，`anim.hpp` 直接 include nl/math 并定向注入类型 `Vec3/Quat/quatIdentity`（API 契约），anim 内部 cpp 用文件级 `using namespace nl::math;`，外部消费方（anim-tools）数学函数改用 `nl::math::` 限定。**用户拍板不设转发头**。ECS 的 Transform 层级数学（非均匀 scale 复合 / mat4）M-E1 起在 nl/math 新增（与 nl/anim::Mat4 合并时机届时定）；本次重构零行为变化（双后端构建 + anim 全系列 selftest 验证）。
- **EntityID = u64 递增**（不用 Carrot 的 UUID：无网络/多编辑器场景，50 物体规模 u64 最简直接；序列化存数值）。组件类型 ID = `bitset<64>` 位（Signature），类型数 <64。
- **序列化 = JSON**（tinygltf vendored 内嵌 nlohmann json.hpp，零新依赖）：场景文件 = 实体树 + 逐实体组件数组 + 组件名→工厂反序列化。**roundtrip 字节一致是硬判据**（序列化输出必须 key 排序 / 插入序固定——材质图轨道踩过 unordered_map 迭代序坑）。
- **砍掉的范围（对 Carrot 有意简化，留痕）**：① 查询缓存 + invalidateQueries 砍掉——50 物体规模直接遍历 O(N) 足够，卡了再加；② ComponentReflection / FIELD 宏 / Inspector 砍掉——那是 E3 序列化 Inspector 的事；③ Prefab 砍掉——远期；④ repairLinks / CSharp / RenderSystem 分家砍掉——E1a 无引用型组件无渲染；⑤ rapidjson 换 tinygltf 内嵌 json。

## 里程碑（可见验收阶梯）

| 里程碑 | 内容 | 验收判据（确定性，亲眼可验） |
|---|---|---|
| **M-E0** | World/Entity 核心：实体创建（递增 ID）/ 延迟增删 flush / 名字 / Flags(Hidden) / exists / getAllEntities；`nanite-scene` 工具骨架 + `selftest --suite ecs-core` | selftest 断言绿：增删 flush 前后存在性、名字读写、Hidden 过滤；tick 中删实体下一 tick 生效（延迟语义锚点）**✅ 已完成**（2026-11-09，feat/ecs tag `ecs-e0`：ctest e0 系 2/2，四栏台账 MILESTONES M-E0 节） |
| **M-E1** ✅ 已完成（feat/ecs tag `ecs-e1`，2026-11-09） | Component 基类（serialise/deserialise/duplicate/getComponentTypeID）+ ComponentLibrary 注册工厂 + `Transform`（pos/scale/quat）+ 层级：setParent / reparent 保世界变换 / 父删递归删子 / local→world 复合 | selftest 锚点：三层父子链复合矩阵数值锚（手算对照）；reparent 后世界变换不变；删父后子不存在；**duplicate 实体含组件深拷贝 |
| **M-E2** ✅ 已完成（tag `ecs-e2`，2026-11-09） | System 基类（Signature 匹配 + 实体列表自动维护 + tick/firstTick 生命周期）+ `SignedSystem<Required...>` + 场景 JSON 序列化/反序列化 **roundtrip 字节一致** | selftest：System 只收 Signature 匹配实体（加/删组件自动进出列表）；roundtrip = `serialise→deserialise→serialise` 逐字节一致（正/负向：非法 JSON / 未知组件名显式退出码 2） |
| **M-E3** ✅ 已完成（2026-11-09，tag `ecs-e3`） | 收口：工具版交代（展示台 tool 卡 usage/正向/负向 + TOOLS-GUIDE）+ MILESTONES 四栏 + tag + merge 回 master + 注册表同步 | §7.2 文档闸过；cases.json `p8-ecs` 卡 4 条全过（expectExit 2/0/0/2）+ run.bat ALL GREEN + 4 张证据图亲眼验收；master 侧 AGENTS/PARALLEL-TRACKS 状态更新 |

## E1b 后挂段（渲染集成）—— M-E1b.0 + M-E1b.1 + M-E1b.2 已完成（2026-11-09）

> S6 收官（tag `s6-done`）后解禁，`feat/ecs` worktree 内开工；**里程碑编号定为 M-E1b.0**（E1a 的 M-E0~E3 已收官）。
> 用户拍板：① 多灯 = **方向灯数组**（点光源留 pbr-direct merge 后）；② **不 bump kStage**（保持 S6.2），照 console 轨道先例，证据走 example 卡 + 截图 + `--only` 抽查。

- **已落地**：`ModelComponent` / `LightComponent` 纯数据组件进 `nl/scene`；`viewer --scene <World JSON>` 反序列化后 O(N) 收集 → `Renderer::loadEcsScene` → `Scene::initScene` 按 meshFile 分组实例化；`vsScene/psScene` 新入口对 + t5 实例缓冲 / t6 灯缓冲，既有 shader 入口与既有 draw 路径逐字节不动。
- **验收（M-E1b.0）**：`example/ecs-e1b/`（5 物体 + 3 方向灯，ambient 0.22）双后端 `deviceErrors=0`；`regression.py --only ecs-e1b` 2/2 bytes，`--only s6.2` 10/10 bytes（kStage 未动、既有路径零回归）；双后端画面互比 3px/maxDelta2（超 near 阈值，GPU 取整噪声，留精度项）。台账见 `docs/MILESTONES.md` M-E1b.0 段。
- **M-E1b.1 规模锚已达成（2026-11-09）**：`ecs-e1b-scale.scene` = 10×5 网格 × 4 种 mesh（13/13/12/12 实例）= **50 物体 + 8 方向灯 / 4 batch / 58 实体**，双后端 `deviceErrors=0`、`--only ecs-e1b` 4/4 bytes、`--only s6.2` 10/10 bytes；`run-scale.bat` 机器断言。三处参考复核多实例 / 多灯容器均无硬上限（Carrot 逐实体 `InstanceData` / UE `DirectionalLights` 动态数组 / bevy clustered forward），不改架构。顺带修掉 M-E1b.0 的多 batch 实例化 bug：DX12 的 `SV_InstanceID` 不含 `StartInstanceLocation`，改为每 batch 各持自己的 t5 实例缓冲（详见 `docs/SHADER-CONVENTIONS.md` §2.10 与 MILESTONES M-E1b.1 踩坑①），M-E1b.0 基线 MD5 逐字节不变。
- **M-E1b.2 点光源已达成（2026-11-09）**：`LightComponent` 增 `type`/`position`/`range`（方向灯不写新键，向后兼容），t6 灯缓冲每灯扩为 3 个 float4，`vsScene` 增 `WORLDPOS` 插值量；衰减照抄 UE `DeferredLightingCommon.ush:480-486`，点光绝对强度独立累加、方向灯 `1/ΣI` 归一化，无点光时走 M-E1b.0 原表达式分支。证据 `example/ecs-e1b/ecs-e1b-point.scene`（7 白模 + 3 彩色点光 + 1 弱方向灯），双后端 `deviceErrors=0`、`--only ecs-e1b` 6/6 bytes、`--only s6.2` 10/10 bytes，点光双后端画面 MD5 完全一致。三处参考实读：Carrot `GPULight::LightType`+union / bevy `PointLight{color,intensity,range,radius}` / UE `UPointLightComponent`（反平方 + AttenuationRadius）。
- **未完成**：「50 物体 + 8 动态灯 GI 正确响应」完整验收仍卡主线 S10 之后；PBR 材质响应 / 阴影 / clustered 灯池留 E1b.3+（前置 = E1 轨道收官 merge 同步 master 的 pbr-direct，差异见 `docs/s5/UE-DIFFS.md` `D-E1b-1`）。
- 三轮均只在本 worktree 提交 + tag，轨道未收官、不 merge 回 master：只读 `git merge-tree` 探查显示同步 master 会撞至少 12 个文件冲突（S7 visbuf + pbr-direct 与 E1b 的 `renderer/*` / `model.slang` / `modes.hpp` 全重叠），留待收官时统一解。

## 排期

不依赖任何渲染步骤，**随时可开工**；体量参照 console 轨道（2~4 业余日）+ 序列化一段，预计 **3~5 个业余日**。worktree 按 §7 惯例新建：`E:/UE_STUDY_2026/nl-engine-ecs`，junction 共享 `.workbuddy` 与 `thirdparty/slang/dist`。

## 验收形式（A 类专属）

- 工具 = `nanite-scene`（独立小工具，对齐 nanite-console 先例）：`selftest --suite <名>` 确定性断言 + 场景文件 roundtrip。
- 展示台 tool 卡三条起步（usage / 正向 roundtrip / 负向非法输入），`expectExit` 0/1/2；不进 bytes/near 渲染回归总闸。

## 双后端纪律

本轨道 E1a 不新增 GPU 资源 / shader，不触发双后端画面回归；E1b 进渲染后按主线纪律执行。

## 底线

- 「50 物体 + 8 灯」是规模锚：**任何设计若为百万实体优化即为过度设计**（bevy 对照的边界）。
- 层级 / 延迟增删 / roundtrip 三条是下游三轨（寻路 / 物理 / 音频）挂接的硬前提，M-E1/M-E2 判据不许降。
