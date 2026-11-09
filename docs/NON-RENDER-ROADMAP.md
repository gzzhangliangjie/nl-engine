# 非渲染方向模块候选 — 从头一步步实现的可行性评估

> 背景：用户原问题「UE 里还有什么模块能像 nanite+lumen 从头一步步实现」，澄清后范围为**整个引擎层面、非渲染方向的子系统**。本文档用于后续立项参考，非已实现代码。
>
> **用户拍板（2026-11-08）：有参考的内容就考虑从头写**——不因「有现成开源库可 vendored」而改走集成路线；参照实现（Carrot 从零参照 / UE 工业终态只读 / 开源库源码）用于对照与节奏校准，不直接拿来用。
> **2026-11-09 修订（用户拍板）**：本原则对**寻路**例外——改走 **vendored Recast/Detour 集成路线**（工具链成熟 + 要与编辑器结合 + 从零工作量过大），成为继物理之后的第二个例外；其余候选仍按原原则。详 `docs/p8/P8-NAV-PLAN.md`（旧「从零写」6 步阶梯作废留痕）。
> **2026-11-09 三修（用户拍板）**：**音频**成为第三个例外——目标收窄为「场景 3D 音效 + 游戏内调试」，改走 **vendored miniaudio + 自写薄集成层**（原 OpenAL Soft + 自写解码器整链作废；旧计划对 Carrot decoders 的「从零写」是误读参照）。详 `docs/p8/P8-AUDIO-PLAN.md` §7。
> **2026-11-09 二次拍板（物理路线复核）**：用户确认「物理用现成的，没必要从头写，成本太高；纠结的只是用哪个」——**不自研求解器的方向不变**，例外理由改写为**成本取舍**（自研求解器成本过高、物理是 A 类并行轨道配角），**不再是「UE 口径 / 业界共识」**：UE5 官方自研 Chaos，Jolt 与 PhysX 均非 UE5 后端，原「UE 5.x 官方提供 Jolt 后端」已于当日勘误（`UE-DIFFS.md` D10）。候选评审后确认**继续走 Jolt 5.3.1**：本机唯一零联网可得的 C++ 求解器源码 / MIT 与项目一致 / 纯 CPU 零图形 SDK / C++17 只依赖 STL / 确定性已实测可锁 / 自带 DebugRenderer 与 Character 接口 / Carrot 2902 行集成层可作本地参照；换库（PhysX 无本机副本且 UE5 已移除、Bullet 无逐位确定性、Rapier 系 Rust、Havok 商业授权）需摘除 446 文件、重写集成层、作废本地参照、重定确定性契约、重做 M-P0 交代物，无对应口径收益。**UE 口径不参与本选型**，仅作只读对照。

---

## 筛选三原则（与渲染模块的 nanite+lumen 同源方法论）

一个模块适合「从头一步步跑通」，当且仅当下列三条同时成立：

1. **有现成可对照的参考实现**（UE 源码 + bevy + Carrot 三处口径，抄思路不抄代码；UE 只读不抄，受 EULA 约束）。
2. **天然能拆成可见里程碑**（出画面/出数据即验收，不是写完才算数）。
3. **建立在已有地基上**（当前地基：mesh 管线、compute、render graph、Slang 单源双后端、ImGui hud、`.nmesh` 离线管线）。

---

## 候选总表（非渲染方向）

