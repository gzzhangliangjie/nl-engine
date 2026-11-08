# P8-CONSOLE-PLAN — 开发者控制台（Console）

> A 类并行轨道（非渲染 / 引擎化），固定 worktree `feat/console`（`E:/UE_STUDY_2026/nl-engine-console`）。
> 2026-11-08 用户拍板立项：单独开 worktree。执行不依赖 P8 E1 场景整合，随时可开工。
> 对应总路线 `docs/ROADMAP.md`；并行纪律见 `AGENTS.md`「并行轨道 worktree 纪律」与 `docs/PARALLEL-TRACKS.md` §7。

## 目标

给引擎一个**运行时开发者控制台**：`~` 呼出 ImGui 窗口 → 输入命令 / 改 CVar → 立即生效。核心价值是**调试效率**——此前每次调参都要改代码重编，控制台让 HUD/相机/渲染参数可在运行时现场改。

## 三处参考口径

| 来源 | 怎么做 | 性质 |
|---|---|---|
| **Carrot `console/`**（369 行） | `Console`（`registerCommand` + `renderToImGui` + `toggleVisibility`）+ `AutocompleteField` 自动补全 + `RuntimeOption` 运行时调参 | **从零节奏参照首选**，规模小、结构清晰 |
| **UE** | `Runtime/Core/Public/HAL/IConsoleManager.h`（`IConsoleManager` CVar 注册体系：CVar + 命令 + help + 回调）+ `UConsole`（Engine 层输入行 UI） | 工业终态只读——重点看 **CVar 与命令统一注册 / 帮助文本 / 变更回调** 的口径 |
| **bevy** | 无等价实现（无内建控制台） | 据实标注 |

> 纪律：UE 只读不抄（EULA）；Carrot 抄思路不抄代码；`core/console/` 零图形符号（ImGui 只进 `renderer/`/viewer 侧的前端文件）。

## 架构约束

- **两层分离**：`core/console/` = 纯逻辑（命令注册表 / CVar 存储 / 解析执行，可单测、零 ImGui）；前端（ImGui 窗口 / 输入行 / 补全 / 日志滚动）放 renderer/viewer 侧，符合「图形符号只在 `renderer/`」铁律。
- 依赖：吃 vendored **ImGui v1.91.9**（已有）与现有 `viewer` 命令行入口；不碰 `kStage`、不进渲染回归。

## 里程碑（可见验收阶梯）

| 里程碑 | 内容 | 验收判据（亲眼可见） |
|---|---|---|
| **M-C0** | `core/console/` 纯逻辑内核：命令注册表（名字→回调+help）、CVar（int/float/bool/string）注册与读写、命令行解析（含引号参数）、执行 + 错误反馈 | `nanite-console` 工具自测：注册/执行/参数解析/未知命令错误 全断言绿；无渲染依赖 |
| **M-C1** | ImGui 前端：`` ` `` 键呼出/收起窗口、输入行 + 历史记录（↑↓ 翻旧命令）、滚动日志区（带级别配色）、**Tab 自动补全**（前缀匹配 + 唯一候选直接补全） | viewer 里 `~` 呼出，敲命令出结果、日志滚动、Tab 补全演示截图 |
| **M-C2** | `RuntimeOption` 式运行时调参 + 内建命令集：`help` / `cvar.list` / `cvar.set` / `hud.*`（开关 HUD 行）/ `cam.*`（现场改相机距离/FOV）；CVar 变更回调 | viewer 运行中 `cam.fov 60` 立即生效；`cvar.list` 列全注册表；改参数不用重编 |
| **M-C3** | 收口：工具版交代（usage/正向/负向用例进展示台）+ 归档 `docs/p8/...-TOOLS-GUIDE.md` 式文档 + MILESTONES 四栏 + tag | §7.2 文档闸过，merge 回 master |

## 排期

不依赖任何场景整合步骤，**随时可开工**；体量小（预计 2~4 个业余日），适合作为大轨道间隙的「小甜点」。

## 双后端纪律

本轨道不新增 GPU 资源 / shader，不触发双后端画面回归；若 M-C1 前端改动波及 HUD 布局，按主线纪律重拍受影响 HUD 基线。
