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

---

# M-SC1：console 绑定（2026-11-09，tag `m-sc1`）

## 绑定面口径（nl/script/Bindings）

- **C++ → C#**：`ApiTable`（6 导出函数指针，v1 固定顺序尾部追加）随托管入口 `args`
  传入；C# 侧 `Engine.Init(table)` 按偏移 `Marshal.ReadIntPtr` 读出转 delegate。
  这是 Carrot `mono_add_internal_call`（按名 internal call）在 hostfxr 路线的对应物——
  Mono 底座不走，显式函数指针表 = 同构的「单一显式绑定面、无反射魔法」。
  6 导出：`cvarGet`（类型码 0=无/1=bool/2=int/3=float/4=string + toString 值）、
  `cvarSet`（引擎路径 setFromString 语义，onChanged 触发）、`cvarList`、
  `consoleExec`（ExecCode 0..3 原样）、`consoleRegister`、`consoleUnregister`。
- **C# → C++**：`ConsoleRegister` 经 reverse P/Invoke——delegate 用静态列表保活
  （thunk 被 GC 收 = native 调用炸），原始 ABI `int fn(int argc, const char** argv,
  const char** outText)`（0=ok/1=失败）；`outText` 由 C# `Marshal.AllocHGlobal`
  （Windows 实现 = CoTaskMemAlloc）分配，C++ 拷成 std::string 后 `CoTaskMemFree`
  （COM 字符串同款配对）。友好包装 `ScriptCommandHandler`（返回串=成功/null=失败，
  异常包装内 catch）；`RegisterRaw` 走原始 ABI（异常穿到 C++ SEH 边界，selftest 用）。
- **内存所有权两条路各定一条**（Bindings.hpp 文件头写死）：C++→C# 返回串 = C++ 静态
  缓冲持有、**下次调用前有效**（console 单线程约定下无竞态，C# 立即拷贝）；
  C#→C++ = CoTaskMem 配对。不许混用。
- **异常边界**：C++ 侧调用托管命令回调经 SEH `__except`（POD guard 函数），
  0xe0434352 → `CommandResult{false}` → ExecCode::CommandFailed，console 存活。
- **Console 扩 `unregisterCommand`**（core/console）：M-SC3 热重载解绑的落点；
  CVar 不在命令表，不会误删。

## selftest 判据（M-SC1 s6 组，M-SC0 s0~s5 不回归）

- s6a C# `CvarSet("script.count","77")` → C++ onChanged 钩子恰触发一次、捕获 77；
- s6b C# 注册 `script.echo` → console exec 调通（回显文本）；
- s6c ExecCode 逐一对齐：未知命令=1 / 引擎路径类型不符=2 / `cvar.set` 命令路径坏值=3 /
  裸 CVar 名=0（C# 视角触发，位掩码 31 断言）；
- s6d 原始 ABI 命令抛托管异常 → SEH 捕获 → CommandFailed + console 存活可复调；
- s6e C++ `registerFloat(1.5)` 后 C# `CvarGet` 读回 "1.5" + `CvarList` 全量视图含脚本 cvar；
- s6f `ConsoleUnregister` → exec → UnknownCommand（M-SC3 解绑预演）。

## 验收复现

```
nl-script selftest script/examples/hello script/examples/console   # s0~s5 + s6a~s6f
nl-script run script/examples/console --api --type "NlScript.Examples.ConsoleScript, ConsoleScript"
nl-script run script/examples/console --api                        # 负向：exit 1 code=5
nl-script check script/examples/console
```

`run --api` 传 `ApiTable*` 给 Entry；宿主侧预注册 `installBuiltins()` +
`script.count`/`script.rate` demo cvar（生产宿主注册自家 cvar 的口径）。

## 踩坑存档（M-SC1 实测）

1. **script-api 与脚本工程都要 `<AllowUnsafeBlocks>true`**：`Engine` 标了 `unsafe`
   class（ApiTable 偏移读），只在示例 csproj 开、api 程序集漏开 → CS0227。
2. **lambda 参数显隐混用**：`(argc, argv, out IntPtr text) =>` 是 CS0748（隐式参数
   不能与显式 out 混排），全部显式 `(int argc, IntPtr argv, out IntPtr text)`。
