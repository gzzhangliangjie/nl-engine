# nl-engine 项目长期约定

> 协作铁律 / 决策前置 / 里程碑纪律的**权威落点在仓库根 `AGENTS.md`**，本文件只记易忘的精确不变量与当前状态，不重复那边的细则。

## 架构（2026-11-06 定稿，不可动摇）
- 通用管线 NVRHI + Slang，**构建链不依赖任何厂商图形 SDK**（`CMakeLists.txt` 禁 `VULKAN_SDK`/`find_package(Vulkan)`）。
- 后端对称：D3D12 走 Windows SDK；Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，loader=驱动自带 `vulkan-1.dll`；Slang 独立获取。
- **OpenGL 后端已否决**（vendored NVRHI 无 GL，破坏「一份 .slang 出 SPIR-V/DXIL」单源约定）。
- `NL_GRAPHICS_BACKEND`(AUTO/DX12/VULKAN) 唯一开关；Windows AUTO→DX12。
- 图形 API / NVRHI / ImGui 符号**只在 `renderer/`**；`core/`/`viewer/` 零命中。
- 一份 .slang 双目标(SPIR-V+DXIL)两后端加载成功 = 通用管线验收核心。

## 本机环境（2026-11-06 实测）
- VS2022 + MSVC 14.38 + Win SDK 10.0.26100.0；CMake 3.31.6 便携版 `E:\UE_STUDY_2026\.build-tools\`。
- CMake 必用 `NMake Makefiles`（VS 生成器被 HTTPS_PROXY/https_proxy 大小写撞键搞挂）。

## 当前进度
- 主线 S0~S3 收官：M0.1~M0.9(58/58) → M1.0~M1.3(82/82) → M2.0~M2.3a(118/118) → M3.0~M3.2(**150/150 ALL GREEN**)。
- 当前 `kStage=S1.0` 基线；主线下一步 S4(性能基准)/S5(运行时组原子 cut)。
- 并行研究模块：GPU-Driven 间接绘制=S4；VSM 插 S9 后；TSR 拆 S7 后两段。
- P9 真实资产验收四里程碑 V1~V4（V4 流式分页必做，非可选项）。

## 工程纪律（易忘点）
- 每阶段：目标→亲眼可见用例→验收判据→放行；tag 在验收后打。
- **总闸**：提交/tag 前 `python tools/regression.py` 全绿（双后端 × 全量历史用例）。
- 双后端一致分档：`bytes`(MD5 逐字节)/`near`(≤1/255 且 ≤2px)/`FAIL`，禁笼统写「一致」。
- 展示台 8787 端口残留先 `netstat -ano | grep 8787` 清。
- 工具 exe：`build/{dx12,vulkan}/nanite-tools.exe` 各后端重编后手动 cp 同步（NMake 时间戳陷阱，别 cp 盖）。
- `nanite-tools dag` 六项校验全 PASS 才 exit 0；`selftest dag` 128×128 全链全量断言不抽样。
- 每次 `kStage` bump 必须重拍 m0.9 gate-hud 与 s2.0 lod-hud 两对基线。
- UE 源码**只读不抄**（EULA）；Carrot/bevy 抄思路不抄代码。

## 资产格式 .nmesh（M2.2~M3.2 定稿）
- v2 = v1 + 尾部 `NMeshMetaHeader(24B,'NLOD')` + `float[3*lockedCount]`；v3 加 `'NFLB'` 自描述尾段（按 magic 识别，v1/v2 兼容）。
- 组记录 32B：error / simplifyError / sphere[4] / genFirst / genCount；组 error=max(组简化误差,成员簇误差) 逐位精确；顶层组 genCount=0。
- **`Meshlet::triangleOffset` 是字节偏移不是三角号**（读三角窗口 `base=triangleOffset+t*3`）。
- 按组重切硬前提：父簇绝不跨组 → 每父簇恰一生成组 → S5 组原子 cut 才成立。单层保留率上界 ~80%(8簇/组整簇产出)。
- fallback = weldPositions 顶层拼接（2.6%≈UE 默认 2%），不做第二次独立简化。
- v4 分页（128KiB 字节预算 + `NPAG` 页表，页分区种子复用 S3 dagGroups）**代码落地推迟到 P9 V1**，但 V1 即写 v4 不破 M3.2 回归基线。

## 渲染 / HUD 约定（改前看 `docs/SHADER-CONVENTIONS.md` / `docs/IMGUI-GUIDE.md`）
- SPIR-V 绑定偏移固定：`slangc -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；shader 只写 `register(b0/t0/s0)`，禁 `[[vk::binding]]`。
- SPIR-V 入口恒 `main`，DXIL 才 `vsMain/psMain`。
- MVP 用 volatile CB（`setMaxVersions(16)`）；`SceneConstants` 严格 160B，**禁整块 memcpy**（尾部有 bool cullBack）。
- pipeline 必 `setFrontCounterClockwise(true)`，否则剔掉正脸；`--dist 0.6 --cull none` 看内壁。
- 资源/绘制逻辑收 `renderer/src/scene.cpp`（两后端共享，只依赖 nvrhi.h）；backend 只管 swapchain/depth/framebuffer/present。
- ImGui vendored **v1.91.9**，自写 `renderer/src/hud.cpp`（ImDrawData 摊平 + R32_UINT + 逐 cmd scissor）；`hudProjection()` Y 尺度必须负；面板高度≤510；HUD 字符串须 ASCII；`renderFrame(...,hud=nullptr)` 不碰 ImGui（截图默认 `--no-hud`）。
- 新增 GPU 资源 slot 级同步表必在 `createTargets()` 初始化（renderFrame 在 ensurePipeline 前按 slot 访问，放错=首帧 139 段错误）。

