// M-SC0 骨架：脚本 API 绑定面的根命名空间 + 版本锚。
// M-SC1 起 console 绑定（cvar.get/set/list、console.exec、Console.Register）落本程序集；
// M-SC2 起 ECS 数据面绑定（World.Spawn/Remove/SetParent、TransformComponent 字段读写）落本程序集。
// M-SC4 起：接线层（delegate/指针字段/Init 偏移/NativeMarshal/TransformData POD 本体）
//   = 生成物 Wiring.g.cs（单源 script/script-api/exports.api）；本文件只剩
//   facade 语义本体（返回语义/错误码映射/回调适配）与 hand 扩展。勿在本文件
//   手写任何 marshal 原语——一律走 NativeMarshal（生命周期账本单源）。
// 热更安全约定（P8-SCRIPT-PLAN.md）：绑定面 API 无跨帧状态；状态全在 C++ 侧。
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Nl.Script.Api
{
    /// <summary>脚本 API 绑定面版本锚（selftest / echo 用例用它证明 script-api 已装载）。</summary>
    public static class Api
    {
        public const string Version = "0.5.0";   // M-SC5：Time/Input/Log 域绑定面（v3 9 导出）
    }

    /// <summary>console.exec 结果（ExecCode 分档与 C++ core/console 一致）。</summary>
    public readonly struct ConsoleExecResult
    {
        public int Code { get; }     // 0=Ok 1=UnknownCommand 2=BadRequest 3=CommandFailed
        public string Output { get; }

        public ConsoleExecResult(int code, string output) { Code = code; Output = output; }
        public bool Ok => Code == 0;
        public override string ToString() => $"[{Code}] {Output}";
    }

    /// <summary>
    /// C# 命令回调（经 ConsoleRegister 的友好包装）。
    /// 返回字符串 = 成功且作为命令输出；返回 null = 失败（ExecCode::CommandFailed）。
    /// 包装自身 catch 所有托管异常转失败——脚本作者无须关心互操作边界。
    /// </summary>
    public delegate string? ScriptCommandHandler(string[] args);

    /// <summary>托管命令回调原始 ABI（与 C++ ManagedCommandFn 对齐；consoleRegister 声明面）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int NativeCommandDelegate(int argc, IntPtr argv, out IntPtr text);

    /// <summary>
    /// console 绑定面（M-SC1，M-SC4 接线改生成物）。C++ 侧函数指针表随托管入口 args 传入
    /// （nl/script/Bindings.hpp ABI），Init 必须在使用前先调（selftest / 工具入口均已接）。
    /// 内存所有权（见 Bindings.hpp 文件头 + Wiring.g.cs NativeMarshal）：C++ 返回串
    /// 「下次调用前有效」→ 经 NativeMarshal.Utf8Read 立即拷贝成托管串；C# 命令 outText 用
    /// AllocHGlobal（CoTaskMem）→ C++ CoTaskMemFree。
    /// </summary>
    public static unsafe class Engine
    {
        /// <summary>从托管入口 args 读 ApiTable（指针；接线偏移全在生成物 ApiWiring）。</summary>
        public static void Init(IntPtr table)
        {
            if (table == IntPtr.Zero) throw new InvalidOperationException("Engine.Init: null api table (entry must receive ApiTable*)");
            ApiWiring.InitAll(table);
        }

        // ---- CVar ----

        /// <summary>读 cvar 现值（toString 口径）。未注册返回 null。typeCode: 1=bool 2=int 3=float 4=string。</summary>
        public static string? CvarGet(string name, out int typeCode)
        {
            typeCode = 0;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.CvarGetFn>(EngineWiring._cvarGet);
                int rc = fn(namePtr, out IntPtr valuePtr);
                if (rc == 0) return null;              // 未注册
                typeCode = rc;
                return NativeMarshal.Utf8ReadOrEmpty(valuePtr);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        public static string? CvarGet(string name) => CvarGet(name, out _);

        /// <summary>文本赋值（setFromString 语义，onChanged 触发）。0=ok 1=未知 cvar 2=类型/格式不符。</summary>
        public static int CvarSet(string name, string value)
        {
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name), valuePtr = NativeMarshal.Utf8Alloc(value);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.CvarSetFn>(EngineWiring._cvarSet);
                return fn(namePtr, valuePtr);
            }
            finally { NativeMarshal.Utf8Free(namePtr); NativeMarshal.Utf8Free(valuePtr); }
        }

        /// <summary>全量 CVar 视图（"name = value (type) help" 逐行）。</summary>
        public static string CvarList()
        {
            var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.CvarListFn>(EngineWiring._cvarList);
            return NativeMarshal.Utf8ReadOrEmpty(fn());
        }

        // ---- console ----

        /// <summary>经绑定面执行控制台行（ExecCode 0..3 原样返回）。</summary>
        public static ConsoleExecResult ConsoleExec(string line)
        {
            IntPtr linePtr = NativeMarshal.Utf8Alloc(line);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.ConsoleExecFn>(EngineWiring._consoleExec);
                int code = fn(linePtr, out IntPtr outputPtr);
                return new ConsoleExecResult(code, NativeMarshal.Utf8ReadOrEmpty(outputPtr));
            }
            finally { NativeMarshal.Utf8Free(linePtr); }
        }

        /// <summary>
        /// 注册 C# 命令进 console 命令表。返回 0=ok / 1=名字被 CVar 占用。
        /// 异常安全：handler 抛出的托管异常在包装内 catch 转 null（失败），不穿互操作边界。
        /// delegate 保活：adapter 落在 script-api（与脚本同 collectible ALC），Unload 随
        /// ALC 整体回收——卸载前 C++ 侧 scriptUnregisterAll 次序铁律不变（M-SC3）。
        /// </summary>
        public static int ConsoleRegister(string name, string help, ScriptCommandHandler handler)
        {
            NativeCommandDelegate adapter = (int argc, IntPtr argv, out IntPtr text) =>
            {
                text = IntPtr.Zero;
                string? result;
                try
                {
                    var args = new string[argc];
                    for (int i = 0; i < argc; i++)
                        args[i] = Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(argv, i * sizeof(IntPtr))) ?? "";
                    result = handler(args);
                }
                catch (Exception) { return 1; }         // 托管异常 → 失败（不穿边界）
                if (result == null) return 1;
                text = NativeMarshal.Utf8Alloc(result);
                return 0;
            };
            return ConsoleRegisterRaw(name, help, adapter);
        }

        /// <summary>原始 ABI 注册（异常不经包装捕获 → C++ SEH 边界路径，selftest s6d 用）。</summary>
        public static int ConsoleRegisterRaw(string name, string help, NativeCommandDelegate raw)
        {
            _pinned.Add(raw);
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name), helpPtr = NativeMarshal.Utf8Alloc(help);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.ConsoleRegisterFn>(EngineWiring._consoleRegister);
                return fn(namePtr, helpPtr, Marshal.GetFunctionPointerForDelegate(raw));
            }
            finally { NativeMarshal.Utf8Free(namePtr); NativeMarshal.Utf8Free(helpPtr); }
        }

        /// <summary>注销命令（0=ok / 1=未知）。M-SC3 热重载解绑的落点。</summary>
        public static int ConsoleUnregister(string name)
        {
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.ConsoleUnregisterFn>(EngineWiring._consoleUnregister);
                return fn(namePtr);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        // ---- 命令回调保活 ----
        // 与脚本同 collectible ALC（Bootstrap 依赖解析口径）：Unload 时本列表连同
        // adapter thunk 一并回收，零残留——「批次归组」由 ALC 边界结构性达成，
        // 无须手工清点（UnrealSharp per-ALC strong refs 同构，2026-11-14 实读对齐）。
        static readonly List<NativeCommandDelegate> _pinned = new();
    }

    /// <summary>ECS 导出错误码（与 nl/script/Bindings.hpp kEcs* 一致）。</summary>
    public static class EcsCode
    {
        public const int Ok = 0;
        public const int NoWorld = 1;      // 宿主未绑 World
        public const int NoEntity = 2;     // 实体不存在（失效安全拒绝）
        public const int Component = 3;    // 组件缺失或重复挂
        public const int BadArg = 4;       // 非法参数
    }

    /// <summary>局部 TRS（POD 本体 = 生成物 Wiring.g.cs；本 partial 只落 hand 扩展）。
    /// quat x,y,z,w 口径与 C++ Transform 一致。</summary>
    public partial struct TransformData
    {
        public static TransformData Identity() => new()
        { Qw = 1f, Sx = 1f, Sy = 1f, Sz = 1f };
    }

    /// <summary>
    /// ECS 数据面绑定（M-SC2，M-SC4 接线改生成物）。操作目标 = 宿主侧 ecsSetWorld 绑定的
    /// World（nl/script/Bindings.hpp v2 导出，ApiTable 偏移 6..15）。延迟语义沿 World 口径：
    /// Spawn 新建立即可见；Remove 排队、Flush/tick 生效（Carrot 口径）。
    /// 失效安全拒绝：所有方法回错误码（EcsCode），不抛异常不崩。
    /// </summary>
    public static unsafe class Ecs
    {
        /// <summary>建实体（新建立即可见，Carrot 口径）。0=ok，*id 为实体 ID。</summary>
        public static int Spawn(string name, out ulong id)
        {
            id = 0;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsSpawnFn>(EcsWiring._ecsSpawn);
                return fn(namePtr, out id);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        /// <summary>延迟删除（flush/tick 生效；父删递归删子）。</summary>
        public static int Remove(ulong id) =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsRemoveFn>(EcsWiring._ecsRemove)(id);

        /// <summary>统一落盘（宿主 tick 边界动作）。</summary>
        public static int Flush() =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsFlushFn>(EcsWiring._ecsFlush)();

        /// <summary>存在性查询（0=ok / 1=未绑 World）。exists: 新建即时真、删除 flush 后假。</summary>
        public static int Exists(ulong id, out bool exists)
        {
            exists = false;
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsExistsFn>(EcsWiring._ecsExists);
            int rc = fn(id, out int e);
            if (rc == EcsCode.Ok) exists = e != 0;
            return rc;
        }

        /// <summary>挂父（纯关系，不动变换）。parent=0 解除父子。</summary>
        public static int SetParent(ulong id, ulong parent) =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsSetParentFn>(EcsWiring._ecsSetParent)(id, parent);

        /// <summary>取父 ID（0=无父）。</summary>
        public static int GetParent(ulong id, out ulong parent)
        {
            parent = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsGetParentFn>(EcsWiring._ecsGetParent);
            return fn(id, out parent);
        }

        /// <summary>挂 TransformComponent（重复挂 = EcsCode.Component）。</summary>
        public static int AddTransform(ulong id) =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsAddTransformFn>(EcsWiring._ecsAddTransform)(id);

        /// <summary>局部 TRS 读回（缺组件 = EcsCode.Component）。</summary>
        public static int TransformGet(ulong id, out TransformData t)
        {
            t = default;
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsTransformGetFn>(EcsWiring._ecsTransformGet);
            float* pos = stackalloc float[3];
            float* rot = stackalloc float[4];
            float* scl = stackalloc float[3];
            int rc = fn(id, pos, rot, scl);
            if (rc == EcsCode.Ok)
            {
                t = new TransformData
                {
                    Px = pos[0], Py = pos[1], Pz = pos[2],
                    Qx = rot[0], Qy = rot[1], Qz = rot[2], Qw = rot[3],
                    Sx = scl[0], Sy = scl[1], Sz = scl[2],
                };
            }
            return rc;
        }

        /// <summary>局部 TRS 写入（缺组件 = EcsCode.Component）。</summary>
        public static int TransformSet(ulong id, in TransformData t)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsTransformSetFn>(EcsWiring._ecsTransformSet);
            float* pos = stackalloc float[3] { t.Px, t.Py, t.Pz };
            float* rot = stackalloc float[4] { t.Qx, t.Qy, t.Qz, t.Qw };
            float* scl = stackalloc float[3] { t.Sx, t.Sy, t.Sz };
            return fn(id, pos, rot, scl);
        }

        /// <summary>按名查实体（未找到 id=0，返回 EcsCode.Ok）。</summary>
        public static int FindByName(string name, out ulong id)
        {
            id = 0;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsFindByNameFn>(EcsWiring._ecsFindByName);
                return fn(namePtr, out id);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }
    }

    /// <summary>
    /// Entity 轻量 wrapper（与 EntityID 映射，Carrot Entity 同构：不持存储）。
    /// 失效安全拒绝：所有实例方法委托 Ecs.* 回错误码——失效实体上调用得到
    /// EcsCode.NoEntity，不崩不改状态（selftest s7 判据本体）。
    /// </summary>
    public readonly struct SceneEntity
    {
        public ulong Id { get; }
        public SceneEntity(ulong id) { Id = id; }

        /// <summary>存在性（新建即时真、删除 flush 后假——Carrot 非对称口径）。</summary>
        public bool Exists
        {
            get { Ecs.Exists(Id, out bool e); return e; }
        }

        public int Remove() => Ecs.Remove(Id);
        public int SetParent(ulong parent) => Ecs.SetParent(Id, parent);
        public int GetParent(out ulong parent) => Ecs.GetParent(Id, out parent);
        public int AddTransform() => Ecs.AddTransform(Id);
        public int GetTransform(out TransformData t) => Ecs.TransformGet(Id, out t);
        public int SetTransform(in TransformData t) => Ecs.TransformSet(Id, t);
    }

    /// <summary>Time 导出错误码（与 nl/script/Bindings.hpp kTime* 一致）。</summary>
    public static class TimeCode
    {
        public const int Ok = 0;
        public const int NoClock = 1;      // 宿主未绑 GameClock
        public const int BadArg = 4;       // 非法参数
    }

    /// <summary>
    /// Time 绑定面（M-SC5）。操作目标 = 宿主侧 timeSetClock 绑定的 GameClock
    /// （viewer --script 场景模式接线）。未绑 = TimeCode.NoClock，不抛异常不崩。
    /// </summary>
    public static unsafe class Time
    {
        /// <summary>时钟状态一次性读回。0=ok；simTime 逻辑秒 / frameDt 渲染帧 delta 秒 / tickCount 逻辑帧数 / paused。</summary>
        public static int GetState(out double simTime, out double frameDt, out ulong tickCount, out bool paused)
        {
            simTime = 0; frameDt = 0; tickCount = 0; paused = false;
            var fn = Marshal.GetDelegateForFunctionPointer<TimeWiring.TimeGetStateFn>(TimeWiring._timeGetState);
            int rc = fn(out simTime, out frameDt, out tickCount, out int p);
            if (rc == TimeCode.Ok) paused = p != 0;
            return rc;
        }

        /// <summary>时钟控制。paused：-1=不变 / 0=继续 / 1=暂停；timeScale&lt;0=不变（NaN 不变）。</summary>
        public static int SetState(int paused, double timeScale) =>
            Marshal.GetDelegateForFunctionPointer<TimeWiring.TimeSetStateFn>(TimeWiring._timeSetState)(paused, timeScale);

        /// <summary>逻辑时间（秒）；未绑返回 0 且 rc=NoClock。</summary>
        public static int SimTime(out double t)
        {
            t = 0;
            var rc = GetState(out t, out _, out _, out _);
            return rc;
        }

        /// <summary>暂停（SetState(1, -1)）。</summary>
        public static int Pause() => SetState(1, -1.0);

        /// <summary>继续（SetState(0, -1)）。</summary>
        public static int Unpause() => SetState(0, -1.0);

        /// <summary>设时间倍率（[0,1024] 钳制在 GameClock 内做；&lt;0 不变）。</summary>
        public static int SetTimeScale(double scale) => SetState(-1, scale);
    }

    /// <summary>Input 导出错误码（与 nl/script/Bindings.hpp kInput* 一致）。</summary>
    public static class InputCode
    {
        public const int Ok = 0;
        public const int NoSource = 1;     // 宿主未绑 ActionMap/InputState（不隐式建表口径）
        public const int NoAction = 2;     // 动作不存在或类型不符
        public const int BadArg = 4;       // 非法参数
    }

    /// <summary>
    /// Input 绑定面（M-SC5）。操作目标 = 宿主侧 inputSetActionMap / inputSetState 绑定的实例。
    /// 动作值 = ActionMap::update 冻结口径（宿主每帧 update，脚本读冻结值）。
    /// 未绑 = InputCode.NoSource，不隐式建表（拍板）。
    /// </summary>
    public static unsafe class Input
    {
        /// <summary>按名查动作（预声明口径）。0=ok；2=不存在（id = InvalidAction 0xFFFFFFFF）。</summary>
        public static int ActionFind(string name, out uint id)
        {
            id = 0xFFFFFFFFu;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionFindFn>(InputWiring._inputActionFind);
                return fn(namePtr, out id);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        /// <summary>Bool 动作冻结值三态读回。0=ok / 2=非法 id 或类型非 Bool。</summary>
        public static int ActionBool(uint id, out bool value, out bool justPressed, out bool justReleased)
        {
            value = false; justPressed = false; justReleased = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionBoolFn>(InputWiring._inputActionBool);
            int rc = fn(id, out int v, out int p, out int r);
            if (rc == InputCode.Ok) { value = v != 0; justPressed = p != 0; justReleased = r != 0; }
            return rc;
        }

        /// <summary>Float 动作冻结值读回。0=ok / 2=非法 id 或类型非 Float。</summary>
        public static int ActionFloat(uint id, out float value, out float delta)
        {
            value = 0; delta = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionFloatFn>(InputWiring._inputActionFloat);
            return fn(id, out value, out delta);
        }

        /// <summary>Vec2 动作冻结值读回（x,y 序）。0=ok / 2=非法 id 或类型非 Vec2。</summary>
        public static int ActionVec2(uint id, out float x, out float y, out float dx, out float dy)
        {
            x = y = dx = dy = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionVec2Fn>(InputWiring._inputActionVec2);
            float* v = stackalloc float[2];
            float* d = stackalloc float[2];
            int rc = fn(id, v, d);
            if (rc == InputCode.Ok) { x = v[0]; y = v[1]; dx = d[0]; dy = d[1]; }
            return rc;
        }

        /// <summary>键持续态（GLFW 键码；越界键码 down=false——InputState 防御性口径）。0=ok。</summary>
        public static int KeyIsDown(int key, out bool down)
        {
            down = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputKeyIsDownFn>(InputWiring._inputKeyIsDown);
            int rc = fn(key, out int d);
            if (rc == InputCode.Ok) down = d != 0;
            return rc;
        }
    }

    /// <summary>Log 导出错误码（与 nl/script/Bindings.hpp kLog* 一致）。</summary>
    public static class LogCode
    {
        public const int Ok = 0;
        public const int BadFilter = 1;    // 过滤指令非法（Error 带原因文本）
        public const int BadArg = 2;       // level 越界（0..5）或 message 空
    }

    /// <summary>
    /// Log 绑定面（M-SC5）。走 nl::core::log 全局面，通道固定 "script"
    /// （输出行 [nl:&lt;level&gt;|script] message——expectLog 锚可断）。
    /// </summary>
    public static unsafe class Log
    {
        /// <summary>level：0..5 = Trace/Debug/Info/Warn/Error/Fatal。0=ok / 2=level 越界或 message 空。</summary>
        public static int Write(int level, string message)
        {
            IntPtr msgPtr = NativeMarshal.Utf8Alloc(message);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<LogWiring.LogWriteFn>(LogWiring._logWrite);
                return fn(level, msgPtr);
            }
            finally { NativeMarshal.Utf8Free(msgPtr); }
        }

        /// <summary>设置过滤指令（全局面替换："nl:info,renderer=warn"）。
        /// 0=ok / 1=指令非法（error=原因文本）/ 2=非法参数。</summary>
        public static int SetFilter(string instruction, out string? error)
        {
            error = null;
            IntPtr instrPtr = NativeMarshal.Utf8Alloc(instruction);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<LogWiring.LogSetFilterFn>(LogWiring._logSetFilter);
                int rc = fn(instrPtr, out IntPtr errPtr);
                if (rc == LogCode.BadFilter) error = NativeMarshal.Utf8ReadOrEmpty(errPtr);
                return rc;
            }
            finally { NativeMarshal.Utf8Free(instrPtr); }
        }
    }
}
