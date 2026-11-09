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
| **Carrot** | 两层共 3592 行（2026-11-09 全文精读）：① `core/core/scripting/csharp/` **1229 行** hosting 层（Engine/CSAppDomain/CSAssembly/CSClass/CSMethod/CSField/CSProperty/CSObject/CSProject）；② `engine/scripting/` **2363 行** 绑定层（中央 `CSharpBindings` 句柄缓存 + `bindings/` 按域拆文件 + `ReflectionProperty` 属性暴露）。⚠️ **底座是 Mono 不是 .NET**（`mono_jit_init`/`MonoDomain`，README 自述目标：「不让引擎代码与运行时耦合太深 + 支持热更」） | **抄结构不抄底座**：① hosting/绑定分层 + forward.h 不透明句柄（我们 PIMPL 同构）；② 热更重绑定 = `registerGameAssemblyLoadCallback`/`UnloadCallback` 程序集换载时组件重 init（CSharpComponent 实证）；③ CSAssembly classCache（unordered_map<ns+name, Class>）→ 我们类型解析缓存同构；④ CSProject 极简 .csproj 解析（只读源文件清单）→ 我们脚本构建清单可借；⑤ Mono 具体 API 全不抄（走 hostfxr，见下节） |
| **bevy** crates 全列表 | **无脚本 crate**——原生系统立场 | 克制先例：只做最小嵌入，不造脚本框架 |
| **UE / 鸣潮**（外部口径） | 鸣潮：UE4 + .NET 托管 C#（3.6 起）；UE 蓝图状态放 C++ 对象、脚本只是逻辑 | ① **脚本无状态、状态在 C++**（热更安全根基，两家共同口径）；② hostfxr 托管加载 = 鸣潮同款技术路线 |

## 现成封装参考（2026-11-09 补充精读，回答「不用从头造」）

- **hostfxr 权威口径**（Microsoft Learn《Write a custom .NET runtime host》已核）：全流程就 **3 个导出函数**——`hostfxr_initialize_for_runtime_config` → `hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer)` → `hostfxr_close`；nethost 的 `get_hostfxr_path` 定位 dll。头三件套 `nethost.h`/`hostfxr.h`/`coreclr_delegates.h` **随 .NET SDK 自带**（vendored 就是从本机 SDK 目录拷，KB 级）。**托管入口默认签名**：`component_entry_point_fn = int (*)(const void* args, int sizeBytes)`，C# 侧 `public delegate int ComponentEntryPoint(IntPtr args, int sizeBytes)`；要别的签名需在调用时传 assembly-qualified delegate 类型名。两条**硬限制**写死进设计：①单进程只能装一个运行时（重复 init 只做兼容性检查）；②hosting API 只支持 **FDD（框架依赖部署）**——脚本程序集不能 self-contained。
- **M-SC0 的实际体量**：hosting 内核 ≈ 官方样例（dotnet/samples `core/hosting`）**200~300 行 C++**——`Vm` 类就是它的 RAII 化封装，不是大工程；真正工作量在绑定层（Carrot 2363 行是结构参照）。
- **netcorehost**（github.com/nbigaouette/netcorehost）：header-only C++17 hostfxr RAII 封装，MIT。**记档不采用**——我们只需要三个函数 + 自己的 Result 错误约定，引第三方模板封装反而加耦合；官方样例直接够。

## 架构约束

- **宿主侧**：`core/include/nl/script/` + `core/src/script/`，namespace `nl::script`。hosting bootstrap 用 **hostfxr/nethost**（口径见上节「现成封装参考」：3 导出函数 + `component_entry_point_fn` 默认签名 + FDD-only + 单运行时限制）；头三件套 vendored 到 `thirdparty/dotnet-hosting/`（从本机 SDK 拷，KB 级，照 METIS 先例），hostfxr.dll 从本机已装运行时加载。
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
| **M-SC0** hosting 内核 | `thirdparty/dotnet-hosting/` vendored（SDK 自带三头件）+ CMake 接 nethost；`nl::script::Vm`（官方三步口径：`initialize_for_runtime_config` → delegate 取 `load_assembly_and_get_function_pointer` → 调用；异常边界）；`script-api` 骨架 classlib + 用户脚本最小示例；`nl-script` 工具（`run` 编译+跑脚本 / `check` 只编译检查） | selftest：C++ 装载运行时→加载 DLL→经 `component_entry_point_fn`（`int(const void*, int)` 默认签名）调静态函数拿返回值→托管异常捕获转错误码；脚本改动经 `dotnet build` 重编后行为更新；工具版交代（usage+正向+负向 tool 卡） | 无（SDK 已装），即刻开工 |
| **M-SC1** console 绑定 | C++→C# 函数指针暴露：`cvar.get/set/list`、`console.exec`（ExecCode 0/1/2/3 → C# 返回）；C#→C++ 注册：`Console.Register(name, fn)` 进 console 命令表（fn 经异常边界调用） | selftest：C# 改 cvar 触发 onChanged、C# 注册命令经 console exec 调通、错误码逐一对齐 | **console M-C3 merge 回 master** |
| **M-SC2** ECS 绑定（数据面） | `World.Spawn/Remove/SetParent/GetParent`、Entity wrapper（与 EntityID 映射 + 失效安全拒绝）、TransformComponent 字段读写（pos/rot/scale） | selftest：C# 建实体→挂 Transform→改字段→C++ 读回一致；删除实体后 C# 访问得到失败结果不崩；JSON roundtrip 不受影响 | E1a 已 merge（2d4ad49）✓ |
| **M-SC2b**（可选，后定） | ScriptComponent 脚本逻辑组件：C++ 组件持程序集/类型名，tick 回调 `OnTick(self, dt)` | 视 M-SC2 后扩展需求决定，**默认不做** | M-SC2 |
| **M-SC3** 热更 + 收口 | `console.reload-script`：新 DLL 载入新 collectible ALC → 重绑定 → 旧 ALC Unload；收口 merge 回 master | 验收：改 C# 脚本 → `dotnet build` → reload → cvar/命令/实体行为换新，C++ 侧状态无损（cvar 值/实体树不动）；连续 reload 无泄漏（句柄计数核）；全量回归绿 | M-SC0~SC2 |

## 有意简化清单（防范围失控，2026-11-09 定）

- 不做：Roslyn 运行时编译（构建期 dotnet build）/ AOT / NativeHost 全自宿主（hostfxr 标准路径够）/ 多 ALC 版本共存（单 collectible ALC 轮换）/ 调试器协议（VS 调试脚本属远期）/ 沙箱权限分级 / OS 级文件 watcher（mtime 轮询够）/ Lua 与 TS（两轮作废留痕，不留复活尾巴）。
- 「脚本调 C++」单向显式函数指针优先；C++ 调脚本只走 console 注册命令与（若做的）组件回调点，不做任意事件总线。
- 互操作不做反射自动绑定（Carrot 的 ReflectionHelper 思路记档不抄）——绑定面手写，宁可啰嗦不可隐晦。

## 注册表

- worktree：`E:/UE_STUDY_2026/nl-engine-script`，分支 `feat/script`（待建）。
- 计划归属：ROADMAP E4 脚本段（2026-11-09 解禁，鸣潮方向）；物理/音频仍保持「等游戏类型拍板」原状。
