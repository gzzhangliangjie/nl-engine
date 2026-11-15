// M-W2A 验收脚本：Physics 查询域绑定面（selftest s17 节逐个调用）。
//
// 入口约定与 mouse 示例同口径：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 宿主注入（main.cpp s17）：M-PQ0 QueryScene 确定性夹具（floor/boxA/boxB/boxC/
// sensor 5 静态体，build() 自建）→ querySetScene 注入。锚值（core/src/physics/
// Query.cpp 单源）：floor 层 0 owner 0x464c4f4f52000000（"FLOOR"）/ boxA 层 1
// owner 0x424f584100000001（"BOXA"）@(0,1,5)。**这是夹具查询，不是 viewer 现役
// 物理世界**（live 化属 M-PQ1）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class QueryScript
    {
        const ulong OwnerFloor = 0x464c4f4f52000000ul;  // "FLOOR"
        const ulong OwnerBoxA  = 0x424f584100000001ul;  // "BOXA"
        const uint  LayerFloor = 0;                     // layers::kFloor
        const uint  LayerDefault = 1;                   // layers::kDefault

        // s17a：绑定路径全链断言。返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 夹具刚体数 = 5（floor/boxA/boxB/boxC/sensor）
            if (Physics.BodyCount(out int bodies) != QueryCode.Ok || bodies != 5) return 1;

            // 射线：竖直下穿 (0,5,0)->(0,-5,0) 命中 floor 顶面（y=0），
            // distance=5 / fraction=5/100=0.05 / normal=(0,1,0) / layer=FLOOR / owner=FLOOR
            if (Physics.Raycast(new float[] { 0f, 5f, 0f }, new float[] { 0f, -1f, 0f }, 100f,
                                0xFu, out PhysicsQueryHit h1) != QueryCode.Ok) return 2;
            if (!h1.Hit) return 3;
            if (System.MathF.Abs(h1.Distance - 5f) > 1e-4f) return 4;
            if (System.MathF.Abs(h1.Fraction - 0.05f) > 1e-5f) return 5;
            if (System.MathF.Abs(h1.Point[1]) > 1e-4f) return 6;   // floor 顶面 y=0（容差内）
            if (System.MathF.Abs(h1.Normal[1] - 1f) > 1e-4f) return 7;
            if (h1.Layer != LayerFloor || h1.Owner != OwnerFloor) return 8;

            // 层屏蔽：排除 floor（bit0=0）后同列无命中
            if (Physics.Raycast(new float[] { 0f, 5f, 0f }, new float[] { 0f, -1f, 0f }, 100f,
                                0xEu, out PhysicsQueryHit h2) != QueryCode.Ok) return 9;
            if (h2.Hit) return 10;

            // 扫掠：球 r=0.5 从 (0,10,5) 落向 (0,0,5) 命中 boxA 顶（y=2）
            if (Physics.Sweep(new float[] { 0.5f, 0f, 0f }, 0u,
                              new float[] { 0f, 10f, 5f }, new float[] { 0f, 0f, 5f },
                              new float[] { 0f, 0f, 0f, 1f }, 0xFu,
                              out PhysicsQueryHit s1) != QueryCode.Ok) return 11;
            if (!s1.Hit || s1.Owner != OwnerBoxA || s1.Layer != LayerDefault) return 12;

            // 重叠：与 boxA 完全同位（半边长 1,1,1 @ (0,1,5)）→ 命中含 BOXA
            if (Physics.Overlap(new float[] { 1f, 1f, 1f }, 1u,
                                new float[] { 0f, 1f, 5f }, new float[] { 0f, 0f, 0f, 1f },
                                0xFu, 32u, out int count) != QueryCode.Ok || count <= 0) return 13;
            bool sawBoxA = false;
            for (int i = 0; i < count; ++i)
            {
                if (Physics.OverlapHitAt(i, out PhysicsQueryHit oh) != QueryCode.Ok) return 14;
                if (oh.Owner == OwnerBoxA) sawBoxA = true;
            }
            if (!sawBoxA) return 15;

            // 负向：零向量方向 / shapeKind 越界 / maxHits=0
            if (Physics.Raycast(new float[] { 0f, 5f, 0f }, new float[] { 0f, 0f, 0f }, 100f,
                                0xFu, out _) != QueryCode.BadArg) return 16;
            if (Physics.Sweep(new float[] { 0.5f, 0f, 0f }, 9u,
                              new float[] { 0f, 10f, 5f }, new float[] { 0f, 0f, 5f },
                              new float[] { 0f, 0f, 0f, 1f }, 0xFu, out _)
                    != QueryCode.BadArg) return 17;
            if (Physics.Overlap(new float[] { 1f, 1f, 1f }, 1u,
                                new float[] { 0f, 1f, 5f }, new float[] { 0f, 0f, 0f, 1f },
                                0xFu, 0u, out _) != QueryCode.BadArg) return 18;

            // 万次级串往返（确定性 + 无漂移：每次同列同结果）
            for (int i = 0; i < 10000; ++i)
            {
                if (Physics.Raycast(new float[] { 0f, 5f, 0f }, new float[] { 0f, -1f, 0f }, 100f,
                                    0xFu, out PhysicsQueryHit hr) != QueryCode.Ok || !hr.Hit) return 19;
            }

            return 0;
        }

        // s17b：解绑路径（宿主先 querySetScene(nullptr)）。
        // 判据：全族回 NoScene（不隐式建表口径）。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Physics.BodyCount(out _) != QueryCode.NoScene) return 1;
            if (Physics.Raycast(new float[] { 0f, 5f, 0f }, new float[] { 0f, -1f, 0f }, 100f,
                                0xFu, out _) != QueryCode.NoScene) return 2;
            if (Physics.Sweep(new float[] { 0.5f, 0f, 0f }, 0u,
                              new float[] { 0f, 10f, 5f }, new float[] { 0f, 0f, 5f },
                              new float[] { 0f, 0f, 0f, 1f }, 0xFu, out _)
                    != QueryCode.NoScene) return 3;
            if (Physics.Overlap(new float[] { 1f, 1f, 1f }, 1u,
                                new float[] { 0f, 1f, 5f }, new float[] { 0f, 0f, 0f, 1f },
                                0xFu, 8u, out _) != QueryCode.NoScene) return 4;
            if (Physics.OverlapHitAt(0, out _) != QueryCode.NoScene) return 5;

            return 0;
        }
    }
}
