# nl-engine 开发规范与踩坑汇总（DEV-PITFALLS）

> 配套纪律见仓库根 `AGENTS.md`（决策前置铁律 / 里程碑交付纪律 / 代码开发与双后端纪律）。本文件是**构建、编译、运行期高频问题的集中速查**，内容均来自 `CMakeLists.txt` 注释、`scripts/build-m0.2.bat`、`docs/MILESTONES.md` 踩坑栏、`docs/SHADER-CONVENTIONS.md` 的实测记录。
>
> 凡本文件与 `AGENTS.md` 冲突，以 `AGENTS.md` 为准。

---

## 1. 构建系统规范（CMake）

- **后端对称、零厂商 SDK**：`NL_GRAPHICS_BACKEND`（`AUTO|DX12|VULKAN`）编译期选定；Windows 下 `AUTO` 落 `DX12`。`CMakeLists.txt` **不得**出现 `VULKAN_SDK` / `find_package(Vulkan)` / 要求装 Vulkan SDK。D3D12 头库来自 Windows SDK，Vulkan 头走 vendored `thirdparty/vulkan-headers`，loader 是驱动自带 `vulkan-1.dll`，Slang 独立获取。
- **NVRHI 只编译当前后端**：`NVRHI_WITH_VULKAN` / `NVRHI_WITH_DX12` 单开，`NVRHI_FETCH_VULKAN_HEADERS=OFF`、`NVRHI_FETCH_DIRECTX_HEADERS=OFF`（不联网）。
- **Vulkan 静态链接必踩的坑**：静态链 `nvrhi_vk` 时 NVRHI 不会自动生成 Vulkan 动态派发器存储与 init，必须显式 `target_compile_definitions(nvrhi_vk PRIVATE NVRHI_SHARED_LIBRARY_BUILD=1)`，**否则链接期存储未定义、运行期 dispatcher 未初始化直接崩**；DX12 后端不受影响（`CMakeLists.txt:75-80`）。
- **Vulkan 不链 `vulkan-1.lib`**：靠 loader 运行时解析函数指针（本机无静态导出符号可链）；`renderer` 自己也要吃 `vulkan.h`，故 CMake 给 renderer 加 `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1` + 私有 include（`CMakeLists.txt:152-160`）。
- **DXIL 依赖 Windows SDK 的 `dxcompiler.dll`/`dxil.dll`**：CMake 用 `file(GLOB …/Windows Kits/10/bin/*/x64/dxcompiler.dll)` 取版本最高者，**不下载**。找不到 `dxcompiler.dll` 直接 `FATAL_ERROR`。
- **Slang 独立获取**：`slangc.exe` 必须在 `thirdparty/slang/dist/bin`，否则 `FATAL_ERROR`。
- **shader 自动收集**：`file(GLOB renderer/shaders/*.slang)` 自动列出源，每个 `<name>.slang` 出 `vs/ps/vscluster/pscluster` × `spv/dxil` 共 8 产物。**新增 `.slang` 后若没触发重编，手动重跑一次 build 脚本**（GLOB 在 configure 时求值）。
- **符号分层（已在 CMake 锁死）**：图形 API / NVRHI / ImGui 符号只允许活在 `renderer/`；`core/` 与 `viewer/` 零命中。`nl_core` 不链任何图形库；`viewer` 只链 `nl_core + nl_renderer + glfw`，**不调用 `groupClusters` → 链接器不把 `grouping.obj` 拉进 viewer → 无 METIS 负担**。
- **编译选项**：C++17；MSVC 下 `/W4 /permissive- /utf-8 /bigobj`，`/wd4819` 压 NVRHI 上游非当前代码页字符噪音（不影响代码）。

---

## 2. 本机构建环境（2026-11-06 实测）

