// M-SC5 验收脚本：Time / Input / Log 域绑定面（多入口静态方法，selftest s9 组逐个调用）。
//
// 入口约定（与 ecs 示例同口径）：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 失效安全判据本体：未绑 Clock / InputState 时各域导出回 NoClock / NoSource，
// 不抛异常不崩（BoundProbe 前宿主已绑活实例，UnboundProbe 前宿主已全部解绑）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class TimeInputScript
    {
        // s9a：全部绑定路径断言（宿主先 tick x3 + 喂键 update + 绑活实例）。
        // 返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // ---- Time：状态读回锚（宿主 tick(16.6667ms) x3 → tickCount=3 / simTime=0.05）----
            int rc = Time.GetState(out double simTime, out double frameDt, out ulong tickCount, out bool paused);
            if (rc != TimeCode.Ok) return 1;
            if (tickCount != 3) return 2;
            if (Math.Abs(simTime - 0.05) > 1e-9) return 3;          // 3 * kStep，double 确定性
            if (frameDt <= 0.0 || frameDt >= 0.25) return 4;        // 缩放截断后 delta，kMaxDelta 界内
            if (paused) return 5;

            // ---- Time：控制 + 读回（C++ 侧在入口返回后再核 paused/timeScale 终态）----
            rc = Time.SetState(1, 2.5);                              // 暂停 + 倍率 2.5
            if (rc != TimeCode.Ok) return 6;
            rc = Time.GetState(out _, out _, out _, out paused);
            if (rc != TimeCode.Ok || !paused) return 7;
            rc = Time.SetState(-1, -1.0);                            // 双不变（哨兵语义）
            if (rc != TimeCode.Ok) return 8;
            rc = Time.SetState(0, -1.0);                             // 恢复继续（倍率保持 2.5）
            if (rc != TimeCode.Ok) return 9;

            // ---- Input：按名查 + 三型冻结值（宿主喂 W/D 并 update 后冻结）----
            rc = Input.ActionFind("s9bool", out uint boolId);
            if (rc != InputCode.Ok || boolId == 0xFFFFFFFFu) return 10;
            rc = Input.ActionFind("s9float", out uint floatId);
            if (rc != InputCode.Ok) return 11;
            rc = Input.ActionFind("s9vec2", out uint vecId);
            if (rc != InputCode.Ok) return 12;
            rc = Input.ActionFind("s9nope", out uint missingId);
            if (rc != InputCode.NoAction || missingId != 0xFFFFFFFFu) return 13;

            // Bool：W 按下 → value/justPressed 真、released 假
            rc = Input.ActionBool(boolId, out bool bv, out bool bp, out bool br);
            if (rc != InputCode.Ok || !bv || !bp || br) return 14;
            // 类型不符拒绝：Float/Vec2 id 打进 Bool 接口 → NoAction
            if (Input.ActionBool(floatId, out _, out _, out _) != InputCode.NoAction) return 15;
            // Float：W=+1（S 未按）→ value/delta 均 +1（首帧 prev=0）
            rc = Input.ActionFloat(floatId, out float fv, out float fd);
            if (rc != InputCode.Ok || fv != 1.0f || fd != 1.0f) return 16;
            // Vec2：D 轴键 +x → x/dx 均 +1
            rc = Input.ActionVec2(vecId, out float vx, out float vy, out float vdx, out float vdy);
            if (rc != InputCode.Ok || vx != 1.0f || vy != 0.0f || vdx != 1.0f || vdy != 0.0f) return 17;

            // 键持续态：W(87) 真 / S(83) 假 / 越界键码假（InputState 防御性口径）
            if (Input.KeyIsDown(87, out bool wDown) != InputCode.Ok || !wDown) return 18;
            if (Input.KeyIsDown(83, out bool sDown) != InputCode.Ok || sDown) return 19;
            if (Input.KeyIsDown(-5, out bool badDown) != InputCode.Ok || badDown) return 20;

            // ---- Log：写入 + 过滤指令 ----
            rc = Log.Write(2, "s9 log info anchor");                 // Info → [nl:info|script]
            if (rc != LogCode.Ok) return 21;
            if (Log.Write(9, "out of range") != LogCode.BadArg) return 22;   // level 越界
            rc = Log.SetFilter("nl:warn", out _);
            if (rc != LogCode.Ok) return 23;
            rc = Log.SetFilter("bad channel! =info", out string? err);
            if (rc != LogCode.BadFilter || string.IsNullOrEmpty(err)) return 24;   // 显式报错带原因

            return 0;
        }

        // s9b：解绑路径（宿主先 timeSetClock/inputSetState/inputSetActionMap(nullptr)）。
        // 判据：各域回 NoClock / NoSource，Log 不受影响（全局面，与宿主活实例无关）。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Time.GetState(out _, out _, out _, out _) != TimeCode.NoClock) return 1;
            if (Time.SetState(1, 2.5) != TimeCode.NoClock) return 2;
            if (Input.ActionFind("s9bool", out _) != InputCode.NoSource) return 3;
            if (Input.ActionBool(0, out _, out _, out _) != InputCode.NoSource) return 4;
            if (Input.ActionFloat(0, out _, out _) != InputCode.NoSource) return 5;
            if (Input.ActionVec2(0, out _, out _, out _, out _) != InputCode.NoSource) return 6;
            if (Input.KeyIsDown(87, out _) != InputCode.NoSource) return 7;
            if (Log.Write(2, "s9 unbound still ok") != LogCode.Ok) return 8;

            return 0;
        }
    }
}
