// M-SC11 跨域示例脚本（整轨收官演示）：Time + Input + Ecs + Log + Camera + Props
// 六域一脚本打通（拍板 Q-SC11-1 = A：五域+Props，runScene 单模式——Physics 在
// 训练场模式与 Camera/Ecs 互斥，不进本脚本；Input 在 viewer 双模式均不绑
// ActionMap，演示 NoSource 既定语义 = 负向锚）。
//
// 入口约定（props 示例同口径）：Entry/OnTick/PropGet/PropSet 全部普通静态方法
//（BootCall/BootTickPtr 反射路径——.NET 8 禁反射 Invoke UC 方法，故不带 UC）。
// Entry（装载期，world/clock/camSource 已绑）：推 [Prop] 描述表 + 建实体挂
// cube.glb（帧边界整场景重建 → 渲染可见，M-SC7 方案 A）+ 六域逐一锚定。
// OnTick（host 级逻辑帧）：相机环绕（确定性：frameDt = 固定步长，同 tick 同
// 值）+ sweep 锚（固定文本，不带动态数字——保 log 字节对拍）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class TourScript
    {
        // ---- [Prop] 暴露面（四 kind 各一；面板初值经 propGet 拉取）----
        [Prop("SpinSpeed", 0f, 90f)] public static float SpinSpeed = 15f;   // 度/秒
        [Prop("SweepEvery", 1, 600)] public static int SweepEvery = 120;    // sweep 周期（tick）
        [Prop("SpinEnabled")] public static bool SpinEnabled = true;
        [Prop("TourLabel")] public static string TourLabel = "tour";

        static double yaw_ = double.NaN;   // 累加器（Entry 首帧从相机现值起步）

        // Entry：装载期六域锚定 + 推描述表。返回 0 = 全部锚一致；非 0 = 失败码。
        public static unsafe int Entry(IntPtr args, int size)
        {
            Engine.Init(args);

            // Props：推 [Prop] 描述表（gen+1 = 面板重建信号）
            int prc = PropsPush.PushStatic(typeof(TourScript));
            if (prc != PropsCode.Ok) return 10 + prc;

            // Ecs：建实体 + Transform + 固定位姿 + 挂模型（白名单内合法名）
            if (Ecs.Spawn("tour_marker", out ulong id) != EcsCode.Ok) return 1;
            if (id == 0) return 2;
            if (Ecs.AddTransform(id) != EcsCode.Ok) return 3;
            if (Ecs.TransformGet(id, out var t) != EcsCode.Ok) return 4;
            t.Px = 5f; t.Py = 2.8f;                        // 偏出 ecs-e1b 猴头群独立入镜
            if (Ecs.TransformSet(id, t) != EcsCode.Ok) return 5;
            if (Ecs.AttachModel(id, "TorusKnotDense.glb") != EcsCode.Ok) return 6;

            // Flush：Spawn 新建立即可见（组件/实体落表），但**按名查找族只扫已
            // flush 实体**——不 flush 则 OnTick tick=0 的 FindByName 落空
            //（sweep 分支 0%N==0 首 tick 即触发——newEntity 延迟落表实测坑）。
            if (Ecs.Flush() != EcsCode.Ok) return 7;

            // Log：装载锚（固定文本；level 2 = Info）
            if (Log.Write(2, "tour: entry ok (props+ecs+log)") != LogCode.Ok) return 8;

            // Input：scene 模式无 ActionMap（不隐式建表口径）→ NoSource = 既定语义
            if (Input.ActionFind("tour_action", out _) != InputCode.NoSource) return 9;
            if (Log.Write(2, "tour: input NoSource as designed (no ActionMap in scene mode)")
                != LogCode.Ok) return 10;

            // Time：帧时钟读通（simTime/frameDt/tickCount/paused 四元组）
            if (Time.GetState(out _, out _, out _, out _) != TimeCode.Ok) return 11;

            // Camera：位姿读通 + yaw 累加器起步（从相机现值起，不改构图锚）
            if (Camera.GetPose(out var pose) != CameraCode.Ok) return 12;
            yaw_ = pose.Yaw;

            return 0;
        }

        // OnTick：host 级逻辑帧驱动（viewer scene 模式）。契约（orbit 示例同款）：
        // args = 指向 double dt（逻辑步长秒），size = 8；**不调 Engine.Init**
        //（args 不是 ApiTable*——把 dt 位型当表指针解引用 = AV，首版实测教训）。
        // Engine 静态指针由 Entry（反射通道，同 ALC 静态共享）装载期初始化。
        public static unsafe int OnTick(IntPtr args, int size)
        {
            if (size < sizeof(double) || args == IntPtr.Zero) return 1;
            double dt = *(double*)args;
            if (Time.GetState(out _, out _, out ulong tick, out _) != TimeCode.Ok)
                return 2;

            // 相机环绕（SpinEnabled 可经面板实时关停——Props 值通道活演示）
            if (SpinEnabled && Camera.GetPose(out var pose) == CameraCode.Ok)
            {
                yaw_ += SpinSpeed * dt;                    // 固定步长 → 确定性
                Camera.SetPose(new CameraPose(pose.Eye, (float)yaw_, pose.Pitch, pose.Fov, pose.Mode));
            }

            // sweep 锚：Ecs.FindByName + Exists 复核 + 固定文本 log（无动态数字）
            if (SweepEvery > 0 && tick % (ulong)SweepEvery == 0)
            {
                if (Ecs.FindByName("tour_marker", out ulong mid) != EcsCode.Ok || mid == 0) return 2;
                if (Ecs.Exists(mid, out bool ex) != EcsCode.Ok || !ex) return 3;
                if (Log.Write(2, "tour: sweep ok (time+camera+ecs live, props panel attached)")
                    != LogCode.Ok) return 4;
            }
            return 0;
        }

        // ---- 可选契约（值通道；BootTickPtr 协议按名解析，一行转发）----

        public static unsafe int PropGet(IntPtr args, int size)
            => PropsPush.PropGet(typeof(TourScript), args, size);

        public static unsafe int PropSet(IntPtr args, int size)
            => PropsPush.PropSet(typeof(TourScript), args, size);
    }
}
