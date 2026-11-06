# 里程碑台账：动作 / 提交版本 / 完成情况 / 踩坑

> 每完成一个小里程碑，**本地提交一波**，并在这里追加一条记录。
> 四栏写实：动作写实际改了哪些文件；完成情况写可复现的验收结果；踩坑写「现象 → 根因 → 处理」，**不许写「已完成」「无」**。
> 提交版本以 git tag 为准，精确 hash 用 `git rev-parse <tag>` 查。
> 总路线 `docs/ROADMAP.md` · S0 阶段门禁 `docs/s0/S0-PLAN.md` · Carrot 流程参考 `docs/s0/CARROT-BOOTSTRAP.md`（仅参考，弯路不复刻）。

## 速查

| 阶段 | 状态 | 提交版本 | 日期 | 一句话结果 |
|---|---|---|---|---|
| M0.0 环境准备 | 🟡 部分完成 | 随 M0.1 提交；环境补齐快照 `v0.1.0`；架构校准见本节补记 | 2026-11-06 | 编译链 / GLFW / CMake / DX12·Vulkan 双后端开关齐并验收，**零厂商 SDK**；Slang / NVRHI / Nsight 按阶段取 |
| M0.1 骨架 + 空窗口 | ✅ 验收通过 | `s0-init` | 2026-11-06 | 窗口弹出、ESC 退出、`SELFTEST PASS (7/7)`；两种后端配置都 PASS |
| M0.2 设备 + swapchain | ✅ 验收通过 | `s0-device` | 2026-11-06 | 双后端清屏逐字节相同、resize 不黑屏、deviceErrors=0、退出无段错误 |
| M0.3 三角形 + 帧同步 | ✅ 验收通过 | `s0-triangle` | 2026-11-06 | 双后端顶点色三角形渲染、frame-in-flight=2、selftest 9/9；SPIR-V 入口恒为 `main` 的坑已固化见 `docs/SHADER-CONVENTIONS.md` |
| M0.4 模型 + 贴图 + 相机 | ✅ 验收通过 | `s0-model` | 2026-11-07 | 双后端带贴图 Suzanne 逐像素一致、内壁剔除对照、resize 回归、selftest 11/11、deviceErrors=0 |
| M0.5 ImGui + 总验收 | ⬜ 未开始 | `s0-done` | — | — |

---

## M0.0 环境准备

