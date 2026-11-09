// M-SC0 骨架：脚本 API 绑定面的根命名空间 + 版本锚。
// M-SC1 起 console 绑定（cvar.get/set/list、console.exec、Console.Register）落本程序集；
// M-SC2 起 ECS 数据面绑定（World.Spawn/Remove/SetParent、TransformComponent 字段读写）落本程序集。
// 热更安全约定（P8-SCRIPT-PLAN.md）：绑定面 API 无跨帧状态；状态全在 C++ 侧。
namespace Nl.Script.Api
{
    /// <summary>脚本 API 绑定面版本锚（selftest / echo 用例用它证明 script-api 已装载）。</summary>
    public static class Api
    {
        public const string Version = "0.1.0";
    }
}
