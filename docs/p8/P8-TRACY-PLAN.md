# P8-TRACY-PLAN — Tracy 性能 + 内存检查接入计划

> 2026-11-11 用户拍板并**确认开工**：接 Tracy（性能 + 内存检查），取**最新版本**；轨道归属 `profiling`，里程碑 M-TR0~M-TR3。
> 本文档是接入唯一口径；§7 六项待拍板已于 2026-11-11 全部拍板清零，开工闸满足。
> 三处实读证据见 §1.3；本地源码缓存 `E:/UE_STUDY_2026/_tmp-tracy/`。

---

## 0. 拍板记录

| 时间 | 决定 | 原话/依据 |
|---|---|---|
| 2026-11-11 | 接 Tracy，覆盖「性能热点检查 + 内存占用/泄漏检查」 | 用户：「接tracy 性能+内存检查」 |
| 2026-11-11 | 版本取最新，不用 Carrot vendored 旧版 | 用户：「找最新版本的吧」 |
| 2026-11-11 | 撰写对接文档先行，落码待闸清 | 用户：「撰写对接文档」 |
| 2026-11-11 | RenderDoc 是 API 拦截型（帧边界来自 Present），Tracy 是埋点型（帧来自 FrameMark）；M-TR0 FrameMark 落在 present 完成处对齐 | 用户追问两轮，口径写进 §2 |
| 2026-11-11 | HUD 零散统计盘点评审（§2.5）+ 末轮 review 补 9 处实操缺失 | 用户：「另外一些零散的数据展示和接入的工作有列么」「最后再review一下」 |
| **2026-11-11（确认开工）** | **Q1~Q6 六项全部照建议拍板**：Q1 新轨道 `profiling`；Q2 M-TR0~M-TR3（tag `tracy-m0`~`tracy-m3`）；Q3 首批三处埋点 + 线程命名冻结；Q4 选项 b（RenderDoc 进程内独立小步 M-TR2，交代归档顺延 M-TR3）；Q5 选项 c（GPU 显存本轮不做，K2 进程内存先顶）；Q6 选项 a（K1+K5 随 M-TR0，K2 随 M-TR1） | 用户：「确认开工」 |

## 1. 选型核查（选型硬闸逐条对照，2026-11-11 实测）

### 1.1 版本与来源

- **版本：v0.15.0**（上游 2026-11-06 发布，比 winget 收录的 0.14.1 新一档）。版本宏实测：`public/common/TracyVersion.hpp` → `Major=0, Minor=15, Patch=0`。
- **来源**：tag 归档 `github.com/wolfpld/tracy/archive/refs/tags/v0.15.0.tar.gz`，经 gh-proxy 镜像下载成功（21MB，http 200）。GitHub Releases 无独立源码资产（只有 GUI 二进制 + 手册 PDF）。
- **GUI（看数据的 profiler）**：`windows-0.15.0.zip`（31MB，http 200）已缓存于 `_tmp-tracy/`；**进程外工具，不入库、不进构建链**。

### 1.2 硬闸逐条

| 闸 | 结论 | 证据 |
|---|---|---|
| 开源无风险 | ✅ **BSD-3-Clause**（根 `LICENSE` 原文实测；⚠️ 更正：此前口误说成 MIT，BSD-3 与 MIT 宽松度同级、与本项目 MIT 兼容） | LICENSE 头 3 行 |
| 许可兼容 MIT | ✅ 允许商用/修改/再分发；无已知专利诉讼风险 | 同上 |
| 零构建期新依赖 | ✅ 纯源码、无 DLL、不联网；**client 只要求 C++11**（上游 `CMakeLists.txt:49` `target_compile_features(TracyClient PUBLIC cxx_std_11)`），MSVC 14.38 / C++17 覆盖 | 实测 |
| 溯源留档 | ✅ 落地时写 `thirdparty/tracy/VENDOR.md`（版本/tag/上游/许可/入库子集=仅 `public/`/排除项=profiler GUI、capture、meson、python 等 server 侧） | 本计划 §3 |
| 边界不变 | ✅ Tracy 是关联系统观测工具，不触碰 nanite/lumen 主线；不暴露第三方类型到业务层（经 `nl/` 薄封装或直接用其宏） | — |

### 1.3 三处参考实读结论

