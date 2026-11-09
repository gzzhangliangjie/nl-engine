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
| M4.1 GPU 视锥剔除 + 可见列表间接寻址 | ✅ 验收通过 | `s4-cull` | 2026-11-08 | compute 剔除 pass（csCull 5 平面包围球测试）+ atomicAdd append visibleList + indirectArgs.instanceCount 回写；pass-through（正对全可见）与 cluster 基线逐字节相同；真剔除姿态（进模 dist0.45/640x720）GPU 30/237 == frustum_check.py 离线 30，双后端一致；kStage=S4.1；交代物 example/s4-cull + cases.json s4.1/s4.1-tools 卡 |
| M4.2 HUD 统计 + 1M 场景 + 性能验收 | ✅ 验收通过 | `s4-done` | 2026-11-08 | cull pass 接第二个 GpuTimer 实例（[frame]/[cull] resolve 日志 + HUD cull GPU 行/曲线）；IcoSphereDense1M 入仓（1,310,720 tri / 19,718 簇 / 33.5MB .nmesh，coverage 100%）；1.3M tri 正对 640x360 全可见 pass-through cull==cluster 逐字节（跨后端也 bytes）；进模 1932/19718（剔除 90.2%）== 离线，双后端一致；60fps：CPU avg 4.9~5.5ms / 整帧 GPU 0.96~1.02ms / cull pass ~0.010ms（判据 16.6ms 余两数量级）；kStage=S4.2；交代物 example/s4-done + cases.json s4.2/s4.2-tools 卡 |
| M5.0 误差投影：世界→屏幕像素 | ✅ 验收通过 | `s5-proj` | 2026-11-08 | core 纯数学层 `screenErrorPixels()`（bevy/zeux 口径，用户拍板）+ `lodErrorSelfTest()` 手算用例进 selftest（15→16）+ OrbitCamera 三个只读访问器 + HUD dag 模式 `s5 err` 行 + `cut thr px` 滑条；三机位 1567.05→352.15→149.84 px 单调下降且与离线手算逐位一致；13 对 HUD 基线 kStage bump 全量重拍 + 3 条关键用例回归 6/6 bytes + s5.0 卡 4/4 bytes + server --check OK；UE 差异台账 `docs/s5/UE-DIFFS.md` 建立（AGENTS.md 固化入口）；kStage=S5.0；交代物 example/s5-proj + cases.json s5.0 卡 |

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

## M-A0~M-A4 动作系统立项 + 纯逻辑层四步（A 类并行轨道 feat/action-system，2026-11-08）

**动作**：立项（三家实读：UE Animation 模块定向读 / bevy_animation 0.20-dev RFC 51 / Carrot 二次确认动作逻辑层空白；网上生态调研：ozz=下层事实标准但刻意不做状态机层、Lumix=自建参照）→ 用户拍板 A 路自建 + 自写最小数学 + 独立 anim-tools exe → M-A1 关键帧采样 → M-A2 线性/加法混合 → M-A3 动作状态机 → M-A4 1D blend space → 工具版交代（展示台 ma-track 卡 + run.bat + 证据图 + TOOLS-GUIDE）。计划落 `docs/action-system/ACTION-PLAN.md`（含三处对照拍板表与网上调研 §2.5）。

**提交版本**：M-A1（M-A0/M-A1 同 commit）/ M-A2 / M-A3 / M-A4+交代（tag `anim-ma1`~`anim-ma4`，验收后打）

**完成情况**
- nl/anim：anim_math（vec3/quat：slerp 最短路径 + 近平行 nlerp 退化 + scaleQuat/mulQuat 加法语义）、anim（AnimClip per-bone 通道 (t,v) 秒制、Linear/Step、域外 clamp、wrapTime、validateClip 空通道=常量、lerpPose/blendPoses/addPose）、state_machine（优先级取首个 + crossfade + UE 回退缩时 effective=D*(1-wNext)、swap+alpha 取反保姿态连续）、blend_space（找段线性插权重、域外 clamp、单 sample 退化）。
- anim-tools selftest 四组全 PASS（确定性数值锚点、两次运行逐位一致）+ sample 确定性演示输出；ctest 5/5（ma1~ma4 + 负向 WILL_FAIL）。
- 交代物：`example/ma-track/`（run.bat ALL GREEN + evidence.png 960×540 由 sample 真实输出经纯标准库 PNG 绘制，两次生成逐字节一致）+ 展示台 ma-track 版本卡 7 张（usage/curve/blend/statemachine/blendspace/negative/evidence，expectExit 0/2 + expectLog 锁死）+ `docs/action-system/ANIM-TOOLS-GUIDE.md`；`server.py --check` OK（stage=S3.2 不受 A 轨影响）。
- A 类纪律：不进 kStage、不动 S0~S14、不进渲染回归总闸；渲染代码零改动。

**踩坑情况**
1. 沙箱 reg.exe 黑名单打断 vcvars 注册表探测（rc/mt/LIB 缺失，CMake 编译器探测挂）→ `scripts/build-anim.bat` 手工补 SDK bin + INCLUDE/LIB；HTTPS_PROXY/https_proxy 撞键致 DevShell/VS 生成器不可用（M0.0 坑 6 重现）→ 继续 NMake。
2. validateClip 曾把空通道判非法 → 仅 rotation 轨道的骨骼（head）被拒 → sampleClip 空 poses → 自测 poses[0] 段错误；逐用例进度输出定位后改「空通道=常量语义」（与采样侧一致）。
3. 自测期望手算三连错（漏算 crossfade 期间 fromTime 同步推进 → step6/7/8 期望值全错；sin(11.25°) 常数笔误；断言时机在场景结束后）——实现全对、期望全错，逐值核算修正；教训：断言值必须在 update 时序模型下手算，不能拍脑袋。
4. blendPoses 输入输出同容器混叠（先 clear 毁输入）——evaluate 改临时缓冲。
5. 测试时点撞语义边界：t=1.0 撞 wrapTime 循环归零（t=duration→0，实现正确）、t=0 撞 clip 起点。
6. NMake 时间戳陷阱复发：obj 在 `CMakeFiles/<target>/<target>/` 子目录，删错层不重编（M2.3a 坑 3 同款）。

**下一步**：M-A5 视觉验收后挂（等 2.3 骨骼蒙皮 + rigged GLB）；2.3 立项时再议 vendored ozz；merge 回 master 走 PARALLEL-TRACKS §7.2 文档闸。

---
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

## M4.1 compute 视锥剔除 + 可见列表间接寻址（tag `s4-cull`，2026-11-08）

**动作**
- `renderer/shaders/cull.slang`（新）：compute 剔除 pass——wg=128 每线程 1 簇，Gribb-Hartmann 5 平面包围球测试（`g_mvp[3]±g_mvp[0]`、`g_mvp[3]±g_mvp[1]`、`g_mvp[2]`，平面归一化，`dot(c,n)+d < -r` 判外）；可见簇 `InterlockedAdd(g_cullArgs[1],1)` 抢槽 + `g_visibleList[slot]=tid` 追加。SPIR-V 需 `-fvk-u-shift 384 0`（vendored NVRHI `VulkanBindingOffsets::unorderedAccess=384`），并入 CMake 通用旗标。
- `model.slang` vsCull 改间接寻址：`clusterId = g_visibleList[instanceIndex]`（原来是 instanceIndex 直通）；簇色 `pcg(clusterId)` 与槽位无关，原子追加的非确定次序不进像素。
- `scene.cpp` initCull 扩展：clusterSpheres SSBO（.nmesh 包围球）、visibleList SSBO（identity 初值，frustum-off 恒等表）、indirectArgs（frustum-on 时 GPU 写 instanceCount）、compute pipeline + binding set（CB0+SRV1/2+UAV0/1）、2 槽 readback 环形（`cpuAccess=Read` 永久 CopyDest）；draw() cull 路径 = reset args → dispatch → `copyBuffer(readback[i&1], args+4, 4)`；`pollCullStats` 延迟 2 帧 map（NVRHI map 内部等 lastUseFence，主循环无等待）。
- 接线：`Renderer::create`/backend init 加 `frustumCull`；`viewer --frustum on|off`；DeviceInfo/HudFrame 加 visibleClusters/totalClusters；HUD 新 frustum 统计行；`renderFrame` 修 info 重同步（每帧从 backend 拉新 DeviceInfo）。shaderArtifacts 自检 16→18（cull.cs spv/dxil）。
- 离线对照：`tools/frustum_check.py` 逐行复刻 camera.cpp（perspectiveZO×lookAt 列主序）+ 同 5 平面球测试，读 .nmesh 簇球；`--expect N` 锁数字、`--list` 出可见簇号；`tools/frustum_check.bat` 可执行 shim（regression 无 shell 启动 .py）。

**提交版本**：s4-cull（本轮）

**完成情况**
- **pass-through 零像素影响**：正对机位（yaw30/pitch20/dist2.6，M4.0 已证字节一致的同机位）`--mode cull`（frustum on，237 全可见）与 `--mode cluster` 基线逐字节相同，DX12/Vulkan 各自一致，双后端 3 连跑稳定。
- **真剔除 GPU==离线**：进模姿态（yaw12/pitch9.2/dist0.45，640x720）GPU 回读 `cull stats: visible 30 / 237`，`frustum_check.py --expect 30` MATCH，双后端一致；正对 237/237 离线同值。HUD frustum 行与日志同数，截图确定性已验（同机位两跑逐字节相同）。
- **总闸**：`regression.py` 164/164 ALL GREEN（bytes 164 / near 0 / FAIL 0）；`server.py --check` OK（stage=S4.1 cases=2 baselines=6）；selftest 15/15（shader artifacts 18/18 含 cull.cs）。
- 交代物：`example/s4-cull/`（6 PNG + run.bat 三段验收 exit 0）+ cases.json s4.1 卡（cull-front pair / cull-inside-hud image+expectLog）+ s4.1-tools 卡（3 个 kind:tool 用例）+ `docs/s4/S4-TOOLS-GUIDE.md` 归档。
- **如实修正（S4-PLAN 同步改）**：轨道相机永远 look at 原点，「背对」姿态整模仍在视锥内（dist4.2 视锥半高≈1.74 > 模型半径≈1.3），S4-PLAN「背对≈0」前提不成立；真剔除维度=近平面（进模 dist0.45），M4.2/S4 验收姿态表述同步修正。

**踩坑情况**
1. **HUD 数字晚一帧**：HUD 帧首读 info，poll 在 renderFrame 内才更新 → HUD 显示比回读晚一拍；`--frames 3` 截图 HUD 显示 `--`，frames≥4 出数（readback 本身 2 帧延迟 + info 一帧滞后）。截图证据统一 frames 4。
2. **宽高比进视锥**：640x360 下进模可见 64、640x720 下是 30——aspect 改变水平视锥，两个数都对；离线对照必须带同尺寸（`--width/--height`），用例 note 已写明。
3. **默认机位 pulling vs cluster 有 1 个取整平局像素**（maxDelta 154）：cull-front pair 初版用默认机位 yaw12/pitch9.2/dist4.2，回归 1 像素 FAIL；cull 渲染自身 5 连跑字节稳定（非原子序非确定），是 pulling 与 cluster 两路径在该机位的浮点平局。换到 M4.0 已证机位 yaw30/pitch20/dist2.6 后双后端 3 连跑字节一致——pair 用例必须落在已验证机位。
4. **NVRHI 多状态缓冲**：visibleList 同帧当 UAV（compute 写）+ SRV（VS 读）+ identity 上传，indirectArgs 同帧 UAV + IndirectArgument + CPU writeBuffer——`setPermanentBufferState` 报状态位错、动态跟踪不跨 command list 报 Unknown prior state → 统一 `initialState=CopyDest + keepInitialState=true`（NVRHI 标准多状态模式）。
5. **UAV 绑定校验三连**：visibleList 缺 `canHaveUAVs`、args 缺 `structStride`、args 缺 `canHaveUAVs`，逐个补齐。
6. **run.bat 延迟展开陷阱（连 s4.0 一起修）**：`%EXE%` 在 for 块内解析期展开为空 → 执行字面 `""` 命令静默 no-op，s4-indirect 旧脚本同病（M4.0 的 run.bat verify 从未真正跑过——假绿实锤）。两份脚本改 `setlocal enabledelayedexpansion` + `!EXE!`，修后 s4.0 也 PASS。
7. **杂键注入偶发**：自动化跑批时 viewer 窗口偶收 'C' 键切 back-face（日志可见 `back-face culling -> off`），84% 像素差特征明显；单跑复验即消失，属环境偶发非代码问题，总闸重跑全绿。

**下一步**：M4.2 HUD 统计完善（cull pass 接 GpuTimer slot）+ 1M 三角 dense 资产（icosphere 升一级细分入仓）+ 60fps 性能验收，tag `s4-done`。

## B2~B4 材质图轨道第一批（A 类并行轨道，feat/material-graph，2026-11-08）

> 详细验收纪要：`docs/material-graph/B2-B4-ACCEPTANCE.md`（固化文档闸要求，merge 回 master 前置）。

**动作**：B 路自建轻量材质图三步落地——B2 节点图数据模型（Node/Pin/Edge + validate + material_graph v1 确定性文本序列化）；B3 图→Slang 代码生成（纯函数 emitter，Output 上游 DAG 递归求值，产物符合 model.slang 约定：vsMain/psMain + register(b0/t0/s0) + SceneConstants）+ slangc 双后端 × vs/ps 四入口真实编译验收；B4 热重载（watch 轮询 mtime，复用 recompileGraph 单一链路）+ `render` 子命令从 .mg 真实数据出 SVG 证据图；主干 8faad52 同步合入（b2d1050）后复验。

**提交版本**：feat/material-graph `1a912fa`(B2) / `0cb2593`(B3) / `b01959e`(B4) / `3a22b40`(render 修复) / `b2d1050`(主干同步)

**完成情况**：ctest b2-graph-selftest / b3-graph-genslang / b4-graph-hot-reload 全绿（主干同步后 3/3 复验）；生成 .slang 经 slangc SPIR-V+DXIL 双后端四入口全过（genslang: OK）；热重载实测后台轮询检测到 .mg 变更并二次重编译 OK；证据图 b3-nodegraph.{svg,png}（4 节点 3 连线，非手绘）。B 路阶梯推进至 B4，B5（接 PBR 材质 pass）排 S7 后。

**踩坑情况**：① junction 共享 .workbuddy 本地改动挡 merge 且 skip-worktree 下 stash 不可见——物理备份→临时 no-skip-worktree 还原→merge→cp 恢复→重设；② reg.exe 被沙箱拉黑致 vcvarsall 缺 SDK——手动注入 SDK 路径，构建批处理禁中文注释（代码页拆行）；③ 匿名命名空间内 main 无外部链接致链接失败；④ system() 路径无条件加引号会被 cmd 解析坏，含空格才加；⑤ watch 长进程管道全缓冲致实时输出不可见——setvbuf _IONBF；⑥ render 节点高度漏算属性行致 value 文本溢出——统一 nodeHeight() 口径。

## M4.2 HUD 统计 + 1M 场景 + 性能验收（tag `s4-done`，2026-11-08）

**动作**
- **cull pass GPU 计时**：`Scene` 持第二个 `GpuTimer` 实例（`cullTimer_`，setLabel("cull")），begin/end 只包 `Scene::draw` 的 compute 剔除段（args 重写 + dispatch + 回读拷贝）；`GpuTimer` resolve 日志加实例标签（`[frame]`/`[cull]`，只影响日志文本不碰基线）。backend 在 `hud_.build()` **之前**调 `scene_.pollCullTimer(device, hud)`（与整帧计时同位同序）；`pollCullStats` 保持只做回读。`HudFrame` 加 cullMs/cullMsHistory[120]/cullMsCount/cullTimingAvailable；viewer 维护 cullHistory 曲线 + deterministic 固定样本 kFixedCullSamples（0.08ms 量级）；HUD 在 frustum 行下新增 `cull GPU x.xxx ms` 文本 + 曲线（仅 cull 模式占位，其它模式布局一像素不动）。
- **1M 资产入仓**：`gen-dense-mesh.py --shape icosphere --subdiv 8` → `IcoSphereDense1M.glb`（655,362 顶点 / 1,310,720 tri，md5 `ea0b6749d0a681c0610bd23882a072e1`，36.7MB）→ `nanite-tools cluster` → `IcoSphereDense1M.nmesh`（19,718 簇，avg 66.5 tri/簇，coverage 100%，limits PASS，33.5MB）。S4-PLAN 预估 ~6554 簇，实测 19,718（meshoptimizer 按簇上限切得更细），如实记录。
- **CPU 帧耗时日志**：截图模式（无 16ms 节流）累计 dtSum/dtMax，收尾打 `frame timing: avg X ms max Y ms over N frames (unthrottled)`——60fps CPU 侧机械证据，只进日志不进画面。
- **面板高度上限 510→560**：cull 模式新内容（frustum 行已有 + cull GPU 行 + 曲线）超出 510 出滚动条；小画布仍由 `min(cap, H-28)` 兜底，行为不变。这是 MEMORY「面板高度≤510」约定的一次显式修订（内容驱动，非随意放宽）。
- kStage → `S4.2 GPU frustum culling + perf acceptance (cull pass GPU timing, 1.3M-tri dense scene)`；selftest stage 断言同步改。

**提交版本**：s4-done（本轮）

**完成情况**
- **1.3M tri pass-through**：正对 640x360（frustum_check 640x360 = 19718/19718 全可见）`--mode cull` 与 `--mode cluster` 逐字节相同，且 **DX12==Vulkan 跨后端也逐字节一致**（bytes 最优档）。640x720 下 frustum 剔 1353 簇、GPU 位图仍与 cluster 一致（包围球测试保守性：视锥外簇零像素贡献的实证）。
- **真剔除 GPU==离线**：进模 dist0.45/640x720 GPU 1932/19718（剔除 **90.2%** > 85% 判据）== `frustum_check.py --expect 1932`，DX12/Vulkan 一致；封闭球从内部看全背面、cullBack 画面为空属正确行为（Suzanne 是开放壳所以有画面），可见列表真实光栅化由 `--cull none` 内壁满屏簇色马赛克证明（跨后端 bytes 一致）。
- **60fps（RTX 4060 Laptop）**：CPU avg 4.87~5.55ms（30 帧无节流，max 尖峰为首帧预热）；整帧 GPU 0.96~1.02ms（正对）/ 0.17ms（进模）；cull pass 恒约 **0.010ms**（19,718 簇一次 dispatch）——距 16.6ms 判据两个数量级，双后端互证。
- **总闸**：`regression.py` **176/176 ALL GREEN**（bytes 176 / near 0 / FAIL 0）；`server.py --check` OK（stage=S4.2 version=s4.2 cases=3 baselines=9）；双后端 selftest 15/15。
- 交代物：`example/s4-done/`（8 PNG + run.bat 四段验收 exit 0）+ cases.json s4.2 卡（dense-front-pair / dense-inside-hud / dense-inside-none）+ s4.2-tools 卡（2 正向 --expect + 1 负向 exit 1）+ `docs/s4/S4-TOOLS-GUIDE.md` M4.2 节 + `docs/screenshots/s4-front.png`/`s4-inside.png`（HUD 93.1% vs 9.8% 对比）。

