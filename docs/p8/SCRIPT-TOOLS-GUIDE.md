# P8-Script 工具指南 —— nl-script（M-SC0 hosting 内核）

> 归档：feat/script worktree，M-SC0 收口段（2026-11-09，tag `m-sc0`）。
> 工具 = C# hosting 内核（`nl::script::Vm`）的自测与演示入口，A 类并行轨道
> （不进 kStage / 渲染回归，工具版交代）。计划：`docs/p8/P8-SCRIPT-PLAN.md`。
> **C# 脚本怎么写、宿主怎么装、全流程与铁律速查** → `SCRIPT-CS-INTEROP.md`
>（本文只收工具面与踩坑历史考证）。

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

## 热更流程（端到端，开发者工作流）

**脚本作者的改动→生效循环**（全程引擎进程不重启）：

```
① 改 C# 脚本源码（script/examples/<name>/ 或任意脚本工程）
② dotnet build（同名覆盖 DLL——必须 LoadFromStream 装载，文件不被锁才可覆盖）
③ 触发 reload：
   - 引擎内：console 执行 `reload-script`（无参=重载当前脚本 DLL；带参=换路径）
   - 工具链：nl-script / ScriptHost::reloadScript(dllPath)
④ 生命周期自动走完，无需手工干预：
   scriptUnregisterAll（解绑旧脚本命令，登记表清空）
   → scriptUnload → BootUnload（旧 collectible ALC Unload + weakref 排空校验）
   → scriptLoad → BootLoad（新 ALC + LoadFromStream 装入新 DLL 及其本地依赖）
   → BootCall Entry（新脚本入口重注册自己的命令/cvar）
⑤ 验证：旧命令执行观察行为变化（如 echo 前缀 [v2]）；
   cvar 值 / 实体树 / 内建命令全程 C++ 持有，reload 不动
```

**运行时一次装载的内部链**（C++ → 托管，全部经 bootstrap 中转）：

```
Vm::loadBootstrap   取 bootstrap 程序集三函数指针（默认 ALC，常驻安全）
Vm::scriptLoad      → BootLoad(assemblyPath)   建 collectible ALC + LoadFromStream
Vm::scriptCall      → BootCall(type, method)   反射 Invoke 脚本静态方法 int(IntPtr,int)
Vm::scriptUnload    → BootUnload               ALC Unload + weakref 排空，返回是否完全收集
```

**两条入口路径**（同一脚本程序集，按宿主形态二选一）：

| 路径 | 取用方式 | 方法要求 | 适用 |
|---|---|---|---|
| collectible（热更） | BootCall 反射 Invoke | 普通 `static int(IntPtr,int)`，**禁** `[UnmanagedCallersOnly]` | 引擎内运行时 |
| native（函数指针） | `getEntryPoint`（默认 ALC） | `[UnmanagedCallersOnly]` 包装（示例命名 `EntryNative`） | 工具进程一次性调用 |

**失败兜底语义**：卸载窗口内脚本命令 UnknownCommand、ECS 导出回 NoWorld（错误码
1）、cvar 读写正常（全在 C++）；reload 中途失败（编译错/加载错）时旧环境已卸载，
重新 `reload-script` 指回上一个好 DLL 即恢复——脚本无跨帧状态，无回滚包袱。

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

## 踩坑存档（M-GL4 实测）

1. **hostfxr 双副本陷阱（本轮核心坑，实锤）**：同一脚本 dll，bootstrap
   `LoadFromStream` 装进 collectible ALC 之后，再对它调 `Vm::getEntryPoint`
   （hostfxr `load_assembly_and_get_function_pointer`）会把程序集**再装一份进
   默认 ALC**——两份副本静态字段独立一套，collectible 副本里 `Engine.Init`
   写的函数指针在默认副本里全是 `IntPtr.Zero` → 默认副本 OnTick 第一帧读
   `_exists` 抛 ArgumentNullException（SEH 0xe0434352）。**规则：凡走
   bootstrap 装载的脚本，一切入口（Entry 反射 + OnTick 指针）都必须从
   collectible 正本取**——OnTick 经新导出 BootTickPtr（反射解析 +
   `UnmanagedFunctionPointer` 委托 thunk + `GetFunctionPointerForDelegate`，
   thunk delegate 存 static 保活列表、Unload 前清空防 rooted 挡回收）。
2. **`Assembly.GetType` 不吃 `", Asm"` 限定符**：`"Ns.T, Asm"` 返回 null、
   裸 `"Ns.T"` 才命中（hostfxr getEntryPoint 的类型参数口径**吃**限定符，
   两条链口径必须统一兼容——BootCall/BootTickPtr 入口先原样再剥限定符重试）。
   诊断：最小 C# 探针 stream 装载 + 两种格式对照（qualified NULL / plain OK）。
3. **OnTick 是可选契约**：缺失 = tick 空转（hello/console/ecs 旧脚本零波及）。
   首版误设「必须存在」打爆 m-sc3（8 FAIL）即回改——「旧脚本零波及」是
   脚本面任何新功能的硬约束。
4. **`ScriptHost::loadScript` 必须校验 Entry 返回值**：scriptCall 的 Result.ok
   只代表 BootCall 协议层成功，脚本 Entry 返回 1（如目标实体不存在）会被吞出
   「script: loaded」假绿 → OnTick 每帧返回 2 刷失败。非 0 = 回滚卸载。
5. **`ecsSetWorld` 必须先于 loadScript**：Entry（装载期）就会 FindByName，
   未绑 World = NoWorld → Entry 返回非 0（与坑 4 连锁直接装载失败）。
6. **Assembly.GetType 限定符坑的同族**：dll 名从 runtimeconfig 推导时
   `rtc.stem()` 只去 `.json` 残留 `.runtimeconfig`（`X.runtimeconfig, X.runtimeconfig`
   → -101）；正解 = 按 `.runtimeconfig.json` 全后缀（19 字符）截断。
