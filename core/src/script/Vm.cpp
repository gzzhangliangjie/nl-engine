// M-SC0：Vm 实现 —— hostfxr/nethost 三导出函数口径（docs/p8/P8-SCRIPT-PLAN.md）。
// hostfxr.h / nethost.h / coreclr_delegates.h 只在本 TU 出现（PIMPL 纪律，见 Vm.hpp）。
//
// 流程（Microsoft Learn《Write a custom .NET runtime host》官方口径）：
//   1. nethost 的 get_hostfxr_path 定位 hostfxr.dll（环境变量 / 全局注册位置）
//   2. LoadLibraryW + GetProcAddress 拿三个导出函数
//   3. hostfxr_initialize_for_runtime_config(*.runtimeconfig.json)（FDD）
//   4. hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer)
//   5. 析构：hostfxr_close + FreeLibrary
// getEntryPoint = load_assembly_and_get_function_pointer(UNMANAGEDCALLERSONLY_METHOD)，
// 返回的指针即默认签名 component_entry_point_fn（int(void*, int32)）。
//
// M-SC3 热更四件套（collectible ALC，bootstrap 中转）：ABI 协议与实现注释见
// Vm.hpp（同文锚在 script/bootstrap/Bootstrap.cs）。核心结构：loadBootstrap 经
// getEntryPoint 取 bootstrap（非收集，常驻安全）的 BootLoad/BootCall/BootUnload
// 三函数指针；脚本程序集经 bootstrap LoadFromStream 进 collectible ALC，其函数
// 指针一律不外泄（Unload 后悬空）。

#include "nl/script/Vm.hpp"

#include "coreclr_delegates.h"
#include "hostfxr.h"
#include "nethost.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

namespace nl::script {
namespace {

// UTF-8 std::string -> UTF-16 std::wstring（Windows char_t = wchar_t）。
std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// 托管入口调用的 SEH 边界：托管异常在 CLR->native 交界处表现为 SEH 异常
// （0xE0434352 等）。MSVC /EHsc 下 C++ catch(...) 接不住，必须 __except。
// 此函数内不许有任何需要展开的 C++ 对象（__try 约束）。
int callEntryGuarded(EntryPointFn fn, void* args, std::int32_t sizeBytes, std::int32_t* ret,
                     std::uint32_t* exceptionCode) {
    __try {
        *ret = fn(args, sizeBytes);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionCode = GetExceptionCode();
        return 1;
    }
}

} // namespace

struct Vm::Impl {
    // hostfxr 三导出（GetProcAddress，不做运行期强签名校验以外的假设）
    hostfxr_initialize_for_runtime_config_fn initForConfig = nullptr;
    hostfxr_get_runtime_delegate_fn getRuntimeDelegate = nullptr;
    hostfxr_close_fn close = nullptr;

    HMODULE hostfxrModule = nullptr;
    hostfxr_handle context = nullptr;
    load_assembly_and_get_function_pointer_fn loadAndGetFn = nullptr;

