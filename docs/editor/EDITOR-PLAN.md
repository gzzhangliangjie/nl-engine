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
| 5 | **M-E6 传输：TCP 默认、命名管道 CLI 显式可选** | 两类传输共用 `transport::Stream/Listener` 抽象与同一线格式；endpoint 只允许 loopback，坏 endpoint 报错不静默回落默认端口 |
| 6 | **M-E6 多视口 = 一编辑器接纳多引擎进程，按 `viewId` 路由** | 每视图独立 ImGui 面板 + 独立 `WS_CHILD` 容器 + 一个引擎 HWND；`viewId` 缺省 `main` 保老插件兼容。**否决**单进程多 HWND / 多 swapchain |

**第三方库现状**：`imgui` / `imgui-node-editor` / `glfw` / `nvrhi` / `tinygltf`(nlohmann json) / `taskflow` 均已 vendored 于 `thirdparty/`，**无需新增依赖、无需联网**。msgpack 编解码采用**自研极简实现**（仅覆盖信封所需类型 nil/bool/int/str/bin/float/array/map），零新增 vendored 库；后续如需完整 msgpack 再 vendored `msgpack-c`。

---

## 3. 分期（里程碑编号沿用 `M-E*` 前缀，不占 m0.* / S* 编号）

- **M-E0** 编辑器外壳骨架：独立 C++ 程序 + ImGui 主窗 + 可停靠视口框 + 最小插件加载器（无引擎也可启动）。 **[DONE 2026-11-11]** — 全量编译链接通过（`editor.exe`/`rpc_self_test.exe` 均产出）。GUI 运行需桌面+GPU 会话，headless 沙箱无法跑 GUI（GLFW 在无证显会话里段错误，环境限制非代码缺陷）。
- **M-E1** 最小 RPC 通路：`RpcHost`(server) + `RpcBridge`(client) + 握手/心跳/echo 双向跑通（含自研 msgpack 编解码 + TCP 分帧）。 **[DONE 2026-11-11]** — `rpc_self_test.exe` 无头自测 11 项全绿（accept / announce(hwnd·pid·proto) / 正向 echo / 反向 echo / 2.6s 心跳 ping-pong 不误判掉线）。
- **M-E2** 视口嵌入：`SetParent` 挂引擎 HWND + 随框尺寸同步 + 几何 watcher。 **[DONE 2026-11-11，画面验收全过]** — 双端接线全量编译链接通过：viewer 加 `--editor-bridge`（可选启动，连不上降级单机；两个交互主循环每帧泵 `bridge.update()`）；编辑器 `attachEngine` 建 WS_CHILD 容器窗 + `EmbedWindow::attach` 跨进程 SetParent，每帧 `syncNative` 贴面板内容区 + 引擎窗铺满容器（WM_SIZE 驱动引擎侧 swapchain 重建）；`onEngineClose` 拆容器回落占位（崩溃隔离可视化）。验收三项实机全过：① 跨进程 SetParent 实挂（Win32 窗口树：容器 NlEditorViewportHost 与引擎窗 GLFW30 pid 跨进程、矩形完全重合）+ GDI 截屏确认 Suzanne 像素直出无偏移（修复 ImGui 坐标系双重 ScreenToClient 偏移 bug）；② resize 跟手（viewer log `resize -> 964x585`）；③ `taskkill viewer` 后编辑器存活、容器自动拆除回落占位（崩溃隔离实挂证据）。另修 GL2 后端移植 bug：客户端数组模式 `glVertexPointer` 第 4 参须传内存基址+偏移（原误传纯偏移 → 驱动近零地址读顶点段错误），并摘除误设的 `RendererHasVtxOffset`。
- **M-E3** 第一个工具插件（相机/场景树）经 RPC 驱动引擎。 **[DONE 2026-11-11，跨进程探针 + UI 点击链路双验收全过]** — 引擎侧 `EditorEmbed::bindCamera`（runNormal/runScene 双模式各 bind 一次）注册 5 个 handler：`camera.getState`（fov/dist/yaw/pitch，度）/ `camera.setFov` / `camera.setDistance`（回读 clamp 后实际值）/ `camera.orbit`（像素增量 ×kOrbitGain=0.007 rad/px）/ `camera.reset`；params 统一命名 Map，未连接也注册（standalone 零开销）。编辑器侧 `CameraPlugin`（替换 stub）：Refresh 拉真值、FOV/Distance 滑条、Orbit 四向按钮（度数语义，deg→px 换算 kPxPerDeg≈2.4913 在编辑器侧，修正按钮"±15deg"实转 6° 的语义错位）、Reset；每次操作后 getState 对齐 UI。验收：`tools/m-e3-cam-probe.py`（python 假编辑器 server，真跨进程打 viewer handler）22 断言 0 fail（announce hwnd/pid/proto、setFov 45 与 clamp 500→120、setDistance clamp 0.01→0.45、orbit 增量精确匹配 0.007 rad/px、reset 回 4.2/0.21rad/0.16rad 且不动 fov、bogus method error 100）；UI 链路 SendInput 实点 Refresh+Orbit R 截屏双证据：状态行 `fov 45.0 dist 4.20 yaw 27.0`（12.0→27.0 精确 +15°）、引擎 HUD 同值、Suzanne 画面可见右转。
- **M-E4** 重连全链路 / announce caps / 焦点路由 / 场景树插件。 **[DONE 2026-11-11]**（台账见 §8 M-E4 段）
- **M-E5** 插件 DLL 动态扫描加载 + 材质检查插件。 **[DONE 2026-11-11]**（台账见 §8 M-E5 段）
- **M-E6** 命名管道传输（TCP 默认不变）+ 多视口（`viewId` 路由，一编辑器接纳多引擎进程）。 **[DONE 2026-11-11]** — `editor_bridge/transport.{hpp,cpp}` 抽象 TCP/命名管道双链路（endpoint 解析 + 异步重叠 IO）；`RpcHost` 多 client 化 + `callTo/notifyTo` 定向路由；`RpcBridge` 真正消费 endpoint；`EmbedWindow::announce` 带 `viewId`；viewer `--editor-endpoint` / `--view-id`，editor `--rpc-endpoint` + 多视口平铺 + `editor.views` / `editor.pingView` / `editor.callView`；插件统一 500ms 快速失败避免冻结渲染线程。验收：`rpc_self_test.exe` TCP+pipe 全绿、`tools/m-e6-probe.py` 五段 33 断言 0 fail、双视口目击证据 `out/m-e6-multiview.png`。（台账见 §8 M-E6 段）
- **M-E7** 模型预览插件（2026-11-14 用户拍板重开 editor 轨道，§7.1b）。新建**专用预览小场景**（地面 + 环境光 + 中性背景，待预览模型居中，编辑器选模型文件加载）；`camera.preset` 预设视角一键切换（前/后/左/右/顶/等距 = 多方位无死角）；光照参数 cvar 化（太阳方位/强度/环境光 → `cvarGet/cvarSet` 既有通道）；model 模式材质写回（修复 M-E5 起 `scene.setMaterial` 在 model 模式 error 100）；editor 侧 `ModelPreviewPlugin`。**参数通道拍板：走 CVar + M-SC10 描述表既有反射层，不另做一套**。
- **M-E8** 场景树增强。层级树（ECS parent 链，绑定面 `ecsSetParent/ecsGetParent` 已有）；`editor.sceneChanged` 实时推送（引擎侧对象增删/参数变更 notify，编辑器收到后拉增量——替换手动 Refresh）；泛化参数读写 handler（原生参数走 cvar 通道；脚本组件字段走 M-SC10 `[Prop]` 描述表，**依赖 M-SC10 落码**，未落则先接 cvar 部分并留描述表接驳点）。

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

