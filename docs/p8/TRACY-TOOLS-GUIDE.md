# TRACY-TOOLS-GUIDE — Tracy 性能/内存 + RenderDoc 抓帧（P8 轨道，M-TR0~M-TR3）

> 工具 exe：`build/{DX12,VULKAN}/viewer.exe`（默认 OFF，纯渲染）；性能/内存观测走 **Tracy**，单帧结构抓帧走 **RenderDoc**。
> 退出码约定（viewer）：`0`=正常退出 / `1`=运行失败 / `2`=用法错误（与 nanite-tools 同口径）。
> GUI 工具：Tracy `tracy-profiler.exe` 与 RenderDoc 均为**进程外工具，不入库、不进构建链**，本机装好即用。
> 展示台卡：`tracy-m0`（连接 + 时间线）/ `renderdoc-mtr2`（进程内抓帧）——见 §6 模板，**仅在装好工具 + GPU 的机器上接入 cases.json**（默认无头回归无 GPU/无 GUI，跑不通，勿进标准回归）。

## 1. 背景与分工

本轨道把「性能热点 + 内存占用/泄漏 + 单帧结构」三件事交给两个成熟工具，引擎零侵入：

| 工具 | 类型 | 看什么 | 接入方式 |
|---|---|---|---|
| **Tracy v0.15.0** | 埋点型（M-TR0/1） | 跨帧 CPU 时间线、job 分布、帧间隔抖动、内存曲线、退出残留清单 | 编译期 `NL_TRACY=ON` 埋点；运行时 GUI 连 client |
| **RenderDoc** | API 拦截型（M-TR2） | 单帧逐 draw call / pipeline / 资源状态 / 依赖 | 外部启动零代码；或 `--capture-frame N` 进程内抓帧 |

- 选型硬闸、版本、许可（Tracy BSD-3 / RenderDoc MIT）见 `P8-TRACY-PLAN.md` 与 `thirdparty/{tracy,renderdoc}/VENDOR.md`。
- **帧边界对齐**：Tracy `FrameMark` 落点在 viewer 主循环 present 完成处（M-TR0），RenderDoc 的「帧」= 两次 Present 之间——两边「第 N 帧」可互相指认。
- **互斥口径**：RenderDoc 注入有捕获开销，**性能基准与 Tracy 读数一律不在 RenderDoc 注入下采**；两者不同时开。

## 2. Tracy 连接工作流（M-TR0/1）

**构建**：`NL_TRACY=ON` 才会编译埋点与内存钩（默认 OFF，宏全空转、零开销）。
```bat
REM 重配 + 编（DX12 树示例；VULKAN 同理）
cmake -S . -B build/DX12-TRACY -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
       -DNL_GRAPHICS_BACKEND=DX12 -DNL_TRACY=ON
cmake --build build/DX12-TRACY --target viewer
```

**连机（裸机交互桌面）**：
```bat
REM 终端 A：起 GUI（须与 client 同版本 0.15.0）
E:\UE_STUDY_2026\.build-tools\tracy-gui\0.15.0\tracy-profiler.exe
REM 终端 B：起 viewer，ON 构建自动 broadcast 连接；--frames 给足帧数才有数据
build\DX12-TRACY\viewer.exe --selftest --frames 60 --profiling-hud
```
- GUI 里应看到：`viewer.frame` zone、`Main` 线程、命名后的 job 线程（`JobWorker`）、以及 `--profiling-hud` 窗口的 draw calls / instances / triangles / proc mem（K1/K2/K5）。
- 首连 Windows 防火墙弹窗（client 监听 TCP 8086）属预期，放行 `localhost` 即可。
- 内存：GUI Memory 面板看实时分配/释放曲线；**故意泄漏用例须走 ESC 干净退出**（进程被杀时 Tracy 数据丢失，残留清单只在正常 shutdown 完整）。`NL_TRACY_MEM_DEPTH` 默认 0（不抓调用栈，最低开销），泄漏归因到源码时按 `-DNL_TRACY_MEM_DEPTH=16` 重编。

**无头端到端验证（沙箱也能跑，M-TR0/1 已验）**：用同版本 `tracy-capture.exe` 监听 + 跑 `nanite-tools selftest jobs`（jobs 埋点）→ 落 `.tracy`。M-TR0 同用例 16.3 MB（仅 zones）；M-TR1 同用例 89.37 MB（含每个 new/malloc 的分配事件），证明内存钩真广播。

## 3. RenderDoc 进程内抓帧（M-TR2）

**零链接、默认关**：仅单头 `thirdparty/renderdoc/renderdoc_app.h`，viewer 通过 `GetModuleHandleA("renderdoc.dll")` + `GetProcAddress("RENDERDOC_GetAPI")` 被动拾取已注入的 in-app API；未注入时全程 no-op，行为零差异。

**用法（裸机）**：
```bat
REM 1) 本机装好 RenderDoc（自带 renderdoc.dll）
REM 2) 用 RenderDoc 的 Launch 拉起 viewer（或把 renderdoc.dll 放 exe 旁）
REM 3) 指定帧号，第 N 帧 present 后自动落 .rdc
viewer.exe --capture-frame N
```
- 两套接法互补：**外部启动**（零代码，热键抓帧，装好当天可用）+ **进程内**（`--capture-frame N` 确定性抓指定帧，CI/脚本友好）。
- `.rdc` 落盘路径默认在 RenderDoc 设置的捕获目录（GUI 可改），与 Tracy 不冲突。

## 4. 证据形态（不手绘）

