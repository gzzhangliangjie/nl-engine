# nl-engine 项目长期约定

> 铁律/决策前置/里程碑纪律权威落点：仓库根 `AGENTS.md`；并行轨道：`docs/PARALLEL-TRACKS.md`；逐里程碑台账：`docs/MILESTONES.md`。本文件只记「AGENTS.md 没写、但容易忘」的不变量与当前状态。

## 本机环境（2026-11-10 实测）
- VS2022 + MSVC 14.38（**C++17**，禁 C++20 API 如 `map::contains`）+ Win SDK 26100；CMake 3.31.6 便携版。必用 **NMake**（vcvars64 被 reg.exe 黑名单拦）→ 构建统一 `scripts/build-m0.2.bat DX12|VULKAN`，worktree 必用自己树里的脚本（`%~dp0..` 定根）。NMake 同秒 mtime 跳重链 → 删 exe 强制重链；**探针结论有效性前置：先核 exe mtime > shader mtime**。
- **本机 exe 随机启动失败（2026-11-10 实证排查，根因待用户拍板处置）**：phys-tools/viewer 等本地 exe ~30-40% 间歇性秒退（bash exit 127 / PowerShell 0xC0000409 fail-fast），零输出、死在 main 之前 CRT 初始化（ucrtbase 固定偏移），块状分布 2~5 连挂后自愈，新旧 exe 同中招，数值层零影响。实证：唯一崩溃块（11-10 00:50:34~00:55:16）紧跟**第 87 次 ASUS AURA SYNC LightingService 崩溃 27 秒**后开始，服务慢性崩溃重启循环（3 天 9 次）；Defender 实时防护开着但零拦截记录；AppInit_DLLs/IFEO 无全局注入配置；排查窗口 40 连跑 0 失败（时机依赖）。待拍板：A 禁用 LightingService 观察；B harness 加秒退零输出自动重试+ENV-FLAKE 标注；C 下次发作管理员读 WER Report.wer 模块清单。**凡「missing anchor / 空日志」类失败先重跑 2~3 次再定性。**
- Bash 工具 cwd 不跨命令保持 → 一律 `git -C <wt>` / `ctest --test-dir` / 绝对路径，不依赖 cd。
- 本机网络：GitHub 直连全断，git remote 走 gh-proxy 镜像，push 走 SSH 443+代理+`KexAlgorithms=curve25519-sha256`（端口每次会话核对 `echo $http_proxy`）。

## 架构不变量（2026-11-06 定稿，不可动摇）
- NVRHI+Slang 通用管线；**构建链禁依赖任何厂商图形 SDK**（D3D12 走 Windows SDK；Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，loader=驱动 vulkan-1.dll）。OpenGL 已否决；`NL_GRAPHICS_BACKEND` 唯一开关。
- 图形/NVRHI/ImGui 符号只在 `renderer/`；一份 .slang 双目标(SPIR-V+DXIL)两后端加载 = 验收核心；跨平台分支依赖必须对称。

## 当前进度（2026-11-10 凌晨）
- 主线 S0~S7.1 收官（tags `s7-visbuf`/`s7-pulling`）；pbr-direct 已 merge（mode 5 Cook-Torrance）。**下一站 M7.2 材质 pass**（Carrot 式单全屏 pass，主树在做未提交）。kStage 现值以 `renderer/src/renderer.cpp` 字符串为准。
- 已 merge 回 master：feat/physics（`7d844ef`，M-P0~P4+M-J5c）、feat/audio（`1aac3d2`）、ECS E1a+E1b 链（2026-11-09 全量 **406/406 ALL GREEN**）、job-system（`fb2b4f4`）、console/material-graph/action-system（`9e12cc6`）。**E1/E1b gate 已解除** → M-S3 / M-GL 游戏循环链可开工（gameloop 轨道已注册，M-GL0 tag `gl-m0` 收官：GameClock 固定步长 1/60、暂停=时钟停摆语义、timeScale∈[0,1024]）。
- feat/physics merge 三教训：① `.gitignore` 无锚定 `build/` 静默挡掉 vendored 子目录（`94bbb66` 修，新 vendored 库应在另一 checkout 验证 configure）；② CRLF 假阳性已根修（`cbde0c9` .gitattributes：tsv/log/provenance=lf、bat=crlf，**禁全局 `* text=auto eol=lf`**；重检出可靠做法=删工作文件后 `git checkout --`）；③ ignore/attributes 标准化（`6a4d6d5`：目录根锚定、禁 `*.lib/*.dll` 通配、CMake 生成物精确文件名）。
- UE 差异台账 `docs/s5/UE-DIFFS.md`（D1~D17）随手追加必须落档；S7/S8 预研结论在 `S7-PLAN.md`/`S8-PLAN.md`「预研结论」节。
- kStage bump 波及：args 含 `--hud` 且 exe=current 的用例全要重拍。

