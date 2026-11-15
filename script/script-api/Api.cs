// M-SC0 骨架：脚本 API 绑定面的根命名空间 + 版本锚。
// M-SC1 起 console 绑定（cvar.get/set/list、console.exec、Console.Register）落本程序集；
// M-SC2 起 ECS 数据面绑定（World.Spawn/Remove/SetParent、TransformComponent 字段读写）落本程序集。
// M-SC4 起：接线层（delegate/指针字段/Init 偏移/NativeMarshal/TransformData POD 本体）
//   = 生成物 Wiring.g.cs（单源 script/script-api/exports.api）；本文件只剩
//   facade 语义本体（返回语义/错误码映射/回调适配）与 hand 扩展。勿在本文件
//   手写任何 marshal 原语——一律走 NativeMarshal（生命周期账本单源）。
// 热更安全约定（P8-SCRIPT-PLAN.md）：绑定面 API 无跨帧状态；状态全在 C++ 侧。
using System;
using System.Collections.Generic;
using System.Numerics;               // M-SC7：ModelInfo tint（Vector3，net8.0 基类库）
using System.Runtime.InteropServices;

namespace Nl.Script.Api
{
    /// <summary>脚本 API 绑定面版本锚（selftest / echo 用例用它证明 script-api 已装载）。</summary>
    public static class Api
    {
        public const string Version = "0.9.0";   // M-SC10：属性面板域（v7 2 导出，ApiTable 39）
    }

    /// <summary>console.exec 结果（ExecCode 分档与 C++ core/console 一致）。</summary>
    public readonly struct ConsoleExecResult
    {
        public int Code { get; }     // 0=Ok 1=UnknownCommand 2=BadRequest 3=CommandFailed
        public string Output { get; }

        public ConsoleExecResult(int code, string output) { Code = code; Output = output; }
        public bool Ok => Code == 0;
        public override string ToString() => $"[{Code}] {Output}";
    }

    /// <summary>
    /// C# 命令回调（经 ConsoleRegister 的友好包装）。
    /// 返回字符串 = 成功且作为命令输出；返回 null = 失败（ExecCode::CommandFailed）。
    /// 包装自身 catch 所有托管异常转失败——脚本作者无须关心互操作边界。
    /// </summary>
    public delegate string? ScriptCommandHandler(string[] args);

    /// <summary>托管命令回调原始 ABI（与 C++ ManagedCommandFn 对齐；consoleRegister 声明面）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int NativeCommandDelegate(int argc, IntPtr argv, out IntPtr text);

    /// <summary>
    /// console 绑定面（M-SC1，M-SC4 接线改生成物）。C++ 侧函数指针表随托管入口 args 传入
    /// （nl/script/Bindings.hpp ABI），Init 必须在使用前先调（selftest / 工具入口均已接）。
    /// 内存所有权（见 Bindings.hpp 文件头 + Wiring.g.cs NativeMarshal）：C++ 返回串
    /// 「下次调用前有效」→ 经 NativeMarshal.Utf8Read 立即拷贝成托管串；C# 命令 outText 用
    /// AllocHGlobal（CoTaskMem）→ C++ CoTaskMemFree。
    /// </summary>
    public static unsafe class Engine
    {
        /// <summary>从托管入口 args 读 ApiTable（指针；接线偏移全在生成物 ApiWiring）。</summary>
        public static void Init(IntPtr table)
        {
            if (table == IntPtr.Zero) throw new InvalidOperationException("Engine.Init: null api table (entry must receive ApiTable*)");
            ApiWiring.InitAll(table);
        }

        // ---- CVar ----

        /// <summary>读 cvar 现值（toString 口径）。未注册返回 null。typeCode: 1=bool 2=int 3=float 4=string。</summary>
        public static string? CvarGet(string name, out int typeCode)
        {
            typeCode = 0;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.CvarGetFn>(EngineWiring._cvarGet);
                int rc = fn(namePtr, out IntPtr valuePtr);
                if (rc == 0) return null;              // 未注册
                typeCode = rc;
                return NativeMarshal.Utf8ReadOrEmpty(valuePtr);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        public static string? CvarGet(string name) => CvarGet(name, out _);

        /// <summary>文本赋值（setFromString 语义，onChanged 触发）。0=ok 1=未知 cvar 2=类型/格式不符。</summary>
        public static int CvarSet(string name, string value)
        {
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name), valuePtr = NativeMarshal.Utf8Alloc(value);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.CvarSetFn>(EngineWiring._cvarSet);
                return fn(namePtr, valuePtr);
            }
            finally { NativeMarshal.Utf8Free(namePtr); NativeMarshal.Utf8Free(valuePtr); }
        }

        /// <summary>全量 CVar 视图（"name = value (type) help" 逐行）。</summary>
        public static string CvarList()
        {
            var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.CvarListFn>(EngineWiring._cvarList);
            return NativeMarshal.Utf8ReadOrEmpty(fn());
        }

        // ---- console ----

