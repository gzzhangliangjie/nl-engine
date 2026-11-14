// M-SC10 验收脚本：属性面板域（拍板 Q-SC10-1~4 = A/A/A/A：静态字段载体 /
// 四基本类型 / --prop-panel additive / 交代物全要）。
//
// 入口约定：public static int(IntPtr args, int size)，args = ApiTable*；
// 全部经 BootCall/BootTickPtr 反射路径（.NET 8 禁反射 Invoke UC 方法，故不带 UC——
// hello Entry/OnTick 契约同口径）。Entry 开头
// PropsPush.PushStatic(typeof(PropsScript)) 反射 [Prop] 静态字段 → 推 POD
// 描述表（gen+1 = 面板重建信号）；PropGet/PropSet 可选契约一行转发——
// 缺失 = 通道惰性（面板空转），在 = 面板读写直达下列静态字段。
// 字段序 = MetadataToken 升序（声明序稳定化，PropsPush.Fields 同文锚）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class PropsScript
    {
        // ---- [Prop] 暴露面（kind 0..3 各一；Range/直输两种控件形态都有）----
        [Prop("Speed", 0f, 10f)] public static float Speed = 3.5f;    // kind 0：Range 滑条
        [Prop("Count", 0, 100)] public static int Count = 7;          // kind 1：Range 滑条
        [Prop("Enabled")] public static bool Enabled = true;          // kind 2：Checkbox
        [Prop("Label")] public static string Label = "hello props";   // kind 3：InputText
        // 非暴露（无特性 / 类型不在四基本集——PropsPush 静默跳过）
        public static float Hidden = 1f;
        [Prop("BadKind")] public static object BadKind = new object();

        // Entry：推描述表（重复调 = 覆盖；reload 后新 ALC 重调 = 面板自动跟随）。
        // 无 [UnmanagedCallersOnly]：经 ScriptHost::loadScript → BootCall 反射
        // Invoke 路径调用（.NET 8 禁反射调用 UC 方法——hello EntryNative 同款口径）。
        public static unsafe int Entry(IntPtr args, int size)
        {
            Engine.Init(args);
            int rc = PropsPush.PushStatic(typeof(PropsScript));
            return rc == PropsCode.Ok ? 0 : 10 + rc;
        }

        // ---- 可选契约（值通道；BootTickPtr 协议按名解析，一行转发）----

        public static unsafe int PropGet(IntPtr args, int size)
            => PropsPush.PropGet(typeof(PropsScript), args, size);

        public static unsafe int PropSet(IntPtr args, int size)
            => PropsPush.PropSet(typeof(PropsScript), args, size);

        // ---- selftest s13 探针（C++ 侧经 BootCall 按名调用；drive 同款约定）----

        /// <summary>s13a 正向锚：读四个字段的现值（按 idx 定点读）。
        /// args = { int idx; int* outRaw }；返回 0=ok / 非 0 = 失败码。</summary>
        public static unsafe int ReadField(IntPtr args, int size)
        {
            int idx = Marshal.ReadInt32(args, 0);
            IntPtr outRaw = Marshal.ReadIntPtr(args, 8);
            switch (idx)
            {
                case 0: Marshal.WriteInt32(outRaw, 0, BitConverter.SingleToInt32Bits(Speed)); return 0;
                case 1: Marshal.WriteInt32(outRaw, 0, Count); return 0;
                case 2: Marshal.WriteInt32(outRaw, 0, Enabled ? 1 : 0); return 0;
                default: return 3;
            }
        }

        /// <summary>s13d 泄露段探针：托管侧 GC 全收（万次串往返后句柄漂移断言前置）。</summary>
        public static unsafe int GCCollect(IntPtr args, int size)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            return 0;
        }
    }
}
