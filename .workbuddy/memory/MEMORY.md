# nl-engine 项目长期约定

## 架构（2026-11-06 定稿）
- 通用管线：NVRHI + Slang，构建链**不依赖任何厂商图形 SDK**。`CMakeLists.txt` 不得出现 `VULKAN_SDK` / `VK_SDK_PATH` / `find_package(Vulkan)` / 要求装 SDK。
- 后端对称：D3D12 / OpenGL 的头、库、调试层来自 Windows SDK；Vulkan 头走 `KhronosGroup/Vulkan-Headers`（NVRHI 的 `NVRHI_FETCH_VULKAN_HEADERS`），loader 是驱动自带 `vulkan-1.dll`；Slang 独立获取，不从 `$VULKAN_SDK` 找。
- `NL_GRAPHICS_BACKEND`（AUTO / DX12 / VULKAN）是唯一后端开关；Windows 上 `AUTO` 落 DX12（零额外依赖），Vulkan 作第二后端验证抽象。
- 图形 API 符号只允许出现在 `renderer/`；`core/` / `viewer/` 零命中。调试层判据按当前后端走（DX12 Debug Layer / VVL），不把 VVL 写成全项目前提。
- 一份 `.slang` 同时出 SPIR-V 与 DXIL、两后端各自加载成功 = 通用管线的验收核心，不是「顺带试一次」。

## 开发节奏
- 每阶段：目标 → **能亲眼看到的用例** → 验收判据 → 放行；验收不过不进下一步，tag 在验收之后打。
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
- `s0-init` = M0.1 验收；`v0.1.0` = M0.0 环境补齐快照。`s0-*` 是里程碑 tag，`v*` 是版本快照，互不覆盖。

## 参考文档/示例（M0.3 拉取，已 vendored 本地，2026-11-06）
- ⚠️ **被误导纠正**：NVRHI 官方仓库是 `NVIDIA-RTX/NVRHI`（不是 `NVIDIA/nvrhi`）；当前 `main` **不含 `examples/`**，权威 API 参考在仓库 `doc/Tutorial.md`+`doc/ProgrammingGuide.md`（已存 `thirdparty/nvrhi/doc/`）。
- 之前误判“当前 main 用新版 beginRenderPass、与 vendored 老接口不兼容”——实测 `Tutorial.md` 仍是 `open→setGraphicsState→draw`，与本地 vendored `nvrhi.h`(2021 代) 同代，可直接对照；差异只在校验细节，每处调用仍需回 vendored 头核对签名。
- Slang 官方仓库 `shader-slang/slang` 的 `examples/` 才是示例所在（已存 `thirdparty/slang-reference/`：`triangle`/`hello-world`(含 `vulkan-api`)/`example-base`）。示例用 Slang C++ API，我们用 `slangc` CLI，等价。
- NVRHI 完整可跑示例在 **Donut-Samples `vertex_buffer`**（已存 `thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`），基于 Donut 框架包在 NVRHI 之上，看完整管线结构。Donut 本身(`NVIDIA-RTX/Donut`) 含三后端 DeviceManager/swapchain/present 样板。
- Falcor(`NVIDIAGameWorks/Falcor`, master) = Slang 生产级框架，Slang 高级特性/材质系统参考。
- 这些 vendor 参考是**文档级，不入构建**；`thirdparty/slang/dist`(工具链) 仍 gitignore。
