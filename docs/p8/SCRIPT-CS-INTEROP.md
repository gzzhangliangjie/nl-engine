# C# 脚本调用指南 —— 全流程与铁律（M-SC0~SC3 + M-GL4 实装口径）

> 本文是 C# 托管代码接入本引擎的**端到端操作手册**：怎么写脚本、宿主怎么装、
> 怎么接逻辑帧、怎么热更，以及全部已实锤的坑。踩坑细节的历史考证见
> `SCRIPT-TOOLS-GUIDE.md` 各里程碑「踩坑存档」；设计依据见 `P8-SCRIPT-PLAN.md`。

## 1. 全链架构（谁装谁、指针从哪来）

```
C++ 宿主（viewer / nl-script）
  │  Vm::create(*.runtimeconfig.json)          hostfxr 启动 .NET 运行时（FDD）
  │  Vm::loadBootstrap(NlScriptBootstrap.dll)  getEntryPoint 拉 4 个 [UnmanagedCallersOnly]
  │                                            （bootstrap 非收集、常驻 → 指针终身安全）
  ▼
NlScriptBootstrap.dll（常驻中转，C++ 只持有它的指针）
  │  BootLoad     → collectible ScriptLoadContext.LoadFromStream（脚本 dll + 本地依赖同 ALC）
  │  BootCall     → 反射 Invoke 脚本方法（Entry 通道，收 ApiTable*）
  │  BootTickPtr  → CreateDelegate 委托 thunk 带出 OnTick 本机指针（逻辑帧通道）
  │  BootUnload   → 清 delegate 保活 → NoInlining Unload → weakref 排空
  ▼
脚本程序集（collectible ALC，可整体卸载）
    NlScriptApi.dll（script-api，同 ALC 装载；Engine 函数指针表由 Entry 期 Engine.Init 缓存）
    框架程序集（System.* 等）→ 默认 ALC 共享，不进 collectible
```

两条铁律从图里直接读出来：

- **C++ 只许持有 bootstrap（常驻）的函数指针**。collectible ALC 内代码的指针
  Unload 后悬空——脚本侧指针一律经 BootTickPtr 中转，且宿主在卸载前置空。
- **默认 ALC 是单向门**：hostfxr `load_assembly_and_get_function_pointer`
  一律装进默认 ALC（不可卸载）。所以脚本程序集只能经 BootLoad 的
  collectible 通道装；详见铁律 R1（双副本陷阱）。

## 2. 写一个脚本（五步）

以 `script/examples/orbit/` 为模板（M-GL4 验收脚本）。

### 第 1 步：目录与命名（双锚约定）

- 工程目录放 `script/examples/<name>/`；**目录名决定缺省 typeName**：
  类名 = `NlScript.Examples.<目录名首字母大写>Script`。
- 类声明 `public static class <Name>Script`，命名空间 `NlScript.Examples`。
  想脱离约定就显式传 typeName（viewer `--script-type` / `loadScript` 参数）。

### 第 2 步：csproj（四个必设项）

```xml
<AssemblyName>OrbitScript</AssemblyName>          <!-- ① 必须 = 类名去 Script 后缀同源；dll 名 = 它 -->
<GenerateRuntimeConfigurationFiles>true</GenerateRuntimeConfigurationFiles>  <!-- ② 宿主靠 *.runtimeconfig.json 定位产物 -->
<AllowUnsafeBlocks>true</AllowUnsafeBlocks>       <!-- ③ OnTick 读 dt 指针需要 -->
<!-- ProjectReference → ../../script-api/NlScriptApi.csproj   ④ 互操作面 -->
```

产物定位规则（宿主代码同款）：glob `bin/Debug/net8.0/*.runtimeconfig.json`
→ dll 名 = rtc 文件名去掉 `.runtimeconfig.json`（**19 字符后缀截断**）拼 `.dll`。
**不能 `replace_extension`**——那会从最后一个点替换成 `*.runtimeconfig.dll`。

