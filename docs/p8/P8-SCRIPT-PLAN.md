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
| **M-SC0** hosting 内核 ✅ **已完成（2026-11-09，tag `m-sc0`）** | `thirdparty/dotnet-hosting/` vendored + CMake 接 nethost；`nl::script::Vm`（runtime 初始化 / 加载程序集 / 取函数指针 / 调用与异常边界）；`script-api` 骨架 classlib + 用户脚本最小示例；`nl-script` 工具（`run` 编译+跑脚本 / `check` 只编译检查） | selftest：C++ 装载运行时→加载 DLL→调静态函数拿返回值→托管异常捕获转错误码；脚本改动经 `dotnet build` 重编后行为更新；工具版交代（usage+正向+负向 tool 卡）——**全部达成：selftest 17/17（含 s5 热重编 42→43 同进程行为更新）、展示台 m-sc0 卡 5 条 + 5 张真实输出证据图 + SCRIPT-TOOLS-GUIDE（踩坑 7 条存档）** | 无（SDK 已装），即刻开工 |
| **M-SC1** console 绑定 ✅ **已完成（2026-11-09，tag `m-sc1`）** | C++→C# 函数指针暴露：`cvar.get/set/list`、`console.exec`（ExecCode 0/1/2/3 → C# 返回）；C#→C++ 注册：`Console.Register(name, fn)` 进 console 命令表（fn 经异常边界调用）——**落地：ApiTable 6 导出随入口 args 传入（Carrot mono internal call 的 hostfxr 对应物）+ reverse P/Invoke（CoTaskMem 配对）+ Console 扩 unregisterCommand** | selftest：C# 改 cvar 触发 onChanged、C# 注册命令经 console exec 调通、错误码逐一对齐——**全部达成：selftest s6a~s6f 全绿（onChanged 恰触发/exec 调通/掩码 31 对齐/SEH 异常命令/双向读回/注销解绑），M-SC0 s0~s5 不回归，展示台 m-sc1 卡 4 条 + 4 张真实输出证据图，双后端构建+selftest 均 PASS** | console M-C3 merge 回 master ✓（9e12cc6） |
| **M-SC2** ECS 绑定（数据面）✅ 2026-11-09 tag `m-sc2` | Bindings v2 尾部追加 10 导出（Spawn/Remove/Flush/Exists/SetParent/GetParent/AddTransform/TransformGet/TransformSet/FindByName）+ 宿主侧 `ecsSetWorld()` 显式绑 World；C# `Ecs` 静态类 + `SceneEntity` wrapper（Carrot Entity 同构）；错误码 0..4（ok/NoWorld/NoEntity/Component/BadArg） | selftest s7a~s7g 全绿（M-SC0/SC1 不回归）：C# 建实体→挂 Transform→改字段→C++ 逐位读回；失效实体七项操作全 NoEntity 拒绝不崩；JSON roundtrip 字节一致；解绑后导出一律 NoWorld（M-SC3 兜底）。踩坑 4 条见 SCRIPT-TOOLS-GUIDE | E1a 已 merge（2d4ad49）✓ |
| **M-SC2b**（可选，后定） | ScriptComponent 脚本逻辑组件：C++ 组件持程序集/类型名，tick 回调 `OnTick(self, dt)` | 视 M-SC2 后扩展需求决定，**默认不做** | M-SC2 |
| **M-SC3** 热更 + 收口 ✅ 2026-11-09 tag `m-sc3` | `console.reload-script`：新 DLL 载入新 collectible ALC → 重绑定 → 旧 ALC Unload；收口 merge 回 master | 验收：改 C# 脚本 → `dotnet build` → reload → cvar/命令/实体行为换新，C++ 侧状态无损（cvar 值/实体树不动）；连续 reload 无泄漏（句柄计数核）；全量回归绿。**实测全过**：selftest s8a~s8h 全绿（reload 解绑 2 命令 + weakref dead + 行为换新 [v2] + 状态四锚 + 连续 reload x3 句柄漂移 0）；踩坑 5 条见 SCRIPT-TOOLS-GUIDE M-SC3 节 | M-SC0~SC2 |

## M-SC3 落地拍板（2026-11-09，热更架构定稿）

- **bootstrap 中转（官方 collectible 口径）**：hostfxr 只能把程序装配进默认 ALC
  （不可卸载），collectible ALC 的创建/卸载必须托管侧完成 → 非收集 bootstrap
  程序集（`script/bootstrap/`，常驻）暴露 BootLoad/BootCall/BootUnload 三函数指针
  （C++ 一次性取用安全）；脚本程序集全部经 bootstrap `LoadFromStream` 进
  collectible ALC（**不锁 DLL 文件**——`LoadFromAssemblyPath` 锁文件会让
  `dotnet build` 同名覆盖必断，热更工作流关键通路）；脚本方法经 bootstrap
  反射 Invoke（机制中转，绑定面照旧显式函数指针，「不做反射自动绑定」原则不破）。
