# S0 详细开发路线图：从空目录到「画出一个能转的 Suzanne」

> 对应总路线 `docs/ROADMAP.md` 的 S0。共 6 个阶段（M0.0~M0.5）、业余时间 1~2 周。
> 每个里程碑 = 一段可运行代码 + 一次本地提交 + 一个 git tag + `docs/MILESTONES.md` 里一条写实记录。**跑不通当下一步，不攒债。**
> 搭建节奏参考 `docs/s0/CARROT-BOOTSTRAP.md`（Carrot 2020-11 从零起步的实况，不是只看成品代码风格）。**仅作参考、不做逐行对齐**：他早期换底座、晚拆模块、相机硬搓、resize 修 4 次都是弯路，文档第三节列了不复刻清单。
> **架构纪律：这是通用管线，构建链不依赖任何厂商图形 SDK。**后端的头文件、运行时、调试层按同一套规则各自本地化，见 M0.0「后端对称」表。

## 阶段门禁：目标 → 可见用例 → 验收 → 放行

**每阶段必须有能亲眼看到、能截图存档的用例；验收不过不进下一阶段，tag 在验收通过之后才打。**

| 阶段 | 目标 | 可见用例（必须能截图 / 能当场演示） | 验收判据 | 放行 |
|---|---|---|---|---|
| M0.0 | 工具链齐备 + 后端开关可用 | 同屏截图：`cmake --version`、两次 configure 的 `backend:` 行、`viewer.exe --selftest` 分别输出 `backend: dx12` 与 `backend: vulkan` | 两种 `-DNL_GRAPHICS_BACKEND` 配置都不装任何图形 SDK 就编译通过且 PASS，输出的后端名不同 | → M0.1 |
| M0.1 | 最小可跑骨架 | `viewer.exe` 弹出窗口，标题栏实时显示帧号/帧时间，按 ESC 干净退出，控制台打印 shutdown ok。截图 `docs/screenshots/m0.1.png` | `viewer.exe --selftest` 输出 PASS；`grep -R "\bvk[A-Z]\|D3D12\|ID3D12" viewer/ core/` 零命中 | → M0.2 |
| M0.2 ✅ | NVRHI device + swapchain，两后端都能起 | 窗口清屏色按空格切换；切换后端后**同一份 viewer 代码出同一个画面**；拖动缩放窗口前后都不黑屏。截图 `m0.2.png`（含两后端 + 缩放前后对比） | 当前后端调试层零输出；DX12 / Vulkan 都跑通 | ✅ 2026-11-06（→ M0.3） |
| M0.3 | 三角形 + frame in flight = 2 | 彩色三角形；任务管理器 GPU 占用非 0；两个后端各跑一次。截图 `m0.3.png` | 当前后端调试层零输出；**故意删一个 fence 等待能触发同步校验报错**（证明安全网有效） | → M0.4 |
| M0.4 | 模型 + 贴图 + 轨道相机 | 带贴图 Suzanne；拖动旋转、滚轮缩放；关背面剔除能看到内壁。截图 `m0.4.png` | 调试层零输出 + resize 回归通过 + 两后端画面一致 | → M0.5 |
| M0.5 | ImGui HUD + 总验收 | HUD 显示 FPS / 帧时间曲线 / 相机参数 / 当前后端名；Nsight Graphics 抓一帧看到 draw call；截图 `docs/screenshots/s0.png` | S0 三条硬指标全过（画面可交互 / 两后端调试层零报错 / **一份 .slang 同时出 SPIR-V 与 DXIL 且两后端都能加载**） | 打 `s0-done`，进 S1 |

> **resize 是每一阶段的回归项，不是 M0.2 的一次性检查**：Carrot 实况里 swapchain recreation 前后修了 4 次（`146517a7` → `6a30f711` → `bb981ccb` → `2c87efe2`），那是没做回归的代价——我们做成回归项，预期一次修好，不预期修 4 次。

