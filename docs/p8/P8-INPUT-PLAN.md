# P8-INPUT-PLAN — 输入层扩展：全键位状态层 + Action 映射（A 类并行轨道，2026-11-10 立项）

> 起因：城市驾驶游戏方向拍板后（见 `.workbuddy/memory/2026-11-10.md`「游戏方向拍板」节），review 实证现有输入是 demo 级——`window.hpp` 的 `Key` 枚举仅 13 个键（相机/HUD 热键级）、鼠标只有轨道相机增量。写第一个玩法（载具控制）前必须先补。
> **性质**：A 类并行轨道（不碰 `kStage`、不进渲染回归），走工具版交代（selftest / 展示台 tool 卡 / 调试模式 example 卡）。
> **归属**：track `input`，里程碑 M-I0~M-I2；tag 名字制（`input-m0` 起），不占 m0.* 编号。

## 1. 目标

给引擎一个**游戏级输入层**，分两层、互不依赖渲染：

| 层 | 内容 | 落点 |
|---|---|---|
| **状态层（M-I0）** | 全键位键盘 + 全鼠标按键 + 光标/滚轮的**三态状态表**（isDown / justPressed / justReleased），每帧统一更新，边沿按帧清零 | `core/include/nl/core/input.hpp` + `core/src/input.cpp`（新模块，与 window 并列） |
| **映射层（M-I1）** | 具名 Action + 键位绑定 + 绑定解析（Bool=OR / Float=最大绝对值）+ ActionSet 组开关（驾驶/UI 模式切换） | 同模块上层（纯逻辑，零 GLFW 依赖） |
| **调试可见（M-I2）** | viewer `--mode input-hud` 实时显示键态/动作值面板 | viewer + renderer HUD 侧（唯一触碰渲染处，只加不改） |

**关键设计约束**（历史约定对齐）：
- 现有两条通道**零回归**：viewer 游戏热键边沿（`keyPressed(Key)`）与 ImGui 事件流（`consumeKeyEvents`/`consumeTextChars`）行为逐字节不变——M-I0 是**并列新增**，不是重构 window.cpp 的消费语义（M7.2 教训：主路径重构必须 spot 老路径）。
- core 零图形 API 纪律不变：input 模块可以 include GLFW 键位常量（window.cpp 已然），但 Action 映射层必须纯逻辑（selftest 无窗口可跑）。
- 确定性契约：selftest 走**事件注入 API**（合成 KeyEvent/鼠标序列喂进去），数值锚断言，逐位可复现——照 phys/audio 先例。
- C++17 / MSVC 14.38 / 零新第三方依赖。

## 2. 三处参考口径（2026-11-10 实读留痕；M-I0 开工已过，后续里程碑按铁律重读点名文件）

| 来源 | 实读证据 | 用法 |
|---|---|---|
| **Carrot** `engine/io/actions/`（README.md + Action.hpp + ActionSet.h 实读） | OpenXR 式 Action 系统：动作**预先声明**、ActionSet 首次 poll 后不可改、`InputState{value, previousValue}` 联合体承载边沿、绑定解析 = Bool 全部 OR / Float 最大绝对值 / Bool←标量阈值 0.5、一个 Action 可绑多个物理输入 | **映射层的锚**：Action/ActionSet/绑定解析三件套直接同构；VR 挛生部分（Pose/Vibration/OpenXR path）裁掉不抄 |
| **bevy** `crates/bevy_input/src/`（lib.rs + keyboard.rs 实读） | `ButtonInput<K>` 资源：`pressed / just_pressed / just_released` 三个集合，事件→状态表每帧统一更新（`keyboard_input_system`），KeyCode 用 W3C 物理键位（`Backquote` 等具名枚举 + Unidentified 兜底） | **状态层的锚**：三态模型 + 「事件批量进、边沿帧末清」的更新节奏直接照抄；物理键位口径对齐 GLFW（同为物理键位，映射表一一对应） |
| **UE EnhancedInput** `Plugins/EnhancedInput/Source/EnhancedInput/Public/`（InputMappingContext.h + 文件清单定向读） | InputAction + InputMappingContext（键→动作映射表，带 Modifier/Trigger 装饰器、上下文按优先级注册叠加、输入模式过滤） | 工业终态**只取思想**：映射上下文可注册/可移除/可分层；Modifier/Trigger 装饰器链**不做**（最小必要方案，真要了再加） |

## 3. 设计（逐里程碑细化，开工前按 M-I0 细化节补全）

### M-I0 状态层

