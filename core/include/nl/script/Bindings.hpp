#pragma once
// M-SC1：console 绑定面（P8-SCRIPT-PLAN.md，C++↔C# 单向显式函数指针）。
//
// 载具域宿主绑定目标（M-SC8 前置声明；PIMPL 公开类，Jolt 符号面挡在
// core/src/physics/World.cpp——绑定头零 Jolt 包含）。
namespace nl::physics {
class DebugScene;
}
//
// 口径来源（三处对照，2026-11-09 留痕）：
//   - Carrot engine/engine/scripting/CSharpBindings.cpp + script-api/Carrot/（抄思路
//     不抄代码）：绑定面 = Mono mono_add_internal_call 按名字注册内部调用；Mono 底座
//     不走（M-SC0 拍板 hostfxr），.NET hosting 无按名 internal call——对应物是
//     Microsoft hosting 口径的「显式函数指针表随入口 args 传入」，思路同构（单一
//     显式绑定面、无反射魔法）。
//   - UE IConsoleManager（只读对照）：CVar/命令统一注册表口径已在 M-C0 对照过；
//     UE 与脚本层无 C# 绑定（走 Blueprint/Python），据实标注无对应物。
//   - bevy：无控制台无脚本 crate，据实标注无对应物。
//
// M-SC2（ECS 数据面）：Carrot 的脚本↔ECS 桥 = C# World.Current 单例 + internal call
// 直接操作 World（engine/scripting/SceneBridge.cs 口径：句柄 = EntityID 数值）；对应物
// 同 M-SC1——ApiTable 尾部追加 ECS 导出，World 实例由宿主侧 ecsSetWorld() 显式绑定
// （宿主注册自家 cvar 同款时序：绑 World → 调入口）。Entity wrapper = C# 侧
// { ulong Id } 轻量壳（Carrot Entity 同构），失效安全拒绝 = 每个导出先查 exists()。
//
// ABI 约定（写死，防两边漂移；v1 固定顺序，只许尾部追加）：
//   - 所有字符串 UTF-8（'\0' 结尾）；
//   - 调用约定 __cdecl（x64 Windows 统一，标注只为可读）；
//   - 非线程安全：console 主线程约定（core/console.hpp 边界）直接沿用；
//
// 内存所有权（两条路各定一条，不许混）：
//   - C++ → C#（返回值）：C++ 侧静态缓冲持有，**下一次同线程 API 调用前有效**，
//     C# 侧包装立即拷贝成托管串（console 单线程约定下无竞态）；
//   - C# → C++（命令回调 outText）：C# 用 Marshal.AllocHGlobal 分配（Windows 实现是
//     CoTaskMemAlloc），C++ 拷成 std::string 后 CoTaskMemFree 释放（COM 字符串同款配对）。
//
// 托管命令回调异常边界：C++ 侧经 SEH __except 捕获（0xe0434352），转
// CommandResult{false}→ExecCode::CommandFailed，进程存活（与 Vm/工具同口径）。

#include "nl/script/Vm.hpp"
#include "nl/script/ApiTable.g.hpp"   // M-SC4：ApiTable 结构 + POD 镜像 = 生成物
                                      //（单源 script/script-api/exports.api；手改无效，
                                      // 重跑 tools/scriptapi/gen.py；新鲜度闸 CMake 侧强制）

#include <cstdint>
#include <string>
#include <vector>

namespace nl::scene { class World; }
namespace nl::core { class GameClock; class InputState; class ActionMap; }

