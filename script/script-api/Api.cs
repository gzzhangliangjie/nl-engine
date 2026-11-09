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
        public const string Version = "0.2.0";   // M-SC1：+console 绑定
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
}