## 渲染 / HUD 关键约定（改前看 `docs/SHADER-CONVENTIONS.md` / `docs/IMGUI-GUIDE.md`）
- SPIR-V 绑定：`slangc -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；shader 只写 `register(b0/t0/s0)`，禁 `[[vk::binding]]`。入口名两阶段：DXIL 由 `-entry` 定，SPIR-V 恒 `main`。
- MVP volatile CB `setMaxVersions(16)`；`SceneConstants` 严格 160B **禁整块 memcpy**；pipeline 必 `setFrontCounterClockwise(true)`。
- **§2.7 每管线单一 BindingSet**；**§2.8 Hi-Z 金字塔 POT** + R32F `setClearValue(1e9)`、帧内禁开第二 cmd list；**§2.9 64 位原子**；**§2.10 禁依赖 `startInstanceLocation` 的 `SV_InstanceID` 全局语义**；**§2.13 阴影 R32F MIN 混合 + 3×3 PCF 整数 texel Load + 坡度 bias `0.003+0.035×(1−NdotL)` + 量化闸基线必须同场景 shadow=false**。
- 资源/绘制逻辑收 `renderer/src/scene.cpp`（两后端共享）；GPU 资源 slot 级同步表必在 `createTargets()` 初始化（放错=首帧 139）。HUD：`hudProjection()` Y 尺度必须负、面板高≤560、字符串 ASCII、`hud=nullptr` 不碰 ImGui。

## .nmesh / 简化器
- v3=`'NFLB'` 自描述尾段（按 magic 识别，v1/v2 兼容）；**`Meshlet::triangleOffset` 是字节偏移不是三角号**；组 error=max(组简化误差,成员簇误差) 逐位精确。
- 简化器=meshopt 多 pass 主循环+link condition+双层边界锁+绝对朝向锚；`check-orient` 扫全链 inward（exit 1=FAIL，已加速 550 倍，加速类改动必须带对拍自证）。

## ECS / job-system 关键契约
- `ComponentOf<T>::nextTypeID()` 静态计数器必须在**非模板 inline 函数**（写在类模板里 → 全 typeID=0 → 读到脏内存段错误）；`createBindingSet` 必须晚于所绑 buffer 创建；多灯强度按 `1/ΣI` 归一化（CPU 预算倒数）；scene/scale 双后端互比超阈值如实记 **FAIL 档**不再折腾。
- jobgraph：create 后计数=numDeps+1（创建者持有），link 完上游必须 `launch()` 发布。并行 tick 契约（P8-ECS-PLAN）：并行段内禁结构变更、未声明访问=自足、确定性=并行==串行逐位。

## 并行轨道易忘点（详 `docs/PARALLEL-TRACKS.md`）
- wt_pool 占坑：开场声明编号 → `claim --track <轨道> --milestone <编号>`；收口 `close` 硬闸 + `release --done` 对称释放。树边界闸 `tools/tree_gate.py`：脚本树≠cwd 树 exit 2，跑工具用自己的树副本（例外 `NL_TREE_GATE_ALLOW=1`）。
- worktree junction（`.workbuddy`、`thirdparty/slang/dist`）**只能 PowerShell 原生 API 建删**（`New-Item -ItemType Junction` / `[IO.Directory]::Delete($path,$false)`）；拆 wt 先摘 junction 再 remove（MSYS `rm -r` 穿透删共享内容）；**分支保留**。
- python 孪生对拍：确定性数值层用独立 python 重实现逐行对拍防假绿；不适用于并发类。

## 工程纪律易忘点（详 `AGENTS.md`）
- 收口回复第一屏=「四项自查+三要素」表格；渲染轮次必须主动展示证据图卡片，无图显式写「本轮无新证据图」。
- **默认不跑全量回归**：改什么 `regression.py --only <版本>[/用例]` 挑 2~3 条；全量只在打 tag 收口跑一次。失败分组统计禁 `head` 截断。
- 双后端分档：`bytes`/`near`(≤1/255 且 ≤2px)/`FAIL`，禁笼统「一致」。工具 exe=`build/{DX12,VULKAN}/nanite-tools.exe`，根目录 `build/nanite-tools.exe` 是废弃残留。
- git 危险操作闸门：`git checkout --`/`restore`/`switch` 前必先 `git status --porcelain -- <path>` 核对本会话未提交修复；还原只许显式点名逐个。Windows 清理临时文件用显式文件名禁通配。
- UE 源码只读不抄；Carrot/bevy 抄思路不抄代码。回复语言简体中文（代码/标识符保留英文）。
- 调试：L0 画面证据 → L1 RenderDoc → L2 NVRHI timestamp → L3 Nsight/PIX/PresentMon/Tracy。NVRHI 权威=`thirdparty/nvrhi/doc/`，每处调用回 vendored `nvrhi.h` 核签名。
