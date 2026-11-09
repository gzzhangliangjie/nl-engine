// M-SC0 骨架：脚本 API 绑定面的根命名空间 + 版本锚。
// M-SC1 起 console 绑定（cvar.get/set/list、console.exec、Console.Register）落本程序集；
// M-SC2 起 ECS 数据面绑定（World.Spawn/Remove/SetParent、TransformComponent 字段读写）落本程序集。
// 热更安全约定（P8-SCRIPT-PLAN.md）：绑定面 API 无跨帧状态；状态全在 C++ 侧。
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Nl.Script.Api
{
    /// <summary>脚本 API 绑定面版本锚（selftest / echo 用例用它证明 script-api 已装载）。</summary>
    public static class Api
    {
        public const string Version = "0.3.0";   // M-SC2：+ECS 数据面绑定
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

    /// <summary>
    /// console 绑定面（M-SC1）。C++ 侧函数指针表随托管入口 args 传入（nl/script/Bindings.hpp
    /// ABI），Init 必须在使用前先调（selftest / 工具入口均已接）。
    /// 内存所有权（见 Bindings.hpp 文件头）：C++ 返回串「下次调用前有效」→ 本类内部立即
    /// 拷贝成托管串；C# 命令 outText 用 AllocHGlobal（CoTaskMem）→ C++ CoTaskMemFree。
    /// </summary>
    public static unsafe class Engine
    {
        // ---- C++ 侧函数指针（ApiTable 偏移序 = Bindings.hpp 字段序）----
        static IntPtr _cvarGet, _cvarSet, _cvarList, _consoleExec, _consoleRegister, _consoleUnregister;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int CvarGetFn(IntPtr name, out IntPtr value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int CvarSetFn(IntPtr name, IntPtr value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate IntPtr CvarListFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int ConsoleExecFn(IntPtr line, out IntPtr output);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int ConsoleRegisterFn(IntPtr name, IntPtr help, IntPtr fn);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int ConsoleUnregisterFn(IntPtr name);

        /// <summary>托管命令回调原始 ABI（与 C++ ManagedCommandFn 对齐）。</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int NativeCommandDelegate(int argc, IntPtr argv, out IntPtr text);

        // 友好注册的 delegate 保活（函数指针 thunk 被 GC 收掉 = native 调用炸）
        static readonly List<NativeCommandDelegate> _pinned = new();

        /// <summary>从托管入口 args 读 ApiTable（指针；按 Bindings.hpp 偏移序）。</summary>
        public static void Init(IntPtr table)
        {
            if (table == IntPtr.Zero) throw new InvalidOperationException("Engine.Init: null api table (entry must receive ApiTable*)");
            _cvarGet           = Marshal.ReadIntPtr(table, 0 * sizeof(IntPtr));
            _cvarSet           = Marshal.ReadIntPtr(table, 1 * sizeof(IntPtr));
            _cvarList          = Marshal.ReadIntPtr(table, 2 * sizeof(IntPtr));
            _consoleExec       = Marshal.ReadIntPtr(table, 3 * sizeof(IntPtr));
            _consoleRegister   = Marshal.ReadIntPtr(table, 4 * sizeof(IntPtr));
            _consoleUnregister = Marshal.ReadIntPtr(table, 5 * sizeof(IntPtr));
            Ecs.Init(table);   // v2 ECS 导出（偏移 6..14，M-SC2 尾部追加）
        }

        // ---- CVar ----

        /// <summary>读 cvar 现值（toString 口径）。未注册返回 null。typeCode: 1=bool 2=int 3=float 4=string。</summary>
        public static string? CvarGet(string name, out int typeCode)
        {
            typeCode = 0;
            IntPtr namePtr = Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<CvarGetFn>(_cvarGet);
                int rc = fn(namePtr, out IntPtr valuePtr);
                if (rc == 0) return null;              // 未注册
                typeCode = rc;
                return valuePtr == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(valuePtr)!;
            }
            finally { Marshal.FreeHGlobal(namePtr); }
        }

        public static string? CvarGet(string name) => CvarGet(name, out _);

        /// <summary>文本赋值（setFromString 语义，onChanged 触发）。0=ok 1=未知 cvar 2=类型/格式不符。</summary>
        public static int CvarSet(string name, string value)
        {
            IntPtr namePtr = Utf8Alloc(name), valuePtr = Utf8Alloc(value);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<CvarSetFn>(_cvarSet);
                return fn(namePtr, valuePtr);
            }
            finally { Marshal.FreeHGlobal(namePtr); Marshal.FreeHGlobal(valuePtr); }
        }

        /// <summary>全量 CVar 视图（"name = value (type) help" 逐行）。</summary>
        public static string CvarList()
        {
            var fn = Marshal.GetDelegateForFunctionPointer<CvarListFn>(_cvarList);
            IntPtr p = fn();
            return p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p)!;
        }

        // ---- console ----

        /// <summary>经绑定面执行控制台行（ExecCode 0..3 原样返回）。</summary>
        public static ConsoleExecResult ConsoleExec(string line)
        {
            IntPtr linePtr = Utf8Alloc(line);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<ConsoleExecFn>(_consoleExec);
                int code = fn(linePtr, out IntPtr outputPtr);
                string output = outputPtr == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(outputPtr)!;
                return new ConsoleExecResult(code, output);
            }
            finally { Marshal.FreeHGlobal(linePtr); }
        }

        /// <summary>
        /// 注册 C# 命令进 console 命令表。返回 0=ok / 1=名字被 CVar 占用。
        /// 异常安全：handler 抛出的托管异常在包装内 catch 转 null（失败），不穿互操作边界。
        /// delegate 经 _pinned 保活（collectible ALC 卸载时的解绑走 ConsoleUnregister，M-SC3）。
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
                text = Utf8Alloc(result);
                return 0;
            };
            return ConsoleRegisterRaw(name, help, adapter);
        }

        /// <summary>原始 ABI 注册（异常不经包装捕获 → C++ SEH 边界路径，selftest s6d 用）。</summary>
        public static int ConsoleRegisterRaw(string name, string help, NativeCommandDelegate raw)
        {
            _pinned.Add(raw);
            IntPtr namePtr = Utf8Alloc(name), helpPtr = Utf8Alloc(help);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<ConsoleRegisterFn>(_consoleRegister);
                return fn(namePtr, helpPtr, Marshal.GetFunctionPointerForDelegate(raw));
            }
            finally { Marshal.FreeHGlobal(namePtr); Marshal.FreeHGlobal(helpPtr); }
        }

        /// <summary>注销命令（0=ok / 1=未知）。M-SC3 热重载解绑的落点。</summary>
        public static int ConsoleUnregister(string name)
        {
            IntPtr namePtr = Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<ConsoleUnregisterFn>(_consoleUnregister);
                return fn(namePtr);
            }
            finally { Marshal.FreeHGlobal(namePtr); }
        }

        // ---- UTF-8 帮手 ----

        static IntPtr Utf8Alloc(string s)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(s ?? "");
            IntPtr p = Marshal.AllocHGlobal(bytes.Length + 1);
            Marshal.Copy(bytes, 0, p, bytes.Length);
            Marshal.WriteByte(p, bytes.Length, 0);
            return p;
        }
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

    /// <summary>局部 TRS（quat x,y,z,w 口径与 C++ Transform 一致）。</summary>
    public struct TransformData
    {
        public float Px, Py, Pz;
        public float Qx, Qy, Qz, Qw;
        public float Sx, Sy, Sz;

        public static TransformData Identity() => new()
        { Qw = 1f, Sx = 1f, Sy = 1f, Sz = 1f };
    }

    /// <summary>
    /// ECS 数据面绑定（M-SC2）。操作目标 = 宿主侧 ecsSetWorld 绑定的 World
    /// （nl/script/Bindings.hpp v2 导出，ApiTable 偏移 6..14）。延迟语义沿 World 口径：
    /// Spawn 新建立即可见；Remove 排队、Flush/tick 生效（Carrot 口径）。
    /// 失效安全拒绝：所有方法回错误码（EcsCode），不抛异常不崩。
    /// </summary>
    public static unsafe class Ecs
    {
        // ---- C++ 侧函数指针（ApiTable 偏移 6..15）----
        static IntPtr _spawn, _remove, _flush, _exists, _setParent, _getParent,
                      _addTransform, _transformGet, _transformSet, _findByName;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int SpawnFn(IntPtr name, out ulong id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int RemoveFn(ulong id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int FlushFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int ExistsFn(ulong id, out int exists);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int SetParentFn(ulong id, ulong parent);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int GetParentFn(ulong id, out ulong parent);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int AddTransformFn(ulong id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int TransformGetFn(ulong id, float* pos3, float* rot4, float* scale3);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int TransformSetFn(ulong id, float* pos3, float* rot4, float* scale3);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int FindByNameFn(IntPtr name, out ulong id);

        /// <summary>由 Engine.Init 调用（读 v2 尾部偏移）。</summary>
        internal static void Init(IntPtr table)
        {
            _spawn         = Marshal.ReadIntPtr(table, 6 * sizeof(IntPtr));
            _remove        = Marshal.ReadIntPtr(table, 7 * sizeof(IntPtr));
            _flush         = Marshal.ReadIntPtr(table, 8 * sizeof(IntPtr));
            _exists        = Marshal.ReadIntPtr(table, 9 * sizeof(IntPtr));
            _setParent     = Marshal.ReadIntPtr(table, 10 * sizeof(IntPtr));
            _getParent     = Marshal.ReadIntPtr(table, 11 * sizeof(IntPtr));
            _addTransform  = Marshal.ReadIntPtr(table, 12 * sizeof(IntPtr));
            _transformGet  = Marshal.ReadIntPtr(table, 13 * sizeof(IntPtr));
            _transformSet  = Marshal.ReadIntPtr(table, 14 * sizeof(IntPtr));
            _findByName    = Marshal.ReadIntPtr(table, 15 * sizeof(IntPtr));
        }

        /// <summary>建实体（新建立即可见，Carrot 口径）。0=ok，*id 为实体 ID。</summary>
        public static int Spawn(string name, out ulong id)
        {
            id = 0;
            IntPtr namePtr = Utf8(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<SpawnFn>(_spawn);
                return fn(namePtr, out id);
            }
            finally { Marshal.FreeHGlobal(namePtr); }
        }

        /// <summary>延迟删除（flush/tick 生效；父删递归删子）。</summary>
        public static int Remove(ulong id) =>
            Marshal.GetDelegateForFunctionPointer<RemoveFn>(_remove)(id);

        /// <summary>统一落盘（宿主 tick 边界动作）。</summary>
        public static int Flush() =>
            Marshal.GetDelegateForFunctionPointer<FlushFn>(_flush)();

        /// <summary>存在性查询（0=ok / 1=未绑 World）。exists: 新建即时真、删除 flush 后假。</summary>
        public static int Exists(ulong id, out bool exists)
        {
            exists = false;
            var fn = Marshal.GetDelegateForFunctionPointer<ExistsFn>(_exists);
            int rc = fn(id, out int e);
            if (rc == EcsCode.Ok) exists = e != 0;
            return rc;
        }

        /// <summary>挂父（纯关系，不动变换）。parent=0 解除父子。</summary>
        public static int SetParent(ulong id, ulong parent) =>
            Marshal.GetDelegateForFunctionPointer<SetParentFn>(_setParent)(id, parent);

        /// <summary>取父 ID（0=无父）。</summary>
        public static int GetParent(ulong id, out ulong parent)
        {
            parent = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<GetParentFn>(_getParent);
            return fn(id, out parent);
        }

        /// <summary>挂 TransformComponent（重复挂 = EcsCode.Component）。</summary>
        public static int AddTransform(ulong id) =>
            Marshal.GetDelegateForFunctionPointer<AddTransformFn>(_addTransform)(id);

        /// <summary>局部 TRS 读回（缺组件 = EcsCode.Component）。</summary>
        public static int TransformGet(ulong id, out TransformData t)
        {
            t = default;
            var fn = Marshal.GetDelegateForFunctionPointer<TransformGetFn>(_transformGet);
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
            var fn = Marshal.GetDelegateForFunctionPointer<TransformSetFn>(_transformSet);
            float* pos = stackalloc float[3] { t.Px, t.Py, t.Pz };
            float* rot = stackalloc float[4] { t.Qx, t.Qy, t.Qz, t.Qw };
            float* scl = stackalloc float[3] { t.Sx, t.Sy, t.Sz };
            return fn(id, pos, rot, scl);
        }

        /// <summary>按名查实体（未找到 id=0，返回 EcsCode.Ok）。</summary>
        public static int FindByName(string name, out ulong id)
        {
            id = 0;
            IntPtr namePtr = Utf8(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<FindByNameFn>(_findByName);
                return fn(namePtr, out id);
            }
            finally { Marshal.FreeHGlobal(namePtr); }
        }

        static IntPtr Utf8(string s)
        {
            byte[] bytes = System.Text.Encoding.UTF8.GetBytes(s ?? "");
            IntPtr p = Marshal.AllocHGlobal(bytes.Length + 1);
            Marshal.Copy(bytes, 0, p, bytes.Length);
            Marshal.WriteByte(p, bytes.Length, 0);
            return p;
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
}