**踩坑情况**
1. **cull 计时轮询时序**：初版把 `pollCullTimer` 放在 `pollCullStats`（`hud_.build()` 之后）——viewer 每帧重建 `HudFrame{}`，晚于 build 写标志当帧即丢，HUD 的 cull GPU 行永远显示不出来（整帧计时在 build 之前轮询所以从未踩过）。修为与整帧计时同位：backend HUD 分支内、build 之前调 `pollCullTimer`。
2. **batch else 块里的括号**：run.bat `else (echo ... (all visible ...))` 文本中的 `)` 把 else 块提前闭合，脚本静默断尾。echo 文本禁裸括号。
3. **宽高比坑重现（M4.1 踩坑②）**：run.bat 离线 front 对照初版写 18365（640x720 值）对照 640x360 GPU 用例 → MISMATCH；640x360 实为 19718 全可见。离线对照必须与 GPU 用例同尺寸，M4.2 再验一次。
4. **基线维护（预期内）**：kStage bump 重拍 m0.9/s2.0/s3.2 三对 HUD 基线之外，`s4.1/cull-inside-hud` 一对因 cull 模式 HUD 新增两行也须重拍——HUD 布局演进会波及所有含该模式的 HUD 基线，总闸兜住。

## P8 M-J0 线程池 + parallel_for 最小内核（tag `job-pool`，2026-11-08，feat/job-system 轨道）

**动作**：按铁律三处实读定稿（bevy_tasks 固定池+scope 主线程驱动 / UE ParallelFor MinBatchSize 切块 / Carrot 无 TaskPool=反向证据）→ 新建 `core/include/nl/core/jobs.hpp` + `core/src/jobs.cpp`（ThreadPool：固定 N 线程 + 共享竞争队列 + 主线程参与执行；`parallelFor(count, body(begin,end), minBatch)` 切块）→ `nanite-tools selftest jobs` 七项机械单测（求和 50 轮随机规模 vs 公式 / 槽位并发写 / minBatch>count 单批 / count=0 / 批次异常重抛+池复用 / 重复运行×3 线程数变体 1/4/硬件 / 参考耗时打印）→ CMake nl_core 加 jobs.cpp + ctest p8-jobs-selftest。

**提交版本**：job-pool（feat/job-system 分支，worktree `E:/UE_STUDY_2026/nl-engine-job-system`，A 类轨道不进 kStage/渲染回归）

**完成情况**
- selftest jobs 全绿 exit 0（7/7 ok）；ctest 16→17 全绿（16 条既有全数通过，纯新增零回归）。
- 判据逐条对上 P8-JOB-PLAN M-J0：求和无差 / 无 data race（槽位写）/ count=0 安全 / 异常不拖垮池。
- 零图形符号（core/jobs 仅 mutex/cv/deque/thread/vector），不引第三方线程库。
- worktree 构建：手工注入 MSVC 14.38 + SDK 10.0.26100.0 环境（见踩坑 1）。

**踩坑情况**
1. **worktree 全新构建环境坑**：vcvars64.bat 内部调 `reg.exe` 查 SDK 路径，本会话沙箱 Program Blacklist 拦截 → SDK 环境残缺 → 链接 RC Pass 报 `CMAKE_MT-NOTFOUND` / "no such file or directory"。对策：不调 vcvars，bat 里手工 `set PATH/INCLUDE/LIB`（MSVC 14.38.33130 + Win SDK 10.0.26100.0，版本对齐 M0.0 环境记录）。另：Git Bash 传引号给 cmd 会被 MSYS 转义成 `\"` 字面量（连续两次踩），统一用临时 .bat 承载构建命令。
2. **设计取舍（留痕）**：不做 per-thread deque 窃取——共享队列 + mutex/cv 足够，离线管线任务粒度大（每组数千三角）锁竞争可忽略；Carrot 反向证据「不过度设计」。接口边界写死在 jobs.hpp：parallelFor 不可嵌套调用同一实例（嵌套支持 M-J1 scope 落地）、多线程不可并发调同一实例的 parallelFor。

**下一步**：M-J1 scoped fork-join（`pool.scope(s.spawn)` 返回 Vec<T>、可嵌套、与 parallel_for 共用同池）→ M-J2 cluster/stat 并行接入。

## P8 M-J1 scoped fork-join（tag `job-scope`，2026-11-08，feat/job-system 轨道）

**动作**：worker 语义重构（任务闭包自含完成计账：parallelFor 用 Impl.pending、scope 用 Scope::State.pending；锁顺序恒 st.mu→im.mu）→ `ThreadPool::scope<T>(fn)` 同步 fork-join（spawn 可嵌套、结果按 spawn 序收集、异常全 join 后重抛、与 parallelFor 共用队列）→ selftest 扩至 13 项。

**提交版本**：job-scope（commit 4e92cf7）

**完成情况**
- 新增 6 判据全绿：spawn 顺序收集（100 任务）/ 8x4 嵌套 spawn 无死锁全 join / 3 层链式 spawn 数据流 / 空 scope / scope 异常重抛 / 异常后池复用。
- ctest 17/17（首轮 s1-nmesh-suzanne 空输出失败 = Defender 首启扫描拦截新 exe 的 flake，单独+全量复跑均绿）。
- 生命周期：C++ 无借用检查，靠 scope 同步语义保证 State 裸指针无悬空（join 前任务全完成）——hpp 注释钉死。

**踩坑情况**
1. 嵌套类模板声明的名字查找两处修：scope() 签名需要 `template <class T> class Scope;` 前置声明；实现里访问 `typename Scope<T>::State` 限定名。第一版 hpp 里混入不存在的 `st_pending()` 伪函数（Write 时手误），编译前自查抓掉。

## P8 M-J2 cluster 并行消费者（tag `job-cluster`，2026-11-08，feat/job-system 轨道）

**动作**：定位热循环（结论：贪心切簇 mesh 内串行依赖不可并行=算法本性；真实并行点=多子网格 per-primitive；stat 无热循环保持串行并留痕）→ `computeClusterResult`（verify+统计，无 I/O）从 `printClusterResult` 拆出 → 多子网格 `scope` 并行 slot 输出、打印保序串行、单子网格保持原串行路径 → `scripts/make-multi-primitive-glb.py`（复制 primitives 成 4 submesh，同 accessors）→ selftest 并行 vs 串行逐字节断言 → 展示台 p8-jobs 卡（selftest / cluster 并行 / 负向 unknown suite，expectExit 0/0/1）。

**提交版本**：job-cluster（本轮）

**完成情况**
- 确定性硬判据：合成 8 primitive 并行 vs 串行，BuiltMeshlets（meshlets/vertices/triangles/bounds）逐字节 memcmp 一致 + ClusterSummary 逐字段一致。
- 命令行：`cluster Suzanne-multi.glb` 4 submesh 并行 limits PASS；耗时串行 4x 440ms vs 并行 194ms（约 2.3x，进程级含加载）。
- ctest 17/17；server --check 挂在主线 s4.2 未归位（kStage 已 bump 而主线 example/ in-flight），p8-jobs 卡自身归位无报错——A 类轨道不背主线 kStage 锅，留痕。

**踩坑情况**
1. **Edit 假成功 + cwd 漂移叠加坑**：一轮 Edit 报成功但 bash grep 看不到新内容——真相是 Bash 工具 cwd 漂回主 worktree，grep 看到主 wt 同名旧 main.cpp（主 wt 是 S4 版本无新函数）。对策：核实文件一律用 Grep/Read 工具（显式绝对路径），Bash 每条命令显式 cd worktree 根或用绝对路径。同一轮里 Edit old_string 撞 clusterPrimitive 连体段误删该函数 + 旧 printClusterResult 残留成重复定义——Grep 定位后一次 Edit 恢复（clusterPrimitive 归位 + 删旧版），verifyCoverage 前向声明补链。
2. ctest 单测 exe 首启被 Defender 实时扫描拦截（输出空、秒退）——首个全量 ctest 轮次 flake，复跑即绿。新 exe 首次跑测前先手动跑一遍暖机。

**下一步**：M-J3 simplify / dag 并行化（METIS 分组按组并行 + 逐组误差自底向上并行、合并链串行）。

## P8 M-J3 lod 链并行化（tag `job-offline`，2026-11-08，feat/job-system 轨道）

**动作**：代码定位澄清（cmdDag 是纯校验/导出——误差树在 lod 层级循环已写盘，DAG 误差自底向上 per-group 段落在 rebuildMeshletsPerGroup 的组误差计算）→ 并行安全审查（组间边界锁→非边界点单组独占；QEM dst 位置不动→pointPositions 只读；共享可变状态为零）→ simplifyGroups 步骤 4 两阶段化（并行逐组 QEM + 串行前缀拼 soup）→ rebuildMeshletsPerGroup 两阶段化（并行重切+组误差/组球 + 串行窗口拼接）→ LodOptions.threads（UE ForceSingleThread 同款）+ lod 命令 --threads → 展示台 lod-parallel 用例 → selftest 双池断言。

**提交版本**：job-offline（本轮）

**完成情况**
- **三方 MD5 确定性**：lod Suzanne 7 层——并行（threads=0 硬件）== 改造前串行基线（/tmp/mj3-baseline 全 7 层逐字节）== threads=1 参考跑；dag 56 groups OK。
- IcoSphereDense（4929 簇）4 层 threads=1 vs threads=32 全 5 层 MD5 MATCH；加速 4.448s → 1.550s（**2.87x**）。
- selftest jobs 15 项全绿（新增 lod 链 threads=1 vs parallel 全链 meshlets/vertices/triangles/bounds/lodMeta 逐字节）；ctest 17/17（复跑）。
- 并行边界已写死进 P8-JOB-PLAN 与 lod.hpp/simplify.hpp 注释：逐组 QEM/重切/组误差并行；soup 前缀拼接与层级推进串行；METIS 保持单点（全局分区不按组可分）。

**踩坑情况**
1. **ctest 冷启动 flake 定性**：重建 exe 后头 1-2 轮 ctest，nanite-tools 前几条测试**空输出 exit 0 秒退**（挂的条目不定：M-J1 挂 #6、M-J3 挂 #6 / #5+6 / #6）；-R 单跑必过、手动直跑必过、连续第 2-3 轮全量全绿。定性 = Defender/AMSI 对新 exe 高频 CreateProcess 的静默拦截（环境问题非代码问题）。对策：构建后先手动跑一次暖机，验收以复跑全绿为准。
2. **Edit 大段替换的连带破坏**：printClusterResult 拆分时 old_string 连带吞掉 clusterPrimitive + 旧函数残留重复定义；cmdLod 计时改造时误删 levels 打印行 + 残留旧 buildLodHierarchy 块成重名变量——均靠 Grep/Read 定位后一次性恢复。教训：大段 Edit 后必须 Grep 全部相关符号核对布局再编译。

**下一步**：M-J4（可选，P9 V4 预留）不排期；P8 并发地基 M-J0~J3 收口，后续按 ROADMAP 接续（VSM 插 S9 后 / TSR 拆 S7 后两段；S5 组原子 cut 可直接消费 DAG 组边界）。
## P8 合并回 master + 回归门禁固化（exe 新鲜度闸 + 基线修正，2026-11-08）

**动作**：主干先合入 feat 分支消化分歧（fd53670：解 CMakeLists/MILESTONES/cases.json 三处内容冲突 + PARALLEL-TRACKS/P8-JOB-PLAN 两处 add/add）→ feat/job-system 快进合回 master → 合并树双后端重编 + ctest 19/19 → 全量总闸揪出 4 对 vulkan HUD 基线为 M4.2 旧 exe 误拍（m0.9/gate-hud、s2.0/lod-hud、s4.1/cull-inside-hud、s4.2/dense-inside-hud）→ 逐对像素取证确认 actual 为正确产物（跨后端差异 895~907px = backend/debug 两行设备文本的既知口径）后重拍 → 按用户要求把「exe 版本」问题固化进门禁：`regression.py` 启动即查 `build/<backend>/*.exe` mtime 是否新于其依赖域全部源文件，过期整闸拒绝（exit 2）→ 增 `NL_REGRESSION_RUNS_DIR` 产物目录覆盖（新目录全程零删除，CI/沙箱友好）。

**提交版本**：本轮（master，合并 commit fd53670 + 门禁固化 commit）

**完成情况**
- 合并零冲突残留；合并树 selftest jobs PASS + ctest dx12/vulkan 各 19/19。
- 4 对 vulkan 基线重拍后双后端逐字节 PASS；最终全量串行总闸 184 条 ALL GREEN（启动拦截 flake 以串行复跑定性，见踩坑 4）。
- exe 新鲜度闸负向实测：touch renderer/src/hud.cpp 后跑闸被拒并列出落后明细；正向：重编后放行。
- 门禁改动落在 `tools/regression.py`：`check_exe_freshness()`（按 exe 依赖域分桶）+ `NL_REGRESSION_RUNS_DIR` + 模块文档同步。

**踩坑情况**
1. **M4.2 vulkan 基线旧 exe 误拍（假绿根源）**：重拍基线时 dx12 用新 exe（面板 cap 560，底边 y525）、vulkan 用旧构建（cap 510，底边 y523，s4.x 两条还是带滚动条裁底的面板）→ 双后端基线口径不一致，一直假绿到 P8 合并后的总闸才炸出。教训已固化进门禁：**跑闸/重拍基线前 exe 必须新于源码，否则拒绝运行**——基线重拍永远用刚重编的 exe。
2. **NMake 按 TU 依赖重链 → 全局源码比对死锁（用户点名固化）**：`touch renderer/src/hud.cpp` 后 NMake 只重链依赖该 TU 的 viewer.exe；nanite-tools / material-graph-tools 不含此 TU 不会重链，exe mtime 保持落后。若门禁用「全局 SOURCE_DIRS 比对所有 exe」会造成无关源文件一碰、工具 exe **永不新鲜**的死锁（重编也没用，NMake 认为它已是最新）。对策：`EXE_SOURCE_DIRS` 按 exe 依赖域分桶——viewer=core+renderer+shaders+viewer/，nanite-tools=core+nanite-tools/，material-graph-tools=core+material-graph-tools/；无关 touch 不再误伤，真依赖变更仍被拦。
3. **沙箱 safe-delete 累计删除阈值杀长跑**：WorkBuddy 沙箱按会话轮累计删除文件数（阈值原 50），全量总闸运行中的产物清理会撞线被杀（进程无 traceback 死在中途，块缓冲还会吞掉已跑输出的尾部）。对策两层：① `NL_REGRESSION_RUNS_DIR` 指向全新目录 = 全程零删除事件；② 用户已把阈值调到 500。另：长跑 python 一律 `python -u`（无缓冲），避免死因诊断被缓冲吞尸。
4. **启动拦截 flake 与真像素 FAIL 的鉴别**：本环境全量跑平均挂 0~3 条，特征两种——「日志缺 X」毫秒级秒退 / 超时（Defender 高频启动拦截，串行复跑必过，条目每轮不同）；与「与基线像素不符」的确定性 FAIL（本轮 4 条 vulkan 基线即此类，逐对像素取证定位）严格区分，前者复跑定性、后者必须查根因不许复跑糊弄过去。

**下一步**：P8 并发地基全部收口（M-J0~J3 已入主干）；主线 S5（组原子 cut）/S6 排期按 ROADMAP；feat 分支任务完成后可清理 worktree 登记。

## P8 支线：可视化证据图补交（2026-11-08，主 worktree）

**动作**：登记支线（bb3680c）→ 主 worktree 真实跑工具留档（selftest jobs / cluster Suzanne-multi / lod IcoSphereDense threads=1 vs 32 / 负向 selftest nope）→ 逐层 hashlib MD5 比对 6/6 MATCH → 新增 `scripts/make-job-evidence.py`（Pillow，计时与 MD5 全部从真实日志/产物解析，无硬编码）→ 生成 `example/p8-jobs/` 4 张证据图 → `cases.json` 4 条 tool 用例 `preview` 回填（diff 仅 +9/-5）。

**提交版本**：本轮（master）

**完成情况**
- 4 张图：`lod-parallel.png`（2.98× 加速条形 + 6 层 MD5 表）、`selftest-jobs.png`（26 项 PASS）、`cluster-parallel.png`（4 子网格统计 + 并行计时）、`negative-unknown.png`（exit 1 拒绝）；全部亲眼验收无重叠/截断/乱码。
- `server.py --check` 当前 FAIL 项为「缺 s5.0 版本用例」——归属 S5.0 主线收口件（主 wt 存在另一条 S5.0 工作线未提交占坑：kStage 已 bump、camera/hud/scene/viewer 等成批修改 + lod_error 新增）。本支线不触发任何 check 项。
- 支线节在 `docs/p8/P8-JOB-PLAN.md` 全勾选并留痕。

**踩坑情况**
1. **`lod -o` 是文件前缀不是目录**：`-o build/.../lod` 产出 `lod.lod0.nmesh` 等同目录平铺文件，按目录 md5sum 会空手而归；MD5 比对按 `lod-dense-{t1,tp}.lodN.nmesh` 逐文件做。
2. **certutil 输出 GBK 解码炸 UTF-8 reader**：证据脚本改用 `hashlib` 本地直接算 MD5，更干净且不依赖外部命令编码。
3. **json 重写注意原缩进**：`cases.json` 原为 1 空格缩进，round-trip 用 `indent=1` 才不会整文件 diff 噪音（先误用 indent=2 产生 ~500 行假 diff，已 `checkout --` 重做）。
4. **cluster 证据图首版只渲染最后一个 submesh**：按 `file` 行重置缓冲的逻辑 bug，改为顺序累积后 4 个子网格齐全——生成器自己也要亲眼验收，不能只看「脚本没报错」。

**下一步**：本支线闭环；等 S5.0 主线收口时 `server.py --check` 随 s5.0 版本归位转绿。

## M5.0 误差投影：世界 → 屏幕像素（tag `s5-proj`，2026-11-08）

**动作**
- **决策前置**：三处参考实读——bevy `cull_shared.wesl:13`（`lod_error_is_imperceptible`，溯源 zeux meshoptimizer `demo/nanite.cpp:115`）、UE `NaniteClusterCulling.usf:255` + `NaniteCullingCommon.ush:112` `GetProjectedEdgeScales`（切线投影）、Carrot `draw_virtual_geometry.slang:81`（flat 双阈值）；开发历史 M3.2 已存档「bevy 屏幕空间公式 S5 用」。发现 S5-PLAN 原文「distance 用相机平面距离」与已存档口径冲突 → 摆证据请用户拍板，**拍板 bevy/zeux**：`screenPx = worldError / max(d, near) × projYScale × 0.5 × H`，`d = |球心−eye| − 球半径`。
- **core 纯数学层**：`core/include/nl/core/lod_error.hpp` + `core/src/lod_error.cpp` `screenErrorPixels()`（零图形符号）+ `lodErrorSelfTest()`（45° fov 手算逐例对比）；CMakeLists 加 `core/src/lod_error.cpp`。
- **OrbitCamera 补只读访问器**：`eyePosition()`（与 `viewProjection()` 的 lookAt 同源公式，不另造一套）、`projectionYScale()`（=1/tan(fovY/2)）、`nearPlane()`。
- **数据通路**：`Scene` 存资产联合包围球 `assetSphere_`（bbox 中心 + 到最远顶点，M5.0 注释明确下钻时优先组记录球）与组球 `dagGroupSphere_` 透出；DeviceInfo/HudFrame 加 `assetSphere[4]/dagGroupSphere[4]/screenErrorPx/screenErrorValid/cutThresholdPx`（POD 通道，两后端对称补齐）；viewer 主循环按 `drilled ? 组球+组误差 : 资产球+顶层 maxError` 计算并喂 HUD。
- **HUD**：dag 模式加 `s5 err %.2f px` 文本行 + `cut thr px` 滑条（0.25~4.0 默认 1.0）；非 dag 模式布局一像素不动。
- **文档同步**：S5-PLAN M5.0 公式行按拍板修正（旧口径划掉留痕）；新建 `docs/s5/UE-DIFFS.md`（与 UE 不一致的集中台账，D1~D7 + 待办 D8/D9 + 对齐确认），AGENTS.md「偏离官方口径」条目下固化台账入口（2026-11-08 用户点名）。
- kStage → `S5.0 screen-space error projection (zeux/bevy cut formula, threshold HUD)`；selftest stage 断言同步改。