7. **nl-script selftest 并行互踩（回归 flake 治理）**：selftest 的 s5/s8 temp
   目录固定名（`nl-script-selftest-hot[-s8]`）——regression.py `--jobs 4`
   同 case 双后端并行时两进程互踩 → 间歇 exit 1。temp 名加 `_getpid()` 后缀
   根治。同族：**25ms 步长落 2×step=33.333ms 浮点刀锋**（舍入方向不定，
   孪生复推与实跑可能不一致），对拍用例选离边界远的钉值（30ms → [1,2,2]）。

---

# M-SC4：绑定导出生成器（exports.api 单源，2026-11-14）

## 机制口径

| 层 | 谁写 | 说明 |
|---|---|---|
| ① 声明 | 手写（唯一声明） | `script/script-api/exports.api`：`域 \| 名 \| 返回 \| 参数 \| 文档` 单行 + `type 名` POD 段 |
| ② C++ 接线 | 生成 | `ApiTable.g.hpp`（结构+POD 镜像）/ `.g.inc`（注册行）/ `.g.asserts.inc`（防漂移） |
| ③ C# 接线 | 生成 | `Wiring.g.cs`：delegate + 指针字段 + Init 偏移 + `NativeMarshal` 生命周期原语 |
| ④ C# facade | 手写薄层 | 返回语义/错误码含义在场；marshal 禁私搭，一律用 `NativeMarshal` |
| ⑤ C++ 实现体 | 手写 | 引擎语义本体（功能本身） |

- **TYPEMAP + 生命周期账本机制单源**（gen.py）：in str = AllocHGlobal + finally 必释放；out str = C++ 静态缓冲「下次同线程调用前有效」+ C# 立即拷贝；命令回调 outText = CoTaskMem 配对（SEH 路径含）；delegate = `_pinned` 保活（script-api 装在 collectible ALC，随 Unload 整体回收——UnrealSharp per-ALC 归组同款，无需额外归组表）；POD = 值/栈传递不跨堆；宿主活实例 = 显式 set/unset + 未绑错误码兜底。
- **硬规则**：C++ 侧永不跨 reload 缓存托管引用；type 段只收纯 POD（数值 + 定长 buf，禁字符串/嵌套引用——gen.py 解析即拒，非 POD 显式拆参数）；动态容器不过界走 count+ptr 协议（argv 先例），字典分解为单值导出/双列表/快照串。
- **防漂移双闸**：`.g.asserts.inc` 每导出一条 `static_assert(is_same_v<decltype(&nl_xxx), 声明签名>)`（编译期红）；CMake -P `check_freshness.cmake` 时间戳闸（exports.api 新于生成物 = 构建红，零 Python）。
- **gen.py --check**：内存重生成与入库产物逐字节比对（golden 自证；M-SC6 起 5 份——C++ 三件 + Wiring.g.cs + docs/script-api/API.md，单源第三消费端 = API 文档，Flax 先例）。
- 先例：Flax `API_` 宏 + Flax.Build 双端生成 / csbindgen 产物入 git / UnrealSharp property translator；cocos bindings-generator 的 libclang 路线不需要（绑定面是纯 C ABI 函数表，无类/继承/模板）。

## 声明格式速查

```
#   参数表逗号分隔，每项「[in|out] 类型 名字」；类型词表（TYPEMAP 单源在 gen.py）：
#     int/u64/f32/f64/u32 = 标量；out = 指针
#     str = const char*（UTF-8）；out str = const char**（静态缓冲语义，见上）
#     bufN = 定长 float 数组指针；ptr = void* 不透明载荷
Time | timeGetState | int | out f64 simTime, out f64 frameDt, out u64 tickCount, out int paused | ...
type TransformData
    f32 px, py, pz      # position
    ...
```

改声明 → `python tools/scriptapi/gen.py` → 提交生成物（构建不依赖 Python；API.md 同批刷新——文档禁手改，签名/生命周期列由声明与 TYPEMAP 账本单源渲染）。

# M-SC5：Time / Input / Log 域面（ApiTable 16 -> 25，2026-11-14）

## 绑定面口径（nl/script/Bindings v3）

| 域 | 导出 | 关键口径 |
|---|---|---|
| Time | `timeGetState` / `timeSetState` | 绑宿主 `GameClock`（`timeSetClock`）；一次读回 simTime/frameDt/tickCount/paused；SetState 哨兵（-1=不变）；未绑 = `kTimeNoClock` |
| Input | `inputActionFind` + `inputActionBool/Float/Vec2` + `inputKeyIsDown` | 绑 `InputState`+`ActionMap`（`inputSetState`/`inputSetActionMap`）；ActionId = u32，冻结值三型；类型不符 = `kInputNoAction`；KeyIsDown 越界 = BadArg；未绑 = `kInputNoSource`（**不隐式建表**） |
| Log | `logWrite` / `logSetFilter` | `nl::core::log` 全局面，通道 `script`（stdout `[nl:level|script]` 契约）；level 0..5=Trace..Fatal 越界 BadArg；非法 filter 指令 = `kLogBadInstruction` 带原因 |

宿主 set 绑定族（M-SC2 `ecsSetWorld` 模式推广）：`timeSetClock / inputSetState / inputSetActionMap`——入口调用前 set，未绑窗口 = 专属错误码（热更兜底，M-SC3 口径同构）。viewer scene 模式只绑 Clock（无注入管线，Input 有意保持未绑）。

## selftest 判据（s9 组，s0~s5 + s6 + s7 + s8 不回归）

- s9a Time：tick x3 → simTime 0.05 / tickCount 3；SetState(1,-1) 暂停 + timeScale 哨兵保持；解绑 → NoClock
- s9a2 哨兵边界：paused=-1 不变、timeScale=-1 不变（显式 0/1 正常生效）
- s9a3 Log：`[nl:info|script]` 通道锚；SetFilter("nl:warn") 生效 + readback；非法指令显式错带原因
- s9b Input：ActionFind 三型冻结值（W/D 喂键 Bool/Float/Vec2）；类型不符 NoAction；KeyIsDown 越界 BadArg；解绑 → NoSource