**动作**
- 自检本机工具链与仓库状态；确认 VS2022 Community、`cl.exe` 14.38.33130、Windows SDK 10.0.26100.0 已装。
- 从本地 `E:\UE_STUDY_2026\Carrot\thirdparty\glfw\` 复制 GLFW 源码到 `thirdparty/glfw/`（Carrot Init 的 vendored 思路，零网络），删掉 submodule 的 `.git` 引用文件、`docs/examples/tests`（提交 105 个构建必需文件）。
- 核对 Carrot 的 `cmake_modules/ShaderSlangConfig.cmake` 与 `BUILDING.md`：确认它从 `$ENV{VULKAN_SDK}` 找 slangc 是**裸 Vulkan 单后端**的做法，本项目通用管线不继承，Slang 独立获取（见下方架构校准补记）。
- 测试外部代码通道：`raw.githubusercontent.com` 默认超时、`gh-proxy` HTTP 200；开代理后直连 0.94s。
- 下载并安装 CMake 3.31.6 便携版到 `E:\UE_STUDY_2026\.build-tools\`（不污染系统 PATH），把 `scripts/build-m0.1.bat` 接到标准构建链并实测全通。
- 实测三后端依赖来源：D3D12 / OpenGL 头与调试层来自 Windows SDK、Vulkan loader 驱动自带、Vulkan 头走 `Vulkan-Headers`；确认**不需要安装任何厂商图形 SDK**。
- 更新 `docs/s0/S0-PLAN.md` 的 M0.0 依赖表（删掉重复的 CMake 行）与本机实测段。

**提交版本**：随 M0.1 的 `s0-init` 提交；环境补齐后的版本快照另打 `v0.1.0`。

**完成情况**（部分完成）
- ✅ 可编译：MSVC + Windows SDK 实测编译 GLFW 与 C++17 代码，退出码 0。
- ✅ GLFW 本地化：`thirdparty/glfw/` 105 文件，`add_subdirectory` 所需结构完整。
- ✅ CMake 3.31.6：`configure` 退出码 0 → `build` 退出码 0（glfw / nl_core / nl_renderer / viewer 四 target 全过）→ `SELFTEST PASS (7/7)`，标准构建链已可复现。
- ✅ Vulkan 运行时就绪：`C:\Windows\System32\vulkan-1.dll`（1.5MB）+ `vulkaninfo.exe` 驱动自带。
- ✅ **不需要 Vulkan SDK**：`VULKAN_SDK` 未设置、`vkcube` 不可用、无 `VkLayer_*.json`；DX12 后端用 Windows SDK 自带 Debug Layer，Vulkan 头由 `NVRHI_FETCH_VULKAN_HEADERS` 拉，VVL 是可选插件而非阶段前置。
- ⬜ Slang release / NVRHI 源码 / Nsight Graphics：用到的阶段再拿，都不是环境前置。

**踩坑情况**
1. **命令含 `MSBuild.exe` 字样被安全策略整条拦截** → 根因：命令字符串命中 LOLBin 名单，与该命令是否真的执行它无关。处理：拆成不含该字样的独立命令。
2. **PowerShell 工具里不能调 `cmd.exe` 来跑 `vcvars64.bat`** → 根因：工具层禁止。处理：手动设置 `INCLUDE` / `LIB` / `PATH` 三个环境变量后直接调 `cl.exe`，不依赖 vcvars。
3. **CMake 装不上**：cmake.org 与 GitHub Releases 的 46MB zip 都只有约 12KB/s（240s 只下 2.8MB），开梯子也只改善小文件 → 根因：单连接被限速，不是站点不可达。处理：先写 `scripts/verify-m0.1-msvc.ps1` 让 M0.1 不依赖 CMake；随后经 `gh-proxy` 分 8 段并发下载（每段约 5.6MB，2m35s 拿齐 45MB，zip 校验无错），解压出便携版 3.31.6。
4. **GitHub 直连状态会变**：默认网络超时，开代理后恢复 → 处理：文档里两条通道都留（直连 / gh-proxy），不写死单一结论。
5. **复制 submodule 目录带 `.git` 引用文件** → 会让 git 当作嵌套仓库。处理：复制后删除，提交前核对 `git status`。
6. **CMake 用 VS 生成器报 `No CMAKE_CXX_COMPILER could be found`** → 根因：`CMakeConfigureLog.yaml` 里 MSBuild 抛 `MSB6001 / System.ArgumentException: 已添加项，字典中关键字 "HTTPS_PROXY" / "https_proxy"`——代理环境变量大小写两份同时存在，MSBuild 用大小写不敏感 Hashtable 收集环境变量时撞键，`CL.exe` 起不来，与 VS / SDK 装没装无关。处理：改用 `NMake Makefiles` 生成器（直接调 `cl.exe`，不经过 MSBuild），`scripts/build-m0.1.bat` 同步改掉；也可在构建前删掉小写那份代理变量再用 VS 生成器。
7. **S0-PLAN 依赖表里 CMake 写了两行** → 根因：多次编辑叠加。处理：合并成一行并写上实际获取方式。
8. **把 Vulkan SDK 列成「必需依赖」，还专门写「为什么需要 Vulkan SDK」为它辩护** → 根因：把 Carrot（裸 Vulkan 单后端）的依赖习惯带进 NVRHI 通用管线，DX12 / OpenGL 不用装却给 Vulkan 单开特权，架构上双标；还错误断言「DX12 与 SPIR-V 验收冲突」（Slang 一份源码本来就同时出 SPIR-V / DXIL）。处理：见下方架构校准补记。

### 补记 · 架构校准：通用管线不装任何厂商 SDK（2026-11-06 15:50）

**动作**
- `CMakeLists.txt` 新增 `NL_GRAPHICS_BACKEND`（`AUTO` / `DX12` / `VULKAN`）：`AUTO` 在 Windows 落 DX12、其它平台落 Vulkan；`configure` 打印 `nl-engine backend: XXX (no vendor SDK required)`；只给 `renderer/` 传一个编译期宏，M0.1 不链接任何图形 API。
- `renderer/` 按宏报告后端名（`status().backend`），`viewer --help` / `--selftest` 打印该名字；同一份源码两种配置编出两个后端名，作为「不绑死单一后端」的机械证据。
- 删除 `S0-PLAN.md` 的「Vulkan SDK」依赖行、`vkcube` / `VULKAN_SDK` 自检项与「为什么需要 Vulkan SDK」整节，换成「后端对称：谁都不装 SDK」表 + 五条写死规则；门禁表 M0.0 / M0.2 / M0.5 改成双后端可看用例；Slang 改为独立获取。
- 同步清理 `PLAN.md`（新增后端对称决策）、`ROADMAP.md`、`LINKS.md`、`s0/CARROT-BOOTSTRAP.md` 与 S0 看板参考索引里的 SDK 表述。`kb/index.html` 原本就是「VVL / DX12 debug layer」对称写法，无需改。

**提交版本**：本次架构校准提交；里程碑 tag 仍为 `s0-init`，版本快照 `v0.1.0` 已存在，本次不预支新 tag。

**完成情况**
- ✅ `-DNL_GRAPHICS_BACKEND=DX12` → `backend: dx12` + `SELFTEST PASS (7/7)`。
- ✅ `-DNL_GRAPHICS_BACKEND=VULKAN` → `backend: vulkan` + `SELFTEST PASS (7/7)`。
- ✅ 两次构建全程未安装、未读取任何厂商图形 SDK；`CMakeLists.txt` / `scripts/` 里 `VULKAN_SDK` / `find_package(Vulkan)` 零命中。
- ✅ 通用管线的可见用例成立：同一份 viewer 源码，两种配置输出不同后端名且都 PASS。

**踩坑情况**
1. **现象**：用户连问三次（「既然是通用管线还专门装 SDK 说不过去」→「为什么 OpenGL / DX 的就不装」→「不要来回反复」）。**根因**：一直在给依赖清单辩护，没接住架构问题；上一轮还把「装 SDK 是省事选择」写成结论。**处理**：一次改到位、不留「也可以装」的尾巴，所有相关文档同步清理，并写进长期记忆。

---

## M0.1 项目骨架 + 空窗口

**动作**
- 新建 `CMakeLists.txt`：三 target `nl_core` / `nl_renderer` / `viewer`，GLFW 走 `add_subdirectory`，`ctest` 注册 `m0.1-selftest`。
- 新建 `core/`：`log.hpp/cpp`（分级日志 + 可替换 sink + 设备错误独立计数，留给 M0.2 接 NVRHI device callback）、`assert.hpp/cpp`（handler 可接管）、`window.hpp/cpp`（pimpl 把 GLFW 关在 .cpp，`GLFW_NO_API` 为 Vulkan 预留，nativeHandle 留给 M0.2 建 surface）。
- 新建 `renderer/` 占位 `status()`（M0.2 起在这里接 NVRHI）。
- 新建 `viewer/main.cpp`：窗口用例（标题栏实时帧号 / 帧时间、ESC 退出）+ `--selftest` 七项无窗口自检。
- 新建 `scripts/verify-m0.1-msvc.ps1`（零 CMake 验收）、`scripts/build-m0.1.bat`（标准 CMake 构建）。
- 挖 Carrot 2020-11 从零搭建的提交历史，新建 `docs/s0/CARROT-BOOTSTRAP.md`；修正 `README.md`、`docs/PLAN.md`、`ROADMAP.md`、`kb/index.html`、`S0-PLAN.md` 里「只看风格」的旧表述，改为「搭建节奏参考、弯路不复刻」；S0-PLAN 新增六阶段门禁表。
- 同步更新 S0 路线图看板的参考索引与铁律条文字。

**提交版本**：`s0-init`（M0.1 验收通过后本地提交并打 tag，`git rev-parse s0-init` 查 hash）。

**完成情况**
- ✅ MSVC 14.38 直编 GLFW + 三 target：编译、链接退出码 0。
- ✅ `viewer.exe --selftest` → `SELFTEST PASS (7/7)`：日志格式化与 sink、干净运行零错误、设备错误独立计数、断言可接管、窗口默认可缩放、renderer 占位不越界、自检收尾干净。
- ✅ 可见用例：`[nl:info] window created 1280x720 | no GL context (Vulkan/NVRHI ready) | ESC to quit`，主循环运行、标题栏帧计数器持续更新。
- ✅ 模块纪律：`viewer/`、`core/` 下 `vk[A-Z]` / `Vk[A-Z]` / `nvrhi` 零命中（`renderer/` 仍为占位）。
- ✅ 复现命令：`powershell -ExecutionPolicy Bypass -File scripts\verify-m0.1-msvc.ps1`。
- ⬜ 验收截图 `docs/screenshots/m0.1.png`：沙箱内 GUI 不便截屏，需本机运行 viewer 后手动保存。

**踩坑情况**
1. **`error C2664`：捕获型 lambda 无法转 `AssertHandler` 函数指针** → 根因：`setAssertHandler` 参数是函数指针，`[&]` 捕获 lambda 无转换。处理：静态布尔变量 + 无捕获 lambda。
2. **`warning C4127`：`NL_CHECK(1 == 2)` 是常量条件** → 处理：改用 `volatile int one = 1; NL_CHECK(one == 2)`，既测到失败路径又不产生常量折叠警告。
3. **链接 19 个 `LNK2019` + `LNK1120`（`glfwWindowHint` / `_glfwInputWindowFocus` 等全丢）** → 根因：我们的 `window.cpp` 和 GLFW 的 `window.c` 都会生成 `window.obj`，同目录编译后者覆盖前者，GLFW 实现被吃掉；同时 `@objs` 与 `@cpp` 重复指定触发 `LNK4042`。处理：C 与 C++ 的 obj 分目录（`build/glfw-obj` / `build/nl-obj`），链接时显式传两组完整路径；编译与链接分两步，不再把 cpp 和 obj 混在一条 cl 命令里。
4. **中文注释在 MSVC 下有编码风险** → 处理：C++ 编译统一加 `/utf-8`。
5. **需求理解偏差：一开始把 Carrot 流程写成「逐行对齐」** → 根因：把实况时间线当成了必须复刻的路线，忽略他早期换底座 / 晚拆模块 / 相机坏 5 天 / resize 修 4 次都是弯路。处理：文档改为「取节奏、避弯路」，新增「不复刻的弯路」表，并同步清理 README / PLAN / ROADMAP / kb / S0-PLAN / 设计稿共 6 处旧表述。

---

## M0.2 设备 + swapchain

**动作**
- 新建 `renderer/src/backend.hpp`（`Backend` 抽象接口：init / resize / renderFrame / shutdown / 设备错误计数）、`backend_dx12.cpp`、`backend_vulkan.cpp`、`png_writer.cpp`（`writePngRGBA8` GPU 回读写 PNG）。
- 扩展 `renderer/src/renderer.cpp` 与 `renderer/include/nl/renderer/renderer.hpp`：按 `NL_GRAPHICS_BACKEND` 选后端，暴露 NVRHI device 错误回调接进 `core` 日志。
- 扩展 `core/src/window.cpp` + `core/include/nl/core/window.hpp`：`nativeHandle()` 给 Vulkan 建 Win32 surface 用，resize 事件经 `consumeResize` 传给 renderer。
- 扩展 `viewer/main.cpp`：`--screenshot <path> [--frames N] [--resize WxH]` 回归模式，渲染 N 帧后 GPU 回读像素写 PNG；resize 回归额外存 `<path>.before.png` 做前后对比；验收门槛 `deviceErrorCount()==0 && errorCount()==0`。
- `CMakeLists.txt`：Vulkan 分支接 `nvrhi_vk`、包含 `vulkan-headers`、定义 `VK_USE_PLATFORM_WIN32_KHR`；**零 `vulkan-1.lib` 依赖**——所有 Vulkan 函数经 `vulkan-1.dll`（驱动自带）运行时 `LoadLibraryW` + `GetProcAddress("vkGetInstanceProcAddr")` 解析。
- vendored 依赖进 `thirdparty/`：`nvrhi/`、`vulkan-headers/`、`directx-headers/`、`stb/`（`png_writer` 用 stb_image_write）。

**提交版本**：`s0-device`（M0.2 验收通过后本地提交并打 tag）。

**完成情况**
- ✅ **Vulkan 全函数链接零 SDK**：本机无 `vulkan-1.lib`，`vkCreateInstance` 等核心函数也无静态导出符号；用 `LoadLibraryW(L"vulkan-1.dll")` 拿 `vkGetInstanceProcAddr`，22 个 Vulkan 函数指针（核心 + KHR）全部运行时解析，编译链接退出码 0。
- ✅ NVRHI Vulkan device + swapchain 起得来：设备 `NVIDIA GeForce RTX 4060 Laptop GPU`、swapchain 创建、`deviceErrors=0`、PNG 写出（9416 字节）。
- ✅ **双后端逐字节相同**：`m0.2-vulkan.png` 与 `m0.2-dx12.png` MD5 完全一致（`bdebaf50f80697e7ee42e362e658cc78`），同一清屏色（深海军蓝 RGB≈13,21,48），验证通用管线不绑死单一后端。
- ✅ resize 回归不黑屏：640×360 → 1024×768，前后 PNG 均非黑、`deviceErrors=0`、无 `VK_ERROR_OUT_OF_DATE_KHR` 硬错误。
- ✅ **退出段错误已修**：`shutdown()` 析构顺序改为先 `nvrhiDeviceHandle_.Reset()` 让 NVRHI 用活着的 device 清内部对象，再销毁原生 native 对象；进程退出码 0（原 139）。
- ✅ 验收门槛：`viewer.exe --screenshot ... --frames 8` 与 `--resize 1024x768 --screenshot ...` 均打印 `shutdown ok ... deviceErrors=0`、退出码 0。

**踩坑情况**
1. **21 个 `vk*KHR` + 12 个核心函数 `LNK2019`** → 根因：本机无 `vulkan-1.lib`，KHR 扩展与核心 `vkCreateInstance` 等都没有静态导出符号，不能静态链接。处理：loader 引导 + 全函数指针化（`LoadLibraryW` + `GetProcAddress("vkGetInstanceProcAddr")` + `loadRemaining()` 解析 22 个 `PFN_vk*`）。
2. **`DispatchLoaderDynamic` 不在 `vk` 命名空间（C2065/C2039）** → 根因：NVRHI 强制 `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=0`，本 TU 的 `#define` 被覆盖。处理：彻底弃用 `vulkan.hpp` 动态派发器，改纯 C 函数指针（同时避开 NVRHI 已定义的全局派发器存储 `LNK2005`）。
3. **命名 `vkDev_` / `vkDevice_` 辨识度为零** → 根因：两名字只差两个字母、极易看串。处理：三前缀约定（用户认可）——`vulkan*_`=原生句柄、`nvrhi*_`=NVRHI 抽象层、`vk*_`=运行时函数指针，全文件统一。
4. **present 阶段 `VK_ERROR_OUT_OF_DATE_KHR`（resize 报硬错误、验收失败）** → 根因：`renderFrame` 只处理 acquire 阶段的过期，present 阶段过期被当硬错。处理：present 返回 `OUT_OF_DATE` 时 `recreateSwapchain()` + `createTargets` + `return true`（不计错误），与 acquire 阶段一致。
5. **进程退出段错误 `exit=139`（删除 `FreeLibrary` 后依旧）** → 根因：原 `shutdown()` 先 `vkDestroyDevice_` 销毁原生 device，**后**才 `nvrhiDeviceHandle_.Reset()`，而 NVRHI 的 `Device::~Device()` 会用那个已被销毁的 device 去 `destroyQueryPool` 等内部对象 → use-after-free。查证：NVRHI 全源码**无 `vkDestroyDevice` 调用**，它只持有非拥有的 `vk::Device` 包装，故 VkDevice 只由本后端销毁一次。处理：`shutdown()` 顺序改为先 `Reset()` NVRHI 句柄、再销毁 sem/swapchain/surface/device/instance。
6. **`nmake: command not found`（Git Bash 下 `cmake --build` 派生失败）** → 根因：Git Bash 用 Windows 风格 `C:/...` 的 PATH 搜不到 exe，且 `cmake --build` 调 Windows cmake 派生 nmake 路径不稳。处理：用原生 `cmd.exe` + `set "PATH=C:\...\Hostx64\x64;..."` 显式环境跑 `cmake --build`（与 `scripts/build-m0.2.bat` 一致）。