        /// <summary>经绑定面执行控制台行（ExecCode 0..3 原样返回）。</summary>
        public static ConsoleExecResult ConsoleExec(string line)
        {
            IntPtr linePtr = NativeMarshal.Utf8Alloc(line);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.ConsoleExecFn>(EngineWiring._consoleExec);
                int code = fn(linePtr, out IntPtr outputPtr);
                return new ConsoleExecResult(code, NativeMarshal.Utf8ReadOrEmpty(outputPtr));
            }
            finally { NativeMarshal.Utf8Free(linePtr); }
        }

        /// <summary>
        /// 注册 C# 命令进 console 命令表。返回 0=ok / 1=名字被 CVar 占用。
        /// 异常安全：handler 抛出的托管异常在包装内 catch 转 null（失败），不穿互操作边界。
        /// delegate 保活：adapter 落在 script-api（与脚本同 collectible ALC），Unload 随
        /// ALC 整体回收——卸载前 C++ 侧 scriptUnregisterAll 次序铁律不变（M-SC3）。
        /// </summary>
        public static int ConsoleRegister(string name, string help, ScriptCommandHandler handler)
        {
            NativeCommandDelegate adapter = (int argc, IntPtr argv, out IntPtr text) =>
            {
                text = IntPtr.Zero;
                string? result;
                try
                {
                    var args = new string[argc];
                    for (int i = 0; i < argc; i++)
                        args[i] = Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(argv, i * sizeof(IntPtr))) ?? "";
                    result = handler(args);
                }
                catch (Exception) { return 1; }         // 托管异常 → 失败（不穿边界）
                if (result == null) return 1;
                text = NativeMarshal.Utf8Alloc(result);
                return 0;
            };
            return ConsoleRegisterRaw(name, help, adapter);
        }

        /// <summary>原始 ABI 注册（异常不经包装捕获 → C++ SEH 边界路径，selftest s6d 用）。</summary>
        public static int ConsoleRegisterRaw(string name, string help, NativeCommandDelegate raw)
        {
            _pinned.Add(raw);
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name), helpPtr = NativeMarshal.Utf8Alloc(help);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.ConsoleRegisterFn>(EngineWiring._consoleRegister);
                return fn(namePtr, helpPtr, Marshal.GetFunctionPointerForDelegate(raw));
            }
            finally { NativeMarshal.Utf8Free(namePtr); NativeMarshal.Utf8Free(helpPtr); }
        }

        /// <summary>注销命令（0=ok / 1=未知）。M-SC3 热重载解绑的落点。</summary>
        public static int ConsoleUnregister(string name)
        {
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EngineWiring.ConsoleUnregisterFn>(EngineWiring._consoleUnregister);
                return fn(namePtr);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        // ---- 命令回调保活 ----
        // 与脚本同 collectible ALC（Bootstrap 依赖解析口径）：Unload 时本列表连同
        // adapter thunk 一并回收，零残留——「批次归组」由 ALC 边界结构性达成，
        // 无须手工清点（UnrealSharp per-ALC strong refs 同构，2026-11-14 实读对齐）。
        static readonly List<NativeCommandDelegate> _pinned = new();
    }

    /// <summary>ECS 导出错误码（与 nl/script/Bindings.hpp kEcs* 一致）。</summary>
    public static class EcsCode
    {
        public const int Ok = 0;
        public const int NoWorld = 1;      // 宿主未绑 World
        public const int NoEntity = 2;     // 实体不存在（失效安全拒绝）
        public const int Component = 3;    // 组件缺失或重复挂
        public const int BadArg = 4;       // 非法参数
        public const int NoTransform = 5;  // M-SC7：挂模型但实体无 Transform（先 AddTransform，不隐式建表）
        public const int BadAsset = 6;     // M-SC7：meshFile 白名单拒绝（防路径穿越名）
    }

    /// <summary>局部 TRS（POD 本体 = 生成物 Wiring.g.cs；本 partial 只落 hand 扩展）。
    /// quat x,y,z,w 口径与 C++ Transform 一致。</summary>
    public partial struct TransformData
    {
        public static TransformData Identity() => new()
        { Qw = 1f, Sx = 1f, Sy = 1f, Sz = 1f };
    }

    /// <summary>
    /// ECS 数据面绑定（M-SC2，M-SC4 接线改生成物）。操作目标 = 宿主侧 ecsSetWorld 绑定的
    /// World（nl/script/Bindings.hpp v2 导出，ApiTable 偏移 6..15）。延迟语义沿 World 口径：
    /// Spawn 新建立即可见；Remove 排队、Flush/tick 生效（Carrot 口径）。
    /// 失效安全拒绝：所有方法回错误码（EcsCode），不抛异常不崩。
    /// </summary>
    public static unsafe class Ecs
    {
        /// <summary>建实体（新建立即可见，Carrot 口径）。0=ok，*id 为实体 ID。</summary>
        public static int Spawn(string name, out ulong id)
        {
            id = 0;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsSpawnFn>(EcsWiring._ecsSpawn);
                return fn(namePtr, out id);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        /// <summary>延迟删除（flush/tick 生效；父删递归删子）。</summary>
        public static int Remove(ulong id) =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsRemoveFn>(EcsWiring._ecsRemove)(id);

        /// <summary>统一落盘（宿主 tick 边界动作）。</summary>
        public static int Flush() =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsFlushFn>(EcsWiring._ecsFlush)();

        /// <summary>存在性查询（0=ok / 1=未绑 World）。exists: 新建即时真、删除 flush 后假。</summary>
        public static int Exists(ulong id, out bool exists)
        {
            exists = false;
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsExistsFn>(EcsWiring._ecsExists);
            int rc = fn(id, out int e);
            if (rc == EcsCode.Ok) exists = e != 0;
            return rc;
        }

        /// <summary>挂父（纯关系，不动变换）。parent=0 解除父子。</summary>
        public static int SetParent(ulong id, ulong parent) =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsSetParentFn>(EcsWiring._ecsSetParent)(id, parent);

        /// <summary>取父 ID（0=无父）。</summary>
        public static int GetParent(ulong id, out ulong parent)
        {
            parent = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsGetParentFn>(EcsWiring._ecsGetParent);
            return fn(id, out parent);
        }

        /// <summary>挂 TransformComponent（重复挂 = EcsCode.Component）。</summary>
        public static int AddTransform(ulong id) =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsAddTransformFn>(EcsWiring._ecsAddTransform)(id);

        /// <summary>局部 TRS 读回（缺组件 = EcsCode.Component）。</summary>
        public static int TransformGet(ulong id, out TransformData t)
        {
            t = default;
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsTransformGetFn>(EcsWiring._ecsTransformGet);
            float* pos = stackalloc float[3];
            float* rot = stackalloc float[4];
            float* scl = stackalloc float[3];
            int rc = fn(id, pos, rot, scl);
            if (rc == EcsCode.Ok)
            {
                t = new TransformData
                {
                    Px = pos[0], Py = pos[1], Pz = pos[2],
                    Qx = rot[0], Qy = rot[1], Qz = rot[2], Qw = rot[3],
                    Sx = scl[0], Sy = scl[1], Sz = scl[2],
                };
            }
            return rc;
        }

        /// <summary>局部 TRS 写入（缺组件 = EcsCode.Component）。</summary>
        public static int TransformSet(ulong id, in TransformData t)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsTransformSetFn>(EcsWiring._ecsTransformSet);
            float* pos = stackalloc float[3] { t.Px, t.Py, t.Pz };
            float* rot = stackalloc float[4] { t.Qx, t.Qy, t.Qz, t.Qw };
            float* scl = stackalloc float[3] { t.Sx, t.Sy, t.Sz };
            return fn(id, pos, rot, scl);
        }

        /// <summary>按名查实体（未找到 id=0，返回 EcsCode.Ok）。</summary>
        public static int FindByName(string name, out ulong id)
        {
            id = 0;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsFindByNameFn>(EcsWiring._ecsFindByName);
                return fn(namePtr, out id);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        /// <summary>挂 ModelComponent（M-SC7；meshFile = assets/ 下 .glb 文件名，白名单
        /// 拒绝路径穿越名）。viewer 帧边界比对 Model 集合签名重建 scene batch。
        /// rc：Ok / NoWorld / NoEntity / Component(重复挂) / BadArg(空名) /
        /// NoTransform(先 AddTransform) / BadAsset(白名单拒绝)。</summary>
        public static int AttachModel(ulong id, string meshFile)
        {
            IntPtr filePtr = NativeMarshal.Utf8Alloc(meshFile);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsAttachModelFn>(EcsWiring._ecsAttachModel);
                return fn(id, filePtr);
            }
            finally { NativeMarshal.Utf8Free(filePtr); }
        }

        /// <summary>摘 ModelComponent（M-SC7；viewer 帧边界重建）。
        /// rc：Ok / NoWorld / NoEntity / Component(无 Model 可摘)。</summary>
        public static int DetachModel(ulong id) =>
            Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsDetachModelFn>(EcsWiring._ecsDetachModel)(id);

        /// <summary>ModelComponent 字段读回（M-SC7）。meshFile = out str 静态缓冲账本
        /// （「下次同线程调用前有效」，本方法立即拷成托管串）；tint/roughness/metallic
        /// 可传 null 跳过（C# 侧用重载固定全读形态）。rc：Ok / NoWorld / NoEntity /
        /// Component(无 Model) / BadArg。</summary>
        public static int ModelInfo(ulong id, out string meshFile, out Vector3 tint,
                                    out float roughness, out float metallic)
        {
            meshFile = "";
            tint = default;
            roughness = 0f;
            metallic = 0f;
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsModelInfoFn>(EcsWiring._ecsModelInfo);
            unsafe
            {
                float* t = stackalloc float[3];
                int rc = fn(id, out IntPtr filePtr, t, out roughness, out metallic);
                if (rc == EcsCode.Ok)
                {
                    meshFile = NativeMarshal.Utf8ReadOrEmpty(filePtr);
                    tint = new Vector3(t[0], t[1], t[2]);
                }
                return rc;
            }
        }

        /// <summary>ModelInfo 全字段读回形态（tint 可空指针版的便捷封装）。</summary>
        public static int ModelInfo(ulong id, out string meshFile)
        {
            meshFile = "";
            var fn = Marshal.GetDelegateForFunctionPointer<EcsWiring.EcsModelInfoFn>(EcsWiring._ecsModelInfo);
            int rc = fn(id, out IntPtr filePtr, null, out _, out _);
            if (rc == EcsCode.Ok) meshFile = NativeMarshal.Utf8ReadOrEmpty(filePtr);
            return rc;
        }
    }

    /// <summary>
    /// Entity 轻量 wrapper（与 EntityID 映射，Carrot Entity 同构：不持存储）。
    /// 失效安全拒绝：所有实例方法委托 Ecs.* 回错误码——失效实体上调用得到
    /// EcsCode.NoEntity，不崩不改状态（selftest s7 判据本体）。
    /// </summary>
    public readonly struct SceneEntity
    {
        public ulong Id { get; }
        public SceneEntity(ulong id) { Id = id; }

        /// <summary>存在性（新建即时真、删除 flush 后假——Carrot 非对称口径）。</summary>
        public bool Exists
        {
            get { Ecs.Exists(Id, out bool e); return e; }
        }

        public int Remove() => Ecs.Remove(Id);
        public int SetParent(ulong parent) => Ecs.SetParent(Id, parent);
        public int GetParent(out ulong parent) => Ecs.GetParent(Id, out parent);
        public int AddTransform() => Ecs.AddTransform(Id);
        public int GetTransform(out TransformData t) => Ecs.TransformGet(Id, out t);
        public int SetTransform(in TransformData t) => Ecs.TransformSet(Id, t);
    }

    /// <summary>Time 导出错误码（与 nl/script/Bindings.hpp kTime* 一致）。</summary>
    public static class TimeCode
    {
        public const int Ok = 0;
        public const int NoClock = 1;      // 宿主未绑 GameClock
        public const int BadArg = 4;       // 非法参数
    }

    /// <summary>
    /// Time 绑定面（M-SC5）。操作目标 = 宿主侧 timeSetClock 绑定的 GameClock
    /// （viewer --script 场景模式接线）。未绑 = TimeCode.NoClock，不抛异常不崩。
    /// </summary>
    public static unsafe class Time
    {
        /// <summary>时钟状态一次性读回。0=ok；simTime 逻辑秒 / frameDt 渲染帧 delta 秒 / tickCount 逻辑帧数 / paused。</summary>
        public static int GetState(out double simTime, out double frameDt, out ulong tickCount, out bool paused)
        {
            simTime = 0; frameDt = 0; tickCount = 0; paused = false;
            var fn = Marshal.GetDelegateForFunctionPointer<TimeWiring.TimeGetStateFn>(TimeWiring._timeGetState);
            int rc = fn(out simTime, out frameDt, out tickCount, out int p);
            if (rc == TimeCode.Ok) paused = p != 0;
            return rc;
        }

        /// <summary>时钟控制。paused：-1=不变 / 0=继续 / 1=暂停；timeScale&lt;0=不变（NaN 不变）。</summary>
        public static int SetState(int paused, double timeScale) =>
            Marshal.GetDelegateForFunctionPointer<TimeWiring.TimeSetStateFn>(TimeWiring._timeSetState)(paused, timeScale);

        /// <summary>逻辑时间（秒）；未绑返回 0 且 rc=NoClock。</summary>
        public static int SimTime(out double t)
        {
            t = 0;
            var rc = GetState(out t, out _, out _, out _);
            return rc;
        }

        /// <summary>暂停（SetState(1, -1)）。</summary>
        public static int Pause() => SetState(1, -1.0);

        /// <summary>继续（SetState(0, -1)）。</summary>
        public static int Unpause() => SetState(0, -1.0);

        /// <summary>设时间倍率（[0,1024] 钳制在 GameClock 内做；&lt;0 不变）。</summary>
        public static int SetTimeScale(double scale) => SetState(-1, scale);
    }

    /// <summary>Input 导出错误码（与 nl/script/Bindings.hpp kInput* 一致）。</summary>
    public static class InputCode
    {
        public const int Ok = 0;
        public const int NoSource = 1;     // 宿主未绑 ActionMap/InputState（不隐式建表口径）
        public const int NoAction = 2;     // 动作不存在或类型不符
        public const int BadArg = 4;       // 非法参数
    }

    /// <summary>
    /// Input 绑定面（M-SC5）。操作目标 = 宿主侧 inputSetActionMap / inputSetState 绑定的实例。
    /// 动作值 = ActionMap::update 冻结口径（宿主每帧 update，脚本读冻结值）。
    /// 未绑 = InputCode.NoSource，不隐式建表（拍板）。
    /// </summary>
    public static unsafe class Input
    {
        /// <summary>按名查动作（预声明口径）。0=ok；2=不存在（id = InvalidAction 0xFFFFFFFF）。</summary>
        public static int ActionFind(string name, out uint id)
        {
            id = 0xFFFFFFFFu;
            IntPtr namePtr = NativeMarshal.Utf8Alloc(name);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionFindFn>(InputWiring._inputActionFind);
                return fn(namePtr, out id);
            }
            finally { NativeMarshal.Utf8Free(namePtr); }
        }

        /// <summary>Bool 动作冻结值三态读回。0=ok / 2=非法 id 或类型非 Bool。</summary>
        public static int ActionBool(uint id, out bool value, out bool justPressed, out bool justReleased)
        {
            value = false; justPressed = false; justReleased = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionBoolFn>(InputWiring._inputActionBool);
            int rc = fn(id, out int v, out int p, out int r);
            if (rc == InputCode.Ok) { value = v != 0; justPressed = p != 0; justReleased = r != 0; }
            return rc;
        }

        /// <summary>Float 动作冻结值读回。0=ok / 2=非法 id 或类型非 Float。</summary>
        public static int ActionFloat(uint id, out float value, out float delta)
        {
            value = 0; delta = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionFloatFn>(InputWiring._inputActionFloat);
            return fn(id, out value, out delta);
        }

        /// <summary>Vec2 动作冻结值读回（x,y 序）。0=ok / 2=非法 id 或类型非 Vec2。</summary>
        public static int ActionVec2(uint id, out float x, out float y, out float dx, out float dy)
        {
            x = y = dx = dy = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputActionVec2Fn>(InputWiring._inputActionVec2);
            float* v = stackalloc float[2];
            float* d = stackalloc float[2];
            int rc = fn(id, v, d);
            if (rc == InputCode.Ok) { x = v[0]; y = v[1]; dx = d[0]; dy = d[1]; }
            return rc;
        }

        /// <summary>键持续态（GLFW 键码；越界键码 down=false——InputState 防御性口径）。0=ok。</summary>
        public static int KeyIsDown(int key, out bool down)
        {
            down = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputKeyIsDownFn>(InputWiring._inputKeyIsDown);
            int rc = fn(key, out int d);
            if (rc == InputCode.Ok) down = d != 0;
            return rc;
        }

        // ---- M-I3 鼠标裸查询（2026-11-15；按钮码 = GLFW 鼠标键码 0=左/1=右/2=中；越界全 0 防御）----

        /// <summary>鼠标键持续态。0=ok / 1=未绑 InputState / 4=非法参数。</summary>
        public static int MouseIsDown(int button, out bool down)
        {
            down = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputMouseIsDownFn>(InputWiring._inputMouseIsDown);
            int rc = fn(button, out int d);
            if (rc == InputCode.Ok) down = d != 0;
            return rc;
        }

        /// <summary>鼠标键本帧按下边沿（帧末 endFrame 清）。rc 同 MouseIsDown。</summary>
        public static int MouseJustPressed(int button, out bool pressed)
        {
            pressed = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputMouseJustPressedFn>(InputWiring._inputMouseJustPressed);
            int rc = fn(button, out int p);
            if (rc == InputCode.Ok) pressed = p != 0;
            return rc;
        }

        /// <summary>鼠标键本帧释放边沿（帧末 endFrame 清）。rc 同 MouseIsDown。</summary>
        public static int MouseJustReleased(int button, out bool released)
        {
            released = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputMouseJustReleasedFn>(InputWiring._inputMouseJustReleased);
            int rc = fn(button, out int r);
            if (rc == InputCode.Ok) released = r != 0;
            return rc;
        }

        /// <summary>光标帧增量（x,y；帧末 endFrame 清）。0=ok / 1=未绑。</summary>
        public static int MouseDelta(out float dx, out float dy)
        {
            dx = dy = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputMouseDeltaFn>(InputWiring._inputMouseDelta);
            float* d = stackalloc float[2];
            int rc = fn(d);
            if (rc == InputCode.Ok) { dx = d[0]; dy = d[1]; }
            return rc;
        }

        /// <summary>滚轮帧增量（x,y；帧末 endFrame 清）。0=ok / 1=未绑。</summary>
        public static int ScrollDelta(out float dx, out float dy)
        {
            dx = dy = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputScrollDeltaFn>(InputWiring._inputScrollDelta);
            float* d = stackalloc float[2];
            int rc = fn(d);
            if (rc == InputCode.Ok) { dx = d[0]; dy = d[1]; }
            return rc;
        }

        /// <summary>光标绝对位置（窗口像素坐标）+ 在窗口内布尔。0=ok / 1=未绑。</summary>
        public static int CursorPos(out float x, out float y, out bool hasCursor)
        {
            x = y = 0; hasCursor = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputCursorPosFn>(InputWiring._inputCursorPos);
            float* p = stackalloc float[2];
            int rc = fn(p, out int h);
            if (rc == InputCode.Ok) { x = p[0]; y = p[1]; hasCursor = h != 0; }
            return rc;
        }

        // ---- M-I5 手柄 / 字符（2026-11-15；GLFW standard mapping）----

        /// <summary>手柄按钮持续态（按钮号 0..15；越界 down=false 防御）。0=ok / 1=未绑。</summary>
        public static int GamepadIsDown(int button, out bool down)
        {
            down = false;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputGamepadIsDownFn>(InputWiring._inputGamepadIsDown);
            int rc = fn(button, out int d);
            if (rc == InputCode.Ok) down = d != 0;
            return rc;
        }

        /// <summary>手柄轴值（-1..1，扳机 0..1；越界 = 0）。0=ok / 1=未绑。</summary>
        public static int GamepadAxis(int axis, out float value)
        {
            value = 0;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputGamepadAxisFn>(InputWiring._inputGamepadAxis);
            return fn(axis, out value);
        }

        /// <summary>
        /// 文本单码点弹出（window consumeTextChars 队列；-1 = 本帧空）。
        /// 操作目标 = 宿主 inputSetWindow 绑定的 Window。0=ok（codepoint 有效）/ 0 且 -1 = 空帧 / 1=未绑 Window。
        /// </summary>
        public static int TextPop(out int codepoint)
        {
            codepoint = -1;
            var fn = Marshal.GetDelegateForFunctionPointer<InputWiring.InputTextPopFn>(InputWiring._inputTextPop);
            return fn(out codepoint);
        }
    }

    /// <summary>Log 导出错误码（与 nl/script/Bindings.hpp kLog* 一致）。</summary>
    public static class LogCode
    {
        public const int Ok = 0;
        public const int BadFilter = 1;    // 过滤指令非法（Error 带原因文本）
        public const int BadArg = 2;       // level 越界（0..5）或 message 空
    }

    /// <summary>
    /// Log 绑定面（M-SC5）。走 nl::core::log 全局面，通道固定 "script"
    /// （输出行 [nl:&lt;level&gt;|script] message——expectLog 锚可断）。
    /// </summary>
    public static unsafe class Log
    {
        /// <summary>level：0..5 = Trace/Debug/Info/Warn/Error/Fatal。0=ok / 2=level 越界或 message 空。</summary>
        public static int Write(int level, string message)
        {
            IntPtr msgPtr = NativeMarshal.Utf8Alloc(message);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<LogWiring.LogWriteFn>(LogWiring._logWrite);
                return fn(level, msgPtr);
            }
            finally { NativeMarshal.Utf8Free(msgPtr); }
        }

        /// <summary>设置过滤指令（全局面替换："nl:info,renderer=warn"）。
        /// 0=ok / 1=指令非法（error=原因文本）/ 2=非法参数。</summary>
        public static int SetFilter(string instruction, out string? error)
        {
            error = null;
            IntPtr instrPtr = NativeMarshal.Utf8Alloc(instruction);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<LogWiring.LogSetFilterFn>(LogWiring._logSetFilter);
                int rc = fn(instrPtr, out IntPtr errPtr);
                if (rc == LogCode.BadFilter) error = NativeMarshal.Utf8ReadOrEmpty(errPtr);
                return rc;
            }
            finally { NativeMarshal.Utf8Free(instrPtr); }
        }
    }

    /// <summary>Camera 导出错误码（M-SC6）：ModeUnsupported = track 模式轨迹数据主权，脚本不可写位姿。</summary>
    public static class CameraCode
    {
        public const int Ok = 0;
        public const int NoSource = 1;         // 未绑 CameraSource（宿主忘 cameraSetSource）
        public const int BadArg = 2;           // eye 指针 NULL 等
        public const int ModeUnsupported = 3;  // track 模式：位姿主权在轨迹文件
    }

    /// <summary>现役相机位姿快照（GetPose 打包 / SetPose 解包；mode 码 0=orbit/1=free/2=chase/3=track）。</summary>
    public readonly struct CameraPose
    {
        public readonly float[] Eye;   // 世界坐标 [3]
        public readonly float Yaw;     // 度
        public readonly float Pitch;   // 度
        public readonly float Fov;     // fovY 度
        public readonly int Mode;
        public CameraPose(float[] eye, float yaw, float pitch, float fov, int mode)
        { Eye = eye; Yaw = yaw; Pitch = pitch; Fov = fov; Mode = mode; }
    }

    /// <summary>
    /// Camera 绑定面（M-SC6）。操作目标 = 宿主侧 cameraSetSource 绑定的现役相机适配源
    /// （viewer 现役相机 orbit/free/chase/track 模式感知；未绑 = NoSource）。
    /// SetPose 模式感知落点：orbit=yaw/pitch/dist=|eye| / free=setPose / chase=snapEyeTo+pitch
    /// （yaw 由车体驱动忽略） / track=ModeUnsupported。SetFov 注册方 clamp [20,120]。
    /// </summary>
    public static unsafe class Camera
    {
        /// <summary>位姿一次读回。rc：0=ok / 1=NoSource / 4=非法参数。</summary>
        public static int GetPose(out CameraPose pose)
        {
            pose = default;
            var fn = Marshal.GetDelegateForFunctionPointer<CameraWiring.CameraGetPoseFn>(CameraWiring._cameraGetPose);
            float* eye = stackalloc float[3];
            int rc = fn(eye, out float yaw, out float pitch, out float fov, out int mode);
            if (rc == CameraCode.Ok)
                pose = new CameraPose(new[] { eye[0], eye[1], eye[2] }, yaw, pitch, fov, mode);
            return rc;
        }

        /// <summary>位姿写入（模式感知落点见类注释）。rc：0=ok / 1=NoSource / 2=BadArg / 3=ModeUnsupported。</summary>
        public static int SetPose(in CameraPose pose)
        {
            if (pose.Eye == null || pose.Eye.Length != 3) return CameraCode.BadArg;
            var fn = Marshal.GetDelegateForFunctionPointer<CameraWiring.CameraSetPoseFn>(CameraWiring._cameraSetPose);
            fixed (float* eye = pose.Eye)
                return fn(eye, pose.Yaw, pose.Pitch);
        }

        /// <summary>fovY 度读回。未绑 = NaN（f32 返回无 rc 通道——生命周期账本特例，float.IsNaN 判未绑）。</summary>
        public static float GetFov()
        {
            var fn = Marshal.GetDelegateForFunctionPointer<CameraWiring.CameraGetFovFn>(CameraWiring._cameraGetFov);
            return fn();
        }

        /// <summary>fovY 度写入（clamp [20,120] 后模式统一生效）。rc：0=ok / 1=NoSource。</summary>
        public static int SetFov(float fovY)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<CameraWiring.CameraSetFovFn>(CameraWiring._cameraSetFov);
            return fn(fovY);
        }
    }

    public static class PhysicsCode
    {
        public const int Ok = 0;
        public const int NoScene = 1;      // 未绑 DebugScene（宿主忘 driveSetScene / 非 scenario 2）
        public const int NoSlot = 2;       // 槽位不存在或未用槽（tombstone 后同）
        public const int AddFailed = 3;    // addVehicle 返回 -1（DebugScene::lastError 置因）
        public const int BadArg = 4;       // out/in 指针 NULL
    }

    /// <summary>单槽完整位姿快照（FullPose 打包；位置米 + quat x,y,z,w + 线速度 m/s）。</summary>
    public readonly struct VehiclePose
    {
        public readonly float[] Pos;   // [3] 世界坐标（米）
        public readonly float[] Quat;  // [4] x,y,z,w
        public readonly float[] Vel;   // [3] 线速度（m/s）
        public VehiclePose(float[] pos, float[] quat, float[] vel)
        { Pos = pos; Quat = quat; Vel = vel; }
    }

    /// <summary>
    /// Physics 载具绑定面（M-SC8）。操作目标 = 宿主侧 driveSetScene 绑定的
    /// physics::DebugScene 训练场（M-NET2 多载具面直通；未绑 = NoScene）。
    /// 线程口径 = OnTick 内调用（输入在下一次注册方 step 消费；Jolt 手刹是
    /// float 0..1 非 bool）。四输入映射由脚本侧自由组合（配合 Input 域读键）。
    /// </summary>
    public static unsafe class Physics
    {
        /// <summary>训练场加车（slot 从 0 起，slot0 = build(2) 既有单车）。
        /// rc：0=ok / 1=NoScene / 3=AddFailed（slot=-1）。</summary>
        public static int AddVehicle(float spawnX, float spawnZ, out int slot)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.PhysicsAddVehicleFn>(PhysicsWiring._physicsAddVehicle);
            return fn(spawnX, spawnZ, out slot);
        }

        /// <summary>tombstone 移除（其余槽位号不变）。rc：0=ok / 1=NoScene / 2=NoSlot。</summary>
        public static int RemoveVehicle(int slot)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.PhysicsRemoveVehicleFn>(PhysicsWiring._physicsRemoveVehicle);
            return fn(slot);
        }

        /// <summary>四输入写（forward/right ∈ 脚本侧自定，brake/handBrake 0..1）。
        /// rc：0=ok / 1=NoScene / 2=NoSlot。</summary>
        public static int SetVehicleInput(int slot, float forward, float right,
                                          float brake, float handBrake)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.PhysicsSetVehicleInputFn>(PhysicsWiring._physicsSetVehicleInput);
            return fn(slot, forward, right, brake, handBrake);
        }

        /// <summary>完整位姿回读。任一字段可不要（传 false 跳过）。
        /// rc：0=ok / 1=NoScene / 2=NoSlot。</summary>
        public static int FullPose(int slot, out VehiclePose pose,
                                   bool wantPos = true, bool wantQuat = true,
                                   bool wantVel = true)
        {
            pose = default;
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.VehicleFullPoseFn>(PhysicsWiring._vehicleFullPose);
            // stackalloc 不能进三元（推成 Span 无法转裸指针）——先分配再条件选用
            float* pBuf = stackalloc float[3];
            float* qBuf = stackalloc float[4];
            float* vBuf = stackalloc float[3];
            float* pos = wantPos ? pBuf : null;
            float* quat = wantQuat ? qBuf : null;
            float* vel = wantVel ? vBuf : null;
            int rc = fn(slot, pos, quat, vel);
            if (rc == PhysicsCode.Ok)
                pose = new VehiclePose(
                    wantPos ? new[] { pos[0], pos[1], pos[2] } : null,
                    wantQuat ? new[] { quat[0], quat[1], quat[2], quat[3] } : null,
                    wantVel ? new[] { vel[0], vel[1], vel[2] } : null);
            return rc;
        }

        /// <summary>幽灵位摆位（DontActivate；刚体保持到下一次 teleport）。
        /// rc：0=ok / 1=NoScene / 2=NoSlot / 4=BadArg。</summary>
        public static int TeleportVehicle(int slot, float[] pos, float[] quat)
        {
            if (pos == null || pos.Length != 3 || quat == null || quat.Length != 4)
                return PhysicsCode.BadArg;
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.TeleportVehicleFn>(PhysicsWiring._teleportVehicle);
            fixed (float* p = pos, q = quat)
                return fn(slot, p, q);
        }

        // ---- M-W2A 查询域（追加到 Physics 域尾部；QueryScene 确定性夹具，非 live 世界——
        //      live 化属 M-PQ1，见 QueryCode/WAVE2-PLAN §5.2）----

        /// <summary>射线查询（dir 单位长；单命中；info.Hit 0|1）。
        /// rc：0=ok / 1=NoScene / 3=NotReady / 4=BadArg。</summary>
        public static int Raycast(float[] origin, float[] dir, float maxT, uint layerMask,
                                  out PhysicsQueryHit info)
        {
            info = default;
            if (origin == null || origin.Length != 3 || dir == null || dir.Length != 3)
                return QueryCode.BadArg;
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.QueryRaycastFn>(PhysicsWiring._queryRaycast);
            fixed (float* o = origin, d = dir)
            {
                float* pt = stackalloc float[3];
                float* nm = stackalloc float[3];
                int rc = fn(o, d, maxT, layerMask, out int hit, pt, nm,
                            out float frac, out float dist, out uint bodyId,
                            out uint sub, out uint layer, out ulong owner);
                if (rc == QueryCode.Ok)
                    info = PackQueryHit(hit, pt, nm, frac, dist, bodyId, sub, layer, owner);
                return rc;
            }
        }

        /// <summary>形状扫掠（shapeKind 0=Sphere/1=Box/2=Capsule；dims 语义见 exports.api）。
        /// rc 同 Raycast（4=shapeKind 越界 / dims 非法）。</summary>
        public static int Sweep(float[] dims, uint shapeKind, float[] from, float[] to,
                                float[] rot, uint layerMask, out PhysicsQueryHit info)
        {
            info = default;
            if (dims == null || dims.Length != 3 || from == null || from.Length != 3 ||
                to == null || to.Length != 3 || rot == null || rot.Length != 4)
                return QueryCode.BadArg;
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.QuerySweepFn>(PhysicsWiring._querySweep);
            fixed (float* dm = dims, f = from, t = to, r = rot)
            {
                float* pt = stackalloc float[3];
                float* nm = stackalloc float[3];
                int rc = fn(dm, shapeKind, f, t, r, layerMask, out int hit, pt, nm,
                            out float frac, out float dist, out uint bodyId,
                            out uint sub, out uint layer, out ulong owner);
                if (rc == QueryCode.Ok)
                    info = PackQueryHit(hit, pt, nm, frac, dist, bodyId, sub, layer, owner);
                return rc;
            }
        }

        /// <summary>形状重叠查询（多命中收 C++ 静态表「下次同线程调用前有效」，
        /// count = min(命中数, maxHits)；经 OverlapHitAt 逐条读回）。
        /// rc：0=ok / 1=NoScene / 3=NotReady / 4=BadArg（maxHits==0 等）。</summary>
        public static int Overlap(float[] dims, uint shapeKind, float[] pos, float[] rot,
                                  uint layerMask, uint maxHits, out int count)
        {
            count = 0;
            if (dims == null || dims.Length != 3 || pos == null || pos.Length != 3 ||
                rot == null || rot.Length != 4)
                return QueryCode.BadArg;
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.QueryOverlapFn>(PhysicsWiring._queryOverlap);
            fixed (float* dm = dims, p = pos, r = rot)
                return fn(dm, shapeKind, p, r, layerMask, maxHits, out count);
        }

        /// <summary>读回静态表第 index 条命中（0 起；表清空 = 下一次同线程 query* 调用前）。
        /// rc：0=ok / 1=NoScene / 2=索引越界或表空 / 4=BadArg。</summary>
        public static int OverlapHitAt(int index, out PhysicsQueryHit info)
        {
            info = default;
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.QueryOverlapHitAtFn>(PhysicsWiring._queryOverlapHitAt);
            float* pt = stackalloc float[3];
            float* nm = stackalloc float[3];
            int rc = fn(index, out int hit, pt, nm, out float frac, out float dist,
                        out uint bodyId, out uint sub, out uint layer, out ulong owner);
            if (rc == QueryCode.Ok)
                info = PackQueryHit(hit, pt, nm, frac, dist, bodyId, sub, layer, owner);
            return rc;
        }

        /// <summary>夹具世界刚体数读回（fixture 5 体）。
        /// rc：0=ok / 1=NoScene / 3=NotReady / 4=BadArg。</summary>
        public static int BodyCount(out int count)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<PhysicsWiring.QueryBodyCountFn>(PhysicsWiring._queryBodyCount);
            return fn(out count);
        }

        private static unsafe PhysicsQueryHit PackQueryHit(
            int hit, float* pt, float* nm, float frac, float dist,
            uint bodyId, uint sub, uint layer, ulong owner)
        {
            return new PhysicsQueryHit(
                hit != 0,
                new[] { pt[0], pt[1], pt[2] },
                new[] { nm[0], nm[1], nm[2] },
                frac, dist, bodyId, sub, layer, owner);
        }
    }

    /// <summary>Props 域错误码（M-SC10；kEcsBadArg 同码位）。</summary>
    public static class PropsCode
    {
        public const int Ok = 0;
        public const int BadArg = 2;   // kind 越界 / name 空 / 表满 256
    }

    /// <summary>
    /// Props 属性面板域（M-SC10）。描述表推送通道——脚本 Entry 内经
    /// PropsPush.PushStatic(typeof(T)) 反射 [Prop] 静态字段后调用本域两导出；
    /// C++ 侧只存 POD 描述表（生命周期硬规则①）。值读写不经本域：走
    /// PropGet/PropSet 可选契约（ScriptHost 经 BootTickPtr 协议按名解析）。
    /// </summary>
    public static class Props
    {
        /// <summary>开始推送描述表（清空 C++ POD 表 + gen+1 = 面板重建信号）。
        /// rc：0=ok。</summary>
        public static int PushBegin()
        {
            var fn = Marshal.GetDelegateForFunctionPointer<PropsWiring.PropsPushBeginFn>(PropsWiring._propsPushBegin);
            return fn();
        }

        /// <summary>追加一条描述（kind 0=f/1=i/2=b/3=s；name 截断 63；min==max =
        /// 无 Range 退直输）。rc：0=ok / 2=BadArg。</summary>
        public static int DescAdd(int kind, string name, float min, float max, float def)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<PropsWiring.PropsDescAddFn>(PropsWiring._propsDescAdd);
            return fn(kind, NativeMarshal.Utf8Alloc(name), min, max, def);
        }
    }

    /// <summary>Ui 事件回调（M-UI0；elId = 元素 id 属性，eventType = Rml 事件名如
    /// "click"，param = onclick 属性值）。包装自身 catch 所有托管异常转 rc=1，
    /// 不穿互操作边界。</summary>
    public delegate void UiEventHandler(string elId, string eventType, string param);

    /// <summary>Ui 事件回调原始 ABI（与 C++ UiEventFn 对齐；uiSetEventCallback 声明面）。</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int NativeUiEventDelegate(IntPtr elId, IntPtr eventType, IntPtr param);

    /// <summary>
    /// Ui 绑定面（M-UI0，2026-11-15 拍板一步到位）。操作目标 = --rmlui-demo 时宿主
    /// 注入的 RmlUiBackend；未绑 = rc 1（kUiNoSource，不隐式建表——全族口径）。
    /// 文档 handle 由 LoadDocument 发放（&gt;0），失效安全：未知 handle 一律 rc 2。
    /// elementGetText 返回串走 C++ 静态缓冲「下次同线程调用前有效」→ Utf8Read 立即拷贝。
    /// </summary>
    public static unsafe class Ui
    {
        // reverse P/Invoke 保活（consoleRegister 同款：随 collectible ALC 整体回收，
        // C++ 侧每次事件现取指针、不缓存——热重载窗口兜底）。
        static readonly List<NativeUiEventDelegate> _pinned = new();

        /// <summary>加载 Rml 文档（assets/ 相对路径）。返回 handle（&gt;0 有效）；
        /// 0 = 失败（未绑 UiSink / 加载失败 / 非法路径——失败细节看 C++ 侧 rmlui 日志）。</summary>
        public static int LoadDocument(string path)
        {
            IntPtr pathPtr = NativeMarshal.Utf8Alloc(path);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<UiWiring.UiLoadDocumentFn>(UiWiring._uiLoadDocument);
                return fn(pathPtr, out int handle) == 0 ? handle : 0;
            }
            finally { NativeMarshal.Utf8Free(pathPtr); }
        }

        /// <summary>关闭文档。rc：0=ok / 1=未绑 UiSink / 2=句柄无效。</summary>
        public static int CloseDocument(int handle)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<UiWiring.UiCloseDocumentFn>(UiWiring._uiCloseDocument);
            return fn(handle);
        }

        /// <summary>显示/隐藏文档。rc 同 CloseDocument。</summary>
        public static int ShowDocument(int handle, bool show)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<UiWiring.UiShowDocumentFn>(UiWiring._uiShowDocument);
            return fn(handle, show ? 1 : 0);
        }

        /// <summary>按 id 写元素内文（SetInnerRML 语义）。rc：0=ok / 1=未绑 / 2=句柄或元素无效 / 4=非法参数。</summary>
        public static int ElementSetText(int handle, string elId, string text)
        {
            IntPtr idPtr = NativeMarshal.Utf8Alloc(elId), textPtr = NativeMarshal.Utf8Alloc(text);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<UiWiring.UiElementSetTextFn>(UiWiring._uiElementSetText);
                return fn(handle, idPtr, textPtr);
            }
            finally { NativeMarshal.Utf8Free(idPtr); NativeMarshal.Utf8Free(textPtr); }
        }

        /// <summary>按 id 读元素内文（GetInnerRML 语义；返回 null = rc 非 0）。</summary>
        public static string? ElementGetText(int handle, string elId)
        {
            IntPtr idPtr = NativeMarshal.Utf8Alloc(elId);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<UiWiring.UiElementGetTextFn>(UiWiring._uiElementGetText);
                int rc = fn(handle, idPtr, out IntPtr textPtr);
                return rc == 0 ? NativeMarshal.Utf8ReadOrEmpty(textPtr) : null;
            }
            finally { NativeMarshal.Utf8Free(idPtr); }
        }

        /// <summary>注册 Ui 事件回调（friendly 包装；异常转 rc=1 不穿边界）。</summary>
        public static int SetEventCallback(UiEventHandler handler)
        {
            NativeUiEventDelegate adapter = (IntPtr elId, IntPtr eventType, IntPtr param) =>
            {
                try
                {
                    handler(Marshal.PtrToStringUTF8(elId) ?? "",
                            Marshal.PtrToStringUTF8(eventType) ?? "",
                            Marshal.PtrToStringUTF8(param) ?? "");
                    return 0;
                }
                catch (Exception) { return 1; }   // 托管异常 → rc=1（不穿边界）
            };
            return SetEventCallbackRaw(adapter);
        }

        /// <summary>原始 ABI 注册（异常不经包装捕获 → C++ SEH 边界路径）。</summary>
        public static int SetEventCallbackRaw(NativeUiEventDelegate raw)
        {
            _pinned.Add(raw);
            var fn = Marshal.GetDelegateForFunctionPointer<UiWiring.UiSetEventCallbackFn>(UiWiring._uiSetEventCallback);
            return fn(Marshal.GetFunctionPointerForDelegate(raw));
        }
    }

    /// <summary>Physics 查询域错误码（M-W2A；独立于载具 PhysicsCode 计数）：
    /// BadIndex = OverlapHitAt 索引越界或静态表空；NotReady = 夹具未 build。</summary>
    public static class QueryCode
    {
        public const int Ok = 0;
        public const int NoScene = 1;     // 未绑 QueryScene（宿主忘 querySetScene）
        public const int BadIndex = 2;    // 索引越界或表空
        public const int NotReady = 3;    // build 未就绪
        public const int BadArg = 4;      // validate*Query 拒绝（零向量/非有限值等）
    }

    /// <summary>查询命中快照（Raycast/Sweep/OverlapHitAt 打包；Hit 0|1）。</summary>
    public readonly struct PhysicsQueryHit
    {
        public readonly bool Hit;
        public readonly float[] Point;    // [3]
        public readonly float[] Normal;   // [3]
        public readonly float Fraction;   // 0..1 along query
        public readonly float Distance;   // 世界单位
        public readonly uint BodyId;
        public readonly uint SubShapeId;
        public readonly uint Layer;
        public readonly ulong Owner;
        public PhysicsQueryHit(bool hit, float[] point, float[] normal, float fraction,
                               float distance, uint bodyId, uint subShapeId, uint layer,
                               ulong owner)
        { Hit = hit; Point = point; Normal = normal; Fraction = fraction; Distance = distance;
          BodyId = bodyId; SubShapeId = subShapeId; Layer = layer; Owner = owner; }
    }

    /// <summary>Audio 域错误码（M-W2A / M-SC9 既定行）：Invalid = 句柄无效或
    /// AudioWorld 拒绝（spatialization 未开等，照实回）；SourceFailed = createSource
    /// 或复合播放链失败（细节走 script 日志通道）。</summary>
    public static class AudioCode
    {
        public const int Ok = 0;
        public const int NoWorld = 1;      // 未绑 AudioWorld（宿主忘 audioSetWorld）
        public const int Invalid = 2;      // 句柄无效或 AudioWorld 拒绝
        public const int SourceFailed = 3; // createSource / configure / start 失败
        public const int BadArg = 4;       // 路径白名单拒绝 / 指针 NULL / 非有限值
    }

    /// <summary>
    /// Audio 绑定面（M-W2A，M-SC9 既定行落地）。操作目标 = 宿主侧 audioSetWorld
    /// 绑定的 AudioWorld（viewer scene 模式 ensureAudioWorld() 注入；未绑 = NoWorld）。
    /// wavPath = assets/audio 下文件名（basename 白名单防穿越）；Play = createSource
    /// -> configure -> setSourcePosition -> start 复合链（M-S3 audio.play 同款）。
    /// </summary>
    public static unsafe class Audio
    {
        /// <summary>加载并播放 WAV（PCM16）。rc：0=ok（sourceId&gt;0）/ 1=NoWorld /
        /// 3=SourceFailed / 4=BadArg。</summary>
        public static int Play(string wavPath, float[] pos, bool looping, out int sourceId)
        {
            sourceId = 0;
            if (wavPath == null || pos == null || pos.Length != 3) return AudioCode.BadArg;
            IntPtr pathPtr = NativeMarshal.Utf8Alloc(wavPath);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<AudioWiring.AudioPlayFn>(AudioWiring._audioPlay);
                fixed (float* p = pos)
                    return fn(pathPtr, p, looping ? 1 : 0, out sourceId);
            }
            finally { NativeMarshal.Utf8Free(pathPtr); }
        }

        /// <summary>停止声源（源保留可复用）。rc：0=ok / 1=NoWorld / 2=Invalid / 4=BadArg。</summary>
        public static int Stop(int id)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<AudioWiring.AudioStopFn>(AudioWiring._audioStop);
            return fn(id);
        }

        /// <summary>写声源增益（保持既有 looping）。rc：0=ok / 1=NoWorld / 2=Invalid / 4=BadArg。</summary>
        public static int SetGain(int id, float gain)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<AudioWiring.AudioSetGainFn>(AudioWiring._audioSetGain);
            return fn(id, gain);
        }

        /// <summary>写声源空间位置（需宿主 spatialization 开；未开 = Invalid 照实回）。
        /// rc：0=ok / 1=NoWorld / 2=Invalid / 4=BadArg。</summary>
        public static int SetSourcePosition(int id, float[] pos)
        {
            if (pos == null || pos.Length != 3) return AudioCode.BadArg;
            var fn = Marshal.GetDelegateForFunctionPointer<AudioWiring.AudioSetSourcePositionFn>(AudioWiring._audioSetSourcePosition);
            fixed (float* p = pos)
                return fn(id, p);
        }

        /// <summary>写监听者位姿（position + direction）。rc：0=ok / 1=NoWorld / 2=Invalid / 4=BadArg。</summary>
        public static int SetListenerPose(float[] pos, float[] dir)
        {
            if (pos == null || pos.Length != 3 || dir == null || dir.Length != 3)
                return AudioCode.BadArg;
            var fn = Marshal.GetDelegateForFunctionPointer<AudioWiring.AudioSetListenerPoseFn>(AudioWiring._audioSetListenerPose);
            fixed (float* p = pos, d = dir)
                return fn(p, d);
        }

        /// <summary>播放态读回。rc：0=ok / 1=NoWorld / 2=Invalid / 4=BadArg。</summary>
        public static int IsPlaying(int id, out bool playing)
        {
            playing = false;
            var fn = Marshal.GetDelegateForFunctionPointer<AudioWiring.AudioIsPlayingFn>(AudioWiring._audioIsPlaying);
            int rc = fn(id, out int flag);
            if (rc == AudioCode.Ok) playing = flag != 0;
            return rc;
        }
    }

    /// <summary>Animation 域错误码（M-W2A 薄面）：Invalid = 句柄无效（clip/sm 单源
    /// 发号表查无）；Load = clip 加载或校验失败；Reset = 状态机 desc 校验失败；
    /// BadAsset = 路径白名单拒绝。</summary>
    public static class AnimCode
    {
        public const int Ok = 0;
        public const int Invalid = 1;    // 句柄无效
        public const int Load = 2;       // clip 加载或校验失败（error 文本带因）
        public const int Reset = 3;      // validateStateMachine 失败
        public const int BadArg = 4;     // 非法参数（clipIndex 越界 / 数组 NULL / idx 越界等）
        public const int BadAsset = 5;   // 路径白名单拒绝
    }

    /// <summary>转移规则声明（声明序 = 优先级；判据 = params[ParamIndex] &gt;= Threshold）。</summary>
    public readonly struct AnimRule
    {
        public readonly int From;
        public readonly int To;
        public readonly int ParamIndex;
        public readonly float Threshold;
        public readonly float BlendDuration;
        public AnimRule(int from, int to, int paramIndex, float threshold, float blendDuration)
        { From = from; To = to; ParamIndex = paramIndex; Threshold = threshold; BlendDuration = blendDuration; }
    }

    /// <summary>
    /// Animation 绑定面（M-W2A 薄面句柄表）。clip 入口 = loadGltfAnim（M-A4b），
    /// clipPath = assets/models 下 .glb basename（白名单防穿越）；状态转移 = C++
    /// 侧预置规则 lambda（nl/anim 公开面零改动——transitionTo 保持 private），
    /// 脚本只经 SetParam 写标量 + Tick 触发优先级转移；规则声明序 = 优先级。
    /// 句柄单源发号（clip/sm 同表），Release 双态擦除失效安全；reload 后句柄清空
    ///（脚本自有状态随换载注销——生命周期硬规则①）。
    /// </summary>
    public static unsafe class Anim
    {
        /// <summary>加载 glTF 动画 clip（按 clipIndex 取一条；clipId &gt;0 有效）。
        /// rc：0=ok / 2=Load（error 带因）/ 4=BadArg / 5=BadAsset。</summary>
        public static int LoadClip(string clipPath, int clipIndex, out int clipId, out string? error)
        {
            clipId = 0;
            error = null;
            if (clipPath == null || clipIndex < 0) return AnimCode.BadArg;
            IntPtr pathPtr = NativeMarshal.Utf8Alloc(clipPath);
            try
            {
                var fn = Marshal.GetDelegateForFunctionPointer<AnimWiring.AnimLoadClipFn>(AnimWiring._animLoadClip);
                int rc = fn(pathPtr, clipIndex, out clipId, out IntPtr errPtr);
                if (rc != AnimCode.Ok) error = NativeMarshal.Utf8ReadOrEmpty(errPtr);
                return rc;
            }
            finally { NativeMarshal.Utf8Free(pathPtr); }
        }

        /// <summary>用已加载 clip 建单层状态机（states 自动命名；规则声明序 = 优先级；
        /// rules 允许 null/空 = 零转移规则（单态/纯采样机）；smId &gt;0 有效）。
        /// rc：0=ok / 1=Invalid（clip 句柄查无）/ 3=Reset / 4=BadArg。</summary>
        public static int CreateStateMachine(int[] clipIds, AnimRule[]? rules, out int smId)
        {
            smId = 0;
            if (clipIds == null || clipIds.Length == 0) return AnimCode.BadArg;
            int ruleCount = rules?.Length ?? 0;
            int[] meta = new int[ruleCount * 3];
            float[] vals = new float[ruleCount * 2];
            for (int i = 0; i < ruleCount; ++i)
            {
                meta[i * 3 + 0] = rules![i].From;
                meta[i * 3 + 1] = rules[i].To;
                meta[i * 3 + 2] = rules[i].ParamIndex;
                vals[i * 2 + 0] = rules[i].Threshold;
                vals[i * 2 + 1] = rules[i].BlendDuration;
            }
            var fn = Marshal.GetDelegateForFunctionPointer<AnimWiring.AnimCreateStateMachineFn>(AnimWiring._animCreateStateMachine);
            // 数组仅在调用期存活（生命周期账本：C++ 调用期拷走，不持指针）。
            fixed (int* cp = clipIds)
            {
                if (ruleCount == 0)
                    return fn((IntPtr)cp, clipIds.Length, IntPtr.Zero, IntPtr.Zero, 0, out smId);
                fixed (int* m = meta)
                fixed (float* v = vals)
                    return fn((IntPtr)cp, clipIds.Length, (IntPtr)m, (IntPtr)v, ruleCount, out smId);
            }
        }

        /// <summary>写状态机标量参数（转移判据输入；idx 越界 = BadArg）。
        /// rc：0=ok / 1=Invalid / 4=BadArg。</summary>
        public static int SetParam(int smId, int idx, float value)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<AnimWiring.AnimSetParamFn>(AnimWiring._animSetParam);
            return fn(smId, idx, value);
        }

        /// <summary>推进状态机（update + evaluate 缓存最近 pose）。
        /// rc：0=ok / 1=Invalid / 4=BadArg（dt 非有限——nl/anim 护栏按 0，此处照回）。</summary>
        public static int Tick(int smId, float dt)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<AnimWiring.AnimTickFn>(AnimWiring._animTick);
            return fn(smId, dt);
        }

        /// <summary>读回最近缓存姿态单骨骼（pose7 = tx ty tz qx qy qz qw；boneIndex
        /// 越界 = BadArg）。rc：0=ok / 1=Invalid / 4=BadArg。</summary>
        public static int GetPose(int smId, int boneIndex, out float[] pose7)
        {
            pose7 = new float[7];
            var fn = Marshal.GetDelegateForFunctionPointer<AnimWiring.AnimGetPoseFn>(AnimWiring._animGetPose);
            int rc;
            unsafe
            {
                float* buf = stackalloc float[7];
                rc = fn(smId, boneIndex, buf);
                if (rc == AnimCode.Ok)
                    for (int i = 0; i < 7; ++i) pose7[i] = buf[i];
            }
            return rc;
        }

        /// <summary>状态机观测读回（current=当前态下标 / alpha=转移目标权重（未转移=1）/
        /// duration=当前 crossfade 生效时长（未转移=0））。rc：0=ok / 1=Invalid / 4=BadArg。</summary>
        public static int StateInfo(int smId, out int current, out float alpha, out float duration)
        {
            current = 0; alpha = 0f; duration = 0f;
            var fn = Marshal.GetDelegateForFunctionPointer<AnimWiring.AnimStateInfoFn>(AnimWiring._animStateInfo);
            return fn(smId, out current, out alpha, out duration);
        }

        /// <summary>显式回收句柄（clip 或 sm 双态擦除；未知 = Invalid 失效安全）。
        /// rc：0=ok / 1=Invalid / 4=BadArg。</summary>
        public static int Release(int handle)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<AnimWiring.AnimReleaseFn>(AnimWiring._animRelease);
            return fn(handle);
        }
    }
}