- 工具链：VS2022 + MSVC **14.38** + Windows SDK **10.0.26100.0**；CMake **3.31.6 便携版**在 `E:\UE_STUDY_2026\.build-tools\`。
- **必须用 `NMake Makefiles` 生成器**：VS 生成器走 MSBuild，会撞 `HTTPS_PROXY` 与 `https_proxy` 同时存在的大小写不敏感 Hashtable 冲突而失败。这是 M0.0 踩坑 #6，别换回 VS 生成器。
- **不用 `vcvars64.bat`**：本机它内部调 `reg.exe` 定位 Windows SDK，而 `reg.exe` 被沙箱策略拦；三个环境变量（`INCLUDE`/`LIB`/`PATH`）在 `scripts/build-m0.2.bat` 里显式写死。
- **双后端分目录增量**：`build/DX12` 与 `build/VULKAN` 各自复用，`thirdparty` 保持增量；切后端 = 换目录，不重拉第三方。
- **标准构建命令（范式见 `scripts/build-m0.2.bat`）**：
  ```
  set CMAKE_BIN=E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe
  "%CMAKE_BIN%" -S . -B build\DX12   -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=DX12
  "%CMAKE_BIN%" --build build\DX12
  "%CMAKE_BIN%" -S . -B build\VULKAN -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=VULKAN
  "%CMAKE_BIN%" --build build\VULKAN
  ```

---

## 3. 常见编译 / 链接 / 运行错（含 139 段错误）

| 现象 | 根因 | 处理 / 预防 |
|---|---|---|
| **进程退出码 139（段错误）** | NVRHI 析构顺序错：`shutdown()` 先销毁原生对象，NVRHI 再在已销毁的 device 上 `Reset` → use-after-free。 | 先 `nvrhiDeviceHandle_.Reset()` 让 NVRHI 用活着的 device 清内部对象，再销毁原生对象（MILESTONES:122；`backend_vulkan.cpp:733/751`）。 |
| **首帧 139 段错误** | 新增 GPU 资源时 slot 级同步表没在 `createTargets()` 初始化；`renderFrame` 在 `ensurePipeline` 之前就按 slot 访问 → 空指针。 | 任何新 GPU 资源的 slot 同步表必须放进 `createTargets()` 初始化（MEMORY 渲染约定）。 |
| **三角形全黑、调试层零报错** | SPIR-V 入口名被设成 `vsMain`。NVRHI SPIR-V 恒以 `main` 为入口，`setEntryName("vsMain")` 找不到 → `createShader` **静默返回 NULL** → pipeline 建不出。 | SPIR-V 入口恒 `main`；DXIL 才 `vsMain/psMain`（SHADER-CONVENTIONS §2.1；MILESTONES:15/185）。 |
| **顶点错乱 / 黑屏，能编译** | shader 顶点输入布局与 backend C++ 的 `VertexAttributeDesc`（slot/格式/步长）不一致。 | 改任一侧必须同步另一侧（SHADER-CONVENTIONS:37）。 |
| **`SceneConstants` 内存错乱** | 整块 `memcpy` 把尾部 `bool cullBack` 等字段冲掉。 | `SceneConstants` 严格 **160 B**，**禁止整块 memcpy**（MEMORY 渲染约定）。 |
| **NMake 显示 `Built target` 却没重编、二进制还是旧的** | 手动 `cp` 把另一后端的 exe/产物盖过来，时间戳更新 → NMake 只看时间戳判定最新。 | **覆盖产物必须删掉再编**；backend 自证看日志 `shader=spv/dxil` 与 `backend: vulkan/dx12`（`CMakeLists` 注入宏；MILESTONES:644）。 |
| **`slangc -dxc-path` 在 Git Bash 里碎** | Bash 对带空格路径再分词/转义，`dxcompiler.dll` 路径被切碎 → `failed to load downstream compiler 'dxc'`。 | 手工验证时用单引号反斜杠 Windows 路径直传；CMake 构建内不受影响（MILESTONES:395）。 |
| **工具卡 `select` 空选项展开成空参数** | cases.json 空选项被展平成空 arg，工具侧没跳过。 | 工具侧遇到空参数跳过；server/regression 同一套 `expectExit` 语义（MILESTONES:468）。 |
| **展示台新实例绑定静默失败** | 8787 端口被旧会话残留进程占着。 | 验证前先 `netstat -ano | grep 8787` 查残留，`taskkill` 清掉再起（MILESTONES:464）。 |

> **139 是本项目最常见的运行期崩溃信号**，多半是「析构顺序 / 资源 slot 未初始化 / 覆盖产物没删」三类。定位时优先看 NVRHI 是否在活 device 上析构、首帧是否访问了未建资源。

---

## 4. Shader 与双后端一致性（速查，详版见 `SHADER-CONVENTIONS.md`）

- **一份 `.slang` 双目标（SPIR-V + DXIL）两后端都能加载出正确画面** = 通用管线验收核心，不是「顺带试一次」。
- SPIR-V 编译必须带 `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；shader 只写 `register(b0/t0/s0)`，**禁 `[[vk::binding]]`**。
- 三角形 ID 用 `SV_PrimitiveID`（PS 直接收、VS 不用传）；颜色用整数 hash（pcg 风格），无浮点随机源 → 同参重渲染 PNG 确定，可进截图回归。
- **双后端画面一致必须分档**：`bytes`（MD5 逐字节）/ `near`（≤1/255 且 ≤2 px）/ `FAIL`，**禁止只写「一致」**（AGENTS.md「代码开发与双后端纪律」）。
- 改 shader → 重编字节码自动触发，无需手跑 slangc。