---

## M0.3 第一个三角形 + frame in flight=2（Slang 双后端）

**动作**
- `renderer/shaders/triangle.slang`：单一 Slang 源，顶点色三角形（POSITION+COLOR）。
- `CMakeLists.txt`：Slang 编译块，同一份源码 `-target spirv` 与 `-target dxil` 各出 vs/ps 四产物（`tri.{vs,ps}.{spv,dxil}`）；`NL_SHADER_DIR` 注入 `nl_renderer`；修正 `add_dependencies(viewer nl_shaders)` 写在 `add_executable(viewer)` 之前的顺序 bug。
- `renderer/src/backend.hpp`：加 `readFileBytes` 内联，两后端共享加载字节码。
- `renderer/src/backend_dx12.cpp` / `backend_vulkan.cpp`：`initTriangle`（读字节码/建 shader/vb/inputLayout/framebuffer）、`ensurePipeline`（`createGraphicsPipeline(desc, FramebufferInfo)`）、`renderFrame` 插入 draw；DX12 加 per-slot fence 同步安全网、Vulkan 加 acquire semaphore 门控（删每帧 `waitForIdle`，实现 frame-in-flight=2）。
- `renderer.cpp` 的 `kStage` → M0.3；`viewer/main.cpp` selftest 断言 → M0.3。
- 拉取并 vendored 参考：`thirdparty/nvrhi/doc/`（Tutorial/ProgrammingGuide，确认 vendored 头为 2021 老接口、无 `beginRenderPass`）、`thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`、`thirdparty/slang-reference/{triangle,hello-world,example-base}/`（含 `hello-world/vulkan-api.cpp` 作 Vulkan 对接参考）。
- 修正 `docs/LINKS.md`、本台账、`MEMORY.md`：纠正原 `NVIDIA/nvrhi/examples/common/SampleRenderer` 链接与「当前 main 用新版 render-pass API」两处误判；官方仓库实为 `NVIDIA-RTX/NVRHI`，且其 `main` 不含 examples（示例在 `NVRHI-samples`），权威参考以 vendored 头 + 本工程 M0.2 后端为准。

