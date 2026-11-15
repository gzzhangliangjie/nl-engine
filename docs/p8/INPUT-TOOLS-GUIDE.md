# INPUT-TOOLS-GUIDE — input-tools 使用指南（P8 M-I0 归档）

> 工具版交代归档（ROADMAP「工具用例约定」口径）。工具 exe 恒指当前构建
> `build/{DX12,VULKAN}/input-tools.exe`；展示台卡见 `example/cases.json` `input-m0` 版本卡。

## 1. 定位

`core/input` 输入状态层（M-I0）的无头验收工具：**无窗口、无 GPU、零图形 API**，
全部锚点走合成事件注入路径——注入与未来窗口路径（`updateFromWindow`）共用同一
更新内核（`applyKey` / `applyMouseButton`），锚对内核背书，窗口路径接线后自动继承。

## 2. 子命令

| 命令 | 形态 | 退出码 |
|---|---|---|
| `selftest [section]` | 9 节机械锚：`edges` / `combos` / `repeat` / `mouse` / `oob` / `mods` / `determinism`（M-I0 状态层 7 节）+ `actions`（M-I1 映射层：OR/max-abs/平局/阈值 0.5/Vec2 additive/组开关冻结）/ `actions-det`（12 帧驾驶映射序列逐位确定性）；无参 = 全跑 | 0=PASS / 1=断言失败 |
| `trace [--frames N]` | 确定性 12 帧驾驶序列 → TSV（15 列，frame + 三态 9 列 + dx/dy/scroll + mods 3 列）；真实工具输出，evidence 管线数据源 | 0 / 1（参数错） |
| `check <tsv>` | 对 dump 出的 TSV 重验同一组 5 锚（12 行 + f0/f1/f3/f10/f11）——防假绿：不信任存档，逐行解析断言 | 0 / 1 |
| `act-trace [--frames N]` | M-I1：12 帧驾驶映射序列 → TSV（9 列：throttle/steer/look xy/handbrake 及边沿/pause 及边沿；含 f8 ui 组开/f9 关停事件） | 0 / 1（参数错） |
| `act-check <tsv>` | 对 dump 出的 act-trace 重验 9 锚（12 行 + f0/f2/f4 平局/f6/f8/f9/f10/f11） | 0 / 1 |
| （无参 / 未知命令） | usage | 2 |

退出码语义：`0` 成功 / `1` 断言或运行失败 / `2` 用法错误（对齐展示台 expectExit 约定）。

## 3. 实跑输出摘录（正向）

```
$ input-tools selftest
[edges]
  ok GLFW_KEY_W == 87
  ...
[determinism]
  ok same sequence replays bit-identical across 12 frames
  ok seq anchor: f11 delta values (0.5*(11%4), -0.25*(11%3))
selftest PASS (9 sections)

$ input-tools check example/input-m0/input-trace.tsv
  ok check: 12 rows
  ...
check PASS (12 rows, 5 anchors)
```

## 4. 负向路径与退出码

| 输入 | 输出 | 退出码 |
|---|---|---|
| `selftest bogus` | `ERROR: unknown selftest section 'bogus'` + usage | 2 |
| `check <不存在的文件>` | `ERROR: cannot open '...'` | 1 |
| `trace --frames 0` / `--frames -1` | `ERROR: bad --frames ...` | 1 |
| `trace --bogus` | `ERROR: unknown trace option '--bogus'` | 1 |

## 5. 证据图

`example/input-m0/acceptance.png` 由 `tools/make-input-m0-evidence.py` 从
`example/input-m0/input-trace.tsv`（**真实 trace 输出**）渲染：八行状态格时间线
（green=isDown / yellow=justPressed / orange=justReleased）+ 鼠标 dx 曲线 + scroll
三角标记；输出确定性（固定画布/配色，无时间戳），`run.bat` 重生成后 `fc /b`
逐字节比对基线。禁手绘、禁他版本截图顶替。

## 6. 一键复现

```
example\input-m0\run.bat        # 前置：scripts\build-m0.2.bat DX12 / VULKAN
```

五段式：双后端 selftest（findstr PASS 断言）→ trace+check → 证据图 fc /b →
两条负向退出码 → ALL GREEN exit 0。

## 7. ctest

```
mi0-input-selftest          # selftest 全节
mi0-input-usage-negative    # selftest bogus → 非 0（WILL_FAIL 口径）
```

## M-I2：viewer `--mode input-hud` 调试面板（2026-11-10，tag `input-m2`）

viewer 侧的调试可见层（唯一触碰渲染处，只加不改）。与 input-tools 互补：工具版验数值锚，面板版肉眼看注入语义。

