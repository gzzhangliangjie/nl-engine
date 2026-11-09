# nl-engine 项目长期约定

> 铁律/决策前置/里程碑纪律权威落点：仓库根 `AGENTS.md`；并行轨道：`docs/PARALLEL-TRACKS.md`；逐里程碑台账：`docs/MILESTONES.md`。本文件只记「AGENTS.md 没写、但容易忘」的不变量与当前状态。

## 本机环境（2026-11-10 实测）
- VS2022 + MSVC 14.38（**C++17**，禁 C++20 API）+ Win SDK 26100；CMake 3.31.6 便携版在 `E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\`。必用 **NMake** → 构建统一 `scripts/build-m0.2.bat DX12|VULKAN`（worktree 用自己树的脚本）；NMake 同秒 mtime 跳重链 → 删 exe 强制重链；**探针结论有效性前置：先核 exe mtime > shader mtime**。
- **本机 exe 间歇秒退两族（2026-11-10）**：① phys 0xC0000409 = JobRuntime 析构次序竞争，**已修复**（见「job-system」节）；② viewer 等本地 exe ~30-40% 秒退（bash 127 / 0xC0000409，零输出，块状 2~5 连挂后自愈）与 LightingService 服务崩溃循环疑似相关，根因待拍板处置（A 禁服务观察 / B harness 秒退零输出自动重试 / C 发作时管理员读 WER Report.wer）。**凡「missing anchor / 空日志」类失败先重跑 2~3 次再定性。**
- Bash cwd 不跨命令保持 → 一律 `git -C <wt>` / 绝对路径。网络：GitHub 直连断，push 走 SSH 443+代理+`KexAlgorithms=curve25519-sha256`（端口先 `echo $http_proxy` 核对）。**nl-engine 仓库无 remote**，tag/分支只落本地。

## 架构不变量（不可动摇）
- NVRHI+Slang 通用管线；**构建链禁依赖任何厂商图形 SDK**（D3D12 走 Windows SDK；Vulkan 头 `NVRHI_FETCH_VULKAN_HEADERS`）。OpenGL 已否决；`NL_GRAPHICS_BACKEND` 唯一开关。图形符号只在 `renderer/`；一份 .slang 双目标两后端加载 = 验收核心。

## 当前进度（2026-11-10 05:10）
- **主线 S0~S7 全收官（tag `s7-done`，commit `abe1acf`，全量 468/468 ALL GREEN）**：M7.2 多材质 = `.nmesh` 'NSUB' 尾段（v3 只加不改，材质 id=子网格序号随簇记录）+ GPUMaterial[4] 间接表 + NonUniformResourceIndex 选槽 + 传统直画 per-prim 批次化对照（model.slang 零改动）。A/B 终验 sil=0 / 99.991%≤2 / maxDelta 4。`milestone/s7-done` 分支留档。
- **M8.0 BLAS 预建收官（tag `s8-blas`，2026-11-10）**：`Scene::initBlas`（hook initDag levels[0]+initCull；initHiz 委托）零拷贝吃 fallback 32B 交错 buffer（NVRHI 枚举名 `RGB32_FLOAT`）；**⚠️ 0x87d 铁律：AS build-input 上传+`setBufferState`+build 必须同命令列表**（跨列表 setPermanentBufferState 无物理 barrier，DX12 debug layer 执行期 fail-fast、录制期零消息）。Vulkan RT 扩展链已接。HUD `BLAS : NNN tri` 行只在 dag/hiz 模式（dag-top 走 initCluster 不触发）。`reshot-hud-stage.py --force`=人工追因后放行超签名重拍的合规通道。
- 已 merge 回 master：physics（M-P0~P4+M-J5c）、audio、ECS E1a+E1b、job-system、console/material-graph/action-system、pbr-direct、game-loop GL0~GL3。E1/E1b gate 已解除；jobsync P1 已解决（launchAll 原语）。
- 下一站：**M8.1 TLAS 每帧重建（D-S8-1 待用户拍板：S8-PLAN §5 建议每帧全量 rebuild，UE 口径）→ M8.2 Ray Query 闭环**；卡上登记 M8.1 实例 buffer 沿用同列表状态转换纪律。
- UE 差异台账 `docs/s5/UE-DIFFS.md`（D1~D18）随手追加必须落档；kStage bump 波及 = args 含 `--hud` 且 exe=current 的用例全重拍（`tools/reshot-hud-stage.py` 通用安全闸，超 527px 签名必须追因禁强刷——追因过了才许 `--force`）。cases.json：version 卡必须带 tag 字段；text-only（无 {out}）viewer 不退出，log 断言卡一律 pair 小图+expectLog 双断言。

## 渲染 / HUD 关键约定（详 `docs/SHADER-CONVENTIONS.md`）
- SPIR-V 绑定 `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；禁 `[[vk::binding]]`。DXIL 入口由 `-entry` 定，SPIR-V 恒 `main`。volatile CB `setMaxVersions(16)` **每帧全量覆写**（批内差异字段必须进 draw 期写入面）；SceneConstants 160B 禁整块 memcpy；`setFrontCounterClockwise(true)`。
- **§2.7 每管线单一 BindingSet；§2.8 Hi-Z POT+R32F 1e9+帧内禁第二 cmd list；§2.9 64 位原子；§2.10 禁 startInstanceLocation 的 SV_InstanceID 全局语义；§2.13 阴影 R32F MIN 混合+PCF 整数 texel+坡度 bias；§2.14 材质间接表（S9/S10/S13 复用）；§2.15 buffer 每帧写回状态登记；§2.16 draw 主路径重构必须保留 initCluster 场景 `modelBatches_.empty()` 单批兜底 + spot 必须含 lod/dag 模式**（M7.2 真回归教训：重写丢兜底 → 三模式空画面，spot 全绿全因测的都是别的路径）。
- GPU 资源 slot 级同步表必在 `createTargets()` 初始化；空纹理槽绑 1x1 dummy 过 NVRHI 校验。HUD：`hudProjection()` Y 尺度负、面板高≤560、字符串 ASCII。