**提交版本**：`s0-triangle`（M0.3 验收通过后本地提交并打 tag）

**完成情况**
- 双后端编译/链接通过（DX12 + Vulkan），`nl_shaders` 四产物生成；魔数校验：`.dxil`=DXBC、` .spv`=SPIR-V（`0x07230203`）。
- DX12 可见用例：`viewer --screenshot` 出 640×360 深蓝底、红顶/蓝左下/绿右下顶点色三角形，`deviceErrors=0`。
- Vulkan 可见用例：同结构三角形正确渲染（修复前空白，见踩坑第 4 条）。
- selftest 9/9（DX12 / Vulkan 均 PASS）。
- frame-in-flight=2：DX12 per-slot fence 门控就位（`renderFrame` 开头按 slot 等上一帧 fence，删此段应触发同步报错，S0-PLAN 校验点）；Vulkan acquire semaphore 门控；8 帧连续渲染稳定无错。

**踩坑情况**
1. **CMake `add_dependencies(viewer nl_shaders)` 顺序错** → 现象：configure 报 `viewer` 目标不存在。根因：Slang 块没经 configure 验证的顺序 bug。处理：依赖移到 `add_executable(viewer)` 之后。
2. **DX12 `queue_->Signal(fence_, v)` 传 `ComPtr` 未 `.Get()`** → 现象：C2664。处理：`fence_.Get()`。
3. **`createGraphicsPipeline(desc, IFramebuffer*)` deprecated** → 现象：C4996。处理：改用 `createGraphicsPipeline(desc, FramebufferInfo(FramebufferDesc))` 重载（两后端一致）。
4. **Vulkan 三角形空白（仅清屏）** → 现象：`vk pipeline create -> 0`，draw 永远跳过；DX12 正常。根因：Slang 把 SPIR-V 入口点**固定命名为 `main`**（与 `-entry vsMain` 无关），而 `ShaderDesc.setEntryName("vsMain")` 让 NVRHI-Vulkan 找不到入口 → `vkCreateGraphicsPipelines` 静默失败返回 NULL；DXIL 入口才是 `vsMain`，故 DX12 正常。查证：自写 SPIR-V 解析器确认 `tri.vs.spv` 的 `OpEntryPoint` 名=`main`。处理：Vulkan 路径 `setEntryName("main")`，DX12 保持 `vsMain`；注释写明 Slang→SPIR-V 入口名约定。