3. **selftest 漏 `installBuiltins()`**：`cvar.set` 命令路径断言拿到 UnknownCommand(1)
   而非 CommandFailed(3)，掩码 27≠31——内建命令集是生产宿主的 install 动作，
   selftest 必须同口径装上。
4. **`DOTNET_CLI_UI_LANGUAGE=en`**：dotnet CLI 中文摘要（已成功生成/警告/错误）在
   证据图 Consolas 下成方框（M-SC0 踩坑 1 复发）——dotnetBuild 命令行前置
   `set ...&&` 根治，顺带证据图输出全英文统一。
5. **cases.json 尾锚插入**：末卡闭括号是四层嵌套（case `}` / cases `]` / version `}` /
   versions `]` / root `}`），锚点切多一行就把 cases 数组闭括号切掉（本轮实测两次
   JSONDecodeError）——只切末三行、保留 case 层 `    }`，插入后必须 json.load 复验。

---

# M-SC2：ECS 数据面绑定（2026-11-09，tag `m-sc2`）

## 绑定面口径（nl/script/Bindings v2）

- **表结构**：ApiTable 尾部追加 10 导出（偏移 6..15）——ecsSpawn / ecsRemove /
  ecsFlush / ecsExists / ecsSetParent / ecsGetParent / ecsAddTransform /
  ecsTransformGet / ecsTransformSet / ecsFindByName。旧 C# 按偏移读，零波及。
- **World 绑定**：表内纯函数指针，World 实例由宿主侧 `ecsSetWorld(World*)` 显式绑定
  （时序同 M-SC1 注册 cvar：绑 World → installBuiltins → 调入口）；nullptr = 解绑，
  此后导出一律 `kEcsNoWorld`——M-SC3 热重载窗口的兜底（s7g 验收）。
- **错误码**（0..4 写死防漂移）：0=ok / 1=NoWorld / 2=NoEntity / 3=组件缺失或重复挂 /
  4=BadArg。失效安全拒绝 = 每导出前置 `exists()` 检查，C# 侧不抛异常不崩。
- **C# 侧**：`Ecs` 静态类（偏移读表 + delegate 直调）+ `SceneEntity` 轻量 wrapper
  （{ ulong Id }，Carrot Entity 同构不持存储；实例方法全委托 Ecs.* 回错误码）。
- **Transform ABI**：局部 TRS，rot = quat (x,y,z,w)，pos/scale = vec3；float 数组
  指针传递（逐位透传，无格式化损耗）。TransformSet 任一指针可 NULL（跳过该组）。
- **内存所有权**：本组导出无字符串返回（名字查询走 ID），无新配对协议。

## 语义锚（Carrot/World 口径，selftest 实测）

- **延迟非对称**：spawn 新建立即可见（names_ 即时登记，exists 即时真）；remove 排队
  flush/tick 生效；`findEntityByName` 走 flush 后全局表（entities_）——**脚本 spawn 后
  必须宿主 flush 再按名查**（selftest s7a「host flush after script spawn」锚）。
  绑定层不改 World 语义（E1a 已 merge，不动 ECS 轨道）。
- **父删级联**：removeEntity 父删递归删子（run --api demo：remove root → leaf 同没）。
- **JSON roundtrip**：deserialise 前必须 `getComponentLibrary().registerComponent
  <TransformComponent>()`（clone 世界的工厂库默认空）——nanite-scene ecs-serial 同款。

## selftest 判据（s7 组，M-SC0 s0~s5 + M-SC1 s6 不回归）

- s7a C# SpawnSetup（spawn×2 + setParent + addTransform×2 + TRS 写入）→ C++ flush 后
  findByName 取回、parent/pos/quat/scale 逐位比对；
- s7b C++ 经绑定表预置 cppEnt 锚值 → C# ReadVerify 读回一致（float 逐位）；
- s7c C# InvalidProbe：未存在 ID 七项操作（Remove/SetParent/GetParent/AddTransform/
  GetTransform/SetTransform/Exists）全 NoEntity，不崩；