namespace nl::script {

// ECS 导出通用错误码（v2；写死防漂移）：
//   0=ok / 1=未绑 World（宿主忘 ecsSetWorld）/ 2=实体不存在（失效安全拒绝）/
//   3=组件缺失或重复挂 / 4=非法参数（空指针等）
inline constexpr int kEcsOk = 0;
inline constexpr int kEcsNoWorld = 1;
inline constexpr int kEcsNoEntity = 2;
inline constexpr int kEcsComponent = 3;
inline constexpr int kEcsBadArg = 4;

// ECS 模型挂载扩展码（v5 M-SC7，尾部追加不与既有码冲突）：
//   5=挂模型但实体无 TransformComponent（E1b「Model without Transform = 拒」
//     同口径——不隐式建表，宿主先 ecsAddTransform）/ 6=meshFile 白名单拒绝
//     （nl::scene::validGlbAssetName：防路径穿越，坏名不落 World）。
inline constexpr int kEcsNoTransform = 5;
inline constexpr int kEcsBadAsset = 6;

// Time 导出错误码（v3 M-SC5）：0=ok / 1=未绑 Clock（宿主忘 timeSetClock）/ 4=非法参数
inline constexpr int kTimeOk = 0;
inline constexpr int kTimeNoClock = 1;
inline constexpr int kTimeBadArg = 4;

// Input 导出错误码（v3 M-SC5）：0=ok / 1=未绑源（ActionMap/InputState，宿主忘
// inputSetState/inputSetActionMap——不隐式建表拍板口径）/ 2=动作不存在或类型不符 /
// 4=非法参数
inline constexpr int kInputOk = 0;
inline constexpr int kInputNoSource = 1;
inline constexpr int kInputNoAction = 2;
inline constexpr int kInputBadArg = 4;

// Log 导出错误码（v3 M-SC5）：0=ok / 1=过滤指令非法 / 2=非法参数（level 越界等）
inline constexpr int kLogOk = 0;
inline constexpr int kLogBadFilter = 1;
inline constexpr int kLogBadArg = 2;

// Camera 导出错误码（v4 M-SC6）：0=ok / 1=未绑 CameraSource（宿主忘 cameraSetSource）/
// 2=非法参数（eye 指针 NULL 等）/ 3=该模式不支持脚本写位姿（track = 轨迹数据主权）
inline constexpr int kCamOk = 0;
inline constexpr int kCamNoSource = 1;
inline constexpr int kCamBadArg = 2;
inline constexpr int kCamModeUnsupported = 3;

// Physics/载具导出错误码（v6 M-SC8）：0=ok / 1=未绑 DebugScene（宿主忘
// driveSetScene——kEcsNoWorld/kCamNoSource 同构口径）/ 2=槽位不存在或未用槽
//（tombstone 后同）/ 3=加车失败（DebugScene::addVehicle 返回 -1，lastError 置因）/
// 4=非法参数（out 指针 NULL / in 指针 NULL——kEcsBadArg 同款）
inline constexpr int kPhyOk = 0;
inline constexpr int kPhyNoScene = 1;
inline constexpr int kPhyNoSlot = 2;
inline constexpr int kPhyAddFailed = 3;
inline constexpr int kPhyBadArg = 4;

// ApiTable 结构 = 生成物（ApiTable.g.hpp，上放 include）：字段序 = exports.api 声明序，
// 每导出带 rc 语义文档。手写结构已废——改绑定面 = 改 exports.api + 重跑 gen.py（M-SC4）。
// v1 console 6 导出 + v2 ECS 10 导出（M-SC2 尾部追加口径，旧脚本零波及）。

// 单例 api 表（函数指针静态填充；内容与 Bindings.cpp 的 extern "C" 实现一一对应，
// 编译期由 ApiTable.g.asserts.inc 逐导出校验声明与实现签名一致）。
const ApiTable* consoleApiTable();

// 宿主侧 World 绑定（M-SC2）：入口调用前设置，ECS 导出全部操作该实例；
// nullptr = 解绑（此后 ECS 导出一律返回 kEcsNoWorld——M-SC3 热重载窗口的兜底）。
void ecsSetWorld(scene::World* world);

// 宿主侧活实例绑定（M-SC5，ecsSetWorld 模式推广；nullptr = 解绑，导出回 NoSource/NoClock）：
//   - timeSetClock：Time 导出操作目标（viewer --script 场景模式接线 gameClock）；
//   - inputSetState / inputSetActionMap：Input 导出操作目标。**不隐式建表**（拍板）——
//     未绑一律 kInputNoSource；宿主不建 InputState 就没有键查询（viewer scene 模式
//     现状：无注入管线，Input 保持未绑，脚本得 NoSource 即正确语义）。
void timeSetClock(core::GameClock* clock);
void inputSetState(core::InputState* state);
void inputSetActionMap(core::ActionMap* map);

// 宿主侧相机绑定源（v4 M-SC6）：现役相机经 4 个函数指针适配进 core——
// Bindings.cpp 在 nl_core，**不能反向 include viewer**（依赖方向不变），
// 且裸绑 CameraRig 会丢 fov 持久化与 orbit 模式控制，故落适配源形态
// （计划「cameraSetRig」为落码前占名，实码定名 cameraSetSource，口径不变：
// 宿主显式 set + 未绑专属错误码 + 热更窗口兜底）。
//
// mode 码 = viewer::CameraMode 枚举序（0=orbit/1=free/2=chase/3=track）；
// setPose 模式感知落点（注册方实现决定）：orbit=setOrbit(yaw,pitch,|eye|)+种子 /
// free=setPose 直接落 / chase=snapEyeTo+setPitch（yaw 由车体航向驱动，入参忽略）/
// track=返回 kCamModeUnsupported（轨迹数据主权在轨迹文件，脚本不可写）。
// setFov 注册方负责 clamp [20,120]（与 OrbitCamera::setFovYDegrees 同口径）后透传。
struct CameraSource {
    void* ctx;   // 注册方上下文（如 &ViewerCamera）
    void (*getPose)(void* ctx, float outEye[3], float* outYawDeg, float* outPitchDeg,
                    float* outFovY, int* outMode);
    int (*setPose)(void* ctx, const float eye[3], float yawDeg, float pitchDeg);
    float (*getFov)(void* ctx);
    void (*setFov)(void* ctx, float fovYDeg);
};
// nullptr = 解绑（此后 Camera 导出一律 kCamNoSource——热更窗口兜底，同族口径）。
void cameraSetSource(const CameraSource* source);

// 宿主侧载具训练场绑定（v6 M-SC8，ecsSetWorld 模式推广）：直持
// nl::physics::DebugScene*（拍板 Q-SC8-1 A'——P8-SCRIPT-PLAN L107 既定口径
// 「直接对齐 M-NET2 多载具 API 面不新造」；DebugScene PIMPL 已挡 Jolt，公开头
// 零新类型。CameraSource 函数指针表是四模式相机专属形态，此处属过度设计）。
// nullptr = 解绑（热更窗口兜底，此后 Physics 导出一律 kPhyNoScene）。
void driveSetScene(physics::DebugScene* scene);

// ---- M-SC10 属性面板域（v7）：描述表 POD + gen 计数 ----
//
// 通道分工（生命周期硬规则①「C++ 侧永不跨 reload 缓存托管引用」的落地）：
//   - 描述表（本文件，C# 推）：C# 侧 Props.PushStatic 反射枚举 [Prop] 静态字段
//     → propsPushBegin/propsDescAdd 推 POD 表。C++ 只存 POD（FieldInfo 等
//     托管反射缓存全部留在脚本程序集内随 ALC 回收）；pushBegin 时 gen+1 =
//     面板/宿主重建信号；重复 push = 覆盖（reload 幂等）。
//   - 值读写（ScriptHost，C++ 拉）：BootTickPtr 协议复用——按名解析可选契约
//     PropGet/PropSet（同 OnTick：缺失 = 通道惰性；reload 重取；unload 前置空）。
//
// 值 ABI（script-api PropsPush.PropGet/PropSet 同文锚）：buf[0..3]=int32 idx
//（C++ 包装填）；标量 buf[4..7]=f32/i32/b(0/1)；string buf[4..259]=UTF8 尾 0
//（get 截断 255，set 读到首个 0 上限 255）。
inline constexpr int kPropsOk = 0;
inline constexpr int kPropsBadArg = 2;   // kind 越界 / name 空 / 表满（kEcsBadArg 同码位）
inline constexpr int kPropKindFloat = 0;
inline constexpr int kPropKindInt = 1;
inline constexpr int kPropKindBool = 2;
inline constexpr int kPropKindString = 3;
inline constexpr int kMaxProps = 256;
inline constexpr int kPropNameCap = 63;      // name 截断（含尾 0 的缓冲 64）
inline constexpr int kPropStringCap = 255;   // string 值区上限（含尾 0 的缓冲 256）
struct PropDesc {
    int kind;                 // kPropKind*
    char name[kPropNameCap + 1];
    float min, max;           // min==max = 无 Range（面板退直输控件）
    float def;                // 推送时字段现值快照（string 恒 0，初值经通道 propGet）
};
const std::vector<PropDesc>& scriptProps();          // 现役描述表（只读）
std::uint32_t scriptPropsGen();                      // 描述表代数（pushBegin +1）

// ---- M-SC3 脚本命令登记（热重载解绑的 C++ 侧台账）----
//
// nl_consoleRegister（C# → C++ 命令注册）把命令名登记进脚本命令表；
// nl_consoleUnregister（C# 主动注销）对称擦除。表内命令的回调都指向
// collectible ALC 内的 reverse P/Invoke thunk——Unload 会注销 thunk，
// C++ 侧残留引用调用即崩，故 ScriptHost::reloadScript 必须先
// scriptUnregisterAll()（逐个解绑 + 清表，返回解绑名单）再 vm->scriptUnload()。
//
// 热重载编排语义（铁律）：unload 前表必须清空（不然 = 卸载后调用悬空指针）。

// 当前脚本注册的命令名快照（登记序；ScriptHost 输出信息 / selftest 断言用）。
std::vector<std::string> scriptRegisteredCommands();

// 逐个 unregisterCommand + 清登记表，返回被解绑的命令名（解除对旧 ALC thunk
// 的全部 C++ 侧引用；unknown 命令跳过——登记表与命令表本应同步，容错不抛）。
std::vector<std::string> scriptUnregisterAll();

} // namespace nl::script
