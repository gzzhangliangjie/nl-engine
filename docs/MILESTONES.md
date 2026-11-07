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
| M0.4 模型 + 贴图 + 相机 | ✅ 验收通过 | `s0-model` | 2026-11-07 | 正常视角双后端 PNG 逐字节相同，内壁 / resize 仅 1~2 像素差（≤1/255）；内壁剔除对照、resize 回归、selftest 11/11、deviceErrors=0 |
| M0.5 ImGui + 总验收 | ✅ 验收通过 | `s0-done` | 2026-11-07 | 双后端 ImGui HUD 实拍 + `--no-hud` 与 M0.4 基线逐字节相同；`model`/`hud` 共 8 个 shader 产物魔数自检、selftest 14/14、`--check` 退出 0；S0 全部完成 |
| M0.6 GPU 帧内计时 | ✅ 验收通过 | `m0.6` | 2026-11-07 | NVRHI timestamp query 真实 GPU 耗时进 HUD（CPU/GPU 并排 + 双曲线）；`--no-hud` 基线逐字节不变；S0 后能力补齐序列第一版 |
| M0.7 高模测试资产 + `--model` | ✅ 验收通过 | `m0.7` | 2026-11-07 | gen-dense-mesh.py 程序化致密网格（纽结 204,800 / 球 327,680 tri，MD5 确定）；`--model` 白名单加载 + HUD model 行；GPU ms 3,936→327,680 tri 双后端互证；meshoptimizer v1.3 vendored（不入构建）；S1 切簇命门就位 |
| M4.0 GPU 簇数据通路 + vertex pulling | ✅ 验收通过 | `s4-indirect` | 2026-11-08 | 单 drawIndexedIndirect 全簇全画（cull 模式 vertex pulling）；`--mode cull` 与 `--mode cluster` 同机位同 shading 逐字节相同（DX12/Vulkan 各自一致），deviceErrors=0；kStage=S4.0；交代物 example/s4-indirect + cases.json s4.0 卡 |

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
- ✅ **双后端画面一致（分档如实记）**：正常视角 `m0.4-dx12.png` 与 `m0.4-vulkan.png` **MD5 完全相同**（`7e528087d19722d8f9ef4de35e36d382`）；内壁 / resize 截图 PNG 字节不同（zlib 编码差异），像素级比对为 **2 个 / 1 个像素相差 1/255**（GPU 取整噪声），不是「全图逐像素一致」——精确结论与处理见下方「附带工具」节踩坑 5。
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
10. **计划写的 push constant / fastgltf 未照搬** → 原因：NVRHI 教程主推 volatile constant buffer（跨 DX12/Vulkan 由 NVRHI 管版本池，比手写 root constants / VkPushConstantRange 更稳）；tinygltf 本机已有、零新依赖。差异已同步写进 `docs/s0/S0-PLAN.md`，不是「无声偏离」。补充一条：`GLM → 未引入`，M0.4 的 4×4 透视 / lookAt 由 `core/camera.cpp` 手写并配 `cameraSelfTest`，当天一次过。

**文档复核补记（2026-11-07，同日收口）**：M0.4 验收后回扫全部文档，发现 5 处旧表述 / 空头支票，已一并清理——
1. `docs/s1/S1-PLAN.md` 让 S1「复用 M0.4 用过的 **fastgltf**」→ 实际 M0.4 用 **tinygltf**，照它做 S1 一开工就找错库。改为指向 `renderer/src/scene.cpp` 的 `core/mesh_io` 抽取方案，并注明实现宏只能在一个 TU 展开。
2. `docs/s0/CARROT-BOOTSTRAP.md` 两处写「M0.4 用 glm」/「`thirdparty/glm/` 用于 M0.4 相机」→ 实际未引入 GLM，改成手写矩阵 + selftest 的实况；`docs/ROADMAP.md` S0 骨架的「数学(GLM 封装)」同步标明实况，并补当前进度行。
3. 台账速查表 / 本节「完成情况」/ S0-PLAN 自检 / `example/cases.json` 的「逐像素一致」「RGBA 平均绝对差 0」→ 统一为分档结论：正常视角逐字节相同，内壁 / resize 为 2 个 / 1 个像素相差 1/255。
4. S0-PLAN 门禁表截图名 `m0.1.png` / `m0.3.png` / `m0.4.png` 与实际落盘文件不符 → 改真实路径；M0.1 明写「交互用例、无截图」，M0.3 注明由 `s0-triangle` 重建补出，M0.1/M0.3/M0.4 完成行补 ✅ 与日期。
5. S0-PLAN「测试模型」依赖表仍写「Blender 导出，或随便一个 glTF 样例」→ 改成 Khronos 官方三件套 + `scripts/pack-suzanne-glb.py` + `NL_ASSET_DIR` 的实况，并给出复现命令。

> 教训：**验收结论一旦被更精确的测量修正，要回扫全仓文档**（含后续阶段的计划文档、UI 展示文案），不能只改台账正文。本次最危险的残留不在 M0.4 章节里，而在 S1 的开工清单里。

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
5. **「双后端画面一致」与 MD5 不同自相矛盾** → 现象：M0.4 内壁 / resize 两后端 PNG 字节不同（`a8ab17dd` vs `26221b55`），但台账写逐像素一致。根因：zlib IDAT 解压后扫描线不同，实际像素只差 ≤1/255（GPU 取整噪声）。处理：服务端 `compare_pixels()`（Pillow 可选加速，标准库 PNG 解码器兜底）返回最大通道差与差异像素数；UI 分 `bytes` / `pixels` / `near` / 不同四态如实显示；台账速查表、本节完成情况、S0-PLAN、`example/cases.json` 里的旧表述同步清理（见上方「文档复核补记」）。
6. **清单放两份会漂移** → 现象：初版 `tools/showcase/cases.json` 与截图目录 `docs/screenshots/` 分离，用户要求「做个 example 目录放每个版本的用例」后必然出现两套索引。处理：清单整体搬进 `example/cases.json`，服务与重建脚本都从它读，截图与 `run.bat` 同目录；`/shots/` 只服务清单里声明过的相对路径，避免目录穿越。
7. **批量测试脚本的 CRLF 污染 JSON** → 现象：`/api/run` 全部回 `Invalid control character at line 1`。根因：Python 在 Windows 写清单文件默认 `\r\n`，shell `read` 把 `\r` 带进 case id。处理：测试侧 strip `\r`；产品侧不动（服务端本就按 JSON 严格解析，没有静默容错）。

---

## M0.5 ImGui HUD + `--mode` 分发器 + S0 总验收（Slang 双后端）

**动作**
- `thirdparty/imgui/`：vendored **ocornut/imgui v1.91.9 主线稳定版**核心 4 个 cpp + 5 个头 + LICENSE（经 gh-proxy 取）。**不用官方 D3D12 / Vulkan backend**——那会把图形 API 散到渲染器之外；改为自写一份 NVRHI 共享渲染后端 `renderer/src/hud.cpp/.hpp`。
- `renderer/shaders/hud.slang`：单源双目标，与 `model.slang` 同样由 `slangc` 出 `hud.{vs,ps}.{spv,dxil}`；`HudConstants : register(b0)` 存投影、`g_font : register(t0)` 字体图集；沿用三个 `-fvk-*-shift`，SPIR-V 入口 `main`、DXIL `vsMain/psMain`。
- `renderer/src/hud.cpp`：进程级 ImGui context（关掉 ini/log 落盘）→ 字体图集上传 → binding layout/set / input layout / 两套资源（VB/IB 动态扩容）→ `ensurePipeline()`（cull none、scissor、depth off、`SrcAlpha+InvSrcAlpha`）→ `build()` 把 `ImDrawData` 摊平成 `HudVertex`(32 B) / `R32_UINT` / 逐 `DrawCmd{scissor}` → `draw()` 逐条 `setGraphicsState()` + `drawIndexed()`。
- `renderer/include/nl/renderer/renderer.hpp`：新增双向 POD `HudFrame`（viewer 填显示数据、HUD 写回 `cullBack` / `colorIndex` / `resetCamera` / `wantCaptureMouse`），`renderFrame()` 加 `HudFrame* hud = nullptr`；`hud == nullptr` 时完全不碰 ImGui，保证 M0.1~M0.4 基线不变。`core/` / `viewer/` 仍是零 ImGui / NVRHI / 图形 API 符号。
- 两后端对称接线：`init` 时 `hud_.init(device, "dxil"/"spv", ...)`，resize / OUT_OF_DATE 时 `invalidatePipeline()`，`scene_.draw()` 之后、截图回读之前 `hud_.draw()`，shutdown 在 device 存活时 `hud_.shutdown()`。
- `core/include/nl/core/modes.hpp` + `core/src/modes.cpp`：`ViewMode` 枚举 / `modeName` / `modeNameAt` / `modeCount` / `parseViewMode` / `modeSelfTest`；`viewer/main.cpp` 的 `runMode()` 分发，目前只有 `normal`，S1 起只加枚举 + 名字 + case。`core/window.*` 把「绝对鼠标坐标」与「拖动增量基准」分开，供 ImGui 命中控件。
- `viewer/main.cpp`：`--hud` / `--no-hud` / `--mode <name>`；交互窗口默认开 HUD，截图回归默认 `--no-hud`；回归模式用 8 个固定帧时间样本并标注 `capture/replay (fixed samples)`，交互窗口用真实 120 帧历史；selftest 由 11 项扩到 14 项（加 `modeSelfTest`、M0.5 stage、`hudSelfTest`、`shaderArtifactsSelfTest`）。
- `renderer/src/renderer.cpp`：`kStage` → M0.5（展示台门禁的阶段判定源）；新增 `shaderArtifactsSelfTest()` 遍历 `model`/`hud` × `vs`/`ps` × `spv`/`dxil` 共 8 个产物，核对 `.spv` = `0x07230203`、`.dxil` = `DXBC`。
- `CMakeLists.txt`：`nl_imgui` 静态库、`modes.cpp` / `hud.cpp` 入 target、ctest 名改 `m0.5-selftest`；`scripts/build-m0.2.bat` 输出去掉写死的阶段名。
- 文档：`docs/IMGUI-GUIDE.md`（新建，含下拉框 / 选择菜单）、`docs/SHADER-CONVENTIONS.md`（hud.slang / binding / 8 产物 / 验收命令）、`docs/s0/S0-PLAN.md` + `ROADMAP.md` + `PLAN.md` + `LINKS.md` + `kb/index.html` + `s4/S4-PLAN.md`（工具计划改为 L0~L3 分层）、`README.md`（S0 done + 新命令）。
- 用例归位：`example/m0.5/`（`hud` / `controls` / `nohud` / `resize` / `selftest` / `hud-controls(manual)` + 10 张基线 PNG + `run.bat`），`example/cases.json` 增 `m0.5` 版本；`docs/screenshots/` 增 `m0.5-*.png` 与 `s0.png`。收尾复核时补的 `controls` 用例用 `--color 2` + `--cull back` 给出「HUD 控件值 → 清屏色 / 画面」的静态对照，补上交互用例在展示台里看不到图证据的缺口。

**提交版本**：`s0-done`（S0 收口里程碑 tag；精确 hash 用 `git rev-parse s0-done` 查）。