### 第 3 步：装载入口 `Entry`（反射通道，必写）

```csharp
public static int Entry(IntPtr args, int size)   // 注意：不许标 [UnmanagedCallersOnly]
{
    Engine.Init(args);                            // 缓存 ApiTable 函数指针（每次装载必须做，静态态 reload 重置）
    int rc = Ecs.FindByName("script-orb", out ulong orb);
    return rc == EcsCode.Ok ? 0 : 1;              // 返回 0 = ok；非 0 宿主回滚卸载
}
```

- **`Entry` 绝不许标 `[UnmanagedCallersOnly]`**——托管反射 Invoke 对带该
  属性的方法抛 `NotSupportedException`（hello/orbit 脚本同款结论）。
- 非 0 返回 = 宿主**回滚卸载**（M-GL4 起校验返回值，不再吞假绿）。

### 第 4 步（可选）：`EntryNative` 直调包装

只有走 `nl-script run`（getEntryPoint 直调通道）才需要：

```csharp
[UnmanagedCallersOnly]                             // getEntryPoint 通道必须标
public static int EntryNative(IntPtr args, int size) => Entry(args, size);
```

### 第 5 步（可选）：逻辑帧回调 `OnTick`

```csharp
public static unsafe int OnTick(IntPtr args, int size)   // 无需 [UnmanagedCallersOnly]（BootTickPtr 委托 thunk 覆盖）
{
    if (size < sizeof(double) || args == IntPtr.Zero) return 1;
    double dt = *(double*)args;
    // ... 每逻辑 tick 一次；写实体/CVAR，返回 0
    return 0;
}
```

- **可选契约**：不写 = 宿主 tick 空转，旧脚本零波及（M-SC0~SC3 脚本均无 OnTick）。
- `args` = 指向 `double dt`（逻辑步长秒，kStep = 1/60），`size` = 8。
- 调用时序：clock callback 内、`world.tick` **之前**（脚本 spawn/remove 排队、
  tick 开头 flush 落盘）；pause = callback 不触发 = 脚本同步冻结。
- **回调内不许抛托管异常**——全部 API 走错误码检查（EcsCode 0~4）。
  返回非 0 / SEH = 宿主**停用 tick**（防每帧刷屏），`reload-script` 恢复。
- 脚本静态态（相位、id 缓存等）reload 归零——**状态全在 C++** 口径。

## 3. 宿主侧装载流程（viewer `--script` 为例）

```
--script <projectDir>                ① dotnet build（外部 / run.bat / nl-script）
  glob rtc → 推导 dll                ② 见上文 19 字符规则
  Vm::create(绝对路径 rtc)           ③ 路径一律 fs::absolute（相对路径在
  │                                     load_assembly_and_get_function_pointer 一步可能 0x80131509）
  ScriptHost::init(vm, bootstrap)    ④ 缺省 bootstrap = script/bootstrap/bin/Debug/net8.0/NlScriptBootstrap.dll
  │                                     （M-GL4 起正道 = bootstrap 工程自己的 bin 产物，部署副本已删除）
  ecsSetWorld(&world)                ⑤ **必须先于 loadScript**——Entry 装载期就 FindByName，
  │                                     未绑 World = EcsCode.NoWorld → Entry 非 0（M-GL4 实证教训）
  loadScript(dll, typeName)          ⑥ 内部序列见下
  clock callback → scriptTick(dt)    ⑦ 每逻辑 tick，world.tick 之前
  收尾：ecsSetWorld(nullptr) → unloadScript() → scriptVm.reset()   ⑧ 逆序三步
```

`loadScript` 内部（已有脚本时先安全换载）：