- s7d 延迟删除非对称：remove 后 flush 前 exists 真 → flush 后假 + 表级 TransformGet
  → NoEntity；
- s7e 父子 roundtrip：解除（parent=0）→ 重挂 → getChildIDs 视图一致；
- s7f JSON serialise→deserialise→serialise 字节一致（脚本侧改动全量入库）；
- s7g ecsSetWorld(nullptr) 后 spawn/flush/exists 一律 NoWorld。

## 验收复现

```
nl-script selftest script/examples/hello script/examples/console script/examples/ecs
nl-script run script/examples/ecs --api
nl-script run script/examples/ecs --api --type "NlScript.Examples.HelloScript, EcsScript"   # 负向 exit 1
nl-script check script/examples/ecs
```

默认类型名约定（本轮起）：`NlScript.Examples.<工程目录名首字母大写>Script, <AssemblyName>`
（hello→HelloScript / console→ConsoleScript / ecs→EcsScript）。

## 踩坑存档（M-SC2 实测）

1. **findEntityByName 走全局表**：spawn 后未 flush 按名查不到（返回 nullopt），与
   exists()（查 names_）非对称——不是 bug，是 Carrot 口径；绑定层照抄，selftest
   补 flush 步骤（「宿主 tick 边界」生产时序）。首轮 s7a 13 处 FAIL 全根因于此。
2. **clone 世界工厂库默认空**：deserialise 遇 "Transform" 报 unknown component name——
   必须 registerComponent<TransformComponent>()（nanite-scene 先例同款）。
3. **默认类型名硬编码**：run 无 --type 时原默认 HelloScript，ecs 工程直接 code=5——
   改按工程目录名推导（首字母大写 + Script）。
4. **cases.json 尾锚 + 转义**：heredoc 内嵌 JSON 的 `\\"` 要一层层数清（多一层 =
   JSONDecodeError line 3870）；command 含引号的字段用 Python 单引号串承载再 replace
   注入，插后 json.loads 复验。

# M-SC3：脚本热更（2026-11-09，tag `m-sc3`）

## 热更架构（bootstrap 中转，官方 collectible 口径）

- **为什么必须中转**：hostfxr 的 `load_assembly_and_get_function_pointer` 一律进
  **默认 ALC**（不可卸载）；collectible ALC 的创建/卸载只能在**托管侧**完成。
  结构 = 非收集 **bootstrap 程序集**（`script/bootstrap/`，常驻，其函数指针取用
  安全）+ collectible `ScriptLoadContext`（脚本程序集轮换）。
- **bootstrap 三入口**（`Nl.Script.Bootstrap.Bootstrap`，[UnmanagedCallersOnly]）：
  - `BootLoad(args)`：args = `struct{ const char* assemblyPathUtf8; }`（**结构体地址**，
    x64 偏移 0）。新 collectible ALC + `LoadFromStream`——**不锁 DLL 文件**，
    `dotnet build` 同名覆盖可写（热更工作流关键通路；`LoadFromAssemblyPath` 会锁）。
  - `BootCall(args)`：args = `struct{ typeUtf8; methodUtf8; void* data; int32 size; }`
    （偏移 0/8/16/24）。反射 Invoke 当前 ALC 脚本静态方法 `int(IntPtr,int)`；
    异常 catch → -102（互操作边界归一，C++ SEH 仅兜底）。
  - `BootUnload(args)`：Unload + weakref 排空校验。0=完全卸载 / 1=未完全 / -1=无脚本。
- **依赖解析**（`ScriptLoadContext.Load`）：脚本 DLL 旁的本地依赖（NlScriptApi.dll 等）
  `LoadFromStream` 进**同一** collectible ALC（Unload 时 `Engine._pinned` 保活列表
  一并回收）；框架程序集返回 null 回退 Default（官方推荐口径）。
- **脚本作者约定（Entry 拆两层）**：热更入口 = 普通 `static int(IntPtr,int)` 方法
  （**禁 [UnmanagedCallersOnly]**——运行时禁止托管代码反射调用它，BootCall 会
  InvalidProgram）；native 入口 = `[UnmanagedCallersOnly]` 包装方法（示例命名
  `EntryNative`），供默认 ALC `getEntryPoint` 取函数指针。三个示例脚本均已按此改。
