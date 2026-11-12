# nl-engine 项目长期约定

> 铁律权威落点：根 `AGENTS.md`；并行轨道 `docs/PARALLEL-TRACKS.md`；台账 `docs/MILESTONES.md`；S12 决策 `docs/s12/S12-PLAN.md`。本文件只记「AGENTS.md 没写、但容易忘」的不变量与当前状态。

## 本机环境
- VS2022 MSVC 14.38（**C++17**，禁 C++20 API）+ Win SDK 26100；CMake 3.31.6 便携版 `E:\UE_STUDY_2026\.build-tools\`。NMake 构建 `scripts/build-m0.2.bat DX12|VULKAN`；同秒 mtime 跳重链 → 删 exe 强制重链；**改 `.slang` 后必须删两棵 `build/{DX12,VULKAN}/viewer.exe` 再重链**，否则 `regression.py` exe 过期门禁拒绝出报告。裸 Git Bash configure 报 nmake not found = 环境三件套没 export，照抄 build bat 头部。
- ctest 间歇挂两族（2026-11-10）：① 便携 ctest 对 0.07s 快进程随机丢 stdout（非回归）→ 统一 `--repeat until-pass:3`；② 0xC0000409 = ArmouryCrate 生态。missing anchor 先重跑再定性。
- Bash cwd 不跨命令 → 一律 `git -C <wt>` / 绝对路径。GitHub 直连断；nl-engine 无 remote，tag/分支只落本地。

## 架构不变量
- NVRHI+Slang 通用管线；构建链禁厂商图形 SDK；OpenGL 已否决；`NL_GRAPHICS_BACKEND` 唯一开关；图形符号只在 renderer/；一份 .slang 双后端 = 验收核心。
- SPIR-V 绑定 shift 256/0/128；禁 `[[vk::binding]]`；volatile CB `setMaxVersions(16)` 每帧全量覆写；SceneConstants 160B 禁整块 memcpy；HUD 面板高≤560、字符串 ASCII（详 `SHADER-CONVENTIONS.md` §2.7~2.19）。
- **CB 尺寸以 `dxc -dumpbin` 反汇编 slang cbuffer Size/offset 为准**（M11.0）：分配小于声明 → shader 读越界得 0 → 全线程静默早退、无 validation 错；CPU 常量数组长度 = cbuffer 字节/4。root sig 反射：slang 消除未用绑定 → dxil 缺号，layout 漏 shader 实际用的槽 → PSO 失败（layout 多于 shader 无害）。slang 无 C++ lambda → `inout` 参数函数。
- **冻结基线的代码形态也是基线的一部分**（M11.1）：`direct.slang` 0xdeadbeef 守卫恒 false → s10-refgi 基线 GI 实为纯天穹；「修成」无条件 = 30347 像素回归。改基线冻结路径代码前先核是 bug 还是基线语义；重开须单独拍板 + 重拍。
- **Q11-1 拍板 A**：每帧 rebuild 的 TLAS/AS 内部布局双后端不 bit-exact → GPU 数值进断言走**统计不变量**，CPU 仿真侧保持 bytes；二值/伪彩不读数值不受影响。
- M8.1 TLAS 每帧重建：DX12 帧命令列表必须常驻 `frameCmd_` 成员（局部句柄析构 → CORRUPTION #921 仅 ODS 可见），Vulkan per-frame 新建——两后端不对称，勿顺手统一。M8.0 AS build 上传+setBufferState+build 必须同命令列表。

## 当前进度（2026-11-13）
- S0~S11 全收官（S11 tags `s11-hash`/`s11-update`/`s11-bounce`/`s11-done`）；S9 Q4 方案 A = GBuffer 属性不落盘；日志轨道 M-LOG0~2 收官（spdlog 1.17.0，格式契约 stdout `[nl:level]` / 通道 `[nl:level|channel]`、文件时间戳 + 同秒 -N 消歧、帧号行尾 `#<n>`）。十余条功能分支已 merge 回 master。
- **S12 M12.0（tag `s12-temporal`）**：GI 分量独立时域累积（`gatherLinear_` GI-only + `history0_/1_` ping-pong，零新增 RT）；Q12-E = 仅 filter 态逐帧随机方向置换（`traceCB_` 80B 零扩容）。验收 `--dist 4` + `--filter-gi-dump`（`--dist 7` 下 `csGather` 权重全灭、GI 恒 0，既有事实）；`hf` 1.216→0.677、MAE 3.902→2.687。全量 804/810。
- **S12 M12.1（tag `s12-spatial`）**：Q12-C = A 先时间后空间单次串联；`filter_spatial.slang` 5×5 联合双边，`--spatial` 默认关、零新增 RT/CB/GBuffer。`hf` 0.677→0.475、`edgeGradRatio` 0.493、MAE ratio 1.121（门限按实测标定 1.15，联合双边有偏估计）。保形 bytes 级。全量 810/812。两轮共性问题：4 个环境间歇族（重跑即绿）+ 2 个 `s11-hash/cacheviz` 既有基线漂移（M11.2 `wcMakeKey` 删法线分量），冻结基线不动、重拍挂后续项。
- **S12 M12.2 开工闸拍板（2026-11-13）**：Q12-D = **D-B 关滤波 N 帧平均**（`--temporal-off --filter-gi-dump` + `--screenshot-seq` 逐像素平均；**N≤64** = `s12Frame_ % 64`；运动不平均，改同帧无历史单帧作鬼影基准；零 shader 改动，新增 1 工具 + 1 参考图）、Q12-B 续拍 = **B1 维持不落盘**（Q4 三次顺延结案；拒绝判据 = 越界 + 重投影深度差 + 当前帧法线，可加零资源邻域 clamp / 帧数置信度）。三处重读口径：Carrot `svgf.slang` 整套 previous GBuffer / Carrot `temporal-denoise` 仅 1 张 previousViewPos / bevy TAA 零属性方差 clip / bevy solari `pixel_dissimilar` packed previous_gbuffer / UE `NumFramesAccumulated` 帧数置信度。权威落点 `S12-PLAN.md §0.8` + `MILESTONES.md` M12.2 开工拍板节。⚠️ 三处口径不一致后的取舍理由与升级触发条件已落 `docs/s5/UE-DIFFS.md` **D20**。
- **S12 M12.2 已收官（tag `s12-done`）**：`filter_temporal.slang` 加 `rejectHistory()` 五条（越界 / 重投影 deviceZ 差 / 当前帧法线一阶切平面外推残差 / 3×3 深度邻域 min/max clamp / 帧数置信度）；**零新增 RT 纹理 CB**，`filterLayout_` 只增 t2 = 已有 `probeDepth_` SRV、`g_flags.w` 复用「开关+阈值」（0=层关）→ FilterConstants 仍 32B（`dxc -dumpbin` 实锤）；HUD 参数面板补齐三层 checkbox + 拒绝阈值。`--filter-reject` / `--reject-threshold` 默认关、零基线扰动。验收：静态 MAE(OFF,D-B)=MAE(ON,D-B)=0.779/0.780、hf 0.677、diffPixels 0（静态 velocity=0 结构性不触发）；运动跟手性代理 ratio **0.846 ≤ 0.95**、层响应 6000→20012；负向控制（单帧噪声冒充 ON）被拒、工具 selftest 全绿。门限按实测标定（MAER 1.05 / MAXMAE 1.00 / HFR 1.05 / GHOSTR 0.95）。工具 `tools/check-s12-done.py`（纯标准库 `avg`/`refcheck`/`ghost`/`selftest`）+ `example/s12-done/run.bat verify` 双后端 14 shots。保形 M12.0/M12.1 verify 数值逐项不变 + `--only` 双后端 bytes。⚠️ **运动证据只是跟手性代理，不是主观鬼影判据**；本场景 `probeDepth_` deviceZ 仅 184~186 三档、单凸体+纯天空无真实几何边缘 → 拒绝层测不出量化收益，多物体场景复评挂后续项；S10 `reference-gi`（1024 spp）跨阶段、与探针 gather 不同构，纳入验收须另拍板。
- **下一步**：S12 全阶段收官，待拍板架构项 0；独立后续项见下。`kStage` 仍 S8.2，本轮新增第 107 张版本卡 `s12-done`（旧 106 张零重拍）。
- 独立后续项（均需单独拍板）：`s11-hash/cacheviz` 基线漂移、多物体场景复验保边、Shader 性能检查（候选 A `-validate-uniformity` / B per-pass gpu_timer / C `-trace-coverage`）、`regression.py` 多进程分片 worker 隔离、`RUN_TIMEOUT` 60→90s 复核。