## 简化器口径（M2.3a 定稿）
- meshopt 多 pass 主循环（src 并入 dst + collapse_locked + cos75°）+ link condition + 双层边界锁（组间边界 + 模型开放边界 LockBorder）+ 绝对朝向锚（判决实验定稿保留）。`check-orient` 扫全链 inward（exit 1=FAIL）。

## 参考 / 调试工具链
- NVRHI 权威参考 `thirdparty/nvrhi/doc/`（`Tutorial.md`+`ProgrammingGuide.md`），每处调用回 vendored `nvrhi.h`(2021 代) 核对签名。
- Slang 示例 `thirdparty/slang-reference/`；NVRHI 完整示例 `thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`；Falcor=Slang 生产级参考。
- 调试：L0 画面证据 → L1 RenderDoc（外部 Launch 零改动，精确抓帧才加 `--capture-frame N`）→ L2 NVRHI timestamp（M0.6 补）→ L3 Nsight/PIX/PresentMon/Tracy。

## 并行轨道纪律（2026-11-08 用户拍板）
- 固定 `feat/*` 分支 + 常驻 worktree 模式（已建 `feat/job-system` / `feat/material-graph` / `feat/action-system`）。**新轨道 = 新 `feat/*` 分支 + 新 worktree**（如本次 Slang 可视化编辑器）。
- 三 worktree 均 `junction` 共享主 worktree 的 `.workbuddy` 与 `thirdparty/slang/dist`；`.workbuddy` 已 `skip-worktree` 防纪律源误入 feature 分支（git status 干净）。
- 纪律三条（详 `docs/PARALLEL-TRACKS.md` §7）：① 实例注册表登记占用说明；② 固化文档闸（merge 回 master 前必写 `S*-PLAN`/`MILESTONES` 验收纪要）；③ 假绿防护（占坑≠文件指向，merge 前双向 `git status` 自查：wt 看得到、master 没有）。
- 工作线粘性：同一轨道回同一 worktree；一条工作线只 merge 一次（验收 FAIL→补丁→复验在占坑内闭环）。
