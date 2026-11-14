# RMLUI-PLAN — 游戏界面 RmlUi 接入（M-UI0，2026-11-15 立项）

> 轨道：`feat/ui-rmlui`（wt `E:/UE_STUDY_2026/nl-engine-ui-rmlui`，建于 master `38e8db8`）。
> 用户拍板（2026-11-15 04:41）：选型 RmlUi（对比表见对话记录）+ **一步到位含 C# 桥接**。
> A 类工具版交代：`--rmlui-demo` 缺省关，非 demo 路径零渲染改动，**不 bump `kStage`**、
> 不进渲染 bytes/near 总闸。

## 1. 目标

给 nl-engine 接入 vendored RmlUi（HTML/CSS 子集声明式 UI）：

1. **C++ 侧**：RmlUi 跑在 renderer/（唯一图形 API 模块），NVRHI 双后端共享一份实现；
   业务代码只见 `nl::renderer::RmlUiFrame` POD 与薄封装，不接触 RmlUi 类型。
2. **C# 侧**：走既有 ApiTable 单源面（`exports.api` 尾部追加 Ui 域 → gen.py 生成接线 →
   Bindings.cpp 实现 + C# facade），桥接层只做「文档生命周期 + 文本读写 + 事件转发」三个
   原语，UI 逻辑全部留在 C#。
3. **开发者体验**：文档/样式热重载（mtime 驱动）+ F8 调试器（元素拾取/盒模型/事件日志）。

## 2. 增量阶梯（M-UI0.0 ~ M-UI0.3，每步验收判据）

### M-UI0.0 vendor + 构建地基

- `thirdparty/rmlui/`：RmlUi 上游（MIT）core + debugger 源码子集 + `LICENSE.txt` +
  `VENDOR.md`（版本/commit/裁剪清单/升级纪律，Jolt 格式对齐）。Samples/Backends 不入库
  （Backends 参考件以「抄思路不抄代码」口径看上游渲染接口签名）。
- `thirdparty/freetype/`：FreeType（FTL/GPL+——RmlUi 字体引擎硬依赖）源码子集 +
  `VENDOR.md` + 许可证双文件（FTL + GPL-2.0 原文随库，消费走 FTL）。
- `assets/ui/fonts/`：OFL 字体（Noto Sans Regular 优先，失败则 RmlUi sample 字体 +
  其独立许可文件）。RmlUi 无字体加载不了任何文档，字体是硬前提。
- CMake：`nl_freetype STATIC` + `nl_rmlui STATIC`（含 Debugger），零厂商 SDK、零联网
  （架构纪律同 Jolt 条目）。
- **验收**：主仓 wt 双后端 BUILD OK；`nl_rmlui` 单独编译无警告升级。

### M-UI0.1 renderer 接入 + viewer demo

- `renderer/shaders/rmlui.slang`：vs/ps 单对（hud.slang 同构，**独立文件不动 hud.slang**
  ——老 shader 产物 100% 不变）；CMake 入口清单 + `kArtifacts` 登记（M8.1 教训：新入口
  必须同步登记）。
- `renderer/src/rmlui_backend.{hpp,cpp}`（`nl::renderer::detail::RmlUiBackend`）：
  - `Rml::RenderInterface` → NVRHI：纹理（writeTexture）/几何摊平/逐 draw 绑定切换，
    绘制纪律与 `Hud` 同门（顶点展开 8 float、scissor、srcAlpha 混合、深度关）。
  - `Rml::SystemInterface` → core：log 走 `nl::core::log`、文件走 `core::paths`。
  - `RmlUiFrame` POD（renderer.hpp 尾部追加）：鼠标/键盘/文本/尺寸/开关，viewer 喂。
    缺省全零 = 非 demo 路径零扰动。
  - 热重载：每帧 stat 已加载文档 + rcss mtime，变了即重载（无编辑器的体验补偿）。
  - F8：`Rml::Debugger::SetVisible` 翻转（仅 demo 开时生效）。