- **脚本入口拆两层**：普通 `Entry`（collectible 反射路径，**禁 [UnmanagedCallersOnly]**
  ——运行时禁止托管代码反射调用它，实测 InvalidProgram）+ `[UnmanagedCallersOnly]
  EntryNative`（默认 ALC `getEntryPoint` 函数指针路径）。
- **卸载次序铁律**：先 `scriptUnregisterAll()`（脚本命令登记表批量解绑，解除对
  旧 ALC reverse P/Invoke thunk 的全部 C++ 侧引用）→ 再 `scriptUnload()`。
- ** unload 与 GC 排空不得同帧**（本轮最大坑，官方教程口径实证）：同方法内
  `alc = null` 是 JIT dead store，栈引用永远 root；unload 必须放 NoInlining 小方法
  返回 WeakReference、排空在调用方。详 SCRIPT-TOOLS-GUIDE M-SC3 踩坑存档。
- **C++ 侧状态无损口径**：reload 不碰 cvar 值/实体树/内建命令；重绑定 = 新脚本
  Entry 自己重新注册（脚本自描述，Carrot load-callback 同构）。连续 reload x3
  `GetProcessHandleCount` 漂移 0。

## 有意简化清单（防范围失控，2026-11-09 定）

- 不做：Roslyn 运行时编译（构建期 dotnet build）/ AOT / NativeHost 全自宿主（hostfxr 标准路径够）/ 多 ALC 版本共存（单 collectible ALC 轮换）/ 调试器协议（VS 调试脚本属远期）/ 沙箱权限分级 / OS 级文件 watcher（mtime 轮询够）/ Lua 与 TS（两轮作废留痕，不留复活尾巴）。
- 「脚本调 C++」单向显式函数指针优先；C++ 调脚本只走 console 注册命令与（若做的）组件回调点，不做任意事件总线。
- 互操作不做反射自动绑定（Carrot 的 ReflectionHelper 思路记档不抄）——绑定面手写，宁可啰嗦不可隐晦。

## 胶水层扩展（M-SC4 起，2026-11-13 拍板）

> **拍板背景**：M-SC0~SC3 收官后用户指令「用脚本封装一套胶水层，提供对引擎接口的访问，用于开发功能，参考成熟引擎提供一套接口」。
> 三处证据复核（2026-11-13 本轮实读）：Carrot `script-api/Carrot/` 按域组织（Audio/Camera/Debug/ECS/Input/NavMesh/Physics/Scenes/Systems/Components）+ `CSharpBindings.h` 按域暴露组件类；
> bevy 无脚本 crate（克制先例维持）；UE/鸣潮「脚本无状态、状态在 C++」口径不变。
> **拍板结论（AskUserQuestion 一次清三项）**：
> ① **域范围 = 五域全做**：Input+时间帧循环+Log、Camera、Physics 载具、Audio、模型场景创建；
> ② **API 形态 = 沿用现状静态 facade**（ApiTable 尾部追加 + C# 按域加静态类，与 Engine/Ecs 同构，热更铁律 R1~R8 直接适用；仿 Carrot 按域拆绑定文件被否——本轮体量下偏过度设计）；
> ③ **轨道 = 重开 feat/script**（§7.1b camera 先例，wt 重建于 master 08a67c0，claim M-SC4 起）。
> 已定口径不变：手写显式函数指针、ApiTable 只许尾部追加、绑定面无跨帧状态、宿主侧显式 set 绑定活实例（`ecsSetWorld` 模式推广：`inputSetState/inputSetActionMap/cameraSetRig/physicsSetWorld/audioSetWorld`，未绑 = 专属错误码，热更窗口兜底）。

### 里程碑（M-SC4~SC9）

