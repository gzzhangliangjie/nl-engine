# nl-engine 外置编辑器轨道计划（EDITOR-PLAN）

> 轨道：`editor`（独立 `feat/editor` worktree，与渲染主线 S*/M* 正交）
> 参照：`D:\SunshineOfficial` / `D:\SunshineClient`（仅借鉴架构思想，不复用其代码）
> 协议契约：`docs/editor/RPC-SPEC.md`（v2，C++ + Dear ImGui 路线）
> 立项日期：2026-11-10

---

## 1. 目标（用户原始 4 条需求）

1. 编辑器与引擎分属两个独立进程，一方崩溃不影响另一方；
2. 两进程通过 RPC 交互（控制指令 / 事件上报）；
3. 引擎原生窗口经 Win32 `SetParent` 嵌入编辑器窗口（像素直出屏幕，**绝不走 RPC**）；
4. 编辑器侧以插件形式扩展多种工具，插件经 RPC 驱动引擎。

---

## 2. 已拍板决策（开工闸：架构项 0 待拍板）

| # | 决策 | 依据 |
|---|---|---|
| 1 | **编辑器宿主 = C++ + Dear ImGui** | MIT、零运行时依赖、与引擎共享 C++ 类型，契合 vendored 习惯；Qt/PyQt5 因许可成本已排除 |
| 2 | **RPC 传输 = msgpack + 4 字节长度前缀 + 本地 TCP** | 保留 JSON-RPC 2.0 语义，替换编码；编辑器=Server，引擎=Client（对齐 Sunshine 连回模型） |
| 3 | **窗口嵌入 = Win32 `SetParent`** | 把引擎 GLFW 窗口 HWND 挂入编辑器 ImGui 视口框；每帧像素直绘屏幕，不进 RPC |
| 4 | **崩溃隔离 = 心跳 2s / 超时 6s** | 编辑器掉线引擎继续自渲染；编辑器走空视口 + 1s 自动重连（需求 #1 硬保障） |

**第三方库现状**：`imgui` / `imgui-node-editor` / `glfw` / `nvrhi` / `tinygltf`(nlohmann json) / `taskflow` 均已 vendored 于 `thirdparty/`，**无需新增依赖、无需联网**。msgpack 编解码采用**自研极简实现**（仅覆盖信封所需类型 nil/bool/int/str/bin/float/array/map），零新增 vendored 库；后续如需完整 msgpack 再 vendored `msgpack-c`。

---

## 3. 分期（里程碑编号沿用 `M-E*` 前缀，不占 m0.* / S* 编号）