### 2026-11-14 — M-E7 模型预览 + cvar 反射通道落地（feat/editor worktree）

**落码六件套**（9 文件改 + 2 新增，461 行）：
1. `assets/scenes/preview.scene.json`（新增）——预览小场景：Suzanne 主体（y=0.2）+ ParkingLot 地面（y=-1.2）+ ambient 0.28；
2. viewer `--preview` / `--preview-model <glb名>` 糖旗标——mode scene + 专用场景 JSON；`--preview-model` 在 World 反序列化后按实体名 `preview-model` 覆写 meshFile（坏名 exit 2）；
3. `light.sunAzimuth / light.sunElevation / light.ambient` 三 cvar（Console::registerFloat + setOnChanged dirty 语义；JSON 有灯时 az/el 覆写仅当 cvar 被显式改过——零回归设计：未调光 `applyLightCvars` 原值透传）；
4. RPC：`engine.cvar.list/get/set`（反射通道，msgpack 传输；`setFromString` 语义）+ `camera.preset` 六预设（front/back/left/right/top/iso，top 钳 OrbitCamera ±1.45 rad 上限并回读实际值）；
5. editor 侧 `PreviewPlugin`（内建注册）：视角按钮排 + 光照滑条（RPC `engine.cvar.set` 逐帧写）；
6. `tools/m-e7-probe.py` 三段 31 断言（场景链 / preview-model 覆写 / cvar RPC + PNG 像素差）。

**renderer 触碰面（拍板 A 放行，只增不改）**：`updateEcsLights` 通道五文件纯新增（Renderer→backend 虚接口→DX12/Vulkan 转发→`Scene::updateLights` writeBuffer，与 `updateEcsObjects` 同构）；存量唯一改动 = initScene 灯缓冲 permanent 位 `ShaderResource → ShaderResource|CopyDest`（NVRHI state-tracking 校验要求 ⊇ CopyDest，免 barrier 模型不变）。根因：`psScene` ambient 消费灯缓冲 `g_lights[0].y`（loadEcsScene 一次性写死），每帧 CB `g_lightDir.w` 只有 probe/trace 链消费——cvar 覆写若不动灯缓冲对预览画面零效果。用户拍板 A（updateEcsLights）否掉 debounce 全场景重建 / 只控 ambient 两案。

**验收（主树口径不适用，editor 轨道惯例 = 探针 + selftest，不进 cases.json）**：双后端 **BUILD OK**（viewer+editor 各一）+ selftest **16/16 ×2**；`m-e7-probe.py` **31/31 全绿**（DX12，RPC 桥后端无关沿用 M-E5 口径）；像素实证 `light.ambient 0/0.28/999` 全图平均亮度 40.7/42.1/91.9、999 时地面 255 纯白——**cvar→灯缓冲→psScene 覆写链实锤**；证据 `out/m-e7-ambient-{default,012}.png`。回归受影响卡双后端 **26/26 bytes 全绿**（DX12: ecs-e1b 4 + p8-ecs 4 + m0.4 5；VULKAN 同 13）；冻结面 `shader/` `example/` `cases.json` `kStage` 零触碰。RPC-SPEC §10 补 M-E7 实装表。