## .nmesh / 简化器
- 尾段按 magic 识别：v2='NLOD'、v3='NFLB'（+'NSUB' 子网格段，4 条 selftest）、v4='NPAG' 留 P9。**`Meshlet::triangleOffset` 是字节偏移不是三角号**。简化器=meshopt 多 pass+link condition+双层边界锁+朝向锚；`check-orient` 扫全链 inward；加速类改动必须带对拍自证。

## ECS / job-system 关键契约
- `ComponentOf<T>::nextTypeID()` 计数器必须在非模板 inline 函数；`createBindingSet` 晚于 buffer 创建；多灯按 `1/ΣI` 归一化。jobgraph：create 后计数=numDeps+1，link 完必须 `launch()` 发布。并行 tick 契约：并行段禁结构变更、未声明访问=自足、确定性=并行==串行逐位。
- **⚠️ JobRuntime 析构次序铁律（2026-11-10 实锤修复）**：Jolt `Job::Execute()` 末尾就通知 barrier 放行主线程，排队引用 `Release()` 在池 worker 闭包里更晚执行 → `JobRuntime` 必须 **pool 声明在 adapter 之后（析构 pool 先死）**，否则 worker 最后一条 Release 对已析构 JobSystem 虚调 FreeJob → 纯虚调用 fail-fast（崩率随线程数升，Heisenbug，时序扰动即消失）。ThreadPool ctor 已带 spawn 重试+降级；phys selftest M-J5c 段有 try/catch 转显式 FAIL。
- **P1 已解决（2026-11-10）**：`m-j5c-prep/selftest-jobsync` 间歇 exit 1 实锤 = 测试违反契约 3（launch 与 link 交错建图 → downstream 被提前发布 → 后续 link 全被拒）。新增 `JobSync::launchAll` 原语把「帧前建图全 link → 帧中统一放行 → 帧后回收」编码进 API；**jobsync 使用纪律：建图期全 link，放行统一走 launchAll，禁止单发 launch 与 link 交错**。

## 并行轨道易忘点（详 `docs/PARALLEL-TRACKS.md`）
- wt_pool：开场声明编号 → claim；收口 close 硬闸 + release --done。树边界闸 `tools/tree_gate.py`：脚本树≠cwd 树 exit 2。worktree junction 只能 PowerShell 原生 API 建删；**分支保留**。python 孪生对拍防假绿（不适用并发类）。

## 工程纪律易忘点（详 `AGENTS.md`）
- 收口回复第一屏=「四项自查+三要素」表格；渲染轮次主动展示证据图卡片。默认不跑全量回归（`--only` 挑 2~3 条），全量只在打 tag 收口跑一次；失败分组统计禁 `head` 截断。双后端分档 bytes/near/FAIL 禁笼统「一致」。工具 exe=`build/{DX12,VULKAN}/nanite-tools.exe`（根目录同名是废弃残留）。
- git 危险操作闸门：checkout/restore/switch 前必先 `git status --porcelain -- <path>` 核对未提交修复；还原只许显式点名。Windows 清理临时文件禁通配。cases.json 负向卡 expectLog 必须从实测 log 复制禁手写；`--only` 语法是单个 `version[/case]`，逗号列表会静默跑 0 条报全绿。
- UE 源码只读不抄；Carrot/bevy 抄思路不抄代码。回复语言简体中文（代码/标识符保留英文）。调试：L0 画面证据 → L1 RenderDoc → L2 NVRHI timestamp → L3 Nsight/PIX/PresentMon/Tracy；NVRHI 每处调用回 vendored `nvrhi.h` 核签名。