- **M-E0** 编辑器外壳骨架：独立 C++ 程序 + ImGui 主窗 + 可停靠视口框 + 最小插件加载器（无引擎也可启动）。 **[DONE 2026-11-11]** — 全量编译链接通过（`editor.exe`/`rpc_self_test.exe` 均产出）。GUI 运行需桌面+GPU 会话，headless 沙箱无法跑 GUI（GLFW 在无证显会话里段错误，环境限制非代码缺陷）。
- **M-E1** 最小 RPC 通路：`RpcHost`(server) + `RpcBridge`(client) + 握手/心跳/echo 双向跑通（含自研 msgpack 编解码 + TCP 分帧）。 **[DONE 2026-11-11]** — `rpc_self_test.exe` 无头自测 11 项全绿（accept / announce(hwnd·pid·proto) / 正向 echo / 反向 echo / 2.6s 心跳 ping-pong 不误判掉线）。
- **M-E2** 视口嵌入：`SetParent` 挂引擎 HWND + 随框尺寸同步 + 几何 watcher。 **[DONE 2026-11-11，画面验收全过]** — 双端接线全量编译链接通过：viewer 加 `--editor-bridge`（可选启动，连不上降级单机；两个交互主循环每帧泵 `bridge.update()`）；编辑器 `attachEngine` 建 WS_CHILD 容器窗 + `EmbedWindow::attach` 跨进程 SetParent，每帧 `syncNative` 贴面板内容区 + 引擎窗铺满容器（WM_SIZE 驱动引擎侧 swapchain 重建）；`onEngineClose` 拆容器回落占位（崩溃隔离可视化）。验收三项实机全过：① 跨进程 SetParent 实挂（Win32 窗口树：容器 NlEditorViewportHost 与引擎窗 GLFW30 pid 跨进程、矩形完全重合）+ GDI 截屏确认 Suzanne 像素直出无偏移（修复 ImGui 坐标系双重 ScreenToClient 偏移 bug）；② resize 跟手（viewer log `resize -> 964x585`）；③ `taskkill viewer` 后编辑器存活、容器自动拆除回落占位（崩溃隔离实挂证据）。另修 GL2 后端移植 bug：客户端数组模式 `glVertexPointer` 第 4 参须传内存基址+偏移（原误传纯偏移 → 驱动近零地址读顶点段错误），并摘除误设的 `RendererHasVtxOffset`。
- **M-E3** 第一个工具插件（相机/场景树）经 RPC 驱动引擎。 **[DONE 2026-11-11，跨进程探针 + UI 点击链路双验收全过]** — 引擎侧 `EditorEmbed::bindCamera`（runNormal/runScene 双模式各 bind 一次）注册 5 个 handler：`camera.getState`（fov/dist/yaw/pitch，度）/ `camera.setFov` / `camera.setDistance`（回读 clamp 后实际值）/ `camera.orbit`（像素增量 ×kOrbitGain=0.007 rad/px）/ `camera.reset`；params 统一命名 Map，未连接也注册（standalone 零开销）。编辑器侧 `CameraPlugin`（替换 stub）：Refresh 拉真值、FOV/Distance 滑条、Orbit 四向按钮（度数语义，deg→px 换算 kPxPerDeg≈2.4913 在编辑器侧，修正按钮"±15deg"实转 6° 的语义错位）、Reset；每次操作后 getState 对齐 UI。验收：`tools/m-e3-cam-probe.py`（python 假编辑器 server，真跨进程打 viewer handler）22 断言 0 fail（announce hwnd/pid/proto、setFov 45 与 clamp 500→120、setDistance clamp 0.01→0.45、orbit 增量精确匹配 0.007 rad/px、reset 回 4.2/0.21rad/0.16rad 且不动 fov、bogus method error 100）；UI 链路 SendInput 实点 Refresh+Orbit R 截屏双证据：状态行 `fov 45.0 dist 4.20 yaw 27.0`（12.0→27.0 精确 +15°）、引擎 HUD 同值、Suzanne 画面可见右转。

---

## 4. 目录结构（claim 后落地于 `feat/editor` worktree）

```
editor/                        # 编辑器宿主进程（C++ / ImGui）
  CMakeLists.txt
  main.cpp                     # 入口：建窗 + ImGui + 泵 update()；含 GL 上下文判空（headless 优雅退出）
  rpc_host.hpp / .cpp          # RpcHost（Server 角色，监听引擎 client）+ isConnected() 状态查询
  viewport.hpp / .cpp          # ImGui 视口框 + SetParent 嵌入控件（M-E2 接线点）
  plugin.hpp / .cpp            # 插件扩展点 / Plugin 基类（隔离原则）+ CameraStub 内建样例
  imgui_backend.hpp / .cpp     # 自建极简 ImGui 后端（GLFW + OpenGL 2.0 固定管线；vendored imgui 为
                               #   标准版非 docking，ImTextureID=ImU64，需经 (ImTextureID) 强转）
  rpc_self_test.cpp            # M-E1 无头自测：同进程双线程 server+client 跑通握手/announce/echo/心跳
editor_bridge/                 # 引擎侧 bridge（新增，不碰既有 renderer 图形路径）
  CMakeLists.txt
  msgpack_min.hpp              # 自研极简 msgpack 编解码（零依赖，host/bridge 共用，落此目录）
  rpc_bridge.hpp / .cpp        # RpcBridge（Client 角色）+ update() 泵
  embed_window.hpp / .cpp      # 接收父 HWND / 上报自身 HWND（M-E2 暴露 API）
```

引擎侧 `editor_bridge/` 与渲染主线 `renderer/` 解耦，仅经 `update()` 在主循环每帧泵；不改动 `renderer/` 任何图形 API 路径。

---

## 5. 冻结面（不碰）