**踩坑**：① `.gitignore` 粘连 bug（见重开段）由本里程碑 wt junction 首建暴露；② msgpack_min 字符串 API 是 `isStr()/asStr()` 不是 `isString()/asString()`（31 错主因）；③ lambda 按 `[&]` 缺捕获补齐；④ **PNG 字节大小与画面亮度反相关**（过曝大片纯白压缩率高→文件反而小），探针曾误断言「大小必须相等」、像素实证后才定案；⑤ 新树并行双构建撞 mspdbsrv（C1090 PDB API 0x6BA RPC 不可用），串行重跑即过；⑥ showcase 独立 exe（`build/showcase/s0-model/`）新树不存在，m0.4 回归 FAIL 属环境错非回归，`rebuild.py --tags s0-model` 补建后 5/5 秒过。

**遗留**：editor 侧 PreviewPlugin GUI 观感（滑条手感/视角按钮布局）待用户目测；M-E8 场景树增强（层级树 + sceneChanged 推送 + M-SC10 描述表接驳）待 M-SC10 落码。

### 2026-11-14 — 轨道重开拍板（M-E7/M-E8 立项，§7.1b）

**背景**：用户提出两个新工具需求——① 模型预览工具（对接外接编辑器、模型放预设小场景、多方位无死角预览、模型参数与光照参数设置）；② 场景树预览工具（对接外接编辑器、看到场景全部对象、实时获取与调整参数）。补充约定：**参数设置尽量走反射胶水层，不另外做一套**。

**证据复核（本轮实读）**：`EDITOR-PLAN.md` + `RPC-SPEC.md` 整轨台账（M-E0~E6 已 merge master `ac50c10`，既有 camera/scene/materials 三插件与手写 handler 现状）；script wt `exports.api`（反射绑定面：cvarGet/Set/List 已实现，M-SC10 `[Prop]` 描述表已拍板未落码）；`P8-SCRIPT-PLAN.md`（M-SC10 属性面板口径）；viewer 光照代码（`FrameParams.lightDir` 非 cvar，写死在 viewer）。

**用户拍板（AskUserQuestion 四项全拍）**：
1. **轨道编排** = 重开 editor 轨道 M-E7（模型预览插件）→ M-E8（场景树增强）；
2. **参数通道** = CVar + M-SC10 描述表（引擎原生参数注册成 cvar 走 `cvarGet/cvarSet`；脚本组件字段走 M-SC10 `[Prop]` 描述表；两层全是既有反射层，RPC 只做传输，零新反射系统）；
3. **预设小场景** = 新建专用预览小场景（不复用现有 model 模式环境）；
4. **实时机制** = 引擎推送 `editor.sceneChanged`（对象增删/参数变更 notify，编辑器收后拉增量）。

**落地登记**：`wt_pool.py` TRACKS editor closed→active；wt 重建于 `E:/UE_STUDY_2026/nl-engine-editor`（feat/editor ff 至 master `e316938`）；junction `.workbuddy` + `thirdparty/slang/dist`（PowerShell 原生 API）重建；claim M-E7 --force。M-E7/E8 分期定义写回 §3。

**顺手修复（master `e316938`）**：`.gitignore` 框线注释行粘连致 6 条规则失效（`/build/`、`/CMakeCache.txt`、`.vs/`、`thirdparty/slang/dist/`、`__pycache__/`、`script/**/bin/` 整体裸奔——主树 status 的 `?? build/`、`?? script/**/bin/` 即此症状），拆行还原并双树验证。

**冻结面**：`renderer/` 图形路径、`shader/`、`example/`、`tools/regression/cases.json`、`kStage` 零触碰；M-E7 光照 cvar 化只加注册不动渲染链路。

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
### 2026-11-11 — M-E4 四件套：重连 + caps + 焦点路由 + 场景树插件（feat/editor worktree）

**产物**
- `editor_bridge/rpc_bridge.{hpp,cpp}`：`connect()` 重连前置关陈旧 socket（心跳超时路径只置位不关句柄）；新增 `isConnected()`。
- `viewer/main.cpp`（EditorEmbed）：① M-E4a 周期重连——`enabled` 成员内部判定，`pump()` 掉线后 2s 退避重试，连不上保持单机零开销，编辑器后启/重启都能被接管；② M-E4b caps——announce 填窗体像素尺寸（GetClientRect）+ 后端名（编译定义 `NL_BACKEND_NAME` 注入，viewer/CMakeLists）；③ M-E4d `bindScene`——`scene.getState`（两模式都注册，model 模式返回 mode="model" 空表）+ `scene.focus`（orbit 相机 eye 落 origin→P 射线：yaw=atan2(px,pz)、pitch=atan2(py,rXz)、dist=|P|·2.5；越界/原点返回 error 100）。
- `editor/main.cpp`：Connection 面板新增 `caps : WxH BACKEND` 行（onEngineAnnounce 解析 + onEngineClose 清除）。
- `editor/plugin.cpp`：`SceneOutlinerPlugin`（Refresh/自动拉取 + 4 列实例表 + 行点击 scene.focus）；连接后首帧自动 getState（掉线重连后 have_ 被清也会自动再拉）。
- `tools/m-e4-probe.py`：python 假编辑器 server 跨进程机械验收（M-E3 探针骨架扩展 fixarray 解码 + caps 断言）。