**完成情况**
- ✅ 两后端 `scripts/build-m0.2.bat DX12|VULKAN`：configure 0 / build 0 / `SELFTEST PASS (14/14)`；`ctest` 两后端 `m0.5-selftest` 100% 通过；8 个 shader 产物魔数全部匹配（`[8/8 ok]`）。
- ✅ **可见用例**：交互窗口 Suzanne 上叠 HUD，FPS / 帧时间曲线 / yaw·pitch·dist / backend + adapter + debug layer / mode / stage 齐全；`back-face culling` / `clear color` / `reset camera` 三个控件可点，值经 `HudFrame` 每帧回写 viewer；960×540 实拍 `docs/screenshots/s0.png`、`m0.5-hud-{dx12,vulkan}.png`。
- ✅ **历史基线不动**：`--no-hud` 正常视角 DX12 / Vulkan PNG MD5 同为 `7e528087d19722d8f9ef4de35e36d382`，与 M0.4 逐字节相同；`--hud` 两图必然不同（面板印的后端名 / 调试层文本不同），已写进清单 note，不谎报「一致」。
- ✅ resize 640×360 → 1024×768 前后非黑、两后端各一份对照；6 个渲染进程退出码 0、`deviceErrors=0`，DX12 Debug Layer + NVRHI validation（两后端）零报错。
- ✅ `--mode bogus` 退出 1 并打印 `unknown --mode 'bogus'. available: normal`；`--mode normal` 正常运行。
- ✅ 放行门禁：`python tools/showcase/server.py --check` → `[example-sync] OK: stage=M0.5 version=m0.5 cases=5 baselines=10`，退出码 0。
- ✅ 展示台服务重载新清单后 `/api/cases` 返回 `m0.1~m0.5` 且 `stageSync.ok=True`；`/api/run` 现场回放 4 个用例：`hud`（双后端 exit 0、baselineMatch True、`identical=False` 为预期）、`nohud`（`identical=bytes`）、`resize`（baselineMatch True）、`selftest`（日志匹配 `SELFTEST PASS (14/14)`），全部 `ok=True`。
- ⬜ 图形 / 性能调试工具抓帧：已从 S0 验收项移出，本机 RenderDoc / Nsight / PIX 均未安装，如实记未做，不虚构结果；L1~L3 计划见 `docs/s0/S0-PLAN.md`。
- ⬜ L2 GPU 帧内计时（NVRHI timestamp query）：M0.5 时挂账，**已由 M0.6 补掉**（见下一节）。

---

## M0.6 GPU 帧内计时（NVRHI timestamp query）

**动作**
- `renderer/src/gpu_timer.hpp`（新建）：两后端共享的 `GpuTimer`——4 个 slot 环形轮转的 `nvrhi::ITimerQuery`；`ensure()` 延迟到第一次需要 HUD 的帧才建 query（`--no-hud` 零调用）；`poll()` 只取已 resolve 的结果、绝不阻塞；`begin()/end()` 包住整帧 GPU 命令；无空闲 slot 时本帧不计时、不报错；`selfTest()` 锁秒→ms 换算与游标回绕。
- `HudFrame` 扩展：`gpuMs` / `gpuMsHistory[120]` / `gpuMsCount` / `gpuTimingAvailable`；HUD 在 CPU `frame ms` 旁新增 `GPU x.xx ms (NVRHI timer)` 行 + `gpu ms` 曲线，面板高度上限 400→460、两条曲线压到 44/34 px，960×540 下整块面板不裁底。
- 两后端对称接线：`renderFrame` HUD 分支里 `ensure()` + `poll()`（**deterministic 时不写回真实计时**，viewer 已填固定样本保 MD5 稳定）；`cmd->open()` 后 `begin()`、`close()` 前 `end()`；shutdown 在 device 存活时释放。首次成功 resolve 落一条 `gpu timer resolved` 日志，作为交互模式的机械证据。
- `viewer/main.cpp`：截图回归模式填 8 个固定 GPU 样本（与 CPU 样本独立）；交互模式维护 120 帧 GPU 历史（renderFrame 返回后追加写回值）。
- `kStage` → `M0.6 GPU frame timing (NVRHI timestamp query)`；selftest 的 stage 校验同步改 M0.6。

**提交版本**：`m0.6`（S0 后能力补齐序列第一版；精确 hash 用 `git rev-parse m0.6` 查）。

**完成情况**
- ✅ 两后端 build + `SELFTEST PASS (14/14)`（`GpuTimer::selfTest` 并入 HUD selftest 项）+ `ctest` 100%。
- ✅ 真实 GPU 计时证据：交互模式首次 resolve 日志 DX12 `0.0369 ms`、Vulkan `0.0380 ms`，量级合理（小场景）；渲染进程 `deviceErrors=0`。
- ✅ HUD 实拍（`docs/screenshots/m0.6-hud-{dx12,vulkan}.png`）：CPU/GPU 并排 + 双曲线 + `capture/replay (fixed samples)` 标注（GPU 固定 0.63 ms），面板完整不裁底。
- ✅ **历史基线不动**：`--no-hud` 两后端 PNG MD5 同为 `7e528087d19722d8f9ef4de35e36d382`，与 M0.4 / M0.5 逐字节相同。
- ✅ 用例归位：`example/m0.6/`（`hud` / `nohud` / `selftest` 3 用例 + 4 张基线 + `run.bat`），`example/cases.json` 增 `m0.6`；`--check` → `stage=M0.6 version=m0.6 cases=3 baselines=4` 退出 0；展示台 `/api/run` 三用例回放全 `ok=True`（hud 双后端 `baselineMatch=True`、nohud `identical=bytes`）。
- ✅ S0 遗留 checklist 修正：M0.3 四条（triangle.slang 双目标 / frame-in-flight=2 / render pass / 两后端三角形）与 `slangc -h` 实际早已完成，checklist 由 `[ ]` 改 `[x]`。

**踩坑情况**
1. **基线参数与用例参数不一致 → 展示台回放 `baselineMatch=False`** → 现象：16.9 万像素差异、bbox 覆盖模型区，初看像渲染坏了。根因：基线用默认参数生成（pitch 9.2），cases.json 用例是 `--pitch 9`，pitch 一度之差就是全图级差异。处理：按用例参数重出基线；教训写进 `docs/s0/M0.6-PLAN.md`——**基线必须用与用例完全相同的参数生成**。
2. **加 GPU 行后面板内容超出 400px 上限，底部帮助文字被滚动条裁掉** → 处理：上限提到 460、两条 PlotLines 压到 44/34 px，960×540 实拍确认整块面板完整。
3. **DX12 `end()` 首次编辑误删 `executeCommandList` 与 fence 注释** → 下一次 Edit 立即发现补回，构建后逐段核对。
4. **selftest「M0.5 stage reported」写死阶段名** → `kStage` 改 M0.6 后该项失败（13/14）。处理：stage 校验与窗口标题同步改 M0.6；阶段名校验本来就该跟 `kStage` 走。

**用例归位**
- `example/m0.6/`：`hud-{dx12,vulkan}.png` / `nohud-{dx12,vulkan}.png` 共 4 张基线 + `run.bat`（`all` / `hud` / `nohud` / `selftest`）。
- `example/cases.json` 增 `m0.6` 版本：3 个可运行用例；验收条目含两后端首次 resolve 实测值与 MD5 parity 记录。
- 后续版本序列（用户已确认）：M0.7 = 高模测试资产生成器（S1 切簇的命门，Suzanne 504 tri 无法演示切簇）→ M0.8 = 线框 / 调试着色 → M0.9 = 一键全量回归总闸；vendored meshoptimizer 作对照基准随 M0.7 一起（文档级参考，不入构建）。Carrot 实况：meshoptimizer + meshlet 是 2023-11-25 同一个大提交进的（`4c100761`），我们拆版本铺路更稳；它没有截图回归总闸，M0.9 是超它的部分。

**踩坑情况**
1. **本机 Carrot 的 ImGui 是 `1.92.7 WIP`** → 现象：初拷自 `Carrot/thirdparty/imgui/` 的源码字体系统已重构，与 1.91 代 API 不同代，风险高。根因：Carrot checkout 的是 WIP 分支，不是稳定版。处理：删掉初拷文件，改下 `v1.91.9` 主线稳定版，只 vendored 核心 4 个 cpp（tables/widgets 都要，否则下拉框 / 表格缺符号）。
2. **`/W4` 报 `C4127: 条件表达式是常量`** → 根因：`hudSelfTest()` 里直接拿 `sizeof(HudVertex)` 做运行时比较，编译期已知。处理：`volatile std::size_t` 中转，同时保留 `hud.hpp` 的 `static_assert` 兜底。
3. **`C2445` / `C2737`：`tellg()` 与 `int -1` 条件表达式类型歧义** → 根因：`std::ifstream::tellg()` 返回 `std::fpos<_Mbstatet>`，不是整型。处理：`std::int64_t size = -1; if (f) size = static_cast<std::int64_t>(f.tellg());`。
4. **DX12 首张 HUD 截图 `Device Removed!` / `Present failed (0x887a0005)` / `deviceErrors=3`** → 现象：日志先行报 `Unknown prior state of buffer ImGuiVB/ImGuiIB`。根因：动态 VB/IB 每帧 `writeBuffer()` 后留下 VertexBuffer / IndexBuffer 状态，NVRHI validation 下一帧不知道 prior state。处理：`BufferDesc` 加 `initialState = CopyDest` + `keepInitialState = true`，让每条 command list 自动从 `CopyDest` 起跟踪、close 时转回。
5. **第一张 HUD 截图文字上下颠倒 / 镜像** → 根因：`hudProjection()` 的 Y 尺度写成 `+2/height`；ImGui y 向下、NDC y 向上。处理：改 `sy = -2/height`、`projection[3] = 1 - displayPosY * sy`，零尺寸分支 `projection[3] = 1`；`hudSelfTest()` 锁住负 Y 与平移项。用 `ViewImage` 看实拍才发现的，纯日志测不出来。
6. **640×360 下 HUD 面板撑到半屏、底部控件被裁** → 根因：stage / adapter / debug 长字符串撑宽窗口，ImGui 自动尺寸。处理：固定 `SetNextWindowPos(14,14)` + `SetNextWindowSize(330, min(400, H-28))` + `TextWrapped` + `SetNextWindowBgAlpha(0.82f)`（模型仍可见）；960×540 实拍确认全部行完整可读。
7. **Vulkan HUD 的 `debug` 行显示 `?????`** → 根因：ImGui 默认字体图集只有 ASCII，而该字符串含中文。处理：把 Vulkan 后端的调试层说明改成 ASCII 字符串，不把后端分支泄漏到 viewer；中文字体作为后续增量写进 `docs/IMGUI-GUIDE.md`。
8. **实时 FPS 写进 PNG 会让基线 MD5 永远对不上** → 根因：截图回归要求确定性，而交互数据逐帧变化。处理：回归模式用 8 个固定帧时间样本并在面板标注 `capture/replay (fixed samples)`；交互窗口显示真实采样；`--screenshot` 默认 `--no-hud`，历史基线一个字节都不动。
9. **展示台 `/api/cases` 只有 m0.1~m0.4、`stageSync.ok=False`** → 根因：旧服务进程仍持有启动时读入的 `cases.json`，文件改了但进程没重载。处理：查 8787 端口 listener、停掉旧 PID、重启服务后再验。
10. **「复杂控件」回答过宽，被用户一句「我说的界面控件指的是 选择菜单 下拉框之类的」纠偏** → 根因：把问题理解成「编辑器级 UI / 复杂空间」，铺开了表格 / 树 / docking。处理：收窄到 `Combo` / `BeginCombo` / `ListBox` / `Selectable` / `RadioButton` / `MenuBar` / `TreeNode` 均在已 vendored 的 `imgui_widgets.cpp`、当前鼠标输入就够用；弹出层的多 `ImDrawList` 与 ClipRect 已被 `build()` / `draw()` 覆盖；只有 `InputText` 需要补字符回调。结论单独写进 `docs/IMGUI-GUIDE.md` §2 / §3。
11. **「Nsight 抓一帧」从 S0 验收项移出** → 起因：用户连续问 RenderDoc / 性能调试工具 / 更新计划。根因：原计划把单一厂商工具写死进通用管线的验收，且本机根本没装。处理：改 L0 画面证据 / L1 RenderDoc 帧结构 / L2 NVRHI timestamp query / L3 Nsight·PIX·PresentMon·Tracy 四层，同步 6 份文档，S0 三条硬指标不依赖任何外部工具；台账里如实留 ⬜，不虚构抓帧结果。
12. **resize 运行时 `libpng warning: iCCP: known incorrect sRGB profile`** → 现象：stb 写出的 PNG 带 sRGB 块被 libpng 提示，但进程 exit 0、`deviceErrors=0`、图片正常。处理：非阻塞项，不为此改写 PNG 编码链；如实记录，后续真要做色彩管理再一起处理。