**提交版本**：s5-proj（本轮）

**完成情况**
- **数值复核（亲眼可见）**：dag-top + HUD 三机位 dist 2.0/4.2/8.0 → `s5 err` 1567.05 / 352.15 / 149.84 px 单调下降；Python 解析 `Suzanne.lod6.nmesh`（顶层 maxError=1.1375、联合球 R=1.2661、eye 公式逐项复算）**逐位一致**。
- 双后端 selftest **16/16**（含新增 `lodErrorSelfTest`）。
- **kStage bump 波及面（含认知修正）**：HUD 标题行含 stage 字符串，但**只有 exe 用当前构建的版本才需要重拍**——m0.1~m0.8/s1.0 的回归走 tag 快照 exe（`build/showcase/<tag>/`，HUD 内嵌当时代 stage 字符串），基线不随 kStage 变。实际重拍 = 当前 exe 的 5 对（m0.9/gate-hud、s2.0/lod-hud、s3.2/dag-hud、s4.1/cull-inside-hud、s4.2/dense-inside-hud，双后端 10 张）。
- **总闸（按需口径）**：3 条关键用例回归 6/6 bytes（s3.2/dag-hud = 本次唯一布局变化 + m0.9/gate-hud + s4.1/cull-inside-hud = stage 行验证）；s5.0 新卡 4/4 bytes（s5-hud pair ×2 + s5-selftest ×2）；`server.py --check` OK（stage=S5.0 version=s5.0 cases=2 baselines=3）。
- 交代物：`example/s5-proj/`（s5-hud 双后端基线 + 3 机位证据图 + run.bat verify exit 0）+ cases.json s5.0 卡（s5-hud pair + s5-selftest tool）+ `docs/s5/UE-DIFFS.md` 台账。

**踩坑情况**
1. **Python 手算复核的 struct 布局连环错**：`NMeshHeader` 实为 **20B**（4+2+2+4+4+4，先误按 24B 解析整体错位 4 字节）；`NMeshMetaHeader` 解析 format 串连错三次（`<IIHfII`→`<IIHHfI`→正解 `<IHHfII`，把 version+level 两个 u16 误并成一个 u32 时数值看似「随机」实为字段拼接）。教训：对账先 hexdump 定 magic 实际偏移反推记录大小，再逐字段解，别对着记忆写 format 串。
2. **HUD 读数须以本机像素为准**：d=4.2 机位早先视觉读数记成 332.15，重看截图确认实为 **352.15**（3↔5 误读）——视觉读数进验收纪要前必须二次确认或直接用可机读通道。
3. **run.bat for 循环内 echo 行尾 `)` 会被当成块结束符**：`echo ...(dx12 + vulkan)` 输出被截断括号，纯文本瑕疵但会误导证据阅读；避免在 for 块内的 echo 行尾用裸括号。
4. **基线重拍 exe 必须与回归用的 exe 同源（本轮自摆乌龙，被总闸当场抓住）**：`cases.json` 里 m0.1~m0.8/s1.0 的 `exe` 指向 **tag 快照**（`build/showcase/{tag}/build/…`），HUD 内嵌快照时代的 stage 字符串；我用当前 S5.0 exe 给这 8 对基线重拍 → `m0.5/resize` 回归立即 FAIL（91k 像素差，maxDelta 242）。修正：`git checkout --` 还原历史版本基线（它们本就不随 kStage 变），只重拍 exe=current 的 5 对。**教训**：重拍基线前先查该版本 `exe` 字段指向快照还是当前构建；「重拍永远用刚重编的 exe」只对 exe=current 的版本成立（M4.2 踩坑 1 的适用边界在此）。

**下一步**：M5.1 compute DAG 遍历（显式栈从根组下钻、簇屏幕误差 ≤ 阈值写 indirect 命令、栈溢出宁粗勿漏；输出接口 = S4 indirect buffer 不另起炉灶）。D1 升级路径监控点：M5.1/M5.2 偏轴或进模场景若选层明显偏粗，把 `screenErrorPixels()` 换成 UE 切线投影（见 `docs/s5/UE-DIFFS.md`）。

## M5.1 compute DAG 遍历（tag `s5-traverse`，2026-11-08）

**动作**
- **决策前置（铁律执行，含一次失职补课）**：开工时拿 M5.0 的公式证据顶数没重读遍历结构参考，被用户两连问揪出 → 补读三处参考并摆对照表：bevy `cull_bvh.wesl`（BVH 8 叉 + 全局队列 + 多 pass，cut = `parent_is_imperceptible || !aabb_in_frustum` 停止下钻）、UE `NaniteCulling.cpp`/`NaniteClusterCulling.usf`（DAG 节点遍历 + 组原子 FClusterGroup）、Carrot `draw_virtual_geometry.slang`（flat 无遍历，已记 D3）。AGENTS.md 追加铁律条款并单独提交（a762fb8）：「证据与当前里程碑模块一一对应：新里程碑开工时必须按 ROADMAP 对照表重读本里程碑点名的文件，不许拿上一里程碑的部分证据顶数」。
- **资产不变量实测（Python 逐层解析 Suzanne.lod0..6）**：成员方向**不连续**（lod0 全部 31 组非连续）→ 必须 CSR 拍平，不能断言区间；生成方向区间反转干净（每粗簇恰一生成组）；父簇 error = 生成组 error（lod.cpp:128 覆写）≥ 任一子簇 error → 单调性数学恒成立，不用运行时防御。
- **数据布局（initDag）**：`renderer/src/scene.cpp` 探测 `<stem>.lod0..N` 全层加载（≥2 层 + dagPresent 校验）→ 全层拼接 vertexPool/localIndices/clusterMeta（vertexBase 重定基，localBase 逐簇取 `localIndices.size()` 天然全局）+ 逐簇 error/level/genGroup SSBO + 组记录 float4×2（`[2g]=(cx,cy,cz,r)`，`[2g+1]=(err,memberFirst,memberCount,0)`；整数值 `static_cast<float>` 打包，shader 侧 `uint()` 数值转换还原）。DagConstants 独立 volatile CB 48B（`g_dagProj/g_dagEye/g_dagRoot` 三个 float4），不动 SceneConstants 160B 契约。
- **遍历 shader**：新建 `renderer/shaders/dag.slang`：每根簇线程显式栈 `uint[512]`，pop → screenPx ≤ thr 或叶层 → `emitCluster(c)`，否则组展开压成员；组重复下钻用 `dagGroupExpanded_` 声标志 + `InterlockedExchange` 三参形式去重（同组多个同误差父簇全失败会重复压成员，实测放大到 54923 emits）。vsCull/绘制链零改动，输出复用 S4 visibleList + indirectArgs。
- **viewer/HUD**：`ViewMode::Dag` + `--mode dag` + `--cut-thr` CLI（复用 M5.0 的 `Options::cutThresholdPx`）；dag 帧参数 eye/projYScale/nearPlane/viewportHeight/cutThresholdPx 走新 CB；HUD 滑条与 `s5 err` 行复用 dagMode；截图日志打印 `dag stats: visible N / 324 clusters`。
- **M4.1 潜伏 bug 修复**：`--frustum off` 因 visibleList 条件创建而布局无条件声明 t4 → set 未绑定 init 即败（viewer 默认 on 从未被用例踩到）。修正：visibleList_ 恒创建恒绑定 t4、恒等列表恒上传。
- 交代物：`example/s5-traverse/`（near/far × 双后端 4 张基线 + run.bat verify，findstr 断言 `visible 237 / 324` 与 `visible 14 / 324`）+ cases.json s5.1 卡（28 versions）；kStage → `S5.1 DAG traversal (atomic group cut, per-thread explicit stack)`。

**提交版本**：s5-traverse（本轮）

**完成情况**
- **选层谱单调（亲眼可见）**：dist 20 固定，thr 4/8/16/32/64 → visible 230/106/14/7/3 簇，连续过渡符合预期；thr=8（14 簇）/thr=16（7 簇）截图形状正确无洞无裂缝。
- **全细节等价（最强证据）**：`--mode dag --cut-thr 0.001` 与 `--mode cull --frustum off` 同机位**逐字节相等**（md5 f0b5db63）；dx12/vulkan 各自成立，且 dag 图**跨后端也逐字节相等**——遍历是纯确定性 compute，双后端同一份输入同一输出。
- **相机钳制解释**：OrbitCamera kMaxDist=20，dist 60/200 结果相同是相机钳制不是遍历 bug。
- 回归全绿（总闸按需口径）：s4.0 4/4（潜伏修复验证）+ s5.0 4/4 + s3.2 2/2 + s4.1 2/2 + s5.1 新卡 6/6，全 bytes；`server.py --check` OK（stage=S5.1 version=s5.1 cases=3 baselines=6）；双后端 selftest 16/16。
- kStage bump 波及面（M5.0 教训升级版）：只重拍 exe=current 的 6 版本 12 张 HUD 基线（m0.9/s2.0/s3.2/s4.1/s4.2/s5.0），tag 快照 exe 的历史版本不动（先查 exe 字段再动手）。
- UE-DIFFS 台账增补 **D3a**：每线程显式栈 + atomicExchange 去重 vs bevy 全局队列多 pass（有意简化，多实例/千组级负载不均时升级，输出接口不变）。

**踩坑情况**
1. **恒只画 3 簇（三连根因排查，本轮最大坑）**：dag 模式画面恒为 3 簇叶子。① 嫌疑 `asuint()` 位型重解释（组记录整数值经 float 打包，`asuint(float(7))` 得到的是位型 `0x40E00000`≈10.9 亿而非 7 → 栈溢出保护恒触发）→ 改 `uint()` 数值转换（必要修复但非充分）；② 通过 indirectArgs 的 args[1] 安全探针（把阈值值选为探针、量程限制防 instanceCount 爆炸挂进程）逐一验证 CB/level/error/members 数据全对、判定方向对、迭代数=1、分支=leaf；③ **真凶：根簇种子漏加根基址**——`dispatchThreadID` 0..2 是 lod0 叶子（level 0 恒 leaf emit），根簇应为 `g_dagRoot.x + dispatchThreadID`（=321..323）。教训：三维排查顺序=数据 → 判定逻辑 → 种子/寻址，探针通道（args[1] 回读）比猜快得多。
2. **`InterlockedExchange` 三参形式（E30019）**：Slang/HLSL 里是 `void InterlockedExchange(dst, val, out old)`，不能当表达式用；先声明 `uint claimed` 再判。
3. **shader 未重编假象**：dxil 时间戳早于 slang 源文件，SPIR-V 因 E30019 失败时画面用的还是旧 dxil——修完 shader 后确认构建管道真的重编了两个目标再验。
4. **组重复下钻指数放大**：一个组生成多个同误差父簇（genCount 最高 8），全部同时判失败 → 每个失败父簇都压同组成员 → 54923 emits。修复 = 每帧清零的 `dagGroupExpanded_` 声标志，赢家压栈、输家跳过；成员并集覆盖所有兄弟面积，不丢面。
5. **NVRHI `setInitialState` 是 Desc 方法非 handle 方法**：`DagGroupExpanded` 缓冲（canHaveUAVs + keepInitialState(CopyDest)）用独立 desc 创建，不能在 handle 上链。
6. **`camera.eyePosition()` 无返回值重载**：只有 `void eyePosition(float out[3])` 数组出参形式。

**下一步**：M5.2 时滞防抖 + HUD（误差阈值滞回：上帧选中簇本帧误差 < 阈值×1.1 不换，抑制边界抖动；GPU 原子计数总三角数实时显示；S5 收官自检=拉远三角数连续下降 + 固定视角数字稳定）。tag `s5-done`。

## M5.2：GPU 三角数 HUD（S5 收官，2026-11-09，tag `s5-done`）

**动作**：kStage → `S5.2 GPU triangle count (atomic counter in cull/dag compute, delayed readback)`。范围调整（用户拍板）：**砍掉误差滞回**——三处参考实读均无运行时滞回（UE 5.8.4 `NaniteClusterCulling.usf:258` 无状态双误差带 + TAA 掩盖、bevy `cull_bvh.wesl` 无状态、Carrot `draw_virtual_geometry.slang:106` 纯双误差判定），滞回需 per-cluster 上帧状态 buffer 主动破坏 M5.1 刚验证的「无状态、逐字节可复现」，且当前单资产规模未实测到抖动危害；与 UE 无状态口径对齐（UE-DIFFS D2 缓解栏留痕）。M5.2 收窄为 GPU 原子三角计数 + HUD。

**提交版本**（核心改动）：
- `cull.slang` u2 / `dag.slang` u3：各加 `RWStructuredBuffer<uint>` 三角计数器，可见判定/emit 时 `InterlockedAdd` 该簇 triCount（`clusterMeta[id*4+1]`，字节偏移陷阱同 M3.1）
- 回读环形 4B→8B（[0]=可见簇数 [4]=可见三角数），每帧 `writeBuffer` 清零 + 两段 `copyBuffer`，`pollCullStats` 延迟两帧一起 map；triCounter_ 双路 init 各建（dag 不调 initCull，与回读环形同款独立）
- HUD cull/dag 模式新增 `s5 tris` 行（frustum 行下方）；截图日志追加 `tris N`（run.bat findstr 断言出口）
- 顺手修 viewer 潜伏 bug：cut thr 滑条回读条件 `dagTop||dagMode` → `dagTop && dagGroupCount>0`（dag 遍历模式 hud.cutThresholdPx 从不被填 = POD 默认 1.0，无条件回读把命令行 `--cut-thr 16` 每帧覆盖回 1.0）
- 顺手修三张历史 selftest 卡：m0.9/s2.0/s3.2（exe=current）expectLog 停在 15/15，M5.0 起套件 16/16，本轮首次跑到即 FAIL → expectLog 同步 16/16（exe=current 断言当前构建，套件数演进必须同步历史卡）

**完成情况**：
- 三角数锚点逐位精确：dag 近景（thr 0.001）237 簇 → **tris 3936 == triangles(finest)**，双后端一致；cull 近景 76 簇 → 1526
- 拉远谱单调连续：thr 2/4/8/16/32/64 → tris **3870/2360/426/142/104/104**（簇 230/106/14/7/3/3）——S5 验收「拉远连续下降 + 固定视角稳定」达成
- HUD 目检：s5 tris 行 near 3936 / far 426 同屏可见；640x720 面板完整
- 交代物：`example/s5-done/`（near/far HUD pair × 双后端 4 张 + verify-near/far-dx12 证据图 + run.bat verify exit 0，findstr 断言 `tris 3936`/`tris 426`）+ cases.json s5.2 卡（29 versions）
- HUD 基线重拍 7 版本 14 张（m0.9/s2.0/s3.2/s4.1/s4.2/s5.0/s5.1，只拍 exe=current）；回归全 bytes：s5.1 6/6 + s4.0 4/4 + s4.1 4/4 + s4.2 6/6 + s5.0 6/6 + s3.2 18/18 + m0.9 4/4 + s2.0 16/16 + s5.2 新卡 6/6；`server.py --check` OK（stage=S5.2 cases=3 baselines=6）；双后端 selftest 16/16
- **S5 全线收官**：M5.0 误差投影 → M5.1 DAG 遍历 → M5.2 三角数 HUD，tag `s5-done`

**踩坑**：
1. **NVRHI BufferDesc 无 `canHaveUAV`**：成员名是 `canHaveUAVs`（复数）；triCounter_ 按 dagGroupExpanded_ 同款纪律建（canHaveUAVs + initialState CopyDest + keepInitialState=true，每条命令列表开头回 CopyDest，帧内 writeBuffer→UAV→CopySource 动态轮转）。
2. **cut thr 滑条回读钳制命令行参数（潜伏 bug，M5.1 埋雷）**：dag 遍历模式 HUD 不画滑条（`frame.dagGroupCount` 恒 0 不填），但 viewer 每帧 `if (dagTop||dagMode) cutThresholdPx = clamp(hud.cutThresholdPx, 0.25, 4.0)` 无条件回读 → 命令行 thr 16 第二帧起被覆盖成 1.0。实锤过程：far HUD 基线图显示 237/3936（= no-hud thr 1 的结果），no-hud 同参数 14/426 → 实测 thr1@dist20H720 == 237/3936 逐位吻合。s5.1 near 没踩到纯属 thr 恰好 = 1.0。
3. **历史 selftest 卡 expectLog 失配（潜伏，M5.0 埋雷）**：m0.9/s2.0/s3.2 卡 exe=current 但写死 15/15，M5.0 加第 16 项 selftest 后必 FAIL，此前收口没挑到这三卡所以没暴露。教训：exe=current 的 tool 卡断言的是**当前构建**，套件数/stage 演进时历史卡必须同步，否则全量跑闸必炸。
4. **Edit 工具误删行**：批量替换时 old_string 尾部含上下文行、new_string 漏带，`beginTrackingBufferState(identityIndex_, ...)` 被吃——编译能过（writeBuffer 不依赖前一行）但状态跟踪会错，目检 diff 当场抓回。
## M-A4b 骨架 FK + rigged glTF 资产入口（feat/action-system，2026-11-08）

**动作**：M-A5 视觉验收的两个前置解阻塞（蒙皮渲染仍等 2.3，但骨架层与资产层不被阻塞）——①`nl/anim/skeleton`（扁平拓扑表 + 纯 (T,R) FK，ozz Skeleton 同构口径）；②`nl/anim/gltf_anim`（rigged glTF -> Skeleton + AnimClip，tinygltf 声明侧第二消费 TU）；③程序化资产 `RiggedChain.glb`（tools/make_rigged_glb.py，纯标准库 GLB）→ selftest skeleton + ctest ma4b + run.bat/展示台卡/TOOLS-GUIDE 收口。

**提交版本**：M-A4b（tag `anim-ma4b`，验收后打）

**完成情况**
- skeleton：boneNames/parentIndices/restPose 三表拓扑序；`localToModel` = model[i]=parent⊗local[i]（rotVecByQuat 作用于局部平移）；out 与输入同容器安全；validate 拒拓扑序破坏/重名/尺寸不齐。
- gltf_anim：DFS 收表（无名节点自动 bone_N）、LINEAR/STEP、rotation 采样归一化；SCALE/CUBICSPLINE 显式拒绝；静态 TRS → 单关键帧轨道（⭐空轨道=零/identity 常量语义，不能兜 rest）。
- 验证：selftest skeleton PASS（FK 手算锚点 + sway t=0.5 spineWorld(-0.25,1.4330127)/headWorld(-0.5,1.8660254) + wrap 逐位 + 两次加载逐位 + 负向）；ctest 6/6；`example/ma-track/run.bat` ALL GREEN（含资产再生成）；`server.py --check` OK。