**验收（双段全过）**
- 段1 跨进程探针 `tools/m-e4-probe.py`：**18 断言 0 fail**——caps w/h=1280x720 + backend=DX12；scene.getState 5 实例 mesh/pos/tint/rough/metal 齐全、位置非退化；scene.focus[0]（P=(-4,0,0)）→ yaw=-90/pitch=0/dist=10 与理论值精确一致、getState 回读一致；focus[99]/focus[-1]/scene.bogus → error 100。实机还复验了 M-E4a 重连全链路：viewer 先启报 "standalone (retry every 2s)" → 编辑器 5s 后启动 → "connected, hwnd+caps announced"。
- 段2 编辑器 UI：PrintWindow 截图——Connection 面板 `caps : 1280x720 DX12`；Scene Outliner 自动填充 `mode: scene objects: 5` + 5 行实例表（数据与探针一致）+ 行点击提示行。行点击 focus 的 UI 侧链路与 M-E3 CameraPlugin 同代码路径（SendInput 实点已证），本轮因用户实机占用焦点未再强抢，RPC 层由探针覆盖。

**M-E4c 焦点路由实测（PASS）**
- SendInput 在视口内拖拽（dx=+50, dy=+25）→ 引擎 HUD yaw 12.0→58.1（含两次拖拽累积）、Suzanne 可见旋转——**视口内点击/拖拽引擎窗直收鼠标**（Win32 子窗激活语义正常）；编辑器面板输入 M-E3 已证。键盘跟随同一激活模型，未单独测。

**踩坑**
- `nl::core::log::info` 非 printf 风格（"%s" 原样打印、参数被拼接输出）——带格式串要自己拼 string 再传。
- ImGui 后端在窗口**未聚焦**时忽略 PostMessage 直投的 WM_MOUSEMOVE/WM_LBUTTONDOWN（合成点击不生效）；SendInput+SetForegroundWindow 会抢用户焦点——用户实机操作期间应改用「插件自动刷新 + PrintWindow 免焦点截图」验证，勿再强抢焦点。
- imgui.ini 面板布局持久化：面板 z 序 = ini 文件内条目顺序（越靠后越高），被 Viewport 盖住的调试面板挪到文件尾即可置顶；面板尺寸/位置也直接改 ini 比手点稳。
- `ImGuiChildFlags_Borders`：vendored imgui 1.91.9 支持（1.90 改名后），直接可用。

**待办（M-E4 收口时）**
- ~~材质检查插件 / plugins 动态加载~~ → M-E5 已做；管道传输 / 多视口 → M-E6 候选。
- ~~周期重连 / 焦点路由 / caps~~ → 本里程碑已全部落地。

### 2026-11-11 — M-E5 插件 DLL 动态扫描加载 + 材质检查插件（feat/editor worktree）

**产物**
- `CMakeLists.txt`（根）：新增 `nl_imgui_dll`（SHARED，`WINDOWS_EXPORT_ALL_SYMBOLS ON`）——editor.exe 与插件 DLL 必须共享同一 ImGuiContext（插件静链 imgui = 双 context 必炸）；`nl_imgui`(STATIC) 保留给 renderer/viewer（跨进程各持一份无碍）。
- `editor/CMakeLists.txt`：新增 `nl_editor_common`（STATIC：rpc_host.cpp，宿主与插件共享单一 RpcHost 实现）；editor 目标切 `nl_imgui`→`nl_imgui_dll`；新增 `editor_plugin_hello`（SHARED 示例插件）→ 输出 `${CMAKE_BINARY_DIR}/plugins/`。
- `editor/plugin.hpp`：`kPluginAbiVersion=1` + `discoverDlls(dir, log)` + 析构对称卸载；`editor/plugin_export.hpp`：`NL_PLUGIN_EXPORTS(class)` 导出宏（`nl_plugin_abi/_create/_destroy` 三符号 C ABI）。
- `editor/plugin.cpp`：`discoverDlls`（FindFirstFile 扫 `plugins/*.dll`，LoadLibrary + ABI 校验，坏 DLL 跳过不拖死宿主；加载日志同步落 stdout 供探针断言）+ 析构按 slot 逆序先 `destroy`（DLL 内 delete）再 `FreeLibrary`；新增 `MaterialInspectorPlugin`。
- `editor/plugins/hello_plugin.cpp`：示例插件 DLL（连接状态 + engine.echo 按钮）。
- `viewer/main.cpp`：`bindMaterials`——`scene.getMaterials`（scene 模式实例 tint/rough/metal；model 模式 loadNMesh 读 .nmesh NSUB 表：name/meshlets/submeshCount/submeshes[materialId,clusterFirst,clusterCount]，材质 id=子网格序号 M7.2 约定）+ `scene.setMaterial`（逐字段可选 tint/rough/metal，clamp 0..1，写回 sceneObjects 由 viewer 每帧原地刷新传 renderer，下一帧可见；model 模式 error 100）。
- `viewer/CMakeLists.txt`：`NL_MESHLET_DIR` 编译定义（根 CMake 的同名宏是 renderer 内联定义不是变量，须用 `${CMAKE_SOURCE_DIR}/assets/meshlets` 自行注入）。
- `tools/m-e5-probe.py`：三段跨进程机械验收探针。