## 验收复现

```
scripts\build-m0.2.bat DX12
example\m-sc5\run.bat          # selftest（5 示例目录）+ s9 特写 + log 通道锚 + check + 4 张证据图
```

## 踩坑存档（M-SC4/M-SC5 实测）

1. **TYPEMAP 首跑即抓 `out u64` → `int*`**：ecsSpawn/GetParent/FindByName 三条映射错——防漂移断言闸当场编译红。闸的价值 = 把「selftest 兜底」升级成「编译失败」。
2. **ActionMap 定义在 `action_map.hpp` 不在 `input.hpp`**：两个 TU（Bindings.cpp / nl-script main.cpp）各漏一次 include。
3. **nl_core 对 glfw 是 PRIVATE link**：include 面不传播——nl-script main.cpp include `input.hpp` 拉 GLFW 头失败；nl-script CMake 显式 `PRIVATE glfw`（viewer 同款）。
4. **reloadScript OnTick 重绑误用 `getEntryPoint`（M-GL4 口径漏改）**：热更路径会把脚本程序集再装一份进默认 ALC（两份副本静态字段独立）；loadScript 走 bootstrap `scriptTickFn`、reloadScript 漏改——M-GL4 踩坑 1 的同族复发，已修并随 M-SC4 selftest 验证。
5. **bat 里 python -c 的坑三连**：`%` 格式化被 cmd 展开（`%d`→变量展开吞字符）；LF 行尾 bat 解析不可靠（统一 CRLF）；`echo`/参数里裸 `|` 被切管道（`^|` 转义或改 findstr）。

# M-SC6：Camera 域面（ApiTable 25 -> 29，2026-11-14）

## 绑定面口径（nl/script/Bindings v4）

| 域 | 导出 | 关键口径 |
|---|---|---|
| Camera | `cameraGetPose` / `cameraSetPose` / `cameraGetFov` / `cameraSetFov` | 绑宿主相机适配源（`cameraSetSource(CameraSource*)`，4 函数指针 + ctx）；`CameraPose` POD（eye[3]+yaw+pitch+fov+mode）；未绑 = `kCamNoSource`；GetFov 未绑 = **NaN**（f32 返回无 rc 通道，生命周期账本特例，exports.api 行内文档化）；SetFov clamp [20,120]（同 OrbitCamera，脚本不得与宿主 fov 持久化打架） |

**为什么是 CameraSource 适配源而不是裸 CameraRig**：nl_core 不能反向依赖 viewer（依赖方向），裸 rig 会丢 fov 持久化 + orbit 控制；plan 里 `cameraSetRig` 是落码前占位名，定稿 `cameraSetSource`（同契约：宿主显式 set，ecsSetWorld 模式推广，不隐式建表）。**模式感知 SetPose 语义在 viewer 适配源实现**（不在绑定层）：orbit = `setOrbit(yaw,pitch,|eye|)`+seed / free = `setPose` / chase = `snapEyeTo`+pitch（yaw 由底盘驱动）/ track = `ModeUnsupported`（轨迹数据主权——读路径无模式限制，写路径拒）。

## selftest 判据（s10 组，s0~s5 + s6 + s7 + s8 + s9 不回归）

- s10a 绑定路径：罐头位姿读回锚（eye 1.5/2.5/3.5、yaw 30、pitch -10、fov 60、mode=orbit）+ SetPose rc Ok（C++ 捕获 {4,5,6,45,15}）+ null eye → BadArg + fov 往返 60→75→clamp 500→120（假源存 clamp 后值）
- s10b track（mode=3）：GetPose 照常（读无模式限制）、SetPose → ModeUnsupported、SetFov(60) Ok（fov 模式统一生效）
- s10c 解绑路径：cameraSetSource(nullptr) → GetPose/SetPose/SetFov = NoSource、GetFov = NaN，不抛异常不崩

## 验收复现

```
scripts\build-m0.2.bat DX12
example\m-sc6\run.bat          # gen --check（29 导出）+ selftest（6 示例目录）+ s10 特写 + check camera + 4 张证据图
```

## 踩坑存档（M-SC6 实测）

1. **`out f32`（cameraGetFov）没有 rc 通道**：POD/引用参数靠 rc 传错误码，f32 直接返回值——未绑语义只能用 NaN 表达，账本特例写进 exports.api 文档列而非 TYPEMAP 例外。
2. **gen.py 重生成顶掉 exe mtime → 回归过期门禁拦**：内容字节一致也会被拦（闸按 mtime）——重编放行，闸不是摆设（M-SC4 已实证过一次，本条为第二次）。
3. **python subprocess 调 build bat 两坑**：`text=True` 用 UTF-8 解码 nmake 的 GBK 输出崩 reader 线程（改 bytes + gbk/errors='replace'）；`'scripts\build-...'` 单反斜杠被 Python 吃成 `\b` 退格（`'scriptsuild'`）——用 r-string。

---

# M-SC7：ECS 模型挂载（v5）——拍板方案 A + 白名单单源 + initScene 可重入

## 绑定面口径（nl/script/Bindings v5）

| 导出 | 签名 | 语义 |
|---|---|---|
| `ecsAttachModel` | int (u64 id, str meshFile) | 挂 ModelComponent；白名单 + Transform 前置 + 重复挂拒绝。rc：0=ok / 1=NoWorld / 2=NoEntity / 3=重复挂（Component）/ 4=BadArg（空名）/ 5=NoTransform / 6=BadAsset（白名单拒） |
| `ecsDetachModel` | int (u64 id) | 摘 ModelComponent。rc：0=ok / 1 / 2 / 3=无 Model 可摘 |
| `ecsModelInfo` | int (u64 id, out str meshFile, out buf3 tint, out f32 roughness, out f32 metallic) | 字段读回；meshFile = 静态缓冲账本（下次同线程调用前有效，C# 立即拷贝）；tint/roughness/metallic 可 NULL 跳过，meshFile 指针必填 |