- 不碰渲染主线 S*/M* 编号与 `kStage`；
- 不触发 AGENTS.md 三源对照规则（编辑器轨道与渲染源码正交）；
- 不引入新 vendored 依赖（除已确认存在的 imgui 系 / glfw，均已在 `thirdparty/`）；
- 引擎侧 `editor_bridge/` 只新增、不改既有 `renderer/` 图形路径。

---

## 6. 待拍板项（调参，M-E1 落地时一句话定稿）

> 以下四项已在 M-E1 实现中按推荐值固化，记录备查；如需变更属 §15 范围扩展。

- [x] **端口**：固定 `29110`（本机本地回路 127.0.0.1）。`RpcHost::start()` 已硬编码 `htons(29110)`；`endpoint` 形参预留给 §15 的命令行/文件通知扩展。
- [x] **传输**：TCP 默认（msgpack + 4 字节小端长度前缀 + JSON-RPC 2.0 信封语义）。命名管道留作 §15 可选；`RpcBridge::connect(endpoint)` 形参已预留。
- [x] **SetParent 方向**：编辑器主动挂。`EmbedWindow::attach(parentHwnd, engineHwnd)` 由编辑器侧调用（parent=编辑器视口原生子窗，child=引擎 HWND）。对启动顺序更鲁棒——引擎先起或后起均可，announce 到位即挂。
- [x] **多视口**：M-E1 单视口。`engine.announce` 的 `hwnd` 为单值；多视口（同一引擎多个嵌入窗）是 M-E2+ 扩展，`caps` 字段已预留数组位。

---

## 7. 验收口径（对齐既有 tools 轨道）

- 每里程碑产出可读证据（启动截图 / RPC echo 日志 / 嵌入画面）；
- 不进渲染 `bytes/near` 总闸；
- 收尾按 PARALLEL-TRACKS §7.6 双向自查 + 同步主树注册表 + merge 回 master（分支与 tag 留档）。

---

## 8. 进度台账

### 2026-11-11 — M-E0 + M-E1 落地（feat/editor worktree）

**产物（均未提交，留 worktree，遵守 tree_gate）**
- `editor/` 宿主进程全量编译链接通过：`editor.exe`（GUI 外壳）、`rpc_self_test.exe`（M-E1 无头自测）。
- `editor_bridge/` 静态库 `nl_editor_bridge` 编译通过，msgpack 编解码 + `RpcBridge`(client) + `EmbedWindow` 框架就位。
- 根 `CMakeLists.txt` 追加 `add_subdirectory(editor_bridge)` 与 `add_subdirectory(editor)`（构建接入）。
- 新增 `editor/build-editor.ps1`：PowerShell + 反斜杠 Windows 路径 + NMake + 自管 `INCLUDE/LIB`，供本机无 vcvars 环境构建（不入 git，属本地工具）。

**M-E1 验收（`rpc_self_test.exe`，本地无头 PASS，0 failures）**
```
=== M-E1 RPC self-test ===
  [ ok ] RpcHost.start() 监听 127.0.0.1:29110
  [ ok ] 引擎 client 已连上（accept 成功）
  [ ok ] engine.announce 上报 hwnd == 0xDEADBEEF
  [ ok ] engine.announce 上报 pid 正确
  [ ok ] engine.announce 上报 proto == 1
  [ ok ] host.call(engine.echo) 成功返回 RpcValue
  [ ok ] 回包内容 == 请求内容 (echo)
  [ ok ] client.call(editor.echo) 回包内容 == 请求内容 (反向 echo)
  [ ok ] 心跳 2.6s 内持续 ping/pong，host 未误判掉线
=== M-E1 self-test PASS (0 failures) ===
```
覆盖：握手 / 双向 announce(hwnd·pid·proto) / 正向与反向 echo / 心跳 ping-pong 不误判掉线。

**踩坑修正（已就地修复，不入 git 历史噪音）**
- vendored imgui 标准版 `ImTextureID = typedef ImU64`（非 `void*`）→ `imgui_backend.cpp` 三处改 `(ImTextureID)` 强转。
- `rpc_self_test.cpp` 直接用了 `msgpack::Map/Value`，但本构建命名空间是 `nl::editor::msgpack` → 加 `namespace msgpack = nl::editor::msgpack;` 别名。
- 自测须泵 `RpcHost::update()` 才能 accept（与真实编辑器主循环一致）；反向 echo 等待期也须持续泵 `bridge.update()`，否则收不到 host 的 req。
- `RpcHost` 新增 `isConnected()` 供连接状态 UI；`main.cpp` 加 `glfwGetCurrentContext()` 判空，headless/无 GPU 时优雅退出（非段错误）。

