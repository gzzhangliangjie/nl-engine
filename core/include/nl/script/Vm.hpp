#pragma once

// M-SC0：C# hosting 内核（docs/p8/P8-SCRIPT-PLAN.md，.NET hosting 鸣潮方向）。
//
// 口径来源（三处对照 + 官方文档精读，2026-11-09 留痕）：
//   - Microsoft Learn《Write a custom .NET runtime host》（权威口径）：
//     全流程 3 个导出函数 —— hostfxr_initialize_for_runtime_config ->
//     hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer) ->
//     hostfxr_close；hostfxr.dll 路径由 nethost 的 get_hostfxr_path 定位；
//     托管入口默认签名 component_entry_point_fn = int (*)(void* args, int32 sizeBytes)。
//   - Carrot core/core/scripting/csharp/（抄结构不抄底座）：hosting/绑定分层 +
//     不透明句柄（PIMPL 同构）；Carrot 底座是 Mono（2026-11-09 修正 + 用户拍板
//     不走 Mono），Mono 具体 API 一律不抄，我们走 hostfxr。
//   - bevy 无脚本 crate（克制先例：只做最小嵌入，不造脚本框架）。
//
// 官方口径两条硬限制（写死进设计）：
//   ① 单进程只能装一个 .NET 运行时（重复 init 只做兼容性检查）；
//   ② hosting API 只支持 FDD（框架依赖部署）——脚本程序集不能 self-contained，
//      必须有对应 *.runtimeconfig.json。
//
// 热更根基（UE/鸣潮共同口径，M-SC3 落地）：脚本无跨帧状态、状态全在 C++ 侧；
// 脚本程序集经 collectible ALC 轮换（loadBootstrap/scriptLoad/scriptCall/
// scriptUnload），运行时本体全程不重初始化（单运行时硬限制①）。
//
// 纪律：core 零图形 API（NVRHI/ImGui 零 include）；头文件不暴露 hostfxr.h /
// nethost.h（PIMPL，hostfxr 符号只在 Vm.cpp）；错误一律转 Result 结构，
// 托管异常在互操作边界捕获（SEH __except），不许穿透 C++ 析构。

#include <cstdint>
#include <memory>
#include <string>

namespace nl::script {

// 统一错误结构（plan 拍板「Result 式结构」，照 console ExecCode 退出码先例）。
// ok=false 时 code 是宿主侧阶段码、message 带根因：
//   1 = nethost 定位 hostfxr.dll 失败
//   2 = LoadLibrary(hostfxr.dll) / 导出函数解析失败
//   3 = hostfxr_initialize_for_runtime_config 失败（runtimeconfig.json 缺失/不兼容）
//   4 = hostfxr_get_runtime_delegate 失败
//   5 = 程集加载 / 类型·方法解析失败（load_assembly_and_get_function_pointer 返回错误）
//   6 = 托管异常（互操作边界 SEH 捕获，异常码记录在 message）
//   7 = bootstrap 未装载（M-SC3 热更 API 在 loadBootstrap 前调用）
//   8 = 热更协议错误（bootstrap 返回负码：load -1/-2/-3、call -100/-101/-102、
//       unload -1，逐码带语义）
//   9 = 热更卸载未完成（bootstrap weakref 校验 GC 未收干净）
struct Result {
    bool ok = false;
    int code = 0;
    std::string message;

    static Result success() { return Result{true, 0, {}}; }
    static Result fail(int code, std::string msg) { return Result{false, code, std::move(msg)}; }
};

// C# 托管入口的 C++ 侧函数指针（默认签名，plan 拍板口径）。
// C# 侧对应：[UnmanagedCallersOnly] public static int Method(IntPtr args, int sizeBytes)。
#if defined(_WIN32)
using EntryPointFn = int(__stdcall*)(void* args, std::int32_t sizeBytes);
#else
using EntryPointFn = int (*)(void* args, std::int32_t sizeBytes);
#endif

// .NET 运行时宿主。一个进程一个实例（官方硬限制①）；析构时关闭 hostfxr context
// 并卸载 hostfxr.dll。
class Vm {
public:
    // 初始化运行时：runtimeConfigPath 指向入口工程 dotnet build 产出的
    // *.runtimeconfig.json（FDD 硬限制②）。失败返回 nullptr 并把阶段码填入 *result。
    static std::unique_ptr<Vm> create(const std::string& runtimeConfigPath, Result* result = nullptr);

    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // 加载程序集并取托管入口函数指针（默认签名）。
    //   assemblyPath  — 托管 DLL 绝对路径
    //   typeName      — 程序集限定类型名（"命名空间.类名, 程序集名"）
    //   methodName    — 公共静态方法名（须兼容默认签名，且带 [UnmanagedCallersOnly]）
    //   ⚠ 只许对**非收集**程序集使用（默认 ALC 装载，不可卸载）：函数指针指向
    //     collectible 程序集时 Unload 后即悬空。脚本热更走下面四件套。
    Result getEntryPoint(const std::string& assemblyPath,
                         const std::string& typeName,
                         const std::string& methodName,
                         EntryPointFn* out);