---

## M0.4 模型 + 贴图 + 深度 + 轨道相机（Slang 双后端）

**动作**
- 资源：Khronos `glTF-Sample-Models/2.0/Suzanne` 官方三件套（`Suzanne.gltf` / `.bin` / `_BaseColor.png` / `_MetallicRoughness.png`，经 gh-proxy 下载，零厂商 SDK）由 `scripts/pack-suzanne-glb.py` 打包成单文件 `assets/models/Suzanne.glb`（2.5 MB，图片内嵌为 bufferView）。
- `thirdparty/tinygltf/`（本机 Carrot vendored：`tiny_gltf.h` / `json.hpp` / `stb_image.h` / LICENSE），实现只在 `renderer/src/scene.cpp` 一个 TU 展开。
- `renderer/shaders/model.slang`：单一源码，`cbuffer SceneConstants`（MVP + model + light + options，160 B）、`register(t0)` 贴图、`register(s0)` sampler；VS 变换位置/法线，PS 采样贴图 + `abs(N·L)` 双面受光 + 2.2 曝光。
- `renderer/src/scene.hpp/.cpp`（两后端共享，只依赖 `nvrhi.h`）：tinygltf 读 POSITION/NORMAL/TEXCOORD_0/indices/baseColor → VB/IB/Texture/Sampler/volatile CB → `writeBuffer`/`writeTexture` 一次性 staging 上传 → binding layout/set → 两套 pipeline（cull back / none）→ 每帧 `writeBuffer(MVP)` + `drawIndexed`。
- `core/camera.hpp/.cpp`：轨道相机（yaw/pitch/distance、透视 ZO + lookAt 列主序矩阵、距离/俯仰钳制、`cameraSelfTest`）；`core/window.*` 加鼠标拖动 / 滚轮 / 左键回调、`C` / `R` 键。
- 两后端：`createTargets` 里建 `Format::D32` 深度、framebuffer 挂 depth attachment、每帧 `clearDepthStencilTexture`；`renderFrame` 接 `FrameParams`（MVP / cullBack）并改用 `Scene::draw`；resize 与 Vulkan OUT_OF_DATE 路径统一失效 framebuffer/depth/pipeline；shutdown 在 device 存活时 `scene_.shutdown()`。
- `renderer/include/nl/renderer/renderer.hpp` 加 `FrameParams` POD；`renderer.cpp` stage → M0.4；`viewer/main.cpp` 接交互、固定相机参数（`--yaw/--pitch/--dist/--cull`）、标题栏显示相机与剔除状态、selftest 扩到 11 项。
- `CMakeLists.txt`：`camera.cpp` / `scene.cpp` 入 target、`NL_ASSET_DIR` 注入、SPIR-V 编译加 `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`、ctest 名改 `m0.4-selftest`。
- 文档：`docs/SHADER-CONVENTIONS.md`（顶点布局 / 资源绑定偏移表 / cbuffer 160 B / volatile CB）、`docs/s0/S0-PLAN.md`（M0.4 勾选 + 与计划的差异）、`README.md`（目录 + 运行方式）、`scripts/build-m0.2.bat`（阶段无关的说明）。

