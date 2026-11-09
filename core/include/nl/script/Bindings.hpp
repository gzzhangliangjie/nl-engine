#pragma once
// M-SC1：console 绑定面（P8-SCRIPT-PLAN.md，C++↔C# 单向显式函数指针）。
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

#include <cstdint>

namespace nl::scene { class World; }

namespace nl::script {

// ECS 导出通用错误码（v2；写死防漂移）：
//   0=ok / 1=未绑 World（宿主忘 ecsSetWorld）/ 2=实体不存在（失效安全拒绝）/
//   3=组件缺失或重复挂 / 4=非法参数（空指针等）
inline constexpr int kEcsOk = 0;
inline constexpr int kEcsNoWorld = 1;
inline constexpr int kEcsNoEntity = 2;
inline constexpr int kEcsComponent = 3;
inline constexpr int kEcsBadArg = 4;

// C ABI api 表（随托管入口 args 传入；C# 侧按偏移 Marshal.ReadIntPtr 读）。
// 字段顺序 = 表内偏移顺序（64 位指针，逐个 8 字节）。v1 console 6 导出 +
// v2 ECS 9 导出（M-SC2，尾部追加——C# 按偏移读，旧脚本零波及）。
struct ApiTable {
    // int cvarGet(const char* name, const char** outValue)
    //   返回类型码：0=未注册 / 1=bool / 2=int / 3=float / 4=string；*outValue=toString()
    void* cvarGet;
    // int cvarSet(const char* name, const char* value)
    //   引擎路径文本赋值（setFromString 语义，onChanged 触发）：
    //   0=ok / 1=未知 cvar / 2=类型不符或格式非法
    void* cvarSet;
    // const char* cvarList()
    //   全量 CVar 视图（"name = value (type) help" 逐行），持有规则见文件头
    void* cvarList;
    // int consoleExec(const char* line, const char** outOutput)
    //   ExecCode 0..3（Ok/UnknownCommand/BadRequest/CommandFailed），*outOutput=输出文本
    void* consoleExec;
    // int consoleRegister(const char* name, const char* help, void* fn)
    //   0=ok / 1=名字已被 CVar 占用。fn 命令回调 ABI：
    //   int fn(int argc, const char** argv, const char** outText)
    //     返回 0=ok / 1=失败；*outText 可为 NULL（无输出文本）
    void* consoleRegister;
    // int consoleUnregister(const char* name)
    //   0=ok / 1=未知命令（M-SC3 热重载解绑的落点）
    void* consoleUnregister;

    // ---- v2 ECS（M-SC2；操作目标 = ecsSetWorld 绑定的 World；延迟语义沿 World 口径）----

    // int ecsSpawn(const char* name, unsigned long long* outID)
    //   newEntity（新建立即可见，Carrot 口径）；*outID=实体 ID（0=无效）
    void* ecsSpawn;
    // int ecsRemove(unsigned long long id)
    //   延迟删除（排队，flush/tick 生效；父删递归删子）
    void* ecsRemove;
    // int ecsFlush()
    //   flushEntityCreationAndRemoval（宿主 tick 边界动作；Carrot flush 口径）
    void* ecsFlush;
    // int ecsExists(unsigned long long id, int* outExists)
    //   *outExists: 0=不存在 / 1=存在（新建即时真、删除 flush 后假——非对称 Carrot 口径）
    void* ecsExists;
    // int ecsSetParent(unsigned long long id, unsigned long long parentID)
    //   parentID=0 解除父子；环防护 = World::setParent 口径（父不得是后代）
    void* ecsSetParent;
    // int ecsGetParent(unsigned long long id, unsigned long long* outParent)
    //   *outParent=0 无父
    void* ecsGetParent;
    // int ecsAddTransform(unsigned long long id)
    //   挂 TransformComponent（重复挂 = 3）
    void* ecsAddTransform;
    // int ecsTransformGet(unsigned long long id, float* outPos3, float* outRot4, float* outScale3)
    //   局部 TRS 读回；rot = quat (x,y,z,w)
    void* ecsTransformGet;
    // int ecsTransformSet(unsigned long long id, const float* pos3, const float* rot4, const float* scale3)
    //   局部 TRS 写入；任一指针可 NULL（跳过该组字段）
    void* ecsTransformSet;
    // int ecsFindByName(const char* name, unsigned long long* outID)
    //   按名查实体（World::findEntityByName 口径）；未找到 *outID=0 返回 0
    void* ecsFindByName;
};

// 单例 api 表（函数指针静态填充；内容与 Bindings.cpp 的 extern "C" 实现一一对应）。
const ApiTable* consoleApiTable();

// 宿主侧 World 绑定（M-SC2）：入口调用前设置，ECS 导出全部操作该实例；
// nullptr = 解绑（此后 ECS 导出一律返回 kEcsNoWorld——M-SC3 热重载窗口的兜底）。
void ecsSetWorld(scene::World* world);

} // namespace nl::script