- **多人游戏方向（2026-11-13 用户拍板进路线）**：`GAME-DEV-ORDER.md` 11→12 步新增第 12 步「多人游戏 / 网络栈」，范围 = 传输 + 序列化 + **同步层**三层（用户追加「同步层也算上吧」）；`NON-RENDER-ROADMAP.md` 原 ★★ 慎入定档已作废并同步勘误。决策材料 `docs/net/NETCODE-OPTIONS.md`（Q-N1 已拍板；**Q-N2~Q-N6 待拍板** → 开工闸冻结落码、不建 PLAN/track）。既有事实：msgpack 已有自研 `editor_bridge/msgpack_min.hpp`（仅 RPC 用）；`RPC-SPEC.md §2` 把 msgpack-c 写成 MIT 有误（实为 BSL-1.0 + 默认依赖 boost）；KCP 纯 ARQ 两文件零 I/O；`P8-GAMELOOP-PLAN.md §4`「回滚 netcode 不在范围」仍有效，选回滚模型须单独解禁。

## 工程纪律（详 AGENTS.md）
- 收口第一屏 = 四项自查 + 三要素表格；默认不跑全量回归（`--only` 挑 2~3），全量只在 tag 收口跑一次；双后端分档 bytes/near/FAIL，禁笼统「一致」；`--only` 是单个 `version[/case]`。
- 开工闸：待拍板未清不许落码；第一屏报「待拍板 N / 已拍板 N / 冻结面」；拍板后全仓 grep 旧口径同步勘误。主线按 §1b 不建 track / 不 claim。
- git checkout/restore/switch 前必 `status --porcelain`；还原只许显式点名。kStage bump 波及 = args 含 `--hud` 且 exe=current 用例全重拍（`reshot-hud-stage.py`，超 527px 签名须追因才 `--force`）；UE-DIFFS 台账随手追加。
- cases.json 合并走 `merge-cases.py`；手工追加必须 python json 库 indent=1 + 尾换行；**追加后必须跑一次 regression 冒烟**（M11.2 卡漏 `id` 挡全量实锤）。工具卡必须带 case 级 `"exe"` 覆盖（否则窗口循环 120s 超时）；子命令/参数/expectLog 锚以 `exe --help` 与实测 log 为准，禁手写（负向卡 expectLog 从实测 log 复制）。
- ctest 新增用例后必须双后端重新 configure（add_test 只在 configure 写入）；固定步长累加 float/double 必须同源。工具 exe = `build/{DX12,VULKAN}/nanite-tools.exe`。
- **regression.py 静默挂死（M12.1 收口实锤）**：`subprocess.run(capture_output, timeout)` 遇派生孙进程继承 stdout → 超时后 `communicate()` 无 timeout 等 EOF 永久阻塞、整池停摆 25min 零输出。修复 = `Popen` + 临时文件重定向 + `stdin=DEVNULL` + `wait(timeout)`（RUN_TIMEOUT 60s）+ `[STALL]` 告警 + 每条 flush / `.progress.jsonl` / 每 50 条心跳。**长任务必须设短检查点主动报进度，不能等用户问**。

## .nmesh / ECS / job
- 尾段 magic NLOD / NFLB+NSUB / NPAG；**Meshlet::triangleOffset 是字节偏移不是三角号**。ComponentOf 计数器在非模板 inline；jobgraph link 完必须 launch；jobsync 建图期全 link、放行统一 `launchAll`。⚠️ JobRuntime 析构次序：pool 声明在 adapter 之后（pool 先死）。

## 并行轨道（详 PARALLEL-TRACKS §7~8）
- wt junction 建/删只用 PowerShell 原生 API（`New-Item -ItemType Junction` / `[IO.Directory]::Delete($p,$false)`），禁 cmd；共享 `.workbuddy` + slang dist + skip-worktree。CLOSED 轨道 = wt 移除 + 分支保留，重开走 §7.1b。树闸 `tree_gate.py`；开场 `claim` / 收尾 `release` 对称。
