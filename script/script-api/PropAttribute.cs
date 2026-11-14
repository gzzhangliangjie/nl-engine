// M-SC10：属性面板反射助手（P8-SCRIPT-PLAN.md M-SC10 行，拍板 Q-SC10-1~4 = A/A/A/A）。
//
// 载体 = 入口脚本类 [Prop] **静态字段**（M-SC2b ScriptComponent 维持默认不做）。
// 用法（脚本侧三行契约）：
//   Entry 内开头： PropsPush.PushStatic(typeof(MyScript));
//   可选通道：    public static unsafe int PropGet(IntPtr args, int size)
//                    => PropsPush.PropGet(typeof(MyScript), args, size);
//                 public static unsafe int PropSet(IntPtr args, int size)
//                    => PropsPush.PropSet(typeof(MyScript), args, size);
// 缺 PropGet/PropSet = 通道惰性（面板空转）；缺 PushStatic = 无描述表（面板空）。
//
// 生命周期账本硬规则①落地：FieldInfo 反射缓存（_cache）在本程序集 static 字段——
// script-api 经 ScriptLoadContext 同 ALC 装载（Bootstrap.cs Load 回退），reload 后
// 新 ALC 携新缓存整体回收，C++ 侧只见 POD 描述表（Bindings.hpp 口径）。
//
// 字段序 = MetadataToken 升序（GetFields 返回序无保证，取稳定序；声明序的工程
// 近似——同一编译单元内 token 随声明递增，实测 C# 编译器成立）。
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace Nl.Script.Api
{
    /// <summary>属性面板暴露标记（Carrot 特性集同构：DisplayName/Range/Default）。
    /// Default 不单独声明 = 推送时字段现值快照（避免双源漂移）。</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class PropAttribute : Attribute
    {
        public string DisplayName { get; }
        public float Min { get; }
        public float Max { get; }
        public bool HasRange => Min != Max;

        /// <summary>无 Range 形态（bool/string 用；float/int 退直输控件）。</summary>
        public PropAttribute(string displayName) { DisplayName = displayName; }

        /// <summary>带 Range 形态（float/int：面板 SliderFloat/SliderInt）。</summary>
        public PropAttribute(string displayName, float min, float max)
        {
            DisplayName = displayName;
            Min = min;
            Max = max;
        }
    }

    /// <summary>描述表推送 + 可选契约值通道（kind 0=f/1=i/2=b/3=s，Bindings.hpp 同文锚）。</summary>
    public static unsafe class PropsPush
    {
        static readonly Dictionary<Type, FieldInfo[]> _cache = new();

        static FieldInfo[] Fields(Type t)
        {
            if (_cache.TryGetValue(t, out var cached)) return cached;
            var list = new List<FieldInfo>();
            foreach (var fi in t.GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                if (fi.GetCustomAttribute<PropAttribute>() == null) continue;
                if (fi.FieldType == typeof(float) || fi.FieldType == typeof(int)
                    || fi.FieldType == typeof(bool) || fi.FieldType == typeof(string))
                    list.Add(fi);
                // 其余类型静默跳过（拍板 Q-SC10-2 四基本类型；防脚本笔误炸 Entry）
            }
            list.Sort((a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
            var arr = list.ToArray();
            _cache[t] = arr;
            return arr;
        }

        static int KindOf(FieldInfo fi) =>
            fi.FieldType == typeof(float) ? 0 :
            fi.FieldType == typeof(int) ? 1 :
            fi.FieldType == typeof(bool) ? 2 : 3;

        /// <summary>反射枚举 t 的 [Prop] 静态字段 → 推描述表（Entry 内开头调；
        /// 重复调 = 覆盖，reload 后新 ALC 重新调 = 面板自动跟随）。返回 rc。</summary>
        public static int PushStatic(Type t)
        {
            var fields = Fields(t);
            int rc = Props.PushBegin();
            if (rc != 0) return rc;
            for (int i = 0; i < fields.Length; i++)
            {
                var fi = fields[i];
                var attr = fi.GetCustomAttribute<PropAttribute>()!;
                float def = 0f;
                if (fi.FieldType == typeof(float)) def = (float)fi.GetValue(null)!;
                else if (fi.FieldType == typeof(int)) def = (int)fi.GetValue(null)!;
                else if (fi.FieldType == typeof(bool)) def = (bool)fi.GetValue(null)! ? 1f : 0f;
                // string def 恒 0（描述表无 string 初值位；面板初值经 PropGet 拉）
                rc = Props.DescAdd(KindOf(fi), attr.DisplayName, attr.Min, attr.Max, def);
                if (rc != 0) return rc;
            }
            return 0;
        }

        // ---- 可选契约实现（值 ABI：buf[0..3]=idx；标量 buf[4..7]；string buf[4..259] UTF8 尾 0）----

        /// <summary>脚本侧一行转发：public static unsafe int PropGet(IntPtr, int) => PropsPush.PropGet(typeof(T), args, size)。</summary>
        public static int PropGet(Type t, IntPtr args, int size)
        {
            if (args == IntPtr.Zero || size < 8) return 1;
            int idx = Marshal.ReadInt32(args, 0);
            var fields = Fields(t);
            if (idx < 0 || idx >= fields.Length) return 2;
            var fi = fields[idx];
            object? v = fi.GetValue(null);
            if (fi.FieldType == typeof(string))
            {
                if (size < 4 + kStringCap + 1) return 1;
                var bytes = Encoding.UTF8.GetBytes((string?)v ?? "");
                int n = Math.Min(bytes.Length, kStringCap);
                for (int i = 0; i < n; i++) Marshal.WriteByte(args, 4 + i, bytes[i]);
                Marshal.WriteByte(args, 4 + n, 0);
                return 0;
            }
            if (fi.FieldType == typeof(float))
                Marshal.WriteInt32(args, 4, BitConverter.SingleToInt32Bits((float)v!));
            else if (fi.FieldType == typeof(int))
                Marshal.WriteInt32(args, 4, (int)v!);
            else
                Marshal.WriteInt32(args, 4, (bool)v! ? 1 : 0);
            return 0;
        }

        /// <summary>脚本侧一行转发：public static unsafe int PropSet(IntPtr, int) => PropsPush.PropSet(typeof(T), args, size)。</summary>
        public static int PropSet(Type t, IntPtr args, int size)
        {
            if (args == IntPtr.Zero || size < 8) return 1;
            int idx = Marshal.ReadInt32(args, 0);
            var fields = Fields(t);
            if (idx < 0 || idx >= fields.Length) return 2;
            var fi = fields[idx];
            if (fi.FieldType == typeof(string))
            {
                if (size < 4 + kStringCap + 1) return 1;
                int n = 0;
                while (n < kStringCap && Marshal.ReadByte(args, 4 + n) != 0) ++n;
                var bytes = new byte[n];
                for (int i = 0; i < n; i++) bytes[i] = Marshal.ReadByte(args, 4 + i);
                fi.SetValue(null, Encoding.UTF8.GetString(bytes));
                return 0;
            }
            int raw = Marshal.ReadInt32(args, 4);
            if (fi.FieldType == typeof(float)) fi.SetValue(null, BitConverter.Int32BitsToSingle(raw));
            else if (fi.FieldType == typeof(int)) fi.SetValue(null, raw);
            else fi.SetValue(null, raw != 0);
            return 0;
        }

        const int kStringCap = 255;   // Bindings.hpp kPropStringCap 同文锚
    }
}
