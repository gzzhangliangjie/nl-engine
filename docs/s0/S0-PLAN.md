# S0 详细开发路线图：从空目录到「画出一个能转的 Suzanne」

> 对应总路线 `docs/ROADMAP.md` 的 S0。共 6 个阶段（M0.0~M0.5）、业余时间 1~2 周。
> 每个里程碑 = 一段可运行代码 + 一次本地提交 + 一个 git tag + `docs/MILESTONES.md` 里一条写实记录。**跑不通当下一步，不攒债。**
> 搭建节奏参考 `docs/s0/CARROT-BOOTSTRAP.md`（Carrot 2020-11 从零起步的实况，不是只看成品代码风格）。**仅作参考、不做逐行对齐**：他早期换底座、晚拆模块、相机硬搓、resize 修 4 次都是弯路，文档第三节列了不复刻清单。

## 阶段门禁：目标 → 可见用例 → 验收 → 放行

**每阶段必须有能亲眼看到、能截图存档的用例；验收不过不进下一阶段，tag 在验收通过之后才打。**

| 阶段 | 目标 | 可见用例（必须能截图 / 能当场演示） | 验收判据 | 放行 |
|---|---|---|---|---|
| M0.0 | 工具链齐备 | 同屏截图：`cmake --version`、`vkcube` 旋转方块、`slangc -h` 输出 | 三项都有正常输出；`VULKAN_SDK` 已设置 | → M0.1 |
| M0.1 | 最小可跑骨架 | `viewer.exe` 弹出窗口，标题栏实时显示帧号/帧时间，按 ESC 干净退出，控制台打印 shutdown ok。截图 `docs/screenshots/m0.1.png` | `viewer.exe --selftest` 输出 PASS；`grep -R "\bvk[A-Z]" viewer/ core/` 零命中 | → M0.2 |
| M0.2 | NVRHI device + swapchain | 窗口清屏色按空格切换；拖动缩放窗口前后都不黑屏。截图 `m0.2.png`（含缩放前后对比） | 验证层全程零输出 | → M0.3 |
| M0.3 | 三角形 + frame in flight = 2 | 彩色三角形；任务管理器 GPU 占用非 0。截图 `m0.3.png` | 验证层零输出；**故意删一个 fence 等待能触发验证层报错**（证明安全网有效） | → M0.4 |
| M0.4 | 模型 + 贴图 + 轨道相机 | 带贴图 Suzanne；拖动旋转、滚轮缩放；关背面剔除能看到内壁。截图 `m0.4.png` | 验证层零输出 + resize 回归通过 | → M0.5 |
| M0.5 | ImGui HUD + 总验收 | HUD 显示 FPS / 帧时间曲线 / 相机参数；Nsight Graphics 抓一帧看到 draw call；截图 `docs/screenshots/s0.png` | S0 三条硬指标全过（画面可交互 / 验证层零报错 / 一份 .slang 出 SPIR-V） | 打 `s0-done`，进 S1 |

> **resize 是每一阶段的回归项，不是 M0.2 的一次性检查**：Carrot 实况里 swapchain recreation 前后修了 4 次（`146517a7` → `6a30f711` → `bb981ccb` → `2c87efe2`），那是没做回归的代价——我们做成回归项，预期一次修好，不预期修 4 次。

---

## M0.0 环境准备（半天）

