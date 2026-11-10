# 城市驾驶 + 交通调度 — 开发顺序总表（2026-11-10 固化）

> 来源：工程能力 review + 用户方向拍板「城市大场景，自由开车，交通调度系统」（对话记录见
> `.workbuddy/memory/2026-11-10.md`「游戏方向拍板」节）。本文档把当时的开发顺序对话
> 结论固化为正式台账，作为后续各轨道立项 / 排期的**唯一数据源**；各步开工时按
> `AGENTS.md` 铁律读三处参考 + 出对应 `*-PLAN.md`。
>
> 排序原则：每一步独立可验收、按现有轨道纪律走（A/B 类、tag 名字制、不碰 `kStage`）、
> 单人串行执行；无硬依赖的打磨项见缝插针。

## 1. 方向与底子判定

- **契合度**：城市大场景正是引擎底子的目标形态——DAG 屏幕误差 LOD（S5）、Hi-Z 遮挡（S6）、
  GPU 视锥剔除（S4）全是为一眼望不到头的城市服务的；P9 真实资产验收关（V1~V4）的目标
  形态就是它。
- **现成能力**：物理（Jolt，M-P0~P4 已收口）、音频（miniaudio）、ECS（E1a/E1b）、
  动画（M-A 系列）、GameClock 双帧（M-GL0 收口）、job 系统、console cvar、
  寻路（M-N0 收口，Recast/Detour vendored）、粒子（M-PT0 在轨）。
- **载具物理**：Jolt 5.3.1 自带 `VehicleConstraint`（轮式车辆），vendored 已在库内，
  只需 nl 集成层（照 phys 集成层纪律）；原「角色控制器」缺口降级为行人需求。
- **P9 V4 流式分页从远期变核心前置**：自由开车相机满城跑，城市规模稍大即超显存。

## 2. 开发顺序（11 步）

| 序 | 内容 | 验收形态 | 前置 | 状态 |
|---|---|---|---|---|
| **1** | 输入层扩展（全键位 + action mapping） | tool 卡 + selftest，core 侧零图形 | 无 | **在轨**：track `input`，`docs/p8/P8-INPUT-PLAN.md`，M-I0 落码中 |
| **2** | 碰撞事件桥（Jolt ContactListener → ECS 事件） | selftest 数值锚（命中事件时序），照 phys 先例 | 无，可与 1 并行 | **已立项**：`docs/p8/P8-COLLISION-PLAN.md`（track `collide`，M-CB0/CB1，2026-11-10），待开 wt |
| **3** | 载具物理（Jolt VehicleConstraint 薄集成层） | phys-tools 式：直线加速/转向/手刹参数表 + console 实调 | 2 | **已立项**：`docs/p8/P8-VEHICLE-PLAN.md`（track `vehicle`，M-V0/V1，2026-11-10；vendored Vehicle 模块全量实证在库），待开 wt |
| **4** | Chase 相机（车辆跟随） | viewer 截图基线（跟车视角双后端） | 3 | 待开工 |
| **5** | **游戏壳收拢**：独立 game target，GameClock 逻辑帧串起输入→载具→相机→World | 「能开一圈」截图 + 暂停语义回归 | 1+3+4 | 待开工（Q2 定 target 形态） |
| **6** | 程序化城市生成工具（blocks/楼房/道路 → `.nmesh` + 路网数据） | 离线工具卡：生成 → 渲染城市俯视/街景证据图 | Q-B 拍板 | 待开工 |
| **7** | 小规模程序化城市 + 载具驾驶闭环 | **第一个 tag 门槛：最小可玩版**——能在生成的城市里开一圈、撞了有事件反馈 | 5+6 | 待开工 |
| **8** | 交通系统 M-T0：lane graph 数据结构 + 路网寻路 + 直线跟车 | 纯逻辑 selftest 数值锚（确定性逐位）+ debug 可视化画车道线 | 6 的路网数据 | 待开工（Q-A 拍板自研立项） |
| **9** | 调度器迭代：红绿灯相位、路口让行、避撞 | 同上 + 场景截图（车流不穿模、路口不堵死） | 8 | 待开工 |
| **10** | 流式分页（城市规模拉到超显存） | V4 口径：分页加载/卸载、帧时间平稳 | Q-C 拍板 | 待开工 |
| **11** | 打磨波：游戏 UI（ImGui 先凑 / RmlUi 可选立项）、引擎音效接玩法、粒子轮胎烟（M-PT0）、行人 NPC（Recast 接）、存档 | 各自独立小步 | 无硬依赖 | 部分在轨（M-PT0） |

