# nl-engine 项目长期约定

## 架构（2026-11-06 定稿）
- 通用管线：NVRHI + Slang，构建链**不依赖任何厂商图形 SDK**。`CMakeLists.txt` 不得出现 `VULKAN_SDK` / `find_package(Vulkan)`。
- 后端对称：D3D12 走 Windows SDK；Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，loader 是驱动自带 `vulkan-1.dll`；Slang 独立获取。
- **OpenGL 后端已否决（2026-11-07）**：vendored NVRHI 无 GL 后端，且破坏「一份 .slang 出 SPIR-V/DXIL」单源约定，S1~S14 方向 GL 无意义，别再捡起。
- `NL_GRAPHICS_BACKEND`（AUTO/DX12/VULKAN）唯一后端开关；Windows AUTO 落 DX12。
- 图形 API / NVRHI / ImGui 符号只允许在 `renderer/`；`core/` / `viewer/` 零命中。
- 一份 `.slang` 双目标（SPIR-V+DXIL）两后端加载成功 = 通用管线验收核心。

## 开发节奏
- 每阶段：目标 → 亲眼可见用例 → 验收判据 → 放行；tag 在验收之后打。
- **里程碑「交代」（2026-11-07）**：渲染版 = `example/` 基线 PNG + `run.bat`；工具版 = 展示台 `kind:"tool"` 用例（点运行实跑、输出展开；usage/正向/负向三条起步，`expectExit` 0/1/2）+ `docs/s*/...-TOOLS-GUIDE.md` 式归档 + 一张工具真实输出生成的证据图（`preview` 静态图，不许手绘）。规范见 `docs/ROADMAP.md`「里程碑交代规范」节。
- 展示台 8787 端口有旧会话残留时新实例绑定静默失败，先 `netstat -ano | grep 8787` 清残留。
- 每轮迭代更新 `example/`：基线 PNG + `run.bat` 进 `example/<版本>/`，登记 `example/cases.json`（唯一数据源）；验收/tag 前 `python tools/showcase/server.py --check` 退出 0 才放行（以 `renderer/src/renderer.cpp` 的 `kStage` 判定）。
- **M0.9 起总闸**：提交/tag 前 `python tools/regression.py` 全绿（全量历史用例 × 双后端比对基线）。
- 每小里程碑本地提交 + 打 tag；`docs/MILESTONES.md` 四栏（动作/提交版本/完成情况/踩坑）写实追加。
- 双后端画面一致分档：`bytes`（MD5）/ `near`（≤1/255 且 ≤2 px）/ FAIL，禁止笼统写「一致」。

