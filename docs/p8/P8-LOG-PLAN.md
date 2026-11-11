# P8-LOG-PLAN：引擎日志系统升级（A 类并行轨道）

> 立项：2026-11-11 用户拍板「立项」。本文档 = 决策材料 + 里程碑阶梯；**落码前待拍板项见 §6**（AGENTS.md 开工闸，M9.0 教训）。
> 现状：`nl::core::log`（`core/include/nl/core/log.hpp` 46 行 + `core/src/log.cpp` 83 行，M0.1 建、M0.2 接 NVRHI device callback sink）= 6 级别 + mutex 线程安全 + 可替换 sink + `deviceError` 独立计数。只写 stdout；无文件落盘、无时间戳/帧号、无分类通道、无 ODS 转发。

---

## 1. 性质

- **A 类并行轨道**（引擎化 / 非渲染主线）：不进 `kStage`、不进 bytes/near 渲染回归总闸；走工具版交代（selftest + 展示台 tool 卡 + 真实输出证据图）。
- **为什么现在立项**：① 日志升级已被实际坑位点名——DX12 debug layer CORRUPTION #921 仅 OutputDebugString 可见、InfoQueue 不落地（M8.1 实锤），现行日志抓不住 ODS，当时靠 `tools/dbwin_capture.py` 临时取证；② example 回归的 log 锚点断言全部依赖外层 stdout 重定向，没有引擎级文件落盘；③ S10~S15 渲染主线复杂度上来后（多 pass、多 cascade、历史帧），无分类过滤的扁平 stdout 会淹掉关键告警。
- **为什么算关联系统**：日志不属于 nanite / lumen 手写主线。按 `AGENTS.md`「自研边界与第三方库选型闸」（2026-11-09 用户拍板）：**用成熟库是通例，自研是例外**。

---

## 2. 三处参考实读（2026-11-11 定向读，与「本轮落码模块」一一对应）