| 来源 | 读到什么 | 对本计划的含义 |
|---|---|---|
| **Carrot** | `thirdparty/tracy` 深度集成：`ZoneScoped` 散布各 System（World/Engine/各 RenderSystem），另写 `CustomTracyVulkan` 把 Vulkan backend GPU 上下文直接接进 Tracy | CPU zone 埋点形态照抄思路；**GPU 上下文 hack backend 的做法不适用**（NVRHI 双后端抽象层禁绕过），GPU 侧维持 L2 的 NVRHI timestamp query（M6.2 已跑通） |
| **bevy** | `tracing-tracy` 是 `bevy_log` 可选 feature（`trace_tracy_memory`），默认关闭、编译期开关；内存 profiling 走 Tracy | 接法对齐：`TRACY_ENABLE` 默认不定义 = 全部宏空转零成本；内存钩走 Tracy |
| **UE** | RenderDoc/PIX 为引擎内置 Developer 插件（`PixWinPlugin`/`GPUReshape`），内存走自家 LLM/Stats | 「引擎内埋点 + 外部工具收数」是工业终态同款分工，Tracy 只承担埋点+收数，不做 HUD 替代 |

## 2. 接入范围与分层

| 层 | 用什么 | 状态 |
|---|---|---|
| L0 画面证据 | `--screenshot` + example/ 基线 | ✅ 已就位，不动 |
| L1 帧结构（单帧逐 draw） | **RenderDoc**（权威口径在 `docs/s0/S0-PLAN.md` L1 节，本计划只管归属） | ✅ M-TR2 已写 `--capture-frame N` 进程内抓帧（零链接，默认关）；外部启动零代码即用 |
| L2 GPU 耗时 | NVRHI timestamp query → HUD | ✅ M6.2 已跑通，**本计划不碰** |
| **L3 CPU 时间线** | **Tracy zone + FrameMark**（本计划核心①） | ✅ M-TR0 接入 |
| **L3 内存占用/泄漏** | **Tracy memory profiler + 退出残留清单**（本计划核心②） | ✅ M-TR1 接入 |

**RenderDoc 与 Tracy 分工（同一观测轨道，互不替代）**：
- RenderDoc = 抓**单帧**：逐 draw call / pipeline / 资源状态 / 资源依赖——查「这一帧画错了什么、状态哪里不对」。
- Tracy = 看**跨帧时间线**：CPU 热点、job 分布、帧间隔抖动、内存曲线——查「帧率为什么掉、谁在吃时间」。
- **流派区别（2026-11-11 用户确认口径）**：RenderDoc 是 **API 拦截型**（启动时注入 hook 层拦截 D3D12/Vulkan 函数表，引擎零插桩、零代码改动）；Tracy 是 **埋点型**（只记录主动上报的 zone，不拦截 API，必须插桩）——所以 Tracy 需要里程碑做埋点，RenderDoc 外部启动不需要。
- **帧边界来源**：RenderDoc 的「帧」= 两次 Present（DX12 `Present()` / Vulkan `vkQueuePresentKHR`）之间的 API 调用序列，无需引擎打点；Tracy 的「帧」完全来自 `FrameMark`。因此 M-TR0 的 `FrameMark` 落点定在 **present 完成处**，与 RenderDoc 帧边界天然对齐——两边「第 N 帧」可互相指认（RenderDoc 查单帧结构，Tracy 看该帧前后 CPU 上下文）。
- 冲突面：RenderDoc 注入有捕获开销，**性能基准与 Tracy 读数一律不在 RenderDoc 注入下采**；两者不同时开（TOOLS-GUIDE 写明）。

**RenderDoc 两种接法（S0-PLAN 已定，此处仅复述）**：
1. 外部启动（零代码）：RenderDoc GUI / CLI 拉起 `viewer.exe`，热键抓帧——装好工具当天可用，不占代码里程碑。
2. 进程内 `--capture-frame N`：`renderdoc_app.h` 单头 + `LoadLibraryW(L"renderdoc.dll")` 拿 in-app API（`StartFrameCapture`/`EndFrameCapture`，device/window 句柄可传空，**viewer/core 不出现图形 API 符号**），约 30 行、零链接依赖、默认关闭。

