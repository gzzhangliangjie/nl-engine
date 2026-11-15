# dotnet-hosting — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | .NET hosting API — C# 脚本宿主的 hostfxr/nethost 装载层 |
| 版本 | **.NET SDK 8.0.19 pack**（M-SC0 台账明确：hostfxr.h / nethost.h / coreclr_delegates.h 三头件 + nethost.lib 真导入库 + nethost.dll） |
| 上游 | https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting（自定义 .NET 运行时宿主官方教程）+ dotnet/runtime hosting pack |
| 获取方式 | vendored 入库（M-SC0）：`include/` 3 头 + `lib/nethost.lib` + `bin/nethost.dll` + `LICENSE` |
| 许可证 | **MIT**（`LICENSE` 原文随库入库；M-PKG0 THIRD-PARTY-NOTICES 汇总含） |
| 形态 | 头 + 导入库 + 运行时 DLL（nethost.dll 必须**紧挨 exe**——M-PKG0 踩坑②：包内缺 DLL 时 exe 静默起不来；nl-script/viewer POST_BUILD copy） |
| 用途与消费点 | `nl::script::Vm`（PIMPL，头文件零 hostfxr 符号）：定位→LoadLibrary→init_for_runtime_config→取 delegate→UNMANAGEDCALLERSONLY 入口；托管异常 SEH 边界 guard（M-SC0） |
| 引入里程碑 | M-SC0（C# hosting 内核，feat/script 轨，tag `m-sc0`，2026-11-09） |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT 原文在库内，无风险面（nethost.dll 分发义务 = 保留版权声明，THIRD-PARTY-NOTICES 已收）。
2. **无厂商锁定**：Microsoft 官方开源 hosting API；硬限制两处已按官方教程锁定——FDD-only（framework-dependent，不带 runtime 自包含）+ 单运行时实例。
3. **零构建期新依赖**：三头 + 一 lib + 一 DLL 自包含；运行时 .NET Runtime 走目标机已装 net8.0（框架依赖语义）。
4. **已知风险如实记录**：① 目标机必须已装 .NET 8 runtime（FDD 语义）；② 托管异常穿边界靠 SEH 0xe0434352 捕获（M-SC0 s4 实测），升级 .NET 版本需重验该边界。
5. **可替换性**：Vm PIMPL 封装，hostfxr 符号零泄漏进头文件；换脚本底座（如 Lua/Mono）改封装层即可，script-api 绑定面独立。
