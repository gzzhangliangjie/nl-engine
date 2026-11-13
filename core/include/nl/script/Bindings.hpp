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
#include "nl/script/ApiTable.g.hpp"   // M-SC4：ApiTable 结构 + POD 镜像 = 生成物
                                      //（单源 script/script-api/exports.api；手改无效，
                                      // 重跑 tools/scriptapi/gen.py；新鲜度闸 CMake 侧强制）

#include <cstdint>
#include <string>
#include <vector>

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

// ApiTable 结构 = 生成物（ApiTable.g.hpp，上放 include）：字段序 = exports.api 声明序，
// 每导出带 rc 语义文档。手写结构已废——改绑定面 = 改 exports.api + 重跑 gen.py（M-SC4）。
// v1 console 6 导出 + v2 ECS 10 导出（M-SC2 尾部追加口径，旧脚本零波及）。

// 单例 api 表（函数指针静态填充；内容与 Bindings.cpp 的 extern "C" 实现一一对应，
// 编译期由 ApiTable.g.asserts.inc 逐导出校验声明与实现签名一致）。
const ApiTable* consoleApiTable();

// 宿主侧 World 绑定（M-SC2）：入口调用前设置，ECS 导出全部操作该实例；
// nullptr = 解绑（此后 ECS 导出一律返回 kEcsNoWorld——M-SC3 热重载窗口的兜底）。
void ecsSetWorld(scene::World* world);

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