## Agent 协作纪律 · 决策前置铁律（权威落点仓库根 `AGENTS.md`）
- **每轮新任务开门四件事，缺一不可**：①读 Carrot `E:\UE_STUDY_2026\Carrot\engine\engine\render\` + `engine\resources\shaders\`；②读 bevy `E:\UE_STUDY_2026\bevy\crates\bevy_pbr\src\meshlet\` + `crates\bevy_solari\src\realtime\`；③读 UE5 `E:\UnrealEngine\5.8.4\Engine\Source\Runtime\Renderer\Private\`（只读点名文件，不漫游）；④读开发历史 `docs/MILESTONES.md` + `docs/carrot-history.md` + `docs/bevy-history.md` + 本文件 + 当日日志 + 当前 `docs/s*/S*-PLAN.md`。对应文件查 ROADMAP「步骤→本地代码对照总表」。
- **不许擅做决定**：计划/S-PLAN 验收判据内、不改架构/范围/约定的机械执行可自己做；凡触碰架构/范围/选型/约定、与历史冲突、三处口径不一致需二选一，**先摆三处证据+历史约定，再请用户拍板**。用户架构质疑先自查自相矛盾、认了一次改到位，不拿依赖清单/官方文档辩护、不留「也可以……」尾巴。
- 若某轮已没遵守：回复里立刻承认、补读三处、重做决断，并同步清理相关文档旧表述。

## 本机环境（2026-11-06 实测）
- VS2022 + MSVC 14.38 + Windows SDK 10.0.26100.0；CMake 3.31.6 便携版在 `E:\UE_STUDY_2026\.build-tools\`。
- CMake 必须用 `NMake Makefiles`（VS 生成器被 HTTPS_PROXY/https_proxy 大小写撞键搞挂）。
- 网络：GitHub 小文件开代理直连，大文件 gh-proxy 分 8 段并发。

## 版本与里程碑
- 0.x：`s0-init`→M0.1 … `s0-done`→M0.5；`m0.6` GPU 计时、`m0.7` 高模+`--model`、`m0.8` 线框/调试着色(f1ba95f)、`m0.9` 回归总闸(58/58)。`v0.1.0`/`v0.4.1` 是快照 tag。
- **S1 收官**：`s1-tools`→M1.0、`s1-cluster`→M1.1、`s1-asset`→M1.2、`s1-done`→M1.3（kStage=S1.0，回归 82/82）。
- **S2 收官**：`s2-group`→M2.0(88)、`s2-qem`→M2.1(94)、`s2-loop`→M2.2(102)、`s2-done`→M2.3(f5bfc9a)、**M2.3a 翻转面根治**(fdb598c，ctest 12/12、回归 118/118)。
- **S3 收官**：`s3-dag`→M3.0(32a10b0)、`s3-fallback`→M3.1(d7b87e1)、`s3-done`→M3.2(150/150 ALL GREEN)。

## S3 约定（M3.0 起）
- **按组重切是硬前提**（UE ReduceGroup / bevy:154）：父簇绝不跨组 → 每父簇恰一生成组 → S5 组原子 cut 才成立。血缘落盘 `.nmesh` meta version 2：u32 groupCount + 32B 组记录（error/simplifyError/sphere[4]/genFirst/genCount）+ u32 clusterGroup×meshlets；v1/旧 v2 逐字节兼容。组 error=max(组简化误差, 成员簇误差)（UE:1441/:1587）逐位精确；父簇 bounds.error 统一携带组误差（UE:1593）；顶层组 genCount=0。
- `nanite-tools dag` 六项校验全 PASS 才 exit 0（membership/generation/error chain/parent error/monotonic/acyclic DFS）；`selftest dag` 128×128 全链全量断言不抽样。
- **按组重切簇粒度取整**：单层保留率上界 70%→~80%（8 簇/组整簇产出，bevy/UE 同款），loop 自检口径 30~80%。
- Suzanne 新链 7 层：3936→1970→1002→500→252→142→104；dag 校验 56 groups/57 edges/87 父簇。Suzanne.lod0..6.nmesh（v3）入仓。
- **M3.1 fallback**：`.nmesh` 文件 version 3='NFLB' 自描述尾段（按 magic 识别，v1/v2 兼容）；落点 `<prefix>.lod0.nmesh` 尾段；buildFallbackMesh=weldPositions 口径顶层拼接（UE BuildFallbackMesh 对照，2.6%≈默认 2%，不做第二次独立简化）；selftest fallback 灵魂断言=三角守恒+weld 收敛+边流形每边≤2。**Meshlet::triangleOffset 是字节偏移不是三角号**（读三角窗口一律 `base=triangleOffset+t*3`）。
- **M3.2 固化**：①expandClusterData 索引基址必须用 `clusterBase=vertices.size()` 快照（掩码跳簇后 kept 簇全错位）；②离线探针顶点切片 stride=8 float（32B MeshVertex），探针工具要有自校验；③每次 kStage bump 必须重拍 m0.9 gate-hud 与 s2.0 lod-hud 两对基线；④exe 快照规范只在「tag 状态==基线状态」时可用（s2.0/m0.9 viewer 卡恒指当前构建）。
- 工具卡 exe 恒指当前构建，重编后两份 `build/{dx12,vulkan}/nanite-tools.exe` 都要用本后端重编（别 cp 盖，NMake 时间戳陷阱）。
- 下一步：按 ROADMAP 顺延 S4（性能基准）/ S5（运行时组原子 cut，bevy/zeux 屏幕空间选层公式存档 `cull_shared.wesl:13`）。

## S2 离线管线约定（M2.0 起）
- METIS vendored 在 `thirdparty/metis/`（METIS 5 + GKlib，自写 CMake 编 `nl_metis`；metis.h 宽度宏 32 位已固定，MSVC `__thread=__declspec(thread)` 在 vendored CMake 固化）。CONTIG=1 仅整图连通时设，否则 rc=-4。
- 组大小目标 8 簇/组（`core/grouping.hpp` `kTargetMeshletsPerGroup`，bevy 0.15 实证值）。`grouping.cpp` 是 core 里唯一碰 METIS 符号的文件；viewer 不调用 groupClusters。
- **regression/展示台的工具 exe 是 `build/{dx12,vulkan}/nanite-tools.exe`，重编后手动 cp 同步两份**。
- UE 离线构建参考 `E:/UnrealEngine/5.8.4/Engine/Source/Developer/NaniteBuilder/`（GraphPartitioner/ClusterDAG/SimplifyTriangles，只读不抄）；翻面判定参照 `Developer/MeshSimplifier/MeshSimplify.cpp`（TriWillInvert 实时位置、InversionPenalty）；Carrot 本地无离线简化实现。
- **.nmesh v2（M2.2 起）** = v1 全部字段 + 尾部 `NMeshMetaHeader(24B, magic 'NLOD')` + `float[3*lockedCount]`；`lodMeta.present` 决定写 1/2，`loadNMesh` 两版都读。分层资产 `<prefix>.lodN.nmesh` 由 `nanite-tools lod -o <prefix>` 产出；lockedPositions 存**位置**不存点号，语义=本层按当前分组重算的组间边界点（顶层为空）；`maxError`=parent error 单调不减（S3 cut 判据）。
- **简化器口径（M2.3a 定稿）**：折叠结构=meshopt 多 pass 主循环（src 并入 dst、collapse_locked、cos75°）+ link condition + 双层边界锁（组间边界 + 模型开放边界 LockBorder）+ 绝对朝向锚（判决实验定稿保留）；`check-orient` 扫全链 inward（exit 1=FAIL），`simplify` 内置 ours vs oracle 对比行。

## 渲染约定（改前看 `docs/SHADER-CONVENTIONS.md`）
- 资源与绘制逻辑收 `renderer/src/scene.cpp`（两后端共享，只依赖 nvrhi.h）；backend 只管 swapchain/depth/framebuffer/present。
- SPIR-V 绑定偏移固定：`slangc -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；shader 只写 `register(b0/t0/s0)`，禁 `[[vk::binding]]`。
- SPIR-V 入口恒 `main`，DXIL 才 `vsMain/psMain`。
- MVP 用 volatile constant buffer（`setMaxVersions(16)`）；`SceneConstants` 严格 160 B，**禁整块 memcpy**（尾部有 bool cullBack）。
- 剔除：pipeline 必须 `setFrontCounterClockwise(true)`，否则剔掉正脸。「关剔除看内壁」用例 `--dist 0.6 --cull none`。
- 资产 `assets/models/Suzanne.glb`；tinygltf 只在 scene.cpp 一个 TU 展开（`TINYGLTF_NO_STB_IMAGE_WRITE`）。
- 新增 GPU 资源时 slot 级同步表必须在 `createTargets()` 初始化（renderFrame 在 ensurePipeline 前按 slot 访问，放错=首帧 139 段错误）。

