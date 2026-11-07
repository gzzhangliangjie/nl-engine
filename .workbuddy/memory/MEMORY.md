# nl-engine 项目长期约定

## 架构（2026-11-06 定稿）
- 通用管线：NVRHI + Slang，构建链**不依赖任何厂商图形 SDK**。`CMakeLists.txt` 不得出现 `VULKAN_SDK` / `find_package(Vulkan)` / 要求装 SDK。
- 后端对称：D3D12 来自 Windows SDK；Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，loader 是驱动自带 `vulkan-1.dll`（未装也无需装 Vulkan SDK）；Slang 独立获取。
- **OpenGL 后端已否决（2026-11-07）**：vendored NVRHI 无 GL 后端，移植成本高且破坏「一份 .slang 出 SPIR-V/DXIL」单源约定，S1~S14 方向 GL 均无意义。别再捡起这个话题。
- `NL_GRAPHICS_BACKEND`（AUTO/DX12/VULKAN）唯一后端开关；Windows AUTO 落 DX12。
- 图形 API / NVRHI / ImGui 符号只允许在 `renderer/`；`core/` / `viewer/` 零命中。调试层判据按当前后端走；VVL 可选、本机未装。
- 一份 `.slang` 双目标（SPIR-V+DXIL）两后端加载成功 = 通用管线验收核心。

## 开发节奏
- 每阶段：目标 → 亲眼可见用例 → 验收判据 → 放行；tag 在验收之后打。
- **每个里程碑必须有「交代」（2026-11-07 用户定）**：渲染版本 = `example/` 截图基线；工具版本 = **展示台网页 `kind:"tool"` 用例**（点运行实跑、输出展开；usage/正向/负向三条起步，`expectExit` 约定 0/1/2）+ `docs/s1/NANITE-TOOLS-GUIDE.md` 式归档。规范固化在 `docs/ROADMAP.md`「里程碑交代规范」节。注意：展示台 8787 端口有旧会话残留进程时新实例绑定静默失败，先 `netstat -ano | grep 8787` 清残留。**补图（同日用户追问「有图么」后固化）**：工具版本不产 GPU 截图，但必须有从工具真实输出生成的证据图（cases.json `preview` 静态图 + `example/<版本>/run.bat` 复现；server `declared_shots()` 收 `preview` 走 `/shots/` 白名单），不许手绘示意；样板 `example/s2-qem/qem-suzanne.png`（`--obj` 导出 + `tools/qem_preview.py` Pillow 离线渲染）。
- **每轮迭代必须更新 `example/`**：基线 PNG + `run.bat` 进 `example/<版本>/`，登记 `example/cases.json`（唯一数据源，别建第二份清单）；验收/tag 前跑 `python tools/showcase/server.py --check` 退出 0 才放行（以 `renderer/src/renderer.cpp` 的 `kStage` 判定阶段）。
- **M0.9 起总闸：提交/tag 前 `python tools/regression.py` 全绿才放行**（全量历史用例 × 双后端比对基线）。
- 每个小里程碑本地提交一波；`docs/MILESTONES.md` 四栏（动作/提交版本/完成情况/踩坑）写实追加。
- 展示台 `tools/showcase/start.bat` → http://127.0.0.1:8787/；干净 clone 先 `python tools/showcase/rebuild.py`；**exe 快照规范（2026-11-07 定案）：仅渲染 viewer 卡**每出新版把上一版映射改 `build/showcase/{tag}/` 并跑 rebuild.py 补建；**工具卡 exe 恒指当前构建** `build/{dx12,vulkan}/nanite-tools.exe`（无基线可比、现场实跑即权威），不建历史快照、不再补建。
- 双后端画面一致分档：`bytes`（MD5）/ `near`（≤1/255 且 ≤2 px）/ FAIL，别笼统写「一致」。