**用例归位**
- `example/m0.5/`：`hud-{dx12,vulkan}.png` / `controls-{dx12,vulkan}.png` / `nohud-{dx12,vulkan}.png` / `resize-{dx12,vulkan}{,-before}.png` 共 10 张基线 + `run.bat`（`all` / `hud` / `controls` / `nohud` / `resize` / `selftest`）；验收存档同步进 `docs/screenshots/m0.5-controls-*.png`。
- `example/cases.json` 增 `m0.5` 版本：5 个可运行用例 + 1 个交互用例 `hud-controls`（manual）。收尾复核补的 `controls` 用例给出了「控件值 → 画面」的静态图证据：`--color 2 --cull back --dist 4.2 --hud` 出绿背景 + 完整 Suzanne 正脸，HUD 的 `clear color` 显示 2、剔除勾选；与 `hud` 用例（深蓝、color=0）对照可见同一 `HudFrame` 同时驱动控件与渲染。
- `python tools/showcase/server.py --check` 退出 0（`stage=M0.5 version=m0.5 cases=5 baselines=10`）；重启展示台服务加载新清单后 `/api/run` 五个可运行用例现场回放全 `ok=True`。`controls` 两后端 `baselineMatch=True`、`deviceErrors=0`，895 个差异像素的 bbox 为 `x=23..328 / y=77..147`（全在 adapter / debug 两行文字内，3D 区域零差异），印证「两图不同是 HUD 文本不同」。

---

## M0.7 高模测试资产生成器 + `--model`（S1 切簇的命门）

**动作**
- 新增 `scripts/gen-dense-mesh.py`：纯 stdlib（argparse/json/struct/zlib/math）程序化生成致密网格 GLB——`--shape torusknot`（(2,3) 纽结管，Frenet 近似 frame，默认 512×200 = 204,800 tri）/ `--shape icosphere`（中点缓存细分，20×4^7 = 327,680 tri，等距柱状 UV）；uint32 索引；内嵌手写 PNG 编码器出的 256px 棋盘格贴图；GLB 封装复用 pack-suzanne-glb.py 的 bufferView 套路。产物 `assets/models/{TorusKnotDense,IcoSphereDense}.glb`（5.8 / 9.2 MB）。
- `--model` 贯通：`viewer/main.cpp` 解析 → `Renderer::create(..., modelFile)`（白名单校验：ASCII 可打印、无路径分隔符、无 `..`、必须 .glb）→ `Backend::init` → `Scene::init`；`scene.cpp` 模型路径 / VB·IB·贴图 debug 名 / 日志全部参数化；`DeviceInfo` 增 `modelName` / `triangleCount`，`HudFrame` 增 `model` / `triangleCount`，HUD 面板增 `model : <名> (N tri)` 行（面板上限 460→480）。
- vendored `thirdparty/meshoptimizer`（gh-proxy 浅克隆 v1.3/1030，去 .git，2.8 MB）：S1 切簇正确性对照基准，**文档级、不入构建**。
- `kStage` → `M0.7 dense test asset generator (--model)`；selftest 阶段校验同步改 M0.7。
- `example/m0.7/` 归位 + `cases.json` 增 m0.7（hud / icosphere / selftest）；m0.6 exe 映射改为 tag 快照（`build/showcase/m0.6/...`），`rebuild.py` 补建两后端快照 exe。

**提交版本**    tag `m0.7`

**完成情况**
- 确定性：同参数两次生成 GLB MD5 相同（TorusKnotDense `130c66f2…`、IcoSphereDense `7edaa6a7…`）；生成过程零网络、纯 stdlib。
- 三模型 × 双后端真实 GPU ms（交互模式 `gpu timer resolved` 首帧日志）：Suzanne 3,936 tri = 0.032/0.039 ms → 纽结 204,800 tri = 0.066/0.065 ms → 球 327,680 tri = 0.154/0.153 ms（DX12/Vulkan 互相印证，M0.6 计时第一次派上用场）。
- `--model` 双后端截图成功、`deviceErrors=0`；HUD 自证 `model : TorusKnotDense.glb (204800 tri)`。
- **回归**：不带 `--model` 时两后端 `--no-hud` PNG MD5 仍为 `7e528087…`，与 m0.4/m0.5/m0.6 基线逐字节相同；selftest 两后端 14/14。
- 展示台：`--check` 退出 0（`stage=M0.7 version=m0.7 cases=3 baselines=4`）；`/api/run` 现场回放 m0.7 三用例 + m0.6 快照 exe 两用例，全 `ok=True`、`baselineMatch=true`、`deviceErrors=0`。
- `thirdparty/meshoptimizer` 在位，CMake 零引用。

**踩坑情况**
1. **uv 打包 `struct.error: required argument is not a float`** → 根因：生成器里 UV 存的是 `(u,v)` 元组列表，`struct.pack` 不接受。处理：pack 前摊平（`[c for pair in uvs for c in pair]`），accessor count 同步用摊平长度。
2. **截图模式抓不到 `gpu timer resolved`** → 根因：`gpuTimer_.ensure()` 只在 HUD 帧里调用（`--screenshot` 默认 `--no-hud` 则完全不建 query），且 deterministic 模式跳过 `poll()`（不写回真实计时保基线）。处理：GPU ms 对比改用限时交互窗口（`timeout 7 viewer.exe`）取首帧 resolve 日志——与 M0.6 的取数口径一致。
3. **台账旧口径「Suzanne 504 tri」失实** → 根因：早年从会话记录传抄的错误数字（504 其实约是顶点相关的另一口径）。实测日志：11,808 索引 = 3,936 tri。处理：`M0.7-PLAN.md` 已改正为 3,936 tri。
4. **展示台服务用 shell `&` 起后台会被沙箱随命令结束回收**（连 curl 一起死）→ 处理：改用宿主后台任务方式常驻服务，回放完再停。

**用例归位**
- `example/m0.7/`：`hud-{dx12,vulkan}.png`（960x540，纽结 + model 行）、`icosphere-{dx12,vulkan}.png`（640x360，无 HUD）+ `run.bat`（`all` / `hud` / `icosphere` / `selftest`）。
- `example/cases.json` 增 `m0.7`：`hud`（dist 可调，默认 5.5）/ `icosphere` / `selftest` 三用例；m0.1~m0.6 用例原样保留。
- `python tools/showcase/server.py --check` 退出 0。

---

## 后续阶段的填写规范

每个阶段追加一节，四栏齐全，另附「可见用例」的截图路径与复现命令：

```
## M0.x 名称
**动作**        实际改动的文件 / 命令
**提交版本**    tag 名（+ commit hash 可选）
**完成情况**    逐条可复现的验收结果；未完成的明确列 ⬜
**踩坑情况**    1. 现象 → 根因 → 处理（没有就写「本阶段无阻塞」，不要空栏）
**用例归位**    example/<版本>/ 的基线 PNG + run.bat，并在 example/cases.json 登记；
                python tools/showcase/server.py --check 退出码必须为 0
```

验收通过才打 tag 并放行下一阶段；tag 打在验收之后，不预支。

> **每轮迭代都要更新 `example/`（2026-11-07 定的硬规则）**：本轮产出的成功用例必须归位到 `example/`，否则展示台就看不到新阶段的效果，等于这轮没留下「能亲眼看到」的证据。`--check` 以 `renderer.cpp` 的 `kStage` 判定当前阶段，代码进了新阶段而 `example/` 没跟上就退出 1；**验收 / 打 tag 前必须过这一关**。S1~S14 的每一步同样适用。

## M0.8 线框 / 调试着色（normal / UV / 三角形ID）

**动作**
- `renderer/shaders/model.slang`：`g_options.x` 加 4 种着色分支（0 textured = M0.4 原路径逐字不动 / 1 世界空间法线 / 2 uv→红绿 / 3 三角形ID）。PS 输入结构新增 `uint triId : SV_PrimitiveID`（rasterizer 系统值，VS 输出不带它），颜色用 pcg 风格整数 hash——无浮点随机源，同参重渲染确定。**开工前先用临时 shader 实测 slangc 双目标编译**：SPIR-V `03 02 23 07` / DXIL `DXBC` 都过。
- `renderer/src/scene.{hpp,cpp}`：pipeline 从 2 套扩到 **4 套（solid/wire × cull back/none）**，线框用 NVRHI `RasterState::setFillWireframe()`（光栅 fill mode，零几何 shader、零索引改动）；`draw()` 把 `params.shading` 写进 `cb.options[0]`，按 `wireframe/cullBack` 选 pipeline。
- `renderer/include/nl/renderer/renderer.hpp`：`FrameParams` 增 `int shading = 0; bool wireframe = false;`，`HudFrame` 增同名双向字段（缺省值 = M0.7 行为）。
- `viewer/main.cpp`：`--shading textured|normal|uv|tri-id`（也接受 0..3，非法值报错退出 1）、`--wireframe`；**V = 线框开关、N = 循环切着色**；标题栏与启动日志带 shading/wire 状态；selftest 增 POD 缺省校验（14→15 项）。
- `core/include/nl/core/window.hpp` + `core/src/window.cpp`：Key 枚举加 V / N（core 依旧零图形符号）。
- `renderer/src/hud.cpp`：面板增 `shading` 下拉框（imgui_widgets 的 Combo）+ `wireframe` 复选框（与 cull 复选框并行一行，省高度）；面板上限 480→510。
- `kStage` → `M0.8 debug shading (wireframe / normal / uv / tri-id)`；selftest 阶段校验同步改 M0.8。
- `example/m0.8/` 归位 4 个 pair 用例 + selftest；`cases.json` 增 m0.8；m0.7 的 exe 映射改为 tag 快照（`build/showcase/m0.7/...`），`rebuild.py` 补建两后端快照 exe。

**提交版本**    tag `m0.8`

