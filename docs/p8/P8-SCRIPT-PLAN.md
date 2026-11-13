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
- 互操作不做反射自动绑定（~~Carrot 的 ReflectionHelper 思路记档不抄~~——**2026-11-14 修订**：绑定接线改生成器产出（M-SC4），属性面板反射入 M-SC10；运行时面仍是显式函数指针，「显式优先于隐晦」内核不变）。

## 胶水层扩展（M-SC4 起，2026-11-13 拍板）

> **拍板背景**：M-SC0~SC3 收官后用户指令「用脚本封装一套胶水层，提供对引擎接口的访问，用于开发功能，参考成熟引擎提供一套接口」。
> 三处证据复核（2026-11-13 本轮实读）：Carrot `script-api/Carrot/` 按域组织（Audio/Camera/Debug/ECS/Input/NavMesh/Physics/Scenes/Systems/Components）+ `CSharpBindings.h` 按域暴露组件类；
> bevy 无脚本 crate（克制先例维持）；UE/鸣潮「脚本无状态、状态在 C++」口径不变。
> **拍板结论（AskUserQuestion 一次清三项）**：
> ① **域范围 = 五域全做**：Input+时间帧循环+Log、Camera、Physics 载具、Audio、模型场景创建；
> ② **API 形态 = 沿用现状静态 facade**（ApiTable 尾部追加 + C# 按域加静态类，与 Engine/Ecs 同构，热更铁律 R1~R8 直接适用；仿 Carrot 按域拆绑定文件被否——本轮体量下偏过度设计）；
> ③ **轨道 = 重开 feat/script**（§7.1b camera 先例，wt 重建于 master 08a67c0，claim M-SC4 起）。
> **拍板修订（2026-11-14，用户拍板覆盖旧表述）**：① **绑定导出自动化生成器本轮就上**——API 接线（typedef/注册行/C# delegate+facade）由单源 spec 生成，产出物仍是显式函数指针（显式性口径不破，变的是「谁写接线」）；② **属性面板要做**（C# `[Prop]` 特性 + .NET 反射枚举 + ImGui 通用面板，Carrot 特性集同构）入 M-SC 序列。
> 其余口径不变：ApiTable 只许尾部追加、绑定面无跨帧状态、宿主侧显式 set 绑定活实例（`ecsSetWorld` 模式推广：`inputSetState/inputSetActionMap/cameraSetRig/physicsSetWorld/audioSetWorld`，未绑 = 专属错误码，热更窗口兜底）。

### 里程碑（M-SC4~SC11，2026-11-14 用户拍板重排）

> **生成器口径（2026-11-14 用户拍板定稿）**：单源声明文件 `script/script-api/exports.api`（每导出一行：域|名|返回|参数|文档，附 `type` 段声明复杂对象）→ `tools/scriptapi/gen.py` 产出 ① C++ `ApiTable.g.hpp`（ApiTable 结构 + POD 镜像 struct）② C++ `ApiTable.g.inc`（注册行）③ C# `Wiring.g.cs`（delegate + 指针字段 + Init 偏移 + NativeMarshal 生命周期原语 + 复杂对象 C# 封装）。**类型映射（TYPEMAP）与生命周期规则（in str 分配/释放配对、out str 静态缓冲+立即拷贝、bufN 定长、delegate 保活）只在机制里写一遍，全部导出统一走**——手写方式下散在各 facade 的类型映射/生命周期重复由此收口。复杂对象（TransformData 等）声明一次，C++/C# 两侧同构封装同时生成（C# 侧 partial，hand 扩展另落）。facade 方法本身手写（返回语义/错误码含义在场，不硬造 wrapper DSL），C++ 实现体手写（功能本体）。生成物入 git，构建不依赖 Python；改声明后手动重跑 gen.py。

> **C++→C# 方向对照（2026-11-14，UnrealSharp 互操作架构实读）**：其双向机制与我们同构（C#→C++ = 原生函数指针表；C++→C# = `[UnmanagedCallersOnly]` 静态 thunk 表 + GCHandle 实例句柄）。~~吸收：托管回调保活按 reload 批次归组~~（**M-SC4 实读勘误**：script-api 本身经 Bootstrap 依赖解析装进 collectible ALC，`_pinned`/函数指针缓存随 Unload 整体回收——「批次归组」由 ALC 边界结构性达成，无须改 `_pinned`）；② 未来 C++→C# 高频事件回调记档 = `[UnmanagedCallersOnly]` 静态 thunk + GCHandle 句柄表，不走 delegate marshal（当前 OnTick/console 回调两通道够用，不提前立）。注意 M-SC3 实测：collectible ALC 反射路径禁 `[UnmanagedCallersOnly]`（InvalidProgram），双层入口形态维持。

