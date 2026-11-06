# S0 详细开发路线图：从空目录到「画出一个能转的 Suzanne」

> 对应总路线 `docs/ROADMAP.md` 的 S0。预计 5 个里程碑、业余时间 1~2 周。
> 每个里程碑 = 一段可运行代码 + 一个 git tag。**跑不通当下一步，不攒债。**

---

## M0.0 环境准备（半天）

| 依赖 | 版本建议 | 获取 | 验证命令 |
|---|---|---|---|
| Visual Studio 2022 | Community，勾「使用 C++ 的桌面开发」 | 官网 | `cl` 能在开发者命令行找到 |
| CMake | ≥ 3.24 | 官网 / VS 自带 | `cmake --version` |
| Vulkan SDK | 最新（LunarG） | https://www.lunarg.com/vulkan-sdk/ （直链下载，不走 GitHub） | `vkcube` 能跑；`VULKAN_SDK` 环境变量已设置 |
| Slang | 最新 release | https://github.com/shader-slang/slang/releases ——**本机 GitHub 下载易挂，失败就用 gh-proxy 镜像前缀**：`https://gh-proxy.com/https://github.com/shader-slang/slang/releases/...` | `slangc -h` 有输出 |
| NVRHI | 最新 master | `git clone --recursive`（走 gh-proxy 镜像）或 CMake FetchContent | `cmake` 能构建出静态库 |
| 测试模型 | Suzanne 带贴图 glb | Blender 导出，或随便一个 glTF 样例 | — |

环境自检清单：
- [ ] `vkcube` 弹出旋转方块（说明驱动+SDK 正常）
- [ ] `slangc -h` 正常
- [ ] Nsight Graphics 已装（验收日要用）

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
- 产出代码风格从第一天定死：`viewer/` 和 `core/` 里 **不出现任何 `vk` 开头的符号**，这是渲染器模块纪律的机械保证

**自检**：`viewer.exe` 弹出空窗口，按 ESC 退出。tag：`s0-init`。

## M0.2 NVRHI 接入：设备 + swapchain（1~2 天）

**做什么**：拿到 NVRHI → 创建 device（feature 一次验全）→ swapchain → 接入主循环。

- [ ] 获取 NVRHI：`git clone --recursive`（本机网络走 gh-proxy 镜像前缀）或 CMake FetchContent
- [ ] `IDeviceCreationParams`：debug 模式开 **NVRHI 验证层**，device callback 错误信息接到日志
- [ ] feature 检查写死在设备创建里：**rayQuery、shaderBufferInt64Atomics、indirectDraw**（S7/S8 要用，现在验掉，免得写完了发现不支持）
- [ ] graphics queue + `ISwapChain`（sRGB 格式起步），resize 处理留好接口（窗口缩放必测）
- [ ] 底层后端起步选 Vulkan（Windows/Linux/MoltenVK 同一套代码；发布期想切 DX12 改一个枚举——这就是跨平台底座的意义）

**自检**：resize 窗口不崩不黑屏；NVRHI 验证层全程零输出。tag：`s0-device`。

⚠️ 常见坑：
- swapchain resize 处理不干净是本里程碑唯一高发 bug
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
| NVRHI 任何一步 | NVRHI / Donut 官方示例；vulkan-tutorial.com 当底层原理对照（NVRHI 的 Vulkan 后端与之一一对应） |
| 想看更工程化的组织 | `E:\UE_STUDY_2026\Carrot\engine\engine\vulkan\`（只看风格，别照抄体量） |
| 同步疑问 | vkguide.dev 的 synchronization 章节；《Vulkan Synchronization Validation》官方页 |
| slangc 参数 | `slangc -h`；https://shader-slang.org 的用户指南 |
| 下载挂了 | GitHub release 用 gh-proxy 镜像前缀；Vulkan SDK 走 LunarG 直链没问题 |

## 底线

- 任何时刻验证层（NVRHI 验证层 / VVL）有输出 = 有 bug，零容忍
- NVRHI 符号出现在 core/viewer = 结构错误，当场挪进 renderer/
- M0.3 的同步想不清楚就画时序图（CPU timeline vs GPU timeline 两行，fence/semaphore 标在交点上），画完自然明白