    // M-SC3 热更：bootstrap（非收集程序集）三函数指针，loadBootstrap 填充。
    EntryPointFn bootLoad = nullptr;
    EntryPointFn bootCall = nullptr;
    EntryPointFn bootUnload = nullptr;
};

Vm::Vm() = default;

Vm::~Vm() {
    if (!impl_) return;
    if (impl_->context && impl_->close) impl_->close(impl_->context);
    if (impl_->hostfxrModule) FreeLibrary(impl_->hostfxrModule);
}

std::unique_ptr<Vm> Vm::create(const std::string& runtimeConfigPath, Result* result) {
    auto setResult = [result](Result r) {
        if (result) *result = std::move(r);
    };

    // 1. nethost：定位 hostfxr.dll
    wchar_t fxrPath[MAX_PATH];
    size_t fxrPathSize = MAX_PATH;
    if (get_hostfxr_path(fxrPath, &fxrPathSize, nullptr) != 0) {
        setResult(Result::fail(1, "get_hostfxr_path failed (dotnet runtime not found via env/global registration)"));
        return nullptr;
    }

    auto vm = std::unique_ptr<Vm>(new Vm());
    vm->impl_ = std::make_unique<Impl>();

    // 2. LoadLibrary + 三个导出函数
    vm->impl_->hostfxrModule = LoadLibraryW(fxrPath);
    if (!vm->impl_->hostfxrModule) {
        setResult(Result::fail(2, "LoadLibraryW(hostfxr) failed: path=" +
                                    std::string(fxrPath, fxrPath + wcslen(fxrPath))));
        return nullptr;
    }
    vm->impl_->initForConfig = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(vm->impl_->hostfxrModule, "hostfxr_initialize_for_runtime_config"));
    vm->impl_->getRuntimeDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(vm->impl_->hostfxrModule, "hostfxr_get_runtime_delegate"));
    vm->impl_->close = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(vm->impl_->hostfxrModule, "hostfxr_close"));
    if (!vm->impl_->initForConfig || !vm->impl_->getRuntimeDelegate || !vm->impl_->close) {
        setResult(Result::fail(2, "GetProcAddress(hostfxr exports) failed"));
        return nullptr;
    }

    // 3. 初始化运行时（FDD：runtimeconfig.json）
    const std::wstring wConfig = toWide(runtimeConfigPath);
    const char_t* config = wConfig.c_str();
    const int rc = vm->impl_->initForConfig(config, /*parameters*/ nullptr, &vm->impl_->context);
    if (rc != 0 || !vm->impl_->context) {
        setResult(Result::fail(3, "hostfxr_initialize_for_runtime_config failed: hresult=0x" +
                                      [&] {
                                          char buf[16];
                                          std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(rc));
                                          return std::string(buf);
                                      }() +
                                      " config=" + runtimeConfigPath));
        return nullptr;
    }

    // 4. 取 load_assembly_and_get_function_pointer delegate
    const int drc = vm->impl_->getRuntimeDelegate(
        vm->impl_->context, hdt_load_assembly_and_get_function_pointer,
        reinterpret_cast<void**>(&vm->impl_->loadAndGetFn));
    if (drc != 0 || !vm->impl_->loadAndGetFn) {
        setResult(Result::fail(4, "hostfxr_get_runtime_delegate(hdt_load_assembly...) failed: hresult=0x" +
                                      [&] {
                                          char buf[16];
                                          std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(drc));
                                          return std::string(buf);
                                      }()));
        return nullptr;
    }

    setResult(Result::success());
    return vm;
}

Result Vm::getEntryPoint(const std::string& assemblyPath, const std::string& typeName,
                         const std::string& methodName, EntryPointFn* out) {
    if (!impl_ || !impl_->loadAndGetFn)
        return Result::fail(5, "Vm not initialized");

    void* fn = nullptr;
    const int rc = impl_->loadAndGetFn(toWide(assemblyPath).c_str(), toWide(typeName).c_str(),
                                       toWide(methodName).c_str(), UNMANAGEDCALLERSONLY_METHOD,
                                       nullptr, &fn);
    if (rc != 0 || !fn) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(rc));
        return Result::fail(5, "load_assembly_and_get_function_pointer failed: hresult=0x" +
                                   std::string(buf) + " assembly=" + assemblyPath +
                                   " type=" + typeName + " method=" + methodName);
    }
    *out = reinterpret_cast<EntryPointFn>(fn);
    return Result::success();
}

// ---- M-SC3 热更四件套 ----

// BootCall 协议结构（与 Vm.hpp / Bootstrap.cs 注释同文；POD，栈上传地址，
// bootstrap 按偏移手读：0=typeUtf8 / 8=methodUtf8 / 16=data / 24=size）。
struct BootCallArgs {
    const char* typeUtf8;
    const char* methodUtf8;
    void* data;
    std::int32_t size;
};
static_assert(sizeof(BootCallArgs) == 32, "BootCallArgs ABI: x64 layout pinned by Bootstrap.cs offsets");

Result Vm::loadBootstrap(const std::string& bootstrapDllPath) {
    if (!impl_) return Result::fail(7, "Vm not initialized");

    // bootstrap 三入口逐个取用（非收集程序集 → 指针常驻安全，Vm.hpp 口径）。
    struct EntrySpec { const char* method; EntryPointFn* slot; const char* what; };
    const EntrySpec specs[] = {
        {"BootLoad",   &impl_->bootLoad,   "BootLoad"},
        {"BootCall",   &impl_->bootCall,   "BootCall"},
        {"BootUnload", &impl_->bootUnload, "BootUnload"},
    };
    for (const auto& spec : specs) {
        Result r = getEntryPoint(bootstrapDllPath, "Nl.Script.Bootstrap.Bootstrap, NlScriptBootstrap",
                                 spec.method, spec.slot);
        if (!r.ok) {
            r.message = "bootstrap " + std::string(spec.what) + ": " + r.message;
            return r;
        }
    }
    return Result::success();
}