拍板方案 A（2026-11-14，用户拍板）：**binding 只做组件增删 + 校验，渲染侧帧边界整场景重建**——viewer 每帧 tick 后比对 Model 集合签名（实体 ID + meshFile，`world.getEntityIDs()` 出现序稳定），变了重跑收集 + `loadEcsScene`。实例矩阵每帧更新走既有 M-GL3 插值链不动。Carrot/bevy/UE 三处参考都是「组件增删自动传播」，但其架构（每帧收集/每组件 proxy）与本项目 E1b 静态 batch 不同——方案 A 是 E1b 架构内的最小传播实现，机器出口锚 = `ecs scene rebuilt: N objects (frame-boundary, script attach/detach)`。

防崩语义三件（拍板）：无 Transform = `kEcsNoTransform=5` 拒绝（E1b「Model without Transform = 拒」同口径，不隐式建表）；重复挂 = `kEcsComponent=3` 拒绝（**World::addComponent 同类型重复是 assert，binding 不挡就崩进程**）；白名单拒 = `kEcsBadAsset=6`（坏名不落 World）。运行期重建时白名单/Transform 不合格实体 = 逐实体跳过 + 显式日志（与初始 `--scene` 硬失败退出 2 口径不同——帧边界不致命）。

## 白名单单源（M-SC7 收编）

`nl::scene::validAssetFileName(name, ext)` / `validGlbAssetName(name)`——`core/src/scene/AssetName.cpp`。历史四处复制（renderer.cpp 原版 / scene.cpp 兜底 / viewer lambda / SceneGeo validMeshName）全部收编；统一语义 = 原版（可打印 ASCII 0x21..0x7E 无空格 + 无路径分隔符 + 无 ".." + 指定后缀）。**SceneGeo 放行字符集变宽**（原 alnum+_-\.），路径穿越防护不变，取舍已写 AssetName.hpp 头注。新增消费面一律走 helper，禁再复制。

## initScene 可重入（重建的防泄漏前提）

`Scene::initScene` 原来不可重入（成员句柄直接重建不释放 → 二次调用整份泄漏）。新增 `Scene::shutdownEcsScene()`：initScene 建立的全部 GPU 句柄（场景管线/sceneBatches_/灯缓冲/中性 PBR 兜底纹理/阴影链）子集释放，initScene 开头调用 + shutdown() 复用同一清单（原 M-E1b.0 块原位收编，语义逐位不变）。**新场景资源进 initScene 时必须同步进 shutdownEcsScene 清单**——与「Scene 新增资源必须进 shutdown 清单」同级纪律。

## selftest 判据（s11 组，s0~s5 + s6 + s7 + s8 + s9 + s10 不回归）

- s11a：Spawn+AddTransform+AttachModel(cube.glb) → ModelInfo 全字段读回（meshFile/tint 缺省/roughness/metallic）→ C++ 终态核：ModelComponent 落进罐头 World 且 meshFile=cube.glb。
- s11b：错误码逐一对齐——NoEntity=2（attach/detach/info 三导出）、BadArg=4（空名）、BadAsset=6（../secret.glb / model.gltf / dir/model.glb，且**坏名不落 World**：ModelInfo 回 Component）、NoTransform=5、重复挂=3（且拒绝不改状态：meshFile 仍是首挂值）、无模型摘=3。
- s11c：ecsSetWorld(nullptr) 后三导出全 NoWorld 不崩，重绑恢复。
- s11d：DetachModel 后 ModelInfo=Component、C++ 核 Model 摘净 + Transform 留守、再摘=Component。

## 验收复现

`example/m-sc7/run.bat`：gen --check（32 exports）→ selftest 全量（含 s11）→ s11 特写过滤 → check model → 证据图重生成 → ALL GREEN。`python tools/regression.py --only m-sc7` 8 卡双后端。

## 踩坑存档（M-SC7 实测）

1. **gen.py C# Init 偏移 bug（本轮最有价值发现）**：偏移按域首现序跑连续计数器，往非末尾域尾部追加导出时后继各域整体错位（表本体一直是声明序所以 struct 没坏、M-SC5/SC6 都在末尾域追加所以从未暴露）。修复 = 每导出声明序全局下标；域注释偏移段合并显示（Ecs = "6..15,29..31"）。**生成器隐性假设（「域组连续」）要写进不变量或断言，不能靠运气**。
2. **getComponent 返回基类指针**：`World::getComponent(id, typeID)` 返回 `Component*`，取字段必须 `static_cast<const ModelComponent*>`——templated 重载 `getComponent<T>(id)` 才内建 cast。漏 cast = C2039（编译闸抓，收口前红一次）。
3. **Api.cs 手写 facade 用 Vector3**：`System.Numerics` 不在 ImplicitUsings 自动集（工程显式 disable），要显式 using。
4. **newEntity 延迟落表**：实体进 `entitiesToAdd_` 队列，`findEntityByName`/C# `FindByName` 只扫 `entities_`——罐头 World 不 tick 就不 flush，按名查找族全空（组件按 id 键控的 hasComponent/getComponent 不受影响，所以出现「attach 全过、按名核全红」错位假象）。宿主终态核前显式 `flushEntityCreationAndRemoval()`。
5. **out buf 传 null = 「跳过该字段」不是「填零」**：ModelInfo 全字段重载首版把 null 传 tint 指针，C# 侧读回恒 (0,0,0)——facade 要读就传实缓冲（stackalloc）。
6. cwd 漂移读错树：开工实读阶段三条 grep 落主仓得出「s9/s10 不存在」假象——pwd 必看。