## 3. 结构性判断（定档留痕）

- **1~5 是一条硬链**，做完即「空场景能开车」；6 完成后 7 一合拢就是第一个可玩版本
  （最小可玩 tag）。
- **8~9（交通调度）是纯逻辑层**：跑在 GameClock 60Hz 逻辑帧、ECS System 承载、
  core 零图形依赖；验收走数值锚 + 确定性对拍（phys/audio 先例）；调试可视化照
  nav-m0 的 debug 模式口径。可与渲染侧任何一步并行开 wt 轨道。
- **交通调度不是 Recast 寻路**：M-N0 那套是行人/NPC 网格寻路口径；车辆交通 =
  路网图（车道节点 + 连接 + 红绿灯相位表）→ 图上路径规划 → 局部调度（跟车避撞 /
  路口让行）。开源参照 CARLA TrafficManager（BSD，看思路不抄代码）；无成熟 vendored
  C++ 库对口 → 按选型闸属「无成熟方案自研例外」，须单独拍板（Q-A）。
- **10（流式）的时机取决于 Q-B**：程序化城市第一版控制在显存内则可后移。
- **UI 口径**（2026-11-10 已议）：起步 ImGui 凑合（零新增依赖，速度表/小地图/暂停菜单
  全覆盖）；要打磨质感再立 RmlUi（MIT，唯一对口的正式开源方案；需 NVRHI
  RenderInterface + FreeType）；Noesis/Coherent/Sciter 商业许可不过选型闸，直接排除。
  中文文本是隐藏工作量——起步 HUD 全 ASCII + 数字（与现有 HUD 约定一致）。

## 4. 开工前待拍板（都是决策，不是活）

| 编号 | 问题 | 建议 |
|---|---|---|
| Q2 | 独立 `game/` target 还是扩 viewer？ | **独立 game target**（viewer 保持展示台口径） |
| Q-A | 交通调度自研立项（lane graph 起步的小步路线） | 按「无成熟方案自研例外」单独立项 |
| Q-B | 城市资产来源：程序化城市生成 vs 外部资产 | 程序化生成（与离线管线同源，顺带产路网数据，可一次产出超大场景验证剔除） |
| Q-C | 流式分页（P9 V4）提前与否 | 提为驾驶游戏核心里程碑；第一版城市显存内则可先用 V2 分块加载过渡 |

## 5. 变更记录

- 2026-11-10：固化本文档（用户指令）；输入层（第 1 步）已先行立项开工（track `input`，
  `P8-INPUT-PLAN.md`，M-I0 落码中）；碰撞事件桥证据：core 全目录 grep 零
  `ContactListener`/trigger 回调（review 实证）。
- 2026-11-10（续）：第 1 步 input 轨道 M-I0~I2 全收口合回主干（merge `0df16a0`）；第 2/3 步
  立项出计划（用户指令「碰撞事件桥和载具物理先出文档」）——`P8-COLLISION-PLAN.md`（track
  `collide`）+ `P8-VEHICLE-PLAN.md`（track `vehicle`）。vendored Jolt 实证：ContactListener.h
  与 Physics/Vehicle/ 全模块在库；载具前置实情修正：VehicleConstraint 走 raycast 轮，
  技术上不依赖第 2 步（软前置 = 玩法面），两轨可并行开 wt，文件重叠仅 CMakeLists.txt。