---

## 5. 离线工具与资产规范

- **`nanite-tools` 只链 `nl_core + meshoptimizer`**（meshopt 仅作 `simplify` 的对照 oracle；`core` 自身 QEM 是正式实现），零图形 API，不依赖 `renderer` / `viewer`。
- **工具 exe 恒指当前构建**：`build/{dx12,vulkan}/nanite-tools.exe` 两份都要用**本后端**重编，**别 `cp` 盖**（NMake 时间戳陷阱，见 §3）。
- **`.nmesh` 字节语义**：`Meshlet::triangleOffset` 是**字节偏移**（每三角 3 字节），不是三角号——读三角窗口一律 `base = triangleOffset + t*3`（`meshlet.hpp` 已钉死注释；MILESTONES:684）。
- **与官方效果对齐**（AGENTS.md 纪律第 3 条）：fallback 焊接 2.6% ≈ UE 默认 2%（不做第二次独立简化）；朝向锚判定随折叠同步更新；按组重切保证父子簇不重叠。任何偏离官方口径必须解释留痕（写进 `S*-PLAN.md` / `MILESTONES.md` 踩坑栏）。

---

## 6. 调试工具链（S1~S14 沿用）

- **L0** 画面证据（截图/PNG）→ **L1** RenderDoc（进程外零改动启动，精确抓帧才加 `--capture-frame N`）→ **L2** NVRHI timestamp query（整帧 GPU 计时）→ **L3** Nsight / PIX / PresentMon / Tracy（时间线佐证）。
- RenderDoc 覆盖 DX12 + Vulkan 双后端；调试工具**不进 CMake、不读 SDK 路径**。
- `viewer --selftest` / `nanite-tools selftest <sub>` 是无 GPU、无窗口的机械验收，CI 里 `ctest` 直接跑；提交 / tag 前 `python tools/regression.py` 全量历史用例 × 双后端比对基线必须全绿。

---

## 7. 踩坑记录怎么写（台账纪律）

`docs/MILESTONES.md` 四栏（动作 / 提交版本 / 完成情况 / 踩坑）**写实追加，不许写「已完成」「无」**。踩坑栏固定格式：**现象 → 根因 → 处理**。本文件 §3 的表格就是按这个格式沉淀出来的，新踩坑优先补进对应表格行，再在台账留痕。