## M-SC8 Physics 载具域（v6，ApiTable 32->37，2026-11-14）

### 绑定形态（拍板 Q-SC8-1 A'：直持 DebugScene*）

`driveSetScene(nl::physics::DebugScene*)`（Bindings.hpp，ecsSetWorld 模式推广）：
绑定面**直持** M-NET2 已落的 `physics::DebugScene` PIMPL 公开类，五导出逐字透传
`addVehicle/removeVehicle/setVehicleInput/vehicleFullPose/teleportVehicle`——
P8-SCRIPT-PLAN L107 既定口径「直接对齐不新造」。**CameraSource 函数指针表是四模式
相机专属形态（ViewerCamera 私有头 + orbit/free/chase/track 模式感知），DebugScene
是单形态具体类，再包一层适配源属过度设计**——开工摆方案时曾按 M-SC7 台账「CameraSource
式适配源」锚拍过一版函数指针表，披露计划口径冲突后重拍 A'（教训：M-SC7 下一步句
是方向性指针不是既定拍板，与计划文档正文冲突时以计划正文为准并披露）。

| 域 | 导出 | rc |
|---|---|---|
| Physics | physicsAddVehicle(spawnX, spawnZ) -> slot | 0=ok / 1=NoScene / 3=AddFailed（lastError 置因）|
| Physics | physicsRemoveVehicle(slot) | 0=ok / 1=NoScene / 2=NoSlot |
| Physics | physicsSetVehicleInput(slot, fwd, right, brake, handBrake) | 0=ok / 1=NoScene / 2=NoSlot |
| Physics | vehicleFullPose(slot) -> pos+quat+vel | 0=ok / 1=NoScene / 2=NoSlot |
| Physics | teleportVehicle(slot, pos, quat) | 0=ok / 1=NoScene / 2=NoSlot / 4=BadArg |

- 错误码族 `kPhyOk=0 / kPhyNoScene=1 / kPhyNoSlot=2 / kPhyAddFailed=3 / kPhyBadArg=4`；
  槽位有效性探针 = `vehicleChassisState(slot).valid`（used && chassis 同条件，
  tombstone 后即 false——M-NET2 内部静默忽略非法槽，绑定面显式回 NoSlot）。
- `buf4`（quat xyzw）= TYPEMAP 首个 4-float buffer；`BUF_RE` 泛型正则天然覆盖任意宽度。
- 线程口径 = OnTick 逻辑帧约定（Q-SC8-2 A：**注册方 step**——脚本 OnTick 写四输入，
  当帧 `physicsScene.step` 消费；runScene 组序 PrePhysics scriptTick → Physics step
  同语义）。输入与读键解耦（Q-SC8-3 A）：Input 域读键 + `Physics.SetVehicleInput`
  写值，映射脚本侧自由。

### viewer runNormal 接线（--script 在物理训练场模式可用）

runScene 的 M-GL4 装载块**复制精简**（scene 模式脚本链是已验收冻结面，零改动）：
产物定位 / Vm::create / init / loadScript / typeName 推导同款。绑定链差异：
`timeSetClock(&gameClock)` + scenario 2 才 `driveSetScene(&physicsScene)`；
**ecsSetWorld / cameraSetSource 不绑**（runNormal 无 ECS World 与 ViewerCamera，
脚本调 Ecs.*/Camera.* 得 NoWorld/NoSource = 不隐式建表拍板同构的正确语义）。
收尾次序铁律同 runScene：解绑场景/时钟 → unloadScript → 析构 Vm。

### selftest s12（drive 示例探针，真训练场）

`s12a`（BoundProbe，DebugScene build(2) 真训练场 + C++ 终态核同实例）：AddVehicle
slot=1（slot0 = 既有单车）→ 四输入写（重复写覆盖语义）→ FullPose 含 out-NULL 跳过
路径 → Teleport 逐位读回（DontActivate）→ RemoveVehicle tombstone 后三导出全
NoSlot 且 slot0 不波及；C++ 核 slot0 valid / 脚本车 invalid。`s12b`：driveSetScene
(nullptr) 后五导出全 NoScene 不崩。

### 踩坑存档（M-SC8 实测）

1. **C# stackalloc 三元表达式**：`cond ? stackalloc float[3] : null` 推成
   `Span<float>` 不能隐式转裸指针（CS0029）——先 stackalloc 到局部再条件赋值指针。
2. **M-SC7 台账「下一步」句与计划正文口径冲突**：M-SC7 写「CameraSource 式适配源」，
   P8-SCRIPT-PLAN L107 写「直接对齐 M-NET2 面不新造」——开工摆方案时先按前者拍了
   一版，摆完才发现后者；**披露冲突重拍**（结论：计划正文赢，台账「下一步」只是
   方向性指针）。里程碑台账的「下一步」句禁止当既定拍板引用。

## M-SC10 属性面板域（v7，ApiTable 37->39，2026-11-14）

### 拍板（Q-SC10-1~4 = A/A/A/A，2026-11-14）

载体 = **静态字段面板**（入口脚本类 `[Prop]` 静态字段反射枚举；M-SC2b
ScriptComponent 维持默认不做——零实例零生命周期，计划行内「C# 组件字段」
在静态入口模型下落为「脚本类静态字段」）；类型集 = float/int/bool/string
（kind 0..3）；挂点 = `--prop-panel` additive 独立窗口默认关（runScene/
runNormal 通用）；交代物全要。

### 双通道架构（生命周期硬规则①的落地）

