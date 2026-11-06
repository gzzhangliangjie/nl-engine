# ImGui HUD 使用说明

> 适用：M0.5 起所有需要在画面上显示的调试信息 / 参数控件。
> 定位：**这是一块调试 HUD，不是编辑器**。做它的目的是给 S1~S13 每层「能单独关掉并看到退化」提供实时观测与开关入口（ROADMAP 验收心法），不是做界面产品。
> 纪律：图形 API 符号只在 `renderer/`；ImGui 头文件只在 `renderer/src/hud.cpp` 与 `nl_imgui` target 里出现；`core/` / `viewer/` 只认 POD。

---

## 1. 分层：数据怎么从窗口走到像素

| 层 | 文件 | 职责 |
|---|---|---|
| 平台输入 | `core/src/window.cpp` | GLFW 回调攒出鼠标位置 / 左键 / 滚轮增量 / 边沿按键，`mousePosition()` / `consumeMouse()` / `keyPressed()` 供上层取 |
| 帧数据 | `viewer/main.cpp` | 每帧填 `nl::renderer::HudFrame`（显示数据），`renderFrame()` 后读回同一 POD 里的控件结果 |
| 双向 POD | `renderer/include/nl/renderer/renderer.hpp` → `struct HudFrame` | **公开头只有 POD**，不泄漏 ImGui / NVRHI 类型 |
| UI 构建 | `renderer/src/hud.cpp` → `Hud::build()` | `ImGui::NewFrame()` → 组面板 → `ImGui::Render()`；把 `ImDrawData` 摊平成顶点 / 索引 / `DrawCmd{scissor}` 列表，并写回控件值 |
| GPU 绘制 | `renderer/src/hud.cpp` → `Hud::draw()` | 每帧 `writeBuffer` 上传 VB/IB/volatile CB，逐 `DrawCmd` 设 scissor + alpha blend，`drawIndexed` |
| shader | `renderer/shaders/hud.slang` | **同一份源码出 SPIR-V + DXIL**；`float4 g_projection` 把屏幕像素转 NDC，PS = 字体图集 × 顶点色 |
| 第三方 | `thirdparty/imgui/` | **1.91.9 主线稳定版**（4 个 cpp，本地 vendored，零网络）；不用官方 D3D12 / Vulkan 后端 |

> 为什么自己写渲染后端：ImGui 官方后端是裸 D3D12 / Vulkan 两套，会把图形符号散到两处、还得跟 NVRHI 的同步模型对齐。这里照 `Scene` 的样子写一份 NVRHI 共享代码，两个后端同一份，`hud.slang` 也走「单源双目标」纪律。

## 2. 加一个控件（最常用操作）

**只要显示、不回传**：直接在 `Hud::build()` 的 `Begin/End` 之间加一行：

```cpp
ImGui::Text("draw calls: %u", value);
ImGui::PlotLines("frame ms", history, count, 0, nullptr, 0.f, 40.f, ImVec2(260, 52));
```

**要能被 viewer 生效（回传）**，三步：
1. `renderer.hpp` 的 `HudFrame` 加一个字段（只能是 POD 类型，带默认值）；
2. `Hud::build()` 里把控件绑到该字段；
3. `viewer/main.cpp` 每帧把当前真实值填进 `hud`，`renderFrame()` 之后读回并应用到相机 / `FrameParams` / 清屏色。

现有三个例子：`Checkbox("back-face culling", &frame.cullBack)`、`SliderInt("clear color", &frame.colorIndex, 0, 3)`、`Button("reset camera")`（置 `frame.resetCamera`，viewer 消费后清零）。

### 下拉框 / 选择菜单（已支持，无需改渲染后端）

```cpp
ImGui::Combo("clear color", &frame.colorIndex,
             "deep blue\0orange\0green\0purple\0", 4);
ImGui::BeginCombo("mode", current);
if (ImGui::Selectable("normal", ...)) { /* ... */ }
ImGui::EndCombo();
```

`Combo` / `BeginCombo` / `ListBox` / `Selectable` / `RadioButton` / `BeginMenuBar` / `TreeNode` 都在已 vendored 的 `imgui_widgets.cpp` 里。下拉展开时是**独立弹出窗口**，会多出 `ImDrawList` 和多组 ClipRect；`Hud::build()` 本来就遍历全部 `CmdLists`、`Hud::draw()` 逐条 `ImDrawCmd` 设 scissor，所以多窗口叠加天然支持。主面板的 `NoResize` 固定尺寸不影响弹出层。以后 `--mode` 加到十几个模式，用同一个 Combo 做模式选择器最自然。

## 3. 输入映射（当前接到哪一步）

| ImGui IO 字段 | 来源 | 状态 |
|---|---|---|
| `DisplaySize` / `DeltaTime` | `HudFrame` 每帧填 | ✅ |
| `MousePos` | `Window::mousePosition()`（GLFW cursor 回调的绝对坐标） | ✅ |
| `MouseDown[0]` | `Window::leftButtonDown()` | ✅ |
| `MouseWheel` | `Window::consumeMouse()` 的 scroll 增量 | ✅ |
| `WantCaptureMouse` | `Hud::build()` 写回 `HudFrame`，viewer 下一帧用它跳过相机拖动 / 滚轮缩放 | ✅ |
| 键盘字符 / 修饰键 | 未接；viewer 只有 `keyPressed(C/R/Space/F12/Escape)` 边沿键 | ⬜ |
| 右键 / 中键 / 手柄 | 未接 | ⬜ |