## HUD / 模式约定（改前看 `docs/IMGUI-GUIDE.md`）
- ImGui vendored **v1.91.9 稳定版**；不用官方 backend，自写 `renderer/src/hud.cpp`（ImDrawData 摊平 + R32_UINT + 逐 cmd scissor）。
- `hudProjection()` Y 尺度必须为负。动态 VB/IB 必须 `initialState=CopyDest` + `keepInitialState=true`。
- `HudFrame` 唯一双向 POD；`renderFrame(..., hud=nullptr)` 完全不碰 ImGui → 截图默认 `--no-hud`。
- HUD 字符串必须 ASCII；`--hud` 两后端 PNG 必然不同。
- 面板高度上限 510：每次改 HUD 布局后 960×540 实拍确认完整。
- `--mode` 分发器在 `core/modes.*` + viewer `runMode()`；每加模式只改枚举/名字/case，未知模式 exit 1。
- M0.8：着色模式 `g_options.x` 分支（0 textured/1 normal/2 uv/3 tri-id），规范名 `tri-id`；tri-id 用 SV_PrimitiveID + pcg hash；线框=RasterFillMode::Wireframe，pipeline 4 套；V/N 键；`--shading` 非法值 exit 1。
- M0.6：`GpuTimer` 在 `renderer/src/gpu_timer.hpp`（4 slot 环形，懒建/poll 不阻塞）；deterministic 模式不写回真实 GPU ms；基线参数必须与 cases.json 一致；`frame ms`=CPU 间隔，GPU ms 才是 GPU 耗时。

## 调试工具链（S1~S14 沿用）
- L0 画面证据 → L1 RenderDoc（外部 Launch 零改动；精确抓帧才加 `--capture-frame N`）→ L2 NVRHI timestamp（整帧已由 M0.6 补）→ L3 Nsight/PIX/PresentMon/Tracy。
- RenderDoc 主工具因同时覆盖 DX12+Vulkan；工具不进 CMake、不读 SDK 路径。