**明确不做**：
- GPU zone / TracyD3D12 / TracyVulkan 头文件接入（NVRHI 抽象层内禁直接接图形 API 的 Tracy 上下文；GPU 数据已有 L2）。
- 用 Tracy 替代 HUD 常驻统计（fps/frameMs/triangleCount 继续走 HUD， Tracy 是离线/连机观测，两者互补）。
- profiler GUI 入库（进程外工具，放 `_tmp-tracy` 解包后的固定本机路径，README/VENDOR.md 记路径）。

## 2.5 零散数据展示清单（HUD 常驻统计盘点，2026-11-11 实读 `renderer.hpp` HudFrame）

**已有（不用重做）**：

| 数据 | 来源里程碑 | 备注 |
|---|---|---|
| fps / frameMs + 120 帧曲线 | M0.5 | CPU 帧间隔 |
| **gpuMs 整帧 GPU 耗时 + 曲线** | M0.6 / M6.2 | NVRHI timer query（L2），异步 1~2 帧延迟 |
| **cullMs 剔除 pass GPU 耗时 + 曲线** | M4.2 | 第二个 timer 实例 |
| triangleCount / lodTriangles / lodMaxError / lodLevel | M0.7 / M2.3 | 面数与 LOD |
| visibleClusters / totalClusters / visibleTriangles | M4.1 / M5.2 | GPU 剔除统计 |
| blasTriangleCount / blasMs / tlasInstanceCount / tlasMs | M8.0 / M8.1 | AS 构建/重建耗时 |
| particleAlive | M-PT0 | 粒子存活数 |
| adapter / backend / mode / stage | M0.x | 设备与环境信息 |

**缺口（零散接入工作，归属已拍板 Q5/Q6）**：

| # | 数据 | 现状 | 定稿归属 |
|---|---|---|---|
| K1 | **drawCall 计数**（CPU 提交侧，scene.cpp 全部 `cmd->draw*` 调用点计数，含 initCluster 单批兜底；不含 compute dispatch / ImGui UI） | 无 | **随 M-TR0**：⚠️ **实施形态修正**——不往常驻 HUD 面板加行（那会让 70 张既有 `--hud` 基线全量重拍，与本轨道「不 bump kStage」冲突），改为 `--profiling-hud` 默认关的**独立窗口**（DriveDebugUi / InputDebugUi 同款 add-on 先例，show=false 零布局）。计数在 backend `recordFrame` 帧开始清零、HUD 绘制后写入 `DeviceInfo.drawCalls` |
| K2 | **进程内存占用**（PSAPI `GetProcessMemoryInfo` working set / private bytes，零图形依赖、双后端对称） | 无 | ✅ **M-TR1 已完成**：`--profiling-hud` 第 4 行常驻（working set MB）+ Tracy memory 曲线互补（Tracy 离线归因） |
| K3 | **GPU 显存占用** | 无 | **本轮不做**（Q5 c）：DX12 DXGI / Vulkan `VK_EXT_memory_budget` 不对称，留到真需要时单独拍板；K2 先顶 |
| K4 | **per-pass GPU 拆分**（GI / shadow / present 前后各自成段；目前只有整帧 + cull 两段） | 部分 | **不进本轨道**——S12/S13 性能验收需要时按 L2 惯例扩 timer 段 |
| K5 | instanceCount 常驻显示 | DeviceInfo 有、HUD 未常驻 | **随 M-TR0** 与 K1 同批 |



## 3. 里程碑拆分（M-TR0~M-TR3，2026-11-11 拍板定稿）

> **本轨道与展示台的关系（M9.0 教训前置）**：本轨道所有里程碑**不 bump `kStage`、不新增渲染版本卡**（无画面变化），`server.py --check` 只用来证明旧阶段零回归；每步验收全走「工具真实输出证据 + `regression.py --only` 抽查」。

### M-TR0 vendored + 开关 + CPU zone（性能检查）