**影响**：下拉框、列表、树、表格选择、滑块、颜色选择器、拖拽、多选**现在都能用**（只要鼠标左键 + 位置 + 滚轮）。**只有需要打字的 `InputText` 要补字符回调**（约 50 行，把 GLFW 的 `char` / `key` / `mod` 事件喂给 `ImGuiIO`）；我一度把这条说成「复杂控件的前置条件」，对下拉框并不成立。

## 4. 渲染约定与边界

- **顶点**：`HudVertex` = 8 个 float（`pos.xy / uv.xy / color.rgba`，32 B），CPU 侧把 `ImU32` 颜色展开，避开 `R8G8B8A8_UNORM` 顶点格式在两后端的差异；索引统一转 `R32_UINT`。VB/IB 从 4096/8192 起按 2 倍扩容。
- **纹理**：目前**只绑一张字体图集**，忽略 `ImDrawCmd::TextureId`。要显示调试图（`ImGui::Image`、探针热力图、vis buffer）需加「纹理绑定表」，按 TexID 换 binding set。
- **字体**：默认图集只有 ASCII。Vulkan 的调试层字符串原本含中文，在 HUD 里显示成 `?????`，M0.5 已把 `DeviceInfo::debugLayer` 改成 ASCII（`backend_vulkan.cpp`）。要显示中文标签需把系统 TTF 灌进 atlas。
- **状态**：`cull none` + `disableDepthTest/Write` + `enableScissor` + `SrcAlpha / InvSrcAlpha`，HUD 永远压在 3D 画面之上；`SetNextWindowBgAlpha(0.82)` 让背后模型仍可见。
- **投影**：`hudProjection()` 的 Y 尺度必须取负（ImGui y 向下、NDC y 向上），少一个负号整个面板上下颠倒——本轮踩过，已由 `hudSelfTest()` 锁住。
- **缓冲状态**：VB/IB 用 `initialState=CopyDest + keepInitialState=true`，否则第二帧 `writeBuffer` 会报 `Unknown prior state of buffer`（本轮踩过，见台账）。Vulkan 后端禁止 volatile 非 CB buffer，故 VB/IB 不是 volatile。

## 5. 截图回归为什么能复现

实时 FPS / 帧时间每帧都不同，写进 PNG 会让基线 MD5 永远对不上。因此：

- `viewer.exe` **交互窗口默认开 HUD**，显示真实采样；
- `--screenshot` 回归模式**默认关 HUD**（M0.1~M0.4 的基线逐字节不变，实测 MD5 `7e528087…` 与 M0.4 相同）；
- `--hud` 强制开：时序数据换成固定样本、面板标注 `capture/replay (fixed samples)`，`--no-hud` 强制关；
- `ImGui` 的 `IniFilename / LogFilename` 置空、窗口位置与尺寸用 `ImGuiCond_Always` 固定，不依赖 `imgui.ini` 或历史状态。

两后端 `--hud` 截图**必然不同**：面板里印的 `backend` 与 `debug layer` 本来就按后端不同，这正是「当前后端名」的可见证据，不是画面 bug。展示台 `example/cases.json` 的 `hud` / `resize` 用例 note 里已写明。

## 6. 复现命令

```bash
scripts/build-m0.2.bat DX12          # 或 VULKAN
build/<backend>/viewer.exe --mode normal --hud            # 交互窗口：拖 HUD 控件看画面变化
build/<backend>/viewer.exe --screenshot out.png --frames 8 --hud --size 960x540
build/<backend>/viewer.exe --screenshot out.png --frames 8 --no-hud   # 回到无 HUD 基线
build/<backend>/viewer.exe --selftest                     # 14/14，含 8 个 shader 产物魔数
example\m0.5\run.bat all                                 # 双后端四个用例一次跑完
python tools/showcase/server.py --check                  # 归位门禁
```

`--mode` 分发器在 `core/include/nl/core/modes.hpp` / `core/src/modes.cpp`，S1 起新增模式只改三处（枚举 + `kModeNames` + `runMode()` 的 case），未知模式名直接退出 1 并列出可用模式。

## 7. 后续增量（都不需要重做渲染层）

| 想做 | 补什么 |
|---|---|
| 输入框 / 快捷键 | GLFW 字符 + 修饰键事件喂 `ImGuiIO` |
| 显示 GPU 调试图 | 纹理绑定表（按 `TextureId` 换 binding set） |
| 中文标签 | 加载系统 TTF 进字体 atlas |
| docking / 多视口 | 换 docking 分支 + 多视口平台后端 |
| 曲线 / 图表 / 节点图 / 3D gizmo | 加 `implot` / `imgui-node-editor` / `ImGuizmo` 的 cpp（本机 `Carrot/thirdparty/` 现成） |
| 真实 GPU 耗时曲线 | NVRHI timestamp query 填进 `HudFrame`，见 `docs/s0/S0-PLAN.md`「可观测性与调试工具计划」L2 |
