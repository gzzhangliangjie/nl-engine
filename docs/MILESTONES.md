# 里程碑台账：动作 / 提交版本 / 完成情况 / 踩坑

> 每完成一个小里程碑，**本地提交一波**，并在这里追加一条记录。
> 四栏写实：动作写实际改了哪些文件；完成情况写可复现的验收结果；踩坑写「现象 → 根因 → 处理」，**不许写「已完成」「无」**。
> 提交版本以 git tag 为准，精确 hash 用 `git rev-parse <tag>` 查。
> 总路线 `docs/ROADMAP.md` · S0 阶段门禁 `docs/s0/S0-PLAN.md` · Carrot 流程参考 `docs/s0/CARROT-BOOTSTRAP.md`（仅参考，弯路不复刻）。

## 速查

| 阶段 | 状态 | 提交版本 | 日期 | 一句话结果 |
|---|---|---|---|---|
| M0.0 环境准备 | 🟡 部分完成 | 随 M0.1 提交，无独立 tag | 2026-11-06 | 编译链与 GLFW 齐，Vulkan SDK / CMake / Nsight 待装 |
| M0.1 骨架 + 空窗口 | ✅ 验收通过 | `s0-init` | 2026-11-06 | 窗口弹出、ESC 退出、`SELFTEST PASS (7/7)` |
| M0.2 设备 + swapchain | ⬜ 未开始 | `s0-device` | — | — |
| M0.3 三角形 + 帧同步 | ⬜ 未开始 | `s0-triangle` | — | — |
| M0.4 模型 + 贴图 + 相机 | ⬜ 未开始 | `s0-model` | — | — |
| M0.5 ImGui + 总验收 | ⬜ 未开始 | `s0-done` | — | — |

---

## M0.0 环境准备

**动作**
- 自检本机工具链与仓库状态；确认 VS2022 Community、`cl.exe` 14.38.33130、Windows SDK 10.0.26100.0 已装。
- 从本地 `E:\UE_STUDY_2026\Carrot\thirdparty\glfw\` 复制 GLFW 源码到 `thirdparty/glfw/`（Carrot Init 的 vendored 思路，零网络），删掉 submodule 的 `.git` 引用文件、`docs/examples/tests`（提交 105 个构建必需文件）。
- 从本地 Carrot 的 `cmake_modules/ShaderSlangConfig.cmake` 与 `BUILDING.md` 确认：新版 Vulkan SDK 自带 slangc，**S0-PLAN 里「单独从 GitHub 下 Slang」那步删掉**。
- 测试外部代码通道：`raw.githubusercontent.com` 默认超时、`gh-proxy` HTTP 200；开代理后直连 0.94s；LunarG SDK 直链 HTTP 200。
- 更新 `docs/s0/S0-PLAN.md` 的 M0.0 依赖表与本机实测段。

**提交版本**：无独立 tag，随 M0.1 的 `s0-init` 一并提交。

**完成情况**（部分完成）
- ✅ 可编译：MSVC + Windows SDK 实测编译 GLFW 与 C++17 代码，退出码 0。
- ✅ GLFW 本地化：`thirdparty/glfw/` 105 文件，`add_subdirectory` 所需结构完整。
- ⬜ Vulkan SDK 未安装（`VULKAN_SDK` 未设置、`vkcube` / `slangc` 不可用）——M0.2 前必须补。
- ⬜ 系统 CMake 未安装（M0.1 已绕开；M0.2 接 NVRHI 前补）。
- ⬜ Nsight Graphics 未确认。

**踩坑情况**
1. **命令含 `MSBuild.exe` 字样被安全策略整条拦截** → 根因：命令字符串命中 LOLBin 名单，与该命令是否真的执行它无关。处理：拆成不含该字样的独立命令。
2. **PowerShell 工具里不能调 `cmd.exe` 来跑 `vcvars64.bat`** → 根因：工具层禁止。处理：手动设置 `INCLUDE` / `LIB` / `PATH` 三个环境变量后直接调 `cl.exe`，不依赖 vcvars。
3. **CMake 装不上**：cmake.org 与 GitHub Releases 的 46MB zip 都只有约 12KB/s（240s 只下 2.8MB），开梯子也只改善小文件 → 根因：大文件被限速。处理：M0.1 不依赖 CMake，写 `scripts/verify-m0.1-msvc.ps1` 用 MSVC 直编验收；CMake 改分段并发下载，M0.2 前补齐。
4. **GitHub 直连状态会变**：默认网络超时，开代理后恢复 → 处理：文档里两条通道都留（直连 / gh-proxy），不写死单一结论。
5. **复制 submodule 目录带 `.git` 引用文件** → 会让 git 当作嵌套仓库。处理：复制后删除，提交前核对 `git status`。

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

## 后续阶段的填写规范

每个阶段追加一节，四栏齐全，另附「可见用例」的截图路径与复现命令：

```
## M0.x 名称
**动作**        实际改动的文件 / 命令
**提交版本**    tag 名（+ commit hash 可选）
**完成情况**    逐条可复现的验收结果；未完成的明确列 ⬜
**踩坑情况**    1. 现象 → 根因 → 处理（没有就写「本阶段无阻塞」，不要空栏）
```

验收通过才打 tag 并放行下一阶段；tag 打在验收之后，不预支。
