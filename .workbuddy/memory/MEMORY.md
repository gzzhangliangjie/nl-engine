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