| 依赖 | 版本建议 | 获取 | 验证命令 |
|---|---|---|---|
| Visual Studio 2022 | Community，勾「使用 C++ 的桌面开发」 | 官网 | `cl` 能在开发者命令行找到 |
| CMake | ≥ 3.24 | **已就位**：便携版 3.31.6 在 `E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe`（GitHub Releases 经 gh-proxy 分 8 段并发下载，2m35s；单连接只有 ~12KB/s） | `cmake --version` |
| Vulkan SDK | 最新（LunarG） | https://www.lunarg.com/vulkan-sdk/ （直链，不走 GitHub）；**只装必要组件即可，见下节** | `vkcube` 能跑；`VULKAN_SDK` 已设置 |
| Slang | **随 Vulkan SDK 附带** | 新版 LunarG SDK 自带 `slangc`（Carrot `BUILDING.md` 用 1.4.341.1，`cmake_modules/ShaderSlangConfig.cmake` 从 `$VULKAN_SDK/include/slang` + `lib/slang` 找）；也可单独从 `https://github.com/shader-slang/slang/releases` 下 | `slangc -h` 有输出 |
| NVRHI | 最新 master | `git clone --recursive`（走 gh-proxy 镜像）或 CMake FetchContent | `cmake` 能构建出静态库 |
| GLFW | 3.3+ | **零网络**：从本地 `E:\UE_STUDY_2026\Carrot\thirdparty\glfw\` 复制到 `thirdparty/`（Carrot Init 就是这么 vendored 的） | `add_subdirectory` 能过 |
| 测试模型 | Suzanne 带贴图 glb | Blender 导出，或随便一个 glTF 样例 | — |

环境自检清单（M0.0 的可见用例：三项输出同屏截图）：
- [x] `cmake --version` 正常 → 3.31.6（便携版）
- [ ] `vkcube` 弹出旋转方块（说明驱动+SDK 正常）
- [ ] `slangc -h` 正常（确认 SDK 自带 Slang）
- [ ] `VULKAN_SDK` 已设置
- [ ] Nsight Graphics 已装（验收日要用）

### 为什么需要 Vulkan SDK（不是「装了才能跑 Vulkan」）

本机实测：`C:\Windows\System32\vulkan-1.dll`（1.5MB）与 `vulkaninfo.exe` 已由显卡驱动装好，**跑 Vulkan 程序根本不需要 SDK**。SDK 给的是开发工具，逐项拆开：

| 需要什么 | 干什么用 | 只有 SDK 能给吗 |
|---|---|---|
| `vulkan/vulkan.h` 等头文件 | 编译 NVRHI 的 Vulkan 后端 | 否，`KhronosGroup/Vulkan-Headers` 单独拉，几 MB |
| `vulkan-1.dll` loader | 运行时加载 ICD | 否，驱动已装 |
| `VkLayer_khronos_validation.dll` + JSON manifest | **S0 硬指标「验证层零报错」**；M0.3「故意删一个 fence 等待能触发报错」的安全网 | **是**（或拉 VVL release 自己配 `VK_LAYER_PATH`，版本配对易踩坑） |
| `slangc` / `glslangValidator` | Slang → SPIR-V | 否，Slang release 单独可下 |
| `vkcube` / `vkconfig` / Nsight | 自检与抓帧道具 | 否，可替代 |

**结论：真正绕不过去的只有 validation layers。** 装 SDK 不是技术必需，是「一次装齐、版本自洽、不用手配 `VK_LAYER_PATH`」的省事选择；安装时只勾 Headers + Validation Layers 组件即可，文档 / 示例 / Nsight 都能不装。
完全不装的替代路线：改走 NVRHI 的 D3D12 后端，验证层用 Windows SDK 自带的 D3D12 Debug Layer（本机已装），零额外安装；但那样 M0.3 的 SPIR-V 验收得改成 DXIL，与 S0 三条硬指标冲突，**不作为主线**。

> 本机实测（2026-11-06 15:40 更新）：
> - ✅ VS2022 Community + `cl.exe` 14.38.33130 + Windows SDK 10.0.26100.0
> - ✅ CMake 3.31.6 便携版；标准构建链 `configure → build → SELFTEST PASS (7/7)` 全通过（`scripts/build-m0.1.bat`，NMake 生成器）。M0.1 另有零 CMake 兜底 `scripts/verify-m0.1-msvc.ps1`。
> - ✅ Vulkan loader / `vulkaninfo` 驱动自带
> - ⬜ Vulkan SDK（只为 validation layers）、Nsight Graphics、vcpkg（未必要）
> - 网络：`raw.githubusercontent.com` 默认超时、`gh-proxy` 稳定可用；开代理后小文件直连 0.94s，大文件仍被限速（单连接 ~12KB/s），改分段并发解决；LunarG SDK 直链 HTTP 200。
> - GLFW 已从本地 `Carrot/thirdparty/glfw` 复制进 `thirdparty/`（105 个构建必需文件、零网络）。

## M0.1 项目骨架（0.5~1 天）

**做什么**：目录 + CMake 三 target + core 日志 + 能弹出的空窗口。

```
nl-engine/
├── CMakeLists.txt
├── core/                 core.hpp / log.hpp / assert.hpp / window.hpp+cpp / input.hpp
├── renderer/             renderer.hpp+cpp（S0 只到 swapchain 初始化）
├── viewer/               main.cpp
└── nanite-tools/         （空，S1 填）
```

要点：
- CMake 三 target：`core`（静态库）、`renderer`（静态库，链接 Vulkan::Vulkan）、`viewer`（exe，链 core+renderer）。`nanite-tools` 先只建 target 不写码
- 窗口用 GLFW（vcpkg 或 FetchContent 均可，别手搓 Win32）
- 日志：spdlog，或者先 `printf` + 宏占位——**别在日志上花超过 1 小时**
- **日志/错误回调框架第一天就留好**（Carrot `46799759` 的 `debugCallback` 与 validation 检查是 Init 的一部分，验证层是验收手段不是后期加固）；M0.2 把 NVRHI device callback 直接接到这个口
- 产出代码风格从第一天定死：`viewer/` 和 `core/` 里 **不出现任何 `vk` 开头的符号**，这是渲染器模块纪律的机械保证
- 三 target 保持薄：Carrot 单 `Engine` 类跑了 4 个月才拆模块（`e6bc5196` 起，单次 900+ 行搬家式重构）是他的弯路，我们提前分层正是避开它；但**不能在 M0.1 就把 4 个月后的壳搬进来**

**可见用例**：`viewer.exe` 弹出窗口，标题栏实时显示帧号/帧时间，按 ESC 干净退出；`viewer.exe --selftest` 无窗口跑自检输出 PASS。
**自检**：窗口出现、`--selftest` PASS、`grep -R "\bvk[A-Z]" viewer/ core/` 零命中；截图 `docs/screenshots/m0.1.png`。验收通过才打 tag：`s0-init`。

**M0.1 验收记录（2026-11-06，本机实跑）**：
- MSVC 14.38 直编 GLFW + 三个 target：编译 / 链接退出码 0
- `viewer.exe --selftest` → `SELFTEST PASS (7/7)`（日志格式化与 sink、干净运行零错误、设备错误独立计数、断言可接管、窗口默认可缩放、renderer 占位不越界、自检收尾干净）
- `viewer.exe` 主循环 → `[nl:info] window created 1280x720 | no GL context (Vulkan/NVRHI ready) | ESC to quit`，标题栏帧计数器持续更新
- `grep` 检查 `viewer/`、`core/` 下 `vk[A-Z]` / `Vk[A-Z]` / `nvrhi` → 零命中
- 复现：`powershell -ExecutionPolicy Bypass -File scripts\verify-m0.1-msvc.ps1`（零 CMake 依赖）；标准构建见 `scripts/build-m0.1.bat`
- 未自动完成项：验收截图 `docs/screenshots/m0.1.png` 需在本机运行 viewer 后手动保存（沙箱内 GUI 不便截屏）

## M0.2 NVRHI 接入：设备 + swapchain（1~2 天）

**做什么**：拿到 NVRHI → 创建 device（feature 一次验全）→ swapchain → 接入主循环。

- [ ] 获取 NVRHI：`git clone --recursive`（本机网络走 gh-proxy 镜像前缀）或 CMake FetchContent
- [ ] `IDeviceCreationParams`：debug 模式开 **NVRHI 验证层**，device callback 错误信息接到日志
- [ ] feature 检查写死在设备创建里：**rayQuery、shaderBufferInt64Atomics、indirectDraw**（S7/S8 要用，现在验掉，免得写完了发现不支持）
- [ ] graphics queue + `ISwapChain`（sRGB 格式起步），resize 处理留好接口（窗口缩放必测）
- [ ] 底层后端起步选 Vulkan（Windows/Linux/MoltenVK 同一套代码；发布期想切 DX12 改一个枚举——这就是跨平台底座的意义）

**自检**：resize 窗口不崩不黑屏；NVRHI 验证层全程零输出。tag：`s0-device`。

⚠️ 常见坑：
- swapchain resize 处理不干净是本里程碑唯一高发 bug，且**每个后续阶段都要回归**（Carrot 实况修了 4 次）
- feature 漏开在创建 device 时才炸——从第一天就把 S7/S8 要的能力写进检查列表，别以后返工

## M0.3 第一个三角形 + 帧同步（2~3 天，本里程碑的灵魂）

**做什么**：dynamic rendering → 图形 pipeline → 顶点/片元 shader（Slang 编译）→ fence/semaphore 同步 → 每帧 submit。

- [ ] 写 `triangle.slang`（vertex + fragment 两个 entry），CMake `add_custom_command` 调 `slangc` 编译（SPIR-V 为主，DXIL 顺手验一个），target 依赖挂上（改 shader 自动重编）
- [ ] 渲染循环：NVRHI `ICommandList` 记录 → `executeCommandList` → fence 同步，**frame in flight = 2**；自动 barrier 跟踪保持开启
- [ ] render target 直接用 NVRHI begin/end render pass（无 Vulkan 式 RenderPass/Framebuffer 样板）
- [ ] 画一个硬编码三角形

**自检**：三角形出现；任务管理器看 GPU 占用非 0（确认真在 GPU 上画）；拖动窗口/resize 正常；NVRHI 验证层零输出；**故意删掉一个 fence 等待看验证层报错**（验证安全网真的在工作）。tag：`s0-triangle`。

⚠️ 常见坑：
- 同步写错的表现是「偶发黑屏/设备丢失」而不是每次都崩——M0.3 写完当场把边界情况测掉，S4 之后再来查就是灾难
- NVRHI 自动 barrier 大幅降低出错面，但**帧间同步（frame in flight）仍要自己管**——fence 语义别想当然

## M0.4 glTF 模型 + 纹理 + 相机（2 天）

**做什么**：加载 Suzanne.glb → 顶点/索引 buffer 上传 → 贴图（staging + image barrier）→ push constant MVP → 轨道相机。

- [ ] glTF 库：`fastgltf`（现代、和 vulkan-tutorial 后期同款）或 `tinygltf`（更简单），选一个别纠结
- [ ] buffer 上传走 staging：createBuffer(map/copy) 封装成 renderer 里第一个真正可复用的函数
- [ ] 纹理：staging 上传 → NVRHI 纹理 + sampler；看懂自动 barrier 替你做了哪些转换（对照 Donut 源码——barrier 原理不懂，S6 Hi-Z 会还债）
- [ ] push constant 传 MVP，轨道相机（拖动旋转/滚轮缩放），core 里写 `Camera` 类
- [ ] 顺带把 depth buffer 挂上（模型有立体感，也为 S6 Hi-Z 铺路）

**自检**：Suzanne 带贴图正确显示，相机拖动丝滑；关闭背面剔除看到内壁，再打开验证 culling 生效。tag：`s0-model`。

## M0.5 ImGui + 收尾验收（1 天）

- [ ] ImGui（docking 分支）接进渲染循环：一个 HUD 显示 FPS、帧时间、相机参数
- [ ] `--mode normal` 参数框架搭好（S1 起每个里程碑加一个 mode，模式分发器今天定型）
- [ ] README 更新构建说明（`cmake -B build && cmake --build build`）
- [ ] **验收对照 docs/ROADMAP.md 的 S0 三条**：
  - [ ] 画面正确、可交互
  - [ ] NVRHI 验证层零报错
  - [ ] 一份 .slang 编译出 SPIR-V（顺带 `--target spirv` 输出 DXIL 试一次，留个开关）

**收尾**：tag：`s0-done`；用 Nsight Graphics 抓一帧（认识工具：能看到 draw call、状态、耗时，S4 以后全靠它）；存验收截图进 `docs/screenshots/s0.png`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M0.0 环境 | 0.5 天 | — |
| M0.1 骨架+空窗口 | 1 天 | `s0-init` |
| M0.2 NVRHI 设备+swapchain | 1~2 天 | `s0-device` |
| M0.3 三角形+同步 | 2~3 天 | `s0-triangle` |
| M0.4 模型+纹理+相机 | 2 天 | `s0-model` |
| M0.5 ImGui+验收 | 1 天 | `s0-done` |

节奏提醒：M0.2 是纯样板代码，看着长其实机械，照 vulkan-tutorial 走别发挥；**M0.3 是 S0 唯一需要动脑的地方**（同步模型），多留时间。

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| **这一步该按什么顺序搭** | ⭐ `docs/s0/CARROT-BOOTSTRAP.md`：Carrot 从 `Init`（`46799759`）到三角形（`152edabe`）、staging 抽象（`d11508e3`）、模型（`d25a1cc9`）的实况流程，带可 `git show` 的 hash |
| NVRHI 任何一步 | NVRHI / Donut 官方示例（外部代码走 gh-proxy 拉来研究）；vulkan-tutorial.com 当底层原理对照 |
| 工程组织怎么长出来 | Carrot `e6bc5196` → `4d06c0d3` → `8c227e91`：单 Engine 跑通 4 个月后才拆模块，看它**什么时候**拆比看最终目录有用 |
| 同步疑问 | vkguide.dev 的 synchronization 章节；《Vulkan Synchronization Validation》官方页 |
| slangc 参数 | `slangc -h`；https://shader-slang.org 的用户指南 |
| 下载挂了 | GitHub release 用 gh-proxy 镜像前缀；Vulkan SDK 走 LunarG 直链没问题 |

## 底线

- 任何时刻验证层（NVRHI 验证层 / VVL）有输出 = 有 bug，零容忍
- NVRHI 符号出现在 core/viewer = 结构错误，当场挪进 renderer/
- M0.3 的同步想不清楚就画时序图（CPU timeline vs GPU timeline 两行，fence/semaphore 标在交点上），画完自然明白