- `Keys` 全键位：直接采用 GLFW 键码为物理键位（`GLFW_KEY_LAST` 上界定表长，具名常量包一层 `nl::core::Keys::`），**不学 bevy 自造 W3C 枚举**（GLFW 已是平台无关物理键位，再造一层只增加映射表维护成本）。
- `InputState`：定长数组状态表 + `update(const Window&)`（窗口事件→状态）+ `isDown/justPressed/justReleased(KeyCode)` + `endFrame()` 清边沿。
- **主循环节奏**（bevy 口径）：`window.pollEvents() → input.update(window) → 游戏读状态 → input.endFrame()`——边沿只在 endFrame 清，双帧模型（GameClock 逻辑帧）下由游戏壳决定喂几帧（起步：渲染帧级更新，逻辑帧读当前状态）。
- 鼠标：全按键（左/右/中/侧键×2）三态 + 光标绝对/相对 + 滚轮增量；`leftButtonDown` 老接口保留。
- **事件注入 API**（selftest 用）：`InputState` 接受合成事件序列（与 GLFW 回调同源数据），真窗口路径与注入路径走同一更新函数——确定性验收的关键。

### M-I1 映射层

- `ActionMap`：具名 Action（string→id 表）+ 类型（Bool/Float/Vec2）+ 绑定列表（键码/鼠标键/滚轮/光标轴）。
- 解析规则照 Carrot：Bool = 所有绑定 OR；Float = 所有绑定最大绝对值（W/S→油门这类轴 = 两个 Float 绑定带符号）；键→Bool 值 = isDown；键→Float = 1.0/0.0。
- `ActionSet`：组开关（驾驶/UI/回放），`setActive(name, bool)`，轮询只取激活组的动作——Carrot「预声明、poll 后冻结」口径照抄。
- 默认驾驶映射表（W/S=油门+刹车、A/D=转向、Space=手刹、C=视角切换）作为 C++ 常量落库，游戏壳直接用。

### M-I2 调试模式

- viewer `--mode input-hud`：HUD 面板实时画键态矩阵（isDown 高亮 + justPressed 闪一帧）+ 激活 ActionSet 的动作值条；ImGui 侧加「注入模拟按键」按钮（肉眼可验，不依赖人手敲键盘）。
- example 卡 + cases.json（image 用例 1 条 pair：注入序列 → 面板状态可断言）+ 展示台 tool 卡（input selftest 三条）。

## 4. 不在范围

- 手柄/游戏板（bevy gamepad、GLFW gamepad API）——驾驶键鼠起步够用，真要了按同架构加。
- 触摸、手写输入法事件（IME 已在 M-C1 全窗口解除关联，维持现状）。
- Modifier/Trigger 装饰器链（UE 重工程）、键位运行时重绑定 UI（数据结构留好，UI 后置）、RmlUi 接入（UI 方案已单独拍板 ImGui 起步）。
- window.cpp 既有热键/ImGui 通道的重构——零回归铁律，M-I0 不动它们。

## 5. 里程碑草案

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M-I0** | `core/input` 状态层（全键位 + 三态 + 鼠标全量 + 注入 API）+ selftest + CMake/ctest 接线；viewer 现有热键回归 spot（--only 挑 2~3 条） | selftest 数值锚全绿；注入序列 → 三态逐位锚；双后端 spot 零回归；tool 卡 usage/正向/负向 | ✅ **已收口**（tag `input-m0`，2026-11-10）：selftest 7 节 39 断言全绿、trace→check 5 锚闭环、证据图 fc /b 逐字节、spot 18/18 bytes、run.bat ALL GREEN、`server.py --check` exit 0；实测踩坑见 MILESTONES M-I0 节（bat CRLF / if 块内括号 / 锚禁手写 / glfw 链接面） |
| **M-I1** | ActionMap/ActionSet 映射层 + 默认驾驶映射表 + selftest（合成事件 → 动作值锚） | selftest 全绿（含 OR/max 解析、组开关冻结语义）；tool 卡三条 | ✅ **已收口**（tag `input-m1`，2026-11-10）：`core/action_map` 三型动作 + 六源绑定 + Carrot 解析规则（Bool=OR / Float=max-abs 平局取先定义 / Bool←浮点源 0.5 阈值 / Vec2 additive）+ 位掩码组开关（关停=清零，有意偏离 Carrot 留痕）+ `buildDrivingActionMap` 落库；selftest 9 节 60 断言、act-trace→act-check 12 行 9 锚闭环、证据图 fc /b 逐字节、M-I0 契约零回归（trace/check 5 锚逐字节不变 + input-m0 run.bat ALL GREEN + s7.2 spot 4/4 bytes）、ctest 4/4 |
| **M-I2** | viewer `--mode input-hud` 调试面板 + example 卡 + cases.json + 收口（close 硬闸） | 截图基线双后端 bytes；`server.py --check` exit 0；注入按钮肉眼可验 |

## 6. 留痕

- 2026-11-10：立项登记（用户指令「输入层扩展先给计划文档然后开 wt 开整」）；三处参考实读完成（本文件 §2）；track `input` 注册 wt_pool/PARALLEL-TRACKS/ROADMAP。
- 与主树 M8.1 TLAS 会话的冲突面：M-I0 会改 `viewer/main.cpp` 与 `CMakeLists.txt`（接线），这两个文件主树当前有**未提交改动**（另一对话 M8.1）——本轨改动全部在 wt 内进行，merge 回 master 时等 M8.1 收口后执行，冲突预期仅此两文件。