**踩坑情况**
1. tinygltf 双 TU 宏视图不一致（mesh_io 的 `TINYGLTF_NO_STB_IMAGE_WRITE` 未被 gltf_anim 复刻）→ inline 构造引用被禁 `WriteImageData` LNK2019。约定固化：第二个 tinygltf 消费 TU 必须复刻 IMPLEMENTATION TU 全部 NO_* 宏。
2. FK 测试初版 local 未以 restPose 打底（local=采样输出语义，静态 TRS 在轨道内）——用例错实现对。
3. bash 调 bat 传多 target 必须分开传参（引号整体成一个参数，NMake 收不到第二个 target）。

**下一步**：M-A5 视觉验收等 2.3 蒙皮渲染（届时资产/骨架/采样/混合/状态机/blend space 全链已备）；或先做 sway 轨迹证据图升级（preview 用）。

## M-A4d 骨骼动画动态演示（feat/action-system，2026-11-08）

**动作**：用户要看「图」→「动态图演示」——anim-tools 新增 `fkdump`（全骨骼逐帧 FK 世界坐标 TSV，确定性）与 `fksample`（末端单点轨迹）；`tools/anim_demo.py`（纯标准库）生成单文件 `example/ma-track/anim-demo.html` 动态演示页。

**提交版本**：M-A4d（tag `anim-ma4d`）

**完成情况**：四面板 canvas 循环播放 Fox Walk/Run、CesiumMan Walk、RiggedFigure（Khronos 标准资产真实 FK 数据，页面零伪造——数据 100% 来自 fkdump，JS 只做投影取帧）；播放/暂停 + 0.25~3× 速度 + XY/ZY 视角切换 + 等比投影居中；fkdata/ 四段 dump 入库；run.bat 补 fkdump 锁行 ALL GREEN；cases.json ma-track 卡 +1（preview=Edge 无头实截）；TOOLS-GUIDE 追加 M-A4d；server --check OK。另：纠正 M-A5 错误前提（commit 7266abc）——「2.3 蒙皮」系编号撞车（里程碑 M2.3=LOD viewer 早收官；实指 ROADMAP §2.3 渲染侧蒙皮层，规划未开工，renderer/shaders 零 skin/bone 命中实证）。

**踩坑情况**：等比投影初版 x/y 各自 sx/sy 拉伸变形——改 min(W/f.w, H/f.h) 统一比例 + 内容居中；Edge 无头 `--headless=new --screenshot --virtual-time-budget` 对本地 HTML 截图核查渲染（本机可用）。

**下一步**：M-A5 蒙皮渲染（CPU 刚体绑定 → LBS → 模型展示）——蒙皮层需从零建，不再「等」任何已有里程碑。

## M-A5 蒙皮层 + 模型展示（feat/action-system，2026-11-08）

**动作**：用户拍板「继续做蒙皮和模型展示」→ 两段落地。M-A5.1：`nl/anim/skinning`（CPU LBS：列主序 Mat4、palette=model*IBM、skinPositions/skinNormals、validateSkinBinding）+ `loadGltfSkinnedModel`（skin.joints 槽位映射、IBM 换算、JOINTS_0/WEIGHTS_0 物化、权重加载归一 bevy 语义），动手前补读 bevy skin.rs / UE SkinnedMeshComponent 两处参考（用户指出未读源码就动手）。M-A5.2：viewer `Skin` 模式（`--mode skin`，动态 VB 逐帧蒙皮，`--anim-time` 截图确定性）+ 取景修正 + 回归收口。

**提交版本**：M-A5.1（`cde41c7`）+ M-A5.2（本提交，tag `anim-ma5`）

**完成情况**：
- selftest skinning 全 PASS：刚体锚点不动点、LBS 中点、法线旋转归一、validate 五负向、CesiumMan 端到端两条强判据（网格撕裂检测：蒙皮/绑定最大边比<2；主关节接近度：顶点距主导关节<1.2m）；ctest ma5 1/1。
- regression ma-track 13/13 ALL GREEN（cases.json 新增 skinning 用例卡；ma-track 版本卡更新 M-A1~M-A5 / tag `anim-ma5`；run.bat 补 skinning 锁行）。
- viewer 截图验收：`example/ma-track/skin-walk-t0/t06/t12.png`（CesiumMan 行走三时刻相位清晰、全身居中、无撕裂无尖刺）。
- 取景修正：skin 模式 init 时做一次 t=0 蒙皮，取蒙皮输出 bbox 中心做逐帧后蒙皮平移（bind bbox 中心无效——网格在 bind 空间中心≈原点，偏移来自骨骼动画输出站姿）；缺省距离 2.6（Suzanne 口径 4.2 对 1.4m 人形太远）。
- ANIM-TOOLS-GUIDE 补 skin 模式与 selftest skinning 用法。

**踩坑情况**：
1. ⭐JOINTS_0 语义：值域是 skin.joints 数组槽位不是 node id——首版把槽位当 node id，脚部顶点绑到手臂骨骼，渲染放射尖刺；`cmdSkinCheck` 诊断定位，jointSlotToBone 映射修复。
2. rest-identity 判据失效：CesiumMan bind=T-pose 而 rest=行走姿，IBM≠inverse(restGlobal) 是资产正常状态（此前自测通过是「槽位误映射 × IBM 误换算」两个错误抵消的巧合）；换撕裂检测+接近度两条强判据。
3. node.matrix：CesiumMan Z_UP/Armature 节点用 matrix 而非 TRS，collectBones 初版忽略 → decomposeNodeMatrix（列主序刚性分解 + Shepperd 四元数提取）。
4. ⭐回归 cwd 错位（真 bug 两枚）：regression.py 工具用例 cwd=exe 目录（build/nmake），ma-track 用例卡却写仓库根相对路径——M-A4 起用例只跑过 run.bat 从未真跑 regression，本次首跑即暴露 6 条 FAIL；修 = 用例参数改 `{root}/` 模板（server.py build_argv 既有白名单）。连带暴露 anim-tools 缺文件段错误：testGltfAssetLoad / testGltfDeterminism 加载失败不早退，`clips[0]` 打空 vector UB——补早退守卫（exit 139 → 干净 FAIL）。
5. 「selftest skeleton + CesiumMan FAIL」虚惊：该用例约定 3 骨合成资产 RiggedChain.glb，传 Khronos 资产属用错参数非回归。

**下一步**：M-A 轨道收口（merge 回 master 走 PARALLEL-TRACKS §7 文档闸）；GPU 调色板（shader 端 palette UB）等更大规模蒙皮资产再立项。

## T1 材质图可视化编辑器（A 类并行轨道支线，feat/material-graph，2026-11-08~09）

> 终点定义与拆解：`docs/material-graph/SIDE-TASKS.md`（2026-11-09 用户拍板「及时收手」封顶）。

**动作**：T1-M1 交互式节点编辑器落地（imgui-node-editor v0.9.4 vendored；viewer `--editor <file.mg>` 经 HudFrame.editor 挂入 hud NewFrame..Render 区间，不碰 ViewMode/kStage；画布拖拽/连线/属性编辑/Save/Compile 全套）；T1-M2 细化（右键增删节点、Del 级联删、Ctrl+S、Add 节点全链示范扩库路径、底部状态条）；T1-M3 封顶版（节点库扩到 10 种 + uniform 曝光 MaterialParams cbuffer b1 + kitchenSink 全算子图入 b3 真实编译）。

**提交版本**：feat/material-graph `2e60906`(M1) / `15ce08c`(M2) / `40d5704`(M3 封顶)

**完成情况**：DX12+Vulkan 双后端编译通过；ctest b2/b3/b4 全绿（M3 后复验 3/3，含 kitchenSink 双图 slangc SPIR-V/DXIL×vs/ps 四路真实编译）；证据图 t1-editor-window.png / b3-nodegraph.{svg,png}（GPU 回读/工具真实输出，非手绘）；editor=null 路径零改动既有基线不受影响。封顶定位：调试工具级，UI 不再加功能；唯一遗留 = 拆解 6 预览视口（等 B5/S7）。

**踩坑情况**：① imgui-node-editor v0.9.4 API 是 PinKind（新版才叫 PinDirection）；② 库对 size<=0 回退「占满剩余」，底部状态条须外层包 ImGui host child 预留；③ NodeEditor.json 运行时状态污染确定性截图，拍摄前 rm；④ 本机 SDK 的 wrl/client.h 在 Include\<ver>\winrt 下（构建脚本 INCLUDE 补 winrt）；⑤ emitter memo 泄漏——上游多边引用把 visited key 写进生成代码（B3 潜伏雷，改 key→expr 记忆化）；⑥ props 按 unordered_map 迭代序写出致 roundtrip 非字节一致（改 key 排序）。

## M6.0：Hi-Z 深度预通道 + 手写 min 金字塔（S6 开篇，2026-11-09，tag `s6-hiz`）

**动作**：hiz.slang 6 入口（vsHizDepth/psHizDepth/csHizDown/csHizTop/vsHizViz/psHizViz）：prepass 深度写（度量 = 视点径向距离 `distance(worldPos, eye)`，与 M5.0 误差公式同空间）+ compute NMin 逐级金字塔 + 右下角 160x160 层级可视化覆盖层 + 最高层 top min 回读（环形 3 项延迟两帧）。Scene `initHiz`/`ensureHizTargets` 懒建；viewer `--mode hiz` + HUD `hiz top` 行 + pyramid mip 滑条；kStage bump S6.0。

**提交版本**：`59d6b72`（master，tag `s6-hiz`）+ 交代物补齐提交（example/s6-hiz/ 卡 + 本台账）

**完成情况**：
- 机械判据：金字塔 top min dist 4.2 → 3.29377、dist 20 → 19.0659，双后端逐位一致（回读环形帧帧相同）；随距离单调增大 = 最近径向深度语义成立。
- 肉眼判据：mip0 灰度猴头剪影近大远小、近亮远暗；视口外清屏 1e9 保守性不破。
- deviceErrors=0 ×2（R32F RT setClearValue 后 D3D12 debug layer 9→0）；selftest 16/16 ×2；32 个 shader 产物全 ok。
- 交代物：`example/s6-hiz/`（near/far pair × 双后端 4 基线 + run.bat verify findstr 断言 3.29/19.06 exit 0）+ cases.json s6.0 卡（3 用例）+ server.py --check OK（stage=S6.0 cases=3）；回归 s6.0 卡 6/6 全 bytes。
- 全量回归闸 228/228 ALL GREEN（收口唯一一次全量；含 kStage 重拍 9 版本 × 双后端 18 张 HUD 基线）。
- 固化：`docs/SHADER-CONVENTIONS.md` §2.7（每管线单 BindingSet）/ §2.8（Hi-Z 金字塔 POT + RT setClearValue）；MEMORY.md / 当日日志收口段。

**踩坑情况**：
1. ⭐Vulkan 多 BindingLayout 拆 set（§2.7 铁律）：prepass 初版两个 BindingSet（constants + hizConstants + SSBO），slangc 把资源全编进 vk set 0，NVRHI Vulkan legacy 模式把多 BindingLayout 拆成多个 vk descriptor set → 第二套 set 的 b1 绑不上读 NaN（DX12 无 set 概念侥幸正常，NVRHI validation 静默）。NaN 级联：prepass 写 NaN → NMin 丢 NaN 只剩清屏值 1e9 → top 恒 0xFFFFFFFF pending → 可视化饱和白剪影。合并为单 `hizPrepassSet_`（b0+b1+t1..t4）后双后端逐位一致。
2. ⭐金字塔尺寸必须 POT（§2.8 铁律）：640 宽 ceil 对半链要 11 层超 DX12 `mipLevels ≤ floor(log2(maxdim))+1 = 10`（debug layer 报错；Vulkan 无 VVL 静默建坏）；视口向上取 2 的幂 1024x512，mip0 视口外清 1e9。
3. 诊断手法沉淀：compute 写哨兵（0xDEADBEEF）验证 UAV/copy 链路；绝对 LOD 多点采样（mip0 角落/模型点/mip3/mip6）逐层定位坏层；像素统计量化（DX12 剪影 mean 80 vs Vulkan 255）。
4. hizTop_ 动态状态跟踪试验报 NVRHI「Unknown prior state」+ Device Removed → 回退 keepInitialState=true（triCounter_ 同款双后端验证模式）；回读环形重建补 `initialState=CopyDest`。
5. viz 常量槽位：W/H 填 `[1].y/z` 与 shader 读法对齐（首版填 `[2]` → 0/0 → NaN 四边形退化覆盖层消失）。
6. ⭐回归 Flake（环境级，非代码）：Windows 并发拉起同镜像进程偶发「exit 0 + 空输出」「超时 + 零输出」两类不可能签名（单独复验必过；并发探针复现 0xC0000005，同 suite 并发写固定临时文件）。regression.py 对两类签名各自动重试一次后，第 5 次全量拿到干净 228/228。
7. 工具 exe 纪律：`build/{DX12,VULKAN}/nanite-tools.exe` 是各后端构建树直接产物，根目录 `build/nanite-tools.exe` 是废弃旧树残留（误 cp 覆盖新产物致回归假 FAIL）；NMake 同秒 mtime 跳过重链须 rm exe 强制重链。

**下一步**：M6.1 保守盒测试串进 DAG 遍历（拍板点：bevy 式 HZB second pass 长在队列多 pass 结构上 vs 现有单 dispatch 改造 = 是否顺带做 UE-DIFFS D3a 升级）→ M6.2 开关对照验收（tag `s6-done`，更新 UE-DIFFS D6）。

## ECS 前置：通用数学上移 nl/math（master 在途，2026-11-09，无 tag——M-E0 前置重构）

**动作**：用户拍板（docs/p8/P8-ECS-PLAN.md 数学决策点 + 「直接改引用不设转发头」）：nl/anim/anim_math.hpp 的通用数学（Vec3/Quat 及 13 个函数）上移 `core/include/nl/math/math.hpp`（namespace `nl::math`，选型/口径注释随行）；删 anim_math.hpp 转发层；anim.hpp 直接 include nl/math 并定向注入 `Vec3/Quat/quatIdentity`（API 契约类型）；anim/gltf_anim/skeleton 三个 cpp 加文件级 `using namespace nl::math;`（TU 局部不泄漏）；anim-tools 数学函数限定改 `nl::math::`（dotQuat×2 / quatFromAxisAngle×3）。ECS Transform 层级数学（非均匀 scale 复合 / mat4）留 M-E1 在 feat/ecs 内新增。

**提交版本**：见收口 commit（本条随重构提交入库）。

**完成情况**：DX12 + Vulkan 双后端构建 BUILD OK、viewer selftest 16/16×2；ctest anim 全系列双树全绿（DX12 5/5 + Vulkan 7/7 含 ma2-blend/ma3-statemachine/ma4-blendspace/ma4b-skeleton/ma5-skinning）；regression s1.0 抽验 6/6 ALL GREEN——零行为变化确认。

**踩坑**：①首轮按「薄转发头」实现后 anim-tools 炸出 C2039（其 using nl::anim::quatFromAxisAngle 依赖转发头全量注入）——用户当场拍板砍转发头直接改引用，按「类型注入 nl::anim（API 契约）/ 数学函数用 nl::math:: 限定（外部）」分层收口；②诊断期间撞上 M6.0 已记录的环境级 flake（「exit 0 + 空输出」签名，bash 手跑与 ctest 均随机中招、输出文件仍正常写出），一度误判为 cluster 崩溃——用 regression.py s1.0 双后端 6/6 绿 + exe 不含 anim/math 头 + 输出恢复三证据排除代码关联，ctest s1-nmesh-suzanne 的偶发红属环境 flake（regression 主体系绿）。

**下一步**：建 `feat/ecs` worktree（wt_pool claim --track ecs --milestone M-E0）→ M-E0 World/Entity 核心。

## M6.1：Hi-Z 遮挡剔除串进 DAG 遍历（S6.1，2026-11-09，tag `s6-cull`）

**动作**：两项用户拍板先行——①金字塔 min→max（三处权威口径：UE `NaniteHZBCull.ush IsVisibleHZB` inverted-Z 取 min=最远表面、bevy `downsample_depth.wesl` reverse-Z 取 min + `occlusion_cull_screen_aabb` 簇最近点比较；S6-PLAN 原文 min 口径在径向距离下不保守会漏画）；②单 dispatch 内联（S6-PLAN 原文方案），bevy 式 HZB second pass 不做（=D3a 升级，触发条件未到）。落地：hiz.slang `csHizDown` min→max + top 语义翻转；dag.slang 新增 `hizOccluded`（Mara&Morgan 精确切线球投影（UE `SphereToScreenRect` 同款）+ bevy `firstLeadingBit` mip 选择 + 4×4 采样 max 聚合 + 簇最近径向距离>区域 hizMax 整树 continue），DagConstants 48B→128B（+g_dagHiz 开关/视口宽 +g_dagView），t8 金字塔 SRV 双 set（dummy/真金字塔互斥，§2.7）；FrameParams 加 view[16]、OrbitCamera 加 view()（与 viewProjection 的 lookAt 同源）；ensureHizTargets 建 dagHizSet_ + just-created 帧内补全 mip 清屏；HizConstants/hizTopFar 三态回读（真实值/-2 背景主导/-1）；HUD `hiz top` 行 farthest depth 语义 + viewer 日志 pyramid top far；kStage bump S6.1。新资产 HizScene（墙+墙后 3 球+侧边对照球，`scripts/make-hiz-scene-glb.py`，6 层 lod 链 check-orient PASS + dag 校验 PASS）。

**提交版本**：`f0dc46c`（M6.1 本体，全量回归闸 240/240 ALL GREEN 跑于该状态）+ `e5fd68c` / `6936862`（台账回填与措辞修正）+ `415699e`（kStage bump 重拍基线补交：m0.9/s2-done/s3-done/s4-cull/s4-done/s5-proj/s5-traverse/s5-done 共 18 张 + memory 追加段）+ 本节台账勘误提交；tag `s6-cull` 挂在本节终版提交上（含全部重拍基线与台账）

**完成情况**：
- 机械判据：正面（yaw 12）墙挡墙后 3 球 → hiz visible **24/85** vs dag 基线 **40/85**（-16 簇整树剔除）；侧面（yaw 75 穿墙）hiz **40/85** == dag（零误剔）；hiz vs dag 正面画面**覆盖层外逐像素 0 差**（剔除对最终画面透明 = 只减被深度测试挡住的簇）；Suzanne 自遮挡 hiz 233/324 vs dag 237/324。
- 双后端：hiz-front/hiz-side/dag-front/dag-side 4 图 bytes 逐字节一致；deviceErrors=0 ×2；selftest 16/16 ×2。
- 交代物：`example/s6-cull/`（hiz-front/hiz-side 双后端 4 基线 + dag-front/side dx12 证据 + run.bat verify findstr 断言 hiz 24/85 + 40/85 ×3 exit 0）+ cases.json s6.1 卡（3 用例）+ server.py --check OK（stage=S6.1 cases=3 baselines=6）；s6-hiz 卡同步 max 口径（run.bat 断言 233/237 per 324 + background-dominates，exit 0）。
- kStage bump 波及基线重拍：exe=current 10 版本 × 双后端 **48/48 全部重拍**（重拍脚本走 cases.json args 模板，deviceErrors=0 全验）。
- 固化：S6-PLAN（M6.0/M6.1 口径勘误 + checklist 打勾 + 两条新坑）、UE-DIFFS D6（遮蔽已实现+结构差异+**度量空间径向距离 vs UE 设备深度**已知偏差行，72cdbe4）与 D3a（second pass 交汇拍板）、SHADER-CONVENTIONS §2.8（max 口径三连锁语义 + 帧内禁开第二 cmd list）、MEMORY.md（固化「UE 差异随手落档」规矩，2026-11-09 用户拍板）、当日日志。