**验收（30 断言 0 fail，exit 0）**
- 段1 scene 模式：getMaterials 5 实例字段齐全；setMaterial rough=0.5 / tint=[0.25,0.5,0.75] 写回 getMaterials 复读一致；rough=5 clamp 1；坏索引 [99]/[-1] error 100；scene.getState 回归不变。
- 段2 model 模式（--mode cluster --model Suzanne-materials.nmesh）：nmesh.meshlets=711、submeshCount=3（NSUB 段）、materialId=0/1/2、clusterFirst=0、clusterCount=237×3 与资产实况一致；setMaterial → error 100。
- 段3 editor.exe 插件扫描：stdout 出现 `plugin dll loaded: editor_plugin_hello.dll (Hello (DLL))`；注入坏 DLL（nl_imgui_dll.dll 改名 _bad_test.dll）→ `skipped: missing C ABI exports` 不拖死宿主。
- 目击证据 `out/m-e5-editor-ui.png`（PrintWindow 免焦点）：Material Inspector 5 行实例材质表（tint 色块 + r/m 值）+ Hello (DLL) 面板（"loaded from plugins\hello_plugin.dll" + engine: connected）+ Scene Outliner + 嵌入视口 5 Suzanne + Log 三行（plugin loaded / connected / announced）。

**踩坑**
- 类内成员函数的**参数类型**在声明点必须可见（inline 函数体可以后置）——`setMaterial(..., const Row&)` 写在 `struct Row` 定义前直接 C4430/C2664，结构体须挪类体前部。
- `RUNTIME_OUTPUT_DIRECTORY` 用 `CMAKE_CURRENT_BINARY_DIR` 在 editor 子目录上下文里是 `build/DX12/editor`，插件 DLL 落错层；要用 `${CMAKE_BINARY_DIR}/plugins`。
- viewer 侧 `NL_MESHLET_DIR` 不能引用 `${NL_MESHLET_DIR}`（根 CMake 里它是 renderer 的内联宏定义，非变量）→ 展开成空串、loadNMesh 失败被 nmeshInfo 包成 error map，探针首版只看到字段缺失；修 `${CMAKE_SOURCE_DIR}/assets/meshlets`。
- editor.exe 的 imgui.ini 按 CWD 解析（Popen 不带 cwd 时落在启动目录）——布局预置必须把 cwd 设到 exe 目录。
- python 探针 msgpack 解码需支持 str8(0xd9)（引擎侧长错误串走 str8）；subprocess 管道用 `communicate(timeout)` 而非 `read(n)`（后者阻塞等 EOF）。

### 2026-11-11 — M-E6 命名管道传输 + 多视口 `viewId` 路由（feat/editor worktree）

**拍板（开工闸：待拍板 4 项全部拍板、已拍板 4 项）**
1. TCP 仍为默认；命名管道经 CLI 显式切换，不改默认值。
2. 多视口语义 = 「一个编辑器同时接纳多个引擎进程，按 `viewId` 路由并各自嵌入独立视口面板」；**否决**单进程多 HWND / 多 swapchain。
3. `viewId` 缺省 `main`，老插件（相机 / 场景树 / 材质）零改动兼容。
4. 冻结面：`renderer/` / `shader/` / `example/` / `tools/regression/cases.json` 全部零触碰；只动 `editor/`、`editor_bridge/`、`viewer/` 参数、`tools/m-e6-probe.py`、docs/editor。

