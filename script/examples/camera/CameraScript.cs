// M-SC6 验收脚本：Camera 域绑定面（多入口静态方法，selftest s10 组逐个调用）。
//
// 入口约定（与 timeinput 示例同口径）：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 宿主侧绑定 = 假 CameraSource（selftest 进程生命周期；罐头值 eye=(1.5,2.5,3.5) /
// yaw=30 / pitch=-10 / fov=60 / mode=0），语义与 viewer 适配源同构
// （setFov clamp [20,120]、GetFov 未绑 = NaN）。
// 失效安全判据本体：未绑 CameraSource 时各导出回 NoSource / GetFov 回 NaN，
// 不抛异常不崩（BoundProbe 前宿主已绑假源，UnboundProbe 前宿主已解绑）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class CameraScript
    {
        // s10a：绑定路径断言（罐头值读回锚 + 写路径 rc + fov clamp 经读回核）。
        // 返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // ---- GetPose：罐头值一次读回锚 ----
            int rc = Camera.GetPose(out CameraPose p);
            if (rc != CameraCode.Ok) return 1;
            if (Math.Abs(p.Eye[0] - 1.5f) > 1e-5f || Math.Abs(p.Eye[1] - 2.5f) > 1e-5f ||
                Math.Abs(p.Eye[2] - 3.5f) > 1e-5f) return 2;
            if (Math.Abs(p.Yaw - 30.0f) > 1e-5f || Math.Abs(p.Pitch - (-10.0f)) > 1e-5f) return 3;
            if (Math.Abs(p.Fov - 60.0f) > 1e-5f) return 4;
            if (p.Mode != 0) return 5;                                   // 假源初始 orbit

            // ---- SetPose：rc=Ok（C++ 侧在入口返回后核假源捕获 {4,5,6,45,15}）----
            rc = Camera.SetPose(new CameraPose(new[] { 4.0f, 5.0f, 6.0f }, 45.0f, 15.0f, 0.0f, 0));
            if (rc != CameraCode.Ok) return 6;
            if (Camera.SetPose(new CameraPose(null!, 0, 0, 0, 0)) != CameraCode.BadArg) return 7;   // eye null

            // ---- fov：读回 → 写 → 读回 → clamp 写 → clamp 读回 ----
            if (Math.Abs(Camera.GetFov() - 60.0f) > 1e-5f) return 8;
            if (Camera.SetFov(75.0f) != CameraCode.Ok) return 9;
            if (Math.Abs(Camera.GetFov() - 75.0f) > 1e-5f) return 10;    // 假源存 clamp 后值
            if (Camera.SetFov(500.0f) != CameraCode.Ok) return 11;
            if (Math.Abs(Camera.GetFov() - 120.0f) > 1e-5f) return 12;   // clamp [20,120] 生效

            return 0;
        }

        // s10b：track 模式（宿主先把假源 mode 翻成 3）——位姿主权在轨迹文件：
        // GetPose 照常（读路径无模式限制），SetPose 回 ModeUnsupported，fov 照常读写。
        [UnmanagedCallersOnly(EntryPoint = "TrackProbe")]
        public static int TrackProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            int rc = Camera.GetPose(out CameraPose p);
            if (rc != CameraCode.Ok || p.Mode != 3) return 1;
            if (Camera.SetPose(new CameraPose(new[] { 1.0f, 1.0f, 1.0f }, 0.0f, 0.0f, 0.0f, 0))
                != CameraCode.ModeUnsupported) return 2;
            if (Camera.SetFov(60.0f) != CameraCode.Ok) return 3;         // fov 模式统一生效

            return 0;
        }

        // s10c：解绑路径（宿主先 cameraSetSource(nullptr)）。
        // 判据：GetPose/SetPose/SetFov 回 NoSource；GetFov 回 NaN（f32 无 rc 通道账本特例）。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Camera.GetPose(out _) != CameraCode.NoSource) return 1;
            if (Camera.SetPose(new CameraPose(new[] { 1.0f, 1.0f, 1.0f }, 0.0f, 0.0f, 0.0f, 0))
                != CameraCode.NoSource) return 2;
            if (Camera.SetFov(60.0f) != CameraCode.NoSource) return 3;
            if (!float.IsNaN(Camera.GetFov())) return 4;

            return 0;
        }
    }
}