- **入口**：`viewer.exe --mode input-hud`（S0 分发器只加 case；normal 场景 Suzanne 打底）。面板是常驻 HUD 右侧的独立 ImGui 窗口，默认关零布局——非 input-hud 模式基线一像素不动。
- **数据流（单消费者铁律）**：viewer 绝不调 `updateFromWindow`（与 ImGui 键盘通道抢队列）。输入全走注入 API：面板 checkbox（W/A/S/D/Space/Esc/LMB 双向态 + release all + ui set 开关）→ 请求态持久在 runNormal 局部 → renderFrame 后读回 → 下帧首转 `feedKey/feedMouseButton` 边沿；帧节奏 feed → `ActionMap::update` → 面板快照 → `endFrame`。面板底部明示 `window-path input: off`。
- **确定性截图**：`--input-demo [start]` 帧序号驱动注入脚本（W@start / D@+1 / Space@+2 / Esc@+3，只 press；可选起始迭代号缺省 5，无值形式直跟其它 --arg 不吞参）。配 `--frames 8` 时 Esc 的 PRESSED 边沿正好落在截图帧：面板可见 W/D/Space 绿 down + throttle/steer +1.00 + handbrake=1 + pause=1+PRESSED（ui 组由 viewer 显式激活——出厂关的组面板要演示 pause 就得开）。证据链：demo log 锚（`input-demo: inject key 256 at iter 8`）+ `fc /b` 双后端 PNG 逐字节基线（`example/input-m2/run.bat` 五段式）。
- **ImGui 坑留痕**：`SameLine()` 后的 if/else-if 分支必须每支都有 item——悬空 SameLine 会把后续行全部折叠到本行（首版 pause 行整行消失并与 handbrake 行重叠，条值同值同位隐形）。

## M-I3~I5：鼠标裸查询 / 拾取 / 重映射 / 拖拽 / 手柄（2026-11-15，tag `input-m3`~`input-m5`）

第二批扩展（用户拍板「整体 review 全做」），计划与实读留痕见 `P8-INPUT-PLAN.md` §7。

### input-tools 新节 `actions-file`

- `input-tools selftest actions-file`：映射文件装载（手柄 A→fire Bool / 左摇杆X→steer Float / W→throttle 键源）+ 组开关清零语义在文件面成立 + 负向三连（unknown src / unknown set / 失败不污染 map）+ `rebind` 冻结语义（生效点 = 下一次 update，throttle W→S 改绑后值翻符号）+ 手柄直查防御面（越界 feed no-op、边沿 endFrame 清）。
- 映射文件格式（`.actions.json`，vendored nlohmann——scene 序列化同款依赖）：
  `{"sets":["driving"],"actions":[{"name":"fire","type":"bool","sets":["driving"],"binds":[{"src":"pad","id":0}]}]}`；src 全集 `key|mouse|scrollX|scrollY|deltaX|deltaY|pad|padAxis`；全部校验后一次性应用，任何错误 = false + 原因、map 不动。
- **C1061 坑留痕**：viewer/main.cpp 选项 else-if 梯子 120 项贴 MSVC 块嵌套上限——`--pick`/`--pick-at`/`--drop-sim`/`--mouse-capture` 一律走 parseArgs 开头独立预扫环，主梯子保持 master 原样。

### 脚本侧（nl-script selftest s14 / s15）

- s14 `MouseScript`：`MouseIsDown/MouseJustPressed/MouseJustReleased/MouseDelta/ScrollDelta/CursorPos` 六 op 注入锚（左键 down+pressed、右键 released、增量 3.5/-2.0、滚轮 1.25、光标 640,360）+ 解绑 NoSource 全族。操作目标 = `inputSetState` 绑定的 InputState（同 v3 Input 域）。
- s15 `GamepadScript`：`GamepadIsDown/GamepadAxis` 锚（A=0 down、B tap released、左摇杆X 0.75、左扳机 0.5、越界防御）+ `TextPop` 在 selftest 无窗口 = NoSource 负向。viewer scene 模式已接 `inputSetWindow(&window)`——`TextPop` 正路径消费 `consumeTextChars`（M-C1 通道，反引号已过滤）。
- 命令：`nl-script selftest <project> <console> <ecs> <bootstrap> <timeinput> <camera> <model> <drive> <props> <mouse> <gamepad>`（新目录均为可选追加参数，旧卡命令不变）。

### viewer 新旗标（全部缺省关 = 零漂移）

| 旗标 | 行为 | 确定性验收 |
|---|---|---|
| `--mouse-capture` | 启动即 GLFW_CURSOR_DISABLED 持续捕获（UE CapturePermanently 语义锚）；**F10 切换**（Esc=退出约定让位） | log 锚 `mouse capture -> on (--mouse-capture)` / F10 行 |
| `--pick` | scene 模式左键点击拾取（consumeMouseButtonEvents 边沿 + 光标位置反投影） | log 锚 `pick -> entity ...` / `pick -> none` |
| `--pick-at <px,py>` | 首帧定点注入拾取（真实鼠标不进对拍，M-NL2 旗标同构） | 同上，tool/image 卡 expectLog |
| `--drop-sim <path>` | 首帧模拟文件拖拽（与真实 dropCallback 同一消费队列） | log 锚 `drop -> <path>` |

拾取实现（viewer/pick.hpp，纯 CPU 零渲染改动）：UE DeprojectScreenPositionToWorld 口径锚（near/far 反投影、手写 inverseMat4）→ glTF 顶点 AABB（节点变换已烘焙）× 实体世界矩阵 → slab 求交最近 t；命中/未命中都出 log。模型加载集成（拖文件进场景）留游戏壳/编辑器接线，本轮只落通道 + 锚。