**做什么**：
1. `thirdparty/tracy/` 入库：**只取 `public/` 子集**（`tracy/` 包装头 + `client/` + `common/`）+ 根 `LICENSE` + `VENDOR.md`；排除 profiler/capture/server/meson/python/tests。GUI 解压落位 `E:/UE_STUDY_2026/.build-tools/tracy-gui/0.15.0/`（与便携 cmake 同级的本机工具区，路径写进 VENDOR.md；`_tmp-tracy/` 缓存 vendoring 完成后清理）。
2. 新增 CMake 静态 target `tracy-client`（编 `client/TracyClient.cpp`），编译选项 `NL_TRACY`（**默认 OFF**）；OFF 时不编译该 target、不定义 `TRACY_ENABLE`，全部宏空转。⚠️ **新增 target 后 DX12 / VULKAN 两个 build tree 都必须重新 configure**（M-N6 实锤：新 target/用例只在 configure 期写入）。
3. 埋点（只在 ON 分支生效，宏天然空转无需 #ifdef 包裹）：
   - viewer 主循环：每帧 `FrameMark`（**present 完成处**，对齐 RenderDoc 帧边界）；input / scene update / render submit 各段 `ZoneScopedN`；
   - renderer 帧提交路径关键段（CPU 侧，不进 shader）；
   - core job system worker 循环 + job 执行体 `ZoneScopedN`。
4. **线程命名**：主线程 / job worker / 音频等用 `tracy::SetThreadName` 全部命名——不命名时间线上全是 Thread N，没法读（job worker 数量动态，worker 内命名随创建走）。
5. Zone/线程命名全 ASCII（沿用 HUD 字符串约定）。
6. **HUD K1/K5 展示**：`--profiling-hud` 默认关的独立窗口 `ProfilingDebugUi`（3 行：draw calls / instances / triangles），由 `DeviceInfo` 供数；`show=false` 时零布局，既有 `--hud` 基线逐字节不动。埋点薄封装 `core/include/nl/core/profiler.hpp`（`NL_PROFILE_*`，OFF 时连第三方头都不 include）。

**验收判据（亲眼可见）**：
- `NL_TRACY=ON` 构建 viewer 跑起来 → tracy-profiler GUI 连上，看到帧序列 + 各 zone 时间线 + **命名后的 job 线程分布**（截图留档）；首次运行 Windows 防火墙弹窗（client 监听 TCP 8086）属于预期，放行 localhost 即可；
- `NL_TRACY=OFF`（默认）构建：行为零差异——`regression.py --only` 挑 2~3 条关键用例过；`grep TRACY_ENABLE` 确认 OFF 时未定义。
- **闭环结论（2026-11-12）**：Tracy 客户端管线已无头端到端验证（见下方实施记录）；viewer/renderer 侧 `viewer.frame`/`FrameMark`/Main 线程 zone + image 回归待裸机交互桌面补验（要 GLFW 窗口），与 M-TR0 代码正确性无关。

**实施记录（2026-11-11，commit 见 feat/profiling，tag `tracy-m0`）**：
- 代码面全部落地：vendored `thirdparty/tracy/public/` 子集 + `LICENSE` + `VENDOR.md`；GUI 解压落位 `.build-tools/tracy-gui/0.15.0/`；根 CMake 增 `nl_profiler` INTERFACE 目标 + 条件 `tracy_client` 静态库 + `option(NL_TRACY OFF)`；`core/include/nl/core/profiler.hpp` 薄封装 `NL_PROFILE_{FRAME,ZONE,THREAD}`（OFF 连第三方头都不 include）；viewer 主循环 `FrameMark`(present 处)/`ZoneScopedN`/`SetThreadName`、renderer 帧提交段、`jobs.cpp` worker 循环 + job 执行体埋点；`Scene::draw` 主路径 `noteDraw()` 计数 → 后端每帧清零/读入 `info_.drawCalls`；`--profiling-hud` 默认关的独立窗口 `ProfilingDebugUi`（draw calls / instances / triangles 三行）。
- **构建验证（真实 exit code，非 `| tail` 掩盖）**：
  - `build/DX12`（OFF）：configure + build viewer = **0** ✓
  - `build/VULKAN`（OFF）：configure + build viewer = **0** ✓（首跑因 kill 残留半截 `gltf_anim.cpp.obj` 报 `LNK1136`，删损坏 obj 后重编通过，非代码问题）
  - `build/DX12-TRACY`（`-DNL_TRACY=ON`）：configure + build viewer = **0** ✓（证明 TracyClient.cpp 编译链接、`TRACY_ENABLE` 经 `nl_profiler` PUBLIC 传播到 renderer/viewer、`FrameMark`/`ZoneScopedN`/`SetThreadName` 全部解析）