**完成情况**
- 双后端构建 + selftest 15/15（stage 校验 M0.8、POD 缺省值校验、8 个 shader 产物魔数照旧全过）。
- 四种着色 + 线框双后端截图全部成功，`deviceErrors=0`、调试层零输出；tri-id 与 uv 两后端 PNG **逐字节相同**（MD5 `1c1421ea…` / `128b51c9…`），hud / wire 用例两后端不同仅因面板文本（老约定）。
- **历史基线逐字节不变**：不带新参数时 Suzanne `--no-hud` 双后端 PNG MD5 仍为 `7e528087d19722d8f9ef4de35e36d382`。
- `SV_PrimitiveID` 双后端实跑通过：NVRHI validation 与 D3D12 Debug Layer 均零报错（预判的签名匹配坑没有出现）。
- HUD 960×540 实拍：shading 下拉框 + wireframe 复选框可见、面板底部操作提示行完整不裁底。
- `python tools/showcase/server.py --check` 退出 0（stage=M0.8，5 用例 / 8 基线）。
- ⬜ 交互窗口里 V/N 键的现场演示为交互用例（无截图，按 M0.5 以来的约定不单独落盘）。

**踩坑情况**
1. **`--shading tris` 静默落回 textured** → 根因：规范名是 `tri-id`（HUD combo 同名），首版解析对未匹配文本走 `atoi("tris")=0`，静默当成 textured，截图与基线 MD5 相同才暴露。处理：解析改为「名字精确匹配 + 严格数字 0..3」，其余一律报错退出 1；测试命令也改用规范名。
2. **slangc 的 `-dxc-path` 在 bash 里传带空格路径会碎** → 根因：Git Bash 对参数再分词/转义，dxcompiler.dll 路径被切碎报 `failed to load downstream compiler 'dxc'`。处理：用单引号反斜杠 Windows 路径直传（CMake 构建内不受影响，只是手工验证时的坑）。

**用例归位**
- `example/m0.8/`：`hud-{dx12,vulkan}.png`（960×540，纽结法线着色 + 新控件）、`wire-{dx12,vulkan}.png`（960×540，Suzanne 线框 + 复选框勾选）、`uv-{dx12,vulkan}.png`（640×360，细分球 UV）、`tris-{dx12,vulkan}.png`（640×360，Suzanne 三角形ID）+ `run.bat`（`all` / `hud` / `wire` / `uv` / `tris` / `selftest`）。
- `example/cases.json` 增 m0.8：`hud`（dist 可调）/ `wire` / `uv` / `tris` / `selftest` 五用例；m0.1~m0.7 原样保留。
- `python tools/showcase/server.py --check` 退出 0。

## M0.9 一键全量回归总闸（tools/regression.py）

**动作**
- `tools/regression.py`（新增）：读 `example/cases.json`，对全部版本的全部可自动用例 × 双后端现场重跑——历史里程碑 exe 走 tag 快照（`build/showcase/<tag>/`），当前版走 `build/<backend>/`。**复用 `tools/showcase/server.py` 的 `exe_for / validated_params / build_argv / baseline_for / before_baseline_for / compare_pixels / md5`**（import 同目录模块），cases.json 只有一份解析器。判据：PNG 与基线 MD5 相同 = `bytes`；不同但像素差 ≤1/255 且 ≤2 像素 = `near`（GPU 取整噪声档，与展示台同约定）；selftest 校验 `expectLog`；每次运行校验 `deviceErrors=0`；exe 缺失 = FAIL 并提示 `rebuild.py --tags <tag>`；resize 类用例的 `.before.png` 与 `beforeBaselines` 同样比对。参数取 `validated_params(case, {})` 默认值（与基线生成同参）。报告落 `build/regression/report.{md,json}`，失败 exit 1。
- `example/cases.json`：m0.8 腾位（`tag: null` → `m0.8`，exe 改 `build/showcase/{tag}/…`，`rebuild.py --tags m0.8` 补建两后端快照）；新增 m0.9 版本：`gate-hud`（**线框 × tri-id × HUD 组合冒烟**，此前从未同时归位过的组合）+ `selftest`（15/15，stage M0.9）+ `regression`（manual，指向总闸本体）。
- `renderer/src/renderer.cpp`：`kStage` → `M0.9 full regression gate (all example cases x both backends)`；`viewer/main.cpp` selftest 阶段校验同步改 M0.9（仍 15/15）。
- `example/m0.9/` 归位：`hud-{dx12,vulkan}.png`（gate-hud 960×540 基线）+ `run.bat`（`all / gate-hud / selftest / regression`）+ `regression-report.md`（最近一次 ALL GREEN 报告存档）。
- README 进度行 / 命令块更新；ROADMAP 此前已把 M0.9 列为 0.x 序列终点。

**提交版本**    tag `m0.9`

**完成情况**
- 全量回归 **58/58 通过（bytes 58 / near 0 / FAIL 0）**，exit 0，用时 ~40 s（9 版本 × 24 可自动用例 × 双后端）。历史基线全部逐字节命中——含从 s0-init 到 m0.8 的全部 tag 快照 exe。
- 负向用例：把 `example/m0.5/nohud-dx12.png` 中间翻 1 字节 → 总闸 `[FAIL] … 基线或产物 PNG 无法解码`、exit 1；`git checkout` 还原后 MD5 仍 `7e528087…`、复跑 PASS。exe 缺失路径也实测（首跑时 m0.8 快照未补建，逐版本 FAIL 提示 rebuild.py）。
- `gate-hud` 目检：彩色线框 Suzanne（每三角形 hash 色）+ HUD 显示 `tri-id` 下拉与 wireframe 勾选，M0.9 阶段行自证；960×540 面板完整不裁底。
- 双后端对照（信息档）：19 个 pair 用例中 9 个两后端 PNG **逐字节相同**（clear / triangle / suzanne / inner-cullback / nohud×2 / icosphere / uv / tris）；10 个不同均属两类已知差异——HUD 类（m0.5~m0.9 的 hud / controls / wire / gate-hud）面板印的 backend / debug layer 文本本来就不同，内壁 / resize 类是既有 ≤1/255 的 GPU 取整噪声（m0.4/inner、m0.4/resize）。注意 gate-hud 两后端 MD5 不同但**各自**与基线逐字节命中，判定不受影响。
- `python tools/showcase/server.py --check` 退出 0（stage=M0.9，2 可见用例 / 2 基线）。
- 纯工具版本：不动任何渲染行为，`m0.5/nohud` 基线逐字节命中即机械证据。

**踩坑情况**
1. **`sys.path` 指错层** → 根因：脚本在 `tools/regression.py`，`HERE = Path(__file__).parent` 是 `tools/` 不是 `tools/showcase/`，`import server` 报 ModuleNotFoundError、ROOT 也多算一层。处理：`sys.path.insert(0, str(HERE / "showcase"))`、`ROOT = HERE.parent`。
2. **`--backend` 缺省时 `backend_filter = (None,)`** → 根因：`b` 变量为 None，f-string `{b:<6}` 对 None 用格式说明符炸 `unsupported format string passed to NoneType`。处理：`backend_filter = (args.backend,) if args.backend else BACKENDS`。
3. **坏基线 PNG 砸穿 zlib 解码** → 根因：负向验证把基线中间翻 1 字节，zlib 流损坏，stdlib PNG 解码器抛 `zlib.error`，总闸没把它当判据而是崩溃。处理：`judge_image` 里 `compare_pixels` 包 try/except → FAIL「PNG 无法解码」。负向用例本身反而帮总闸补了一条判据。

**用例归位**
- `example/m0.9/`：`hud-{dx12,vulkan}.png`（gate-hud 组合冒烟）+ `run.bat`（all / gate-hud / selftest / regression）+ `regression-report.md`（ALL GREEN 存档）。
- `example/cases.json` 增 m0.9 三用例；m0.8 exe 腾位到 tag 快照并 `rebuild.py --tags m0.8` 补建。
- `python tools/showcase/server.py --check` 退出 0。

**下一步**：0.x 序列到此冻结，进 S1 切簇（meshlet building / cluster culling，`thirdparty/meshoptimizer` 对照基准已就位）。

---

## M1.0 nanite-tools 骨架 + glTF 公共读取层（S1 第一步）

**动作**：S1 开工。`core/include/nl/core/mesh_io.hpp` + `core/src/mesh_io.cpp` 新增公共 glTF 读取层（场景图遍历、节点世界变换烘进顶点——切簇误差单位从第一天就统一成世界空间长度）；tinygltf 实现 TU 从 `renderer/src/scene.cpp` 移到 `core/src/mesh_io.cpp`（仍全工程唯一展开）。`nanite-tools/main.cpp` 新增 CLI：`stat` 实现（顶点/三角/子网格/AABB），`cluster/simplify/dag` 占位 exit 2。CMake 增 `nanite-tools` target + `s1-tools-stat` ctest（PASS_REGULAR_EXPRESSION 锁 Suzanne 3,936 tri）。

**提交版本**：`s1-tools`

**完成情况**
- `nanite-tools stat` 三资产全对上已知口径：Suzanne 11,808 vert / 3,936 tri；TorusKnotDense 103,113 vert / 204,800 tri；IcoSphereDense 163,842 vert / 327,680 tri。AABB 正常（球 ±1.0 严丝合缝）。
- scene.cpp 改走 `nl::core::loadGltfScene` 后渲染零变化：**`python tools/regression.py` 58/58 ALL GREEN（bytes 58）**，viewer --selftest 15/15（DX12 + Vulkan 两后端构建均过）。
- ctest 2/2（m0.5-selftest + s1-tools-stat）。`server.py --check` 退出 0（kStage 保持 M0.9——纯工具版本不动渲染，S1 的 example 归位放在 M1.3 可视化验收）。

**踩坑情况**
1. **vendored tinygltf 是老版本，`Model` 没有 `scene` 成员** → 根因：新版 tinygltf 用 `model.scene`，本机 vendored 版字段叫 `defaultScene`，编译报 C2039。处理：改用 `gltf.defaultScene`（注释标明版本差异）。
2. **缺 NORMAL/UV 属性会误报错** → 根因：第一版用 `readFloatAccessor` 的返回值同时当「属性存在」与「读取成功」，accessorIndex=-1（属性缺失）会落进「index out of range」错误分支。处理：先 `attr("NORMAL") >= 0` 判存在，缺失补零，存在但读失败才是真错误。

**下一步**：M1.1 meshlet 贪心构建（128 tri / 64 vert 上限、包围球、法线锥、初始误差），参考 bevy `from_mesh.rs` + meshoptimizer `meshlet.cpp`。

---

## M1.0 补档：交代挂展示台网页 + 交代规范固化（接上节，tag 仍 `s1-tools`）

**动作**：用户纠正「交代不限于 exe 截图」——工具的使用说明与实跑结果就是展示物，且要**挂展示台网页**（不是只写 markdown）。展示台新增 `kind:"tool"` 用例类型：cases.json 增 `s1-tools` 版本（usage / stat×三资产下拉 / 未实现负向，共 3 条 × 2 构建目录）；server.py 支持 `{root}` 占位与 `expectExit` 预期退出码；app.js 工具卡「▶ 运行工具」输出直接展开显示；regression.py 同语义兼容文本用例。交代规范固化进 `docs/ROADMAP.md` 新增「里程碑交代规范」节 + `docs/s1/S1-PLAN.md` M1.0 勾选补注。

**提交版本**：`s1-tools`（补档提交，不重打 tag）