**踩坑情况**：
1. ⭐**view space 朝向**：`camera.lookAt` 是右手系 **-z 朝前**（camera.cpp:28），Mara&Morgan/UE 公式按 +z 朝前推导——首跑直接用 `vc.z` 使「跨近断面」判定恒真 → 剔除**静默失效**（hiz==dag 40/85 假绿）。修：`wz = -vc.z` 换轴后 40→26 立现。教训：三处参考的公式移植必须核对本项目坐标系约定。
2. ⭐**金字塔方向是计划文字错误**：S6-PLAN M6.1「查 min 深度、clusterMinDepth > hizMinDepth」在径向距离度量下非保守（texel 同盖近墙 3m 与远景 100m 时 min=3，簇最近 50m 被误剔但实际在 100m 像素侧可见）——三处权威口径全部是「簇最近点 vs 区域最远表面」，即径向距离 max 金字塔。读参考代码时用具体数字推一遍保守方向，别信计划文字。
3. ⭐**帧内禁开第二 immediate command list**：ensureHizTargets 首版自建 cmd list 清屏 → NVRHI「Two or more immediate command lists cannot be open」×4 + top 回读恒 pending。修：hizJustCreated_ 标志 + 帧内 cmd 补清全 mip。
4. **top 回读 1e9 是 max 口径合法值**：画面有背景像素时 top far = 1e9（清屏值主导）——初版当作 pending 丢弃（v<1e8 过滤）恒显示 pending；改三态（≥0 真实值 / -2 背景主导 / -1 无数据），HUD 显示 `hiz top : bg`。
5. **asset 生成 winding**：首版 HizScene 盒/球绕序内翻（check-orient exit 1，2116/2120 inward worst cos -1.000）——盒面角序未按「外视 CCW」排、球 [a,b,a+1] 绕向反；修正后 PASS。check-orient 是资产管线的硬闸，生成类资产必须过。
6. **NMake 跳重链**：dag.slang 改后 viewer.exe 未重链（链接依赖未变）→ regression exe 新鲜度门禁拒绝（落后于 dag.slang）；删 exe 强制重链后过闸（§工具 exe 纪律同款）。
7. 640x720 竖幅把对照球挤出画面（aspect 0.89 → ctrl 球 NDC x 1.61 > 1）——基线改 960x540 让「墙 + 对照球」同框，亲眼可见「剔除选择性」。
8. **重拍基线漏 add**：f0dc46c 的 add 清单只含 s6 系基线 + cases.json，kStage bump 重拍的其余 8 版本 18 张 PNG 留在工作树（tag 已打、检出会 FAIL）。收口时用 `git log -1 -- <file>` 抽查 PNG 最后提交发现，补交 `415699e` 并 `git tag -f s6-cull` 移至该提交。教训：基线重拍收口时先 `git status -- example/` 全列再 add，逐版本核对最后提交 hash。

**下一步**：M6.2 开关对照 + 验收（ImGui 一键开关 Hi-Z、HUD 剔除前/后簇数与帧时间、S4 1M+ 大模型+挡板帧时间对比；tag `s6-done`，`docs/screenshots/s6.png`）。

## M6.2：Hi-Z 运行时开关 + 剔除前/后簇数 + 1.3M 挡板场景（S6 收官，2026-11-09，tag `s6-done`）

**动作**：三处前置（UE `NaniteCullRaster.cpp:250-262` `r.Nanite.Culling.HZB/Frustum` 独立 CVar + `NaniteDefinitions.h:517` FNaniteStats 单结构原子累加；bevy `bindings.wesl:143-152` 每 pass 分立 atomic counter、无运行时开关；Carrot 无 Hi-Z 模块无对照）→ 落码：①开关 FrameParams.hizEnable（ImGui checkbox 双向 + `--hiz on|off`，scene.cpp hizCullActive && hizEnable 两处同口径，UE permutation 差异进 UE-DIFFS D6）；②剔除计数 dag.slang hizOccluded 命中 `InterlockedAdd(g_triCounter[1])` + triCounter 4B→8B + 回读环形 dag 16B（[8]=occluded，hiz top 迁 [12]）+ DeviceInfo/HudFrame hizOccluded + HUD occl 行（剔除前 visited = emit+occluded 整树语义）；③HUD hiz 区上移 mode/model 行后（960x540 面板 512px 钉死，原位置掉滚动区——仅 hiz 模式布局变化）；④GPU timer 移出 HUD 块无条件 ensure（DeviceInfo.lastGpuMs + `gpu timing: avg` 日志——CPU 口径 draw 轻时不敏感，GPU 口径才显性）；⑤新资产 OcclScene1M（1.3M tri 球 + 前挡板，`make-occl-scene-glb.py`，挡板角宽 14.8° 盖球投影 18.2° 中央 ~2/3，lod0~8 链）；⑥kStage S6.2 + selftest 断言同步；⑦11 个 exe=current 版本 46/46 基线重拍（deviceErrors=0 全验）。

**提交版本**：见收口回复（本节随提交回填）；tag `s6-done` 挂在本节终版提交上

**完成情况**：
- 机械判据：HizScene 开关 on 24/85 + occluded 10 of 34 visited；off 40/85（== dag 基线）+ occluded 0（一键回退）；侧视 on occluded 0（零误剔）。OcclScene1M：on 6861/43547（occluded 5378 of 12239，84% 簇被剔）vs off 12324/43547。
- 性能：1920x1080 unthrottled NVRHI timer 3 轮均值 on ~2.74ms vs off ~4.03ms（**-32%**，被遮挡场景帧时间明显下降）；960x540 GPU 口径差距小（prepass 重复开销 ≈ 绘制节省）——验收用 1080p。
- 无漏画：OcclScene1M hiz on/off 最终画面 MD5 逐字节一致（6c0d5a0a…）。
- 双后端：toggle 三对 + occl1m 两张 bytes 一致（同数字 6861/5378/12239 ×2 后端）；selftest 16/16 ×2。
- 交代物：`example/s6-done/`（toggle 三对双后端 6 基线 + occl1m-on/off 双后端 4 基线 + run.bat verify 6 条 findstr 断言 exit 0）+ cases.json s6.2 卡（5 用例）+ `docs/screenshots/s6.png`（开/关 HUD 对比拼接图）+ server.py --check OK（stage=S6.2 cases=5 baselines=12）。

**踩坑情况**：
1. ⭐**HUD hiz 区掉滚动区**：960x540 画布面板高被 `min(cap, H-28)=512` 钉死（cap 560 不生效），hiz 区放 frustum/cull GPU 行后会被截断、checkbox/occl 验收证据不可见——整段上移到 mode/model 行后。教训：新增 HUD 区块要按**最小画布**核算可见性，不是按 cap。
2. ⭐**CPU 帧时间口径不敏感**：on/off CPU 循环均值 3.48 vs 3.51ms 几乎无差（draw 段轻，CPU 不是瓶颈）——GPU timer 才是帧时间验收的正确口径；而 deterministic 模式 GPU 计时原被 HUD 块吞掉（--no-hud 不 ensure timer）→ timer 移出 HUD 块无条件 ensure，真实值恒缓存 gpuLastMs_（deterministic 只是不写 HUD，基线 MD5 不受影响——timer 不写像素，回归验证 bytes 不变）。
3. **GPU timer 与 nanite-tools.exe 链接锁**：后台 check-orient 跑着 DX12 侧 nanite-tools.exe，Windows 锁 exe 导致 NMake 重链 U1077——换 Vulkan 侧同名工具继续跑、DX12 侧解锁重链（两 exe 同源等价）。
4. **1.3M 资产 check-orient 45 分钟（已定位已修复，2026-11-09）**：`countInwardNearest` 暴力最近邻 O(T×3×P)——ref=lod0 65.5 万点 × targets 124 万 tri ≈ 2.4e11 次距离计算，45 分钟是数学必然（原注释「组内规模都很小不值得建空间索引」只对小资产成立）。修为 uniform spatial hash（27 邻域 + <cell 判据硬保证 + fallback 暴力兜底），45min→4.9s（~550 倍），数值与暴力版逐位一致。**修复过程又踩一坑**：首版 counting sort 漏 `cursor[id]++`（每 cell 只留最后写入的一个点，ref 点几乎全丢 → 结果错但仍 PASS/FAIL 乱报），靠「hash vs 暴力逐查询对拍 assert + exit 9」定位——**加速类改动必须带对拍自证**。lod0 自身朝向补 `check-orient lod0 lod0` 自检（ref=target，worst +0.905 PASS，双后端一致）。
5. **NMake 跳重链**：main.cpp/scene.cpp 改后 viewer.exe 未重链 → 删 exe 强制重链（§工具 exe 纪律同款）。

**下一步**：S6 全部收官（tag `s6-done`）。主线下一站 S7（PBR + vis buffer + vertex pulling + 材质解析，`docs/s7/S7-PLAN.md`）或按 ROADMAP 插入项（PBR 直射光已排在 S1~S6 之间、SSR 排 S7 后）——开工前按铁律重读对应 PLAN + 三处对照，确认插入顺序与用户对齐。

## M-E0：World/Entity 核心（feat/ecs 开篇，2026-11-09，tag `ecs-e0`）

**动作**：wt_pool claim（track ecs / M-E0）→ 建 worktree `E:/UE_STUDY_2026/nl-engine-ecs`（junction 共享 `.workbuddy` + `thirdparty/slang/dist`，`.workbuddy` skip-worktree）→ 三处证据重读（Carrot `World.h/.cpp` + `EntityTypes.h` 定向读 M-E0 点名段：newEntity/newEntityWithID/removeEntity/flush/tick/exists/flags；bevy `entity_disabling.rs` 借语义：隐藏是标记不是删除、只禁自身不牵连子级）→ 落码 `core/include/nl/scene/World.hpp` + `core/src/scene/World.cpp`：实体创建（u64 递增，0=无效）、延迟增删（entitiesToAdd/Remove + flush 先加后删）、tick 开头 flush（延迟语义锚点）、名字表（exists=names_.contains，Carrot 口径）、Flags(Hidden) 位标志 + getEntitiesWithFlags 过滤、getAllEntities/findEntityByName O(N) 无缓存 → 工具 `nanite-scene`（anim-tools 先例）`selftest --suite ecs-core` 四组断言 → CMake：nl_core 挂 World.cpp + nanite-scene 目标 + ctest 正/负向注册。master 侧注册表三处同步（AGENTS.md / PARALLEL-TRACKS §7.1c / wt_pool TRACKS，commit 6647fcf）。

**提交版本**：feat/ecs 本收口 commit（tag `ecs-e0`，即本条台账所在提交）；master 注册表同步 6647fcf。

**完成情况**：DX12 构建 BUILD OK + viewer selftest 16/16（全量构建顺带验证无回归）；`nanite-scene selftest --suite ecs-core` PASS：递增 ID / 增删 flush 前后存在性（Carrot 非对称口径：新建即时可见、删除 flush 后不可见）/ 名字读写与 findEntityByName / Hidden 过滤（隐藏≠删除、getAllEntities 不受 flags 影响）/ 重复 remove 幂等 / 延迟语义锚点（删实体下一 tick 生效）；ctest e0 系 2/2（正向 PASS + 负向未知 suite 退出码 2 WILL_FAIL）。收口前四项自查：①渲染交代物——本轮无（A 类 E1a 纯逻辑零渲染，如实写明）；②工具交代物——ctest 正/负向已注册并在构建树跑绿，展示台 tool 卡 + TOOLS-GUIDE 按计划留 M-E3 收口统一上；③MILESTONES 四栏已追加（本节）；④固化——P8-ECS-PLAN.md M-E0 行标完成 + 踩坑沉淀见下。

**踩坑**：①`unordered_map::contains` 是 C++20，MSVC 本项目标准下 C2039 → 一律 `find != end`；②exists() 语义是 Carrot 非对称口径（名字表即时登记 / 全局实体表 flush 后才有）——首轮 selftest 在 flush 前调 findEntityByName 直接 FAIL，修测试（补 tick(0) flush）并把「flush 前查名 miss」固化为正向断言（把延迟语义本身锁进锚点，防后续实现漂移）；③构建必须走 `scripts/build-m0.2.bat`（裸 shell 无 nmake/MSVC 环境，cmake --build 直接炸 generator）。

**下一步**：M-E1 Component 基类（serialise/deserialise/duplicate/类型 ID）+ ComponentLibrary 注册工厂 + Transform（pos/scale/quat，nl/math 扩展）+ 层级（setParent / reparent 保世界变换 / 父删递归删子 / local→world 复合 / duplicate 深拷贝）。


## M-E1：Component + Transform + 层级（feat/ecs，2026-11-09，tag `ecs-e1`）

**动作**：wt_pool claim（track ecs / M-E1）→ 三处证据重读（Carrot `components/Component.h` 基类五虚 + `TransformComponent.{h,cpp}` + `math/Transform.cpp` TRS 顺序 + `World.cpp:476-650` 层级段：setParent 摘旧挂新 / reparent 取世界变换后写回 / duplicateWithRemap 递归深拷 + "(Copy)" / removeEntity 立即递归压子孙 / flush 双结构双侧清理；TRS = translate*rotate*scale，层级复合 = parentGlobal*local）→ `nl/math` 新增 Mat4 段（列主序 m[col*4+row]：mat4TRS / mulMat4 / quatToMat4 / inverseMat4 gluInvertMatrix 展开 / mat4TransformPoint）→ `nl/scene/Component.hpp/.cpp`：Component 基类五虚（getName/duplicate/serialise/deserialise/getComponentTypeID，serialise 签名用 vendored nlohmann json 零新依赖）+ ComponentOf<T> CRTP（类型 ID 0..63 静态递增 = Signature 位对应 + duplicate 换 owner 拷构造）+ ComponentLibrary 显式注册工厂（未知名 nullptr = M-E2 负向铺垫）+ Transform/TransformComponent（getWorldMatrix 递归复合 / getWorldPosition=局部原点世界位置 / setWorldFromMatrix 矩阵分解写回）→ World.hpp/.cpp 扩展：组件存储 unordered_map 增删即时（Carrot 口径）+ 层级双结构 parents_/children_ + setParent 环防护 assert（Carrot 不防，引擎路径必须防）+ reparent 统一矩阵路径（取旧世界矩阵→setParent→inverse(新父)*世界矩阵分解写回，保 reparent 前后世界矩阵逐元素不变——硬判据；偏离 Carrot 逐字段路径的留痕见 Component.hpp 注释，Carrot 同病：非均匀 scale+旋转父链有剪切时 reparent 是近似，锚点用例用均匀 scale 父保严格成立）+ removeEntity 立即递归压子孙 + flush 清组件/双结构 + duplicateWithRemap → `nanite-scene` 加 suite `ecs-hier` 五组数值锚（全部手算对照）→ CMake：Component.cpp 进 nl_core + tinygltf include 改 PUBLIC（组件头接口暴露 json）+ ctest e1 系注册。

**提交版本**：feat/ecs 本收口 commit（tag `ecs-e1`，即本条台账所在提交）。

**完成情况**：DX12 构建 BUILD OK + viewer selftest 16/16；`nanite-scene selftest --suite ecs-hier` PASS 五组：①三层父子链复合矩阵手算锚（A(pos1,0,0·s2)←B(pos0,1,0·s3·Rz90)←C(pos1,1,0)：C 世界矩阵 col0(0,6,0) col1(-6,0,0) col2(0,0,6) col3(-5,8,0) 逐元素；局部点 (1,1,0)→世界 (-11,14,0)；getWorldPosition=局部原点=(-5,8,0) 即 W 平移列）②reparent 保世界变换硬判据（世界矩阵逐元素不变 + 新局部手算锚 pos(-3,4,0) scale(3) rot=Rz90）+ 无 Transform 父直通（Carrot 断链口径）③删父递归删子（flush 后子孙全不存在 + 双结构清理）④duplicate 深拷（"(Copy)" 名 / 组件局部一致 / 改副本原件不动 / 副本孙世界矩阵一致 / 父关系继承）⑤组件库（显式注册 / 按名创建 / 类型 ID 稳定 / 未知名 nullptr）；ctest 3/3（e0 正负向 + e1 正向）。收口四项自查：①渲染交代物——无（A 类 E1a 纯逻辑）；②工具交代物——ctest e1 正向已注册跑绿（负向复用 e0 未知 suite），展示台留 M-E3；③MILESTONES 四栏（本节）；④固化——P8-ECS-PLAN M-E1 标完成 + 数学段口径注释留痕（nl/math Mat4 与 anim 合并时机届时定）。

**踩坑**：①World.hpp 缺 `<memory>`（unique_ptr 进头后 std 成员 C2039 连锁报错一片）——头文件加新 std 类型先补 include；②`nl::scene` 内用 nl/math 类型未限定 → 头里定向 using 声明（类型 + mat4TRS/quatIdentity），cpp 文件级 `using namespace nl::math`（anim 先例，头不污染）；③「世界位置」语义口径：getWorldPosition = Carrot computeFinalPosition = **局部原点**的世界位置（= W 平移列），首轮锚点误写成局部点 (1,1,0) 的变换值 FAIL——两个语义分开锚定（原点 vs mat4TransformPoint 任意局部点）；④裸 shell 无 nmake 环境，构建必须走 bat 脚本（cmake --build 直接炸 generator）；⑤ComponentTypeID 归 World.hpp 基础类型层（World 存储与组件层共用，避免循环 include——Component.hpp include World.hpp，World.hpp 只前向声明 Component）。

**下一步**：M-E2 System 基类（Signature 匹配 + 实体列表自动维护 + tick/firstTick 生命周期）+ 场景 JSON 序列化/反序列化 roundtrip 字节一致（正/负向：非法 JSON / 未知组件名退出码 2）。

## M-E2：System + 场景 JSON roundtrip（feat/ecs，2026-11-09，tag `ecs-e2`）