| 来源 | 实读结论 |
|---|---|
| **Carrot**（`E:\UE_STUDY_2026\Carrot\engine\engine\` 全目录搜） | **无专职日志系统**（据实标注）：全文无 Log 类 / spdlog / fmt；唯一 `std::cout` 在 `network/NetworkInterface.cpp:86` 且被注释掉。错误报告靠异常 + assert。反向印证：从零引擎不必上重炮，但 Carrot 没有渲染验证层计数需求，本项目有（errorCount 硬判据），不能照抄它的「不做」 |
| **bevy**（`crates/bevy_log/src/lib.rs` 451 行） | **薄封装流**：`bevy_log` 本体不自研，全委托 `tracing` + `tracing_subscriber`——分层 layer 架构（fmt layer 默认写 stderr，`with_writer` 可换）、`EnvFilter` 字符串指令过滤（`RUST_LOG` 风格，如 `info,wgpu=warn`）、`LogPlugin` 挂 ECS 世界。启示：**调用面薄 + 内核成熟库 + 过滤指令字符串化**是性价比最高形态 |
| **UE 5.8.4**（`Runtime/Core/Public/Misc/OutputDevice*.h` 13 个 + `Private/Logging/` 11 个，只读） | **工业终态**：`FOutputDevice` 抽象基类 + 设备群（File / Debug(ODS) / Console / AnsiError / StdOut / Memory / Null / Buffered / Error）+ **`FOutputDeviceRedirector` 聚合广播**（一条日志同时写 console + file + debugger 三路）；`FOutputDeviceFile` 带 `FAsyncWriter` 独立写线程 + 内存缓冲 + 旧文件备份轮转；分类系统 = `LogCategory`/`LogMacros`（`DEFINE_LOG_CATEGORY` + `UE_LOG(LogTemp, Warning, ...)`）+ `ELogVerbosity` 逐类运行时/编译期 verbosity + suppression + scoped override。启示：**redirector 广播 + 异步文件写 + 分类过滤**是「看起来对」的目标形态，但 UE 全家桶对我们过度设计（无 crash reporter / structured log / 集群需求） |

**三处口径不一致（需拍板）**：Carrot「不做」vs bevy「薄封装成熟内核」vs UE「全套设备群 + 分类」。按自研边界闸，通例走 bevy 形态（薄封装 + 成熟内核），过滤/分类学 UE 语义但按需裁剪。

---

## 3. 选型硬闸核查（spdlog，通例候选）

| 闸项 | 核查结果 |
|---|---|
| 开源无风险 | ✓ MIT License；业界事实标准，社区极活跃 |
| 许可兼容 | ✓ MIT 与本项目 MIT 兼容；**如实声明：MIT 无显式专利授权条款，不作「绝对零风险」表述** |
| 零构建期新依赖 | ✓ header-only（bundled fmt 版），纯源码编译，MSVC 14.38 / C++17 兼容（spdlog v1.x 支持 C++11 起） |
| 溯源留档 | 拍板后 `thirdparty/spdlog/VENDOR.md`（版本 / tag / 上游 / 许可证 / 入库子集 = `include/spdlog` bundled-fmt 头 + 必要 src，排除 `tests/` `bench/` `examples/`） |
| 边界不变 | ✓ 只解决日志，不碰渲染主线；业务代码不直接暴露 `spdlog::` 类型，经 `nl/core/log.hpp` 薄封装隔离（保证可替换） |
| 可达性 | ✓ gh-proxy 镜像 `git ls-remote` 实测可达，最新 tag `v1.17.0` |

---

## 4. 兼容铁律（无论选哪个方案都成立）

1. **调用面零改动**：`nl::core::log` 六个入口（trace/debug/info/warn/error/fatal）+ `setSink/setLevel/deviceError/errorCount/deviceErrorCount/resetCounters` 签名不变；现网 10 个使用文件（core 3 + renderer 5 + viewer 1）**一行不返工**。
2. **log 锚点回归卡不漂**：现有回归用例大量用 expectLog 断言（`[nl:...]` 前缀格式）；默认输出格式**必须保持 `[nl:level] message`**（或改造经全量对拍验证后才许改）。落码期任何格式变更先跑 `regression.py` 受影响用例对拍。
3. **errorCount / deviceErrorCount 语义不变**：验收判据「验证层零错误」的机器统计口径不动；spdlog 侧错误计数经 wrapper 逻辑保持等价。
4. **帧内线程安全**：渲染线程 + job worker 并发写日志不能死锁/撕裂（spdlog 自带 mutex/async 模式，锁口径在 wrapper 声明）。
5. **A 类验收纪律**：selftest 断言 + 展示台 tool 卡（usage/正向/负向）+ 真实输出证据图；不进渲染回归总闸。

---

## 5. 里程碑阶梯（拍板后开工，可调整）

| 里程碑 | 内容 | 可见验收点 |
|---|---|---|
| **M-LOG0** | vendored spdlog v1.17.0 + `nl::core::log` 内核替换（调用面不变）+ **文件落盘**（异步写，默认路径与命名待拍板）+ **ODS 转发**（OutputDebugString，喂 L3 调试链路）+ selftest 全锚点对拍（现网 log 输出逐条不漂） | `log selftest` 逐条过；文件/ODS/stdout 三路同写证据；既有用例 log 锚点零回归（`--only` 抽查 2~3 条带 log 断言的卡） |
| **M-LOG1** | **分类通道**（renderer / core / scene / asset…，学 UE category 但按需裁剪）+ **字符串过滤指令**（bevy EnvFilter 口径：`nl:info,renderer=warn` 式）+ per-channel level 运行时可调 | 过滤指令单测（正/负向：非法指令明确报错）；tool 卡实跑「同一条日志按指令有/无输出」 |
| **M-LOG2** | 帧号 + 时间戳注入（渲染帧序号从 renderer 侧注入 wrapper）+ viewer 接线：console `log.*` cvar（level/channel 实时调）+ 文件滚动策略（学 UE 备份语义，简化版） | console 实时改 level 生效证据；滚动备份证据图 |

M-LOG0 是本轨道最小闭环；M-LOG1/2 按需续开。

---

## 6. 拍板记录（2026-11-11，三轮 AskUserQuestion 收敛，全数写回）

- **Q-L1 选型 = 方案 A**（用户拍板「spdlog」，先问明「spdlog 是啥」确认理解后拍板）：vendored **spdlog v1.17.0**（MIT，header-only，bundled fmt），`nl/core/log.hpp` 薄封装换内核，调用面不变；方案 B（自研升级）否决。
- **Q-L2 文件落盘 = 方案 A**：`out/log/<exe>-<backend>-<yyyymmdd-hhmmss>.log`，启动时建，旧文件不清理（回归验证日志人肉清理痛点直接受益）。
- **Q-L3 范围 = M-LOG0 + M-LOG1 一并做**（用户拍板）：分类通道 + 过滤指令随首落一并定型（wrapper API 扩面一次到位）；M-LOG2（帧号/console cvar/滚动）仍后置另议。

## 6b. 开工前置结论（AGENTS.md 4 步 checklist 落痕，2026-11-11）

- **三处实读**（与落码模块一一对应）：Carrot（无日志系统，据实标注）/ bevy（`bevy_log` 451 行 = tracing 薄封装 + EnvFilter 指令 + layer 架构）/ UE（`OutputDevice*.h` 13 设备 + `FOutputDeviceRedirector` 广播 + `FAsyncWriter` 异步文件写 + LogCategory 分类）——见 §2。
- **历史约定**：log.hpp M0.1 建、M0.2 接 NVRHI sink（MILESTONES:83）；`errorCount/deviceErrorCount` 是验收硬判据不能动；2026-11-09 自研边界闸 = 成熟库通例。
- **spdlog 可达性**：gh-proxy 镜像 `git ls-remote` 实测可达，`v1.17.0` 为最新 tag。
- **开工待拍板项清零**：本节即拍板记录，§5 阶梯随之修正——M-LOG0+M-LOG1 合并落地（vendored + 兼容层 + 文件落盘 + ODS + 分类通道 + 过滤指令），M-LOG2 后置。

---

## 7. 排期约束

- **立项文档轮边界（2026-11-11 用户拍板「这里只写文档不开工」）**：本轮只产出本文档 + PARALLEL-TRACKS §7.1 注册行；**`wt_pool.py TRACKS` 注册、claim、wt `feat/logging` 建立、spdlog vendored、全部落码 = 留给开工会话**，不在本轮发生。
- A 类轨道，与主线 S10（VSM）、pathfinding M-N3 **零冲突**：不碰 `renderer/shaders` 渲染路径、不进 `kStage`；唯一渲染侧接触点是 `backend_dx12.cpp` / `backend_vulkan.cpp` / `scene.cpp` / `hud.cpp` / `gpu_timer.hpp` / `viewer/main.cpp` 的 log 调用行（只允许 wrapper 兼容层保证其零改动）。
- 开工会话动作序列：① `wt_pool.py TRACKS` 加 `logging` 条目 → ② §7.1b 建 wt `feat/logging`（junction `.workbuddy`/`slang dist` + skip-worktree）→ ③ `claim --track logging --milestone M-LOG0` → ④ vendored spdlog v1.17.0（gh-proxy 镜像 + VENDOR.md）→ ⑤ 落码（兼容层先行，log 锚点对拍后动文件落盘/ODS/分类）→ ⑥ selftest + tool 卡 + 证据图 + 四栏台账 → ⑦ tag `log-m0` + §7.2 文档闸收口。
- 收口走 PARALLEL-TRACKS §7.2 文档闸 + §7.5 关联文档同步；tag 名字制 `log-m0` 起，不占 m0.* 编号。


---

## 8. M-LOG0 收口纪要（2026-11-11，tag `log-m0`）

**交付面**：vendored spdlog v1.17.0（`thirdparty/spdlog/` + VENDOR.md）+ `nl::core::log` 内核替换（调用面零改动）+ 文件落盘（Q-L2）+ ODS 转发 + 分类通道 + 过滤指令（Q-L3 的 M-LOG1 范围一并落地）+ `log-tools` 工具 + `example/log-m0/` 交付物 + 展示台 `log-m0` 卡。验收数字与踩坑全文见 `docs/MILESTONES.md` M-LOG0 节；工具用法见 `docs/p8/LOG-TOOLS-GUIDE.md`。

**实现决策偏差留档 1 处（本节为拍板口径同步点）**：§5 M-LOG0 原文「文件落盘（异步写）」→ 落码取**同步 `basic_file_sink_mt` + 每条 flush**。理由：日志量小、正确性优先；spdlog async_logger 的线程池在进程退出期不 join，尾行丢失会破坏回归取证（Q-L2 的落盘动机恰恰是取证）。async 化并入 M-LOG2 候选（与文件滚动策略一起议）。

**格式契约（M-LOG2 不得破坏）**：stdout 老入口 = `[nl:<level>] message`（回归 expectLog 依赖）；通道新入口 = `[nl:<level>|<channel>] message`；文件路 = `[<日期> <时间.毫秒>] ` 前缀 + 上述行。文件名碰撞消歧 `-2/-3` 后缀为拍板形态的组成部分。

**M-LOG2 待拍板项（后续会话按开工闸先拍板再落码）**：① 帧号注入方式（wrapper 全局 vs logger 上下文）；② console `log.*` cvar 面；③ 文件滚动/清理策略（当前只增不删）；④ async 化是否重开；⑤ 现网 6 调用点文件是否迁移分类通道。