## 参考文档（已 vendored，文档级不入构建）
- NVRHI 官方 `NVIDIA-RTX/NVRHI`（权威参考 `doc/Tutorial.md`+`ProgrammingGuide.md` 存 `thirdparty/nvrhi/doc/`），每处调用回 vendored `nvrhi.h`(2021 代) 核对签名。
- Slang 示例 `thirdparty/slang-reference/`；NVRHI 完整示例 `thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`；Falcor = Slang 生产级参考。
- Carrot：meshlet+meshoptimizer 是 2023-11-25 同一提交 `4c100761`（无截图 MD5 回归）；其 vendor+切簇塞一个提交的教训 → 我们拆版本铺路。
- S1 卡住翻：bevy `bevy_pbr/src/meshlet/from_mesh.rs`、meshoptimizer `src/meshlet.cpp`、`docs/blogs/jms55-bevy-meshlet.md`。

## P9 真实资产落地验收（2026-11-08 用户拍板定稿，计划已入 ROADMAP）
- **阶段定位**：主线 S0~S14 + P8 之后的**收尾验收关，也是项目立身之本**。前 8 阶段用测试模型，P9 换用户提供的真实资产（含超大场景）做全链路真验。大场景加载是项目意义所在，不是可选项。
- **四里程碑**：V1 接入（离线管线+分层画廊无黑斑无裂缝无洞，M2.3a 口径）→ V2 超大场景全链路（**首版即须跑通，稳定 60fps，不以流式为前提**）→ V3 多物体真实场景（承接 P8 E1）→ **V4 完整流式落地**（`.nmesh` 分页 + renderer streaming 接口由「全常驻」替换为「分页 residency」，超显存超大场景按页加载/卸载、帧平稳无爆闪）。**流式非可选、非推远，是 P9 必做收尾。**
- **提前预留流式接口（用户拍板）**：`.nmesh` 与离线管线设计须**分页友好**（cluster 自包含、顶点可分页、支持按簇/页独立加载，不为全局共享顶点池焊死）；renderer 预留 streaming 抽象接口，V4 替换实现。
- 资产预处理由离线管线 cleanup/weld/法线继承消化（沿用 S2 `rebuildMeshlets` 口径）；资产来源用户给。
- 验收复用 `example/cases.json` + `tools/regression.py` 总闸。`.nmesh` 分页格式 **2026-11-08 已拍板**（v4=v3+强制 `NPAG` 页表；分页粒度=字节预算默认 128KiB；边界顶点复制=接受；V1 即写 v4；页分区种子=复用 S3 dagGroups），字段草案 `docs/p9/NMESH-PAGING-DRAFT.md`（状态 DECIDED）；**代码落地推迟到 V1 启动**，避免现在打破 S0~S14 回归基线（M3.2 150/150）。
- **三处证据印证（2026-11-08 复读三处确证）**：Carrot `ClusterManager.cpp:144-150` 每簇顶点自包含（Carrot 本身无流式模块，印证流式是公认后期重活）；bevy `aa626e4f0b` 顶点自包含"Prepare for streaming" + `bdaf47c72b` 大场景示例；UE5 `NaniteStreamOut.h` 偏移寻址 + `kb:820` `NaniteStreamingManager` 分页驻留。三处一致结论=**顶点自包含 + 偏移寻址 + 独立 page/residency 管理**是流式正路，`kb:475` 警告的全局共享顶点池三处来源无一采用。详细计划见 `docs/p9/P9-PLAN.md`（目录避 `docs/s9/` 已属 S9 步骤 GBuffer+阴影）。

## 新增研究模块决议（2026-11-08，用户拍板「前三个安排上」）
- 用户点名「前三个」= VSM / GPU-Driven 间接绘制骨架 / TSR。
- **GPU-Driven 间接绘制骨架 已 = ROADMAP 的 S4（硬件光栅+GPU视锥剔除+indirect draw），不重复建。**
- **VSM（Virtual Shadow Maps）与 TSR（Temporal Super Resolution）为真正新增模块**，S15/S16 编号保留；**2026-11-08 用户拍板提前**：VSM 插入 **S9 之后**（阴影阶段 rasterized 对照路线，合并 `shadow-map` 穿插项，依赖 S4+S7/S9 在 S9 已就绪）；TSR **拆为两段**——velocity/history 基建 **S7 之后**先建喂 S12，完整算法 **S7 之后**轻量场景验证，最终 slot tonemap 前。开工前按铁律读三处参考（UE 为主，bevy/Carrot 若无对应实现如实标注）。
- **VT（Virtual Texturing）用户拍板跳过**：流式靠 residency 层（V4 几何分页）+ 纹理随几何 page 走，不引入 feedback-buffer VT；VT 留作「未来纹理成瓶颈」的可选增强。
