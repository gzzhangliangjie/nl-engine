# P8-Script 工具指南 —— nl-script（M-SC0 hosting 内核）

> 归档：feat/script worktree，M-SC0 收口段（2026-11-09，tag `m-sc0`）。
> 工具 = C# hosting 内核（`nl::script::Vm`）的自测与演示入口，A 类并行轨道
> （不进 kStage / 渲染回归，工具版交代）。计划：`docs/p8/P8-SCRIPT-PLAN.md`。

## 工具定位

`nl-script.exe`（源码 `nl-script/main.cpp`，只链 nl_core + vendored nethost 导入库）
是脚本轨道的工具面：把「hostfxr 装载运行时 → 加载托管 DLL → 调默认签名入口 →
异常边界捕获」整条链变成可机械验收的命令行用例。

## 命令与退出码

退出码约定（与展示台 expectExit 0/1/2 对齐）：

- `0` = PASS / 脚本运行成功
- `1` = 断言失败 / dotnet build 失败 / 托管异常（SEH 边界捕获转 Result）
- `2` = 用法错误（未知子命令 / 缺参数）

| 命令 | 说明 |
|---|---|
| `nl-script check <projectDir>` | 只编译（`dotnet build`），不装载运行时。 |
| `nl-script run <projectDir> [--args <s>] [--type <ns.Class, Assembly>]` | 编译 + 装载 + 调 `Entry`，打印 `run: ret=<n>`。 |
| `nl-script selftest <projectDir>` | 全量机械断言（见下）。 |

## selftest 判据（M-SC0 验收）

- **s0** 定位 build 产物（`bin/Debug/net8.0/*.runtimeconfig.json` + 同名 `.dll`）
- **s1** `Vm::create` 装载运行时（hostfxr 三导出官方口径）
- **s2** `getEntryPoint` 默认签名入口（C# `[UnmanagedCallersOnly]` +
  C++ `UNMANAGEDCALLERSONLY_METHOD`，签名 `int(void*, int32)`）
- **s3** 返回值锚：`"ok"→42` / 空→`-1` / `"echo"→0`（script-api Version 进 stdout）
  / 未知 mode→`-1`
- **s4** 托管异常在互操作边界 SEH 捕获（0xe0434352），进程不崩，异常后入口可复调
- **s5** 热重编行为更新：拷贝工程 → 源码 42→43 → csproj 内换 AssemblyName →
  `dotnet build` → 同进程装载新 DLL → 43（宿主不重启）

## 验收复现

```
example\m-sc0\run.bat      # 五用例实跑 → *.log → 证据图重生成 → ALL GREEN exit 0
```

前置：`scripts\build-m0.2.bat DX12`（产 `build\DX12\nl-script.exe`，POST_BUILD 自动
拷 `nethost.dll`）+ `dotnet` SDK 在 PATH。

证据图：`example/m-sc0/*.png`（5 张，全部由工具真实输出渲染，
`tools/make-script-evidence.py`；禁止手绘示意）。展示台：`example/cases.json`
版本卡 `m-sc0`（usage / run-ok / run-throw / check / selftest 五条 tool 用例，
expectExit 2/0/1/0/0）。

## 架构落点

- `core/include/nl/script/Vm.hpp` + `core/src/script/Vm.cpp`：`nl::script::Vm`
  PIMPL（hostfxr.h/nethost.h/coreclr_delegates.h 只在 Vm.cpp 出现）；Result 阶段码
  1~6（nethost/LoadLibrary/init/delegate/入口解析/托管异常）。
- `thirdparty/dotnet-hosting/`：SDK 8.0.19 pack vendored——三头件 + `nethost.lib`
  （真导入库）+ `bin/nethost.dll`（运行期紧挨 exe，POST_BUILD copy）。
- `script/script-api/`：绑定面骨架 classlib（M-SC1 console 绑定 / M-SC2 ECS 绑定
  落这里）；`script/examples/hello/`：最小用户脚本（mode 驱动行为）。

## 踩坑存档（M-SC0 实测）

1. **libnethost.lib 是静态库**（1.6MB，无 `__imp_` 前缀符号），与 nethost.h 默认
   `dllimport` 语义不符 → LNK2019 `__imp_get_hostfxr_path`。用 `nethost.lib`
   （1.7KB 真导入库）+ nethost.dll 运行期 copy（官方样例布局）。
2. **classlib 默认不产 runtimeconfig.json** → csproj 加
   `<GenerateRuntimeConfigurationFiles>true</GenerateRuntimeConfigurationFiles>`
   （hostfxr FDD 加载硬前置）。
3. **`-p:AssemblyName` 是全局属性**，连带 ProjectReference 的 script-api 一起改名
   → NuGet restore "Ambiguous project name"。热重编换名必须改 csproj 本体。
4. **拷贝工程带 obj/ 会撞出同名 Ambiguous**（NuGet 缓存残留 FullPath）→ 热重编
   拷贝只拷源文件（csproj + .cs）。
5. **`Path::extension()` 只取最后一个点**（`HelloScript.runtimeconfig.json` 的
   extension 是 `.json`）；同理不能对 runtimeconfig 路径 `replace_extension(".dll")`
   （会拼出 `HelloScript.runtimeconfig.dll`）。文件名后缀判断用 ends_with、DLL 名
   用「程序集名 + .dll」显式拼。
6. **hostfxr.h 的 `initialize_for_runtime_config_fn` 是三参数**（path, parameters,
   out handle）——`parameters` 可传 nullptr，漏传即 C2198。
7. 托管异常在 MSVC `/EHsc` 下 **C++ `catch(...)` 接不住**，必须 SEH `__try/__except`
   （且 `__try` 函数体不许有需展开的 C++ 对象 → 独立纯 POD guard 函数）。
