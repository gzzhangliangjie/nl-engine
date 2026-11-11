# LOG-TOOLS-GUIDE — 日志系统升级工具（A 类轨道，M-LOG0）

> 工具 exe：`build/{DX12,VULKAN}/log-tools.exe`（纯 CPU，双构建树产物等价）。
> 退出码约定：0=PASS / 1=断言失败 / 2=用法错误。展示台卡：`log-m0`（4 用例）。

## 1. 背景

`nl::core::log`（M0.1 建）只有 stdout；M-LOG0 换 vendored spdlog v1.17.0 内核并一次定型：文件落盘（Q-L2）+ ODS 转发 + 分类通道 + 字符串过滤指令（Q-L3 把 M-LOG1 并入首落）。兼容铁律（P8-LOG-PLAN §4）：

- 老入口 `trace/debug/info/warn/error/fatal + setSink/setLevel/deviceError/errorCount/deviceErrorCount/resetCounters` 签名行为零改动；
- stdout 锚点格式 `[nl:<level>] message` 不漂（回归 expectLog 依赖）；
- 计数口径不变：**被级别闸滤掉的 error/fatal 也计数**（M0.1 语义）；
- spdlog 类型不进 `log.hpp`（PIMPL，内核可替换）。

## 2. 输出格式契约

| 路径 | 格式 |
|---|---|
| stdout（老入口） | `[nl:<level>] message` |
| stdout（通道入口 `*Ch`） | `[nl:<level>|<channel>] message` |
| ODS（OutputDebugStringA） | 同 stdout（`%v` 原样转发） |
| 文件 `out/log/<exe>-<backend>-<yyyymmdd-hhmmss>.log` | `[<yyyy-mm-dd hh:mm:ss.mmm>] ` 前缀 + 同 stdout |

- 文件在**首条日志**时懒建（相对进程 cwd），旧文件不清理（Q-L2）；
- **同秒碰撞消歧**：同名文件已存在时加 `-2`/`-3` 后缀（truncate 互吞坑，见 MILESTONES 踩坑 1）；
- backend 来自编译期宏 `NL_LOG_BACKEND_NAME`（根 CMakeLists 注入 nl_core）。

## 3. 过滤指令（M-LOG1，bevy EnvFilter 口径）

```
setFilter("nl:info,renderer=warn")   // 全局缺省 info + renderer 通道单放 warn
setFilter("renderer=off")            // 静默单通道（连 fatal 都不出）
setFilter("nl:debug")                // 只设全局缺省
setChannelLevel("asset", Level::Warn) // 运行时单通道调整
filterInstruction()                  // 当前指令规范形态回读
```

- 语法：逗号分隔；段 = `<level>`（全局缺省）或 `<channel>=<level>`；`nl:` 前缀可有可无；level ∈ `trace/debug/info/warn/error/fatal/off`；channel = 小写字母开头、`[a-z0-9_-]`、≤32 字符；
- 非法指令 `setFilter` 返回 false + 原因写 `errOut`，**当前过滤面保持不变**（显式报错，不静默吞）；
- 未覆盖通道跟随全局缺省；`setFilter` 成功 = 整面替换。

## 4. log-tools 命令

| 命令 | 退出码 | 内容 |
|---|---|---|
| `log-tools` | 2 | usage 总览 |
| `log-tools selftest` | 0/1 | 六段：M0.1 兼容语义 / 文件落盘 / 过滤正向 / 过滤负向（8 类非法指令）/ setChannelLevel / 4×200 并发冒烟。并发段打 800 行 debug 属预期 |
| `log-tools selftest --negative-only` | 0/1 | 只跑负向段（ctest 最小负向卡） |
| `log-tools demo` | 2/0 | demo 需 `--filter`；同一组行 before/after 两遍，stdout 直接可见有/无差异 |
| `log-tools demo --filter <指令>` | 0/1 | 正向实跑；非法指令 exit 1 + 可读原因 |

ctest：`log-m0-selftest` / `log-m1-bad-filter` / `log-m0-negative`（3 条 × 双后端，跑前 `--repeat until-pass:3`）。

## 5. example/log-m0 交付物

`run.bat` 七段机器断言（usage exit 2 / selftest PASS + M0.1 锚 / demo before-after 有无差异 6 断言 / negative 显式拒绝 / 文件路锚点）：DX12 树 ALL GREEN exit 0。`log-m0-evidence.png` 由 run.bat 真实输出生成（tools 侧 PIL 渲染脚本，非手绘）。产物：`usage.log` / `selftest.log` / `demo.log` / `negative.log` / `run-output.log`。

## 6. ODS 取证现状（L3 调试链路）

机制级留证：`msvc_sink_mt(false)` 无条件 `OutputDebugStringA`（vendored `sinks/msvc_sink.h:34` 核过签名，`SPDLOG_WCHAR_TO_UTF8_SUPPORT` 默认未开 = ANSI 路）。**DBWIN 实测取证受会话沙箱注入拦截**（WorkBuddy TSBX：沙箱自身 ODS 能到 DBWIN_BUFFER，子进程 ODS 不能；python ctypes 直调 ODS 连进程都崩）。真取证（DebugView / 真调试器 attached，喂 M8.1 CORRUPTION #921 同类场景）留待非沙箱会话——不阻塞 M-LOG0。

## 7. 已知边界 / 后续

- 现网 6 调用点文件（core 3 + renderer 5 处文件中的 viewer/main.cpp、backend_dx12/vulkan、scene、hud、gpu_timer）暂未迁移分类通道（`*Ch` 零强制，按需渐进）；
- M-LOG2 候选（帧号注入 / console `log.*` cvar / 文件滚动 / async 化重开）见 P8-LOG-PLAN §8 待拍板清单——**先拍板再落码**（AGENTS.md 开工闸）；
- 全量渲染回归总闸不适用于本轨（A 类）；merge 回 master 待用户拍板。
