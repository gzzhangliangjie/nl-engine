# P8 脚本轨道计划：C# 脚本层（.NET embedding，鸣潮同款方向）

> **2026-11-09 立项拍板（三轮收敛，全程留痕）**：用户解禁 E4 脚本段，目标 = **功能扩展 + 热更**。
> 收敛过程：Lua（初拟）→ 用户给出参考「鸣潮方案 TypeScript+C#」+ 实证文章
> （smzdm `awwke0wg`：鸣潮 = UE4 + TS(V8/PuerTS 桥接) → 3.6 换 **C# 由 .NET 托管执行**，平均 +10 帧，
> 本质是拆桥接中转站）→ 用户拍板 **走 C#，「C# 现在的热更也很成熟了」**（HybridCLR 等 Unity 生态 +
> .NET collectible AssemblyLoadContext 官方卸载路径佐证）。Lua/TS 两案作废留痕，binding 层设计思路沿用。
> 轨道：A 类并行（纯逻辑，不碰 kStage、不进渲染回归）→ `feat/script` worktree。里程碑编号 M-SC0~SC3。

---

## 三处参考口径（2026-11-09 精读留痕）

| 来源 | 实况 | 我们取什么 |
|---|---|---|
| **Carrot** `engine/scripting/`（2363 行） | C#/.NET hosting：中央 `CSharpBindings` 缓存 CSClass/CSField/CSMethod 句柄；`bindings/` 按域拆文件；`ReflectionProperty` 属性暴露 | **直接同构可抄思路**（同为 C# hosting）：① 绑定按域拆文件、一个注册入口；② 句柄缓存 → C# 侧 Entity wrapper 与 C++ EntityID 映射；③ 属性暴露 → 统一注册表。其 mono 旧栈细节不抄（我们走 hostfxr 现代路径） |
| **bevy** crates 全列表 | **无脚本 crate**——原生系统立场 | 克制先例：只做最小嵌入，不造脚本框架 |
| **UE / 鸣潮**（外部口径） | 鸣潮：UE4 + .NET 托管 C#（3.6 起）；UE 蓝图状态放 C++ 对象、脚本只是逻辑 | ① **脚本无状态、状态在 C++**（热更安全根基，两家共同口径）；② hostfxr 托管加载 = 鸣潮同款技术路线 |

## 架构约束

- **宿主侧**：`core/include/nl/script/` + `core/src/script/`，namespace `nl::script`。hosting bootstrap 用 **hostfxr/nethost**（dotnet 官方 Hosting API：`hostfxr_initialize_for_runtime_config` → `hostfxr_get_runtime_delegate` → `load_assembly_and_get_function_pointer`）；nethost 头文件+导入库 vendored 到 `thirdparty/dotnet-hosting/`（KB 级，照 METIS 先例），hostfxr.dll 从本机已装运行时加载。
- **脚本侧**：C# classlib 两份——`script-api`（绑定面程序集：Console/CVar、World/Entity/Transform 的 C# 包装）+ 用户脚本程序集（引用前者）；`dotnet build` 构建（构建期步骤，SDK 9.0.304 本机已装，**目标 net8.0 LTS**）。
- **互操作方向**：脚本调 C++ = C++ 暴露**非托管函数指针**（reverse P/Invoke delegate）给 C#——单向显式绑定面，不做自动 marshal 反射魔法。
- **热更机制（定死）**：**collectible AssemblyLoadContext**——新版本 DLL 载入新 ALC、旧 ALC `Unload()`；C++ 侧状态（cvar/实体）不动。不做 Roslyn 运行时编译（构建期 `dotnet build` 出 DLL）。
- 热更安全根基（UE/鸣潮共同口径）：**脚本无跨帧状态**，状态全在 C++；C# 脚本禁止缓存跨 reload 的对象引用（API 层用句柄 + 失效检查兜底）。
- C++ 侧零渲染依赖（A 类铁律）；头文件不暴露 hostfxr.h（PIMPL）。
- 错误约定：C++ API 返回 `Result` 式结构（照 console `ExecCode` 先例）；托管异常在互操作边界捕获转 Result，不许穿透 C++ 析构。

## 本机环境实测（2026-11-09）

- .NET SDK **9.0.304**（`C:\Program Files\dotnet\sdk`）✓；运行时 `Microsoft.NETCore.App` 8.0.14/8.0.19/9.0.8 ✓——硬前置满足，脚本目标 **net8.0 LTS**（8.x 运行时在场）。

## 里程碑

