# thirdparty/renderdoc — vendored 子集

> M-TR2（2026-11-12）接入，归 `profiling` 轨道。本目录只入库 **in-app 抓帧 API 单头**，不入库 RenderDoc 本体（GUI / 捕获运行时是进程外工具，用户本机装好即可，零入库、零构建链）。

## 入库文件

| 文件 | 来源 | 用途 |
|---|---|---|
| `renderdoc_app.h` | RenderDoc 仓 `renderdoc/api/app/renderdoc_app.h`（clone HEAD `86cadb859462c37a1dccb7d920338b5b58147701`，2026-11-11） | in-application capture API：定义 `RENDERDOC_API_1_6_0` / `eRENDERDOC_API_Version_1_6_0` / `RENDERDOC_GetAPI` |

## 许可

**MIT License**（Baldur Karlsson，2015-2026）。头文件顶部自带许可声明，已随文件保留，未删减。

## 选型闸（与 Tracy 同源口径）

- 开源无风险：MIT，与本项目 MIT 兼容。
- 零构建期依赖：`renderdoc_app.h` 是纯头文件（仅 `#include <stdint.h>`），**不编译任何 .cpp、不链接任何库**。
- 零图形符号侵入：本头只定义 C 风格接口 struct 与函数指针；viewer 通过 `GetModuleHandleA("renderdoc.dll")` + `GetProcAddress("RENDERDOC_GetAPI")` 被动拾取已注入的 in-app API，**viewer/core 不出现任何 D3D12/Vulkan 符号**，符合 §M0.1 图形符号纪律。
- 默认关闭：仅在 `viewer --capture-frame N` 且进程在 RenderDoc 下启动时生效；否则 `GetModuleHandle` 返回空 → 全程 no-op，行为零差异。

## 用法（见 docs/p8/TRACY-TOOLS-GUIDE.md）

1. 本机装好 RenderDoc（GUI 自带 `renderdoc.dll`）。
2. 用 RenderDoc 的 **Launch** 拉起 `viewer.exe`（或把 `renderdoc.dll` 放 exe 旁）。
3. `viewer.exe --capture-frame N`：第 N 帧 present 后自动落 `.rdc`。

> ⚠️ 与 Tracy 互斥：RenderDoc 注入有捕获开销，性能基准与 Tracy 读数一律不在 RenderDoc 注入下采。