---

## M0.0 环境准备（半天）

| 依赖 | 版本建议 | 获取 | 验证命令 |
|---|---|---|---|
| Visual Studio 2022 | Community，勾「使用 C++ 的桌面开发」 | 官网（自带 Windows SDK） | `cl` 能在开发者命令行找到 |
| CMake | ≥ 3.24 | **已就位**：便携版 3.31.6 在 `E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe`（GitHub Releases 经 gh-proxy 分 8 段并发下载，2m35s；单连接只有 ~12KB/s） | `cmake --version` |
| Slang | 独立 release | `https://github.com/shader-slang/slang/releases` 取预编译 `slangc`，或 FetchContent 编源码。**独立获取，不从任何 SDK 路径找** | `slangc -h` 有输出 |
| NVRHI | 最新 master | `git clone --recursive`（gh-proxy 镜像）或 CMake FetchContent；Vulkan 头由 `NVRHI_FETCH_VULKAN_HEADERS` 拉 `KhronosGroup/Vulkan-Headers` | `cmake` 能构建出静态库 |
| GLFW | 3.3+ | **零网络**：从本地 `E:\UE_STUDY_2026\Carrot\thirdparty\glfw\` 复制到 `thirdparty/`（Carrot Init 就是这么 vendored 的） | `add_subdirectory` 能过 |
| 测试模型 | Suzanne 带贴图 glb | Blender 导出，或随便一个 glTF 样例 | — |

环境自检清单（M0.0 的可见用例）：
- [x] `cmake --version` 正常 → 3.31.6（便携版）
- [x] `-DNL_GRAPHICS_BACKEND=DX12` 与 `=VULKAN` 两种配置各编一遍，`--selftest` 都 PASS 且 `backend:` 行不同——**全程零厂商 SDK**
- [ ] `slangc -h` 正常（M0.3 前拿到即可）
- [ ] Nsight Graphics 已装（验收日要用，可选）

### 后端对称：谁都不装 SDK（2026-11-06 架构校准）

通用管线下，给某一个后端单开一列「装 SDK」是自相矛盾的：DX12 和 OpenGL 不用装，Vulkan 也不该装。三者按同一套规则各自本地化即可。

| 后端 | 头文件 / 链接库 | 运行时 | 调试层 | 需要额外装什么 |
|---|---|---|---|---|
| **D3D12**（Windows 默认） | `d3d12.h` / `d3d12.lib` / `dxgi` —— **Windows SDK 已装** | 系统自带 | **Windows SDK 自带 D3D12 Debug Layer / GBV** | 无 |
| **Vulkan** | `KhronosGroup/Vulkan-Headers`（几 MB，M0.2 由 NVRHI 的 `NVRHI_FETCH_VULKAN_HEADERS` 拉；不读 `$VULKAN_SDK`） | 显卡驱动自带 `vulkan-1.dll`（本机已装） | VVL release 单独拿（可选插件，不装也能跑，只是少一层校验） | 无 |
| **OpenGL**（如需对照） | `GL/gl.h` + `opengl32.lib` —— **Windows SDK 已装** | 显卡驱动 | — | 无 |

**写死的规则**：
- 构建脚本里出现 `VULKAN_SDK` / `VK_SDK_PATH` / `find_package(Vulkan)` / 要求装厂商 SDK = 架构错误，当场删掉。
- Slang 独立于后端：`slangc` 自己拿，不从 `$VULKAN_SDK/include/slang` 找（那是裸 Vulkan 项目 Carrot 的做法，不能继承）。
- 默认后端 **DX12**（Windows 上零额外依赖），Vulkan 作为第二后端验证抽象；Linux 下 `AUTO` 落到 Vulkan。
- 调试层判据按**当前启用后端**走：「零报错」= DX12 Debug Layer 或 VVL 任一有输出都算 bug；不把 VVL 写成全项目前提。
- 一份 `.slang` 编出 SPIR-V 和 DXIL 两个目标、两个后端各自加载——**这才是通用管线的验收核心**，不是「顺带试一次」。

**本机实测（2026-11-06 15:50 更新）**：
- ✅ VS2022 Community + `cl.exe` 14.38.33130 + Windows SDK 10.0.26100.0（D3D12 / OpenGL 头、库、debug layer 全在这里）
- ✅ CMake 3.31.6 便携版；标准构建链 `configure → build → SELFTEST PASS (7/7)` 全通过（`scripts/build-m0.1.bat`，NMake 生成器）。M0.1 另有零 CMake 兜底 `scripts/verify-m0.1-msvc.ps1`
- ✅ `C:\Windows\System32\vulkan-1.dll` + `vulkaninfo.exe` 驱动自带；**未安装、也无需安装 Vulkan SDK**
- ✅ `NL_GRAPHICS_BACKEND` 两种配置实测都能编译并输出对应后端名（见 M0.1 验收记录）
- ⬜ Slang release、NVRHI 源码、Nsight Graphics（用到的阶段再拿）
- 网络：`raw.githubusercontent.com` 默认超时、`gh-proxy` 稳定可用；开代理后小文件直连 0.94s，大文件仍被限速（单连接 ~12KB/s），改分段并发解决
- GLFW 已从本地 `Carrot/thirdparty/glfw` 复制进 `thirdparty/`（105 个构建必需文件、零网络）

> 已废弃的旧结论（别再捡回来）：「装 Vulkan SDK 自带 slangc / 自带验证层，只装必要组件即可」。那是把单一后端特殊化，与通用管线冲突；且「DX12 会跟 SPIR-V 验收冲突」也是错的——Slang 同一份源码本来就同时出 SPIR-V 和 DXIL。

## M0.1 项目骨架（0.5~1 天）

**做什么**：目录 + CMake 三 target + core 日志 + 能弹出的空窗口 + 后端选择开关。

```
nl-engine/
├── CMakeLists.txt        NL_GRAPHICS_BACKEND (AUTO / DX12 / VULKAN)
├── core/                 core.hpp / log.hpp / assert.hpp / window.hpp+cpp / input.hpp
├── renderer/             renderer.hpp+cpp（M0.1 只报告选定后端，不链接图形 API）
├── viewer/               main.cpp
└── nanite-tools/         （空，S1 填）
```

要点：
- CMake 三 target：`core`（静态库）、`renderer`（静态库，**M0.1 不链接任何图形 API**）、`viewer`（exe，链 core+renderer）。`nanite-tools` 先只建 target 不写码
- 后端由 `NL_GRAPHICS_BACKEND` 编译期选定，只传给 `renderer/` 一个宏；切换后端不需要装任何东西
- 窗口用 GLFW（本地 vendored，别手搓 Win32）
- 日志：spdlog，或者先 `printf` + 宏占位——**别在日志上花超过 1 小时**
- **日志/错误回调框架第一天就留好**（Carrot `46799759` 的 `debugCallback` 与调试层检查是 Init 的一部分，调试层是验收手段不是后期加固）；M0.2 把 NVRHI device callback 直接接到这个口
- 产出代码风格从第一天定死：`viewer/` 和 `core/` 里 **不出现任何图形 API 符号**（`vk` / `Vk` / `D3D12` / `ID3D12` / `nvrhi`），这是渲染器模块纪律的机械保证
- 三 target 保持薄：Carrot 单 `Engine` 类跑了 4 个月才拆模块（`e6bc5196` 起，单次 900+ 行搬家式重构）是他的弯路，我们提前分层正是避开它；但**不能在 M0.1 就把 4 个月后的壳搬进来**

**可见用例**：`viewer.exe` 弹出窗口，标题栏实时显示帧号/帧时间，按 ESC 干净退出；`--help` 与 `--selftest` 都打印当前选定后端名。
**自检**：窗口出现、`--selftest` PASS、`grep -R "\bvk[A-Z]\|D3D12\|ID3D12" viewer/ core/` 零命中；截图 `docs/screenshots/m0.1.png`。验收通过才打 tag：`s0-init`。

**M0.1 验收记录（2026-11-06，本机实跑）**：
- MSVC 14.38 直编 GLFW + 三个 target：编译 / 链接退出码 0
- `viewer.exe --selftest` → `SELFTEST PASS (7/7)`（日志格式化与 sink、干净运行零错误、设备错误独立计数、断言可接管、窗口默认可缩放、后端已选定且无 device、自检收尾干净）
- `-DNL_GRAPHICS_BACKEND=DX12` → `backend: dx12`；`-DNL_GRAPHICS_BACKEND=VULKAN` → `backend: vulkan`，两次都 PASS，**全程未安装任何厂商图形 SDK**（通用管线的机械证据）
- `viewer.exe` 主循环 → `[nl:info] window created 1280x720 | no GL context | ESC to quit`，标题栏帧计数器持续更新
- `grep` 检查 `viewer/`、`core/` 下图形 API 符号 → 零命中
- 复现：`powershell -ExecutionPolicy Bypass -File scripts\verify-m0.1-msvc.ps1`（零 CMake 依赖）；标准构建见 `scripts/build-m0.1.bat`
- 未自动完成项：验收截图 `docs/screenshots/m0.1.png` 需在本机运行 viewer 后手动保存（沙箱内 GUI 不便截屏）

## M0.2 NVRHI 接入：设备 + swapchain（1~2 天）

**做什么**：拿到 NVRHI → 创建 device（feature 一次验全）→ swapchain → 接入主循环 → 两个后端都跑通。

- [x] 获取 NVRHI：`git clone --recursive`（gh-proxy 镜像）或 CMake FetchContent；Vulkan 头用 `NVRHI_FETCH_VULKAN_HEADERS` 拉 `Vulkan-Headers`，**不读 `VULKAN_SDK`**
- [x] `IDeviceCreationParams`：debug 模式开**当前后端的调试层**（DX12 Debug Layer / VVL），device callback 错误信息接到日志
- [x] feature 检查写死在设备创建里：**rayQuery、shaderBufferInt64Atomics、indirectDraw**（S7/S8 要用，现在验掉，免得写完了发现不支持）
- [x] graphics queue + `ISwapChain`（sRGB 格式起步），resize 处理留好接口（窗口缩放必测）
- [x] 默认 DX12（Windows 零额外依赖）；`-DNL_GRAPHICS_BACKEND=VULKAN` 重配一次，**同一份 viewer 代码出同一个画面**——这一步就是「抽象是否真的通用」的验收

**自检**：resize 窗口不崩不黑屏；两后端调试层全程零输出；两后端画面一致。tag：`s0-device`。

**M0.2 验收记录（2026-11-06，本机实跑）**：
- ✅ DX12 / Vulkan 两后端 `viewer.exe --screenshot ... --frames 8` 均打印 `shutdown ok ... deviceErrors=0`、退出码 0
- ✅ **两后端画面逐字节相同**：`docs/screenshots/m0.2-vulkan.png` 与 `m0.2-dx12.png` MD5 一致（`bdebaf50f80697e7ee42e362e658cc78`），清屏色深海军蓝 RGB≈13,21,48
- ✅ resize 回归 `640x360 → 1024x768`：`--resize 1024x768 --screenshot ...` 存 `.before.png` + 终帧 PNG，前后均非黑、`deviceErrors=0`、无 `VK_ERROR_OUT_OF_DATE_KHR` 硬错误（present 阶段过期已按标准 Vulkan 实践处理）
- ✅ **退出段错误已修**：`shutdown()` 析构顺序改为先 `nvrhiDeviceHandle_.Reset()` 让 NVRHI 用活着的 device 清内部对象，再销毁原生 native 对象；进程退出码 0（原 139 use-after-free）
- ✅ **零厂商 SDK**：Vulkan 后端无 `vulkan-1.lib` 依赖，全部 Vulkan 函数经 `vulkan-1.dll`（驱动自带）运行时 `LoadLibraryW` + `GetProcAddress("vkGetInstanceProcAddr")` 解析，编译链接退出码 0
- 复现：`scripts/build-m0.2.bat VULKAN`（NMake 生成器，原生 `cmd.exe` 显式环境跑 `cmake --build`）

⚠️ 常见坑：
- swapchain resize 处理不干净是本里程碑唯一高发 bug，且**每个后续阶段都要回归**（Carrot 实况修了 4 次）
- feature 漏开在创建 device 时才炸——从第一天就把 S7/S8 要的能力写进检查列表，别以后返工
- 别把后端差异泄漏到 `renderer/` 之外；一旦发现 `viewer/` 里出现 `if (backend == ...)`，说明抽象没做干净

## M0.3 第一个三角形 + 帧同步（2~3 天，本里程碑的灵魂）

**做什么**：dynamic rendering → 图形 pipeline → 顶点/片元 shader（Slang 编译）→ fence/semaphore 同步 → 每帧 submit。

- [ ] 写 `triangle.slang`（vertex + fragment 两个 entry），CMake `add_custom_command` 调 `slangc` **同一份源码编译出 SPIR-V 和 DXIL 两个目标**，target 依赖挂上（改 shader 自动重编）
- [ ] 渲染循环：NVRHI `ICommandList` 记录 → `executeCommandList` → fence 同步，**frame in flight = 2**；自动 barrier 跟踪保持开启
- [ ] render target 直接用 NVRHI begin/end render pass（无手写 RenderPass/Framebuffer 样板）
- [ ] 画一个硬编码三角形，两个后端各跑一次

**自检**：三角形出现；任务管理器看 GPU 占用非 0（确认真在 GPU 上画）；拖动窗口/resize 正常；当前后端调试层零输出；**故意删掉一个 fence 等待看同步校验报错**（验证安全网真的在工作）。tag：`s0-triangle`。

⚠️ 常见坑：
- 同步写错的表现是「偶发黑屏/设备丢失」而不是每次都崩——M0.3 写完当场把边界情况测掉，S4 之后再来查就是灾难
- NVRHI 自动 barrier 大幅降低出错面，但**帧间同步（frame in flight）仍要自己管**——fence 语义别想当然

## M0.4 glTF 模型 + 纹理 + 相机（2 天）

**做什么**：加载 Suzanne.glb → 顶点/索引 buffer 上传 → 贴图（staging + image barrier）→ 每帧 MVP → 轨道相机。

- [x] glTF 库：**tinygltf**（本机 Carrot 已 vendored，零网络；不选 fastgltf 是为了不引入新依赖）→ `thirdparty/tinygltf/`，只在 `renderer/src/scene.cpp` 一个 TU 展开实现
- [x] buffer 上传走 staging：NVRHI `writeBuffer`/`writeTexture` 内部的 upload manager（staging buffer + CopyBufferRegion / vkCmdCopyBufferToImage + 自动 barrier），收进共享 `Scene` 而不是两后端各抄一遍
- [x] 纹理：staging 上传 → NVRHI 纹理（`RGBA8_UNORM`）+ sampler；自动 barrier 把资源转到 `ShaderResource`，对照 `doc/Tutorial.md` 的 "Filling the Resource Data" 一节
- [x] MVP：**volatile constant buffer**（NVRHI 官方教程的轻量 CB，语义接近 push constant，两后端由 NVRHI 统一管版本池），轨道相机 `core/camera.*`（拖动旋转 / 滚轮缩放 / 距离钳制）
- [x] depth buffer：`Format::D32`，两个后端随 swapchain 尺寸重建，framebuffer 带 depth attachment，每帧 `clearDepthStencilTexture`
- [x] 关背面剔除对照：`C` 键 / `--cull none` 切两套 pipeline；滚轮把相机推进模型内部（`--dist 0.6`）时，`cull=back` 只剩背景、`cull=none` 能看到带贴图内壁

**实现与计划的差异（有意为之，写进台账）**：`fastgltf → tinygltf`（零新依赖）；`push constant → volatile CB`（NVRHI 教程主推、跨两后端更稳）；`Suzanne.glb` 由 Khronos 官方 glTF 三件套（`Suzanne.gltf/.bin/_BaseColor.png/_MetallicRoughness.png`）用 `scripts/pack-suzanne-glb.py` 打包成单文件，几何/贴图原样保留。

**自检**（已实测，证据 `docs/screenshots/m0.4-*.png`）：Suzanne 带贴图正确显示（黄色眼睛 + 皮毛细节），相机可拖动/缩放；`--dist 0.6 --cull none` 看到内壁、同参数 `--cull back` 只剩背景；resize 640×360 → 1024×768 前后非黑；两后端正常视角 PNG **逐字节相同**（MD5 `7e528087…`）、内壁/resize 画面 RGBA 平均绝对差 0、`deviceErrors=0`。tag：`s0-model`。

## M0.5 ImGui + 收尾验收（1 天）

- [ ] ImGui（docking 分支）接进渲染循环：一个 HUD 显示 FPS、帧时间、相机参数、**当前后端名**
- [ ] `--mode normal` 参数框架搭好（S1 起每个里程碑加一个 mode，模式分发器今天定型）
- [ ] README 更新构建说明（`cmake -B build -DNL_GRAPHICS_BACKEND=... && cmake --build build`）
- [ ] **验收对照 docs/ROADMAP.md 的 S0 三条**：
  - [ ] 画面正确、可交互
  - [ ] **两个后端**的调试层都零报错
  - [ ] 一份 `.slang` 同时编译出 SPIR-V 与 DXIL，两个后端各自加载成功

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

节奏提醒：M0.2 是纯样板代码，看着长其实机械，照官方示例走别发挥；**M0.3 是 S0 唯一需要动脑的地方**（同步模型），多留时间。

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| **这一步该按什么顺序搭** | ⭐ `docs/s0/CARROT-BOOTSTRAP.md`：Carrot 从 `Init`（`46799759`）到三角形（`152edabe`）、staging 抽象（`d11508e3`）、模型（`d25a1cc9`）的实况流程，带可 `git show` 的 hash |
| NVRHI 任何一步 | NVRHI / Donut 官方示例（外部代码走 gh-proxy 拉来研究）；vulkan-tutorial.com 当底层原理对照 |
| 工程组织怎么长出来 | Carrot `e6bc5196` → `4d06c0d3` → `8c227e91`：单 Engine 跑通 4 个月后才拆模块，看它**什么时候**拆比看最终目录有用 |
| 同步疑问 | vkguide.dev 的 synchronization 章节；《Vulkan Synchronization Validation》官方页 |
| slangc 参数 | `slangc -h`；https://shader-slang.org 的用户指南 |
| 下载挂了 | GitHub release 用 gh-proxy 镜像前缀；Vulkan 头走 NVRHI 的 `NVRHI_FETCH_VULKAN_HEADERS`，别去下整套 SDK |

## 底线

- 任何时刻**当前后端调试层**（DX12 Debug Layer / VVL）有输出 = 有 bug，零容忍
- 图形 API 符号出现在 core/viewer = 结构错误，当场挪进 renderer/
- **构建链出现厂商图形 SDK 依赖 = 架构错误**：通用管线不能只给一个后端开特权
- M0.3 的同步想不清楚就画时序图（CPU timeline vs GPU timeline 两行，fence/semaphore 标在交点上），画完自然明白