**完成情况**
- 展示台网页实测：侧栏出现 s1-tools 版本卡，三张工具卡带运行按钮与模型下拉框，stat 切换 IcoSphereDense 实跑返回 `triangles : 327680`；无头 Edge 960×540+ 截图确认布局正常。
- **总闸扩容 58→64 全绿**：`python tools/regression.py` 64/64（新增 6 条 = 3 工具用例 × 2 目录），历史 58 条基线判定逐字节不动。
- 参数白名单负向实测：`model: "../secret.glb"` 被拦（`not in options`），args 只能来自清单、`{root}` 是唯一引用仓库内文件的方式。
- 坑：8787 端口被两个旧会话残留的展示台进程占着（旧清单只到 m0.6），新实例绑定失败静默——taskkill 清掉旧 PID 后重启才生效。**以后验证展示台先 `netstat -ano | grep 8787` 查残留**。

**踩坑情况**
1. **旧服务进程占端口、新实例静默失败** → 根因：cases.json 在 server.py import 时加载，残留进程永远吐旧数据；bind 失败的异常又因为后台任务不回显看不出来。处理：杀残留 PID 重启；后续 start.bat 前先查端口。
2. **`code != 0` 一刀切判败与工具退出码约定冲突** → 根因：usage（exit 1）、未实现子命令（exit 2）是合法预期结果，原判据会把它们当 FAIL。处理：cases.json 用 `expectExit` 声明预期退出码，server.py 与 regression.py 同一套语义校验（清单唯一、两处一致）。

---

## M1.1 meshlet 贪心切簇（S1 核心算法）

**动作**：`core/include/nl/core/meshlet.hpp` + `core/src/meshlet.cpp` 新增切簇算法层（meshoptimizer clusterizer 裁剪版）：每三角预计算质心+单位法线 → **位置 weld**（位级相等归代表顶点）→ CSR 几何邻接 → 拓扑优先级 + 空间评分贪心吸收，128 tri / 64 vert 双上限；每簇 Ritter 包围球 + 法线锥 + 世界空间叶子误差（簇内最长边长）。nanite-tools 新增 `cluster <model> [-v]` 与 `selftest cluster`；cases.json 增 s1-cluster 版本卡（3 工具用例）；M1.0 的 not-impl 负向用例从 cluster 换到 simplify（cluster 已实现）。CMake 增 ctest s1-cluster-selftest / s1-cluster-suzanne。

**提交版本**：`s1-cluster`

**完成情况**
- selftest cluster 全绿：三个确定性网格（连续 64x64 / 双断连块 / 单四边形）覆盖率 100%（多重集相等）、上限不破、包围球含全部簇顶点、越界索引与非三倍数 indexCount 显式拒绝。
- 三实机资产 coverage 100% + limits PASS：Suzanne 237 簇（avg 16.6 tri / 49.8 vert）、IcoSphereDense 4929 簇（avg 66.5 tri）、TorusKnotDense 3094 簇（avg 66.2 tri / max 64 vert 贴上限）。确定性验证：同 exe 连跑 4 次输出逐字相同。
- ctest 4/4；**回归总闸 64→70 ALL GREEN**（新增 s1-cluster 3 工具用例 × 2 目录，历史 64 条基线逐字节不动）；viewer 零改动、selftest 15/15 双后端。
- 交代物：`docs/s1/NANITE-TOOLS-GUIDE.md` M1.1 节（实跑输出归档）+ 展示台 s1-cluster 版本卡。

**踩坑情况**
1. **主循环推进计数漏自增** → 根因：append 里只置 emitted[] 标志，忘了 ++emittedCount，while 永不退出、簇数组无限增长直至段错误（bash 报 139 且无输出）。处理：计数挪进 Builder 成员，append 单点自增。
2. **代际戳从 0 起步与零初始化撞车** → 根因：usedStamp 全零初始化，stamp 初始 0，首个三角形的三顶点被判成「已在簇内」（usedStamp==stamp），产出 vertexCount=0 / triangleCount=1 的垃圾簇，后续 bounds 全 NaN。处理：stamp 从 1 起步（代际戳惯例）。
3. **按索引建几何邻接，Suzanne 退化成每 tri 一簇（3936 簇）** → 根因：glTF 导出常见每面拆顶点，Suzanne 11808 = 3936×3 索引零共享，索引邻接完全断开。处理：按位置 weld 出 canonical 顶点（位级相等排序分组），邻接/live/emitted 按 canonical，簇内条目去重与 GPU 上限按原始索引（保留 UV 缝两侧不同属性）。与 bevy position-merge、meshoptimizer weld 语义对齐。

**下一步**：M1.2 `.nmesh` 资产读写——二进制落盘（header 带版本号）+ roundtrip 逐字节一致 + stat 簇统计直方图。

---

## M1.2 .nmesh 资产读写 + stat 完善（S1 资产化）

**动作**：`core/include/nl/core/meshlet_io.hpp` + `core/src/meshlet_io.cpp` 新增 .nmesh 二进制读写（magic NMSH / version 1 / header 20B + record 52B + MeshVertex 32B 自包含顶点 + 三角局部索引字节流，static_assert 锁布局；写盘先临时后 rename；读回校验 magic/版本/截断/窗口越界）。nanite-tools：`cluster -o` 落盘+立即读回自检；`stat` 识别 .nmesh 打印簇统计 + ASCII 误差直方图；`selftest nmesh` roundtrip 单测。cases.json 增 s1-asset 版本卡；`assets/meshlets/Suzanne.nmesh`（402KB）入仓为 M1.3 viewer 输入资产。CMake 增 ctest s1-nmesh-roundtrip / s1-nmesh-suzanne。

**提交版本**：`s1-asset`

**完成情况**
- selftest nmesh 全绿：loadNMesh、逐字段一致、**重写逐字节一致**、坏 magic 拒绝、截断拒绝。
- 端到端：cluster -o 写盘 → stat 读回（237 簇 / 11808 vert / 3936 tri + 误差直方图），ctest 6/6。
- **回归总闸 70→76 ALL GREEN**；viewer 零改动、双后端 selftest 15/15、`--check` 退出 0。

**踩坑情况**
1. **NMeshHeader static_assert 写错（24B vs 实际 20B）** → 根因：手算字段和算错（4+2+2+4+4+4=20）。处理：修正断言——static_assert 恰好证明其价值，布局漂移编译期就爆。
2. **负向用例设计错**：第一版拿 `stat Suzanne.glb` 当「坏 magic」负向 → 根因：stat 对 .glb 本来就合法，exit 0。处理：换成缺失 .nmesh 路径（走 loadNMesh 失败分支）。教训：负向用例的输入必须真落在目标错误分支上。

**下一步**：M1.3 viewer `--mode cluster` 每簇随机着色（吃 assets/meshlets/Suzanne.nmesh），S1 收官可视化验收。

---

## M1.3 viewer --mode cluster 簇着色可视化（S1 收官）

**动作**：modes 分发器加 `Cluster` 模式；viewer `--mode cluster` 加载 `assets/meshlets/*.nmesh`（缺省 Suzanne.nmesh，`--model` 可选），簇窗口展开成全局索引单次 drawIndexed，**簇色 CPU 端 pcg hash（与 shader tri-id 同式）烘进 CLUSTERCOLOR 顶点属性**（.nmesh 窗口复制保证每条目归属唯一簇），零新增 GPU 资源类型；绑定贴图换 1x1 白占位（binding 布局不变）。model.slang 单源**双入口** vsMain/psMain + vsCluster/psCluster；CMake shader 产物 8→12、shaderArtifactsSelfTest 同步；kStage 升 `S1.0`；HUD shading combo 加 cluster；N 键循环 cluster 场景 0..4 / normal 场景钳 0..3。example/s1.0 归位（2 pair + selftest）；assets/meshlets/IcoSphereDense.nmesh（8MB）入仓。

**提交版本**：`s1-done`（S1 收官 tag）

**完成情况**
- 可视化验收达标：Suzanne 237 簇色块紧凑贴表面无畸形长条；球面 4929 簇均匀密铺——正是「吸收逻辑优先空间相邻」的直接视觉证据。
- **双后端 nohud PNG MD5 逐字节相同**（簇色 CPU 烘顶点的直接收益：确定性跨后端）。
- 回归总闸 76→**82 ALL GREEN**（+s1.0 用例 3×2）；ctest 6/6；selftest 15/15 双后端；`--check` 退出 0。
- m0.9 基线重拍：m0.9 的 exe 模板是当前 build（非快照），HUD stage 文本随 kStage 变 S1.0，重拍属预期动作而非漂移。

**踩坑情况**
1. **D3D12 debug layer 把「VS 签名有 CLUSTERCOLOR 但 normal 场景 input layout 不提供」记为 device error**（deviceErrors=24，每帧报）→ 根因：最初共用一对 shader 入口，以为未绑定属性是无害未定义值。处理：model.slang 拆双入口，normal/cluster 场景各用各的签名，CMake 入口清单按源文件过滤。
2. **NL_ASSET_DIR 是 assets/models 而非 assets** → cluster 资产路径拼错。处理：CMake 新增 `NL_MESHLET_DIR` 独立定义。
3. **m0.9/gate-hud 回归 FAIL 两后端** → 根因即坑 1 的 device error（dx12 退出码 1）+ kStage 变更导致 HUD stage 文本与旧基线像素不符（vulkan 877 px）。处理：修 shader 入口 + 用当前 exe 重拍 m0.9 基线（exe 模板=当前 build 的版本语义如此）。

**下一步**：S2 simplify（簇重切，`nanite-tools simplify` 占位已在）。S1 全程四个 tag：s1-tools → s1-cluster → s1-asset → s1-done。

---

## M2.0 METIS 簇聚组（S2 第一步）

**动作**：METIS 5 + GKlib vendored（`thirdparty/metis/`，KarypisLab 官方 main 浅克隆，自写 CMake 编静态库 `nl_metis`，不用上游构建系统）；core/grouping（`groupClusters`）：位置 weld（位级相等、忽略 UV 缝）→ 簇邻接图（边权=共享点数）→ `METIS_PartGraphKway`（vwgt=簇三角数、seed=42 固定、CONTIG 仅整图连通时开）→ 不连通分区按连通分量 fixup 拆分；`simplify` 子命令第一步（读 .nmesh → 聚组 → 统计打印）；selftest group（合成网格 5 断言 + 独立重算交叉验证）；ctest 6→8；cases.json 加 s2-group 工具卡（s1-tools 负向用例随 simplify 实现退到 dag）；交代归档 `docs/s2/NANITE-TOOLS-GUIDE.md`；S2-PLAN 组大小口径修订（~50000 tri → 8 簇/组，bevy 0.15 实证值）。

**提交版本**：`s2-group`

**完成情况**
- selftest group 全绿：单四边形 1 组、断连块无组跨块、每组连通（独立重算防同错）、空输入拒绝。
- 实机：Suzanne 237 簇→31 组（1 次 fixup）avg 7.6；球 4929 簇→617 组 avg 8.0 max 12 零 fixup，connectivity PASS。
- viewer 零改动：selftest 15/15、ctest 8/8、回归总闸全绿（渲染基线逐字节不动）。
- 展示台 s2-group 工具卡现场实跑（selftest / simplify 资产下拉 / 负向 exit 1）。

