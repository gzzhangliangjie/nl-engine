# nl-engine 项目长期约定

## 架构（2026-11-06 定稿）
- 通用管线：NVRHI + Slang，构建链**不依赖任何厂商图形 SDK**。`CMakeLists.txt` 不得出现 `VULKAN_SDK` / `VK_SDK_PATH` / `find_package(Vulkan)` / 要求装 SDK。
- 后端对称：D3D12 / OpenGL 的头、库、调试层来自 Windows SDK；Vulkan 头走 `KhronosGroup/Vulkan-Headers`（NVRHI 的 `NVRHI_FETCH_VULKAN_HEADERS`），loader 是驱动自带 `vulkan-1.dll`；Slang 独立获取，不从 `$VULKAN_SDK` 找。
- `NL_GRAPHICS_BACKEND`（AUTO / DX12 / VULKAN）是唯一后端开关；Windows 上 `AUTO` 落 DX12（零额外依赖），Vulkan 作第二后端验证抽象。
- 图形 API / NVRHI / ImGui 符号只允许出现在 `renderer/`；`core/` / `viewer/` 零命中。调试层判据按当前后端走（DX12 Debug Layer + NVRHI validation / Vulkan NVRHI validation）；VVL 是可选插件、本机未装，不写成全项目前提。
- 一份 `.slang` 同时出 SPIR-V 与 DXIL、两后端各自加载成功 = 通用管线的验收核心，不是「顺带试一次」。

## 开发节奏
- 每阶段：目标 → **能亲眼看到的用例** → 验收判据 → 放行；验收不过不进下一步，tag 在验收之后打。
- **每轮迭代必须更新 `example/`（2026-11-07 用户定的硬规则）**：成功用例的基线 PNG + `run.bat` 放进 `example/<版本>/`，并登记到 `example/cases.json`；验收 / 打 tag 前跑 `python tools/showcase/server.py --check`，退出码 0 才放行。该检查以 `renderer/src/renderer.cpp` 的 `kStage` 判定当前阶段，代码进了新阶段而用例没归位就退出 1，展示台侧栏也显示状态。S0 的 M0.x 与 S1~S14 每一步同样适用，不许「只改代码不归位用例」。
- 每个小里程碑本地提交一波；`docs/MILESTONES.md` 四栏（动作 / 提交版本 / 完成情况 / 踩坑）写实追加，不写「已完成」「无」。
- Carrot 的**节奏**参考 `docs/s0/CARROT-BOOTSTRAP.md`，其弯路（换底座、晚拆模块、相机硬搓、resize 修 4 次）不复刻。
- 门禁表与依赖实况见 `docs/s0/S0-PLAN.md`；总路线 `docs/ROADMAP.md`；台账 `docs/MILESTONES.md`。

