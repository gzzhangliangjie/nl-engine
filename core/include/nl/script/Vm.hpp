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
// 本类只负责「装载运行时 + 取托管函数指针」，collectible ALC 卸载在 M-SC3 扩展。
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
    Result getEntryPoint(const std::string& assemblyPath,
                         const std::string& typeName,
                         const std::string& methodName,
                         EntryPointFn* out);

private:
    Vm();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::script
