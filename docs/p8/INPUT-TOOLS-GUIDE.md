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