| 通道 | 方向 | 载体 | 生命周期 |
|---|---|---|---|
| 描述表 | C# → C++ | `propsPushBegin`（gen+1 = 面板重建信号）+ `propsDescAdd(kind, name, min, max, def)` 推 POD 表（`PropDesc{kind, name[64], min, max, def}`，上限 256） | C++ 只存 POD；FieldInfo 反射缓存留 script-api 程序集内随 ALC 回收 |
| 值读写 | C++ → C# | **BootTickPtr 协议复用**——按名解析可选契约 `PropGet`/`PropSet`（public static int(IntPtr,int)），ScriptHost::propGet/propSet 包装 | 与 tickFn_ 同纪律：缺失 = 通道惰性；reload 重取；unload 前置空（thunk 保活列表 Unload 清空白得——bootstrap 零改动） |

脚本侧三行契约（PropsScript 示例同款）：

```csharp
Entry 内开头：  PropsPush.PushStatic(typeof(MyScript));
public static unsafe int PropGet(IntPtr a, int s) => PropsPush.PropGet(typeof(MyScript), a, s);
public static unsafe int PropSet(IntPtr a, int s) => PropsPush.PropSet(typeof(MyScript), a, s);
```

- 值 ABI：buf[0..3]=int32 idx（C++ 包装填）；标量 buf[4..7]=f32/i32/b(0/1)；
  string buf[4..259]=UTF8 尾 0（get 截断 255，set 读到首个 0）。
- Default 不单独声明 = 推送时字段现值快照（防双源漂移）；字段序 =
  MetadataToken 升序（GetFields 无序，取稳定序；**reload 后行序会变**——
  面板按 gen 重建、工具侧按 kind 定位，禁硬编码 idx）。
- `[Prop]` 无 Range 形态 = min==max = 面板退直输控件（SliderFloat/SliderInt/
  Checkbox/InputText 四态）。

### viewer 接线（--prop-panel，runScene/runNormal 共用）

- 帧外持久 `PropPanel::State` + `propPanelSync()` 共用助手：描述表 gen 变化 →
  重建行（初值经 propGet 拉）；dirty 行 → propSet 清边沿；active/状态行按
  通道与表态回填。
- **State 是 HudFrame 值成员**：帧前整份拷入 `hud.propPanelState`、帧后整份
  拷回（drawWindow 编辑落在 hud 副本，不拷回 = 编辑丢失——M-T5 SignalPanel
  教训同族：只挂指针 = 面板渲染全零）。
- 主面板可见性：runScene 补 `hud.panelShow = o.hudEnabled`（M-C2 缺省可见
  收敛为显式门）——`--no-hud + --prop-panel` = 只画 script props；profiling-hud
  的 M-TR0 抑制语义不变。

### selftest s13（props 示例探针）

`s13a` 描述表 4 行 kind/Range/default 逐位 + 通道接通；`s13a2` 四类型值往返
+ C# 真源核（ReadField 探针经 bootstrap BootCall 读活静态字段——**禁
getEntryPoint 直拉脚本 dll**，M-GL4 口径）；`s13b` idx 越界 rc 8 / 坏缓冲 rc 2；
`s13c` 热重编加字段（copy → 改源 → dotnet build 同名覆盖 → reload）→ 表
4->5 + 契约重取；`s13d` **万次 string 串往返 + GC.Collect 探针句柄漂移 <32**
（s8f 手法扩展，生命周期账本硬规则②新域必带）；`s13e` unload 通道前置空 +
POD 表 C++ 留存。

### 踩坑存档（M-SC10 实测）

1. **C++ propSet staging 拷贝源错位**：`memcpy(staging+4, buf, size-4)` 拷的是
   调用方 buf 的 [0..3]（idx 位），而调用方 ABI 值区在 buf+4——set 全零（实测
   propGet 回 0.0）。修 = 从 `buf+4` 取。教训：**「包装填 idx + 值区偏移 4」
   的双段 ABI 在跨端两侧各写一遍，逐字对照必做**。
2. **.NET 8 禁反射 Invoke `[UnmanagedCallersOnly]` 方法**（InvalidProgram:
   attempted to call a UnmanagedCallersOnly method from managed code）——
   BootCall/BootTickPtr 走反射的契约方法（Entry/PropGet/PropSet/探针）一律
   **不带 UC**；UC 只用于 getEntryPoint 函数指针路径（hello EntryNative 同款
   分工）。CreateDelegate 路径（BootTickPtr）UC 可用但非必需。
3. **reload 后行序随 MetadataToken 变化**：热重编把新字段插在旧字段声明前 →
   idx 位移——工具/测试按 kind 或名字定位，禁硬编码 idx（实测 4 字节 string
   比特写进 int 字段，get 回 "leak"）。
4. **C1090 PDB API 0x6BA** 二连：mspdbsrv 瞬时故障，重跑即绿（M-SC8 踩坑②
   复现，非代码错）。

## M-SC11 整轨收口：跨域示例脚本（v0.9.0 终态，ApiTable 39 不变，2026-11-14）

### 拍板（Q-SC11-1~3 = A/A/A，2026-11-14）

① 示例域集 = **五域+Props**（计划行 Time+Input+Ecs+Log+Camera 加 M-SC10
Props；Physics 不进——训练场模式与 Camera/Ecs 互斥，见下表）；② merge 回
master 单独拍板不入本轮（S13 教训：merge 后重跑全历史卡）；③ GUIDE = 新节
+ 既有 M-SC4~SC10 各节一致性终检。

### 双模式域可用性约束（跨域示例的物理前提）

| 域 | runScene（scene 模式） | runNormal（训练场模式） | 绑定点 |
|---|---|---|---|
| Time / Input / Log | Time+Log ✅ / Input ❌(NoSource) | 同左 | timeSetClock；Input 双模式均不绑（不隐式建表口径） |
| Ecs（挂载） | ✅ ecsSetWorld | ❌ 不绑 | World 未绑 = EcsCode.NoWorld |
| Camera | ✅ cameraSetSource | ❌ 不绑 | 未绑 = CameraCode.NoSource |
| Physics | ❌ 不绑 | ✅ driveSetScene | 未绑 = PhysicsCode.NoScene |
| Props | ✅ | ✅ | 描述表导出 + BootTickPtr 值通道 |

