// M-SC3 热更中转 bootstrap（docs/p8/P8-SCRIPT-PLAN.md「热更机制」定稿口径）。
//
// 官方口径（Microsoft Learn《Write a custom .NET runtime host》+ dotnet/samples
// host loading 系列）：collectible AssemblyLoadContext 的创建与卸载只能在托管侧
// 完成——hostfxr native API 没有对应入口，且 load_assembly_and_get_function_pointer
// 一律进默认 ALC（不可卸载）。因此热更结构 = 非收集本程序集（常驻）+ collectible
// ScriptLoadContext（脚本程序集轮换）。C++ 侧只持有本程序集的 3 个函数指针
// （[UnmanagedCallersOnly] + 非收集 → 常驻安全）；脚本 ALC 内代码的函数指针
// 一律不许外泄（Unload 后指针悬空，官方文档明确警告）。
//
// LoadFromStream 而非 LoadFromAssemblyPath：后者在 Windows 上锁 DLL 文件，
// dotnet build 无法同名覆盖 → 热更工作流（改脚本→重编→reload）必断。
// Stream 加载不持文件句柄，构建可覆盖写入（本里程碑验收的关键通路）。
//
// 依赖解析：脚本 DLL 的本地依赖（NlScriptApi.dll 等）LoadFromStream 进**同一个**
// collectible ALC——Unload 时 script-api 的静态（Engine 函数指针缓存、_pinned
// delegate 保活列表）一并回收，零残留。框架程序集（System.* 等）返回 null 回退
// 默认解析（官方推荐：只把用户程序集及其依赖装进 collectible）。
//
// 卸载排空：Unload() 后循环 GC.Collect + WaitForPendingFinalizers 直到
// WeakReference(Assembly).IsAlive == false（有上限轮数，防死循环）。
// 返回 1 = Unload 已调但 GC 未收干净（selftest 判 0）。
//
// ABI 协议（与 C++ 侧 Vm.cpp/ScriptHost.cpp 注释同文，写死防漂移；x64，
// C++ 侧手构 POD 结构传地址，本类按偏移手读——不用 Marshal 结构 marshal，
// 防 packing 踩坑）：
//   BootLoad args  = struct { const char* assemblyPathUtf8; }        // 偏移 0
//     返回 0=ok / -1=空参或读不到路径 / -2=装载失败（流读取/依赖解析）/
//          -3=已有脚本未卸载（先 BootUnload 再换载）
//   BootCall args  = struct { const char* typeUtf8;                  // 偏移 0
//                             const char* methodUtf8;                // 偏移 8
//                             void* data;                            // 偏移 16
//                             int32_t size; }                        // 偏移 24
//     返回 = 脚本静态方法返回值（签名 int(IntPtr, int)）/
//          -100=未装载 / -101=类型或方法解析失败 / -102=托管异常（Invoke 包装）
//   BootUnload args = NULL
//     返回 0=完全卸载（weakref dead）/ 1=未完全（Unload 已调）/ -1=无脚本
//   BootTickPtr args = struct { const char* typeUtf8;               // 偏移 0（M-GL4）
//                               const char* methodUtf8;             // 偏移 8
//                               void** outFn; }                     // 偏移 16
//     返回 0=ok（*outFn = 方法本机指针）/ -1=空参 / -100=未装载 /
//          -101=类型或方法解析失败 / -102=托管异常（包装）
//     **必须经本导出取 tick 指针**：hostfxr getEntryPoint 会把脚本程序集再装
//     一份进默认 ALC（静态字段独立一套，Engine.Init 状态不通——M-GL4 实证
//     默认副本 OnTick 读 _exists=0 崩）。本导出从 _lastAssembly（collectible
//     正本）反射解析，经 UnmanagedFunctionPointer 委托 thunk 带出本机指针；
//     thunk delegate 存 static 保活列表（Unload 前清空，否则 rooted 挡回收）。
//
// 脚本方法约定：公共静态 int(IntPtr, int)——反射 Invoke 路径不要求
// [UnmanagedCallersOnly]（有也兼容，Invoke 走托管反射不走函数指针）。
// 脚本异常：TargetInvocationException 在此 catch → -102（互操作边界归一，
// 与 Vm.cpp SEH 口径一致；正常路径下异常不出本类，SEH 只是兜底）。
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Nl.Script.Bootstrap
{
    /// <summary>
    /// collectible ALC：脚本主程序集 + 本地依赖同 ALC 装载，Unload 整体回收。
    /// </summary>
    sealed class ScriptLoadContext : AssemblyLoadContext
    {
        readonly string _basePath;

        public ScriptLoadContext(string basePath)
            : base("nl-script-collectible", isCollectible: true)
        {
            _basePath = basePath;
        }

        protected override Assembly? Load(AssemblyName name)
        {
            if (name?.Name is null) return null;
            // 本地依赖（script-api 等）→ 同 ALC stream 装载（随 Unload 回收）
            string candidate = Path.Combine(_basePath, name.Name + ".dll");
            if (File.Exists(candidate))
            {
                using var fs = new FileStream(candidate, FileMode.Open, FileAccess.Read, FileShare.Read);
                return LoadFromStream(fs);
            }
            return null;   // 框架程序集回退默认解析（Default ALC 共享，不进 collectible）
        }
    }

    public static class Bootstrap
    {
        static ScriptLoadContext? _alc;
        static Assembly? _lastAssembly;

        // M-GL4：tick 指针 thunk 保活（BootTickPtr 填充，UnloadAndWeak 清空——
        // delegate 引用 collectible 程序集内方法，不清 = rooted 挡回收）。
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int TickFn(IntPtr args, int size);
        static readonly List<Delegate> _tickDelegates = new();

        static IntPtr PtrAt(IntPtr args, int offset) => Marshal.ReadIntPtr(args, offset);

        static string? Utf8At(IntPtr args, int offset)
        {
            IntPtr p = PtrAt(args, offset);
            return p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);
        }

        // ---- BootLoad ----

        [UnmanagedCallersOnly(EntryPoint = "BootLoad")]
        public static int BootLoad(IntPtr args, int size)
        {
            if (_alc is not null) return -3;                    // 已有脚本未卸载
            string? path = args == IntPtr.Zero ? null : Utf8At(args, 0);
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return -1;

            try
            {
                var alc = new ScriptLoadContext(Path.GetDirectoryName(Path.GetFullPath(path))!);
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
                Assembly asm = alc.LoadFromStream(fs);
                _alc = alc;
                _lastAssembly = asm;
                return 0;
            }
            catch (Exception)
            {
                // 装载失败：ALC 一并丢弃（半初始化状态不残留）
                if (_alc is not null) { try { _alc.Unload(); } catch { } _alc = null; }
                _lastAssembly = null;
                return -2;
            }
        }

        // ---- BootCall ----

        [UnmanagedCallersOnly(EntryPoint = "BootCall")]
        public static int BootCall(IntPtr args, int size)
        {
            if (_alc is null || _lastAssembly is null) return -100;
            string? type = args == IntPtr.Zero ? null : Utf8At(args, 0);
            string? method = args == IntPtr.Zero ? null : Utf8At(args, 8);
            if (type is null || method is null) return -101;

            try
            {
                Type? t = _lastAssembly.GetType(type);
                if (t is null)
                {
                    // Assembly.GetType 不接受 ", AssemblyName" 限定符（hostfxr
                    // load_assembly_and_get_function_pointer 的类型参数口径**吃**
                    // 限定符，两者约定必须统一兼容——否则 getEntryPoint 与
                    // BootCall 同一 typeName 字符串一通一炸，M-GL4 实证）。
                    int comma = type.IndexOf(',');
                    if (comma > 0)
                        t = _lastAssembly.GetType(type.Substring(0, comma).Trim());
                }
                if (t is null) return -101;
                MethodInfo? m = t.GetMethod(method,
                    BindingFlags.Public | BindingFlags.Static,
                    binder: null, types: new[] { typeof(IntPtr), typeof(int) }, modifiers: null);
                if (m is null) return -101;
                IntPtr data = PtrAt(args, 16);
                int dataBytes = Marshal.ReadInt32(args, 24);
                object? ret = m.Invoke(null, new object?[] { data, dataBytes });
                return ret is int i ? i : 0;
            }
            catch (Exception)
            {
                return -102;   // 托管异常 → 边界归一（与 Vm.cpp SEH 口径一致）
            }
        }

        // ---- BootTickPtr（M-GL4）----

        [UnmanagedCallersOnly(EntryPoint = "BootTickPtr")]
        public static int BootTickPtr(IntPtr args, int size)
        {
            if (_alc is null || _lastAssembly is null) return -100;
            string? type = args == IntPtr.Zero ? null : Utf8At(args, 0);
            string? method = args == IntPtr.Zero ? null : Utf8At(args, 8);
            IntPtr outFn = args == IntPtr.Zero ? IntPtr.Zero : Marshal.ReadIntPtr(args, 16);
            if (type is null || method is null || outFn == IntPtr.Zero) return -1;

            try
            {
                Type? t = _lastAssembly.GetType(type);
                if (t is null)
                {
                    int comma = type.IndexOf(',');
                    if (comma > 0)
                        t = _lastAssembly.GetType(type.Substring(0, comma).Trim());
                }
                if (t is null) return -101;
                MethodInfo? m = t.GetMethod(method,
                    BindingFlags.Public | BindingFlags.Static,
                    binder: null, types: new[] { typeof(IntPtr), typeof(int) }, modifiers: null);
                if (m is null) return -101;
                var del = (TickFn)m.CreateDelegate(typeof(TickFn));
                _tickDelegates.Add(del);   // thunk 保活（Unload 前清空）
                Marshal.WriteIntPtr(outFn, Marshal.GetFunctionPointerForDelegate(del));
                return 0;
            }
            catch (Exception)
            {
                return -102;
            }
        }

        // ---- BootUnload ----

        [UnmanagedCallersOnly(EntryPoint = "BootUnload")]
        public static int BootUnload(IntPtr args, int size)
        {
            if (_alc is null) return -1;
            // 官方《assembly unloadability》教程口径：unload 必须在 NoInlining 小方法内
            // 完成并**返回 WeakReference**，GC 排空在调用方——方法边界清除栈 root
            //（同方法内 `alc = null` 是 dead store，JIT 会删除，引用留栈挡回收，
            // M-SC3 实测：卸载与排空同方法时 weakref 永远 alive）。
            WeakReference? weak = UnloadAndWeak(_alc);
            _alc = null;

            if (weak is null || !weak.IsAlive) return 0;
            // GC 排空：官方口径「Unload 尽力而为，弱引用校验是唯一判据」。
            for (int i = 0; i < 30 && weak.IsAlive; i++)
            {
                GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
                GC.WaitForPendingFinalizers();
                GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            }
            return weak.IsAlive ? 1 : 0;
        }

        /// <summary>NoInlining 卸载步：清静态引用 + Unload，weakref 经返回值带出。
        /// 本方法返回后，alc / 程序集的栈引用随之消亡（不得与排空循环同帧）。</summary>
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        private static WeakReference? UnloadAndWeak(ScriptLoadContext alc)
        {
            // 先清 tick thunk 保活（delegate rooted 挡回收——次序铁律），再 Unload。
            _tickDelegates.Clear();
            WeakReference? weak = _lastAssembly is null ? null : new WeakReference(_lastAssembly);
            _lastAssembly = null;
            alc.Unload();
            return weak;
        }
    }
}
