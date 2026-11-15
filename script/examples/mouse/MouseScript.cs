// M-I3 验收脚本：鼠标裸查询域绑定面（多入口静态方法，selftest s14 节逐个调用）。
//
// 入口约定与 timeinput 示例同口径：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 宿主注入序列（main.cpp s14）：
//   feedMouseButton(0,true)                 → 左键 down + justPressed
//   feedMouseButton(1,true) + (1,false)     → 右键 justReleased（down=false）
//   feedMouseDelta(3.5, -2.0)               → 帧增量
//   feedScroll(0, 1.25)                     → 滚轮增量
//   feedCursor(640, 360, true)              → 绝对位置 + hasCursor
// 失效安全判据本体：未绑 InputState 时全部 Mouse* 导出回 NoSource，
// 不抛异常不崩（与 v3 Input 域 NoSource 口径同构）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class MouseScript
    {
        // s14a：绑定路径断言（宿主先注入鼠标序列并绑活 InputState）。
        // 返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 持续态：左键真 / 右键假 / 越界按钮码假（InputState mouseInTable 防御）
            if (Input.MouseIsDown(0, out bool leftDown) != InputCode.Ok || !leftDown) return 1;
            if (Input.MouseIsDown(1, out bool rightDown) != InputCode.Ok || rightDown) return 2;
            if (Input.MouseIsDown(9, out bool oobDown) != InputCode.Ok || oobDown) return 3;
            if (Input.MouseIsDown(-1, out bool negDown) != InputCode.Ok || negDown) return 4;

            // 边沿：左 justPressed 真 / 右 justReleased 真 / 交叉边沿假
            if (Input.MouseJustPressed(0, out bool ljp) != InputCode.Ok || !ljp) return 5;
            if (Input.MouseJustPressed(1, out bool rjp) != InputCode.Ok || rjp) return 6;
            if (Input.MouseJustReleased(1, out bool rjr) != InputCode.Ok || !rjr) return 7;
            if (Input.MouseJustReleased(0, out bool ljr) != InputCode.Ok || ljr) return 8;

            // 增量：帧增量 / 滚轮（宿主喂值逐位读回——3.5/-2.0/1.25 全为精确浮点）
            if (Input.MouseDelta(out float mdx, out float mdy) != InputCode.Ok) return 9;
            if (mdx != 3.5f || mdy != -2.0f) return 10;
            if (Input.ScrollDelta(out float sdx, out float sdy) != InputCode.Ok) return 11;
            if (sdx != 0.0f || sdy != 1.25f) return 12;

            // 绝对位置：640,360 + hasCursor 真
            if (Input.CursorPos(out float cx, out float cy, out bool has) != InputCode.Ok) return 13;
            if (cx != 640.0f || cy != 360.0f || !has) return 14;

            return 0;
        }

        // s14b：解绑路径（宿主先 inputSetState(nullptr)）。
        // 判据：全部 Mouse* 导出回 NoSource。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Input.MouseIsDown(0, out _) != InputCode.NoSource) return 1;
            if (Input.MouseJustPressed(0, out _) != InputCode.NoSource) return 2;
            if (Input.MouseJustReleased(0, out _) != InputCode.NoSource) return 3;
            if (Input.MouseDelta(out _, out _) != InputCode.NoSource) return 4;
            if (Input.ScrollDelta(out _, out _) != InputCode.NoSource) return 5;
            if (Input.CursorPos(out _, out _, out _) != InputCode.NoSource) return 6;

            return 0;
        }
    }
}
