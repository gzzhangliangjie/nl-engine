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
- **每个里程碑必须有「交代」（2026-11-07 用户定）**：渲染版本 = `example/` 截图基线；工具版本 = **展示台网页 `kind:"tool"` 用例**（点运行实跑、输出展开；usage/正向/负向三条起步，`expectExit` 约定 0/1/2）+ `docs/s1/NANITE-TOOLS-GUIDE.md` 式归档。规范固化在 `docs/ROADMAP.md`「里程碑交代规范」节。注意：展示台 8787 端口有旧会话残留进程时新实例绑定静默失败，先 `netstat -ano | grep 8787` 清残留。
- **每轮迭代必须更新 `example/`**：基线 PNG + `run.bat` 进 `example/<版本>/`，登记 `example/cases.json`（唯一数据源，别建第二份清单）；验收/tag 前跑 `python tools/showcase/server.py --check` 退出 0 才放行（以 `renderer/src/renderer.cpp` 的 `kStage` 判定阶段）。
- **M0.9 起总闸：提交/tag 前 `python tools/regression.py` 全绿才放行**（全量历史用例 × 双后端比对基线）。
- 每个小里程碑本地提交一波；`docs/MILESTONES.md` 四栏（动作/提交版本/完成情况/踩坑）写实追加。
- 展示台 `tools/showcase/start.bat` → http://127.0.0.1:8787/；干净 clone 先 `python tools/showcase/rebuild.py`；每出新版把上一版 exe 映射改 `build/showcase/{tag}/` 并跑 rebuild.py 补建。
- 双后端画面一致分档：`bytes`（MD5）/ `near`（≤1/255 且 ≤2 px）/ FAIL，别笼统写「一致」。

## 本机环境（2026-11-06 实测）
- VS2022 + MSVC 14.38 + Windows SDK 10.0.26100.0；CMake 3.31.6 便携版在 `E:\UE_STUDY_2026\.build-tools\`。
- CMake 必须用 `NMake Makefiles`（VS 生成器被 HTTPS_PROXY/https_proxy 大小写撞键搞挂）。
- 网络：GitHub 小文件开代理直连，大文件 gh-proxy 分 8 段并发。

## 版本
- tag 序列：`s0-init`→M0.1、`s0-device`→M0.2、`s0-triangle`→M0.3、`s0-model`→M0.4、`s0-done`→M0.5、`m0.6` GPU 计时、`m0.7` 高模资产+`--model`、`m0.8` 线框/调试着色(f1ba95f)、`m0.9` 一键回归总闸（0.x 终点，58/58 ALL GREEN）。`v0.1.0`/`v0.4.1` 是快照 tag，与里程碑 tag 互不覆盖。
- **0.x 冻结在 M0.9，下一步 S1 切簇**。PBR/卡通是主线「穿插项」，tag 用名字（`pbr-direct`/`toon`）不占 m0.* 编号；详细计划在 `docs/s0/PBR-DIRECT-PLAN.md`、`docs/s0/TOON-PLAN.md`。PBR 直射光是 S7 前置。
- 穿插项归宿（防重复建设，总表在 ROADMAP「穿插项」节）：ShadowMap/PCF/PCSS=`shadow-map`、PRT/球谐=`prt-sh`、SSR=排 S6 后（复用 Hi-Z）、Kulla-Conty=并入 pbr-direct、SSAO=`ssao`、后处理链=`post-chain`（S7 后插）、Forward+/渲染图/热重载=归 P8。
- **Suzanne = 3,936 tri（11,808 索引）**，旧口径 504 tri 是错的。

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