## Agent 协作纪律 · 决策前置铁律（2026-11-07 固化；权威落点仓库根 `AGENTS.md`）
- **每轮新任务开门四件事，缺一不可**：①读 Carrot 代码 `E:\UE_STUDY_2026\Carrot\engine\engine\render\` + `engine\resources\shaders\`；②读 bevy 代码 `E:\UE_STUDY_2026\bevy\crates\bevy_pbr\src\meshlet\` + `crates\bevy_solari\src\realtime\`；③读 UE5 官方 `E:\UnrealEngine\5.8.4\Engine\Source\Runtime\Renderer\Private\`（只读对照表点名文件，不漫游）；④读开发历史 `docs/MILESTONES.md` + `docs/carrot-history.md` + `.workbuddy/memory/MEMORY.md` + 当日日志 + 当前 `docs/s*/S*-PLAN.md`。当前步骤对应哪几份文件查本文件「S* 约定」节与 ROADMAP「步骤→本地代码对照总表」。
- **不许擅做决定**：在已定计划/S-PLAN 验收判据内、不改架构/范围/约定的机械执行可以自己做；凡触碰架构/范围/选型/约定、与历史决策冲突、或三处口径不一致需二选一，**必须先摆三处证据+历史约定，再请用户拍板**，不得直接落地。用户架构质疑先自查自相矛盾、认了一次改到位，不拿依赖清单/官方文档辩护、不留「也可以……」尾巴。
- 若某轮已没遵守：回复里立刻承认、补读三处、重做决断，并同步清理相关文档旧表述。

## 本机环境（2026-11-06 实测）
- VS2022 + MSVC 14.38 + Windows SDK 10.0.26100.0；CMake 3.31.6 便携版在 `E:\UE_STUDY_2026\.build-tools\`。
- CMake 必须用 `NMake Makefiles`（VS 生成器被 HTTPS_PROXY/https_proxy 大小写撞键搞挂）。
- 网络：GitHub 小文件开代理直连，大文件 gh-proxy 分 8 段并发。

## 版本
- tag 序列：`s0-init`→M0.1、`s0-device`→M0.2、`s0-triangle`→M0.3、`s0-model`→M0.4、`s0-done`→M0.5、`m0.6` GPU 计时、`m0.7` 高模资产+`--model`、`m0.8` 线框/调试着色(f1ba95f)、`m0.9` 一键回归总闸（0.x 终点，58/58 ALL GREEN）。`v0.1.0`/`v0.4.1` 是快照 tag，与里程碑 tag 互不覆盖。
- **S1 收官（2026-11-07）**：`s1-tools`→M1.0、`s1-cluster`→M1.1、`s1-asset`→M1.2、`s1-done`→M1.3（kStage=S1.0，回归 82/82）。
- **S2 收官（2026-11-07）**：`s2-group`→M2.0（回归 88/88）、`s2-qem`→M2.1（94/94）、`s2-loop`→M2.2（102/102）、`s2-done`→M2.3（提交 f5bfc9a）、**M2.3a 翻转面根治（提交 fdb598c，ctest 12/12、回归 118/118 ALL GREEN）**。M2.3：viewer `--mode lod N` + HUD 滑条；`Scene::uploadClusterMesh` 首载与切层同一条路；`probeLodLevelCount` 在 backend.hpp；缺省着色 normal、显式 `--shading` 生效（先场景缺省再覆写）。负向 viewer tool 卡必须带 `--screenshot` 否则进交互循环超时。**M2.3a 判决实验定稿（用户方法论三原则）**：官方两家翻面判定均**无存储参照场**（meshopt 实时 vertex_positions / UE TriWillInvert 实时 GetPosition）；摘锚纯官方口径跨层累计 L1=1→L6=8（worst cos -0.972），官方无逐层链场景可抄 → **朝向锚定稿保留**（变形后法线 vs 当前角点法线和 ≤~87°，随折叠同步更新；无法线数据的合成网格退化纯官方判定），ours inward=0 反超 oracle（2）；`rebuildMeshlets` **法线继承**（继承输入法和不重算，跨层=L0 原始场，与 check-orient 同口径）；分层资产 7 层 lod0..6 入仓。验收：七层画廊无黑斑无裂缝无洞，「L4/L5 已知翻转面」口径**作废**。backend 自证看日志 `shader=spv/dxil`（NMake 只看时间戳，cp 盖产物后必须删掉重编）。下一步 ~~S3 DAG~~ → **M3.0 已完成（见下节）**。

## S3 约定（M3.0 起，tag s3-dag 提交 32a10b0）
- **按组重切是硬前提**（UE ReduceGroup / bevy:154）：父簇绝不跨组 → 每父簇恰一生成组 → S5 组原子 cut 才成立（只 cut 一个父簇会同组兄弟重叠重复绘制）。血缘落盘 .nmesh **meta version 2**：u32 groupCount + 32B 组记录（error/simplifyError/sphere[4]/genFirst/genCount）+ u32 clusterGroup×meshlets；v1/旧 v2 逐字节兼容。组 error=max(组简化误差, 成员簇误差)（UE:1441/:1587）逐位精确；父簇 bounds.error 统一携带组误差（UE:1593）；顶层组 genCount=0。
- `nanite-tools dag <prefix> [--export json]`：六项校验全 PASS 才 exit 0（membership/generation/error chain/parent error/monotonic/acyclic DFS）；`selftest dag` 128x128 全链全量断言不抽样。
- **按组重切簇粒度取整**：单层保留率上界 70%→~80%（8 簇/组整簇产出，bevy/UE 同款），loop 自检口径 30~80%。
- **server.py stage_sync：preview 证据图计入基线证据**（M2.1 固化约定的落实，缺失文件照样 FAIL）；kStage=S3.0，viewer selftest stage 断言在 viewer/main.cpp（strstr("S3.0")）随 kStage 同步改。s1-tools not-impl 负向已退役为「未知子命令 exit 1」（工具侧占位子命令清零）。
- Suzanne 新链 7 层：3936→1970→1002→500→252→142→104；dag 校验 56 groups/57 edges/87 父簇。Suzanne.lod0..6.nmesh（v3）入仓。
- 工具卡 exe 恒指当前构建，重编后两份 build/{dx12,vulkan}/nanite-tools.exe 都要用本后端重编（别 cp 盖，NMake 时间戳陷阱）。
- **M3.1 fallback（tag s3-fallback 提交 d7b87e1）**：.nmesh **文件 version 3**='NFLB' 自描述尾段（按 magic 识别，v1/v2 兼容）；落点 <prefix>.lod0.nmesh 尾段（lod 写回，输入文件不动）；buildFallbackMesh=weldPositions 口径顶层拼接（UE BuildFallbackMesh 对照，2.6%≈默认 2%，不做第二次独立简化）；selftest fallback 灵魂断言=三角守恒+weld 收敛+边流形每边≤2。**Meshlet::triangleOffset 是字节偏移不是三角号**（M1.1 语义，读三角窗口一律 `base=triangleOffset+t*3`，meshlet.hpp 已钉死注释）。ctest 16/16、回归 132/132。
- 下一步：**M3.2 viewer --mode dag-top**（顶层渲染=正确粗模 + 任选组下钻一层验证父子衔接无洞）→ tag s3-done → S5 运行时 cut。

## S2 离线管线约定（M2.0 起）
- METIS vendored 在 `thirdparty/metis/`（METIS 5 + GKlib，自写 CMake 编 `nl_metis`；metis.h 宽度宏 32 位已固定，MSVC 宏 `__thread=__declspec(thread)` 在 vendored CMake 固化）。CONTIG=1 仅整图连通时设，否则 rc=-4。
- 组大小目标 8 簇/组（`core/grouping.hpp` `kTargetMeshletsPerGroup`，bevy 0.15 实证值；S2-PLAN 旧稿 50000-tri 是 UE page 口径已修订）。
- 纪律：`grouping.cpp` 是 core 里唯一碰 METIS 符号的文件；viewer 不调用 groupClusters，链接器不会拉 metis。
- **regression/展示台的工具 exe 是 `build/{dx12,vulkan}/nanite-tools.exe`，重编后要手动 cp 同步两份**，否则总闸跑的是旧 exe。
- UE 离线构建参考：`E:/UnrealEngine/5.8.4/Engine/Source/Developer/NaniteBuilder/`（GraphPartitioner/ClusterDAG/SimplifyTriangles，只读不抄）；翻面判定参照 `Developer/MeshSimplifier/MeshSimplify.cpp`（TriWillInvert 实时位置、InversionPenalty 罚金）；Carrot 本地无离线简化实现。
- cases.json 工具卡 select 空选项会展开成空 arg，工具侧需跳过空参数。
- **.nmesh v2（M2.2 起）** = v1 全部字段不变 + 尾部 `NMeshMetaHeader(24B, magic 'NLOD')` + `float[3*lockedCount]`；`NMeshData.lodMeta.present` 决定 saveNMesh 写 1 还是 2，loadNMesh 两版都读。分层资产 `<prefix>.lodN.nmesh` 由 `nanite-tools lod -o <prefix>` 产出（Suzanne 现为 7 层 lod0..6）；lockedPositions 存**位置**不存点号（逐层重切后编号全变），语义 = **本层按当前分组重算的组间边界点**（不累积历史层），顶层为空；`maxError` = parent error 单调不减（S3 cut 判据）。
- **简化器口径（M2.3a 定稿）**：折叠结构 = meshopt 多 pass 主循环（src 并入 dst、collapse_locked、cos75°）+ link condition + 双层边界锁（组间边界 + 模型开放边界 LockBorder）+ 绝对朝向锚（自创加码，判决实验定稿保留）；`check-orient` 子命令扫全链 inward（exit 1 = FAIL），`simplify` 命令内置 ours vs oracle 同判据对比行。自检预期已对齐 LockBorder 语义：孤四边形不折叠、末层收敛停滞（>70%）例外。

## 渲染约定（改前先看 `docs/SHADER-CONVENTIONS.md`）
- 资源与绘制逻辑收在 `renderer/src/scene.cpp`（两后端共享，只依赖 nvrhi.h）；backend 只管 swapchain/depth/framebuffer/present，别各抄一遍。
- SPIR-V 绑定偏移固定：slangc 带 `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；shader 只写 `register(b0/t0/s0)`，禁 `[[vk::binding]]`。
- SPIR-V 入口恒 `main`，DXIL 才 `vsMain/psMain`。
- MVP 用 volatile constant buffer（`setMaxVersions(16)`）；`SceneConstants` 严格 160 B，**禁止整块 memcpy**（尾部有 bool cullBack）。
- 剔除：pipeline 必须 `setFrontCounterClockwise(true)`，否则剔掉正脸。「关剔除看内壁」用例 `--dist 0.6 --cull none`。
- 资产 `assets/models/Suzanne.glb`；tinygltf 只在 scene.cpp 一个 TU 展开（`TINYGLTF_NO_STB_IMAGE_WRITE`）。
- 新增 GPU 资源时 slot 级同步表必须在 `createTargets()` 初始化（renderFrame 在 ensurePipeline 前就按 slot 访问，放错=首帧 139 段错误）。