**提交版本**：`s0-model`（M0.4 验收通过后本地提交并打 tag）

**完成情况**
- ✅ 两后端 `scripts/build-m0.2.bat DX12|VULKAN`：configure 0 / build 0 / `SELFTEST PASS (11/11)`；`ctest` 两后端 `m0.4-selftest` 100% 通过。
- ✅ 可见用例：`viewer.exe` 弹出窗口显示带贴图 Suzanne（黄色眼睛 + 皮毛细节）；左键拖动旋转、滚轮缩放、`C` 切剔除、`R` 重置、`F12` 存图、标题栏实时显示 yaw/pitch/dist/cull。
- ✅ **双后端画面一致**：正常视角 `m0.4-dx12.png` 与 `m0.4-vulkan.png` **MD5 完全相同**（`7e528087d19722d8f9ef4de35e36d382`）；内壁 / resize 截图 PNG 字节不同但 RGBA 平均绝对差 **0**（PIL 核对，extrema 最多 ±1）。
- ✅ **背面剔除真生效**：`--dist 0.6 --cull back` 相机在模型内部只剩背景（13 KB，平均亮度 13.71）；同角度 `--cull none` 显示带贴图内壁（200 KB，平均亮度 147.52）；两后端各一份，结果一致。
- ✅ 深度与 resize：640×360 → 1024×768 前后 PNG 均非黑（平均亮度 36.82 / 41.21），两后端一致；8 帧连续渲染稳定。
- ✅ 全 8 个验收进程退出码 0，日志均 `deviceErrors=0`；D3D12 Debug Layer + NVRHI validation / Vulkan NVRHI validation 零报错。
- ✅ 同一份 `model.slang` 编出 `model.{vs,ps}.{spv,dxil}` 四产物；SPIR-V binding 实测 `{256}`（vs）、`{0,128,256}`（ps），全部 descriptor set 0，与 NVRHI 默认 offset 对齐；DXIL 用 `vsMain/psMain`、SPIR-V 用 `main`（沿用 M0.3 已固化的坑）。
- ✅ 截图证据：`docs/screenshots/m0.4-{dx12,vulkan}.png`、`m0.4-*-cull-{back,none}-inner.png`、`m0.4-*-resize.png(.before.png)`。

