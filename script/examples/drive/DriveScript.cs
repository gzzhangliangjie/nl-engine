// M-SC8 验收脚本：Physics 载具域（多入口静态方法，selftest s12 组逐个调用）。
//
// 入口约定（与 camera/model 示例同口径）：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 操作目标 = 宿主侧 driveSetScene 绑定的 physics::DebugScene 训练场
//（M-NET2 多载具面直通；slot0 = build(2) 既有单车，脚本加的车从 slot1 起）。
// 线程口径 = OnTick 逻辑帧约定（selftest 探针内直调 = 同线程即时读回等价）。
// 失效安全判据本体：未绑 DebugScene 时各导出回 NoScene，不抛异常不崩
//（UnboundProbe 前宿主已 driveSetScene(nullptr)）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class DriveScript
    {
        // s12a：加车 + 输入 + 位姿读回 + 幽灵摆位 + tombstone 移除（全正向链）。
        // 返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 加车：slot0 = build(2) 既有单车，脚本车 = slot1
            int rc = Physics.AddVehicle(5.0f, -5.0f, out int slot);
            if (rc != PhysicsCode.Ok) return 1;
            if (slot != 1) return 2;

            // 四输入写（Jolt 手刹 float 0..1 非 bool）
            if (Physics.SetVehicleInput(slot, 1.0f, 0.0f, 0.0f, 0.0f)
                != PhysicsCode.Ok) return 3;
            // 重复写合法（同槽覆盖语义）
            if (Physics.SetVehicleInput(slot, 0.5f, 0.25f, 0.0f, 1.0f)
                != PhysicsCode.Ok) return 4;

            // 完整位姿读回：pos 量级合理（训练场内）+ quat 近单位 + vel 未炸
            rc = Physics.FullPose(slot, out VehiclePose pose);
            if (rc != PhysicsCode.Ok) return 5;
            if (pose.Pos == null || pose.Quat == null || pose.Vel == null) return 6;
            if (Math.Abs(pose.Pos[0] - 5.0f) > 25.0f) return 7;
            if (Math.Abs(pose.Pos[1] - 1.0f) > 25.0f) return 8;
            float qLen = 0f;
            for (int i = 0; i < 4; ++i) qLen += pose.Quat[i] * pose.Quat[i];
            if (Math.Abs(qLen - 1.0f) > 1e-3f) return 9;

            // 任一字段可不要（out NULL 跳过路径）：只要 quat
            rc = Physics.FullPose(slot, out VehiclePose quatOnly,
                                  wantPos: false, wantVel: false);
            if (rc != PhysicsCode.Ok) return 10;
            if (quatOnly.Pos != null || quatOnly.Vel != null || quatOnly.Quat == null)
                return 11;

            // 幽灵摆位：teleport 后 FullPose pos 逐位读回（DontActivate 直摆）
            var tp = new float[] { 12.5f, 2.0f, -7.25f };
            var tq = new float[] { 0.0f, 0.0f, 0.0f, 1.0f };
            if (Physics.TeleportVehicle(slot, tp, tq) != PhysicsCode.Ok) return 12;
            rc = Physics.FullPose(slot, out VehiclePose after);
            if (rc != PhysicsCode.Ok) return 13;
            if (Math.Abs(after.Pos[0] - 12.5f) > 1e-4f ||
                Math.Abs(after.Pos[1] - 2.0f) > 1e-4f ||
                Math.Abs(after.Pos[2] - (-7.25f)) > 1e-4f) return 14;

            // tombstone 移除：rc=0；移除后 FullPose/输入/teleport 全 NoSlot
            if (Physics.RemoveVehicle(slot) != PhysicsCode.Ok) return 15;
            if (Physics.FullPose(slot, out _) != PhysicsCode.NoSlot) return 16;
            if (Physics.SetVehicleInput(slot, 0f, 0f, 0f, 0f) != PhysicsCode.NoSlot)
                return 17;
            if (Physics.TeleportVehicle(slot, tp, tq) != PhysicsCode.NoSlot) return 18;
            // 非法槽（越界）同 NoSlot
            if (Physics.SetVehicleInput(99, 0f, 0f, 0f, 0f) != PhysicsCode.NoSlot)
                return 19;

            // slot0（既有单车）不受 tombstone 影响
            if (Physics.FullPose(0, out _) != PhysicsCode.Ok) return 20;

            return 0;
        }

        // s12b：未绑路径（宿主已 driveSetScene(nullptr)）：五导出全 NoScene。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Physics.AddVehicle(0f, 0f, out _) != PhysicsCode.NoScene) return 1;
            if (Physics.RemoveVehicle(0) != PhysicsCode.NoScene) return 2;
            if (Physics.SetVehicleInput(0, 0f, 0f, 0f, 0f) != PhysicsCode.NoScene)
                return 3;
            if (Physics.FullPose(0, out _) != PhysicsCode.NoScene) return 4;
            var tp = new float[] { 0f, 0f, 0f };
            var tq = new float[] { 0f, 0f, 0f, 1f };
            if (Physics.TeleportVehicle(0, tp, tq) != PhysicsCode.NoScene) return 5;

            return 0;
        }
    }
}