**Headless 限制（明确标注）**
- 本沙箱无桌面+GPU 会话，`editor.exe` 无法跑 GUI（GLFW 在无显示会话里硬段错误，属环境限制）。M-E1 已用无头 `rpc_self_test` 充分验证；GUI 画面验收需在有桌面的机器上跑 `editor.exe` + `viewer.exe`（M-E2 接线后）。

**下一步**
- M-E2：引擎 `viewer.exe` 接 `editor_bridge`——`EmbedWindow::announce(bridge, engineHwnd, pid, proto, caps)` + 主循环每帧 `bridge.update()`；编辑器侧 `onEngineAnnounce` 拿视口原生子窗 HWND 调 `EmbedWindow::attach` 完成 SetParent。
- M-E3：`CameraStubPlugin` 接真实相机/场景树 RPC。

### 2026-11-11 — M-E2 落码 + 画面验收全过（feat/editor worktree，未提交）

**产物（未提交，留 worktree）**
- `viewer/main.cpp`：`Options.editorBridge` + `--editor-bridge` 解析；`EditorEmbed` 辅助结构（connect 29110 → 注册 `engine.echo` handler → `EmbedWindow::announce` 上报 HWND/pid/proto）；`runNormal`/`runScene` 两个交互主循环 `pollEvents` 后各泵一次 `edEmbed.pump()`；截图回归模式强制不启用（隐藏窗 + 定帧退出，嵌入无意义）；usage 加三条文案。
- `viewer/CMakeLists.txt`：`target_link_libraries(viewer ... nl_editor_bridge)`。
- `editor/viewport.hpp/.cpp`：`attachEngine()`（建 WS_CHILD 容器窗 + `EmbedWindow::attach` 跨进程 SetParent，幂等可重连）、`detach()`（掉线拆容器）、`syncNative()`（每帧容器贴面板内容区 ScreenToClient+SetWindowPos；引擎窗 MoveWindow 铺满容器，同尺寸去抖）。
- `editor/main.cpp`：`GLFW_EXPOSE_NATIVE_WIN32` + `glfwGetWin32Window(w)` 拿编辑器顶层 HWND；`onEngineAnnounce` → `attachEngine`；`onEngineClose` → `detach`（崩溃隔离可视化证据）；Connection 面板加 `embed : SetParent ok/pending` 状态行。

**架构要点（对齐 §6 拍板口径）**
- attach 由编辑器主动挂：引擎侧零 attach handler，只 announce + 泵；引擎窗挂入编辑器容器后引擎 GLFW 照常自转（WM_SIZE → consumeResize → swapchain 重建）。
- 引擎 HWND 直接用 `Window::nativeHandle()`（实现本就返回 `glfwGetWin32Window`），core 零改动。
- 像素直出：native child 永远盖在父窗 GL 内容之上，容器精确贴 Viewport 面板内容区，ImGui 只出面板 chrome。

**验收（2026-11-11 实机全过，含画面三项）**
- 三目标全量编译链接 0 error（editor / rpc_self_test / viewer）；`rpc_self_test` 回归 PASS（0 failures），M-E1 通路零回归。
- ① **跨进程 SetParent 实挂**：Win32 窗口树枚举——容器 `NlEditorViewportHost` 与引擎窗 `GLFW30`（跨进程 pid）矩形完全重合（964x585 @150% DPI）；GDI BitBlt 截屏确认 Suzanne 像素直出、完整贴 Viewport 面板无偏移。期间修掉坐标系 bug：ImGui 的 `GetCursorScreenPos` 返回的已是窗口客户区坐标，`SetWindowPos` 子窗要的也是客户区相对坐标——原实现多了一次 `ScreenToClient`，双重相减导致整体偏移（用户目击"偏移了"即此根因）。
- ② **resize 跟手**：拖编辑器窗角，viewer log `resize -> 964x585`，引擎 swapchain 随 WM_SIZE 链路重建。
- ③ **崩溃隔离**：`taskkill viewer` 后编辑器存活（hwnd 不变）、`onEngineClose` 自动拆容器、子窗清零回落占位。
- 另修 GL2 后端移植 bug（画面黑屏段错误根因）：客户端数组模式下 `glVertexPointer` 系列第 4 参必须传「内存基址+偏移」（`vtx + OFFSETOF`），原误传纯偏移导致驱动从近零地址读顶数据段错误；并摘除误设的 `ImGuiBackendFlags_RendererHasVtxOffset`（GL2 不支持，16 位索引溢出风险）。行级插桩法定位（mark() 逐步缩圈 → GL 上下文探针排除环境 → cmd0 数据 dump 确认数据合法 → 锁定 GL 调用参数），定位后插桩全清。

