# 崩溃分析系统（Crash Capture）计划与拍板记录

> 归属：关联系统（非 nanite/lumen 渲染主线），薄封装进 `nl/crash/`，落 `nl_core`。
> 状态：**已拍板（2026-11-13），可落码**。
> 触发：`viewer` / `nanite-tools` 反复遇到 `0xC0000409`（ArmouryCrate 生态）/ `0xC0000374`（堆损坏）等崩溃，回归/展示台只看退出码无第一现场。用户原话「上报感觉没必要 本地都有文件 直接开就好了」。

## 0. 三处参考实读（2026-11-13）
- **UE 5.8.4**：`Core/Private/Windows/WindowsPlatformCrashContext.cpp/.h` 存在 → 工业终态 = 自研 SEH handler + `dbghelp::MiniDumpWriteDump` 生成 minidump（+ CrashReportClient 上传，不抄）。
- **Carrot**：`grep crash|minidump|exception filter` 零命中 → 从零小引擎无专门崩溃报告系统（裸 assert/abort）。
- **bevy**：仅 `panic_handler.rs`（Rust panic hook + backtrace），无 native minidump 捕获。
- **结论**：native 崩溃捕获工业标准底层 = **SEH + dbghelp `MiniDumpWriteDump`**，UE 同款，**零第三方库依赖**。

## 1. 拍板结论（用户 2026-11-13 三轮拍板）
1. **范围**：本地 `.dmp` + `.json` 元数据落盘到固定目录，**无上报、无归集、无服务端**。需要时 WinDbg/VS 加载 dmp + 同版本 PDB 直接开。
2. **路线**：**Windows 原生 dbghelp 薄封装**（`SetUnhandledExceptionFilter` + `MiniDumpWriteDump`），零第三方依赖，命中选型闸硬闸 3。不引 Crashpad（BSD-3，但需 crashpad_handler 子进程 + HTTP + mini_chromium，违反零依赖）。
3. **上报目标**：本地中心目录归集即可，不建任何服务端。

## 2. 设计与模块边界
- 落点：`core/src/crash.cpp` + `core/include/nl/core/crash.hpp`，加入 `nl_core` STATIC 库；`viewer` / `nanite-tools` 已 `PRIVATE` 链 `nl_core`，自动继承。
- API：`nl::crash::install()`（main 第一行调用，幂等）；`nl::crash::trigger_test_crash()`（验收专用，故意解引用 null）。
- 捕获面：`SetUnhandledExceptionFilter` + `std::set_terminate` + `_set_purecall_handler` + `_set_invalid_parameter_handler`。
- 落盘：`exe同目录/crashdumps/<exe>_<时间戳>_<异常码>.dmp` + 同名 `.json`。
- 元数据（手拼 ASCII，零堆分配）：exe 名/路径、git sha（编译期注入 `NL_BUILD_GIT_SHA`，CMake configure 时 `git rev-parse HEAD`）、构建时间（`NL_BUILD_TIME` = `__DATE__ __TIME__`）、命令行、异常码、异常地址、线程 id、本地时间戳。
- **正确性硬约束**：崩溃上下文里堆可能已坏 → 落盘路径只用 Win32 API（`CreateFileW`/`WriteFile`）+ 栈缓冲 + `snprintf`（栈），**禁用 `std::string`/`std::ofstream`/任何 CRT 堆分配**；先写元数据（轻量必成）再尝试写 dmp；`MiniDumpWriteDump` 在 `install()` 时 `LoadLibraryW("dbghelp.dll")` + `GetProcAddress` 预取，避免崩溃时加载。
- dump 类型：`MiniDumpWithDataSegs | MiniDumpWithProcessThreadData`（标准 mini dump，含调用栈 + 模块列表 + 堆段）。栈溢出（`EXCEPTION_STACK_OVERFLOW`）仅尽力，不保证。

## 3. 注册点
- `viewer/main.cpp:4025` `int main` 第一行：`#include "nl/core/crash.hpp"` + `nl::crash::install();`
- `nanite-tools/main.cpp:3798` 同上。
- 两处均在 `install()` 后立即检查 `--crash-test` 参数 → 调 `trigger_test_crash()` 用于验收（无 GPU 依赖，置于任何 GPU 初始化之前）。

## 4. 验收判据
1. `nanite-tools --crash-test`：进程非零退出，`crashdumps/` 下生成 `.dmp` + `.json`。
2. WinDbg 加载 `.dmp` + 同版本 `.pdb` 能解析出崩溃栈帧（行号）。
3. 现有回归/展示台：崩溃时自动留证（文档化，不强制接入）。
4. 不破坏现有构建/基线：双后端重建 + 受影响用例 `regression.py --only` 小子集。