    // ---- M-SC3 热更四件套（collectible ALC，bootstrap 中转口径）----
    //
    // 结构（官方 collectible 口径）：hostfxr 只能把程序装配进默认 ALC（不可卸载），
    // collectible ALC 的创建/卸载必须托管侧完成 → 用一个**非收集 bootstrap 程序集**
    // （script/bootstrap/，常驻、其函数指针取用安全）中转：C++ 一次性取 bootstrap
    // 的 BootLoad/BootCall/BootUnload 三个函数指针；脚本程序集全部经 bootstrap
    // LoadFromStream 进 collectible ALC（不锁 DLL → dotnet build 同名覆盖可写），
    // 脚本方法经 bootstrap 反射 Invoke 调用（机制中转，绑定面仍是显式函数指针）。
    //
    // ABI 协议（与 script/bootstrap/Bootstrap.cs 注释同文，写死防漂移；x64，
    // C++ 栈上 POD 结构传地址，bootstrap 按偏移手读）：
    //   BootLoad args   = struct { const char* assemblyPathUtf8; }          // 偏移 0
    //   BootCall args   = struct { const char* typeUtf8;                    // 偏移 0
    //                              const char* methodUtf8;                  // 偏移 8
    //                              void* data;                              // 偏移 16
    //                              std::int32_t size; }                     // 偏移 24
    //   BootUnload args = nullptr
    //   BootTickPtr args = struct { const char* typeUtf8;               // 偏移 0
    //                               const char* methodUtf8;             // 偏移 8
    //                               void** outFn; }                     // 偏移 16
    //     （M-GL4：从 collectible 正本解析 [签名 int(IntPtr,int)] 静态方法的本机
    //      指针——不许 getEntryPoint 直拉脚本 dll，hostfxr 会装第二份进默认 ALC，
    //      静态字段独立一套，M-GL4 实证）
    //
    // 卸载次序铁律（ScriptHost 编排，Vm 只做单步）：先解绑一切指向旧 ALC 的
    // C++ 侧引用（脚本注册命令 unregisterCommand——reverse P/Invoke thunk 在
    // Unload 时被 CLR 注销，残留引用调用即崩），再 scriptUnload。

    // 一次性装载 bootstrap：取 BootLoad/BootCall/BootUnload 三函数指针
    //（bootstrapDllPath = script/bootstrap dotnet build 产出的 NlScriptBootstrap.dll）。
    Result loadBootstrap(const std::string& bootstrapDllPath);

    // 装载脚本程序集进新 collectible ALC（已有脚本未卸载 = code 8，协议 -3）。
    Result scriptLoad(const std::string& assemblyPath);

    // 经 bootstrap 反射调用当前 ALC 内脚本静态方法（args/size 原样透传）。
    // 托管异常经 bootstrap 归一为协议码 -102（code 8）；SEH 仅兜底。
    Result scriptCall(const std::string& typeName, const std::string& methodName,
                      void* args, std::int32_t sizeBytes, std::int32_t* ret);

    // 卸载当前 collectible ALC 并做 weakref 排空校验：*fullyUnloaded=false 表示
    // Unload 已调但 GC 未收干净（code 9；继续跑 GC 可能最终收掉，判据从严）。
    Result scriptUnload(bool* fullyUnloaded);

    // M-GL4：从当前 collectible 正本解析脚本静态方法本机指针（OnTick）。
    // *outFn 指向 collectible ALC 内代码——unload/reload 前调用方必须先置空
    // （指针生命周期铁律，ScriptHost 编排）。方法缺失 = code 8（调用方按
    // 「可选契约」吞掉 = tick 空转）。
    Result scriptTickFn(const std::string& typeName, const std::string& method,
                        void** outFn);

private:
    Vm();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::script