- **C++ 侧**：`Vm` 热更四件套（`loadBootstrap`/`scriptLoad`/`scriptCall`/
  `scriptUnload`，阶段码 7=bootstrap 未装载 / 8=协议负码 / 9=卸载未完成）；
  `ScriptHost`（单例）编排 reload + 注册 **`console.reload-script`** 命令
  （无参=重载当前脚本 DLL，带参=换路径）；`Bindings` 脚本命令登记表
  （`nl_consoleRegister` 登记 / `nl_consoleUnregister` 对称擦除 /
  `scriptUnregisterAll()` 批量解绑）。**卸载次序铁律**：先解绑命令（解除对旧 ALC
  reverse P/Invoke thunk 的引用——Unload 时 CLR 注销 thunk，残留引用调用即崩）
  → 再 `scriptUnload`。**状态全在 C++**：cvar 值/实体树/内建命令 reload 不动；
  重绑定 = 新脚本 Entry 自己重新注册（脚本自描述，Carrot load-callback 同构）。

## selftest 判据（s8 组，s0~s5 + s6 + s7 不回归）

- s8a bootstrap 装载（3 函数指针）+ `console.reload-script` 注册（幂等 init）
- s8b 首载 collectible ALC + Entry 重绑定（echo v1 锚 + 命令登记表）
- s8c 改源 → `dotnet build` **同名覆盖** → `ScriptHost::reloadScript`
- s8d 行为换新（echo 前缀 `[v2]`）
- s8e C++ 状态无损四锚（cvar 值 / 实体逐位 / 内建命令 / 命令登记表重登记）
- s8f 连续 reload x3 无泄漏（`GetProcessHandleCount` 漂移 ≤ 32，实测 0）
- s8g `unloadScript` → weakref dead（ALC 完全收集）
- s8h 卸载后脚本命令 UnknownCommand + C++ 导出仍可用

## 验收复现

```
scripts\build-m0.2.bat DX12
example\m-sc3\run.bat          # selftest + run --api + check bootstrap + 4 张证据图
```

## 踩坑存档（M-SC3 实测）

1. **unload 与 GC 排空不得同帧（本轮最大坑）**：同方法内 `alc = null;` 是 JIT
   dead store（赋值后不再读取即被删除），栈 slot 一直持有 ALC 引用 → weakref
   **永远 alive**（30 轮强制 GC 都救不回）。官方《assembly unloadability》教程
   口径 = unload 放 `[MethodImpl(NoInlining)]` 小方法、返回 WeakReference、
   **排空在调用方**——方法边界是清除栈 root 的唯一可靠方式。诊断过程：三场景
   隔离实验（裸 stream / resolver stream / AssemblyPath）+ 纯托管 `dotnet run`
   对照全部 alive，定位到方法结构而非实现；同帧 vs 方法边界双 case 实证
   （True/False）。
2. **运行时禁止托管代码反射调用 [UnmanagedCallersOnly] 方法**：BootCall 的
   `MethodInfo.Invoke` 直接 `InvalidProgram`。默认 ALC 取函数指针又必须该
   attribute → Entry 拆两层（见脚本作者约定）。
3. **LoadFromAssemblyPath 锁文件**：Windows 上锁 DLL → `dotnet build` 无法同名
   覆盖 → 热更工作流必断；必须 `LoadFromStream`（不持文件句柄）。
4. **BootLoad args 是结构体地址不是字符串指针**：首版直传 `const char*`，
   bootstrap 按偏移 0 读指针 → AV。协议统一「C++ 栈上 POD 结构传地址 +
   bootstrap 偏移手读」（与 Marshal 结构 marshal 的 packing 坑隔离）。
5. **脚本命令登记表须对称维护**：`nl_consoleRegister` 登记 / `nl_consoleUnregister`
   擦除 / `scriptUnregisterAll()` 批量清（unload 前表必须空——不然 = 卸载后调用
   悬空 thunk）。登记表 set 有序去重，reload 输出解绑名单稳定可断言。