TourScript（`script/examples/tour/`，类 `NlScript.Examples.TourScript`）=
单模式单脚本六域打通的展示样例：Entry 装载期六域锚定（PushStatic 描述表 +
Spawn/Flush/AddTransform/AttachModel(TorusKnotDense.glb) + Log 固定文本锚 +
Input NoSource **负向锚** + Time/Camera 读通），OnTick 相机环绕
（yaw += SpinSpeed*dt，SpinEnabled 可经面板实时关停 = Props 值通道活演示）
+ sweep 周期锚（Ecs.FindByName+Exists 复核 → 固定文本 log）。

### viewer 复现（m-sc11 卡）

```
viewer.exe --scene example/ecs-e1b/ecs-e1b.scene --tick parallel --fixed-dt 16.7 ^
  --script script/examples/tour --script-bootstrap script/bootstrap/bin/Debug/net8.0/NlScriptBootstrap.dll ^
  --prop-panel --no-hud --screenshot <out.png> --frames 8 --size 960x540 --yaw 12 --pitch 9
```

验收锚：log `tour: entry ok (props+ecs+log)` / `tour: input NoSource as
designed` / `tour: sweep ok (…)` + `ecs scene rebuilt: 6 objects
(frame-boundary, script attach/detach)`（M-SC7 方案 A 视觉兑现——脚本 attach
的模型帧边界重建入镜）。130 帧版（tour-sweep 卡）相机转 ~32°，tick 0/120
两条 sweep 锚，fixed-dt 确定性 bytes 基线双后端分文件。

### 踩坑存档（M-SC11 实测）

1. **scene 模式默认 `--tick off`**：不带 `--tick parallel` 则 GameClock 不
   推进 → OnTick 零执行、ticks=0、Model 签名比对不触发、attach 的模型不入镜
   （PNG 与无脚本版逐字节同）。脚本示例卡必须带 `--tick parallel --fixed-dt
   16.7`（gl4 tick 卡先例同款）。
2. **OnTick 的 args = `double*`（逻辑步长秒），size = 8，不是 ApiTable\***：
   OnTick 里调 `Engine.Init(args)` 把 dt 位型当表指针解引用 = AV（SEH
   0xe0434352）。Engine 静态指针由 Entry（反射通道，同 ALC 静态共享）装载期
   初始化一次，OnTick 直接用 facade——orbit 示例同款契约。
3. **Spawn 后按名查找必须先 Flush**：name lookup 族只扫已 flush 实体
   （newEntity 延迟落表，s7 时代同坑）；tick 0 的 sweep 分支 `0 % N == 0`
   首帧即触发 FindByName——Entry 尾补 `Ecs.Flush()` 根修。
4. **白名单校验 ≠ 文件存在**：selftest 罐头名 `cube.glb` 过 M-SC7 白名单
   （字符集校验）但 `assets/models/` 无此文件——viewer 真加载时 rebuild
   failed。示例挂载用真实资产（TorusKnotDense.glb）。


## M-W2A 波 2 绑定域第一段：Audio / Physics 查询 / Animation 三域（v9，ApiTable 54→72，2026-11-15）

冻结口径 `docs/script-api/WAVE2-PLAN.md`（2026-11-15 用户拍板方案①：三域一次做、
Q1 绑 QueryScene 探针世界、Q2 Animation 薄面、主线里程碑不建 track）。本轮三域
18 op 尾部追加，既有 54 op 语义零改动；`renderer/**`/`.slang`/`kStage`/PNG 基线
一行未碰。

### 注入点（ecsSetWorld 模式推广，nullptr = 解绑，不隐式建表全族口径）

| 域 | 注入 | viewer 接线 | selftest 接线 |
|---|---|---|---|
| Audio | `audioSetWorld(AudioWorld*)` | scene 模式 `--script` 装载段 `ensureAudioWorld()`（设备失败降级 offline） | offline AudioWorld（spatialization 开，无设备确定性） |
| Physics 查询 | `querySetScene(QueryScene*)` | 同批注入静态 fixture（build 失败 = 注 nullptr） | M-PQ0 夹具 `build()` 一次 |
| Animation | 无宿主实例（句柄表在 script 内部面） | — | 探针直接建/收 |

### Audio 域（6 op，M-SC9 既定行落地）

`audioPlay(wavPath,pos,looping,out id)` = createSource → configure(gain=1,looping)
→ setSourcePosition → startSource 复合链（M-S3 audio.play console 同款；失败即停
不留半配置源）；`audioStop/audioSetGain（保持既有 looping）/audioSetSourcePosition/
audioSetListenerPose/audioIsPlaying`。**wavPath = assets/audio 下 basename 白名单**
（AssetName 单源 ".wav"）+ core paths 双层解析（不依赖 cwd）。错误码 kAud*：
0=ok / 1=NoWorld / 2=句柄无效或 AudioWorld 拒绝（spatialization 未开照实回）/
3=createSource 或文件失败（细节走 script 日志通道）/ 4=BadArg。

### Physics 查询域（5 op，追加 Physics 域尾部）

`queryRaycast/querySweep/queryOverlap/queryOverlapHitAt/queryBodyCount`，绑
**M-PQ0 QueryScene 确定性夹具**（floor/boxA/boxB/boxC/sensor 5 静态体）——
**不是 viewer 现役物理世界，live 化属 M-PQ1，本文档与 API.md 均不得暗示场景查询**。
shapeKind 0=Sphere/1=Box/2=Capsule；quat x,y,z,w；layerMask = QueryFilter 位屏蔽
（ignoreBodyIds 空）。多命中容器协议（P8 既有口径）：queryOverlap 收 C++ 静态表
（「下次同线程调用前有效」，QueryScene 内部按 maxHits 截断），HitAt 按序读回。
validate*Query 先过（零向量/shapeKind 越界/maxT<=0/dims 非法 = kQryBadArg）。
错误码 kQry*：0=ok / 1=NoScene / 2=索引越界或表空 / 3=build 未就绪 / 4=BadArg。
s17 逐位锚：floor 竖直射线 distance=5 / fraction=0.05 / layer=0 / owner
`0x464c4f4f52000000`（"FLOOR"）；球扫掠命中 boxA（layer 1 / owner "BOXA"）；
万次级串往返零漂移。