```
tickFn_ = nullptr                      ① 先解绑指向旧 ALC 的指针（铁律 R4）
scriptUnregisterAll()                  ② 解绑脚本命令 thunk
scriptUnload()                         ③ weakref 校验
BootLoad(dll)                          ④ collectible ALC 装载（-3 = 旧脚本没卸干净）
BootCall(Entry, ApiTable*)             ⑤ 校验返回值，非 0 → 回滚卸载（不留半态）
BootTickPtr(OnTick)                    ⑥ 可选；失败只停用不回滚（缺失 = 合法）
```

## 4. 热更流程（reload-script）

```
改 .cs → dotnet build（可同名覆盖——LoadFromStream 不锁文件）
  → console.reload-script（ScriptHost::reloadScript）
      scriptUnregisterAll → scriptUnload（weakref 排空）→ 同路径重装 → Entry 重注册
```

- 重绑定 = 脚本自描述：新脚本自己重新 `consoleRegister` / 设 cvar（Carrot
  load-callback 同构）。C++ 侧不动：cvar 值 / 实体树 / 内建命令无损。
- OnTick 指针在 Entry 成功后自动重取（BootTickPtr）。
- 卸载判据从严：BootUnload 返回 1 = Unload 已调但 GC 未收干净。

## 5. 铁律（R1~R8，全部实锤入档）

| # | 铁律 | 后果（违反实测） |
|---|------|------------------|
| R1 | **禁对脚本 dll 调 `Vm::getEntryPoint`**（bootstrap collectible 装载后）。OnTick 只走 BootTickPtr | hostfxr 把程序集**再装一份进默认 ALC**，静态字段独立一套，Engine.Init 状态不通 → 默认副本 OnTick 读 `_exists=0` 崩（0xe0434352 ArgumentNullException）。M-GL4 核心新知 |
| R2 | typeName 统一 `NlScript.Examples.X, Asm` 带限定符格式；**`Assembly.GetType` 不吃限定符**（hostfxr 口径吃） | BootCall/BootTickPtr 内部剥限定符重试（已兼容），但自己绕过 bootstrap 直调 `Assembly.GetType` 会 NULL → -101 |
| R3 | `[UnmanagedCallersOnly]` 适用面：`EntryNative`（getEntryPoint 通道）**必须标**；`Entry`（反射通道）**禁标**；`OnTick`（BootTickPtr thunk）**无需** | Entry 误标 → 反射 Invoke 抛 NotSupportedException（-102）；EntryNative 缺标 → 0x80131513 |
| R4 | 卸载次序：**先清一切指向旧 ALC 的引用**（宿主 tickFn_ / 脚本命令 thunk / bootstrap delegate 保活列表），再 Unload；Unload 必须在 NoInlining 小方法内、weakref 由调用方排空 | delegate rooted 挡 GC → 程序集永不完全卸载（返回 1）；卸载与排空同方法 → weakref 永远 alive（M-SC3 实测） |
| R5 | 脚本 dll 装载必须 `LoadFromStream`，禁 `LoadFromAssemblyPath` | 后者在 Windows 锁文件 → `dotnet build` 无法同名覆盖 → 热更工作流必断 |
| R6 | hosting API 的 assembly/runtimeconfig 路径**一律绝对化** | 相对路径在 load_assembly_and_get_function_pointer 一步可能 0x80131509 |
| R7 | 每进程**一个** .NET 运行时 / 一个 Vm / 一个 ScriptHost（.NET 单运行时硬限制） | 重复 create = 未定义 |
| R8 | 产物定位：rtc glob + 19 字符截断推导 dll 名；`--script-bootstrap` 缺省值是仓库根相对路径，**cwd ≠ 仓库根时必须显式传**（回归 cwd=exe 目录，cases.json 卡一律 `{root}` 绝对传参） | dll 名推错 → BootCall -101；bootstrap 找不到 → init 失败 |

## 6. 错误码速查表

**bootstrap ABI 返回码**（`Bootstrap.cs` 头注释同文，写死防漂移）：