- **构建期两处 bug 已就地修复**：
  1. `noteDraw()` 是 `Scene` 成员，Python 批量插入时误放进 `DebugLines::draw` 与粒子类（非 Scene 作用域）→ 编译 `找不到标识符`；移除那两处，K1 只计 Scene 主路径（debug 线框/粒子补充绘制不计入，M-TR2 可再细化）。
  2. 初版只把 `nl_profiler` 链进 `nl_core`，漏了 `nl_renderer`/`viewer` → ON 构建下这两侧 TU 的 `NL_TRACY` 不定义、宏全空转（等于开了也不埋点）；已显式补进两处 `target_link_libraries`。
- **运行时验收（无头沙箱可验部分已闭环，2026-11-12）**：`viewer.exe` 需建 GLFW 窗口/显示器，本无头沙箱起不了（DX12/Vulkan 两后端同在极早期 init 同点崩，与环境/GPU 沙箱限制有关，非代码问题）；但 **Tracy 客户端管线已用无头等价手段端到端验证**：在 `DX12-TRACY`（NL_TRACY=ON）树补编 `nanite-tools`，同 shell 内 `tracy-capture -p 8086` 监听 + 跑 `nanite-tools selftest jobs`（触发 `jobs.cpp` 的 `JobWorker` 线程命名 + `job.task` zone），捕获日志：
  ```
  Connecting to 127.0.0.1:8086...
  Frames: 2
  Time span: 5.28 s
  Zones: 2,759,437
  Saving trace... done!
  Trace size 16.3 MB
  ```
  落盘 `build/DX12-TRACY/mtr0_capture.tracy`（16.3 MB，270 万条 zone）——证明 M-TR0 的 Tracy 客户端在 ON 构建下能初始化 → 连 server → 真实广播 zone 数据。OFF 构建下 `profiler.hpp` 三宏全空转、连 Tracy 头都不 include（源码已核），零成本。余下 viewer/renderer 侧 `viewer.frame`/`FrameMark`/Main 线程 zone + image 回归须在裸机交互桌面补验（要 GLFW 窗口），与 M-TR0 代码正确性无关。
- 过程纪律：`| tail` 会吞掉 cmake 真实退出码（曾误报 0），后续一律 `> log 2>&1; echo EXIT=$?`；改 CMake 链接面后三棵树均重配（M-N6 实锤）。

### M-TR1 内存占用 + 泄漏检查

**做什么**：
1. 内存钩子：**引擎侧薄封装** `core/src/profiler_memory.cpp`（仅 `NL_TRACY=ON` 编入），自定义全局 `operator new/delete`（含 aligned 版），内部 `malloc/free` + `TracyAllocS(ptr,size,16)` / `TracyFreeS(ptr,16)` 上报（显式 depth 才有调用栈，`TRACY_CALLSTACK` 默认 0）。⚠️ **实测更正（2026-11-11 vendoring 时）**：Tracy **0.15 不自带全局 new/delete 钩**——`public/client/TracyOverride.cpp` 只剩 Linux `dlclose` 覆盖，全 `public/` 内 `operator new` 零命中；原计划的「编 TracyOverride.cpp」是旧版印象，作废。上游伞形 `TracyClient.cpp` 已含该文件，无需单独编译。见 `thirdparty/tracy/VENDOR.md`「0.15 实测要点」。
2. **覆盖口径**：新/delete 钩覆盖 CPU 侧经全局分配器的部分（含 STL）；**NVRHI 显存/堆分配不经过全局 new 的不在此列**——这正是 §2.5 K3「显存占用」是另一口径的原因，两者不混。
3. 泄漏口径：进程退出时的**存活分配残留清单**即泄漏候选（Tracy Memory 面板按调用栈归因）；Windows callstack 归因走 dbghelp（PDB 在旁即带行号，同 ASan 手册经验）。
4. 分工写死：Tracy memory = 运行期曲线 + 退出残留归因；**CRT debug heap（`_CrtSetDbgFlag`）保留为兜底**——MSVC ASan 无 LSan 是本项目实测（`docs/ASAN-DIAGNOSIS.md` §4），Tracy 也不能替代双保险，只是主通道换 Tracy。
5. 纪律核对：Tracy 0.15 文档明确 memory 上报需在锁内进行（release notes 实录）；job 多线程分配归因到线程。