**动作**：wt_pool claim（track ecs / M-E2）→ 三处证据重读（Carrot `systems/System.{h,cpp,.ipp}`：实体列表 World 推送维护 + 签名匹配 `(entitySig & systemSig) == systemSig` 子集判定 + firstTick waiting 列表恰一次 + addSystem 注册即收编；Carrot `World.cpp` tick:313-321 系统循环 + flush:268/277 推送点 + getSignature:460 现算；Carrot 场景序列化 = `scene/Scene.cpp` 文件夹+TOML+UUID 编辑器格式——单文件 JSON 按计划拍板自定，字段口径借 Carrot：id/name/flags/逐组件 serialise，层级用 parent 字段替代目录树；bevy_scene 新版结构已演进无逐键对照价值，语义「实体 id + 组件映射」一致）→ `nl/scene/System.hpp/.cpp`：System 基类（签名/实体列表/firstTick/tick/duplicate/serialise + onEntities* 推送维护）+ SignedSystem<Required...>（构造置位 + forEachEntity）+ SystemLibrary 工厂；有意裁剪留痕：Logic/Render 双类目、prePhysics/postPhysics/swapBuffers/broadcast 事件、parallelSubmit（job 轨道挂接后按需）、entitiesWithComponents 缓存 → World 扩展：getSignature 现算 / registerSystem 注册即收编 + firstTick 等待列表 / tick = flush -> firstTick -> 逐系统 tick / flush 推送 added/removed / 组件增删即时推送 onEntitiesUpdated（Carrot 口径，不走队列——组件增删本身即时，省一队列留痕）/ findSystem → `nl/scene/Serialise`（World::serialise/deserialise）：版本 1 格式 + 实体数组按 id 升序 + 组件键 nlohmann(std::map) 排序 + systems 按注册序 = 输出确定性；deserialise 三轮（壳 -> 挂父/组件 -> flush 后注册系统），非法结构/未知组件名/未知系统名/未知父/重复 ID 显式拒绝（err 非空不 assert）；TransformComponent serialise/deserialise 落地（float->json double->float 无损）→ `nanite-scene` 加 suite `ecs-system` + `ecs-serial` + 子命令 `deserialise-check`（badjson/unknown 显式退出码 2）→ CMake：System.cpp 进 nl_core + ctest e2 系四条。

**提交版本**：feat/ecs 本收口 commit（tag `ecs-e2`，即本条台账所在提交）。

**完成情况**：DX12 构建 BUILD OK + viewer selftest 16/16；`nanite-scene selftest --suite ecs-system` PASS（注册收编匹配实体 / 非匹配不收 / 组件挂上即时收编（Carrot 非对称口径）/ 删组件自动出列 / 删实体 flush 后出列 / firstTick 恰一次 / tick 计数与 dt 传递 / duplicate 数据拷贝换 World）；`selftest --suite ecs-serial` PASS（roundtrip 逐字节一致硬判据 + ID/名字/层级/Hidden/组件数值/非平凡 quat 逐位无损/世界矩阵重建抽查 + nextID 接续不重用 + 五路负向显式拒绝：未知组件/未知父/未知系统/坏版本/重复 ID）；ctest 8/8（e0 正负向 + e1 + e2 四条含 badjson/unknown-comp 退出码 2 显式判据）。收口四项自查：①渲染交代物——**无**（A 类 E1a 纯逻辑零渲染，无截图无 example 卡）；②工具交代物——ctest e2 系 4 条已注册跑绿（含 2 条负向退出码 2），展示台 tool 卡 + TOOLS-GUIDE 按 P8-ECS-PLAN 留 M-E3 收口统一上，本轮无独立证据图；③MILESTONES 四栏（本节）；④固化——P8-ECS-PLAN M-E2 标完成 + 裁剪范围/序列化格式口径注释留痕（System.hpp / World.hpp serialise 注释）。

**踩坑**：①`#include <bitset>` 写进了 namespace nl::scene 里 → MSVC 标准库头在 namespace 内展开全线爆炸（bitset C2061 conditional_t 等十余条）——include 永远在 namespace 外，using 才可进；②`sysPtr->duplicate(w2).get()` 取裸指针后临时 unique_ptr 立即析构 → use-after-free，表现为 ecs-system suite exit 127 且无任何输出（stdout 缓冲随崩溃丢失）——所有权先落 named unique_ptr 再取指针，教训已注释进测试；③组件推送时机的非对称：组件增删**即时**推送 onEntitiesUpdated（挂上 Transform 即收编系统列表，不等 flush），实体增删才走 flush——首轮断言错把列表时机绑 flush FAIL，修断言并在注释留痕（Carrot addComponent 直调 system->onEntitiesUpdated 同口径）；④裸 shell 无 nmake 环境（cmake --build 直接炸 generator），构建必须走 build-m0.2.bat（第二次踩，已彻底记住）；⑤duplicate 返回 unique_ptr<System>，auto 推导后访问派生成员 C2039——先 static_cast 再访问。

**下一步**：M-E3 收口——展示台 tool 卡（usage/正向/负向三用例 expectExit 0/1/2）+ `docs/p8/ECS-TOOLS-GUIDE.md` 归档 + MILESTONES 四栏 + tag + feat/ecs 全量 merge 回 master + 注册表同步（AGENTS/PARALLEL-TRACKS/wt_pool）。

## M-E3：收口段（feat/ecs，2026-11-09，tag `ecs-e3`）

**动作**：wt_pool claim（track ecs / M-E3）→ 重读判据（cases.json tool 卡三用例 + §7.2 文档闸 + 注册表同步）→ 真实输出留档（四用例实跑重定向 `example/ecs-e1a/*.log`：usage exit 2 / system+serial exit 0 / badjson exit 2 显式拒绝）→ `nanite-scene` 加 `--verbose`（断言成功逐条打印 ok 行，证据图数据源；expectQuat 顺带改单行输出防 4 连刷屏）→ 测试断言字符串全量 ASCII 化（工具输出纪律同 HUD：证据图 Consolas 无中文字形；注释中文保留）→ `tools/make-ecs-evidence.py`（Pillow terminal 风格，标题参与宽度计算防截断 + 超长行自动折行，全部内容来自真实日志）→ 4 张证据图逐张亲眼验收（首轮发现中文方框 + 标题截断两处，修复后复验）→ `example/ecs-e1a/run.bat`（四 suite + 负向 exit 2 校验 + 证据图重生成，ALL GREEN exit 0）→ `example/cases.json` 追加 `p8-ecs` 版本卡 4 条（usage/system/serial/negative，expectExit 2/0/0/2 + expectLog 锁死；**文本级插入**：先对齐 master 版（M6.1 max 口径修正 + s6.1 新版本），再在 versions 尾部追加，master 段逐条相等断言防 diff 噪音——首轮 json.dumps 全量重写产生 26 行 s6.0/s6.1 args 数组展开假 diff，checkout 重做）→ `docs/p8/ECS-TOOLS-GUIDE.md` 归档 → 本四栏 → 提交打 tag `ecs-e3` → merge 回 master + 注册表同步（AGENTS / PARALLEL-TRACKS §7.1c / wt_pool）。

**提交版本**：feat/ecs 本收口 commit（tag `ecs-e3`，即本条台账所在提交）；master 侧 merge commit 见 PARALLEL-TRACKS §7.1c ECS 行。

**完成情况**：run.bat ALL GREEN exit 0（四 suite 全过 + usage/badjson exit 2 校验 + good exit 0 + 证据图重生成）；4 张证据图亲眼验收无重叠/截断/乱码（usage 861×336 / system 696×532 / serial 1008×896 / negative-badjson 1158×224）；cases.json `p8-ecs` 卡 4 条 tool 用例 args/expectExit/expectLog 与真实行为逐条对上（usage 首行 "usage:" 实测核对）；ctest 8/8 不回退。收口四项自查：①渲染交代物——**无**（E1a 纯逻辑零渲染，无截图无 example 基线卡；渲染集成在 E1b 等 S6 收官）；②工具交代物——**本轮有**：展示台 p8-ecs 版本卡 4 条（含 preview 证据图）+ run.bat + 4 张证据图 + ECS-TOOLS-GUIDE + --verbose 逐断言输出；③MILESTONES 四栏（本节）；④固化——P8-ECS-PLAN M-E3 标完成 + TOOLS-GUIDE 踩坑存档 6 条 + 注册表同步留痕。

**踩坑情况**
1. 证据图首轮两处缺陷（中文断言字符串在 Consolas 下成方框；标题未参与画布宽度计算被截断）——生成器「脚本没报错」不等于产物可用，逐张亲眼验收是硬纪律；测试字符串 ASCII 化 + 生成器修宽度/折行后复验全过。
2. `cases.json` 全量 json.dumps 重写会把紧凑 `args` 数组展开成 26 行假 diff（p8-jobs 支线同款坑复发）——改文本级插入：以 master 现文本为底、versions 尾部追加、`d['versions'][:32] == orig['versions']` 断言 master 段逐字节不动。
3. wt 内 `git diff cases.json` 的 26 删不是本次改动——是 wt 基线（e283b86）落后 master（M6.1 修正）的表现；diff 基线是 wt HEAD 不是 master，判噪音前先看清和谁比。
4. expectLog 必须与真实输出首行核对（usage 卡初版写了臆造的 banner 行，实测首行是 "usage:"，已改）。

**下一步**：E1a 全链收口（merge 回 master + 注册表同步）；E1b（viewer 加载场景文件渲染 + ModelComponent/LightComponent）等 S6 收官（tag `s6-done`）后另编号开工。
## P8 M-C0 开发者控制台纯逻辑内核（feat/console 轨道，2026-11-08）

**动作**
- **决策前置**：三处参考实读——Carrot `console/Console.h/.cpp`（registerCommand + 补全 + toggleVisibility 结构；**命令不支持参数**，源码 TODO 挂着，RuntimeOption 是二级体系）、UE `HAL/IConsoleManager.h`（CVar bool/int/float/string 与命令**统一注册表**、help 必填、`SetOnChangedCallback`、命令回调带 args，只读）、bevy 无内建控制台（据实标注）。拍板（计划内）：注册体系/回调/参数化取 UE 口径，节奏参照 Carrot，不做 RuntimeOption 二级体系（CVar 覆盖其用途）。
- **`core/console/` 纯逻辑内核**（零 ImGui / 零图形符号）：`nl/core/console.hpp` + `core/src/console.cpp`——`Console::get()` 单例（UE/Carrot 同款）；CVar 四类型注册 + 类型化 set + `setFromString`（bool 接受 true/false/1/0/on/off/yes/no，数字全串校验）+ `setOnChanged` 变更回调；`registerCommand` 回调带 args（UE 口径）；`tokenize` 引号解析（双/单引号段可拼接，不支持转义，未闭合=错误）；`exec` 统一查找（CVar 无参=打印值+类型+help，单参=赋值）+ `ExecCode{Ok=0, UnknownCommand=1, BadRequest=2, CommandFailed=3}` 分档（M-C3 展示台 expectExit 的源头）；`completePrefix` 前缀补全候选（M-C1 Tab 消费）+ `allEntries`（M-C2 help/cvar.list 消费）+ 512 行环形日志（M-C1 滚动区消费）。重复注册：CVar 保留现值仅更新 help / 命令覆盖回调，记 Warn 不 assert。
- **`nanite-console` 工具**（只链 nl_core）：`selftest` 42 断言全绿（注册/读写/解析/执行/未知命令/回调/补全/日志，含负向：int 拒 '3.5'、float 拒 'xyz'、失败 set 保旧值、未闭合引号）；`exec "<line>"...` demo 注册表执行器，退出码=ExecCode。
- CMake：`nl_core` 加 `core/src/console.cpp`；新 exe `nanite-console`；ctest `p8-console-selftest`；编译选项 foreach 补 `nanite-console`（/utf-8 等，见踩坑 1）。

**提交版本**：feat/console 本轮 commit（M-C3 收口时打 tag merge）

**完成情况**
- selftest **42/42 PASS**，exit 0（ctest `p8-console-selftest` 登记同口径）。
- `exec` CLI 实跑验证（亲眼可见）：`demo.fov` → `[0] demo.fov = 60 (float) demo fov cvar`；`demo.fov 75` → `[0]`；`echo hello "big world"` → `[0] echo: hello big world`（引号参数保空格）；`add 1 2.5 3` → `[0] add: 6.5`；`demo.fov abc` → `[2] invalid value... expected float`；`no-such-cmd` → `[1] unknown command`。
- 无渲染依赖：`nanite-console` 只链 nl_core，`core/console/` 零图形符号，不碰 kStage、不进渲染回归（P8-CONSOLE-PLAN 双后端纪律豁免成立）。

**踩坑情况**
1. **MSVC 默认按 GBK 读源码**：新建 main.cpp 带 UTF-8 中文注释，未进 CMakeLists 编译选项 foreach 列表 → cl 按 GBK 解码产生幻影标识符（`r1 未声明` 等上百错误，报错行与真实代码对不上）。修正：foreach 补 `nanite-console`（/utf-8）。教训：新 exe/新 lib 进 CMake 必须同步进 281 行 foreach。
2. **selftest 断言笔误一次**：`tokenize("cmd a \"b c\" d")` 误断言 t1[1]=="b c"（实为 t1[2]），首轮 41/42；实现无错。教训：改完跑不绿先查断言再查实现。

**下一步**：M-C1 ImGui 前端（`` ` `` 呼出/收起、输入行 + ↑↓ 历史、滚动日志区带级别配色、Tab 补全；renderer/viewer 侧，`core/console/` 不动）。

## P8 M-C1 控制台 ImGui 前端（feat/console 轨道，2026-11-09）

**动作**
- **决策前置**：本里程碑点名的文件重读——Carrot `console/AutocompleteField.h/.cpp`（其补全依赖 **ImSearch** 第三方库：BeginSearch/SearchBar/SearchableItem + 上下键导航；本项目 vendored 清单只有 ImGui 四 cpp，**不引**，按 P8-CONSOLE-PLAN 判据「前缀匹配 + 唯一候选直接补全」自写 ~40 行）+ 本项目 `docs/IMGUI-GUIDE.md`（§3 明确「键盘字符/修饰键未接，InputText 要补字符回调 ~50 行」——本轮补齐）+ `viewer/main.cpp` / `window.cpp` / `hud.cpp` 全文（HudFrame POD 双向通道、keyPressed 边沿、截图确定性机制）。bevy 无等价（据实）；UE UConsole 打字流口径（只读）。
- **键盘输入通路**（IMGUI-GUIDE §3 预言的缺口补齐）：`core/window` 加 GLFW charCallback + KeyEvent 队列（PRESS/RELEASE/REPEAT 全记 + mods）→ `HudFrame` 定长 POD 数组（16 键 / 32 字符，溢出丢最旧）→ `console_ui.cpp` 映射 `ImGuiKey`（官方后端同款口径，Tab 不映射——走 viewer 边沿）喂 `io.AddKeyEvent`/`AddInputCharacter`。反引号字符在 core 过滤：只做呼出边沿，不进文本流。
- **`renderer/src/console_ui.{hpp,cpp}`**（renderer 侧第二个 ImGui include 点，符合铁律）：独立 Console 窗口（右上，避开 330 宽 HUD 面板，窄画布 min 兜底）；滚动日志区（Info 灰白/Warn 黄/Error 红，新行自动滚底、手滚上去不拽）；输入行（呼出首帧 `SetKeyboardFocusHere`；历史上下键走 ImGui 内建 `CallbackHistory`，带草稿暂存与连续重复去重）；Tab 补全（`completePrefix`，唯一候选直接补全+空格，多候选补到最长公共前缀无进展不动，幂等）。
- **viewer 键控**：反引号边沿呼出/收起；ESC 开着控制台先收；`hud.wantCaptureKeyboard` 回写屏蔽 Space/C/V/N/R 热键（读旧帧，防把 cvar 值当热键吞）；HUD 关着也排干事件泵（防 window 侧队列无限增长）。
- **新 CLI**：`--console on|off` / `--console-exec <line>`（启动预执行，结果留控制台日志=截图证据）/ `--console-type <text>`（输入行预填；截图模式合成 Tab 边沿演示补全，幂等保确定性）。
- CMake：`nl_renderer` 加 `console_ui.cpp` + PRIVATE 链 glfw（只吃键值常量，窗口库仍归 core/viewer）；`hudSelfTest` 补控制台通道 POD 缺省断言。

**提交版本**：feat/console 本轮 commit（M-C3 收口时打 tag merge）

**完成情况**
- viewer selftest **16/16**（含新增控制台 POD 缺省断言）+ ctest `p8-console-selftest` PASS（M-C0 内核不受前端影响）。
- **验收截图（亲眼可见，DX12 960x540）**：`build/out/mc1-console-evidence-dx12.png`——反引号呼出的 Console 窗口与 HUD 同屏不重叠；日志区完整回显 `echo hello world`→`echo: hello world`、`demo.fov 75`→`demo.fov = 75`、`demo.fov`→带类型+help 的打印、`add 1 2.5`→`add: 3.5`，错误行 `unknown command: no-such-cmd` 红色；输入行显示 `demo.fov ` = 预填 `demo.fo` 经 Tab 补全产物（stderr 插桩证实 frame 2 起 `demo.fov ` 稳定）。
- Vulkan 后端：viewer 编译通过 + 证据截图（console_ui 零 GPU 代码，通道层验证；P8-CONSOLE-PLAN 双后端画面回归豁免成立——不新增 shader/GPU 资源，console 关闭时 HUD 布局一像素不动）。
- 未跑渲染回归（总闸口径：本轨道不进回归；console 关闭 = 历史 HUD 基线零波及，`hudSelfTest` POD 缺省断言锁住）。

**踩坑情况**
1. **PowerShell cwd 落错树**：用绝对路径调 exe 但 cwd 停在主 worktree，`--screenshot out/xxx.png` 相对路径把产物写进了 master 的 `out/`（占坑纪律第 4 条当场自查发现，迁移+清理）。教训：worktree 内跑 exe 必须先 cd 到 wt 路径，产物路径不能依赖 shell cwd 假设。
2. **ViewImage 缓存假信号**：截图重拍后连续两次「图像未变化」——用 md5/时间戳+stderr 插桩才确认补全实际已生效，假信号是工具侧缓存。教训：像素级判断以文件 hash + 程序内插桩为准，别让「看图工具说没变」盖过机械证据。
3. **演示 Tab 需要 viewer 合成边沿**：截图模式无真实按键，`window.keyPressed(Tab)` 恒 false——`--console-type` 存在时由 viewer 合成（applyTabCompletion 幂等：补完再 Tab 前缀为空/无进展直接返回，逐帧合成无副作用）。交互模式仍走真实 Tab 边沿。
4. **GLFW 常量跨层**：GLFW key code 在 core 收集（int 原值）、renderer 映射 ImGuiKey——GLFW 头只进 `console_ui.cpp`（窗口库常量非图形 API，nl_renderer PRIVATE 链 glfw 注释写明）。

**下一步**：M-C2 运行时调参 + 内建命令集（`help`/`cvar.list`/`cvar.set`/`hud.*`/`cam.*`，CVar 变更回调接相机/HUD 参数；viewer 起步注册表 `demo.*` 届时替换）。交互模式键盘输入（真实打字/上下键历史/真实 Tab）建议用户开窗手验一次——截图管道已证 ImGui 通路，GLFW charCallback 属系统级输入，隐藏窗口收不到。
## P8 M-C1 修复包：交互手验暴露的三个真 bug（feat/console，2026-11-09）

**动作**
- SendInput 自动化手验链（Python ctypes SendInput + PrintWindow 截图，绕沙箱对 .NET 动态编译/COM 实例化的封锁）替代/补充隐藏窗口截图，逐键验证交互通路，暴露三个 M-C1 截图验收盖不住的 bug：
- **① Tab 补全交互模式失效**：InputText 激活期真值在其内部缓冲，外部改 `g_input` 默认不可见（写回只在失焦，imgui_widgets.cpp 5156）。修：改 buf 前先从 `st->TextA` 同步真值 → `applyTabCompletion()` → `ReloadUserBufAndMoveToEnd()`（imgui_internal.h 官方重载机制，`WantReloadUserBuf` 在本帧 InputText 开头消费、光标落尾）。截图模式反而能过是隐藏窗口焦点路径不同的假象。
- **② 回车后输入行失焦**：`EnterReturnsTrue` 执行完主动失活，后续打字全落空。修：`g_refocus` 标记，下帧 `SetKeyboardFocusHere` 重抢焦点（终端语义）。
- **③ 反引号呼出被中文 IME 吞掉**：插桩实证 ESC/TAB 的 WM_KEYDOWN 到 GLFW、VK_OEM_3 一个事件都没有——IME 系统层抢键。修：`ImmAssociateContext(glfwGetWin32Window(), nullptr)` 全窗口解除 IME（nl_core 链接 imm32；控制台输入本就 ASCII，ImGui 默认字体无 CJK 字形，游戏视口不该被 IME 拦热键）。
- **④ 顺带**：Console 窗口 `NoResize|NoCollapse` 摘除 + `SetNextWindowPos/Size` 从 `ImGuiCond_Always` 改 `FirstUseEver`（Always 每帧强制归位，拖动/缩放静默失效）——响应用户「可关闭/可移动/可缩放」诉求。

**提交版本**：feat/console 本轮 commit（修复包）

**完成情况**
- 自动化验证（DX12 960x540，SendInput 注入 + PrintWindow 截图 + stderr 插桩三重证据）：Tab 补全（`demo.fo`+Tab→`demo.fov |`）、连发三条命令全执行（回车后焦点保持）、ESC 收起、`` ` `` 再呼出（stdout `console -> open` + keyprobe glfwKey=96）全部通过。
- UP/DOWN 历史回填在注入环境复现不了（GLFW 探针零事件——VK_UP 在系统层被吃，与 grave 的 IME 现象同层但 ImmAssociateContext 后依旧；WorkBuddy 环境的注入链路限制），通道代码为 ImGui vendored 官方 `CallbackHistory` 用法（DeleteChars/InsertChars），**留用户真手验**。
- 窗口可拖动/可缩放/可折叠，位置大小记忆（FirstUseEver）。