- `viewer/main.cpp`：`--rmlui-demo` 旗标（Options 尾部追加）+ 输入喂数 + `RmlUiFrame`
  线程穿 `renderFrame`（与 HudFrame 同路）；绘制顺序 = scene → rmlui → hud（ImGui
  调试面板永远压顶）。
- **验收**：双后端 `viewer --rmlui-demo --screenshot ... --frames N` 出 PNG：
  demo 文档可见、日志锚「RmlUi ready」；既有全部回归卡零漂（缺省关）。

### M-UI0.2 C# ApiTable 桥接

- `script/script-api/exports.api` 尾部追加 **Ui 域**（只许尾部追加，禁中间插桩）：
  - `uiLoadDocument(in str path, out int handle)` / `uiCloseDocument(in int handle)`
  - `uiShowDocument(in int handle, in int show)`
  - `uiElementSetText(in int handle, in str elId, in str text)` /
    `uiElementGetText(in int handle, in str elId, out str text)`
  - `uiSetEventCallback(in ptr fn)`——reverse P/Invoke，ABI
    `int fn(const char* elId, const char* eventType, const char* param)`，
    script-api 静态保活同 consoleRegister 口径。
- 重跑 `python tools/scriptapi/gen.py`（5 生成物全提交；CMake freshness 闸过 `--check`）。
- C++ 实现体在 `core/src/script/Bindings.cpp`；renderer 侧实现经
  `nl::script::UiSink`（core 定义纯虚接口，viewer 装配 —— CameraSource 同构，
  nl_core 不反向依赖 renderer）。
- C# facade：`script/script-api/Api.cs` 加 `Ui` 静态类 + `script/examples/ui/`
  演示脚本（load → setText → 事件回调打日志）。
- **验收**：`--script` 挂 UiScript 跑通：C# 改文本可见、点击按钮 C# 回调触发（日志锚）；
  `gen.py --check` 绿。

### M-UI0.3 收官

- 截图证据卡（双后端）+ 展示台登记 + `MILESTONES.md` M-UI0 节 + 日志。
- `close` 闸 → `release --done` → merge master（§7.2 文档闸）。

## 3. 三处对照（决策前置）

| 来源 | 对应实现 | 取材 |
|---|---|---|
| Carrot | `engine/imgui/` HUD overlay：renderer 级共享 ImDrawData→顶点摊平→双后端提交 | 摊平/混合/scissor 纪律直接对齐本仓 Hud（已同构） |
| bevy | `bevy_egui`：渲染提取层隔离 UI 库类型，ECS 侧只见 POD | RmlUiFrame POD 通道同构；数据驱动 UI 思路与 RmlUi data binding 对齐 |
| UE | UMG/Slate：retained-mode widget + 样式系统；Rml 接近 Slate 的声明式样式层 | **RmlUi 无编辑器（已拍板接受）**——文本工作流 + 热重载 + F8 调试器补位 |

三处口径差异如实记录：Carrot 无声明式 UI 层（imgui 即时模式），bevy_egui 也是即时模式；
UE Slate 是保留模式但闭源只读。RmlUi 是三方里唯一「声明式样式 + 保留模式 + 可 vendored」
组合，本轨按其官方 RenderInterface/SystemInterface 契约实现（上游文档为权威，不抄
Backends 代码）。

## 4. 双后端纪律

- 一份 `rmlui_backend.cpp` 同时服务 DX12/Vulkan（与 `Hud`/`Scene` 同构——后端差异只
  在 shaderSuffix/入口名/编译期选定文件）。
- rmlui.slang 单源双目标（dxil + spv），`kArtifacts` 同步登记 4 产物。
- 缺省关：`RmlUiFrame` 缺省全零 → backend 不建 context 不碰管线 → 全部历史截图基线
  bytes 不动、`kStage` 不 bump。

## 5. 不在范围

- 编辑器（上游没有；MyGUI LayoutEditor 对照项已拍板不追）。
- RmlUi Lua/AngelScript 插件、libRmlSvg、Lottie 等可选模块。
- C# 侧完整 data-binding 桥（数据模型双向同步留 M-UI1+，本期只做三原语）。
- 输入焦点抢占（wantCaptureMouse 通道本期不接，demo 模式鼠标直通）。