| 观测 | 证据 | 备注 |
|---|---|---|
| Tracy 时间线 | GUI 截图（`viewer.frame` / `JobWorker` 线程 / zone 树） | 真机 GUI 截，非手绘 |
| Tracy 内存 | GUI Memory 面板曲线 + 退出残留清单导出 | 需 ESC 干净退出才有残留清单 |
| Tracy 无头 | `.tracy` 文件（tracy-capture 落盘） | 可同版本 tracy-profiler 打开复核 |
| RenderDoc | `.rdc` 文件（进程内/外部抓帧落盘） | RenderDoc GUI 打开逐 draw 看 |

## 5. 常见坑

1. **协议版本漂移**：Tracy GUI 与 client 必须**同版本**（0.15.0 client ↔ 0.15.0 GUI）；升级必须成对。`VENDOR.md` 记死配对。
2. **防火墙弹窗**：client 首次监听 8086 时弹窗，预期行为，放行 `localhost`。
3. **异常退出丢数据**：Tracy 依赖正常 shutdown flush；崩溃/`taskkill` 时未送出的帧全丢（崩溃观测走 ASAN 线，非 Tracy）。
4. **无头沙箱跑不了 GPU viewer**：viewer 需建 GLFW 窗口/显示器，纯无头会话起不来（DX12/VULKAN 两后端同在极早期 init 同点崩，与环境/GPU 沙箱限制有关，非代码问题）。性能/抓帧验证须裸机交互桌面。
5. **与 RenderDoc 互斥**：见 §1；两者不同时开。
6. **全局 new/delete 钩的链接**：Tracy 内存钩定义在静态库 `nl_core` 中，靠 `profiler.hpp` 的 MSVC `/INCLUDE` 强制拉入整 obj（否则链器丢弃、钩静默失效）。详见 `P8-TRACY-PLAN.md` M-TR1 实施记录。

## 6. 展示台工具卡模板（接管线，非默认回归）

> 下列 `kind:"tool"` 卡按 `example/cases.json` 精确 schema 编写。**Tracy 卡需 Tracy GUI 在旁监听、RenderDoc 卡需 RenderDoc 注入 + GPU**，本沙箱/默认无头回归均跑不通，故**勿直接并入标准回归**；在装好工具 + GPU 的机器上把它们贴进 `example/cases.json` 即可。case 级 `exe` 覆盖指到 `build/DX12-TRACY/viewer.exe`（Tracy 卡）或标准 `build/DX12/viewer.exe`（RenderDoc 卡）。

```json
{
  "id": "tracy-m0",
  "label": "M-TR0/1",
  "name": "Tracy 连接 + 时间线/内存观测（NL_TRACY=ON 构建）",
  "tag": "tracy-m0",
  "date": "2026-11-12",
  "summary": "ON 构建 viewer 连 tracy-profiler GUI：viewer.frame / JobWorker 线程 / zone 树 / Memory 曲线。无头可用 tracy-capture 落 .tracy 复核。",
  "acceptance": [
    "ON 构建 viewer 跑起来 → GUI 连上，看到帧序列 + zone 时间线 + 命名 job 线程",
    "无头：tracy-capture 监听 + nanite-tools selftest jobs → 落 .tracy（M-TR1 同用例 ~89MB，含内存事件）",
    "OFF 构建行为零差异（regression --only 抽查；TRACY_ENABLE 未定义）"
  ],
  "exe": {
    "dx12": "build/DX12-TRACY/viewer.exe",
    "vulkan": "build/DX12-TRACY/viewer.exe"
  },
  "cases": [
    {
      "id": "connect",
      "name": "ON 构建 viewer 连 GUI，时间线出现 viewer.frame",
      "kind": "tool",
      "note": "需 tracy-profiler GUI 预先监听 8086；expectLog 取首帧已 broadcast 的连接日志。",
      "args": ["--selftest", "--frames", "60", "--profiling-hud"],
      "expectExit": 0,
      "expectLog": "viewer.frame",
      "command": "viewer --selftest --frames 60 --profiling-hud"
    }
  ]
}
```

```json
{
  "id": "renderdoc-mtr2",
  "label": "M-TR2",
  "name": "RenderDoc 进程内抓帧 --capture-frame N",
  "tag": "tracy-m2",
  "date": "2026-11-12",
  "summary": "viewer 在 RenderDoc 注入下用 --capture-frame N 抓第 N 帧到 .rdc。零链接，默认关。",
  "acceptance": [
    "RenderDoc Launch 拉起 viewer --capture-frame N → 第 N 帧落 .rdc",
    "非 RenderDoc 下启动：no-op，行为零差异",
    "与 Tracy 不同时开"
  ],
  "exe": {
    "dx12": "build/DX12/viewer.exe",
    "vulkan": "build/VULKAN/viewer.exe"
  },
  "cases": [
    {
      "id": "capture",
      "name": "进程内抓第 N 帧到 .rdc",
      "kind": "tool",
      "note": "需 RenderDoc 注入（Launch 或 dll 旁置）；expectLog 取 TriggerCapture 后的落盘日志行。",
      "args": ["--capture-frame", "5", "--frames", "8"],
      "expectExit": 0,
      "expectLog": "RenderDoc capture triggered at frame 5",
      "command": "viewer --capture-frame 5 --frames 8"
    }
  ]
}
```

## 7. 已知边界 / 后续

- K3 GPU 显存占用（DX12 DXGI / Vulkan `VK_EXT_memory_budget` 不对称）**本轮不做**，K2 进程内存先顶；真需要时单独拍板。
- K4 per-pass GPU 拆分不进本轨道（S12/S13 性能验收按 L2 惯例扩 timer 段）。
- 全量渲染回归总闸不适用于本轨（观测类）；merge 回 master 待用户拍板。