| 模块 | 复用已有地基 | 增量阶梯（可见验收点） | 参考口径 | 评级 |
|---|---|---|---|---|
| **骨骼动画 + 蒙皮** | 最高，直接吃 mesh 管线 + 矩阵调色板 | 静态姿势 → 线性混合蒙皮(LBS) → 多 clip 混合 → 状态机/混合树 | UE Anim / bevy_animation / assimp | ★★★★★ **大部分已由动作系统轨道实现并合入 master**（feat/action-system：M-A1~M-A5.4 纯逻辑层 + CPU LBS 蒙皮 + viewer skin 模式 + 四资产演示页，merge 5a1912f；剩 GPU 调色板，等更大规模蒙皮资产再立项） |
| **ECS 架构 / 模块生命周期** | 中，是地基本身、要先铺 | entity 存储 → system 调度 → 并行 → 模块化插件 | bevy（纯 ECS 范本）/ Carrot（节点式） | ★★★★ 参考强但重构大 |
| **任务 / Job 并行系统** | 中，跨系统受益 | 线程池 → parallel_for → 任务图依赖 → 抢占 | bevy_tasks(Rayon) / UE TaskGraph | ★★★★ 验收看加速比 |
| **材质图 → Slang 编译** | 最高，吃 Slang 单源双后端 | 固定管线材质 → 节点图 → 生成 Slang → 热重载 | UE Material Editor / ReaUI | ★★★★ 最贴合技术栈 |
| **寻路（vendored Recast/Detour）** | 中，几何喂给上游 `rcConfig`、集成层挂 `nl/scene`；编辑器吃 ImGui/console cvar 现成基建 | **集成层可见验收**：单模型烘焙 → Detour 查询/路径平滑 → 编辑器调参重烘焙 + `DebugUtils` 五阶段可视化 → E1 多物体场景 / tiled 增量重烘（每步出 PNG 证据） | **上游 recastnavigation（ZLib，1.6.0，Recast/Detour/DebugUtils/DetourTileCache）** / UE `Runtime/Navmesh`（`Recast.tps` 实证 vendored Recast v1.4.2 + Detour，工业做法实证，只读）/ Carrot `pathfinding`（2336 行从零，降级为验收口径与编辑器交互参照）/ bevy 无原生 | ★★★★ **已立项 `docs/p8/P8-NAV-PLAN.md`（2026-11-09 改拍 vendored 集成）；执行排 P8 场景整合（E1）之后** |
| **GPU 粒子 / VFX** | 高，纯 compute | 单发射器 → 生灭 → SDF 碰撞 → 点/精灵/ribbon 渲染 | UE Niagara / bevy | ★★★ 自闭环 |
| **3D 空间音频 + 游戏内调试** | 低，独立子系统 | 单声源播放/离线确定性 dump → 3D 定位与距离衰减 → console cvar + ImGui 声源面板（solo/freeze/单步）→ E1 场景集成 | **miniaudio**（PD/MIT-0，单文件零依赖，内建 WAV/FLAC/MP3 解码 + ma_engine 3D 空间化）/ UE `AudioDebug.cpp`（只读定调试字段）/ Carrot `engine/audio/`（对象模型 + 绑定形状）/ bevy `SpatialListener` | ★★★ **已立项 `docs/p8/P8-AUDIO-PLAN.md`（2026-11-09 全量重写：vendored miniaudio + 薄集成层，旧 OpenAL Soft/自写解码器作废）**；执行排 P8 场景整合（E1）之后 |
| **物理（Jolt 集成）** | 低，CPU 侧独立子系统 | **集成层可见验收**：掉落弹跳 → 堆叠 → 角色胶囊 → 碰撞层 → Jolt debug 渲染接 ImGui | **Carrot `physics/`（Jolt 集成层 2902 行，集成方式首选参照）** / UE 5.8.4 自研 **Chaos**（只读；**官方无 Jolt 后端 / 亦无 PhysX SDK 源码**，2026-11-09 实读全树确认，勘误见 `UE-DIFFS.md` D10）/ bevy 本体无物理 crate、生态 avian | ★★★ **已立项 `docs/p8/P8-PHYS-PLAN.md`（vendored Jolt 不自研）；选型依据 = 成本取舍 + 工程适配，非 UE 口径（2026-11-09 二次拍板）；提前开工 M-P0~M-P3（worktree `feat/physics` 已建、Jolt 锁 v5.3.1），M-P5 场景集成仍排 P8 E3 之后** |
| **角色控制器** | 中低 | 胶囊扫掠 → 地面检测 → 斜坡 → 动画衔接 | UE CharacterMovement | ★★★ |
| **Lua / 脚本 VM 接入** | 低，但参考极多 | 绑定核心 API → 调用渲染 → 游戏逻辑脚本化 | sol2 / 诸多实现 | ★★★ |
| **网络同步 / 复制** | 低，体量爆炸 | 快照 → 状态同步 → 插值 → 回滚 | bevy_renet / UE replication | ★★ 里程碑不直观 |
| **完整编辑器** | 中，吃 ImGui hud | 视口 → gizmo → 属性面板 → 序列化 | bevy editor / UE editor | ★★ 工程量巨大 |

---

## 分桶结论

- **第一桶（最值得搞）**：骨骼动画、材质图、GPU 粒子、Job 系统——要么直接吃图形地基，要么参考现成，且每步都能出可见画面/数据。**寻路 2026-11-09 移出本桶**（改走 vendored 集成，见下方例外）。
- **第二桶（能搞但要从零铺一层）**：ECS、Lua——参考丰富，但跟现有 GPU/渲染地基耦合松，等于另开一条线。**3D 音频 2026-11-09 移出本桶**（改走 vendored miniaudio + 薄集成层，见下方例外）。
- **第三桶（慎入）**：网络同步、完整编辑器、视觉脚本——参考要么体量爆炸、要么里程碑不直观，容易「写完才算数」。
- **已拍板例外（2026-11-08 物理 / 2026-11-09 寻路、音频）**：物理不进三桶分类，走 **vendored Jolt 集成路线**——理由经 2026-11-09 二次拍板改为**成本取舍**（自研求解器成本过高、物理为 A 类配角），**不是 UE 口径或业界共识**（UE 自研 Chaos 恰恰是自己写求解器，见 `UE-DIFFS.md` D10）；选型评审确认继续用 Jolt 5.3.1（本机可得 / MIT / 纯 CPU / 确定性 / 自带接口 / Carrot 集成层参照）。**寻路同样走 vendored Recast/Detour 集成路线**（工具链成熟 + 编辑器结合 + 从零工作量过大，`Recast.tps` 与上游 README 实证工业做法）。**音频走 vendored miniaudio 集成路线**（目标只需 3D 音效 + 调试；OpenAL 的 HRTF/EFX 用不上而 LGPL/C++20/自写解码器成本全是净支出；FMOD/Wwise 属商业内容管线的目标分歧，见 `P8-AUDIO-PLAN.md` §1/§7）。三条均见候选总表与各自 `P8-*-PLAN.md`。

