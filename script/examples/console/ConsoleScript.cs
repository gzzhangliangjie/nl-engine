// M-SC1 console 绑定示例：多入口静态方法（[UnmanagedCallersOnly] 默认签名）。
// 约定：绑定入口的 args = nl::script::ApiTable*（nl/script/Bindings.hpp ABI）。
// 各入口由 nl-script selftest / run --api 按名取用，验收判据见 P8-SCRIPT-PLAN.md M-SC1。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class ConsoleScript
    {
        // ---- selftest 入口 ----

        /// <summary>s6a：C# 改 cvar（C++ 侧已挂 onChanged 钩子）→ 返回 CvarSet 码。</summary>
        [UnmanagedCallersOnly(EntryPoint = "SetCvar")]
        public static int SetCvar(IntPtr table, int size)
        {
            Engine.Init(table);
            return Engine.CvarSet("script.count", "77");
        }

        /// <summary>s6b：C# 注册命令 script.echo（args 原样拼接回显）。</summary>
        [UnmanagedCallersOnly(EntryPoint = "RegisterCmd")]
        public static int RegisterCmd(IntPtr table, int size)
        {
            Engine.Init(table);
            return Engine.ConsoleRegister("script.echo", "echo args back (C# registered)",
                                          args => "script.echo: " + string.Join(" ", args));
        }

        /// <summary>
        /// s6c：错误码逐一对齐（C# 视角触发，返回断言位掩码；期望 31）。
        ///   bit0: exec 未知命令 -> 1 (UnknownCommand)
        ///   bit1: cvar.set 引擎路径类型不符 -> 2
        ///   bit2: exec cvar.set 命令路径坏值 -> 3 (CommandFailed)
        ///   bit3: exec 裸 CVar 名 -> 0 (Ok，打印现值)
        ///   bit4: CvarGet 读回 77（与 C++ 侧赋值一致）
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "ErrorCodes")]
        public static int ErrorCodes(IntPtr table, int size)
        {
            Engine.Init(table);
            int mask = 0;
            if (Engine.ConsoleExec("no_such_cmd_xyz").Code == 1) mask |= 1;
            if (Engine.CvarSet("script.count", "notanumber") == 2) mask |= 2;
            if (Engine.ConsoleExec("cvar.set script.count notanumber").Code == 3) mask |= 4;
            var ok = Engine.ConsoleExec("script.count");
            if (ok.Code == 0 && ok.Output.Contains("77")) mask |= 8;
            if (Engine.CvarGet("script.count") == "77") mask |= 16;
            return mask;
        }

        /// <summary>s6d：注册原始 ABI 命令（直接抛托管异常 → 走 C++ SEH 边界）。</summary>
        [UnmanagedCallersOnly(EntryPoint = "ThrowCmd")]
        public static int ThrowCmd(IntPtr table, int size)
        {
            Engine.Init(table);
            return Engine.ConsoleRegisterRaw("script.throwcmd", "always throws (SEH boundary probe)",
                (int argc, IntPtr argv, out IntPtr text) =>
                {
                    text = IntPtr.Zero;
                    throw new InvalidOperationException("boom from C# command");
                });
        }

        /// <summary>s6e：C++ 类型化赋值后 C# 读回 + cvar.list 全量视图含脚本 cvar。</summary>
        [UnmanagedCallersOnly(EntryPoint = "Readback")]
        public static int Readback(IntPtr table, int size)
        {
            Engine.Init(table);
            int mask = 0;
            if (Engine.CvarGet("script.rate") == "1.5") mask |= 1;          // C++ setFloat 1.5
            if (Engine.CvarList().Contains("script.count")) mask |= 2;      // 全量视图含脚本 cvar
            return mask;                                                     // 期望 3
        }

        /// <summary>s6f：注销 script.echo → 再 exec 应 UnknownCommand（M-SC3 解绑预演）。</summary>
        [UnmanagedCallersOnly(EntryPoint = "UnregisterCmd")]
        public static int UnregisterCmd(IntPtr table, int size)
        {
            Engine.Init(table);
            return Engine.ConsoleUnregister("script.echo");
        }

        // ---- run --api 演示入口 ----

        /// <summary>完整演示：注册 + 赋值 + 执行 + 读回，结果走 stdout（run 用例肉眼可验）。</summary>
        [UnmanagedCallersOnly(EntryPoint = "Entry")]
        public static int Entry(IntPtr table, int size)
        {
            Engine.Init(table);
            Console.WriteLine("script-api v" + Api.Version + " console binding demo");

            int rc = Engine.ConsoleRegister("script.echo", "echo args back (C# registered)",
                                            args => "script.echo: " + string.Join(" ", args));
            Console.WriteLine("register script.echo -> " + rc);

            rc = Engine.CvarSet("script.count", "77");
            Console.WriteLine("cvar.set script.count 77 -> " + rc);

            var r = Engine.ConsoleExec("script.echo hello from csharp");
            Console.WriteLine("exec script.echo -> " + r);

            r = Engine.ConsoleExec("cvar.set script.count 100");
            Console.WriteLine("exec cvar.set script.count 100 -> " + r);
            Console.WriteLine("cvar get script.count -> " + (Engine.CvarGet("script.count") ?? "(null)"));

            Console.WriteLine("cvar list:" + Engine.CvarList());
            return 0;
        }
    }
}