## HUD / 模式约定（改前看 `docs/IMGUI-GUIDE.md`）
- ImGui vendored **v1.91.9 稳定版**（不是 Carrot 的 1.92.7 WIP）；不用官方 backend，统一自写 `renderer/src/hud.cpp`（ImDrawData 摊平 + R32_UINT + 逐 cmd scissor）。
- `hudProjection()` Y 尺度必须为负。动态 VB/IB 必须 `initialState=CopyDest` + `keepInitialState=true`。
- `HudFrame` 唯一双向 POD；`renderFrame(..., hud=nullptr)` 完全不碰 ImGui → 截图默认 `--no-hud`，老基线不动。
- HUD 字符串必须 ASCII（字体图集无中文）；`--hud` 两后端 PNG 必然不同，别误判。
- 面板高度上限 510：每次改 HUD 布局后 960×540 实拍确认完整。
- `--mode` 分发器在 `core/modes.*` + viewer `runMode()`；每加模式只改枚举/名字/case，未知模式 exit 1。
- M0.8：着色模式 `g_options.x` 分支（0 textured/1 normal/2 uv/3 tri-id），规范名 `tri-id`；tri-id 用 SV_PrimitiveID + pcg hash；线框 = `RasterFillMode::Wireframe`，pipeline 4 套；V/N 键；`--shading` 非法值 exit 1。
- M0.6：`GpuTimer` 在 `renderer/src/gpu_timer.hpp`（4 slot 环形，懒建/poll 不阻塞）；deterministic 模式不写回真实 GPU ms；基线参数必须与 cases.json 完全一致；`frame ms`=CPU 间隔，GPU ms 才是 GPU 耗时。

