// M-SC2 验收脚本：ECS 数据面绑定（多入口静态方法，selftest s7 组逐个调用）。
//
// 入口约定（与 hello/console 同口径）：[UnmanagedCallersOnly] 默认签名
// int(IntPtr args, int size)，args = ApiTable*（--api 路线）。
// 失效安全判据本体：对失效实体调用得到 EcsCode.NoEntity，不抛异常不崩。
using System;
using System.Runtime.InteropServices;
using Nl.Script.Api;

namespace NlScript.Examples
{
    public static class EcsScript
    {
        // s7a：C# 建实体 → 挂 Transform → 改字段（C++ 侧读回验证）。
        // 实体树：scriptRoot（pos 1,2,3 / quat 0.5^4 / scale 2,2,2）
        //         └ scriptLeaf（pos -1,-2,-3 / identity quat / scale 1,1,1）
        // 名字固定，C++ 侧经绑定表 findByName 取回验证。
        [UnmanagedCallersOnly(EntryPoint = "SpawnSetup")]
        public static int SpawnSetup(IntPtr args, int size)
        {
            Engine.Init(args);
            var root = new SceneEntity(SpawnNamed("scriptRoot"));
            var leaf = new SceneEntity(SpawnNamed("scriptLeaf"));
            if (root.Id == 0 || leaf.Id == 0) return 1;

            int rc = leaf.SetParent(root.Id);
            if (rc != EcsCode.Ok) return rc;

            rc = root.AddTransform();
            if (rc != EcsCode.Ok) return rc;
            rc = leaf.AddTransform();
            if (rc != EcsCode.Ok) return rc;

            rc = root.SetTransform(new TransformData
            {
                Px = 1f, Py = 2f, Pz = 3f,
                Qx = 0.5f, Qy = 0.5f, Qz = 0.5f, Qw = 0.5f,
                Sx = 2f, Sy = 2f, Sz = 2f,
            });
            if (rc != EcsCode.Ok) return rc;

            rc = leaf.SetTransform(new TransformData
            {
                Px = -1f, Py = -2f, Pz = -3f,
            });
            return rc;
        }

        // s7b：C++ 侧改字段 → C# 读回一致（C++ 事先对 "cppEnt" 写入锚值）。
        // 返回 0 = 读到的值与锚完全一致（float 逐位比较）。
        [UnmanagedCallersOnly(EntryPoint = "ReadVerify")]
        public static int ReadVerify(IntPtr args, int size)
        {
            Engine.Init(args);
            var e = new SceneEntity(FindByName("cppEnt"));
            if (e.Id == 0) return 1;
            int rc = e.GetTransform(out TransformData t);
            if (rc != EcsCode.Ok) return rc;
            if (t.Px != 10f || t.Py != -20f || t.Pz != 30f) return 2;
            if (t.Qx != 0f || t.Qy != 0f || t.Qz != 0.7071067811865476f || t.Qw != 0.7071067811865476f) return 3;
            if (t.Sx != 0.5f || t.Sy != 1.5f || t.Sz != 3f) return 4;
            return 0;
        }

        // s7c：失效安全拒绝——未存在 ID / 未挂组件 / 未绑前提示码，全部拿到预期
        // 错误码且不崩。返回 0 = 全部拒绝到位。
        [UnmanagedCallersOnly(EntryPoint = "InvalidProbe")]
        public static int InvalidProbe(IntPtr args, int size)
        {
            Engine.Init(args);
            var ghost = new SceneEntity(999999);
            if (ghost.Exists) return 1;
            if (ghost.Remove() != EcsCode.NoEntity) return 2;
            if (ghost.SetParent(0) != EcsCode.NoEntity) return 3;
            if (ghost.GetParent(out _) != EcsCode.NoEntity) return 4;
            if (ghost.AddTransform() != EcsCode.NoEntity) return 5;
            if (ghost.GetTransform(out _) != EcsCode.NoEntity) return 6;
            if (ghost.SetTransform(TransformData.Identity()) != EcsCode.NoEntity) return 7;
            return 0;
        }

        // s7d：删除实体（延迟语义）：C++ 侧 flush 后验证 exists 假 + TransformGet 拒绝。
        [UnmanagedCallersOnly(EntryPoint = "RemoveSpawned")]
        public static int RemoveSpawned(IntPtr args, int size)
        {
            Engine.Init(args);
            var e = new SceneEntity(FindByName("scriptLeaf"));
            if (e.Id == 0) return 1;
            return e.Remove();
        }

        // run --api demo 入口：spawn → 挂组件 → 写字段 → 读回打印 → flush → 再验。
        // M-SC3 拆两层口径：Entry = 普通静态方法（collectible ALC 反射路径专用——
        // 运行时禁止托管代码调用 [UnmanagedCallersOnly] 方法）；EntryNative =
        // [UnmanagedCallersOnly] 包装（默认 ALC getEntryPoint 函数指针路径）。
        public static int Entry(IntPtr args, int size)
        {
            Engine.Init(args);
            Console.WriteLine("script-api v" + Api.Version + " ecs binding demo");

            int rc = Ecs.Spawn("demoRoot", out ulong root);
            Console.WriteLine("spawn demoRoot -> " + rc + " id=" + root);
            rc = Ecs.Spawn("demoLeaf", out ulong leaf);
            Console.WriteLine("spawn demoLeaf -> " + rc + " id=" + leaf);

            rc = Ecs.SetParent(leaf, root);
            Console.WriteLine("setParent(leaf->root) -> " + rc);
            rc = Ecs.GetParent(leaf, out ulong p);
            Console.WriteLine("getParent(leaf) -> " + rc + " parent=" + p);

            rc = Ecs.AddTransform(root);
            Console.WriteLine("addTransform(root) -> " + rc);
            rc = Ecs.TransformSet(root, TransformData.Identity() with { Sx = 2f });
            Console.WriteLine("transformSet(root) -> " + rc);
            rc = Ecs.TransformGet(root, out TransformData t);
            Console.WriteLine("transformGet(root) -> " + rc +
                              " scale=" + t.Sx + "," + t.Sy + "," + t.Sz);

            rc = Ecs.Flush();
            Console.WriteLine("flush -> " + rc);
            Console.WriteLine("exists demoRoot=" + new SceneEntity(root).Exists +
                              " demoLeaf=" + new SceneEntity(leaf).Exists);

            rc = Ecs.Remove(root);   // 父删递归删子（Carrot 口径）
            Console.WriteLine("remove demoRoot -> " + rc);
            Ecs.Flush();
            Console.WriteLine("after flush: exists demoRoot=" + new SceneEntity(root).Exists +
                              " demoLeaf=" + new SceneEntity(leaf).Exists);
            return 0;
        }

        static ulong SpawnNamed(string name)
        {
            int rc = Ecs.Spawn(name, out ulong id);
            return rc == EcsCode.Ok ? id : 0;
        }

        // 按名取 ID（ecsFindByName 绑定，offset 15）。
        static ulong FindByName(string name)
        {
            int rc = Ecs.FindByName(name, out ulong id);
            return rc == EcsCode.Ok ? id : 0;
        }

        /// <summary>native 包装：默认 ALC getEntryPoint 函数指针路径用。</summary>
        [UnmanagedCallersOnly(EntryPoint = "Entry")]
        public static int EntryNative(IntPtr args, int size) => Entry(args, size);
    }
}