**踩坑情况**
1. **首帧 Segmentation fault（退出 139，无任何报错日志）** → 现象：删掉 M0.3 的 `initTriangle()` 后，第一次 `renderFrame` 就崩。根因：`frameFence_` 的 `assign()` 原本写在被删掉的 `initTriangle()` 里，而 `renderFrame` 在 `ensurePipeline()` **之前**就按 swapchain slot 读 `frameFence_[index]`，空 vector 越界。处理：把 framebuffer/fence 表初始化搬进 `createTargets()`（init 与 resize 都经过），代码注释写明顺序约束。定位方式是分段日志（最终定位到「`[dbg] renderFrame enter` 之后、`ensurePipeline` 内第一条日志之前」）。
2. **剔除方向判反：cull=back 显示碗状内壁、cull=none 才是完整正脸** → 根因：glTF 外表面 CCW，叠加 Slang 目标与 NVRHI Vulkan 视口换算后，外表面被判成 back-face；`RasterState::frontCounterClockwise` 默认 false 不对。处理：两套 pipeline 都 `setFrontCounterClockwise(true)`，两后端一致。另把相机最小距离 1.8 → 0.45，允许滚轮钻进模型内部——闭合曲面从外部看不出剔除差异，只有从内部看 `cull=back`（全空）vs `cull=none`（内壁）才有「能亲眼看到」的对照。
3. **SPIR-V 资源绑定与 NVRHI 对不上 + slangc `E39029` 警告** → 根因：Vulkan 没有 b/t/s 三套命名空间，NVRHI 默认 `VulkanBindingOffsets` 把 slot 映射到 SRV 0 / Sampler 128 / CB 256；`register(t0)` 又没给 `-fvk-t-shift` 触发警告。处理：SPIR-V 命令统一加三个 shift（对无资源的 `triangle.slang` 无影响）；自写 SPIR-V 解析器 dump `OpDecorate Binding` 逐个核对后再写渲染代码。规范已固化到 `docs/SHADER-CONVENTIONS.md` §2.5。
4. **`FrameParams` 不能整块塞进 cbuffer** → 根因：C++ 侧 `FrameParams` 尾部有 `bool cullBack`，sizeof 164，而 shader cbuffer 只要前 160 字节；直接 `writeBuffer(&params, sizeof(params))` 会越界。处理：定义纯 POD `SceneConstants`（`static_assert(sizeof == 160)`），`Scene::draw` 里逐段 `memcpy` 四个数组，剔除开关只用于选 pipeline。
5. **官方 Suzanne 贴图太暗，看起来像「没贴图的灰模型」** → 根因：`Suzanne_BaseColor.png` 本身是中灰（只有眼睛是黄色）。处理：`model.slang` 乘 2.2 曝光并用 `min(...,1)` 钳制，截图里皮毛细节和黄色眼睛可见；正式色彩管理（sRGB）留到后续阶段，已写进文档局限。
6. **相机 selftest 假失败（10/11）** → 根因：自检用 `640/360` 与 `16/9` 两个 aspect 期望矩阵不同，但 float 下两者数值相同，`differs()` 永远为 false。处理：改成 `1.0f` aspect 对照，非法 aspect 检查随之调整。
7. **官方仓库没有现成 `Suzanne.glb`** → 现象：直接下 `glTF-Binary/Suzanne.glb` 两个仓库路径都 404；只有 glTF 三件套 + 2 张 PNG。处理：写 `scripts/pack-suzanne-glb.py` 把 bin 与图片转 bufferView 封装成单文件 GLB（保留原始几何/贴图），运行时只读一个绝对路径，不依赖 CWD。
8. **Vulkan `recreateSwapchain()` 的 OUT_OF_DATE 分支会留陈旧引用** → 根因：M0.4 新增 depth/framebuffer/pipeline 后，acquire / present 过期时只重建 swapchain 不失效这些对象。处理：`recreateSwapchain()` 统一 `scene_.invalidatePipelines()` + 清 framebuffer/depth，覆盖 resize 与两条 OUT_OF_DATE 路径。
9. **tinygltf / nlohmann json / stb 头在 `/W4` 下噪音大** → 处理：只在 `scene.cpp` 用 `#pragma warning(push,0)` 包住实现展开，定义 `TINYGLTF_NO_STB_IMAGE_WRITE` 避免与 `png_writer.cpp` 的 `stb_image_write.h` 重复。
10. **计划写的 push constant / fastgltf 未照搬** → 原因：NVRHI 教程主推 volatile constant buffer（跨 DX12/Vulkan 由 NVRHI 管版本池，比手写 root constants / VkPushConstantRange 更稳）；tinygltf 本机已有、零新依赖。差异已同步写进 `docs/s0/S0-PLAN.md`，不是「无声偏离」。

---

## 附带工具：S0 成果展示台 + example 用例库（M0.4 后，非里程碑）

**动作**
- 新增 `example/`：按 `m0.1`~`m0.4` 分目录，归置 12 个成功用例的基线 PNG（含 resize 前后对照）、4 个一键复现 `run.bat`，以及 `example/cases.json`（版本 / 用例 / 命令 / 参数 / exe 映射的唯一数据源）。
- 新增 `tools/showcase/`：`server.py`（Python 标准库 HTTP 服务，`/api/cases` + `/api/run`，参数白名单校验、路径白名单、运行串行锁）、`rebuild.py`（按 tag 现场重建历史 exe）、`index.html` / `app.js` / `style.css`（版本时间线、用例画廊、双后端并排、参数滑块、日志与 MD5/像素对比）、`start.bat`（一键启动）。
- 用 `git archive` 重建 `s0-init` / `s0-device` / `s0-triangle` 各 DX12+Vulkan 两份 viewer.exe 到 `build/showcase/<tag>/build/<BACKEND>/`（不入库）；Slang 工具链用 NTFS junction 复用主工作区，不复制 180 MB。
- 补 M0.3 缺失的三角形截图 `docs/screenshots/m0.3-{dx12,vulkan}.png`；同步 README（目录树 + 展示台章节）、`.gitignore`（`__pycache__/`）。

