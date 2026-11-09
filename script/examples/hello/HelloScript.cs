// M-SC0 最小用户脚本示例（docs/p8/P8-SCRIPT-PLAN.md）。
// 入口契约（plan 拍板默认签名）：int(void* args, int32 sizeBytes)，args = UTF-8 字节串。
// 纪律：脚本无跨帧状态（UE/鸣潮共同口径，热更安全根基）——本类只有静态纯函数。
using System;
using System.Runtime.InteropServices;
using System.Text;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class HelloScript
    {
        // UNMANAGEDCALLERSONLY_METHOD 路线要求 [UnmanagedCallersOnly]（coreclr_delegates.h 契约）。
        // 签名必须精确匹配 component_entry_point_fn：int(IntPtr, int)。
        [UnmanagedCallersOnly]
        public static int Entry(IntPtr args, int sizeBytes)
        {
            string mode = ReadArgs(args, sizeBytes);
            switch (mode)
            {
                case "ok":
                    return 42;
                case "echo":
                    Console.WriteLine($"[hello-script] script-api v{Api.Version}, host says hello");
                    return 0;
                case "throw":
                    // 有意抛出：验证托管异常在 C++ 互操作边界被 SEH 捕获转 Result（不穿透析构）。
                    throw new InvalidOperationException("boom from managed (intentional)");
                default:
                    return -1;
            }
        }

        private static string ReadArgs(IntPtr args, int sizeBytes)
        {
            if (args == IntPtr.Zero || sizeBytes <= 0) return string.Empty;
            var buf = new byte[sizeBytes];
            Marshal.Copy(args, buf, 0, sizeBytes);
            return Encoding.UTF8.GetString(buf);
        }
    }
}
