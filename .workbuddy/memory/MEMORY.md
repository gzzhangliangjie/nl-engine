# nl-engine 项目长期约定

> 铁律/决策前置/里程碑纪律的权威落点在仓库根 `AGENTS.md`；并行轨道详 `docs/PARALLEL-TRACKS.md`；逐里程碑台账以 `docs/MILESTONES.md` 为准。本文件只记易忘不变量与当前状态。

## 架构（2026-11-06 定稿，不可动摇）
- NVRHI + Slang 通用管线，**构建链禁依赖任何厂商图形 SDK**（CMakeLists 禁 `VULKAN_SDK`/`find_package(Vulkan)`）。D3D12 走 Windows SDK；Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，loader=驱动自带 `vulkan-1.dll`；Slang 独立获取。
- **OpenGL 后端已否决**；`NL_GRAPHICS_BACKEND`(AUTO/DX12/VULKAN) 唯一开关，Windows AUTO→DX12。
- 图形/NVRHI/ImGui 符号**只在 `renderer/`**；`core/`/`viewer/` 零命中。
- 一份 .slang 双目标(SPIR-V+DXIL)两后端加载成功 = 通用管线验收核心。

## 本机环境（2026-11-06 实测）
- VS2022 + MSVC 14.38 + Win SDK 10.0.26100.0；CMake 3.31.6 便携版 `E:\UE_STUDY_2026\.build-tools\`。
- CMake 必用 **NMake Makefiles**（VS 生成器被 HTTPS_PROXY 大小写撞键搞挂）；vcvars64.bat 被 reg.exe 沙箱黑名单拦 → 用 `scripts/build-m0.2.bat DX12|VULKAN`（显式 INCLUDE/LIB）。

## 当前进度（2026-11-09）
- 主线 S0~S5 已收官（tag s0-done…s5-done，实况看 MILESTONES 台账）；A 类并行轨道 job-system / material-graph / action-system 均已全量合入 master 并拆 wt（分支保留）。
- 当前 `kStage=S6.1`：**M6.1 Hi-Z 遮挡剔除串进 DAG 遍历已收口**（tag `s6-cull`）。两项拍板：①金字塔 **min→max**（S6-PLAN 原文 min 口径不保守会漏画；权威口径=簇最近点 vs 区域最远表面，径向距离度量下取 max）；②**单 dispatch 内联**（bevy 式 HZB second pass 不做，=D3a 升级，触发条件未到）。验收：HizScene 墙场景 hiz 24/85 vs dag 40/85（整树剔除）、穿墙侧视 hiz==dag 零误剔、覆盖层外逐像素 0 差、双后端 bytes。关键坑固化 §2.8：view space 右手系 -z 朝前须 `wz=-vc.z` 换轴（否则剔除静默失效假绿）、top 回读三态（1e9=背景主导合法）、帧内禁开第二 cmd list。主线下一步=**M6.2 开关对照 + 验收**（ImGui 一键开关 Hi-Z、HUD 剔除前/后簇数与帧时间、S4 1M+ 大模型+挡板；tag `s6-done`，`docs/screenshots/s6.png`）。
- **UE 差异台账** `docs/s5/UE-DIFFS.md`（D1~D9），里程碑收口/review 前过一遍。
- **S7/S8 预研完成（2026-11-09）**：三处对照+NVRHI RT 接口核查已固化进 `docs/s7/S7-PLAN.md` / `docs/s8/S8-PLAN.md` 的「预研结论」节（含 S7 打包定稿 Option A、barycentrics 解析重构作废另存 RT、S8 NVRHI 签名速查表）。**两处拍板点待开工前确认**：D-S7-1 材质 pass 执行模型（建议 Carrot 式单全屏 pass）；D-S8-1 TLAS 每帧全量 rebuild（UE 口径，update 路径留升级项）。
- kStage bump 波及：**只有 exe=current 的版本需重拍基线**（重拍前查 cases.json 的 exe 字段指向）；每次 bump 重拍 m0.9/s2.0/s3.2 三对；HUD 布局演进波及含该模式的所有 HUD 基线，总闸兜底。
- P9 真实资产验收四里程碑 V1~V4（V4 流式分页必做，非可选）。

## 工程纪律（易忘点）
- 每阶段：目标→亲眼可见用例→验收判据→放行；tag 在验收后打。
- **收口前四项自查清单**（M6.0 教训）：① example/<版本>/ 基线+run.bat+cases.json+`server.py --check` 退出 0；② 工具交代物（无产出要写明）；③ MILESTONES.md 四栏台账；④ 固化文档。四项齐才许宣布收口。收口回复三要素：提交版本（hash+tag）/ 修改文件清单（git show --stat 逐文件）/ 固化内容。
- **总闸（2026-11-08 拍板）**：默认不跑回归，改什么就 `regression.py --only <版本>[/用例]` 挑 2~3 条；全量只在打 tag 收口跑一次（用例间并行、同用例双后端串行）。
- 双后端分档：`bytes`(逐字节)/`near`(≤1/255 且 ≤2px)/`FAIL`，禁笼统「一致」。
- 工具 exe=`build/{DX12,VULKAN}/nanite-tools.exe`（构建树直接产物）；根目录 build/nanite-tools.exe 是废弃残留**勿用**；NMake 同秒 mtime 跳重链 → 删 exe 强制重链。
- example/*/run.bat for 块内必须 `!VAR!` 延迟展开（`%VAR%` 解析期为空=静默假绿）。
- UE 源码**只读不抄**（EULA）；Carrot/bevy 抄思路不抄代码。

## 资产格式 .nmesh
- v2=v1+尾部 `NMeshMetaHeader(24B,'NLOD')`+`float[3*lockedCount]`；v3 加 `'NFLB'` 自描述尾段（按 magic 识别，v1/v2 兼容）。
- 组记录 32B：error/simplifyError/sphere[4]/genFirst/genCount；组 error=max(组简化误差,成员簇误差) 逐位精确；顶层组 genCount=0。
- **`Meshlet::triangleOffset` 是字节偏移不是三角号**（读三角 base=triangleOffset+t*3）。
- 按组重切硬前提：父簇绝不跨组→每父簇恰一生成组→S5 组原子 cut 成立；单层保留率上界 ~80%。
- fallback=weldPositions 顶层拼接（2.6%≈UE 默认 2%），不做第二次独立简化。
- v4 分页（128KiB 预算+`NPAG` 页表）落地推迟到 P9 V1。

## 渲染 / HUD 约定（改前看 `docs/SHADER-CONVENTIONS.md` / `docs/IMGUI-GUIDE.md`）
- SPIR-V 绑定：`slangc -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；shader 只写 `register(b0/t0/s0)`，禁 `[[vk::binding]]`。
- 入口名两阶段：编译期 `-entry` 决定 DXIL 入口名；SPIR-V 恒 `main`（加载期 `setEntryName` 仅 Vulkan 用，必须 main；DX12 忽略）。
- MVP 用 volatile CB(`setMaxVersions(16)`)；`SceneConstants` 严格 160B **禁整块 memcpy**；pipeline 必 `setFrontCounterClockwise(true)`。
- **§2.7 每管线必须单一 BindingSet**：slangc 把资源全编进 vk set 0，多 BindingLayout 在 Vulkan legacy 模式拆多 vk set → 第二 set 的 b1 绑不上读 NaN（DX12 无 set 概念侥幸不炸，validation 静默）。
- **§2.8 Hi-Z 金字塔尺寸必须 POT**（非 POT ceil 对半链超 mipLevels 上限；DX12 debug layer 报错、Vulkan 无 VVL 静默建坏）；R32F RT 必须 `setClearValue(1e9)`。
- 资源/绘制逻辑收 `renderer/src/scene.cpp`（两后端共享，只依赖 nvrhi.h）；backend 只管 swapchain/depth/framebuffer/present。
- ImGui v1.91.9 + 自写 `hud.cpp`；`hudProjection()` Y 尺度必须负；面板高≤560；HUD 字符串须 ASCII；`hud=nullptr` 不碰 ImGui。
- 新增 GPU 资源 slot 级同步表必在 `createTargets()` 初始化（放错=首帧 139 段错误）。
- **viewer --screenshot 建隐藏窗口**（visible=false 防抢焦点）；交互模式仍 visible=true；tag 快照 exe=tag+隐藏窗口补丁，非纯 tag 原样。