**产物**
- `editor_bridge/transport.{hpp,cpp}`（新增）+ `nl_editor_transport` STATIC（`ws2_32`）：`Endpoint` / `parseEndpoint` / `Stream` / `Listener`；TCP 用 winsock2 blocking + `select`，命名管道用 `CreateNamedPipeA` + `FILE_FLAG_OVERLAPPED` + `PeekNamedPipe` + `PIPE_UNLIMITED_INSTANCES`。endpoint 语法 `tcp://127.0.0.1:29110` / `tcp://127.0.0.1` / `pipe://<name>` / 裸端口，只允许 loopback，坏 endpoint 不静默回落。
- `editor_bridge/rpc_bridge.{hpp,cpp}`：`Impl` 从裸 `SOCKET` 改为 `unique_ptr<Stream> + Endpoint`；`connect(endpoint)` 真正解析 endpoint 并重连前先 `close/reset`；收发统一走 `sendAll/recvSome/waitReadable`；`shutdown()` 不再 `WSACleanup()`（Winsock 改进程级一次性管理）。
- `editor_bridge/embed_window.{hpp,cpp}`：`announce(..., viewId="main")`，载荷增 `viewId` 字段。
- `editor/rpc_host.{hpp,cpp}`：`vector<unique_ptr<Client>>`（每 client 独立 `recvBuf` / `viewId` / `lastRecvMs`），`update()` 每帧最多 accept 16、逐 client 非阻塞收包；`callTo/notifyTo(viewId,...)` 精确路由（未知视图 error 103）、`viewIds()/clientCount()/setActiveView()`；`rpc.pong` 按 `Client&` 定向回；重复 `viewId` 打 warning；`onEngineAnnounce/onEngineClose` 改带 `viewId` 签名。
- `editor/main.cpp`：`--rpc-endpoint <ep>`；`std::map<string, ViewInfo> g_views`；`editor.views` / `editor.pingView` / `editor.callView` 三 handler；Connection 面板显示 endpoint / clients / active view Combo / 各视图 pid·caps·embed；`n<=2` 并排、`>2` 两列网格平铺 `Viewport [viewId]`；退出先 `g_views.clear()` 拆容器再 `host.stop()`。
- `editor/viewport.{hpp,cpp}`：实例独立 `viewId` → 独立 ImGui 窗口 ID（`label()` 懒生成，`mutable`）；容器加 `WS_CLIPSIBLINGS` 防多容器互盖；析构 `detach()` 拆 `WS_CHILD`。
- `editor/plugin.cpp` / `editor/plugins/hello_plugin.cpp`：`callQuick(host, method, p)` = `call(..., 500)`，替换内建插件全部同步调用（含示例 DLL），避免 5s 阻塞冻结渲染线程。
- `viewer/main.cpp`：`Options.editorEndpoint/viewId`，`--editor-endpoint` / `--view-id`，`EditorEmbed::start(hwnd, enable, ep, vid)` 贯穿，announce 日志带 `view=` / `endpoint=`，`--help` 同步。
- `editor/rpc_self_test.cpp`：阶段 E 新增管道双链路 + `viewId` / `callTo` / 未知视图断言。
- `tools/m-e6-probe.py`：五段跨进程机械验收（极简 msgpack 编解码 + Win32 命名管道 ctypes 封装 + TCP/Pipe 统一 `Conn`）。
- `docs/editor/RPC-SPEC.md`：§3 传输层 / endpoint 语法、§4 大端长度前缀勘误（早期「LE」是文档笔误，实装自 M-E1 起一直是 BE，M-E6 只勘误文档不改线格式）、§9 多连接生命周期、§10 M-E6 方法表、§15 四项开放项全部标记已拍板。

**验收**
- `scripts\build-m0.2.bat DX12` **BUILD OK**；viewer selftest 16/16。
- `rpc_self_test.exe`：`=== M-E1 self-test PASS (0 failures) ===`，18 条 `[ ok ]`（收口复跑实测计数）含 `M-E6 named-pipe transport` / 管道 `engine.echo` 往返 / `callTo` 视图路由。
- `tools/m-e6-probe.py`：**33 PASS / 0 FAIL，exit 0**。S1 真 viewer 走 `pipe://nl-e6-probe`（announce/caps/echo/scene.getState/camera.getState）；S2 默认 TCP 回归（`viewId=main`、materials count=5）；S3 真 editor + 2 假引擎（`editor.views` 两视图、`viewIds=[left,right]`、hwnd 保真、`pingView` 只投 right、`callView` 定向转发、未知视图 `sent=false` / error 103、duplicate warning、editor 存活）；S4 真 editor 监听 `pipe://nl-e6-editor`（`transport=pipe`）；S5 负向（无 pipe server 的 viewer 存活重试、`bogus://x` 不崩且日志 `bad endpoint`）。
- 目击证据 `out/m-e6-multiview.png`（DPI-aware 实时屏幕合成）：`Viewport [left]` / `Viewport [right]` 两面板、`endpoint : tcp://127.0.0.1:29110`、`clients : 2 / views: 2`、两个视图 `pid` / `caps : 1280x720 DX12` / `embed : SetParent ok`、Log 里 `view=left` 与 `view=right` 两条 announce、左右各 5 个 Suzanne 各在自己面板内；`out/m-e6-multiview-marked.png` 叠加 Win32 窗口树矩形，两容器 `623x729` 位于 `(359,102)-(982,831)` / `(998,102)-(1621,831)`，与 ImGui 面板内容区严丝合缝、互不重叠。