**踩坑情况**
1. **METIS `CONTIG=1` 要求输入图整体连通** → 断连合成网格 rc=-4（METIS_ERROR_INPUT）。处理：先 BFS 检测整图连通才设 CONTIG，断连靠 fixup 拆分兜底（「组不跨断连块」由 selftest 锁死）。
2. **GKlib `__thread` 在 MSVC 无定义** → 60+ 语法错。处理：固化上游 TLS 检测结果 `__thread=__declspec(thread)`（vendored CMake 直接写死）。
3. **UCRT limits.h 与 metis.h 宏重定义 C4005** → vendored metis.h 加 `#ifndef` guard（记入 VENDORED.md 改动清单）。
4. **选型校准**：S2-PLAN 旧稿「聚成 ~50000-tri 组」经 UE/bevy 源码侦察修订——UE group ≤128 簇、bevy 8 簇/组；50000-tri 是 UE page 口径。聚组选 METIS（bevy 0.15/0.16 同款背书）而非 UE 自研 GraphPartitioner（流送分页需求，S3 前不需要）；Carrot 本地仓库只有运行时 group 管理、无离线简化实现可参考。

**下一步**：M2.1 组内 QEM 简化 + 边界锁定（灵魂里程碑；meshoptimizer simplifier 作对照 oracle）。

## M2.1 组内 QEM 简化 + 边界锁定（S2 灵魂里程碑）

**动作**：core/simplify（`simplifyGroups`）：位置 weld → canonical 三角形 soup → 边界判定看组归属（点被 ≥2 个组的簇引用即锁）→ 逐组 QEM（面积加权平面二次型 + 有序集折叠队列，平局按点编号决胜）→ link condition + 法线翻转 + 接缝边保护拒绝坏折叠 → 折叠到 50% 即停；`simplify` 子命令升级（每组统计 + boundary pts + meshopt oracle 对照行）；meshoptimizer 1.3 vendored（仅 nanite-tools 链，core 零依赖）；weld 逻辑抽成 `weldPositions`（grouping/simplify/selftest/oracle 单一出处）；selftest qem（灵魂断言：边界点集合交叉验证 + 边界逐点位级不变 + 接缝边双侧存活 + 确定性重跑）；ctest 8→10；cases.json 加 s2-qem 工具卡（selftest / simplify 资产下拉 / 负向 -r 0）；交代归档 `docs/s2/NANITE-TOOLS-GUIDE.md`。

**提交版本**：`s2-qem`

**完成情况**
- selftest qem 全绿：64x64 网格 22 组、807 边界点，简化后边界逐点位级一致（自动断言，非肉眼）；接缝边两侧组都有三角形；确定性重跑逐字节一致。
- 实机：Suzanne 3936→1976 tri（50.2%）maxErr 0.025 世界空间；球 327680→178308（54.4%）maxErr 0.0005、617 组 7.6 s；oracle meshopt 49.5% / 49.9% 同档。
- viewer 零改动：ctest 10/10、回归总闸 94/94 ALL GREEN（渲染基线逐字节不动）。
- 展示台 s2-qem 工具卡现场实跑 + server.py --check 通过。

**踩坑情况**
1. **NMeshData 三角局部索引写错**：`canon[m.vertices[base+k]]` 两层错——正确是 `canon[m.vertexOffset + nm.triangles[base+k]]`（triangles 存的是簇内局部索引，先加 vertexOffset 得顶点号）。MSVC C2679 报「canon 的下标是 MeshVertex」才暴露。
2. **triNormal 覆盖哨兵用 0**：`overrideIdx=0` 表示「无覆盖」但 0 是合法局部顶点号，空指针被当坐标读 → selftest qem 段错误。改判 `overridePos` 指针本身。
3. **selftest 断言写错**：断连双块 case 误断言「无边界点」——块内相邻簇分属不同组时块内照样有组间边界点（这正是组归属口径的正确行为）；改为断言「锁定点坐标不落块间空档」。
4. **简化率略高于 50%（球 54.4%）**：50% 附近只剩 2-tri 折叠会越过目标或被 link condition 拒，组提前停住。口径内不改（M2.2 重切对 4~5% 偏差不敏感）。

**下一步**：M2.2 简化后重切（复用 M1.1 buildMeshlets）+ 逐层循环，分层 .nmesh 落盘时把边界点集合持久化（S3 DAG 合并用）。

## M2.1 补：可视化证据图（用户追问「有图么」后补齐）

**动作**：`simplify --obj <prefix>` 导出真实几何（before/after OBJ + locked 边界点集合，父目录自动创建）；新增 `tools/qem_preview.py`（Pillow 离线正交渲染，同机位同尺度左右对照 + 红点高亮）；产出 `example/s2-qem/qem-suzanne.png` + `example/s2-qem/run.bat`；展示台支持 `case.preview` 静态证据图（server 白名单 + app.js 卡片渲染 + CSS），s2-qem 卡挂图并新增 `dump` 现场实跑用例；ROADMAP「里程碑交代规范」固化「工具版本必须有真实输出生成的证据图」。

**提交版本**：`s2-qem`（补齐后重打 tag）

**完成情况**
- 图由工具真实导出的 2012 canonical 点 / 3936→1976 三角渲染，左 BEFORE 右 AFTER，622 红点两栏逐像素同位；`run.bat` 一键复现，`/shots/` 路由实测 200、461135 字节。
- 双后端 nanite-tools 重编、ctest 10/10、回归 96/96 ALL GREEN（新增 2 条 dump 用例），`server.py --check` OK。
- 规范同步：ROADMAP 表格与约定条目改掉「工具版本不产截图」的旧表述，明确不产 GPU 截图但必须有真实证据图。

**踩坑情况**
1. 原规范写「工具版本不产截图」，容易被当成「工具版本不用给图」——用户一句「有图么」直接指出缺口；改成「不产 GPU 截图，但必须有从工具真实输出生成的证据图」，并给出 `preview` 机制作为样板。
2. 展示台原本只为渲染基线（`baselines`/`beforeBaselines`）开放 `/shots/` 白名单，静态图直接引用会 403；把 `preview` 一并收进 `declared_shots()` 才通。
3. `--obj` 首次实现不创建父目录，干净 clone 的 `build/qem/` 不存在会写文件失败；加 `std::filesystem::create_directories(parent, ec)`。

## M2.2 逐层 LOD 循环：重切 + 分层落盘（S2）

**动作**：`core/lod`（`buildLodHierarchy`）按 Nanite 系标准做法逐层 重新聚组 → 边界锁按当前分组全量重算 → 每组硬目标 50% QEM → `buildMeshlets` 重切；误差按 UE/bevy 口径单调化为 parent error；`.nmesh` v2 尾部 section 持久化 `level/maxError/lockedPositions`（v1 逐字节兼容）；`groupClusters` 补 nparts<2 单组直通；`nanite-tools lod` 子命令（每层统计 + 分层写盘）；`selftest loop`（每层边界独立重算断言 / parent error 单调 / 每层约减半 / v1-v2 读回 / 确定性 / 负向）；ctest 10→12；`tools/lod_preview.py` 从落盘二进制渲染 `example/s2-loop/lod-suzanne.png` + `run.bat`；展示台 `s2-loop` 卡（preview 挂图）。

**提交版本**：`s2-loop`

**完成情况**
- 严格对齐三份源码，不自造机制：bevy `from_mesh.rs:114-120/520-523`（每轮覆写 `vertex_locks`）、`:556`（`group_indices.len()/2`）、UE `ClusterDAG.cpp:1470`（硬目标）、`Cluster.cpp:1023-1039`（LockPosition 只作用当前 group）、meshoptimizer `simplifier.cpp:2492`（折叠到硬目标）。M2.1 边界锁实现一行未改（本来就是标准 `LockBorder` 语义）。
- selftest loop 全绿：64x64 网格 **7 层** 7938→3966→1981→990→495→246→123，每层约 50%；灵魂断言「每层锁点集合 == 独立重算的当前分组边界集合」通过（证明不继承历史锁点）；parent error 单调不减；v2 读回逐字段一致、v1 仍可读、确定性重跑一致。
- 实机 Suzanne：**6 层** 3936→1976→985→490→246→124 tri，簇 237→33→17→13→5→3，锁点 622→117→84→35→0→0（递减而非累积），parent error 0→0.0260 单调；写出 6 个 v2 `.nmesh`。
- ctest 12/12、回归总闸全绿（渲染基线逐字节不动）、`server.py --check` OK；展示台 `s2-loop` 卡现场实跑 + 六层金字塔证据图。

**踩坑情况**
1. **自造「跨层累积锁定」被推翻（本轮主要修正）**：最初担心「更细层接缝点在更粗层被 QEM 移走会裂」，于是把每层锁点按世界空间位置累积继承，结果锁集合单调增长、可折叠边越来越少——Suzanne 只出 3 层（L2 降 31.4%、L3 降 9%），还用自造的 `minReduction 30%` 提前收敛把它藏起来（顶层仍有 1356 tri，层级根本没跑完）。核 bevy / UE / meshopt 源码后确认：标准做法是**每层重新分组、边界锁按当前分组全量重算、不继承**，跨层无裂缝靠 DAG cut 以 group 为原子单位，不靠永久锁顶点。删掉累积与门槛后，同一资产自然收敛出 6 层、每层严格约 50%。M2.1 的边界锁不动。
2. **曾误判 M2.1 边界锁「比标准更严」**：核 `simplifier.cpp:526-529` 后确认 `meshopt_SimplifyLockBorder` 把 border/fringe 整体改判 `Kind_Locked`（不能作折叠源、只能作目标，双锁边不折叠），与我们一致；结论已同步纠正到 `simplify.hpp` 注释，未改代码。
3. **误差不单调**：原先 `lodMeta.maxError` 存「生成下一层引入的误差」，跨层会忽大忽小，S3 DAG cut 判据不成立。按 UE `ClusterDAG.cpp:1441` / bevy `:146-151` 改成 parent error 单调不减，并加自动断言。
4. **METIS nparts=1**：逐层后期簇数 ≤8 时 `nparts=1`，`METIS_PartGraphKway` 不接受 <2 → `grouping.cpp` 单组直通 + 连通性 fixup。
5. **NMeshLodMeta 定义位置**：结构体曾放在 `NMeshData` 之后导致 C3646，须先定义后使用。
6. **工具卡历史 exe 补建——已定案不做**：M2.2 期间曾按「每出新版把上一版 exe 改指 `build/showcase/{tag}/`」尝试给 s2-group/s2-qem 的 nanite-tools.exe 补建历史快照（`git archive <tag>` 全量构建），跑出 1/4 后取消。复核后定案：历史快照机制是为渲染卡基线 PNG 逐字节复现服务的，工具用例无基线可比、现场实跑即权威，S1 起工具卡事实口径一直是当前构建。`s2-group`/`s2-qem` 两卡映射维持 `build/{dx12,vulkan}/nanite-tools.exe`，不再补建；规范已修订到 ROADMAP「里程碑交代规范」（工具卡 exe 恒指当前构建，`rebuild.py` 只管渲染 viewer 快照）。

**下一步**：M2.3 `viewer --mode lod N` + ImGui 滑条切层，肉眼验收「任意相邻层拼接无裂缝」（S2 收官 tag `s2-done`）。

## M2.3 viewer --mode lod N + HUD 滑条（S2 收官）

