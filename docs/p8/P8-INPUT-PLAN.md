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
| **M-I2** | viewer `--mode input-hud` 调试面板 + example 卡 + cases.json + 收口（close 硬闸） | 截图基线双后端 bytes；`server.py --check` exit 0；注入按钮肉眼可验 | ✅ **已收口**（tag `input-m2`，2026-11-10）：`ViewMode::InputHud=11` + `renderer/input_debug` 面板（增量兄弟窗口，默认关零布局）+ 注入管线（请求态局部持久 + 帧首转 feed 边沿 + feed→update→快照→endFrame 节奏）+ `--input-demo` 帧序号脚本（Esc PRESSED 落截图帧）+ pair 双后端 PNG 逐字节基线 + run.bat ALL GREEN + spot 回归 m0.9/s7.2 共 8/8 bytes 零回归；实测踩坑见 MILESTONES M-I2 节（悬空 SameLine 折叠行 / 可选值参数吞参 / HudFrame 每帧新建的请求态持久化） |

## 6. 留痕

- 2026-11-10：立项登记（用户指令「输入层扩展先给计划文档然后开 wt 开整」）；三处参考实读完成（本文件 §2）；track `input` 注册 wt_pool/PARALLEL-TRACKS/ROADMAP。
- 与主树 M8.1 TLAS 会话的冲突面：M-I0 会改 `viewer/main.cpp` 与 `CMakeLists.txt`（接线），这两个文件主树当前有**未提交改动**（另一对话 M8.1）——本轨改动全部在 wt 内进行，merge 回 master 时等 M8.1 收口后执行，冲突预期仅此两文件。

## 7. 第二批扩展：M-I3~M-I5（2026-11-15 立项，用户拍板「整体 review 全做」）

> 起因：键鼠能力面整体 review（本日会话）确认 M-I0~I2 状态/映射/HUD 已通，但存在七缺口。用户拍板全做。归属 track `input` 续编号；§7.1b 重开 wt（feat/input ff 到 master `3d17d42`，junction .workbuddy + slang dist，skip-worktree 已设）。

### 7.1 三处实读留痕（2026-11-15，M-I3~I5 点名文件）

| 来源 | 实读证据 | 用法 |
|---|---|---|
| **Carrot** `engine/io/actions/Action.hpp`（291 行全读） | 绑定路径已含鼠标四源 `/user/glfw/mouse/%d` / `pos` / `delta` / `wheel` / `delta_grabbed` + 手柄三源 `gamepad/%d/button|axis|vec2/%d`；`Action::wasJustPressed` = value && !previousValue（与 M-I0 边沿语义同构） | M-I3 鼠标裸查询 ops + M-I5 手柄源的**源集合锚**：本仓 ActionMap 六源（Key/MouseButton/ScrollX/ScrollY/DeltaX/DeltaY）已覆盖鼠标，缺手柄 → M-I5 增源照此路径口径 |
| **bevy** `crates/bevy_input/src/mouse.rs`（前 120 行定向读） | `MouseButton{Left,Right,Middle,Back,Forward,Other(u16)}` 五键枚举；`MouseMotion{delta}` 相对增量与 `MouseWheel` 分离 | M-I3 鼠标 API 面**语义锚**：按键三态 + 增量/滚轮/绝对位置三类查询（本仓 InputState M-I0 已有全部字段，只缺暴露） |
| **UE** `Engine/Classes/Engine/GameViewportClient.h:781` + `EngineBaseTypes.h:45`（grep 实读） | `SetMouseCaptureMode(EMouseCaptureMode)`；`EMouseCaptureMode{CapturePermanently, CapturePermanently_IncludingInitialMouseDown, CaptureDuringMouseDown}`；`PlayerController.h:735 DeprojectScreenPositionToWorld` | M-I3 指针锁定**模式锚**（本仓取 CapturePermanently 语义：旗标开=持续捕获、Esc 释放）；M-I4 拾取**反投影锚**（near/far 反投影，不抄 UE 代码） |