## 简化器（M2.3a 定稿）
- meshopt 多 pass 主循环（src 并入 dst + collapse_locked + cos75°）+ link condition + 双层边界锁（组间边界+LockBorder）+ 绝对朝向锚；`check-orient` 扫全链 inward（exit 1=FAIL）。

## 参考 / 调试
- NVRHI 权威 `thirdparty/nvrhi/doc/`（Tutorial+ProgrammingGuide），每处调用回 vendored `nvrhi.h` 核签名（⚠️ 2026-11-09 修正：该头文件版权头写 2014-2021，但 API 面是含 cluster ops/OMM/LSS/CoopVec/RTXMU 的**新 fork**，非纯 2021 代；S8 用到的 RT 签名速查表见 `docs/s8/S8-PLAN.md` 预研 §1）；Slang 示例 `thirdparty/slang-reference/`；NVRHI 完整示例 `thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`。
- 调试：L0 画面证据 → L1 RenderDoc → L2 NVRHI timestamp → L3 Nsight/PIX/PresentMon/Tracy。

## 并行轨道（详 `docs/PARALLEL-TRACKS.md`，权威）
- A 类=非渲染（不进 kStage/渲染回归，走工具版交代）；B 类=渲染研究只作主线穿插项。新轨道=新 `feat/*` 分支+新 worktree；收官=全量合回 master+拆 wt+**分支保留**。
- 状态（2026-11-09）：三轨已收官；console 进行中（M-C0~C2 完成，剩 M-C3 收口）；**ECS 进行中**（`feat/ecs` wt 已建 `E:/UE_STUDY_2026/nl-engine-ecs`，junction 共享，M-E0/M-E1 完成 tag `ecs-e0`/`ecs-e1`，feat/ecs dc9fadb；下一步 M-E2 System+JSON roundtrip，详 `docs/p8/P8-ECS-PLAN.md`，E1b 渲染集成等 S6 收官后）；寻路/物理/音频已立项 `docs/p8/`，执行排 E1（ECS）之后。
- **wt_pool 占坑闸**：任务号=里程碑编号；对话开场声明里程碑编号，`python tools/wt_pool.py claim --track <轨道> --milestone <编号>` 登记后动手；CLOSED 轨道硬拒绝；锁=主树根 `.wt_pool_busy.json`。
- worktree junction 共享 `.workbuddy` 与 `thirdparty/slang/dist`；**拆 wt 时 junction 只能删链接**（`.NET Directory.Delete(path,false)`/`cmd rmdir`），MSYS `rm -r` 穿透删共享内容。

## 非渲染候选拍板（2026-11-08）
- 总原则：**有参考的内容就考虑从头写**，参照实现只作对照与节奏校准。
- 寻路从零写（Carrot `pathfinding/` 节奏参照，UE `Runtime/Navmesh` 只读）；物理 vendored Jolt 不自研（原则例外）；音频 OpenAL Soft+自写集成层/解码器；三条均执行排 P8 E1 后。