**动作**：modes 加 `Lod`（"lod"）；viewer `--lod N`（非数字/负数 parseArgs 直接 exit 1）+ `--mode lod` 分发（复用 cluster 场景，缺省模型/缺省着色 normal，显式 `--shading` 生效）；renderer 分层资产探测 `probeLodLevelCount`（backend.hpp 公共函数，两后端共用：`<stem>.lodN.nmesh` 从 0 连续计数）；`Scene::uploadClusterMesh`（读盘→簇窗口展开→新建 VB/IB 上传→替换句柄，首载与切层同一条路，shader/pipeline/binding 全复用）；`Renderer::setLodLevel` + `DeviceInfo.lodLevel/Count/MaxError`；HudFrame 加 lod 四字段，HUD 滑条（仅 lodCount>1 显示）+ `lod N/M tri maxErr` 行；kStage 升 `S2.0`；分层资产 `Suzanne.lod0..5.nmesh`（~0.5 MB）入仓；`tools/lod_gallery.py` 六层画廊 + `example/s2-done/run.bat`；展示台 `s2.0` 卡（L0/L3/L5 + HUD pair 双后端基线、selftest、3 负向 tool 卡）。

**提交版本**：`s2-done`

**完成情况**
- 六层同机位画廊（viewer 真实 GPU 截图）：3936→1976→985→490→246→124 tri，逐层简化、轮廓连续、**无裂缝无洞**；L4/L5 眼窝黑面 `--cull none` 复检为完整内壁几何（M2.2 口径内翻转三角形），非裂缝——边界锁定生效的物理证据。
- 层切换零闪烁；HUD 同步显示 `lod N/5 tri <n> maxErr <e>`（数值来自 .nmesh v2 尾部 section）。
- 负向全拒：`--lod abc`（无窗口）/ `--lod 9`（out of range）/ 无分层资产（提示 nanite-tools lod 命令）均 exit 1。
- ctest 12/12（双后端）、回归总闸 118/118 ALL GREEN、`server.py --check` OK；m0.9 gate-hud 基线随 kStage→S2.0 重拍（预期动作，同 M1.3）。

**踩坑情况**
1. **lod 缺省着色三元式写反（回归当场抓住）**：`(lodMode && !shadingSet) ? 1 : (cluster ? 4 : o.shading)` 在显式 `--shading normal` 时落到 `cluster ? 4` 分支——lod 也是 cluster 场景，显式着色被 4=cluster 静默吞掉，回归 pair 基线 2.8 万像素不符暴露。修正为「先按场景缺省、lod 再覆写」：`shading = cluster ? 4 : o.shading; if (lodMode) shading = o.shadingSet ? o.shading : 1;`。教训：新场景叠在旧场景的缺省分支上时，覆写方向要想清楚谁优先。
2. **负向 tool 卡不能跑无参 viewer**：不带 `--screenshot` 的 viewer 进交互主循环，展示台 120s 超时；负向用例补 `--screenshot {root}/build/...` 让它走确定性截图路径后正常退出 1。
3. **m0.9 HUD 基线漂移属预期**：kStage 文本在 HUD 里，升 S2.0 后 gate-hud 双后端 FAIL；重拍两张基线即恢复全绿（与 M1.3 升 S1.0 同一动作）。

**下一步**：S3 构建 cluster DAG + fallback mesh（`nanite-tools dag`；DAG cut 以 group 为原子单位吃 `.nmesh` v2 的 lockedPositions / parent error）。

## M2.3a 简化器翻转面根治（判决实验定稿：朝向锚 + 法线继承）

**动作**：用户方法论三原则（①标准先行 ②抄全不遗漏 ③官方 oracle 对比）驱动。核源码定参照系事实：meshopt/UE 翻面判定均**无存储参照场**（meshopt 实时 vertex_positions、UE TriWillInvert 实时 GetPosition，`MeshSimplify.cpp:779`）；判决实验摘锚跑纯官方口径（cos75°+多 pass+开放边界锁全抄）——单层 inward=1（oracle 2）但**跨层硬目标链累计 L1=1→L6=8（worst cos -0.972）**，官方无逐层链场景无现成机制可抄 → 锚定稿保留；`rebuildMeshlets` 改**法线继承**（继承输入法线和，不按邻面重算）：位置全程不动、法线场跨层不变 = L0 原始场，与 check-orient 审计同口径；锚对无法线数据的合成网格退化为纯官方判定（自检回归当场抓住 ratio=100%）；自检预期与 LockBorder 官方语义对齐（孤四边形不折叠、末层收敛停滞例外）。

**提交版本**：（本轮，s2-done 之后）

**完成情况**
- 判决数据：同分组同 50% 硬目标下 ours inward 0 / worst +0.063 vs oracle inward 2 / worst -0.333；全链 check-orient L0..L6 七层全 PASS（exit 0），纯官方口径对照 L1=1→L6=8。
- 七层画廊（真实 GPU 截图，`example/s2-done/lod-layers.png`）：3936/1970/982/490/248/140/104 tri，**无黑斑无裂缝无洞**，L6 眼睛仍完整——M2.2 以来「L4/L5 已知翻转面」口径作废。
- ctest 12/12、回归总闸 118/118 ALL GREEN、`server.py --check` OK；分层资产 7 层（~0.52MB）重拍入仓。

**踩坑情况**
1. **锚退化拒绝一切折叠（自检当场抓住）**：合成测试网格无法线数据 → 法线和全零 → 锚 refLen≈0 → 拒绝所有折叠 ratio=100%，qem/loop 自检 5 例 FAIL。修正：锚只在有法线信息时加约束，refLen 退化的三角退化为纯官方 cos75° 判定（保守方向不变：真实资产锚生效，合成网格退化不出错）。
2. **自检预期滞后于官方语义**：孤四边形「2→1」预期写在开放边界锁加入前——meshopt LockBorder 同样不折叠（边界全锁+对角折叠会清空整组）；loop「每层 30~70%」在末层收敛停滞（89.6%）越界。测试预期对齐官方语义，非凑结果。
3. **VULKAN viewer 被 cp 盖掉后 nmake 不重链**：手动 cp DX12 viewer.exe 到 VULKAN 目录后时间戳变新，重编「Built target」零编译行、二进制仍是 DX12（日志 shader=dxil 暴露）。删 exe 强制重链恢复 shader=spv。教训：NMake 只看时间戳，覆盖产物必须删掉再编；backend 自证看 `shader=spv/dxil` 与 `backend: vulkan/dx12` 日志。
4. **判决实验方法论沉淀**：「纯官方 vs 加码」的取舍必须跑同分组同目标对照实验，不是看谁说得有理。锚两次参照系 bug 的根因（出生角点 vs 当前角点、更新前 vs 更新后参照）都是实验抓的，不是审代码审出来的。

**下一步**：S3 构建 cluster DAG + fallback mesh（`nanite-tools dag`；DAG cut 以 group 为原子单位吃 `.nmesh` v2 的 lockedPositions / parent error）。

## M3.0 cluster DAG 组装与校验（tag `s3-dag`，2026-11-07）

**动作**：三家源码实读定稿（UE ClusterDAG.cpp / bevy from_mesh.rs / Carrot→zeux clusterlod.h）→ M2.2 全局重切改**按组重切**（父簇绝不跨组，S5 组原子 cut 语义前提）→ 血缘落盘 .nmesh meta version 2（dagGroups + clusterGroup，v1/旧 v2 逐字节兼容）→ `nanite-tools dag` 六项校验器 + `selftest dag` 套件 + `--export` JSON → 资产再生成 + 基线重拍 + 交代物。

**提交版本**：s3-dag（本轮）

**完成情况**
- dag Suzanne 六项校验全 PASS：56 groups / 57 edges / 87 父簇携带组误差 / DFS 无环；顶层 3 组 terminal（genCount=0）。
- check-orient 全链 L0..L6 inward=0（按组重切不破 M2.3a 朝向锚）。
- ctest 12→14（s3-dag-selftest / s3-dag-suzanne）、回归总闸 118/118 ALL GREEN、`server.py --check` OK（kStage 升 S3.0）。
- 交代物：证据图 `example/s3-dag/dag-structure.png`（真实 export JSON，蓝→红 = ParentLODError）+ run.bat + `docs/s3/S3-TOOLS-GUIDE.md` + 展示台 s3.0 卡（selftest/dag/2 负向）。
- Suzanne 新链：3936→1970→1002→500→252→142→104 tri（7 层，链更陡）；L0/L3/L5+HUD 双后端基线重拍。

**踩坑情况**
1. **buildGeneratingMap 开尺寸错**：gen 区间索引的是**下一层**簇号，数组却按本层簇数开尺寸——L0 簇数多于 L1 时尾部全是「无生成组」假报警。selftest 当场抓住；修为显式传 nextClusters。
2. **保存侧漏 payloadBytes + 组数前缀**：meta 重构时丢了 `payloadBytes` 赋值（恒 0）与 DAG 段的 `u32 groupCount` 前缀，v3 roundtrip 必挂。加载侧段长校验（section != expected）是靠它能抓住的设计。
3. **按组重切簇粒度取整**：8 簇/组减半后仍要整簇产出，单层保留率上界从 70% 抬到 ~80%（bevy/UE 同款效应）；loop 自检口径 30~70% → 30~80% 并注明，非凑结果。
4. **export JSON 反斜杠未转义**：prefix 含 `\` 时写出非法 JSON（run.bat 实跑抓出，手工跑正斜杠路径测不出来）。修 C 侧转义 `'\'→"\\\\"`。
5. **s1-tools not-impl 负向退役**：`dag` 实现后 exit 2 占位清零，负向按其自身约定退化为「未知子命令显式拒绝」（cut 是 S5 运行时能力，不进离线工具）。

**下一步**：M3.1 fallback mesh（顶层连普通 indexed mesh，S8 BLAS 用）→ M3.2 viewer --mode dag-top 验收。

## M3.1 fallback mesh（tag `s3-fallback`，2026-11-07）

**动作**：UE BuildFallbackMesh/BuildCoarseRepresentation 实读定稿（取链顶层，UE 默认 ~2% 同量级，不做第二次独立简化）→ .nmesh 文件 version 3（'NFLB' 自描述尾段，按 magic 识别）→ core/fallback buildFallbackMesh（weldPositions 同口径拼接）→ lod 命令写 lod0 尾段（输入文件不动）→ selftest fallback + ctest → 证据图 + 交代物。

**提交版本**：s3-fallback（本轮）

**完成情况**
- fallback Suzanne：104 tris / 96 verts（from L6, maxErr 1.1375），2.6% ≈ UE 默认 ~2%。
- selftest fallback 全绿：三角守恒 / weld 收敛 / 边流形 ≤2 / 确定性 / v3 roundtrip / fallback-only 尾段 / 负向。
- ctest 14→16 双后端全绿；dag 六项校验 + check-orient inward=0 无回归。
- 交代物：fallback-preview.png（真实资产字节离线渲染）+ run.bat + S3-TOOLS-GUIDE M3.1 节 + 展示台 s3.1 卡（selftest/lod/负向）。

**踩坑情况**
1. **triangleOffset 语义踩坑**：M1.1 的 `triangleOffset` 是**字节偏移**（`triOffset = out->triangles.size()`，每三角 3 字节），不是三角号——fallback 拼接按三角号 ×3 读窗口全部错位（selftest 当场抓住：local index ≥ vertexCount、边流形爆炸）。全仓其余使用点（simplify/scene/loadNMesh 校验）均为字节语义，只有新代码用错。已修 + meshlet.hpp 注释钉死「字节偏移，不是三角号」。
2. **fallback-only 测试网格不合法**：合成 v3 测试用了 0 簇 + 三角主体，被 loadNMesh 既有校验（meshlets empty but triangles present）拒绝——该校验是对的，测试网格补了真实单簇记录。
3. **设计取舍记录**：fallback 落点选 lod0 尾段而不是改写输入文件——展示台工具卡实跑 `-o build/...` 时不能碰入仓资产；v3 尾段按 magic 识别使「v1 主体 + 仅 fallback」也成为合法 v3。