### Animation 域（7 op，薄面句柄表）

落点 `core/src/script/AnimInstances.hpp/.cpp`（**script 内部私有面，nl/anim 公开面
零改动**——`AnimationStateMachine::transitionTo` 保持 private，不给 core 加
public `requestTransition()`，那是范围外新架构判定须回拍板）。句柄单源发号
（clip/sm 同表，>0），`animRelease` 双态擦除失效安全；clip 入口 = `loadGltfAnim`
（M-A4b），clipPath = assets/models 下 ".glb" basename 白名单。状态转移 = C++ 侧
按脚本规则表构造 `TransitionRule::canTake` lambda（判据 `params[paramIdx] >=
threshold`，paramIdx 越界恒 false），**std::function 不过 blittable 边界**；脚本只
`animSetParam` 写标量 + `animTick` 触发优先级转移（声明序 = 优先级）。规则数组协议
（调用期存活，C# pin）：`ruleMeta = int[r*3]`（from,to,paramIdx）、
`ruleVals = float[r*2]`（threshold,blendDuration）。pose 读回 = translation(3)+
quat xyzw(4)，创建即评估一次（t=0 初始姿态，未 tick 也有确定值）。
**热更钩子**：`scriptUnregisterAll()`（reloadScript 统一解绑点，s8c 全链已过）内
`animReleaseAll()`——句柄表 = 脚本自有状态随换载清空 + 发号器重置；AudioWorld/
QueryScene 是宿主实例**不在**此清（跨 reload 重绑定无损，s18c 复验）。
错误码 kAnim*：0=ok / 1=句柄无效 / 2=clip 加载或校验失败 / 3=desc 校验失败 /
4=BadArg / 5=路径白名单拒绝。

### selftest 判据（s16/s17/s18 组，s0~s15 不回归）

- s16a 12 断言：播放复合链 id>0 + 播放态真 + 播放中增益/位置/监听者 + 停止翻假 +
  白名单（带分隔符）/缺文件（SourceFailed）/未知句柄（Invalid）负向；s16b 解绑
  NoWorld 全族 6 断言。
- s17a 19 断言：BodyCount=5 + 射线六锚 + 层屏蔽 + 扫掠 owner + 重叠容器读回 +
  负向三连 + 万次串往返；s17b 解绑 NoScene 全族 5 断言。
- s18a 30 断言：加载/越界 index/白名单/缺文件（error 文本账本）/建状态机/参数驱动
  转移时序（当帧 alpha 从 0 起）/pose 手算锚（hip y=1）/万次 Tick/参数与骨骼下标
  越界/显式回收双态/GC.Collect 句柄漂移；s18b 无效句柄 7 断言；s18c/d 换载钩子段
  （Seed 句柄 1/2 → unregisterAll → Audio/Query 全链探针照常过 → 旧句柄失效 +
  发号器重置回 1）。

### 展示台（m-w2a-audio / m-w2a-query / m-w2a-anim 三卡）

每卡 usage / 正向 selftest / 负向（目录不存在 dotnet build 失败 exit 1）三起起步；
证据图 = `tools/make-w2a-evidence.py [domain]` 从真实 selftest stdout 渲染
（禁手绘）。复现：`example/m-w2a-*/run.bat`（ALL GREEN）。**位置参数注意**：
selftest 签名 audioDir=argv[13] 先于 queryDir=argv[14]，query/anim 卡必须连带
传入前面域的目录（query 卡 13 目录、anim 卡 14 目录全参）。

### 踩坑存档（M-W2A 实测）

1. **ptr 参数 TYPEMAP 是 `void*` 不是 `const void*`**：asserts.inc 防漂移直接
   static_assert 抓死（Anim.animCreateStateMachine）——in ptr 也是可写语义面，
   照 UiSetEventCallback 先例落 `void*`。
2. **if 块内 echo 文本带未转义括号 = cmd 静默吞块**：run.bat 首版
   `echo ... (NL_BUILD_MAINREPO=1 ...)` 在 if 块里把 `)` 当块闭合 → 整 bat
   秒退 rc=1 零输出。echo 文本禁裸括号（或 ^转义）。
3. **python 循环里 `open(p,'wb')` 先截断后写**：首个文件 write 抛异常 = 该文件
   留 0 字节（audio run.bat 被截空过一次）。批量改文件用「读全文→内存改→一次性
   原子写」或逐文件 try 包住 open+write。
4. **query 卡位置参数**：13 目录时 argv[13]（audio 槽）拿到 query 目录 → s16 用
   QueryScript.dll 找 AudioScript → 0x80131522。位置参数签名扩参后各卡目录数
   必须与签名对齐。
5. **共享证据渲染器读别域日志**：audio 卡阶段 query.log 尚不存在 → 渲染器按域
   参数化（`make-w2a-evidence.py audio`），单卡只渲染自己。
6. **锚行格式**：selftest 成功断言行是 `  ok  s16a ...`（ok 后双空格），证据图
   过滤器按 `ok + 域名包含` 匹配，别按单空格前缀。

### M-MOD0 迁移遗留顺手根修（非本轮改动面，单独留痕）

`modules/material-graph-tools/CMakeLists.txt` b4-graph-hot-reload 的
`sample.mg` 路径仍指根级旧目录（M-MOD0 迁移 modules/ 时未跟）→ cannot stat
假挂。改 `${NL_ROOT}/modules/material-graph-tools/sample.mg`，DX12/VULKAN
重配后 ctest 176/176 ×2 全绿。