> **生命周期账本（2026-11-14 用户拍板补条：反射对象生命周期必须防泄露）**：每条 TYPEMAP 映射带显式所有权标注（谁分配/谁释放/何时失效），生成器按规则产配对代码。规则表：in str = C# AllocHGlobal + finally 必释放（生成物写死配对）；out str = C++ 静态缓冲「下次调用前有效」+ C# 立即拷贝（C# 侧零分配）；命令回调 outText = C# CoTaskMem 分配 + C++ 拷后 CoTaskMemFree（SEH 路径含）；delegate 参数 = ApiWiring 统一 _pinned 保活登记（按 reload 批次归组）+ unload 前 scriptUnregisterAll 全清；POD 复杂对象 = 值/栈传递不跨堆；宿主活实例 = 显式 set/unset + 未绑错误码兜底。**硬规则**：① C++ 侧永不跨 reload 缓存托管引用（M-SC10 反射描述符同款——描述符缓存放托管侧随 ALC 回收，C++ 只存 POD 描述表）；② selftest 泄露段 = 万次级串往返 + GC.Collect 后句柄漂移阈值断言（s8f 手法扩展），新域必带。

> **复杂对象档位（2026-11-14，UnrealSharp Marshalling System 实读）**：其三档 = Blittable（布局一致 memcpy 直传，零开销快路径）/ Natively copyable（引擎拷贝构造 + NativeStructHandle 析构句柄）/ Mirror Struct（逐字段兜底）。我们 type 段 POD 双层镜像只做 **Blittable 一档**（正是其认定的快路径）；后两档依赖 UE 属性系统/拷贝构造，纯 C ABI 函数表不需要。**生成器校验规则：type 段只收纯 POD（数值字段 + 定长 buf，禁字符串/嵌套引用成员），gen.py 解析即拒，非 POD 需求显式拆 str/bufN 参数**。

> **容器规则（2026-11-14 用户追问补条；UnrealSharp/Flax 实读：容器 = 包装对象模式，FastAccess 禁用的慢路径）**：容器不进 type 段、不过 blittable 边界，按协议拆：① 动态数组 = count+指针协议（C#→C++：`count + const T* data` 调用期存活；C++→C#：`out count + const T** data` 静态缓冲「下次调用前有效」+ C# 立即拷，argv/consoleRegister 即此形态先例）；② 字典不过界——按 key 查走单值导出（ecsFindByName 形态），全量视图走双列表 count+ptr 或序列化快照串（cvarList 文本协议先例）；③ 嵌套（字符串数组）= count + 二级指针，逐元素仍走 str 账本规则。现 16 导出零容器缺口，M-SC5+ 各域按需落协议。