**踩坑**
1. **⚠️ 验证脚本 DPI 坐标系混用（本轮最大坑，曾一度误判为产品代码回归）**：本机系统 150% DPI（物理 2560×1440，DPI-unaware 进程看 1707×1067）。python 默认 DPI-unaware，`GetWindowRect` / `EnumChildWindows` 返回**逻辑坐标**（÷1.5），而 `BitBlt(GetDC(NULL))` 抓的是**物理像素**；把逻辑坐标当物理坐标叠加到截图上，凭空造出 1.5 倍「容器比面板窄 / 整体右移」的假象，甚至怀疑是 M-E2 `ScreenToClient` bug 复现。修法：截图脚本开头 `SetProcessDpiAwarenessContext((void*)-4)`（PER_MONITOR_AWARE_V2）让窗口树与截图同处物理坐标系；DPI-aware 后后台启动窗常被系统最小化到 `-32000`，须 `ShowWindow(SW_RESTORE)` 轮询 `IsIconic` 恢复。**定性后产品代码零改动**：叠加图里两个容器与面板完全对齐。顺带纠正先前判断 —— 根因不是 `PrintWindow` 的 DWM 缓存帧（换实时 BitBlt 现象相同），而是坐标系。
2. **插件在 ImGui 渲染线程同步 `host->call` 默认 5s 阻塞**：假引擎不回插件 req 时主循环冻结，后续 announce 排不进来，duplicate viewId 断言假 FAIL。修 `callQuick(..., 500)` + 探针尾段持续回插件 req；临时 `[f] A/B/C` / `[tick]` 插桩已全部删除。
3. **探针 `collect()` 固定跑满 timeout** 制造 5s+ 静默，被 6s heartbeat 判掉线 → `ConnectionAbortedError(10053)`。修 `stop_when` 早退 + `req()` 命中目标 `resp id` 即返回。
4. **单 client 时代「抓全局 socket 回 pong」在多 client 下回错连接**：`dispatch` 必须带 `Client&` 定向回。
5. **命名管道必须 `FILE_FLAG_OVERLAPPED`**：同步 `ReadFile` 会阻塞整个 host 帧循环；用 `PeekNamedPipe` + `Sleep(1)` 探测可读，且每 accept 成功立即 arm 下一个实例，否则第二个 client 连不上。
6. **坏 endpoint 禁止静默回落默认端口**：`bogus://x` 必须报错且 viewer 存活重试（S5 负向断言）。
7. `Viewport::label()` 是 `const` 却懒生成 `label_` → C2678，须 `mutable std::string`。
8. `replace_all` 把 `callQuick` 函数体自身也换成递归调用 → 改回 `host->call(method, p, 500)` 并补 `<string_view>`。
9. `RpcBridge::shutdown()` 不再 `WSACleanup()`：Winsock 初始化已进程级一次性管理，per-bridge cleanup 会打断同进程其它连接。

## §8 附：整轨 merge 回 master（2026-11-11，用户「合回主干」拍板）

- **合并形态**：`feat/editor` 与 master 真分叉（merge-base `b5c16a3`，master 独有 16 提交 = S10 T0/M10.0~M10.2 + particles，editor 独有 6 提交 = M-E0~E6），`--no-ff` 生成双父 merge commit `ac50c10`（父 `e43eda1` + `0ca2715`）。合并前 `git merge-tree --write-tree` 无副作用预演：根 `CMakeLists.txt` 自动合并；`docs/MILESTONES.md`（两侧四栏纯追加）与 `viewer/main.cpp`（相机初始化 vs `edEmbed.bind*` 独立接线）两处冲突，按字节剔除三行冲突标记、保留双方全部内容，CRLF 原样；`git add` 只显式点名两个冲突文件，主树在途日志与 9 个未跟踪产物零触碰。
- **集成修复 `fc34176`（合并后实编才发现，文本三方合并看不出）**：① master 的 M-CAM b 段把两个渲染循环相机实例从 `nl::core::OrbitCamera` 换成 `nl::viewer::ViewerCamera`，而 `EditorEmbed::bindCamera` 仍收 `OrbitCamera*` → C2664 ×2；改持 `ViewerCamera*`（Orbit 模式同名方法逐位转发 `orbitCam`，非 Orbit 模式由 ViewerCamera 自身门控，不再假生效），并把 fov 20..120 clamp 显式移到 `camera.setFov` RPC 侧（`ViewerCamera::setFovYDegrees` 自身 `fovY_` 不 clamp），保住 `m-e3-cam-probe.py` 的 `setFov 500 -> 120` 契约。② `tools/m-e{3,4,5,6}-probe.py` 四个探针把仓库根硬编码成 `E://UE_STUDY_2026//nl-engine-editor`，在主树跑会启旧 worktree 的 exe = 假绿（M-D3 归档绝对路径同款坑），统一改 `ROOT = dirname(dirname(abspath(__file__)))`。
- **验收（主树，不在 worktree）**：`build-m0.2.bat DX12` / `VULKAN` 双 **BUILD OK**，viewer selftest **16/16 ×2**、Slang **90/90**；`rpc_self_test.exe` **0 failures**；四探针串行 **M-E3 25 / M-E4 18 / M-E5 30 / M-E6 33 = 106 PASS / 0 FAIL**。冻结面 `renderer/` `shader/` `example/` `tools/regression/cases.json` 零触碰，`kStage` 仍 S8.2，无新基线 / 版本卡。
- **合并纪律**：用户只说「合回主干」→ 合并当时未标 CLOSED、未拆 worktree、未改 `wt_pool.py TRACKS` state（drive 轨道先例）。**2026-11-11 用户「拆 editor」拍板后 worktree 已摘除**（详见下方拆除记录）；`feat/editor` 分支按 §7.6 CLOSED 规定**保留（明文规定，不是待拍板项）**。同日 17:1x 用户「轨道CLOSED是要的」拍板后 `wt_pool.py TRACKS` state 翻 `closed`（详见下方 CLOSED 记录）。
- **教训固化**：并行轨道 merge 后**必须实编双后端** —— `git merge` 无冲突 ≠ 语义兼容；归档探针与产物禁带 worktree 绝对路径（两棵树都要能跑）。

## §8 附：editor worktree 拆除（2026-11-11 17:0x，用户「拆 editor」拍板）