**踩坑情况**
1. **x64 INPUT 结构体少算 8 字节**：union 只放 KEYBDINPUT（20B 对齐后 24B）→ sizeof=32，真身 40（MOUSEINPUT 最大成员）；SendInput 校验 cbSize 不符**静默返回 0**，一个键都没进去。修：union 补 MOUSEINPUT。教训：ctypes 定义 Win32 结构必须与 MSDN 逐字段核对宽度。
2. **BitBlt 抓 flip-model swapchain 得过期帧**：四张截图 MD5 完全一致而标题栏帧号在走。修：`PrintWindow + PW_RENDERFULLCONTENT`。另 Python 进程需 `SetProcessDPIAware()`，否则 GetWindowRect 返回逻辑坐标截不全。
3. **WorkBuddy 环境焦点窃贼**：注入中途前台可被夺走，按键进错窗口；每阶段前重申 SetForegroundWindow + Alt 解锁技巧。
4. **检查工具链**：沙箱拦 .NET 动态编译与 COM 实例化；键盘注入改走 Python ctypes SendInput，截图走 GDI PrintWindow，均纯标准库/系统 API。

**下一步**：用户真手验 UP/DOWN 历史（唯一遗留）→ M-C2 内建命令集（help/cvar.list/cvar.set/hud.*/cam.* + 变更回调）。


## P8 M-C2 运行时调参 + 内建命令集（feat/console 轨道，2026-11-09）

**动作**
- **决策前置**：本里程碑点名证据重读——Carrot `console/RuntimeOption.hpp/.cpp`（bool-only 静态注册表二级体系，M-C0 已拍板不复刻，CVar 覆盖其用途）、UE `Core/Private/HAL/ConsoleManager.cpp`（help/dump 类查询内建在 ConsoleManager 本体——`DumpObjects`，CVar 注册 `RegisterConsoleVariableRef`+help 必填；bevy 无等价据实）。计划口径不变：内建命令进 core 注册表，业务侧只注册引擎参数。
- **`Console::installBuiltins()`**（core，幂等二次调用无副作用）：`help`（列全部命令+hint）、`cvar.list`（列全部 CVar：名字 = 现值 (类型) help）、`cvar.set <名> <值>`（走 setFromString 同路径，脚本化执行用）。lambda 内不调 exec()（避免二次记 `> cmd` 日志行）。
- **`OrbitCamera` FOV/距离可写**：`kFovY` 从 constexpr 降级为初值，实例字段 `fovY_`（缺省 = 历史 45°，老基线零波及）；新增 `setFovYDegrees`（clamp 20°..120°）/`fovYDegrees`/`setDistance`（clamp 与 zoom 同阈）。`viewProjection`/`projectionYScale` 改用 `fovY_`，数学路径不变。
- **viewer 注册表 `demo.*` → 真参数**：`cam.fov`/`cam.dist`（onChanged 回调打进相机，cvar 初值取相机实际状态）、`hud.show`（onChanged 写 `hudEnabled` 本地可变量——`o` 是 const）；`echo`/`add` 保留；返回「相机实际值→cvar 静默回写」每帧同步函数（`g_syncing` 挡 onChanged 重入），滚轮 zoom / reset 后 `cam.dist` 不陈旧。
- **HUD 面板与控制台解耦**（验收中发现的真问题：`hud.show false` 会把画在 HUD pass 里的控制台一起带走，且控制台死了就无法再敲 `hud.show true`）：`HudFrame.panelShow`（缺省 true）+ hud.cpp 面板整块作用域包裹；viewer 传参条件 `hudEnabled ? &hud : nullptr` → `(hudEnabled || consoleOpen) ? &hud : nullptr`。历史基线安全：HUD 关+控制台关时仍传 nullptr（零 ImGui，一像素不动），hudSelfTest 补 `panelShow` 缺省断言。
- **nanite-console 自测第 8 节**：help 列表 / cvar.list 现值 / cvar.set 三态（成功 / 坏值保旧 / 未知名 / 缺参）/ double-install 幂等；demo 注册表挂 installBuiltins（M-C3 展示台用例真实执行体）。

**提交版本**：feat/console 本轮 commit（M-C3 收口时打 tag merge）

**完成情况**
- selftest **49/49 PASS**（新增 7 条内建命令断言），viewer selftest 16/16（含 panelShow 缺省）。
- **验收截图（DX12 960x540，亲眼可见）**：
  - `docs/screenshots/mc2-registry-dx12.png`——`help` 列出 add/cvar.list/cvar.set/echo/help；`cvar.list` 列出 `cam.dist = 4.2 (float)` / `cam.fov = 45 (float)` / `hud.show = true (bool)`（相机实际值）；输入行 `cam.fov |` = `cam.fo` Tab 补全产物（新注册表下补全照常）。
  - `docs/screenshots/mc2-fov100-dx12.png`——`cam.fov 100` 生效（模型透视明显变广 vs 45°）；`hud.show false` 只藏 HUD 面板、**控制台窗口存活**（解耦生效），日志回显 `cam.fov = 100` / `hud.show = false`。
- Vulkan 后端编译 + 证据截图 `docs/screenshots/mc2-registry-vulkan.png`（HUD `backend : vulkan` 同框 `help`/`cvar.list` 完整输出 + Suzanne.glb 贴图模型正常；console_ui 零 GPU 代码，通道层验证；P8-CONSOLE-PLAN 双后端画面回归豁免成立）。
- 未跑渲染回归（总闸口径：panelShow 缺省 true + 传参条件在双关时仍为 nullptr，历史 HUD 基线零波及；hudSelfTest 缺省断言锁住）。

**踩坑情况**
1. **自测断言笔误**：`cvar.list` 断言写死 `st.count = 7`，但第 6 节回调测试已把它改成 8——首轮 48/49。教训：断言引用可变状态前先核对当前值。
2. **`hud.show false` 连带杀控制台**（设计缺陷非代码 bug）：控制台画在 HUD pass 里，`hudEnabled=false` 连 ImGui 帧都不跑；且控制台死了无法再敲 `hud.show true`，等于单程票。修：`panelShow` 解耦面板可见性与 ImGui 帧存续。
3. **首版注册表 lambda 悬垂**：`CVar&` 局部引用被 lambda 按引用捕获，函数返回即 UB——重写为 `CVar* const` 指针捕获（指向注册表 map 节点，与 Console 单例同寿）。

**下一步**：M-C3 收口（展示台 tool 用例 usage/正向/负向 + TOOLS-GUIDE 归档 + tag + merge 回 master + wt_pool release）。

## P8 M-C3 收口：展示台用例 + 归档 + tag + merge（feat/console 轨道，2026-11-09）

**动作**
- **证据补档（用户点名「还是没有交代的图片」的修正）**：M-C2 验收截图原本只拍在 `build/out/`（gitignore 内，没进 git 不可追）——三张图肉眼核验后归档 `docs/screenshots/`（mc2-registry-dx12 / mc2-fov100-dx12 / mc2-registry-vulkan），Vulkan 对称图为补拍（build-vulkan viewer `--console-exec help/cvar.list` + PrintWindow 截图脚本 snap.py）；台账证据路径改指可追位置；`docs/screenshots/CONSOLE-EVIDENCE.md` 图目索引（含 M-C1 图目与拍法口径）。纪律固化：**证据图必须落 `docs/screenshots/`，`build/out/` 只作中间产物；收口必须展示图，不许只给路径**。
- **展示台 `console-mc3` 版本卡**（M-C0~C2 欠的 tool 交代一并补齐）：`example/cases.json` 五用例——usage(exit 2) / selftest 49 条(exit 0) / exec 正向三连 echo+cvar.list+cvar.set(exit 0) / 负向未知命令(exit 1) / 负向 cvar.set 坏值保旧(exit 3)；exe 字段 dx12/vulkan 同指 `build/nanite-console.exe`（纯 CPU 不链图形后端）；`expectExit` 如实按 ExecCode 四档分，不为迁就 nanite-tools 的 0/1/2 全局约定改语义（note 写明双约定并存）。
- **归档**：`docs/p8/CONSOLE-TOOLS-GUIDE.md`（定位 / ExecCode 退出码语义表 / 用法 / 五用例实跑输出逐条归档 / 踩坑三条）。

**提交版本**：feat/console `fc61c2d`（M-C2 证据补档）→ M-C3 本轮 commit（tag `console-mc3`）→ merge master

**完成情况**
- 展示台五用例全部实跑核验：退出码 2/0/0/1/3 与 ExecCode 分档逐一对上，`expectLog` 子串全部命中真输出（含 `demo.fov = 90`、`unknown command: nonsense.cmd`、`invalid value for demo.fov: expected float, got 'abc'`）；`example/cases.json` JSON 合法性校验过、`server.py --check` 退出 0。
- selftest 无管道直跑复核 exit 0（`| tail; echo $?` 拿的是 tail 退出码——M6.0 已留痕的坑，本次守住）。
- `docs/p8/CONSOLE-TOOLS-GUIDE.md` 归档完成；`docs/screenshots/` 三张 M-C2 证据图 + 图目索引入库。
- tag `console-mc3` 在验收通过后打；merge 回 master + master 侧注册表同步（AGENTS.md / PARALLEL-TRACKS §7 / P8-CONSOLE-PLAN 状态）+ wt_pool release。

**踩坑情况**
1. **M-C2 截图落在 `build/out/` 不可追**（收口纪律四项自查漏 ① 渲染交代物）——用户点名后补档；修复不止拷图，还包括台账路径、图目索引、纪律固化三点。
2. **退出码双约定**：ExecCode 四档 vs nanite-tools 0/1/2——按「语义如实」处理而非强行归一，卡内 note 留痕。
3. `server.py --check` 只按 kStage 校验渲染卡，工具卡不在其范围——JSON 合法性另行 `json.load` 校验，五用例退出码/输出以实跑为准。

**下一步**：轨道收官。wt（nl-engine-console）按 §7.1b 保留分支拆 worktree；M-C4 级别的后续（命令自动补全模糊匹配、日志导出等）不在本计划，需要时另立项。

## pbr-direct：PBR 直射光 + glTF 贴图采样（S7 前置穿插项，2026-11-09，tag `pbr-direct`）

**动作**：feat/pbr-direct worktree（`E:/UE_STUDY_2026/nl-engine-pbr-direct`，junction 共享 `.workbuddy`/`slang dist`）。model.slang 新增 shading mode **5** = Cook-Torrance 直射光（D=GGX walter07 数值稳定式、V=Smith 高度相关精确式、F=Schlick、diffuse=Lambert*(1-F)*(1-metallic)；与 bevy `pbr_lighting.wesl` / UE `BRDF.ush` / Carrot `brdf.glsl` 三处对拍同构）；`core/mesh_io` 提取 metallicRoughness 贴图（G=roughness B=metallic，glTF 2.0 spec §3.9.3）与 baseColor/metallic/roughness 因子；scene 走 t1 MR 贴图（RGBA8_UNORM 线性）+ 新开 b1 `LightConstants`（80B volatile CB）+ init/initSkin 绑定集扩 slot1；viewer `--shading pbr` + `--pbr-naive` 对照开关 + HUD combo；输出端 pow(1/2.2) gamma 编码（不做 tonemap，plan 边界不变）；kStage bump `pbr-direct`。

**提交版本**：`d277bc7`（feat/pbr-direct，代码+example）+ 本条固化提交（docs），tag `pbr-direct`

**完成情况**：
- 机械判据：`--shading pbr` 双后端 PNG **逐字节相同**（MD5 `54ff8a33…`，bytes 档）；deviceErrors=0 ×2、调试层零输出；selftest 16/16 ×2；32 个 shader 产物魔数全 ok（单源双目标）。
- 不变量判据：不带新参数行为不变——textured 基线 MD5 `7e528087…` 与 `example/m0.4` 冻结基线逐字节一致（双后端同 MD5）；回归 spot：s4.0 4/4 bytes、s5.1/s5-traverse-far 2/2 bytes（cluster/cull/dag 管线像素零扰动）。
- sRGB 对照（验收判据 3）：`--pbr-naive`（误用 UNORM 直读）泛灰偏亮、高光对比糊掉；正确路径深色金属 + 金色眼 GGX 高光。Suzanne MR 实测 metallic≈0.98 / roughness≈0.32——无 IBL 直射光下金属只剩高光，身体深色是物理正确行为（S9 IBL 的活教材）。
- 交代物：`example/pbr-direct/`（pbr/naive/hud/textured 四对基线 + run.bat + cases.json 卡 5 用例）+ `server.py --check` OK（stage=pbr-direct cases=5 baselines=8）。
- kStage bump 波及：11 对 exe=current 的 HUD 基线（stage 文字行）全部重拍（m0.9/s2.0/s3.2/s4.1/s4.2/s5.0/s5.1/s5.2×2/s6.0×2）。
- 固化：本条 + `docs/s0/PBR-DIRECT-PLAN.md` 状态与偏差记录。

**踩坑情况**：
1. ⭐pbr 分支只能编译进 psMain（§2.7 新例）：共享 `shade()` 也会被 psCluster/psCull 编译，而 cull/dag 管线的 t1~t4 是 StructuredBuffer（顶点池/索引/元数据/可见列表）——同一 descriptor slot 两种资源类型在 Vulkan descriptor set layout 里非法。pbr 移入 psMain 专属分支后 cluster/cull/dag 的 SPIR-V 不引用 t1/b1，绑定面只动 init/initSkin。
2. ⭐写空 volatile CB = 段错误：draw() 常规路径无条件 `writeBuffer(lightConstants_)`，而 initCluster 场景（lod 模式走这里）没有 b1 槽位、句柄为空 → 0xC0000005。kStage 重拍 11 对基线时被 s2.0/s3.2/s5.0 六连崩暴露；`| tail` 管道吃掉退出码一度误判「手动跑不崩」（tail 恒 0），python subprocess 直跑退出码才现形。修复 = null 守卫（语义也正确：无 b1 的场景本来就不该写）。
3. mode 编号：plan 预写的「mode 4 = pbr」与 M1.3 cluster 色撞号，改用 5（重编号 cluster 会动 viewer 缺省与历史基线，零收益）。
4. t0 格式不能动：plan 的「t0 用 RGBA8_UNORM_SRGB」会让 M0.4 textured 基线全变（验收判据 2 是硬闸）——t0 保持 UNORM，pbr 分支在 shader 内做精确分段 sRGB→linear（`--pbr-naive` 跳过即对照路径）。
5. HUD 回写钳制：非簇场景 `min(shading,3)` 会吞掉 pbr——改为只挡 4=cluster（唯一非法值），5=pbr 放行。
6. STAGE_RE 闸口扩展：`tools/showcase/server.py` 原只认 `M*/S*` 前缀，具名穿插项（pbr-direct，不占 m0.*/S* 编号是 plan 明文）加进白名单，version id = 前缀小写。
7. 光照强度 2.5：金属体无 IBL 直射高光偏弱，强度从 1 提到 2.5 让 GGX 形状醒目（光线方向沿用 b0 g_lightDir 缺省，确定性截图不受影响）。

**下一步**：已 merge 回 master（2026-11-09，merge commit `e57173d`，kStage=`pbr-direct`、17 对 exe=current HUD 基线随 bump 重拍、--check OK + pbr-direct 10/10 + s4.0 4/4 + s6.2 2/2 回归 bytes）；PRT 球谐环境光 / Kulla-Conty 两个 GAMES202 点解锁（ROADMAP 穿插项表），S7 只剩 vis buffer + vertex pulling + 材质解析。

## P8 M-S0：vendored miniaudio + nl/audio 封装 + 离线确定性 PCM（feat/audio 轨道，2026-11-09，tag `audio-s0`）

**动作**：用户「开工 M-S0 建 feat/audio」= 提前解禁音频轨道 M-S0~S2 排期闸（M-S3 场景集成仍 gate 在 P8 E1/E1b 之后，照物理 §7.1e 先例）。基于 master 建 `feat/audio` worktree（`E:/UE_STUDY_2026/nl-engine-audio`，两处 junction 共享 + skip-worktree）。三处参考实读：Carrot `engine/audio/` 对象模型（Manager→SFX/Music→Sound→SoundSource→SoundListener，AudioThread 不抄）/ bevy `bevy_audio` `audio_output.rs`（无设备 `stream: None` 优雅降级）+ `audio.rs`（无 HRTF 只左右声像）/ UE `AudioDebug.cpp`（M-S2 调试字段口径，只读不抄）；ROADMAP 对照总表无音频行（A 类轨道），按铁律就近读三处音频同类模块。
落地：`thirdparty/miniaudio/` vendored **v0.11.26（2026-10-31）**（`miniaudio.h` + 单实现 TU `miniaudio.c` + `VENDOR.md`，选 Public Domain / Unlicense）；CMake `nl_miniaudio` 静态目标 + `nl_core` 挂 `core/src/audio/AudioWorld.cpp` + `audio-tools` 工具 + ctest `s0-audio-selftest`；`nl/audio::AudioWorld` 薄封装（Clip 只读 WAV 头探测 / Source 同步预解码 / Listener 位置 / 离线 noDevice `ma_engine_read_pcm_frames` f32 2ch / 实时默认设备阻塞播放）；`scripts/make-audio-wav.py` 生成确定性 440Hz 衰减 WAV；`audio-tools`（selftest / probe / dump / check / play，退出码 0/1/2）；`example/audio-s0/` 交代物（run.bat 七段实跑 + 7 张 log + 波形/验收 PNG + bad provenance 负向夹具）+ `example/cases.json` `audio-s0` 卡 7 用例 + `docs/p8/AUDIO-TOOLS-GUIDE.md` 归档。