## 5. 冻结面
- 不碰 nanite/lumen 渲染主线、不改 shader、不改现有 CMake target 结构（仅向 `nl_core` 增 1 源文件 + 1 编译宏）。
- 不改 `--check` / `kStage` / example 基线。

## 6. 里程碑承载
主线小里程碑，tag 建议 `crash-m0`（不占 S* 大版本，不新增 track、不 claim，主线收口口径）。收口 = 上述验收 + 四栏台账 + 全量总闸（仅 tag 时一次）。

## 7. 落盘后如何分析（符号化 + 自动出栈，不开 VS）
dmp 落盘只是第一现场留证，真正"分析"是把 minidump 符号化成调用栈。本项目构建**本来就吐 PDB**（`build/<backend>/*.pdb`，与 exe 同目录），符号自动命中，无需符号服务器。

### 7.1 三层分析
- **L1 元数据（零工具）**：读同名 `.json` → 直接分类崩溃（异常码 / git_sha / build_time / command_line / exception_address / thread_id）。常见码：`0xC0000005` 访问违例、`0xC0000374` 堆损坏、`0xC0000409` 快速失败、`0xC00000FD` 栈溢出、`0xC0000094` 除零。
- **L2 符号化调用栈**：调试器引擎读取 `.dmp` + 同版本 `.pdb` → `.ecxr;!analyze -v;k` 出崩溃栈帧 + root cause。
- **L3 锁定二进制**：`.json` 的 `git_sha` 钉死提交；若无当时 PDB，`git checkout <sha>` 重建即得逐字节一致 PDB（本项目构建确定性强）。

### 7.2 调试器引擎（2026-11-13 已装）
- **WinDbg Preview 1.2611.4001.0 已安装**（winget `Microsoft.WinDbg` / Store，用户级）。别名：`%LOCALAPPDATA%\Microsoft\WindowsApps\WinDbgX.exe`，支持 `-z / -y / -c / -logo` 无头命令行 → `analyze-crash.py` 已纳入探测（优先级 cdb > windbg 经典 > WinDbgX）。
- 可选补装经典 cdb（纯控制台、无 GUI 闪窗，无头更稳）：VS 安装器 → 修改 → 单个组件 → 勾选 **Windows Debugging Tools** → `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`。
- 沙箱会话拦截 GUI 子进程启动（安全策略），故 L2 全链路由用户在正常终端跑 `tools\analyze-crash.bat` 完成。

### 7.3 自动分析脚本（已就绪，无需开 VS）
`tools/analyze-crash.py`（纯标准库）+ `tools/analyze-crash.bat` 封装：
- 自动定位 `cdb.exe`（优先）/ `windbg.exe`，自动配置符号路径 = `build/<backend>/*.pdb` + Microsoft 公共符号服务器。
- 执行 `.ecxr;!analyze -v;k 30;lm`，把完整分析写入 `<dump>.analysis.txt` 并打印。
- 用法：
  - `tools\analyze-crash.bat` → 分析 `build/**/crashdumps` 里最新的 `.dmp`
  - `tools\analyze-crash.bat 2` → 分析第 2 新的 dump（`--list` 带序号，1 = 最新，按 dump mtime 新→旧）
  - `tools\analyze-crash.bat path\to\x.dmp` → 分析指定 dump
  - `python tools/analyze-crash.py --list` → 列所有 dump（带序号）
  - `python tools/analyze-crash.py --json x.dmp` → 仅打印 L1 元数据（不需调试器）
- 引擎未安装时脚本会明确提示去装哪个组件，装好后重跑即出栈。
- 实测：L1（`--list` / `--json`）已对真实 dump 跑通（见 `nanite-tools_20261113_115312_C0000005`）；L2 符号化链路就绪，待用户终端首跑确认（沙箱禁拉 GUI 进程）。

### 7.4 边界
- mini dump 足够事后调用栈分析；要"挖第一现场写坏点"（尤其 `0xC0000374` 堆损坏）可把 `crash.cpp` 的 dump 类型改为 `MiniDumpWithFullMemory`（一行，文件变大）。
- **GPU 侧错误**（device removed / D3D debug layer）不在 CPU SEH dump 内，仍靠 debug layer / NVRHI validation，与 `ASAN-DIAGNOSIS.md` 口径一致。本系统专治 CPU 侧 fault（`0xC0000409`/`0xC0000374`/`0xC0000005`），正是项目反复踩的。