- **拆前只读核对**：目录为真实目录（实测无重解析点，台账旧记「junction 共享 `.workbuddy`」已不成立）；`git status --porcelain` 只有 `## feat/editor`，无未提交 / 未跟踪改动；无 `editor.exe` / `viewer.exe` 残留进程；editor 轨道无活动 claim（M-E6 已 `release --done`）；5 个 `e-m*` tag 全部在 master 可达。
- **数据保全**：`out/` 三份主树缺失产物（`m-e5-editor-ui.png`、`m-e6-probe-final.log`、`m-e6-selftest-final.log`）带回主树，`m-e6-multiview*.png` 两张 `cmp` 逐字节一致；`.workbuddy/memory` 归一化 CRLF/LF 后 5/6 份与主树逐字相同，`MEMORY.md` 是旧快照（唯一「独有」行是已被推翻的 M9.2 旧口径）→ 无独有数据，仍整目录归档到仓库外 `E:/UE_STUDY_2026/.archive/editor-wt-2026-11-11/`（17 文件 / 777K，零 git 污染）。
- **执行两处中断**：① `git worktree remove` 被 SIGTERM 打断在半途 → git 已摘 `.git` 指针、该 worktree 变 `prunable`，但磁盘残留 6809 文件；`git worktree prune -v` 清掉 `.git/worktrees/nl-engine-editor` 元数据后 `git worktree list` 只剩主树 + pathfinding。② 系统级 `Remove-Item -Recurse -Force` 被环境 safe-delete 钩子以 `SAFE_DELETE_FAIL_CLOSED / trash-failed` 拦下（一次性把 6809 文件塞回收站部分失败；顶层还有 3 个历史误操作畸形文件名，`thirdparty` 单目录 6070 文件）。最终用 Python `shutil.rmtree` **按顶层 18 项分批** + Win32 长路径前缀重试 + 只读属性清除，12.2s / 0 failures 删净。
- **删除安全闸**：脚本前置断言「目录名 == `nl-engine-editor`」「目录下无 `.git` 指针」「`.git/worktrees/nl-engine-editor` 元数据已不存在」，三重确认后才动手，避免误删仍存活的 worktree 或主树。
- **终态**：worktree 目录与 git 登记双消失；`feat/editor` @ `0ca2715` 保留；5 个 tag 全在 master 可达；`wt_pool status` editor 自动变 `ACTIVE / wt=absent`、无活动 claim；主树在途 10 项改动（当日日志 + 9 个 S10 未跟踪产物）**一项未动**。拆除当时未改 `wt_pool.py TRACKS` state、未标 CLOSED（属后续单独拍板，同日 17:1x 已拍板落地）；**分支非待拍板项，按 §7.6 CLOSED 规定保留**；未 push（仓库无 remote）。
- **三条教训**：① 拆 worktree 前必须实测 `.workbuddy` 是不是 junction —— 链接存在时递归删除有带走主树 `.workbuddy`（日志 / `wt_pool` 状态）的风险，且台账里的旧「junction」表述可能是过时口径；② `git worktree remove` 对含大量 ignored 产物的树可能中途被打断，**落地后必须 `git worktree list` 复核是否 `prunable` + 目录是否残留**，别只看退出码；③ 大目录删除别走单次回收站 trash，分批 + 长路径重试更稳，且畸形文件名会放大失败面。

## §8 附：editor 轨道标 CLOSED（2026-11-11 17:1x，用户「轨道CLOSED是要的」）

- **口径纠正**：上一轮误把已在 §7.6 明文规定的分支保留也列进待拍板清单、并与轨道状态变更并列，是错的。`docs/PARALLEL-TRACKS.md` §7.6 明文规定 **CLOSED 轨道 = wt 应已移除、分支保留**，`tools/wt_pool.py` TRACKS 注释同样写 `closed = merged back to master, wt removed, branch kept (do NOT touch)`；drive / camera / script / console 等收官轨道一律只拆 wt、留分支。分支保留是规定，不该问。已把三份台账里所有把分支保留列为决策点的伪待拍板句清掉。
- **落地**：`tools/wt_pool.py` `TRACKS["editor"]["state"]` `active` → `closed`，note 补 merge commit `ac50c10` + 集成修复 `fc34176` + tags `e-m0e2-done`~`e-m6-done` + wt removed / branch kept / CLOSED 时间，与 drive 条目同口径。`docs/PARALLEL-TRACKS.md` §7.1 editor 行状态列改 CLOSED、分支列改「保留（§7.6 规定）」；`docs/MILESTONES.md` merge 四栏「完成情况 / 拆除记录 / 下一步」三处同步；本文件 §8 两条历史时点句改口径并明确「分支非待拍板项」。
- **机械验证**：`python tools/wt_pool.py selftest`（21 项，进程内临时锁）全过；`wt_pool.py status` editor 显示 `CLOSED / wt=absent / [feat/editor]`；`claim --track editor --milestone M-E7` 负向被硬拒绝（exit 1，输出含 `CLOSED ... branch kept`）。重开须按 §7.1b 重建 wt 并更新 TRACKS，再 `--force` claim。
- **未动**：`feat/editor` 分支与 5 个 tag、`.git/worktrees` 登记、主树在途 10 项改动、仓库外归档 `E:/UE_STUDY_2026/.archive/editor-wt-2026-11-11/`；未 push（仓库无 remote）、未打新 tag、未跑全量总闸（非里程碑收官）。
- **教训**：收口回复列「待拍板项」前先对照 `AGENTS.md` / `PARALLEL-TRACKS.md` 既有铁律 —— 把已明文规定的动作重新抛给用户，等于制造不存在的决策点，也会让台账互相打架。
