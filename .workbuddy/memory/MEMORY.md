# nl-engine 项目长期约定

> 铁律权威落点：根 `AGENTS.md`；并行轨道：`docs/PARALLEL-TRACKS.md`；台账：`docs/MILESTONES.md`。本文件只记「AGENTS.md 没写、但容易忘」的不变量与当前状态。

## 本机环境
- VS2022 MSVC 14.38（**C++17**，禁 C++20 API）+ Win SDK 26100；CMake 3.31.6 便携版在 `E:\UE_STUDY_2026\.build-tools\`。NMake 构建：`scripts/build-m0.2.bat DX12|VULKAN`（wt 用自己树脚本）；同秒 mtime 跳重链 → 删 exe 强制重链；探针结论先核 exe mtime > shader mtime。
- ctest 间歇挂两族（2026-11-10 实锤）：①便携 ctest 对 0.07s 快进程随机丢 stdout（非回归）→ ctest 统一带 `--repeat until-pass:3`；②真秒退 0xC0000409 = ArmouryCrate 生态（LightingService 已停，残余观察）。missing anchor 先重跑再定性；python subprocess 直拉可区分两族。
- Bash cwd 不跨命令 → 一律 `git -C <wt>` / 绝对路径。GitHub 直连断；nl-engine 无 remote，tag/分支只落本地。

## 架构不变量
- NVRHI+Slang 通用管线；构建链禁任何厂商图形 SDK；OpenGL 已否决；`NL_GRAPHICS_BACKEND` 唯一开关；图形符号只在 renderer/；一份 .slang 双后端 = 验收核心。

## 当前进度（2026-11-11）
- S0~S7 收官（tag `s7-done`，全量 468/468）；M7.2 多材质 = NSUB 尾段 + GPUMaterial 间接表。
- M8.0 BLAS（tag `s8-blas`）：⚠️ AS build 上传+setBufferState+build 必须同命令列表。M8.1 TLAS 每帧重建（tag `s8-tlas`）：⚠️ DX12 帧命令列表必须常驻 `frameCmd_` 成员（per-frame 局部句柄析构 → CORRUPTION #921 仅 ODS 可见）；Vulkan 帧列表保持 per-frame 新建——两后端语义不对称，勿顺手统一。
- M9.0~M9.2 收官（tags `s9-gbuffer`/`s9-shadow`/`s9-done` @ `a8c255c`）：GBuffer / shadow ray / reference 真值图，S9 全收官。`kStage` 仍 S8.2；Q4 冻结：不新增独立 GBuffer 纹理 / MRT / 落盘 UAV，Q4 与 S10/S12 历史帧一起拍。
- **S11 M11.0 GPU flat hashmap 世界缓存已收官**（tag `s11-hash` @ `5722c1c`，已随 master）；下一站 M11.1 探针命中点改查缓存（二级弹射）/ M11.2 多跳 GI / S12 历史帧。已 merge 回 master：physics / audio / ECS / job-system / console / material-graph / action-system / pbr-direct / game-loop / pathfinding(M-N0) / drive / input / camera / editor。
- UE-DIFFS 台账（D1~D19）随手追加落档；kStage bump 波及 = args 含 `--hud` 且 exe=current 用例全重拍（`reshot-hud-stage.py`，超 527px 签名须追因才许 `--force`）。
- 日志轨道 **M-LOG0~M-LOG2 全收官**（tags `log-m0`/`log-m2`，已 merge 回 master `aaf43b8`，wt/分支保留待 CLOSED 化拍板）：vendored spdlog v1.17.0；**格式契约**：stdout 老入口 `[nl:level]`、通道 `[nl:level|channel]`（老锚点是前缀子串——回归零漂的依赖）、文件路加时间戳前缀；**时间戳命名 + 每次新建必须配同秒碰撞 -N 消歧**；M-LOG2 增量 = 帧号行尾 `#<n>`（行组装层追加，sink 收原始 message）+ console `log.*` 四命令（`registerLogCommands` 单一事实源）+ 滚动一代 + 现网 6 文件 185 处已迁移通道。工具卡传参：`log-tools exec` 每 arg = 一条完整命令行；并行跑时文件断言必须 PID 锚定（mtime 竞态实锤）。功能面闭环（M-LOG3 候选空），格式契约详 PLAN §8/§9。

## 渲染 / HUD 约定（详 SHADER-CONVENTIONS.md）
- SPIR-V 绑定 shift 256/0/128；禁 `[[vk::binding]]`；volatile CB `setMaxVersions(16)` 每帧全量覆写；SceneConstants 160B 禁整块 memcpy；§2.7~2.16 各节铁律（含 §2.16 draw 主路径重构必须保留 initCluster 单批兜底 + spot 必须含 lod/dag 模式）；HUD 面板高≤560、字符串 ASCII。

## .nmesh / ECS / job
- 尾段 magic：NLOD / NFLB+NSUB / NPAG；**Meshlet::triangleOffset 是字节偏移不是三角号**。ComponentOf 计数器在非模板 inline；jobgraph link 完必须 launch；jobsync 纪律：建图期全 link、放行统一走 `launchAll`。⚠️ JobRuntime 析构次序：pool 声明在 adapter 之后（pool 先死）。

## 并行轨道易忘点（详 PARALLEL-TRACKS §7~8）
- wt junction 建/删只用 PowerShell 原生 API（`New-Item -ItemType Junction` / `[IO.Directory]::Delete($p,$false)`），禁 cmd；wt junction 共享 `.workbuddy` + slang dist + skip-worktree。CLOSED 轨道 = wt 移除 + 分支保留（明文规定，不是待拍板项），重开走 §7.1b。
- cases.json 合并走 `merge-cases.py` 语义驱动；手工追加必须 python json 库 indent=1 + 尾换行。树闸 `tree_gate.py`；开场 `claim` / 收尾 `release` 对称。

## 工程纪律易忘点
- 收口第一屏 = 四项自查+三要素表格；默认不跑全量回归（`--only` 挑 2~3），全量只在 tag 收口跑一次；双后端分档 bytes/near/FAIL 禁笼统「一致」。git checkout/restore/switch 前必 `status --porcelain`；还原只许显式点名。负向卡 expectLog 从实测 log 复制禁手写；`--only` 是单个 `version[/case]`。
- 开工闸：待拍板未清不许落码；第一屏报「待拍板 N / 已拍板 N / 冻结面」。拍板后全仓 grep 旧口径同步勘误。**ctest 新增用例后必须双后端重新 configure**（add_test 只在 configure 写入，M-N6 实锤）。固定步长累加 float/double 必须同源（M-N6）。工具 exe = `build/{DX12,VULKAN}/nanite-tools.exe`。
- M11.0 实锤（SSC §2.21）：CB 分配尺寸必须以 `dxc -dumpbin` 反汇编的 slang cbuffer Size/offset 为准——分配小于声明 → shader 读越界得 0 → early-out 恒真全线程静默早退（dispatch 照常、无 validation 错）；CPU 常量数组长度 = cbuffer 字节/4，禁顺手 [16]。cases.json 工具卡（gi-tools 等）**必须带 case 级 "exe" 覆盖**，否则 regression 拿版本级 viewer.exe 跑子命令 → 窗口循环 120s 超时；工具子命令名/参数名/expectLog 锚以 `exe --help` 与实测 log 为准，禁手写。