**提交版本**：feat/audio 本轮 commit（hash 见收口回复），annotated tag `audio-s0`

**完成情况**：
- **双后端 BUILD OK**：`scripts/build-audio-tools.bat DX12` 与 `VULKAN` 各自 configure + 编译 + `audio-tools selftest` **21/21 PASS**（A 类纯 CPU 工具，不进渲染 bytes/near 回归总闸、不动 kStage）。
- **确定性硬判据**：两次独立 noDevice world 渲染 24000 帧 × 2 声道 = 48000 个 float **逐位一致**（memcmp）；单声道展开 L/R 完全相同；峰值 0.595886 / RMS 0.122469 / 非零帧 23950；前 512 帧 RMS > 后 512 帧 4 倍（指数衰减包络可见）。
- **防假绿**：`check` 从 provenance 读参数（wav 写仓库相对路径、跨 checkout 可解析）、核 tsv 的 FNV-1a 哈希、再重建 world 重渲染，48000 个 float 位级 `==`；篡改 sampleRate=44100 在字段校验阶段显式 exit 1。展示台 `audio-s0` 卡 7 用例（usage 2 / selftest 0 / probe 0 / dump 0 / check 0 / 缺 WAV 1 / 坏 provenance 1），`server.py --check` OK。
- **亲眼可见证据（已肉眼核验）**：`example/audio-s0/waveform.png`（真实 PCM dump 绘制的 L/R 双栏衰减波形，参数行/hash/峰值完整，无截断无方框）+ `acceptance.png`（七条命令真实 stdout 渲染，七节齐全）。两图均由 `tools/make-audio-evidence.py` 从真实输出生成，非手绘。
- `example/audio-s0/run.bat` 全链 exit 0（AUDIO M-S0 ACCEPTANCE: ALL GREEN）。
- **边界**：M-S0 不做 3D 空间化（M-S1）/ console cvar + ImGui 面板（M-S2）/ ECS 集成（M-S3）；`core/audio/` 零图形符号；未 merge 回 master（用户未要求，轨道收口在 M-S3）。
- **主树注册表同步**：`AGENTS.md`（音频表行 + 例外 4）、`docs/PARALLEL-TRACKS.md`（§7.1b 表 + 新增 §7.1f）、`docs/ROADMAP.md` E4 行、`docs/NON-RENDER-ROADMAP.md` 候选总表/例外段、`tools/wt_pool.py` TRACKS（`audio` planned→active + M-S3 gate note）已**就地同步但留在主树工作区未提交**：同文件含另一会话 M-J5a（job-system 重启）的未提交改动，精确 commit 会连带扫走别人现场；待该会话收口后一并提交。内容口径与本台账一致。

**踩坑情况**：
1. **Python `// 1` 不转整数**：`FRAMES = SAMPLE_RATE * DURATION_S // 1` 得到 float → `range()` TypeError；改 `int(...)`。
2. **`git update-index` 的 `--stdin` 必须置于末尾**：`-z --stdin --skip-worktree` 报 `option 'stdin' must be the last argument`；正确 `-z --skip-worktree --stdin`。共享 `.workbuddy` 5 个 memory 文件靠这步 skip-worktree，否则 junction 内容一变工作树就脏。
3. **provenance 写死本机绝对路径**：展示台换个 checkout 跑 `check` 就打不开 WAV。修 = `portableWav()`（写仓库相对路径）+ `resolveWav()`（读时拼编译期 `NL_AUDIO_ROOT`），坏路径仍显式 exit 1。
4. **miniaudio 0.11.26 签名与 noDevice 契约**：`ma_sound_init_from_file` 是 **6 参**（新增 `ma_fence* pDoneFence`，传 nullptr）；`noDevice=TRUE` 时 `channels`/`sampleRate` **必须显式赋值**（头 1086-1099 行）否则 engine 无从推断格式；engine 内部恒 f32（78146 行注释实锤）。
5. **生命周期顺序**：`ma_sound` 必须先于 `ma_engine` uninit（声源节点挂在 engine 的 node graph 上），`Impl::~Impl` 显式遍历再 `ma_engine_uninit`。
6. **从 Bash 调 PowerShell 被安全策略拦**：junction 建立必须走独立 PowerShell 工具（`New-Item -ItemType Junction`），不能 Bash 里 `powershell -Command`。
7. **MA_SOUND_FLAG 组合**：`DECODE` 同步全解码是离线确定性的前提（避开 resource manager 异步 job）；`NO_SPATIALIZATION` 是 M-S0 有意简化，3D 留 M-S1，已写进 GUIDE 与计划。
8. **证据图全 ASCII 文本**：Consolas 无中文字形（M-SC0 中文方框教训），本轮预防性守住。

**下一步**：M-S1（3D 定位 / 4 种距离衰减 / 多声源并发 / Clip 缓存）；M-S2 游戏内调试（console cvar + 独立 ImGui 窗口，默认关、不动常驻 HUD）；M-S3 场景集成待 E1/E1b 收口。

## P8 M-S1：3D 定位 + 4 种距离衰减 + 多声源并发 + Clip 缓存（feat/audio 轨道，2026-11-09，tag `audio-s1`）

**动作**：用户「继续做 M-S1」。先 `wt_pool claim --track audio --milestone M-S1` 占坑。三处参考**重读**（铁律：新里程碑即使同阶段也按对照总表重读）：Carrot `engine/audio/OpenAL.cpp:86` / `SoundSource.cpp:77` / `SoundListener.cpp`（只有 `setPosition` 转发 `alSource3f`，**无衰减模型参数**）/ bevy `audio_output.rs:330-380` / `audio.rs:56` / `sinks.rs:336-355`（`SpatialAudioSink` 仅 emitter/双耳 + `SpatialScale`，注释「无 HRTF、只左右声像」）/ UE `Engine/Classes/Engine/Attenuation.h:16-24`（**只读不抄**：`EAttenuationDistanceModel` 6 种 + 4 shape + LPF + `AudioComponent.h:815 IsInAudibleRange`）。结论：对象模型借 Carrot、衰减 API 不照抄；3D 不必 HRTF（bevy 印证）；超距判定口径借 UE `IsInAudibleRange`；落码依据 = miniaudio 原生 `ma_attenuation_model`（正好 4 种：none/inverse/linear/exponential，inverse 等价 OpenAL `INVERSE_DISTANCE_CLAMPED`，与 Carrot 底座同源）。`docs/carrot-history.md` / `bevy-history.md` 全文无音频条目，本轮以三处源码实读为准。
落地：`AudioConfig` 加 `spatialization`（**默认 false** → M-S0 format=1 基线逐字节不变）；`nl/audio` 新增 4 种 `AttenuationModel` 枚举、声源/听者位置朝向 setter、超 maxDistance 按 UE 口径补增益 0 剔除、`ma_resource_manager` 承担 Clip 缓存（`registerClip` / `createSourceFromClip` / `clipCacheStats`）；`audio-tools` 新增 `spatial <outdir>`（21 距离 × 4 模型 = 84 次独立离线渲染 + 两声源并发混音）+ `check` 支持 format=1(M-S0)/format=2(M-S1) 双格式（M-S1 从 provenance 全量重渲染逐字比对）；`scripts/make-audio-wav.py` 加频率/幅度参数生成第二素材 `test-tone-b.wav`（原 `test-tone.wav` 默认参数不变、逐字节不变）；`tools/make-audio-evidence.py` 加衰减/混音/验收三图；`example/audio-s1/` 交代物（run.bat 全链 + 7 log + spatial/ 三文件 + bad/ 与 bad-prov/ 两个负向夹具 + 三证据图）+ `example/cases.json` `audio-s1` 卡 7 用例 + `docs/p8/AUDIO-TOOLS-GUIDE.md` 追加 M-S1 节 + `docs/s5/UE-DIFFS.md` 增 D16。

**提交版本**：feat/audio 本轮 commit（hash 见收口回复），annotated tag `audio-s1`

**完成情况**：
- **双后端 BUILD OK**：`build-audio-tools.bat DX12` 与 `VULKAN` 各自编译 + `audio-tools selftest` **33/33 PASS**（M-S0 21 + M-S1 12）；A 类纯 CPU 工具，不进渲染回归、不动 kStage。
- **4 种衰减模型分离**：听者原点面向 +Z、声源沿 +Z 步进 21 点；每点 warmup 512 帧越过 `gainSmoothTimeInFrames=360`（7.5ms 增益平滑）后测 1024 帧 RMS。rolloff 取 **0.5**（rolloff=1 时 inverse≡exponential 数学恒等，曲线无法区分——实踩坑）；none 恒定 / inverse 最陡 / linear 线性 / exponential 幂律，4 条曲线肉眼可辨。
- **超距静音**：maxDistance=20 后 inverse/linear/exponential 增益由 `nl/audio` 按 UE `IsInAudibleRange` 强制 0（miniaudio 公式在 d>max 仍给极小正值，不自动静音——D16 留痕），证据图 `z=20` 静音竖线可见。
- **并发线性叠加**：声源 A（440Hz `test-tone.wav`）与 B（660Hz `test-tone-b.wav`）并发；MIX 与 A+B 逐样本 `maxAbsDiff=0 / exactSamples=1024`（残差零平线）。
- **Clip 缓存**：`ma_resource_manager` 承担 PCM 去重；2 clips / 2 misses / 2 hits / 4 sources，缓存统计入 provenance 并由 `check` 重探测比对。
- **防假绿 format=2**：`check` 不信任 tsv——从 provenance 重建 21×4 衰减 + 512 混音帧 + 重探测缓存统计，渲染成格式化文本逐字比较；两条负向夹具分别被抓：`bad/`（超距静音行篡改 → `attenuationHash` 校验阶段 exit 1）、`bad-prov/`（maxDistance 由 20→10 → 哈希合法、**重渲染后曲线不匹配**阶段 exit 1）。
- **M-S0 基线零回归**：`test-tone.wav` 默认参数未动，重新 dump+check 仍 `waveformHash=4293b40fe2f9a770` 逐字节一致；`audio-s0` 展示台卡仍 PASS。
- **亲眼可见证据（已肉眼核验）**：`example/audio-s1/attenuation.png`（4 曲线分离 + 静音竖线 + 参数/hash 完整）、`mixing.png`（A/B/MIX/RESIDUAL 四栏铺满全宽、残差零平线）、`acceptance.png`（七条命令真实 stdout、七节齐全、无中文方框）；均由 `make-audio-evidence.py` 从真实输出生成，非手绘。
- `example/audio-s1/run.bat` 全链 exit 0（AUDIO M-S1 ACCEPTANCE: ALL GREEN）；`server.py --check` exit 0；展示台 `audio-s1` 卡 7 用例（usage 2 / selftest 0 / spatial 0 / check 0 / probe-b 0 / 坏 tsv 1 / 坏 provenance 1）实跑全绿。
- **边界**：M-S1 不做 console cvar / ImGui 面板（M-S2）、不碰 ECS 场景集成（M-S3）；`core/audio/` 零图形符号；未 merge 回 master（轨道收口在 M-S3）。

**踩坑情况**：
1. **rolloff=1 让 inverse≡exponential**：两者公式数学恒等（都是 `min/d`），证据图只剩 3 条可辨曲线；改 `rolloff=0.5` 分离四条。
2. **增益平滑 warmup**：miniaudio `gainSmoothTimeInFrames=360`（7.5ms），直接读首帧量到过渡段而非稳态；每距离点先 warmup 512 帧再测 1024 帧。
3. **超距不自动静音**：miniaudio 衰减公式 d>max 仍给极小正值；`nl/audio` 按 UE `IsInAudibleRange` 补 `gain=0` 剔除（D16 留痕）。
4. **第二素材频率必须不同**：`test-tone-b.wav` 660Hz 与 `test-tone.wav` 440Hz 频率不同，才能证明并发是两路独立信号相加而非复制；默认参数脚本不改原素材（逐字节不变）。
5. **证据图宽度铺满**：`draw_channel` 原 `cols=min(w,len)` 在 512 帧时只占左 43% 像被裁切；改按画布宽铺满（24000 帧的 M-S0 图不受影响）。
6. **`const_cast<Impl*>` 误用**：4 个只读访问器重复该写法，抽成 `Impl::at()` 内联成员。
7. **miniaudio `ma_attenuation_model` 正好 4 种**：落码即原生 4 种（计划 §4 已定），属计划内机械执行，不另请拍板；6 种 UE 模型（含 Custom / NaturalSound 等）不进本轮。

**下一步**：M-S2 游戏内调试（console cvar `audio.volume/debug/solo/freeze/dump` + 独立 ImGui 声源面板，默认关、不动常驻 HUD）；M-S3 场景集成待 E1/E1b 收口。

## P8 M-S2：游戏内音频调试——volume / solo / freeze 单步 / 声源快照（feat/audio 轨道，2026-11-09，tag `audio-s2`）

**动作**：用户「继续做 M-S2」。`wt_pool claim --track audio --milestone M-S2` 占坑。三处参考重读：Carrot `engine/audio/`（无调试面，仅 AudioManager/SoundSource 基础面）/ bevy `audio.rs`（`play` + sink 即全部，无调试）/ UE `AudioDebug.cpp`（**只读不抄**：`au.DumpActiveSounds`、命令级 solo/mute、`au.3dVisualize.*` 字段口径——M-S2 快照字段清单照它定）。落码：`AudioDebugController`（core 层单例，零图形零 console，管 masterVolume/solo/frozen/debug 四态，默认纯透传保 M-S0/M-S1 基线逐字节不变）；`AudioWorld::refreshAudibility` 折叠 masterVolume+solo 因子进 effectiveGain；`readFrames` frozen 时返回 0 不推进时钟、`stepFrames` 跳过守卫单步推进（仅离线世界）；`SourceSnapshot`（entity/position/listenerPosition/distance/baseGain/effectiveGain/playing/audible/soloed/muted/clip/model）+ `snapshot()` + `setSourceEntity` + `bindToDebug`；`audio_cvars.cpp`（依赖 console 的唯一 TU：`audio.volume/debug/solo/freeze` 四 CVar + `audio.dump` 命令）；`renderer/src/audio_debug_ui.cpp` 独立 ImGui 窗口（默认关、`hud.cpp` 在 console 窗后调 drawAudioDebugWindow，**不动常驻 HUD**）；`audio-tools` 新增 `volume/solo/freeze/debug` 四子命令（format=3 provenance 记调试态 + waveformHash）+ selftest 13 条 M-S2 断言。

**提交版本**：feat/audio 本轮 commit（hash 见收口回复），annotated tag `audio-s2`

**完成情况**：
- **双后端 BUILD OK**：DX12 与 VULKAN 各自编译 + `audio-tools selftest` **46/46 PASS**（M-S0 21 + M-S1 12 + M-S2 13）；A 类纯 CPU 工具，不进渲染回归、不动 kStage。
- **volume 折叠**：masterVolume 0..2 经 `refreshAudibility` 折进 effectiveGain——x0 全零、x2 峰值翻倍、x0.5 峰值恰为一半（证据图 ratio=0.500000）。
- **solo**：solo(id) 后他源 effectiveGain=0；solo(1) 混音与单源渲染逐字节一致；provenance 逐源记录 effGain（src1=1 / src2=0）。
- **freeze 单步**：frozen 时 `readFrames` 返回 0（时钟保持）；`stepFrames` 跳过守卫推进 2048 帧且首块与未冻结世界逐字节一致；**solo 与 freeze 的 waveformHash 相同**（单步首块 == solo 混音），成为交叉验证锚点。
- **debug 快照**：`snapshot.tsv` / `debug.txt`（同 ImGui 面板数据源）逐源 entity/pos/dist/gain/state；clip 路径经 `portableWav` 归一化为仓库相对路径（不含 `..\..` 冗余段）。
- **防假绿 format=3 + 独立复算**：dumpDebugCore 写 `waveformHash=FNV-1a64(文件全字节)`；`example/audio-s2/verify-dump-hash.py`（python 复刻同参 FNV-1a64）独立复算——篡改波形样本（bad-volume/）与篡改存储哈希（bad-hash/）两条负向都被抓（exit 1），失败原因互异（波形改 / 哈希改）。
- **CVar 接线分离**：`audio_cvars.cpp` 依赖 console，`AudioWorld`/`AudioDebugController` 保持零 console 零图形依赖；viewer 已注册 `registerAudioConsole`，ImGui 面板仅 `audio.debug` 为真时开窗（默认关）。
- **亲眼可见证据（已肉眼核验）**：`example/audio-s2/volume.png`（x0.5 vs x1.0 双对照、ratio=0.500000）、`solo-freeze.png`（双波形 + hash 相同结论行）、`debug-panel.png`（两源快照表）、`acceptance.png`（十条命令真实 stdout、46/46 + 三负向齐全）——均由 `make-audio-evidence.py --s2` 从真实输出生成。
- `example/audio-s2/run.bat` 全链 exit 0（AUDIO M-S2 ACCEPTANCE: ALL GREEN）；`server.py --check` exit 0；展示台 `audio-s2` 卡 7 用例（usage 2 / selftest 0 / volume 0 / solo 0 / freeze 0 / debug 0 / 负向缺参 2）实跑全绿。
- **边界**：M-S2 不碰 ECS 场景集成（M-S3）；`core/audio/` 零图形符号；未 merge 回 master（轨道收口在 M-S3）。

**踩坑情况**：
1. **`std::vector` 漏 include**：`AudioWorld.hpp` 168 行 `snapshot()` 返回 `std::vector` 但头文件只 include 了 cstdint/memory/string——一个解析错误级联丢掉 `snapshot`/`impl_` 声明，报错点却落在别的 TU（`audio_cvars.cpp` 报「未定义类型」）。补 `#include <vector>` 一次修复。
2. **frozen 态调用 `render()` 基准被拦**：selftest freeze 断言「单步首块 == 正常首块」在 frozen=true 时调 `render()` 生成基准——`render()` 按设计尊重 controller（volume/solo 断言正依赖此），其内部 `readFrames` 被 frozen 守卫拦成 0 → 基准为空 → 对比恒 FAIL。修法：渲染基准前临时解冻、渲后恢复。实现无 bug，是测试基准的顺序错误。
3. **`dumpDebugCore` 参数类型不一致**：调用方传 `fs::path`、签名收 `std::string`，而函数体用 `outdir / "..."`（fs::path 语法）——统一为 `fs::path`。
4. **快照 clip 路径带 `..\..`**：run.bat 传绝对路径后 `snapshot.tsv` 的 clip 字段出现冗余段；`cmdDebug` 改用 `portableWav()` 归一化后再 createSource（provenance 已归一化、快照漏了）。
5. **VULKAN 构建树 cache 失效**：旧 cache `CMAKE_MAKE_PROGRAM=nmake`（裸名），build 报 generator 错误；显式 `-DCMAKE_MAKE_PROGRAM=<nmake 绝对路径>` 重 configure 后恢复。

**下一步**：M-S3 场景集成（**gate 在 P8 E1 / E1b 收口之后**，不随本轮解锁）；feat/audio 轨道在 M-S3 收口时全量合回 master。
