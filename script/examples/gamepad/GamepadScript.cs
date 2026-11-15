// M-I5 验收脚本：手柄 / 字符域绑定面（selftest s15 节逐个调用）。
//
// 宿主注入序列（main.cpp s15）：
//   feedGamepadButton(0,true)               → A 键 down + justPressed
//   feedGamepadButton(1,true) + (1,false)   → B 键 justReleased（down=false）
//   feedGamepadAxis(0, 0.75)                → 左摇杆 X
//   feedGamepadAxis(4, 0.5)                 → 左扳机（0..1）
// Window 不绑（selftest 无窗口）→ TextPop 全程 NoSource。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class GamepadScript
    {
        // s15a：绑定路径断言（宿主先注入手柄序列并绑活 InputState）。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 按钮三态：A 真 / B 假 / 越界假（InputState padInTable 防御）
            if (Input.GamepadIsDown(0, out bool aDown) != InputCode.Ok || !aDown) return 1;
            if (Input.GamepadIsDown(1, out bool bDown) != InputCode.Ok || bDown) return 2;
            if (Input.GamepadIsDown(99, out bool oobDown) != InputCode.Ok || oobDown) return 3;
            if (Input.GamepadIsDown(-1, out bool negDown) != InputCode.Ok || negDown) return 4;

            // 轴值：左摇杆 X=0.75 / 左扳机=0.5 / 越界轴=0
            if (Input.GamepadAxis(0, out float lx) != InputCode.Ok || lx != 0.75f) return 5;
            if (Input.GamepadAxis(4, out float lt) != InputCode.Ok || lt != 0.5f) return 6;
            if (Input.GamepadAxis(5, out float rt) != InputCode.Ok || rt != 0.0f) return 7;
            if (Input.GamepadAxis(9, out float oobAxis) != InputCode.Ok || oobAxis != 0.0f) return 8;

            // TextPop：selftest 无窗口 → 即使 InputState 已绑，Window 未绑 = NoSource
            if (Input.TextPop(out _) != InputCode.NoSource) return 9;

            return 0;
        }

        // s15b：解绑路径（宿主先 inputSetState(nullptr)；Window 本就未绑）。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Input.GamepadIsDown(0, out _) != InputCode.NoSource) return 1;
            if (Input.GamepadAxis(0, out _) != InputCode.NoSource) return 2;
            if (Input.TextPop(out _) != InputCode.NoSource) return 3;

            return 0;
        }
    }
}