| 入口 | 0 | 负值 |
|------|---|------|
| BootLoad | ok | -1 空参/路径不存在 · -2 装载失败（流读取/依赖解析） · -3 已有脚本未卸载 |
| BootCall | = 脚本方法返回值 | -100 未装载 · -101 类型或方法解析失败 · -102 托管异常 |
| BootTickPtr | ok（`*outFn` = 本机指针） | -1 空参 · -100/-101/-102 同上 |
| BootUnload | 完全卸载（weakref dead） | 1 Unload 已调但 GC 未收净 · -1 无脚本 |

**hostfxr / SEH 层**（Vm.cpp / ScriptHost.cpp 边界）：

| 码 | 含义 | 常见原因 |
|----|------|----------|
| `0x80008093` | runtimeconfig 装载失败 | rtc 文件缺失 / 路径错 |
| `0x80131509` | InvalidOperationException（装载步） | 路径相对未绝对化；**刚重编完 dll 载入**——MSBuild/VBCSCompiler 常驻节点持句柄（先重跑 2~3 次，仍炸 `dotnet build-server shutdown`） |
| `0x80131513` | MissingMethodException | 入口方法缺失，或 getEntryPoint 通道方法缺 `[UnmanagedCallersOnly]` |
| `0xe0434352` | 托管异常穿到 SEH | 脚本回调抛异常（R1 双副本 / 空指针 / 未检查错误码） |

**EcsCode**（C# 侧 `EcsCode`，与 C++ 绑定表逐一对齐）：0 Ok · 1 NoWorld（宿主未绑 World）· 2 NoEntity（失效安全拒绝）· 3 Component（缺失/重复挂）· 4 BadArg。

## 7. 排错决策树

```
装载就炸（Vm::create / loadBootstrap）
  ├─ 0x80008093 → rtc 路径对吗？dotnet build 过吗？
  └─ 0x80131509 → 绝对路径了吗（R6）？刚重编过 → 重跑 2~3 次 / build-server shutdown
BootCall -101
  ├─ typeName 拼错 / AssemblyName ≠ dll 名（R8）
  └─ 自己解析类型却传了限定符（R2）
Entry 返回非 0 / NoWorld
  └─ ecsSetWorld 在 loadScript 之前了吗？FindByName 的名字在场景里吗？
OnTick SEH（0xe0434352，宿主停用 tick）
  ├─ 违反 R1（宿主侧 getEntryPoint 直拉脚本 dll）→ 改走 BootTickPtr
  └─ 回调内抛异常 → 全 API 错误码检查，日志查 nl:error
装上了但 tick 不跑
  ├─ 脚本没写 OnTick（可选契约，合法空转）
  ├─ 上次 OnTick 失败被停用 → game.script 看 tick=off，reload-script 恢复
  └─ pause 中（clock callback 不触发 = 同步冻结，合法）
```

## 8. 参考索引

| 层 | 文件 | 关键内容 |
|----|------|----------|
| 托管中转 | `script/bootstrap/Bootstrap.cs` | ABI 协议头注释 + collectible ALC + BootTickPtr thunk |
| 互操作 API | `script/script-api/Api.cs` | `Engine.Init` / Ecs / Console / CVR 绑定面（函数指针表） |
| 宿主编排 | `core/include/nl/script/ScriptHost.hpp` + `core/src/script/ScriptHost.cpp` | tick 面口径注释 / loadScript 回滚序列 |
| hosting 内核 | `core/src/script/Vm.cpp` | hostfxr 三导出、hresult 语义 |
| 工具 | `nl-script/main.cpp` | check / run / selftest（s1~s8 机械断言） |
| 模板脚本 | `script/examples/orbit/OrbitScript.cs` | Entry + EntryNative + OnTick 全契约示范 |
| viewer 接线 | `viewer/main.cpp` runScene 装载块 | Options / 时序 / probe 行 / 收尾三步 |
| 历史考证 | `docs/p8/SCRIPT-TOOLS-GUIDE.md` 各「踩坑存档」+ `docs/MILESTONES.md` M-SC*/M-GL4 | 每条铁律的实锤过程 |