| 里程碑 | 内容 | 验收判据 | 依赖 |
|---|---|---|---|
| **M-SC0** hosting 内核 ✅ **已完成（2026-11-09，tag `m-sc0`）** | `thirdparty/dotnet-hosting/` vendored + CMake 接 nethost；`nl::script::Vm`（runtime 初始化 / 加载程序集 / 取函数指针 / 调用与异常边界）；`script-api` 骨架 classlib + 用户脚本最小示例；`nl-script` 工具（`run` 编译+跑脚本 / `check` 只编译检查） | selftest：C++ 装载运行时→加载 DLL→调静态函数拿返回值→托管异常捕获转错误码；脚本改动经 `dotnet build` 重编后行为更新；工具版交代（usage+正向+负向 tool 卡）——**全部达成：selftest 17/17（含 s5 热重编 42→43 同进程行为更新）、展示台 m-sc0 卡 5 条 + 5 张真实输出证据图 + SCRIPT-TOOLS-GUIDE（踩坑 7 条存档）** | 无（SDK 已装），即刻开工 |
| **M-SC1** console 绑定 | C++→C# 函数指针暴露：`cvar.get/set/list`、`console.exec`（ExecCode 0/1/2/3 → C# 返回）；C#→C++ 注册：`Console.Register(name, fn)` 进 console 命令表（fn 经异常边界调用） | selftest：C# 改 cvar 触发 onChanged、C# 注册命令经 console exec 调通、错误码逐一对齐 | **console M-C3 merge 回 master** |
| **M-SC2** ECS 绑定（数据面） | `World.Spawn/Remove/SetParent/GetParent`、Entity wrapper（与 EntityID 映射 + 失效安全拒绝）、TransformComponent 字段读写（pos/rot/scale） | selftest：C# 建实体→挂 Transform→改字段→C++ 读回一致；删除实体后 C# 访问得到失败结果不崩；JSON roundtrip 不受影响 | E1a 已 merge（2d4ad49）✓ |
| **M-SC2b**（可选，后定） | ScriptComponent 脚本逻辑组件：C++ 组件持程序集/类型名，tick 回调 `OnTick(self, dt)` | 视 M-SC2 后扩展需求决定，**默认不做** | M-SC2 |
| **M-SC3** 热更 + 收口 | `console.reload-script`：新 DLL 载入新 collectible ALC → 重绑定 → 旧 ALC Unload；收口 merge 回 master | 验收：改 C# 脚本 → `dotnet build` → reload → cvar/命令/实体行为换新，C++ 侧状态无损（cvar 值/实体树不动）；连续 reload 无泄漏（句柄计数核）；全量回归绿 | M-SC0~SC2 |

## 有意简化清单（防范围失控，2026-11-09 定）

- 不做：Roslyn 运行时编译（构建期 dotnet build）/ AOT / NativeHost 全自宿主（hostfxr 标准路径够）/ 多 ALC 版本共存（单 collectible ALC 轮换）/ 调试器协议（VS 调试脚本属远期）/ 沙箱权限分级 / OS 级文件 watcher（mtime 轮询够）/ Lua 与 TS（两轮作废留痕，不留复活尾巴）。
- 「脚本调 C++」单向显式函数指针优先；C++ 调脚本只走 console 注册命令与（若做的）组件回调点，不做任意事件总线。
- 互操作不做反射自动绑定（Carrot 的 ReflectionHelper 思路记档不抄）——绑定面手写，宁可啰嗦不可隐晦。

## 注册表

- worktree：`E:/UE_STUDY_2026/nl-engine-script`，分支 `feat/script`（**已建**，wt_pool track `script` 已注册 active；junction 共享 `.workbuddy`/`slang dist` 实测在位）。M-SC0 收口 tag `m-sc0`（2026-11-09）。
- 计划归属：ROADMAP E4 脚本段（2026-11-09 解禁，鸣潮方向）；物理/音频仍保持「等游戏类型拍板」原状。

## M-SC0 实测补记（2026-11-09，踩坑详档见 docs/p8/SCRIPT-TOOLS-GUIDE.md）

- vendored 实际落位：`include/`（hostfxr.h/nethost.h/coreclr_delegates.h）+ `lib/nethost.lib`（**真导入库**；pack 里的 libnethost.lib 是静态库，与 dllimport 语义不符，不入库）+ `bin/nethost.dll`（POST_BUILD copy 到 exe 旁，官方样例布局）。
- `UNMANAGEDCALLERSONLY_METHOD` 路线实测可用：C# 方法标 `[UnmanagedCallersOnly]`、签名 `int(IntPtr, int)`，返回指针即默认签名——与官方口径一致。
- 托管异常边界：MSVC /EHsc 下必须 SEH `__try/__except`（catch(...) 接不住），实测 0xe0434352 捕获、进程存活。
- 热重编验收（s5）落地细节：拷工程只拷源文件；换 AssemblyName 改 csproj 本体（`-p:` 全局属性会污染 ProjectReference → NuGet「Ambiguous project name」）；classlib 需 `GenerateRuntimeConfigurationFiles=true` 才产 runtimeconfig.json。