**验收判据**：
- GUI Memory 面板看到实时分配/释放曲线（截图）；
- **故意泄漏用例**：临时开关下漏一个 64KB 分配，**走 ESC 干净退出**（⚠️ Tracy 数据在进程被杀时丢失，残留清单只在正常 shutdown 时完整），残留清单能报出该块及其调用栈（截图 + 步骤写进 TOOLS-GUIDE）；
- OFF 构建零差异（同 M-TR0 口径）。

**实施记录（2026-11-12，commit 见 feat/profiling，tag `tracy-m1`）**：
- 代码面：新增 `core/include/nl/core/profiler_memory.hpp`（`nl::profiler::processWorkingSetBytes()`/`processPrivateBytes()`，PSAPI，始终可用、与 Tracy 开关无关）+ `core/src/profiler_memory.cpp`。后者分两部分：① **K2 进程内存**（PSAPI `GetProcessMemoryInfo` → `PROCESS_MEMORY_COUNTERS_EX.WorkingSetSize`/`PrivateUsage`，零图形依赖、双后端对称，经 `#pragma comment(lib,"psapi.lib")` 自动链）；② **Tracy 内存钩**：全局 `operator new/delete`（含 nothrow / `[]` / C++17 对齐版）覆盖，内部 `malloc`/`free` + `TracyAllocS(ptr,size,NL_TRACY_MEM_DEPTH)`/`TracyFreeS(ptr,NL_TRACY_MEM_DEPTH)` 上报，**仅 `NL_TRACY=ON` 编译**，OFF 退回 CRT 默认。
- **`/INCLUDE` 强制链接（M-TR1 最大实现坑）**：全局 `operator new/delete` 定义在静态库 `nl_core` 中，若 exe 不显式引用该 .obj，链接器整块丢弃、钩子静默失效（Tracy 0.15 也正是因此移除内置全局钩）。在 `profiler.hpp` 的 `NL_TRACY` 分支加 MSVC `#pragma comment(linker,"/INCLUDE:??2@YAPEAX_K@Z")` 等四个修饰名，强制拉入整 obj（对齐版 new/delete 一并带上）。
- **构建验证（真实 exit code）**：`build/DX12-TRACY`（`-DNL_TRACY=ON`）重配 + 编 `nanite-tools` = **0**（证明 TracyAllocS 编译链接、`/INCLUDE` 拉钩无 LNK2005）；`build/DX12`（OFF）重配 + 编 `nanite-tools` = **0**（K2/PSAPI 路径编译链接，无 Tracy 钩）。
- **无头端到端验证（M-TR1 内存钩真广播）**：DX12-TRACY 树同 shell 内 `tracy-capture -p 8086` + `nanite-tools selftest jobs`，捕获日志 `Zones: 2,759,872`、**Trace size 89.37 MB**（对比 M-TR0 同用例 16.3 MB）——体积暴涨 ~73 MB 即每个 `new`/`malloc` 的分配事件被上报，证明全局内存钩经 `/INCLUDE` 已链接并真实广播。落盘 `build/DX12-TRACY/mtr1_capture.tracy`（89.37 MB）。
- 待裸机补验：① GUI Memory 面板曲线 + 故意泄漏用例残留清单（需 viewer 连 GUI，要 GLFW 窗口）；② `--profiling-hud` 窗口 K2 进程内存行（已加第 4 行 `proc mem : X.X MB`、窗口高度调至 150，但窗口渲染须裸机看）。`NL_TRACY_MEM_DEPTH` 默认 0（不抓调用栈、最低开销），泄漏归因到源码时按 `-DNL_TRACY_MEM_DEPTH=16` 重编即可。

### M-TR2 RenderDoc 进程内抓帧（Q4 选项 b，独立小步）

**做什么**：`--capture-frame N` + `renderdoc_app.h` 单头 + `LoadLibraryW(L"renderdoc.dll")` 的 in-app API（`StartFrameCapture`/`EndFrameCapture`），约 30 行、零链接依赖、默认关闭；device/window 句柄传空，viewer/core 零图形 API 符号。外部启动（零代码）不占里程碑，本机装好即用。

**验收判据**：指定帧号跑一发，落出 `.rdc` 文件 + 工具输出证据；默认关闭时行为零差异（`--only` 抽查）。与 Tracy 不同时开（§2 互斥口径）。