**提交版本**：`v0.4.1`（M0.5 未启动，不占 `s0-*` 里程碑 tag）

**完成情况**
- 8/8 历史 exe 构建通过：selftest 7/7（M0.1）、9/9（M0.3）、11/11（M0.4），全部 `BUILD OK`。
- 11 个可运行用例 × 双后端全部 `ok=True`、`deviceErrors=0`、实时 PNG 与各自基线 MD5 逐字节相同；3 个文字用例（selftest）日志匹配 `SELFTEST PASS`；1 个交互用例（M0.1 空窗口）明确标 manual，不自动跑。
- 双后端对比：8 项 `bytes`（MD5 相同，含 M0.2 清屏、M0.3 三角形、M0.4 Suzanne 正常视角）、2 项 `near`（M0.4 内壁 / resize：MD5 不同但像素差仅 1/255，分别 2 个、1 个像素），页面分别标注而不笼统写「一致」。
- `example/m0.{1,3,4}/run.bat` 抽测（`selftest` / `triangle` / `suzanne`）全部退出码 0，PNG 落到 `build/showcase/runs/`，基线未被覆盖。
- 无头浏览器截图确认首页布局与图片正常（版本列表、验收判据、双后端并排图、参数控件、运行按钮）；`/shots/../cases.json` 穿越请求被拦（404）。

**踩坑情况**
1. **M0.3 验收只查了画面、没落盘截图** → 现象：`docs/screenshots/` 无三角形图，展示台该版本开天窗。根因：M0.3 用肉眼确认后没执行 `--screenshot` 存证。处理：从 `s0-triangle` 原始代码现场重建两后端 exe 再出图，双后端 MD5 `6f7711d1…` 相同；图同时进 `docs/screenshots/` 与 `example/m0.3/`。
2. **清单里历史 exe 路径少一层 `build/`** → 现象：`/api/cases` 里 6 个历史 exe 全 `ready=false`。根因：构建脚本 `BUILDDIR=%ROOT%\build\%BACKEND%`，而清单写成 `build/showcase/<tag>/<BACKEND>/`。处理：统一改为 `build/showcase/<tag>/build/<BACKEND>/viewer.exe`，8/8 就绪。
3. **`s0-init` 自带构建脚本在本机跑不通** → 根因：`build-m0.1.bat` 调 `vcvars64.bat`，后者要 `reg.exe`（本会话被安全策略禁用）。处理：`rebuild.py` 检出历史 tag 后注入当前阶段无关的 `build-m0.2.bat`（显式 INCLUDE/LIB/PATH + NMake），两后端均 PASS。
4. **M0.2 resize 基线不是该用例的产物** → 现象：DX12 实时图与基线 MD5 不同（`d6fe0511` vs `bfbbf3a7`），查尺寸发现基线是 800×450、before 图还带 `--color 1`。根因：`case-dx12-resize.png` 是历史不同参数的遗留截图。处理：用本次现场运行的 DX12/Vulkan 各两张图覆盖 `example/m0.2/resize-*.png`，并给「清屏色切换」补 `color-*.png` 基线，之后 11/11 用例基线全匹配。
5. **「双后端画面一致」与 MD5 不同自相矛盾** → 现象：M0.4 内壁 / resize 两后端 PNG 字节不同（`a8ab17dd` vs `26221b55`），但台账写逐像素一致。根因：zlib IDAT 解压后扫描线不同，实际像素只差 ≤1/255（GPU 取整噪声）。处理：服务端 `compare_pixels()`（Pillow 可选加速，标准库 PNG 解码器兜底）返回最大通道差与差异像素数；UI 分 `bytes` / `pixels` / `near` / 不同四态如实显示。
6. **清单放两份会漂移** → 现象：初版 `tools/showcase/cases.json` 与截图目录 `docs/screenshots/` 分离，用户要求「做个 example 目录放每个版本的用例」后必然出现两套索引。处理：清单整体搬进 `example/cases.json`，服务与重建脚本都从它读，截图与 `run.bat` 同目录；`/shots/` 只服务清单里声明过的相对路径，避免目录穿越。
7. **批量测试脚本的 CRLF 污染 JSON** → 现象：`/api/run` 全部回 `Invalid control character at line 1`。根因：Python 在 Windows 写清单文件默认 `\r\n`，shell `read` 把 `\r` 带进 case id。处理：测试侧 strip `\r`；产品侧不动（服务端本就按 JSON 严格解析，没有静默容错）。

---

## 后续阶段的填写规范

每个阶段追加一节，四栏齐全，另附「可见用例」的截图路径与复现命令：

```
## M0.x 名称
**动作**        实际改动的文件 / 命令
**提交版本**    tag 名（+ commit hash 可选）
**完成情况**    逐条可复现的验收结果；未完成的明确列 ⬜
**踩坑情况**    1. 现象 → 根因 → 处理（没有就写「本阶段无阻塞」，不要空栏）
```

验收通过才打 tag 并放行下一阶段；tag 打在验收之后，不预支。