### 7.2 里程碑

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M-I3** | 脚本鼠标裸查询 API（exports.api 追加 6 op：MouseIsDown/MouseJustPressed/MouseJustReleased/MouseDelta/ScrollDelta/CursorPos；gen.py 五件套重生成）+ core `Window::setMouseCaptured`（GLFW CURSOR_DISABLED，取 UE CapturePermanently 语义）+ viewer `--mouse-capture` 旗标（缺省关=零漂移）+ selftest 新节（注入 feedMouseButton/feedMouseDelta/feedScroll/feedCursor → C# rc/值锚） | selftest 新节全绿 + s0~s11 零回归；`gen.py --check` 绿；双后端 BUILD + 便携 ctest；既有 input-m0/m1/m2 卡 spot bytes；tool 卡三条（usage/正向/负向） |
| **M-I4** | 屏幕拾取：viewer `--pick` 旗标 + `--pick-at <px,py>` 确定性注入（不依赖真实鼠标，demo 锚）；逆 VP 反投影 near/far → CPU 射线 vs 场景实体 AABB（glTF 顶点 AABB × 实体世界矩阵，slab 法）最近命中；**结果只进 log 锚（"pick -> ..."），零渲染路径改动（A 类零漂移）——M-I4 实做勘误：原计划「HUD pick 行 + PNG pair」会让拾取面触碰渲染路径，与 A 类零漂移口径矛盾，改为 log 锚 + tool 卡交代** | `--pick-at` demo 卡（image 卡跑真实场景，expectLog pick 锚）+ 未命中卡；缺省关零渲染差异（历史卡 spot bytes）；命中/未命中两态各有证据 |
| **M-I5** | `.actions.json` 重映射文件 + `ActionMap::loadFromFile/rebind`（**有意偏离 Carrot poll 后冻结**，编辑器需要，留痕；vendored nlohmann——scene 序列化同款依赖，§7.3 的「core 无 JSON」初判已勘误：nl_core PUBLIC tinygltf 目录本就带 json.hpp）+ window 拖拽（`glfwSetDropCallback` → `consumeDroppedPaths`，viewer `--drop-sim` 确定性旗标）+ 脚本字符暴露 `inputTextPop`（Blittable 单码点，window 侧 `consumeTextChars` M-C1 已有；宿主 `inputSetWindow` 绑 Window）+ 手柄（InputState gamepad 表 + feed 注入 + `Window::pollGamepad` 窗口路径 + ActionMap Source 增 GamepadButton/GamepadAxis + 脚本 `inputGamepadIsDown/inputGamepadAxis`） | input-tools selftest 新节 `actions-file`（装载正向/负向不动 map/rebind 冻结语义/手柄防御锚）+ nl-script selftest s15（C# 手柄导出锚）+ 既有锚零回归；tool 卡 + `--drop-sim` demo 卡；M-I3 契约零回归 |

### 7.3 有意决策留痕

- **脚本鼠标走裸查询 op 而非新 Action 源**：ActionMap 六源已含 MouseButton/Scroll/Delta（M-I1 落地），脚本经 Action 系统读鼠标已可用；裸查询补的是「不经映射直接读」的对称面（bevy ButtonInput 同构）。M-I5 手柄才是真增源。
- **指针锁定取 CapturePermanently 语义**（UE 三档取一）：`--mouse-capture` 开 = 获焦即捕获持续、Esc 释放重按再捕获；不做 CaptureDuringMouseDown 档（真要了再加）。
- **拾取反投影不抄 UE**：`DeprojectScreenPositionToWorld` 只作口径锚（near/far 反投影），实现为本仓手写 4x4 逆 + slab AABB。
- **重映射文件选 JSON 而非行式**（§7.3 初稿「core 无 JSON 解析器」已勘误）：nl_core 本就 PUBLIC tinygltf 目录（内嵌 vendored nlohmann json.hpp，P8-ECS-PLAN 拍板口径），scene 序列化先例在先——JSON 装载零新依赖且与场景文件同生态，行式格式作废。
- **确定性验收口径**：真实鼠标/拖拽事件不可自动化 → `--pick-at` / `--drop-sim` 注入旗标（M-NL2 夜景 demo 旗标同构教训：真实设备路径不进对拍）。