| 里程碑 | 内容（C++ 导出 + C# 静态类） | 关键口径 | 依赖 |
|---|---|---|---|
| **M-SC4** 绑定生成器（✅ 2026-11-14 收官，tag `m-sc4`） | 声明 → 产出 ① C++ ApiTable 接线块 ② C# delegate + 按域静态 facade。**回归基线 = 对现有 16 导出（console 6 + ECS 10）回放**：生成接线替换手写后编译过 + selftest 全绿 + `reload-script` 链路无损 | 声明是唯一手写项；实现体照写（语义不可生成）；尾追加纪律对声明序列生效；生成器入 `tools/scriptapi/` | 无 |
| **M-SC5** Time + Input + Log（✅ 2026-11-14 收官，tag `m-sc5`，实际导出集 timeGetState/timeSetState + inputActionFind/Bool/Float/Vec2/KeyIsDown + logWrite/logSetFilter 共 9 条） | `timeQuery(simTime/frameDt/tickCount/paused)` + `timeSetState(paused,timeScale)`；`inputActionFind(name→id)` + `inputActionBool/Float/Vec2(id→值+边沿)` + `inputKeyIsDown(key)`；`logWrite(level,msg)` + `logSetFilter(instruction)`。C# = `Time`/`Input`/`Log` 静态类 | 绑 GameClock（ScriptHost 已持有）；InputState/ActionMap 宿主显式 set，viewer `--script` 模式接线（ActionMap 未建 = NoSource 错误码，不隐式建表）；log 走 `nl::core::log` 全局面（level 0..5=Trace..Fatal，越界 = BadArg）；**本域 = 生成器实战首用** | M-SC4 |
| **M-SC6** Camera | cameraGetPose/cameraSetPose（pos+yaw+pitch 口径随实码定）+ fov get/set；C# = `Camera` 静态类 | 绑 viewer 现役相机（rig/chase/free 装配落码期读 viewer/main.cpp 定；未绑 = NoCamera） | M-SC5 |
| **M-SC7** 模型场景创建 | `ecsAttachModel(id, meshFile)` + `ecsDetachModel(id)`（+ 可选 ecsModelInfo）；C# = `Ecs` 尾部追加 | 实体挂 mesh → scene batch 装配复用 E1b 链（scene.cpp 白名单口径落码期实读定）；meshFile 校验走 viewer 既有白名单语义 | M-SC5 |
| **M-SC8** Physics 载具 | `physicsAddVehicle/removeVehicle/setVehicleInput/vehicleFullPose/teleportVehicle`；C# = `Physics` 静态类 | 直接对齐 M-NET2 已落的 `physics::World` 载具 API 面（不新造）；未绑 = NoWorld 同构错误码；线程 = 逻辑帧约定（OnTick 内调用） | M-SC5 |
| **M-SC9** Audio | `audioPlay(name,x,y,z)/audioStop(id)/audioSetListenerPose(...)`（导出集落码期读 AudioWorld 实码定）；C# = `Audio` 静态类 | 绑 M-GL2 交付的 `AudioWorld`；播放句柄失效安全拒绝沿用 SceneEntity 口径 | M-SC5 |
| **M-SC10** 属性面板 | C# 组件字段打 `[Prop]` 特性（DisplayName/Range/Default，Carrot 特性集同构）→ .NET 反射枚举 → C++ 取描述表 → ImGui 通用面板读写；热更 `reload-script` 后面板自动跟随 | 面板挂 imgui（vendored）= HUD 面板规范适用（高≤560/ASCII）；载体（ScriptComponent/M-SC2b 重议启用与否）落码期与用户确认；kStage 不动，渲染回归只进面板自身新卡 | M-SC7 |
| **M-SC11** 收口 | 跨域示例脚本（Time+Input+Ecs+Log+Camera 一脚本打通）+ GUIDE 扩章 + close 闸 + merge 回 master | — | M-SC4~SC10 |

- **验收口径（每域一致）**：nl-script selftest 新段（s9 起：C# 读写引擎状态 → C++ 逐位读回断言 + 未绑错误码负向）；展示台 tool 卡（usage/正向/负向三起步）；`reload-script` 后新域 API 重绑定无损；A 类铁律 = 零渲染符号、不进 kStage、不进渲染回归（**M-SC10 面板例外按其行口径**）。生成器验收 = M-SC4 行回归基线。
- **有意不做（防范围失控）**：不做脚本侧物理步进 API（物理步进归引擎 tick）、不做音频流解码 API、不做任意事件总线（P8-SCRIPT-PLAN 既有否决维持）。~~不做反射属性暴露~~（2026-11-14 用户拍板翻转：属性面板入 M-SC10）。

## 注册表

- worktree：`E:/UE_STUDY_2026/nl-engine-script`，分支 `feat/script`（**已建**，wt_pool track `script` 已注册 active；junction 共享 `.workbuddy`/`slang dist` 实测在位）。M-SC0 收口 tag `m-sc0`（2026-11-09）。
- 计划归属：ROADMAP E4 脚本段（2026-11-09 解禁，鸣潮方向）；物理已提前开工（`P8-PHYS-PLAN.md` M-P0~M-P3），音频 2026-11-09 定案 vendored miniaudio（`P8-AUDIO-PLAN.md`，排期闸仍排 E1 后）——两者均已不再「等游戏类型拍板」，见 ROADMAP E4 行。

## M-SC0 实测补记（2026-11-09，踩坑详档见 docs/p8/SCRIPT-TOOLS-GUIDE.md）

- vendored 实际落位：`include/`（hostfxr.h/nethost.h/coreclr_delegates.h）+ `lib/nethost.lib`（**真导入库**；pack 里的 libnethost.lib 是静态库，与 dllimport 语义不符，不入库）+ `bin/nethost.dll`（POST_BUILD copy 到 exe 旁，官方样例布局）。
- `UNMANAGEDCALLERSONLY_METHOD` 路线实测可用：C# 方法标 `[UnmanagedCallersOnly]`、签名 `int(IntPtr, int)`，返回指针即默认签名——与官方口径一致。
- 托管异常边界：MSVC /EHsc 下必须 SEH `__try/__except`（catch(...) 接不住），实测 0xe0434352 捕获、进程存活。
- 热重编验收（s5）落地细节：拷工程只拷源文件；换 AssemblyName 改 csproj 本体（`-p:` 全局属性会污染 ProjectReference → NuGet「Ambiguous project name」）；classlib 需 `GenerateRuntimeConfigurationFiles=true` 才产 runtimeconfig.json。