## 本机环境（2026-11-06 实测）
- VS2022 Community + MSVC 14.38.33130 + Windows SDK 10.0.26100.0；CMake 3.31.6 便携版在 `E:\UE_STUDY_2026\.build-tools\`（仓库外，不污染 PATH）。
- `vulkan-1.dll` / `vulkaninfo.exe` 驱动自带；**未装也无需装 Vulkan SDK**。
- CMake 必须用 `NMake Makefiles` 生成器：VS 生成器会被 `HTTPS_PROXY` / `https_proxy` 大小写撞键搞挂（构建工具的大小写不敏感 Hashtable）。
- GLFW 从本地 `Carrot/thirdparty/glfw` vendored（105 个构建必需文件，零网络）。
- 网络：GitHub 小文件开代理后直连可用，大文件限速 ~12KB/s → gh-proxy 分 8 段并发。

## 版本
- `s0-init` = M0.1；`s0-device` = M0.2；`s0-triangle` = M0.3；`s0-model` = M0.4 验收（commit `2e897a6`）；**`s0-done` = M0.5 + S0 总验收，S0 全部完成，下一步 S1**。`v0.1.0` = M0.0 环境补齐快照，`v0.4.1` = 展示台快照。`s0-*` 是里程碑 tag，`v*` 是版本快照，互不覆盖。

## M0.5 起的 HUD / 模式约定（2026-11-07 实测，改前看 `docs/IMGUI-GUIDE.md`）
- ImGui 用 vendored **v1.91.9 稳定版**（不要拿本机 Carrot 的 `1.92.7 WIP`，字体系统重构过）；**不用官方 D3D12/Vulkan backend**，统一走自写 `renderer/src/hud.cpp`（`ImDrawData` 摊平成 HudVertex 32 B + R32_UINT + 逐 cmd scissor）。
- `hud.slang` 与 `model.slang` 都单源双目标；`--selftest` 机械核对 8 个 `.spv`/`.dxil` 产物魔数（14/14）。`hudProjection()` 的 **Y 尺度必须为负**（ImGui y 向下、NDC y 向上），否则文字镜像。
- 动态 VB/IB 的 `BufferDesc` 必须 `initialState = CopyDest` + `keepInitialState = true`，否则 NVRHI validation 报 `Unknown prior state` → DX12 Device Removed。
- `HudFrame` 是唯一公开双向 POD：viewer 填显示数据、HUD 写回控件值；`renderFrame(..., hud=nullptr)` 时完全不碰 ImGui → `--screenshot` 默认 `--no-hud`，M0.1~M0.4 基线逐字节不动。
- 截图确定性：回归模式用 8 个固定帧时间样本 + 面板标注 `capture/replay`；交互窗口才显示真实 FPS。`--hud` 两后端 PNG 必然不同（面板文本不同），别误判成 3D 画面不一致。
- 默认字体图集只有 ASCII：HUD 里所有字符串（含后端调试层说明）必须 ASCII，中文会显示 `?????`；要中文 / InputText（键盘字符回调）/ 用户纹理（`TextureId` 当前忽略）/ docking 都是后续增量。
- **下拉框 / 选择菜单现在就能用**：`Combo` / `BeginCombo` / `ListBox` / `Selectable` / `RadioButton` / `TreeNode` 在已 vendored 的 `imgui_widgets.cpp` 里，鼠标输入已够；弹出层的多 `ImDrawList` / ClipRect 已被 `build()` / `draw()` 覆盖。仅 `InputText` 需补字符回调。
- `--mode` 分发器在 `core/modes.*` + `viewer` 的 `runMode()`：M0.5 只有 `normal`；S1 起每加一个模式只改枚举 / 名字 / case，旧模式保留当对照。未知模式退出 1。

## 调试与性能工具链（2026-11-07 定，S1~S14 沿用）
- **四层**：L0 画面证据（`--screenshot` + `example/` + 展示台）→ L1 **RenderDoc** 帧结构（外部 Launch Application 零代码改动；精确抓第 N 帧才加 `--capture-frame N` + 运行时 `LoadLibraryW(L"renderdoc.dll")`）→ L2 **NVRHI timestamp query** 真实 GPU 帧内计时（S4 前补，填进 `HudFrame`）→ L3 Nsight / PIX / PresentMon / Tracy 时间线。
- 选 RenderDoc 当主工具是因为**同时覆盖 DX12 + Vulkan**；Nsight 偏 N 卡、PIX 只管 D3D12，退到 L3。工具全在进程外或 NVRHI 抽象层内，不进 CMake、不链接、不读 SDK 路径。
- **当前 HUD 的 `frame ms` 是 CPU 帧间隔，不是 GPU 耗时**，不能拿它验收 60fps / 性能退化。
- 本机（2026-11-07）RenderDoc / Nsight / PIX / `renderdoc.dll` **均未安装**；S0 验收不依赖抓帧，台账如实记 ⬜，不虚构结果。详见 `docs/s0/S0-PLAN.md`。

## M0.4 起的渲染约定（2026-11-07 实测，改动前先看 `docs/SHADER-CONVENTIONS.md`）
- 资源与绘制逻辑收在 `renderer/src/scene.cpp`（两后端共享，只依赖 `nvrhi.h`）；两个 backend 只管 swapchain / depth / framebuffer / present，**不要再在两个后端里各抄一遍 shader 或资源创建**。
- **SPIR-V 绑定偏移固定**：NVRHI 默认 `VulkanBindingOffsets` = SRV 0 / Sampler 128 / CB 256；slangc 必须带 `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`。shader 只写 `register(b0/t0/s0)`，禁用 `[[vk::binding]]` 之类 GLSL 注解。
- SPIR-V 入口恒为 `main`、DXIL 才是 `vsMain/psMain`（M0.3 老坑，Model shader 同样适用）。
- MVP 用 NVRHI **volatile constant buffer**（`setMaxVersions(16)`），不是 push constant；`SceneConstants` 严格 160 B，C++ 的 `FrameParams` 尾部有 `bool cullBack`，**禁止整块 memcpy 进 cbuffer**。
- 剔除方向：glTF CCW 外表面在本项目两后端都被判为 back-face（叠加 Slang + NVRHI Vulkan 视口换算），pipeline 必须 `setFrontCounterClockwise(true)`；默认 false 会剔掉正脸只剩内壁。「关剔除看内壁」用例 = `--dist 0.6 --cull none`（相机进模型内部），外部视角看不出差异。
- 资产 `assets/models/Suzanne.glb` 是 Khronos 官方 glTF 三件套经 `scripts/pack-suzanne-glb.py` 打包的单文件（2.5 MB，内嵌贴图）；路径由 `NL_ASSET_DIR` 注入，不依赖 CWD。tinygltf 只在 `scene.cpp` 一个 TU 展开实现，`TINYGLTF_NO_STB_IMAGE_WRITE`。
- 每阶段新增 GPU 资源时，**slot 级同步表（如 `frameFence_`）必须在 `createTargets()` 初始化**：`renderFrame` 在懒建 `ensurePipeline()` 之前就会按 slot 访问它，放错位置 = 首帧 139 段错误。

## 成果展示台 / 用例库（2026-11-07 新增，`v0.4.1`）
- **`example/cases.json` 是版本与用例的唯一数据源**：截图、`run.bat`、exe 映射、参数范围都从它读；新增用例只改它 + 把 PNG 放进 `example/<版本>/`，**不要另建第二份清单**。
- 启动：`tools/showcase/start.bat`（或 `python tools/showcase/server.py --port 8787`）→ http://127.0.0.1:8787/；服务是 Python 标准库、零依赖，只从清单白名单读图、参数按 spec 校验、运行加串行锁（viewer 会开真实 GPU 窗口）。
- 干净 clone 后 `build/` 不存在 → 先 `python tools/showcase/rebuild.py`：按 tag `git archive` 出历史快照，注入阶段无关 `build-m0.2.bat`（s0-init 自带脚本要 `reg.exe`，本机被禁），junction 复用 `thirdparty/slang/dist`，产物在 `build/showcase/<tag>/build/<BACKEND>/viewer.exe`。M0.4 直接用当前 `build/<backend>/viewer.exe`。
- **双后端「画面一致」要分档**：正常视角 PNG 逐字节相同；M0.4 内壁 / resize 的 MD5 不同但像素差仅 ≤1/255（2 个 / 1 个像素，GPU 取整噪声）。展示台的 `compare_pixels()` 如实标 `bytes` / `near` / 不同，别笼统写「一致」。
- 历史遗留：`docs/screenshots/` 是验收存档（含 M0.2 不同参数的旧图），`example/` 里的基线必须与用例命令的产物一致；M0.2 resize 基线已用现场产物替换。

## 参考文档/示例（M0.3 拉取，已 vendored 本地，2026-11-06）
- ⚠️ **被误导纠正**：NVRHI 官方仓库是 `NVIDIA-RTX/NVRHI`（不是 `NVIDIA/nvrhi`）；当前 `main` **不含 `examples/`**，权威 API 参考在仓库 `doc/Tutorial.md`+`doc/ProgrammingGuide.md`（已存 `thirdparty/nvrhi/doc/`）。
- 之前误判“当前 main 用新版 beginRenderPass、与 vendored 老接口不兼容”——实测 `Tutorial.md` 仍是 `open→setGraphicsState→draw`，与本地 vendored `nvrhi.h`(2021 代) 同代，可直接对照；差异只在校验细节，每处调用仍需回 vendored 头核对签名。
- Slang 官方仓库 `shader-slang/slang` 的 `examples/` 才是示例所在（已存 `thirdparty/slang-reference/`：`triangle`/`hello-world`(含 `vulkan-api`)/`example-base`）。示例用 Slang C++ API，我们用 `slangc` CLI，等价。
- NVRHI 完整可跑示例在 **Donut-Samples `vertex_buffer`**（已存 `thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`），基于 Donut 框架包在 NVRHI 之上，看完整管线结构。Donut 本身(`NVIDIA-RTX/Donut`) 含三后端 DeviceManager/swapchain/present 样板。
- Falcor(`NVIDIAGameWorks/Falcor`, master) = Slang 生产级框架，Slang 高级特性/材质系统参考。
- 这些 vendor 参考是**文档级，不入构建**；`thirdparty/slang/dist`(工具链) 仍 gitignore。