Result Vm::scriptLoad(const std::string& assemblyPath) {
    if (!impl_ || !impl_->bootLoad) return Result::fail(7, "bootstrap not loaded (call loadBootstrap first)");

    // BootLoad args = struct { const char* assemblyPathUtf8; }（偏移 0；
    // 传结构体地址——args 本身不是字符串指针，协议见 Vm.hpp）
    struct BootLoadArgs { const char* assemblyPathUtf8; } load{assemblyPath.c_str()};
    static_assert(sizeof(BootLoadArgs) == 8, "BootLoadArgs ABI: x64 layout pinned by Bootstrap.cs offset 0");
    std::int32_t ret = 0;
    std::uint32_t excCode = 0;
    if (callEntryGuarded(impl_->bootLoad, &load, 0, &ret, &excCode) != 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", excCode);
        return Result::fail(6, "scriptLoad: managed exception at SEH boundary (code=0x" + std::string(buf) + ")");
    }
    if (ret != 0) {
        const char* why = ret == -1 ? "assembly path missing/unreadable"
                        : ret == -2 ? "load failed (stream read / dependency resolution)"
                        : ret == -3 ? "previous script not unloaded (call scriptUnload first)"
                                    : "unknown protocol code";
        return Result::fail(8, "scriptLoad: bootstrap BootLoad returned " + std::to_string(ret) +
                                   " (" + why + ") assembly=" + assemblyPath);
    }
    return Result::success();
}

Result Vm::scriptCall(const std::string& typeName, const std::string& methodName,
                      void* args, std::int32_t sizeBytes, std::int32_t* ret) {
    if (!impl_ || !impl_->bootCall) return Result::fail(7, "bootstrap not loaded (call loadBootstrap first)");

    BootCallArgs call{typeName.c_str(), methodName.c_str(), args, sizeBytes};
    std::int32_t result = 0;
    std::uint32_t excCode = 0;
    if (callEntryGuarded(impl_->bootCall, &call, sizeof(call), &result, &excCode) != 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", excCode);
        return Result::fail(6, "scriptCall: managed exception at SEH boundary (code=0x" + std::string(buf) + ")");
    }
    if (result == -100 || result == -101 || result == -102) {
        const char* why = result == -100 ? "no script loaded"
                        : result == -101 ? "type or method resolution failed"
                                         : "managed exception inside script (caught at bootstrap)";
        return Result::fail(8, "scriptCall: bootstrap BootCall returned " + std::to_string(result) +
                                   " (" + why + ") type=" + typeName + " method=" + methodName);
    }
    if (ret) *ret = result;
    return Result::success();
}

Result Vm::scriptUnload(bool* fullyUnloaded) {
    if (!impl_ || !impl_->bootUnload) return Result::fail(7, "bootstrap not loaded (call loadBootstrap first)");
    if (fullyUnloaded) *fullyUnloaded = false;

    std::int32_t ret = 0;
    std::uint32_t excCode = 0;
    if (callEntryGuarded(impl_->bootUnload, nullptr, 0, &ret, &excCode) != 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", excCode);
        return Result::fail(6, "scriptUnload: managed exception at SEH boundary (code=0x" + std::string(buf) + ")");
    }
    if (ret == -1) return Result::fail(8, "scriptUnload: bootstrap BootUnload returned -1 (no script loaded)");
    if (ret == 1) {
        return Result::fail(9, "scriptUnload: ALC Unload called but GC has not fully collected "
                               "(weakref alive after drain loop)");
    }
    if (ret != 0) {
        return Result::fail(8, "scriptUnload: bootstrap BootUnload returned " + std::to_string(ret) +
                                   " (unknown protocol code)");
    }
    if (fullyUnloaded) *fullyUnloaded = true;
    return Result::success();
}

} // namespace nl::script