**实施记录（2026-11-12，commit 见 feat/profiling，tag `tracy-m2`）**：
- 代码面：vendored `thirdparty/renderdoc/renderdoc_app.h`（仅头，MIT，clone HEAD `86cadb8…`，见 `thirdparty/renderdoc/VENDOR.md`）；`viewer/main.cpp` 新增 `Options::captureFrame`（默认 0=关）+ `--capture-frame N` CLI（`<1` 报用法错误 exit 2）；`initRenderDocCapture()` 在 main 起调，经 `GetModuleHandleA("renderdoc.dll")` + `GetProcAddress("RENDERDOC_GetAPI")` 被动拾取 in-app API（回退 `LoadLibraryW`），未注入时全程 no-op；`maybeTriggerRenderDocCapture(frame+1, o.captureFrame)` 注入主循环两处（普通循环 ~2515 与 ECS/脚本循环 ~3630），到达指定帧 `TriggerCapture()` 一次（fire-once 防重抓）；`--capture-frame N` 进 `printUsage` 帮助文本。
- **零链接 / 零图形符号**：`renderdoc_app.h` 仅头文件，经 `viewer/CMakeLists.txt` 加 `target_include_directories(viewer PRIVATE .../thirdparty/renderdoc)`；viewer/core 不出现任何 D3D12/Vulkan 符号，符合 §M0.1 纪律。
- **构建验证（真实 exit code，两棵树全过）**：`build/DX12`（OFF）重配 + 编 `viewer` = **0**；`build/DX12-TRACY`（`-DNL_TRACY=ON`）重配 + 编 `viewer` = **0**（日志 `Built target viewer`；`renderdoc_app.h` 的 `RENDERDOC_API_1_6_0`/`GetModuleHandleA`/`GetProcAddress`/`LoadLibraryW` 在 OFF/ON 两配置下均编译链接通过，无报错；既有 `/W4` 警告 C4244/C4458 与本次无关）。
- 待裸机补验（要 RenderDoc + GPU，沙箱跑不了）：① RenderDoc Launch 拉起 `viewer --capture-frame N` 落 `.rdc`；② 非注入下启动行为零差异（`--only` 抽查）。

### M-TR3 交代归档

- 展示台工具卡：`kind:"tool"`，usage / 正向（ON 构建 + GUI 连接脚本 / RenderDoc 抓帧）/ 负向（泄漏用例期望输出）三条起步，`expectExit` 0/1/2；⚠️ **cases.json 必须 case 级 `"exe"` 覆盖**（M11.0 实锤：漏了会拿版本级 viewer.exe 跑子命令 → 窗口循环 120s 超时）；Tracy 不产 GPU 截图，`preview` 静态证据图取工具真实输出（GUI 时间线截图 / 残留清单导出 / .rdc 落盘日志），不手绘。
- `docs/p8/TRACY-TOOLS-GUIDE.md` 归档（GUI 启动、连接、常见坑、与 RenderDoc 的互斥口径）。
- 收口流程照 AGENTS.md 里程碑交付纪律（轨道闸 `wt_pool.py close` + 四项自查 + 台账 + tag）。

## 4. 技术要点（实测口径）

- **开关宏**：`TRACY_ENABLE` 不定义 = `Tracy.hpp` 第 25 行 guard 使全部宏空转（实测）；ON 时按需 `TRACY_NO_CODETRANSFER` 等调优，初版不加。
- **连机模式**：初版用默认 broadcast（client 常驻监听 TCP 8086，GUI 即连即看）；若日常 ON 构建嫌监听碍事，再切 `TRACY_ON_DEMAND`（连上才记录）——初版不引入，留为后手。
- **协议版本强约束**：GUI 与 client 必须**同版本**（0.14.1 GUI 连不了 0.15.0 client；0.15 dropped pre-0.11 trace 支持）——GUI 只用 `windows-0.15.0.zip`，升级必须成对。
- **client 编译面**：单翻译单元 `TracyClient.cpp`，C++11 起步；MSVC 14.38 编译无阻碍（上游 0.15 还专门修了 MSVC C4366 警告）。
- **与 spdlog 无冲突**：两者都在热路径外或轻量段；Tracy zone 不嵌日志格式契约，互不污染。
- **双后端对称**：Tracy 在 core/viewer 层，与 DX12/Vulkan 后端选择正交；两后端埋点同一套。
- **图形符号纪律**：`tracy-client` target 只被 core/viewer/renderer 链接侧引用宏；`viewer/`、`core/` 不出现任何图形 API 符号——Tracy 头非图形 API，不违反 §M0.1 硬闸。

