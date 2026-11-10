// M-GL4 验收脚本：宿主级 OnTick（逻辑帧驱动轨道运动）。
//
// 契约（nl/script/ScriptHost.hpp M-GL4 节）：
//   - Entry（scriptCall 反射通道）收 ApiTable* —— 必须 Engine.Init（静态指针
//     每次 reload 重置，装载期初始化一次）。
//   - OnTick（bootstrap BootTickPtr 通道，禁 getEntryPoint 直拉——双副本陷阱）
//     收 args = 指向 double dt（逻辑步长秒），size = 8。
//   - 返回 0 = ok；非 0 / 抛异常 = 宿主停用 tick（防刷屏），reload-script 恢复。
//     [UnmanagedCallersOnly] 回调不许抛托管异常 —— 全部 ECS 调用走错误码检查。
//
// 脚本态纪律：t（轨道相位）与 orb id 是**脚本静态态**——reload 重置（t 归零、
// orb 重新 FindByName），宿主态（实体树 / tickCount / cvar）不受影响 =
// 「状态全在 C++」口径的对称演示（s8d 行为换新的 tick 面）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class OrbitScript
    {
        const float Radius = 1.6f;   // 与 gl2 orb-a 同量级（轨道半径几何不变量）
        const float Omega = 0.9f;    // rad/s（SpinSystem 家族同量级）
        static double t = 0.0;       // 脚本态：轨道相位，reload 重置
        static ulong orb = 0;        // 脚本态：目标实体句柄，reload 重挂

        // 直调包装（getEntryPoint 通道，nl-script run/check 用）：与 hello 脚本
        // 同款 EntryNative 约定——反射通道不许调 [UnmanagedCallersOnly]，故由
        // 本包装转发。
        [UnmanagedCallersOnly]
        public static int EntryNative(IntPtr args, int size) => Entry(args, size);

        // 装载入口（scriptCall "Entry" 反射通道）：解析 ApiTable + 找目标实体。
        // 注意不许标 [UnmanagedCallersOnly]——托管反射 Invoke 对该方法抛
        // NotSupportedException（hello 脚本同款结论）；OnTick 走 getEntryPoint
        // 原生直调才需要它。
        public static int Entry(IntPtr args, int size)
        {
            Engine.Init(args);
            int rc = Ecs.FindByName("script-orb", out orb);
            if (rc != EcsCode.Ok || orb == 0) return 1;
            return 0;
        }

        // 逻辑帧回调（宿主 getEntryPoint 直调）：t += dt，写轨道位置。
        // 每 tick 一次；pause = 宿主不调 = 脚本同步冻结（M-GL2 同构）。
        // 逻辑帧回调（宿主 clock callback 内、world.tick 之前）：t += dt，写轨道
        // 位置。每 tick 一次；pause = 宿主不调 = 脚本同步冻结（M-GL2 同构）。
        // 经 bootstrap BootTickPtr 带出本机指针（delegate thunk），无需
        // [UnmanagedCallersOnly]——hostfxr 直拉会装第二份默认 ALC 副本（禁）。
        public static unsafe int OnTick(IntPtr args, int size)
        {
            if (size < sizeof(double) || args == IntPtr.Zero) return 1;
            double dt = *(double*)args;
            t += dt;

            var e = new SceneEntity(orb);
            if (!e.Exists) return 2;
            float a = (float)(Omega * t);
            return e.SetTransform(new TransformData
            {
                Px = Radius * MathF.Cos(a),
                Py = 0f,
                Pz = Radius * MathF.Sin(a),
            });
        }
    }
}