## M3.2 dag-top 组原子下钻（tag `s3-done`，2026-11-07）

**动作**：三家运行时实读定稿（UE NaniteHierarchyTraversal 遍历单步冻结 / bevy cull_bvw parent_error / Carrot 双阈值）→ core/modes 加 dag-top → Scene::setDagCut（挖 gen 区间父簇、换组内子簇，与 lod 同一条上传路径）→ 双后端 setDagCut + 组数探测 → HUD 下钻滑条 → viewer --dag-group（负向全拒）→ kStage S3.2 → 双证据验收 → 交代物。

**提交版本**：s3-done（本轮）

**完成情况**
- dag-top 三组下钻 GPU coverage hole=0；g1/g2 混合集与纯顶层 PNG 逐字节一致（16→16 零折叠父簇≡子簇）。
- dag_weld_check.py --all PASS：mixed 集开边集合=full top 的 84 条模型开放边（逐层守恒），零新增开边零重叠。
- HUD 下钻滑条 960×540 面板完整（底边 345）；DX12 vs Vulkan 四对非 HUD 基线逐字节一致。
- ctest 16/16 双后端；回归总闸全绿；交代物：example/s3-done（5 对基线 + run.bat + dag-drills.png）+ 展示台 s3.2 卡 + S3-TOOLS-GUIDE M3.2 节；s2.0 exe 按快照规范改 build/showcase/s2-done/。

**踩坑情况**
1. **expandClusterData 索引基址**：文件 vertexOffset 只有在全量按序展开时才等于缓冲位置；掩码跳簇后被跳簇之后的 kept 簇索引全错位（kept 簇整个消失、洞 1182px）。修为 clusterBase=入缓冲位置快照，全量路径逐位等价。
2. **离线探针 stride 坑**：dag_weld_check.py 顶点按 i*3（12B）切片，MeshVertex 实为 8 float（32B）——NN/bbox/位级探针全垃圾，一度误判「轴交换」「父簇顶点 10/62 位级存在」，浪费一轮排查。修为 i*8 后自洽：父簇顶点全部位级存在于子簇（简化保位置）。
3. **流形判据口径**：Suzanne 开壳的 84 条 1x 边是眼窝+嘴的**开放边界**（L0 即存在、逐层守恒），不是洞；判据应为「mixed 开边集合==full top」而非「0 条 1x」。
4. **exe 快照规范与 M2.3a 后置修复的冲突**：s2.0 卡映射改 s2-done tag 快照后回归 FAIL（2.8 万像素）——tag（f5bfc9a）是 M2.3a 翻转面根治**之前**的代码，而卡基线已在 fdb598c/32a10b0 重拍，tag 快照永远无法复现。回退为当前构建（与 m0.9 同款既定模式）；快照规则只在「tag 状态与基线状态一致」时可用，后续 viewer 卡收敛时机须选渲染代码已定稿的 tag。

**下一步**：S4 分块/LOD 交换（或按 ROADMAP 顺延）；S5 运行时组原子 cut（M3.2 已预演，屏幕空间选层公式已存档）。

---

## M4.0 GPU 簇数据通路 + vertex pulling 渲染（tag `s4-indirect`，2026-11-08）

**动作**
- S4-PLAN 复读三家定稿：bevy `visibility_buffer_hardware_raster.wesl` 单 draw + vertex pulling（SSBO 拉顶点）作蓝本；UE 跳过硬件光栅、Carrot 走 mesh shader（NVRHI 2021 代无承诺）均不适用；范围边界写死（遮挡/Hi-Z/S6、软光栅/S7、多实例剔除、BVH 加速全不做；剔除须无状态可重复）。
- 方案 A 拍板：单 `drawIndexedIndirect`，`instanceCount=可见簇数`（M4.0 恒=总簇数全画），S5 遍历 cut 只换 instanceCount 语义零返工；VS 多一次 SSBO 间接寻址。~~方案 B per-cluster 多条 args + drawIndexedIndirectCount 否决~~。
- `core/modes` 加 `ViewMode::Cull=4` + `kModeNames` "cull"；`viewer::runMode` 加 Cull 分支、`Renderer::create` 透传 `cullMode`、`--cull off`(=none) 合法；`Renderer::create` 对 cull/cluster 按 `.nmesh` 白名单校验。
- `scene` 加 `initCull`：读 .nmesh 铺三张 SSBO——`vertexPool`(8f/顶点 pos/normal/uv)、`localIndices`(每簇局部索引)、`clusterMeta`([vertexBase,triCount,localBase,0])；`maxTriCount→stride=3*maxTriCount`；`identityIndex`(0..stride-1) 作索引缓冲；`indirectArgs`(DrawIndexedIndirectArguments{indexCount=stride, instanceCount=clusterCount})；binding 三 `StructuredBuffer_SRV(t1/t2/t3)` + CB + tex + sampler；4 套 pipeline **无顶点输入**（null input layout）。
- `scene::draw` 加 cull 分支：写 CB → 按 (fill×cull) 选 cull pipeline → 只绑 `cullBindingSet` + `identityIndex_`(index buffer) + `setIndirectParams(indirectArgs_)` → `drawIndexedIndirect(0,1)`；不绑任何顶点缓冲。
- shader `model.slang` 加 `vsCull`/`psCull`：`SV_InstanceID`→簇元数据→SSBO 拉取；越界三角置 `asfloat(0x7fc00000u)` NaN 被光栅化整三角丢弃；簇色 `triIdColor(instanceIndex)` 与 cluster 场景 CPU 端 `clusterColor()` 同式 → 逐像素一致。`CMakeLists` `_ENTRIES_MODEL` 加 `vsCull`/`psCull`，双后端 16 个 shader 产物齐全。

**提交版本**：s4-indirect（本轮）

**完成情况**
- 双后端（DX12/Vulkan）viewer `--mode cull` 单 drawIndexedIndirect 全簇全画成功：`meshlets=237 / triangles=3936 / stride=63`，`deviceErrors=0`。
- **逐像素一致验收（M4.0 判据，S4-PLAN 第 38 行）**：同机位同 shading（yaw30/pitch20/dist2.6, 640x360, shading cluster）下 `--mode cull` 与 `--mode cluster` 截图 `cmp` 逐字节相同——DX12：`cluster_back==cull_back`、`cluster_none==cull_none` 均 IDENTICAL；Vulkan 同样两组 IDENTICAL；`back` vs `none` 正确相异（sanity 通过）。
- `tools/showcase/server.py --check`：OK（stage=S4.0 version=s4.0 cases=2 baselines=4）。
- `tools/regression.py --only s4.0`：4/4 ALL GREEN（bytes，DX12+Vulkan × back/none），产出 `build/regression/report.md`。
- 交代物：`example/s4-indirect/`（4 张 cluster 基线 PNG + `run.bat` 现场复现 cluster==cull 自检 exit 0）+ `example/cases.json` s4.0 卡（2 个 pair 用例，基线=cluster 渲染、用例渲染 cull，regression 比对逐字节相同）。

**踩坑情况**
1. **D3D12 入口名忽略**：NVRHI D3D12 `createShader` 只存整块字节码，`entryName` 不用于选入口；Vulkan 侧 SPIR-V 入口恒为 `main`。初版给 Vulkan 传 `vsMain` 致入口缺失，统一为 DX12=`"vsMain"`/VK=`"main"` 后双后端通过。
2. **MeshVertex 布局**：顶点池步长=8 float/顶点（pos3+normal3+uv2=32B，`static_assert(sizeof==32)`），索引公式 `b=gv*8`；初版误用 3 float 步长致顶点错位，按 `core/include/nl/core/mesh_io.hpp` 修正。
3. **null input layout + indirect draw 合法性**：`GraphicsPipelineDesc` 不调 `setInputLayout`→inputLayout=null；D3D12 仅当 `inputLayout && !inputElements.empty()` 才设 IA，Vulkan 仅当 `inputLayout != null` 才设 vertex input——故无顶点缓冲的 pulling pipeline 合法，无需兜底 input layout（与 vendored nvrhi.h 实测一致）。
4. **indirectArgs 字段对位**：`DrawIndexedIndirectArguments` 顺序为 indexCount/instanceCount/firstIndex/firstInstance（与 Vulkan `VkDrawIndexedIndirectCommand` 的 indexCount 字段对位）；indexCount 用 `stride=3*maxTriCount`，instanceCount=总簇数全画。

**下一步**：M4.1 compute 视锥剔除（Gribb-Hartmann 5 平面球体测试、atomicAdd append 可见列表、GPU 每帧只写 instanceCount；M4.0 全画通路零返工改 instanceCount 语义即可）+ `--mode cull` HUD 可见簇数 + `tools/frustum_check.py` 离线对照。

## B2~B4 材质图轨道第一批（A 类并行轨道，feat/material-graph，2026-11-08）

> 详细验收纪要：`docs/material-graph/B2-B4-ACCEPTANCE.md`（固化文档闸要求，merge 回 master 前置）。

**动作**：B 路自建轻量材质图三步落地——B2 节点图数据模型（Node/Pin/Edge + validate + material_graph v1 确定性文本序列化）；B3 图→Slang 代码生成（纯函数 emitter，Output 上游 DAG 递归求值，产物符合 model.slang 约定：vsMain/psMain + register(b0/t0/s0) + SceneConstants）+ slangc 双后端 × vs/ps 四入口真实编译验收；B4 热重载（watch 轮询 mtime，复用 recompileGraph 单一链路）+ `render` 子命令从 .mg 真实数据出 SVG 证据图；主干 8faad52 同步合入（b2d1050）后复验。

**提交版本**：feat/material-graph `1a912fa`(B2) / `0cb2593`(B3) / `b01959e`(B4) / `3a22b40`(render 修复) / `b2d1050`(主干同步)

**完成情况**：ctest b2-graph-selftest / b3-graph-genslang / b4-graph-hot-reload 全绿（主干同步后 3/3 复验）；生成 .slang 经 slangc SPIR-V+DXIL 双后端四入口全过（genslang: OK）；热重载实测后台轮询检测到 .mg 变更并二次重编译 OK；证据图 b3-nodegraph.{svg,png}（4 节点 3 连线，非手绘）。B 路阶梯推进至 B4，B5（接 PBR 材质 pass）排 S7 后。

**踩坑情况**：① junction 共享 .workbuddy 本地改动挡 merge 且 skip-worktree 下 stash 不可见——物理备份→临时 no-skip-worktree 还原→merge→cp 恢复→重设；② reg.exe 被沙箱拉黑致 vcvarsall 缺 SDK——手动注入 SDK 路径，构建批处理禁中文注释（代码页拆行）；③ 匿名命名空间内 main 无外部链接致链接失败；④ system() 路径无条件加引号会被 cmd 解析坏，含空格才加；⑤ watch 长进程管道全缓冲致实时输出不可见——setvbuf _IONBF；⑥ render 节点高度漏算属性行致 value 文本溢出——统一 nodeHeight() 口径。
