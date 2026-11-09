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

#include "nl/script/Vm.hpp"

#include "coreclr_delegates.h"
#include "hostfxr.h"
#include "nethost.h"

#include <cstdio>
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

} // namespace nl::script
