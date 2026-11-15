// M-W2A 验收脚本：Animation 域绑定面（薄面句柄表；selftest s18 节逐个调用）。
//
// 入口约定与 mouse 示例同口径：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 本域无宿主实例注入（句柄表在 C++ script 内部面）——「未绑」负向 = 无效句柄
// 全族失效安全拒绝。资产 = assets/models/RiggedChain.glb（M-A4b 程序化资产：
// 3 骨链 hip->spine->head + 单 sway clip 2.0s；关键帧手算可断言）。
// 状态转移口径：规则在 C++ 侧预置 lambda（判据 params[idx] >= threshold），脚本
// 只写标量参数 + Tick；转移当帧 alpha 从 0 起（nl/anim 确定性时序）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class AnimScript
    {
        // s18a：绑定路径全链断言。返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 加载 clip（3 骨 1 clip）：id>0；越界 index / 白名单拒绝 / 缺文件负向
            if (Anim.LoadClip("RiggedChain.glb", 0, out int clipId, out _) != AnimCode.Ok
                    || clipId <= 0) return 1;
            if (Anim.LoadClip("RiggedChain.glb", 99, out _, out _) != AnimCode.BadArg) return 2;
            if (Anim.LoadClip("../secret.glb", 0, out _, out _) != AnimCode.BadAsset) return 3;
            if (Anim.LoadClip("Nope.glb", 0, out _, out string? loadErr) != AnimCode.Load
                    || string.IsNullOrEmpty(loadErr)) return 4;

            // 建状态机：两态共用同一 clip（StateDef 按值持拷贝）；两条规则（声明序=优先级）：
            // 0->1 @param0>=0.5 blend 0.25s / 1->0 @param0>=0.2 blend 0.2s
            var rules = new AnimRule[] { new AnimRule(0, 1, 0, 0.5f, 0.25f),
                                         new AnimRule(1, 0, 0, 0.2f, 0.2f) };
            if (Anim.CreateStateMachine(new[] { clipId, clipId }, rules, out int sm)
                    != AnimCode.Ok || sm <= 0) return 5;

            // 初值：param0=0 → 稳定态 0（未转移 alpha=1 / duration=0）
            if (Anim.SetParam(sm, 0, 0f) != AnimCode.Ok) return 6;
            if (Anim.Tick(sm, 0.016f) != AnimCode.Ok) return 7;
            if (Anim.StateInfo(sm, out int st0, out float a0, out float d0) != AnimCode.Ok
                    || st0 != 0 || a0 != 1f || d0 != 0f) return 8;

            // 触发转移：param0=1.0（>=0.5）→ Tick 后 current=1、当帧 alpha 从 0 起（<1）
            if (Anim.SetParam(sm, 0, 1f) != AnimCode.Ok) return 9;
            if (Anim.Tick(sm, 0.016f) != AnimCode.Ok) return 10;
            if (Anim.StateInfo(sm, out int st1, out float a1, out float d1) != AnimCode.Ok
                    || st1 != 1 || a1 >= 1f || d1 <= 0f) return 11;

            // 回退不触发：param0=0（<0.2）→ 停在态 1，crossfade 推进中（0<alpha<1）
            if (Anim.SetParam(sm, 0, 0f) != AnimCode.Ok) return 12;
            if (Anim.Tick(sm, 0.016f) != AnimCode.Ok) return 13;
            if (Anim.StateInfo(sm, out int st2, out float a2, out _) != AnimCode.Ok
                    || st2 != 1 || a2 <= 0f || a2 >= 1f) return 14;

            // pose 读回：hip 局部平移 (0,1,0)（静态 TRS 单关键帧，手算锚）；
            // 万次级串往返（确定性无漂移）
            if (Anim.GetPose(sm, 0, out float[] hip) != AnimCode.Ok) return 15;
            if (System.MathF.Abs(hip[1] - 1f) > 1e-4f) return 16;
            for (int i = 0; i < 10000; ++i)
                if (Anim.Tick(sm, 0.01f) != AnimCode.Ok) return 17;
            if (Anim.GetPose(sm, 2, out _) != AnimCode.Ok) return 18;

            // 负向：参数下标越界 / 骨骼下标越界（params 表容量 = 规则引用的最大 idx+1 = 1）
            if (Anim.SetParam(sm, 5, 1f) != AnimCode.BadArg) return 19;
            if (Anim.GetPose(sm, 99, out _) != AnimCode.BadArg) return 20;

            // 显式回收（双态擦除）：先 sm 后 clip；回收后全族 Invalid 失效安全
            if (Anim.Release(sm) != AnimCode.Ok) return 21;
            if (Anim.GetPose(sm, 0, out _) != AnimCode.Invalid) return 22;
            if (Anim.Release(clipId) != AnimCode.Ok) return 23;
            if (Anim.Release(999999) != AnimCode.Invalid) return 24;
            if (Anim.Release(0) != AnimCode.BadArg) return 25;

            // GC.Collect 句柄漂移断言（生命周期硬规则②：C++ 按值持表，托管回收不动句柄）
            if (Anim.LoadClip("RiggedChain.glb", 0, out int clipId2, out _) != AnimCode.Ok) return 26;
            if (Anim.CreateStateMachine(new[] { clipId2 }, Array.Empty<AnimRule>(), out int sm2)
                    != AnimCode.Ok || sm2 <= 0) return 27;
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            if (Anim.Tick(sm2, 0.016f) != AnimCode.Ok) return 28;
            if (Anim.Release(sm2) != AnimCode.Ok) return 29;
            if (Anim.Release(clipId2) != AnimCode.Ok) return 30;

            return 0;
        }

        // s18b：无效句柄路径（本域无宿主实例；负向 = 查表失效安全拒绝）。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Anim.SetParam(999999, 0, 1f) != AnimCode.Invalid) return 1;
            if (Anim.Tick(999999, 0.016f) != AnimCode.Invalid) return 2;
            if (Anim.GetPose(999999, 0, out _) != AnimCode.Invalid) return 3;
            if (Anim.StateInfo(999999, out _, out _, out _) != AnimCode.Invalid) return 4;
            if (Anim.Release(999999) != AnimCode.Invalid) return 5;
            if (Anim.CreateStateMachine(new[] { 999999 },
                    new AnimRule[] { new AnimRule(0, 1, 0, 0.5f, 0.25f) }, out _)
                    != AnimCode.Invalid) return 6;
            if (Anim.CreateStateMachine(Array.Empty<int>(), Array.Empty<AnimRule>(), out _)
                    != AnimCode.BadArg) return 7;

            return 0;
        }

        // s18c：换载钩子种子（宿主 scriptUnregisterAll 前）：留 1 clip + 1 sm 活句柄
        //（句柄单源发号从 1 起：clip=1 / sm=2——VerifyProbe 依赖此确定性编号）。
        [UnmanagedCallersOnly(EntryPoint = "SeedProbe")]
        public static int SeedProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Anim.LoadClip("RiggedChain.glb", 0, out int clipId, out _) != AnimCode.Ok
                    || clipId != 1) return 1;
            if (Anim.CreateStateMachine(new[] { clipId }, Array.Empty<AnimRule>(), out int sm)
                    != AnimCode.Ok || sm != 2) return 2;
            if (Anim.Tick(sm, 0.016f) != AnimCode.Ok) return 3;

            return 0;
        }

        // s18d：换载钩子验证（宿主 scriptUnregisterAll 后——reloadScript 的统一解绑点）：
        // 旧句柄 1/2 必须已失效（失效安全拒绝）；新 LoadClip 因发号器重置回到 1。
        [UnmanagedCallersOnly(EntryPoint = "VerifyProbe")]
        public static int VerifyProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Anim.Release(1) != AnimCode.Invalid) return 1;   // 旧 clip 已清
            if (Anim.Release(2) != AnimCode.Invalid) return 2;   // 旧 sm 已清
            if (Anim.LoadClip("RiggedChain.glb", 0, out int clipId, out _) != AnimCode.Ok
                    || clipId != 1) return 3;                     // 发号器已重置
            if (Anim.Release(clipId) != AnimCode.Ok) return 4;

            return 0;
        }
    }
}