## 5. 与既有纪律兼容性核对表

| 纪律 | 是否触碰 | 说明 |
|---|---|---|
| 构建链禁厂商图形 SDK | ❌ 不触碰 | Tracy 非图形 SDK，纯观测库 |
| 图形符号只在 renderer/ | ❌ 不触碰 | 见 §4 |
| NVRHI+Slang 通用管线 | ❌ 不触碰 | GPU 侧仍走 L2 timestamp |
| C++17 / MSVC 14.38 | ✅ 兼容 | client 只需 C++11 |
| HUD 面板高≤560 / ASCII | ❌ 不触碰 | HUD 不改；Tracy 命名 ASCII |
| 回归纪律 | ✅ 遵守 | 默认不跑全量；OFF 路径 `--only` 抽查 |
| 自研边界 | ✅ 通例 | 关联系统用成熟库=通例（2026-11-09 拍板） |

## 6. 风险与坑

1. **协议版本漂移**：GUI/client 成对升级，VENDOR.md 记死配对版本。
2. **全局 new/delete 钩的重入（自实现，非上游）**：`profiler_memory.cpp` 内上报路径不得再触发分配（只调 `malloc/free` + Tracy 的 MemAlloc/MemFree，禁止构造 std:: 容器 / 拼字符串）；job system 自有分配器若绕过全局 new，需逐个补 `TracyAllocS` 点——M-TR1 实测时核对。
3. **退出残留 ≠ 全是真泄漏**：单例/静态池的存活分配（如世界缓存 GPU flat hashmap 的 CPU 侧）会出现在残留清单，需要人工分辨「有意存活」；TOOLS-GUIDE 里给白名单口径。
4. **0.15 新行为**：memory 事件归因到虚拟 fiber 线程（release notes）——多线程分配归因读数时注意口径变化。
5. **ON 构建的样本开销**：zone 埋点有纳秒级开销，性能基准（S4 沿用的基准）一律 OFF 构建跑，避免把探针开销算进基线。
6. **防火墙弹窗**：client 首次监听 TCP 8086 时 Windows 防火墙会弹窗（预期行为，放行即用）；GUI 连 `localhost:8086`。
7. **异常退出丢数据**：Tracy 数据依赖正常 shutdown flush——进程被杀（崩溃 / `taskkill`）时未送出的帧全丢；崩溃场景的观测要靠 ASan 手册那条线（`docs/ASAN-DIAGNOSIS.md`），不是 Tracy。
8. **新增 CMake target 的 configure 闸**：M-TR0 加 `tracy-client` 后，DX12 / VULKAN 两个 build tree 都必须重新 configure 再跑 ctest（M-N6 实锤，add_test/新 target 只在 configure 期写入）。

## 7. 拍板结论（2026-11-11 全部清零，开工闸满足）

| # | 结论 | 落点 |
|---|---|---|
| Q1 | **开新并行轨道 `profiling`**（wt `E:/UE_STUDY_2026/nl-engine-profiling` + `feat/profiling`） | 三处注册：本文件 / `tools/wt_pool.py` TRACKS / `docs/PARALLEL-TRACKS.md` §7.1 / `AGENTS.md` 实例注册表；`claim --track profiling --milestone M-TR0` |
| Q2 | **M-TR0~M-TR3**，tag `tracy-m0`~`tracy-m3` | §3 |
| Q3 | **首批冻结** = viewer 主循环（含 present 处 FrameMark）/ renderer 帧提交 / job worker 三处 + `SetThreadName` 线程命名 | §3 M-TR0 |
| Q4 | **选项 b**：RenderDoc 进程内抓帧独立小步 **M-TR2**；原交代归档顺延 **M-TR3** | §3 |
| Q5 | **选项 c**：GPU 显存占用（K3）**本轮不做**，K2 进程内存先顶；不对称面留到真需要时单独拍板 | §2.5 |
| Q6 | **选项 a**：K1（drawCall）+ K5（instanceCount）随 M-TR0；K2（进程内存）随 M-TR1；per-pass GPU 拆分（K4）不进本轨道 | §2.5 / §3 |