**踩坑（本轮新增）**
- `windows.h` 的 min/max 宏：`/permissive-` 并不自动压掉（错误认知）→ viewer/editor 两目标显式 `NOMINMAX`；editor 的 `std::max(1, ...)` 在 viewport.cpp 被宏撕碎（C2059）实锤。
- windows.h 必须先于 glfw3.h 包含，否则 `APIENTRY` 重定义 C4005（glfw 官方口径）。
- ImGui 面板内嵌原生窗的经典时序：`GetCursorScreenPos()` 必须在 `Dummy(avail)` 之前取（Dummy 推进光标，之后取会偏一行）。
- 沙箱跑 viewer.exe 报 UCRT DLL 127：bash/Start-Process 环境缺 redist PATH + `HTTPS_PROXY/https_proxy` 大小写键冲突炸 Start-Process——均为环境问题，回归 harness（python subprocess + 系统 PATH）不受影响。

### 2026-11-11 — M-E3 相机插件（feat/editor worktree）

**产物**
- `viewer/main.cpp`：`EditorEmbed` 加 `camera` 成员 + `bindCamera()`（5 handler，见上）+ 两处主循环 bind 点。
- `editor/plugin.cpp`：`CameraStubPlugin` → `CameraPlugin`（Refresh/滑条/Orbit 四向/Reset，操作后 getState 回读对齐）。
- `tools/m-e3-cam-probe.py`：python 极简 msgpack 假编辑器 server（4 字节大端长度帧 + 信封），跨进程机械验收引擎 handler。

**验收（双段）**
- 段1 跨进程 RPC：probe 22 断言 0 fail（含 clamp 两向、orbit 增量与 kOrbitGain 精确匹配、reset 语义、负向 error 100）。
- 段2 编辑器 UI 链路：SetForegroundWindow 核验后 SendInput 实点 Refresh + Orbit R → 截屏：状态行 fov 45.0 dist 4.20 yaw 27.0（+15° 精确）、引擎 HUD 同值、Suzanne 可见右转。

**踩坑**
- 探针首版 3 条假 FAIL 全是脚本自身问题，引擎无错：reset 后 getState 返回**度**（12.032°=0.21rad），探针拿 0.21 当度数比；`orbit()` 参数是**像素**（×0.007 rad/px）不是度；msgpack fixstr 解码要覆盖 0xA0..0xBF（`(o&0xE0)==0xA0`，只判 `&0xF0` 漏一半）。
- 插件 orbit 按钮原写"±15deg"实转 6°（直传 15 给像素参数）——语义错位，deg→px 换算放编辑器侧（引擎 API 保持像素语义不动）。
- SendInput 点击验收：编辑器可能被其他前台窗口盖住（本次实发：WorkBuddy 聊天窗在上层，首轮点击点进了别人窗口）——必须 SetForegroundWindow + 每次点击后核验 `GetForegroundWindow()==hwnd` 再动手。

**待办**
- M-E4 候选（EDITOR-PLAN §4 未排期项）：场景树/材质检查插件、bridge 周期重连、焦点路由实测、announce.caps 填充。
- 体验缺口（已识别、未排期，做 M-E3 时拍板）：引擎先启/编辑器后启时 `--editor-bridge` 只连一次失败即永久降级单机，无周期重连；跨进程嵌入后的键盘/鼠标焦点路由未实测；`announce.caps` 仍空（分辨率/后端元信息预留位）。
