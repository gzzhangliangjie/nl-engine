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

- **M-E0** 编辑器外壳骨架：独立 C++ 程序 + ImGui 主窗 + 可停靠视口框 + 最小插件加载器（无引擎也可启动）。
- **M-E1** 最小 RPC 通路：`RpcHost`(server) + `RpcBridge`(client) + 握手/心跳/echo 双向跑通（含自研 msgpack 编解码 + TCP 分帧）。
- **M-E2** 视口嵌入：`SetParent` 挂引擎 HWND + 随框尺寸同步 + 几何 watcher。
- **M-E3** 第一个工具插件（相机/场景树）经 RPC 驱动引擎。

---

## 4. 目录结构（claim 后落地于 `feat/editor` worktree）

```
editor/                        # 编辑器宿主进程（C++ / ImGui）
  CMakeLists.txt
  main.cpp                     # 入口：建窗 + ImGui + 泵 update()
  rpc_host.hpp / .cpp          # RpcHost（Server 角色，监听引擎 client）
  msgpack_min.hpp              # 自研极简 msgpack 编解码（零依赖）
  viewport.hpp / .cpp          # ImGui 视口框 + SetParent 嵌入控件
  plugin.hpp                   # 插件扩展点 / Service 基类（隔离原则）
  plugins/                     # 工具插件（M-E3 起）
editor_bridge/                 # 引擎侧 bridge（新增，不碰既有 renderer 图形路径）
  CMakeLists.txt
  rpc_bridge.hpp / .cpp        # RpcBridge（Client 角色）+ update() 泵
  embed_window.hpp / .cpp      # 接收父 HWND / 上报自身 HWND
```

引擎侧 `editor_bridge/` 与渲染主线 `renderer/` 解耦，仅经 `update()` 在主循环每帧泵；不改动 `renderer/` 任何图形 API 路径。

---

## 5. 冻结面（不碰）

- 不碰渲染主线 S*/M* 编号与 `kStage`；
- 不触发 AGENTS.md 三源对照规则（编辑器轨道与渲染源码正交）；
- 不引入新 vendored 依赖（除已确认存在的 imgui 系 / glfw，均已在 `thirdparty/`）；
- 引擎侧 `editor_bridge/` 只新增、不改既有 `renderer/` 图形路径。

---

## 6. 待拍板（非架构，调参项，M-E1 落地前按一句话定）

- [ ] 端口固定 `29110` 还是随机 + 命令行/文件通知？
- [ ] 命名管道作默认（Windows 专属）还是 TCP 默认、管道可选？
- [ ] `SetParent` 由编辑器主动挂（推荐，对启动顺序更鲁棒）还是引擎带 `--parent-hwnd` 自建子窗？
- [ ] 是否支持「多视口」（同一引擎多个嵌入窗）——影响 `engine.announce` 的 hwnd 结构。

---

## 7. 验收口径（对齐既有 tools 轨道）

- 每里程碑产出可读证据（启动截图 / RPC echo 日志 / 嵌入画面）；
- 不进渲染 `bytes/near` 总闸；
- 收尾按 PARALLEL-TRACKS §7.6 双向自查 + 同步主树注册表 + merge 回 master（分支与 tag 留档）。