## 调试工具链（S1~S14 沿用）
- L0 画面证据 → L1 RenderDoc（外部 Launch 零改动；精确抓帧才加 `--capture-frame N`）→ L2 NVRHI timestamp（整帧已由 M0.6 补）→ L3 Nsight/PIX/PresentMon/Tracy。
- RenderDoc 主工具因同时覆盖 DX12+Vulkan；工具不进 CMake、不读 SDK 路径。

## 参考文档（已 vendored，文档级不入构建）
- NVRHI 官方仓库 `NVIDIA-RTX/NVRHI`（main 无 examples/），权威参考 `doc/Tutorial.md`+`ProgrammingGuide.md`（存 `thirdparty/nvrhi/doc/`），每处调用回 vendored `nvrhi.h`(2021 代) 核对签名。
- Slang 示例存 `thirdparty/slang-reference/`；NVRHI 完整示例存 `thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`；Falcor = Slang 生产级参考。
- Carrot：meshlet+meshoptimizer 是 2023-11-25 同一提交 `4c100761`（无截图 MD5 回归）；其 vendor+切簇塞一个提交的教训 → 我们拆版本铺路。
- S1 卡住翻：bevy `bevy_pbr/src/meshlet/from_mesh.rs`（本机 `E:\UE_STUDY_2026\bevy\`）、meshoptimizer `src/meshlet.cpp`、`docs/blogs/jms55-bevy-meshlet.md`。
