// M-SC7 验收脚本：ECS 模型挂载面（多入口静态方法，selftest s11 组逐个调用）。
//
// 入口约定（与 camera 示例同口径）：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 操作目标 = 宿主侧 ecsSetWorld 绑定的罐头 World（selftest 进程生命周期）。
// 挂载语义（拍板方案 A）：binding 只做组件增删 + 白名单校验；无 Transform 拒绝
//（NoTransform=5，不隐式建表）；重复挂拒绝（Component=3——World::addComponent
// 同类型重复是 assert，binding 先挡）；白名单拒绝 = BadAsset=6（防路径穿越名）。
// 失效安全判据本体：未绑 World 时各导出回 NoWorld，不抛异常不崩
//（UnboundProbe 前宿主已 ecsSetWorld(nullptr)，调用后宿主重绑）。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class ModelScript
    {
        // s11a：挂载 + 读回 + 留态路径（宿主在入口返回后用 C++ World API 终态核：
        // probeEnt 上 ModelComponent 存在且 meshFile == "cube.glb"）。
        // 返回 0 = 全部锚一致；非 0 = 第一个失败的断言编号。
        [UnmanagedCallersOnly(EntryPoint = "BoundProbe")]
        public static int BoundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 建实体 + Transform（挂载前置，不隐式建表）
            if (Ecs.Spawn("probeModel", out ulong id) != EcsCode.Ok) return 1;
            if (id == 0) return 2;
            if (Ecs.AddTransform(id) != EcsCode.Ok) return 3;

            // 挂模型：合法名
            if (Ecs.AttachModel(id, "cube.glb") != EcsCode.Ok) return 4;

            // ModelInfo 全字段读回：meshFile / tint 缺省 (1,1,1) / roughness 1 / metallic 0
            int rc = Ecs.ModelInfo(id, out string meshFile, out System.Numerics.Vector3 tint,
                                   out float roughness, out float metallic);
            if (rc != EcsCode.Ok) return 5;
            if (meshFile != "cube.glb") return 6;
            if (Math.Abs(tint.X - 1f) > 1e-5f || Math.Abs(tint.Y - 1f) > 1e-5f ||
                Math.Abs(tint.Z - 1f) > 1e-5f) return 7;
            if (Math.Abs(roughness - 1f) > 1e-5f) return 8;
            if (Math.Abs(metallic - 0f) > 1e-5f) return 9;

            // 留态返回（宿主 C++ 侧核 ModelComponent 存在），供 ErrorProbe 重复挂判据复用
            return 0;
        }

        // s11b：错误码逐一对齐（对已绑 World）：
        //   NoEntity=2 / BadArg=4 / BadAsset=6 / NoTransform=5 / Component=3(重复挂+无模型摘)
        [UnmanagedCallersOnly(EntryPoint = "ErrorProbe")]
        public static int ErrorProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            // 不存在的实体（attach / detach / info 三导出同码）
            if (Ecs.AttachModel(0xDEADBEEFUL, "cube.glb") != EcsCode.NoEntity) return 1;
            if (Ecs.DetachModel(0xDEADBEEFUL) != EcsCode.NoEntity) return 2;
            if (Ecs.ModelInfo(0xDEADBEEFUL, out _) != EcsCode.NoEntity) return 3;

            // 空名 = BadArg
            if (Ecs.Spawn("errEnt", out ulong id) != EcsCode.Ok) return 4;
            if (Ecs.AttachModel(id, "") != EcsCode.BadArg) return 5;

            // 白名单拒绝（路径穿越名 / 非法后缀 / 目录分隔符）= BadAsset，坏名不落 World
            if (Ecs.AddTransform(id) != EcsCode.Ok) return 6;
            if (Ecs.AttachModel(id, "../secret.glb") != EcsCode.BadAsset) return 7;
            if (Ecs.AttachModel(id, "model.gltf") != EcsCode.BadAsset) return 8;
            if (Ecs.AttachModel(id, "dir/model.glb") != EcsCode.BadAsset) return 9;
            if (Ecs.ModelInfo(id, out string mf) != EcsCode.Component) return 10;   // 坏名没落 World

            // 无 Transform 挂载 = NoTransform（不隐式建表）
            if (Ecs.Spawn("bareEnt", out ulong bare) != EcsCode.Ok) return 11;
            if (Ecs.AttachModel(bare, "cube.glb") != EcsCode.NoTransform) return 12;
            if (Ecs.ModelInfo(bare, out _) != EcsCode.Component) return 13;

            // 重复挂 = Component（probeEnt 在 BoundProbe 已挂且留态）
            if (Ecs.FindByName("probeModel", out ulong probe) != EcsCode.Ok || probe == 0) return 14;
            if (Ecs.AttachModel(probe, "sphere.glb") != EcsCode.Component) return 15;
            // 拒绝不改状态：meshFile 仍是首挂的 cube.glb
            if (Ecs.ModelInfo(probe, out string mf2) != EcsCode.Ok || mf2 != "cube.glb") return 16;

            // 无模型可摘 = Component
            if (Ecs.DetachModel(bare) != EcsCode.Component) return 17;

            return 0;
        }

        // s11c：摘除 + 未绑路径（宿主先摘 probeModel？不——宿主在调用本入口前已
        // ecsSetWorld(nullptr)；摘除路径由宿主 C++ 侧直接核 World 终态）。
        // 判据：未绑 World 时 attach/detach/info 回 NoWorld，不抛异常不崩。
        [UnmanagedCallersOnly(EntryPoint = "UnboundProbe")]
        public static int UnboundProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Ecs.AttachModel(42, "cube.glb") != EcsCode.NoWorld) return 1;
            if (Ecs.DetachModel(42) != EcsCode.NoWorld) return 2;
            if (Ecs.ModelInfo(42, out _) != EcsCode.NoWorld) return 3;

            return 0;
        }

        // s11d：摘除路径（宿主已重绑 World）：DetachModel 后 ModelInfo 回 Component。
        [UnmanagedCallersOnly(EntryPoint = "DetachProbe")]
        public static int DetachProbe(IntPtr args, int size)
        {
            Engine.Init(args);

            if (Ecs.FindByName("probeModel", out ulong probe) != EcsCode.Ok || probe == 0) return 1;
            if (Ecs.DetachModel(probe) != EcsCode.Ok) return 2;
            if (Ecs.ModelInfo(probe, out _) != EcsCode.Component) return 3;
            // 再摘一次 = Component（对称）
            if (Ecs.DetachModel(probe) != EcsCode.Component) return 4;

            return 0;
        }
    }
}