---

## Carrot 模块覆盖盘点（2026-11-08 对照）

Carrot 全模块（按代码规模降序）与本候选文档的覆盖情况：

| Carrot 模块 | 规模 | 我们覆盖情况 | 结论 |
|---|---|---|---|
| `render` | 23358 行 | ✅ 渲染主线 S0~S14 全向覆盖 | 已覆盖 |
| `ecs` | 7246 行 | ✅ 本表「ECS 架构」条目 | 已覆盖（候选拍板重） |
| `physics` | 2902 行 | ✅ 本表「物理（Jolt 集成）」条目——**已拍板（2026-11-08）vendored Jolt**，集成方式参照 Carrot | 已覆盖（拍板：不自研） |
| `vulkan` | 2790 行 | ✅ 双后端纪律 | 已覆盖 |
| `scripting` | 2465 行 | ✅ 本表「Lua / 脚本 VM」条目 | 已覆盖 |
| `pathfinding` | 2336 行 | ✅ 本表「寻路」条目——**从零实现仅作验收口径 / 编辑器交互参照**；实现走上游 vendored Recast/Detour（2026-11-09 修订） | 已覆盖（方案已变更） |
| `audio` | 1358 行 | ✅ 本表「3D 空间音频」条目——**仅抄对象模型与绑定形状**（AudioManager/Source/Listener/SFX-vs-Music）；其 `AudioThread` 独立混音线程与 OpenAL 底座不抄；`decoders/` 实为 dr_libs/stb_vorbis 薄封装，是「不自写解码器」的实证 | 已覆盖（方案已变更） |
| `io` | 1133 行（异步资源加载，`AsyncResource`） | ⚠️ 部分——V4 几何分页 + Job 系统已覆盖其核心诉求 | 不单列 |
| `vr` | 1121 行（Session / HandTracking / VRInterface，OpenXR 式） | ❌ 未覆盖 | **不建议排**：验收依赖 VR 硬件，与项目场景无交集 |
| `network` | 888 行 | ✅ 本表「网络同步」条目（★★ 慎入） | 已覆盖 |
| `scene` | 846 行 | ✅ 主线 P8 引擎化覆盖（场景图/变换层级） | 已覆盖 |
| `edition` | 816 行（EditorSettings / FreeCamera / DragDrop / Widgets） | ⚠️ 隐含在本表「完整编辑器」条目内 | 不单列，编辑器立项时一并纳入 |
| `task` | 549 行 | ✅ **已立项** `docs/p8/P8-JOB-PLAN.md` | 已覆盖 |
| `assets` | 450 行 | ✅ `.nmesh` 离线管线已覆盖 | 已覆盖 |
| `console` | 369 行（命令注册 + ImGui 渲染 + 自动补全 + `RuntimeOption` 运行时调参） | ✅ **已立项（2026-11-08）**：`docs/p8/P8-CONSOLE-PLAN.md`（M-C0~M-C3），worktree `feat/console` 已建；两层架构（`core/console/` 纯逻辑 + renderer 侧 ImGui 前端）；不依赖场景整合，随时可开工 | 小而独立，吃 ImGui 地基，可见验收直观，对日常调试立刻有用 |
| `utils` / 根级（Window / Settings / LoadingScreen / Capabilities） | — | 通用基础设施 | 不算独立方向 |

---

## 优先推荐

1. **骨骼动画 + 蒙皮** — 让引擎从 demo 变成能跑真实资产的临门一脚，精确踩中 mesh 管线地基。
2. **材质图 → Slang** — 精确踩中 Slang 单源双后端地基，最贴合现有技术栈，且能反哺所有渲染模块。
3. **Job / 并行系统** — 底层地基工程，建议作为 nl-engine 的**重构轨道**单独排，不与渲染 S 轨混编。

> ECS / Job 属底层重构，里程碑不直观（验收看加速比/架构清晰），立项时应与渲染 S 轨明确隔离，避免范围互相污染。

---

## 后续动作（待用户圈定）

选定方向后，按 `docs/s*/S*-PLAN.md` 格式展开里程碑草案，执行前先按 `AGENTS.md` 铁律读三处参考代码 + 开发历史做对照。本文档仅作候选清单与优先级备忘。
