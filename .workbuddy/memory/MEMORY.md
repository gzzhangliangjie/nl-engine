# nl-engine 项目长期约定

> 铁律权威落点：根 `AGENTS.md`；台账 `docs/MILESTONES.md`；并行轨道 `docs/PARALLEL-TRACKS.md`。各里程碑决策详对应 `docs/*/S*-PLAN.md` 与各 PLAN 文档。本文件只记「AGENTS.md 没写、但容易忘」的不变量与当前状态。

## 本机环境
- VS2022 MSVC 14.38（**C++17**，禁 C++20 API）+ Win SDK 26100；CMake 3.31.6 便携版 `E:\UE_STUDY_2026\.build-tools\`。NMake 构建 `scripts/build-m0.2.bat DX12|VULKAN`；同秒 mtime 跳重链 → 删 exe 强制重链；**改 `.slang` 后必须删两棵 `build/{DX12,VULKAN}/viewer.exe` 再重链**，否则 regression exe 过期门禁拒绝出报告。裸 Git Bash configure 报 nmake not found = 环境三件套没 export，照抄 build bat 头部。
- ctest 快进程随机丢 stdout（非回归）→ 统一 `--repeat until-pass:3`；0xC0000409 = ArmouryCrate 生态。missing anchor 先重跑再定性。
- Bash cwd 不跨命令 → 一律 `git -C <wt>` / 绝对路径。GitHub 直连断；nl-engine 无 remote，tag/分支只落本地。

## 架构不变量
- NVRHI+Slang 通用管线；构建链禁厂商图形 SDK；OpenGL 已否决；`NL_GRAPHICS_BACKEND` 唯一开关；图形符号只在 renderer/；一份 .slang 双后端 = 验收核心。
- SPIR-V 绑定 shift 256/0/128；禁 `[[vk::binding]]`；volatile CB `setMaxVersions(16)` 每帧全量覆写；SceneConstants 160B 禁整块 memcpy；HUD 面板高≤560、字符串 ASCII（详 `SHADER-CONVENTIONS.md` §2.7~2.19）。
- **CB 尺寸以 `dxc -dumpbin` 反汇编 slang cbuffer Size/offset 为准**：分配小于声明 → shader 读越界得 0 → 全线程静默早退、无 validation 错；CPU 常量数组长度 = cbuffer 字节/4。root sig 反射：layout 漏 shader 实际用的槽 → PSO 失败（多于无害）。slang 无 C++ lambda → `inout` 参数函数。
- **冻结基线的代码形态也是基线的一部分**（M11.1，`direct.slang` 0xdeadbeef 守卫等）：改基线冻结路径代码前先核是 bug 还是基线语义；重开须单独拍板 + 重拍。
- Q11-1 拍板 A：GPU 数值进断言走**统计不变量**，CPU 仿真侧保持 bytes。
- M8.1：DX12 帧命令列表必须常驻 `frameCmd_` 成员、Vulkan per-frame 新建——两后端不对称，勿顺手统一；AS build 上传+setBufferState+build 必须同命令列表。

## 当前进度（2026-11-13）
- S0~S12 全收官（S12 收官 tag `s12` @ 4c386a6）；S9 Q4 = GBuffer 属性不落盘；M-LOG0~2 日志轨道收官（spdlog，stdout `[nl:level]` 契约）。`kStage` 仍 S8.2。十余条功能分支已 merge 回 master。
- **网络栈第 12 步整轨完成**：M-NET0~M-NET3 收官（tag `net-m3` + `net-done` + `milestone/net-done` 分支 c43dbe0）。ENet+bitsery vendored、服务器权威+快照插值、viewer `--net-server/--net-client`、`--interp-delay`、server 优雅关服 + client 断链边沿。踩坑：控制消息漏 proto 字段 → Input 全静默拒收；Hello 建实体必须同步建输入槽；双进程 run.bat 须 start /b + ping 延后；regression 卡 `exe` 必须双后端字典。权威落点 `docs/net/NETCODE-PLAN.md` / `NETCODE-OPTIONS.md` §6。
- **M-CRASH0 收官**（tag `crash-m0`）：SEH+dbghelp dmp+json 入 nl_core；`tools/analyze-crash.py` 符号化；L2 待用户终端首跑 bat 确认（沙箱禁 GUI）。
- **M-PERF0 收官**（tag `perf-m0`）：`tools/check-shader-uniformity.py` 59 artifacts×2 门禁零违规；**M-PERF1 已收官**（tag `perf-m1`：`PassTimers` 13 pass × 4-slot 环、`--pass-timing` 默认关零扰动、HUD 逐 pass 面板 + `[nl:info|pass-timing]` 日志锚、`tools/check-perf-m1.py` + run.bat verify 双后端 8 段 ALL GREEN；Q-P4 表已按实码回写）权威落点 `docs/perf/PERF-PLAN.md`。
- 独立后续项（需单独拍板）：shader trace-coverage（PERF C 档）；`tools/analyze-crash.bat` UTF-8 中文 rem 潜伏违规（待拍板顺手修）；多物体场景复验保边；行驶段循环 demo 旗标。

## 工程纪律（详 AGENTS.md）
- 收口第一屏 = 四项自查 + 三要素表格；默认不跑全量回归（`--only` 挑 2~3），全量只在 tag 收口跑一次；双后端分档 bytes/near/FAIL，禁笼统「一致」；`--only` 是单个 `version[/case]`。
- 开工闸：待拍板未清不许落码；第一屏报「待拍板 N / 已拍板 N / 冻结面」；拍板后全仓 grep 旧口径同步勘误。主线按 §1b 不建 track / 不 claim。
- git checkout/restore/switch 前必 `status --porcelain`；还原只许显式点名。kStage bump 波及 = args 含 `--hud` 且 exe=current 用例全重拍；UE-DIFFS 台账随手追加。
- cases.json 合并走 `merge-cases.py`；手工追加必须 python json 库 indent=1 + 尾换行；**追加后必须跑一次 regression 冒烟**。工具卡必须带 case 级 `"exe"` 覆盖；子命令/参数/expectLog 锚以 `exe --help` 与实测 log 为准，禁手写。
- ctest 新增用例后必须双后端重新 configure；固定步长累加 float/double 必须同源。工具 exe = `build/{DX12,VULKAN}/nanite-tools.exe`。
- regression.py 静默挂死已修（Popen + 临时文件重定向 + wait(timeout)）；**长任务必须设短检查点主动报进度，不能等用户问**。

## .nmesh / ECS / job
- 尾段 magic NLOD / NFLB+NSUB / NPAG；**Meshlet::triangleOffset 是字节偏移不是三角号**。ComponentOf 计数器在非模板 inline；jobgraph link 完必须 launch；jobsync 建图期全 link、放行统一 `launchAll`。⚠️ JobRuntime 析构次序：pool 声明在 adapter 之后（pool 先死）。

## 并行轨道（详 PARALLEL-TRACKS §7~8）
- wt junction 建/删只用 PowerShell 原生 API（`New-Item -ItemType Junction` / `[IO.Directory]::Delete($p,$false)`），禁 cmd；共享 `.workbuddy` + slang dist + skip-worktree。CLOSED 轨道 = wt 移除 + 分支保留，重开走 §7.1b。树闸 `tree_gate.py`；开场 `claim` / 收尾 `release` 对称。