| 里程碑 | 内容（C++ 导出 + C# 静态类） | 关键口径 | 依赖 |
|---|---|---|---|
| **M-SC4** 胶水层一期：Time + Input + Log | `timeQuery(simTime/frameDt/tickCount/paused)` + `timeSetState(paused,timeScale)`；`inputActionFind(name→id)` + `inputActionBool/Float/Vec2(id→值+边沿)` + `inputKeyIsDown(key)`；`logWrite(level,msg)` + `logSetFilter(instruction)`。C# = `Time`/`Input`/`Log` 静态类 | 绑 GameClock（ScriptHost 已持有）；InputState/ActionMap 宿主显式 set，viewer `--script` 模式接线（ActionMap 未建 = NoSource 错误码，不隐式建表）；log 走 `nl::core::log` 全局面（level 0..5=Trace..Fatal，越界 = BadArg）；Log 类别 = `script` 通道（writeCh 口径） | 无（均已 merge） |
| **M-SC5** Camera | cameraGetPose/cameraSetPose（pos+yaw+pitch 口径随实码定）+ fov get/set；C# = `Camera` 静态类 | 绑 viewer 现役相机（rig/chase/free 装配落码期读 viewer/main.cpp 定；未绑 = NoCamera） | M-SC4 |
| **M-SC6** 模型场景创建 | `ecsAttachModel(id, meshFile)` + `ecsDetachModel(id)`（+ 可选 ecsModelInfo）；C# = `Ecs` 尾部追加 | 实体挂 mesh → scene batch 装配复用 E1b 链（scene.cpp 白名单口径落码期实读定）；meshFile 校验走 viewer 既有白名单语义 | M-SC4 |
| **M-SC7** Physics 载具 | `physicsAddVehicle/removeVehicle/setVehicleInput/vehicleFullPose/teleportVehicle`；C# = `Physics` 静态类 | 直接对齐 M-NET2 已落的 `physics::World` 载具 API 面（不新造）；未绑 = NoWorld 同构错误码；线程 = 逻辑帧约定（OnTick 内调用） | M-SC4 |
| **M-SC8** Audio | `audioPlay(name,x,y,z)/audioStop(id)/audioSetListenerPose(...)`（导出集落码期读 AudioWorld 实码定）；C# = `Audio` 静态类 | 绑 M-GL2 交付的 `AudioWorld`；播放句柄失效安全拒绝沿用 SceneEntity 口径 | M-SC4 |
| **M-SC9** 收口 | 跨域示例脚本（Time+Input+Ecs+Log+Camera 一脚本打通）+ GUIDE 扩章 + close 闸 + merge 回 master | — | M-SC4~SC8 |

- **验收口径（每域一致）**：nl-script selftest 新段（s9 起：C# 读写引擎状态 → C++ 逐位读回断言 + 未绑错误码负向）；展示台 tool 卡（usage/正向/负向三起步）；`reload-script` 后新域 API 重绑定无损；A 类铁律 = 零渲染符号、不进 kStage、不进渲染回归。
- **有意不做（防范围失控）**：不做脚本侧物理步进 API（物理步进归引擎 tick）、不做音频流解码 API、不做反射属性暴露（Carrot ReflectionProperty 记档不抄）、不做任意事件总线（P8-SCRIPT-PLAN 既有否决维持）。

## 注册表

- worktree：`E:/UE_STUDY_2026/nl-engine-script`，分支 `feat/script`（**已建**，wt_pool track `script` 已注册 active；junction 共享 `.workbuddy`/`slang dist` 实测在位）。M-SC0 收口 tag `m-sc0`（2026-11-09）。
- 计划归属：ROADMAP E4 脚本段（2026-11-09 解禁，鸣潮方向）；物理已提前开工（`P8-PHYS-PLAN.md` M-P0~M-P3），音频 2026-11-09 定案 vendored miniaudio（`P8-AUDIO-PLAN.md`，排期闸仍排 E1 后）——两者均已不再「等游戏类型拍板」，见 ROADMAP E4 行。

## M-SC0 实测补记（2026-11-09，踩坑详档见 docs/p8/SCRIPT-TOOLS-GUIDE.md）

- vendored 实际落位：`include/`（hostfxr.h/nethost.h/coreclr_delegates.h）+ `lib/nethost.lib`（**真导入库**；pack 里的 libnethost.lib 是静态库，与 dllimport 语义不符，不入库）+ `bin/nethost.dll`（POST_BUILD copy 到 exe 旁，官方样例布局）。
- `UNMANAGEDCALLERSONLY_METHOD` 路线实测可用：C# 方法标 `[UnmanagedCallersOnly]`、签名 `int(IntPtr, int)`，返回指针即默认签名——与官方口径一致。
- 托管异常边界：MSVC /EHsc 下必须 SEH `__try/__except`（catch(...) 接不住），实测 0xe0434352 捕获、进程存活。
- 热重编验收（s5）落地细节：拷工程只拷源文件；换 AssemblyName 改 csproj 本体（`-p:` 全局属性会污染 ProjectReference → NuGet「Ambiguous project name」）；classlib 需 `GenerateRuntimeConfigurationFiles=true` 才产 runtimeconfig.json。
