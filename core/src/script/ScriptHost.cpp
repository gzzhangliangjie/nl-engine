// M-SC3：ScriptHost 实现（口径/编排次序铁律见 nl/script/ScriptHost.hpp 文件头）。
//
// `console.reload-script` 命令：无参 = 热重载当前脚本（同名 DLL 覆盖工作流）；
// 带 1 个参数 = 换路径装载（typeName 沿用当前记录或按默认约定推导——类型名
// 由宿主记录，路径可换）。输出走 CommandResult::output（进 console 环形日志）。

#include "nl/script/ScriptHost.hpp"

#include "nl/core/console.hpp"
#include "nl/script/Bindings.hpp"

#include <windows.h>   // SEH __except（Vm.cpp 同口径；EXCEPTION_EXECUTE_HANDLER）
#include <cstdio>      // std::snprintf（SEH 异常码格式化）

namespace nl::script {

namespace {
// M-GL4：SEH 隔离壳（Vm.cpp 同口径——MSVC /EHsc 下 C++ catch(...) 接不住
// 0xE0434352，且 __try 所在函数不许有需要展开的 C++ 对象，故独立自由函数）。
// 哨兵码 = SEH 命中（负值域，脚本作者返回值约定 0/正数）。
constexpr std::int32_t kTickSehSentinel = -2000000;
std::int32_t callTickFn(EntryPointFn fn, void* args, std::int32_t size,
                        std::uint32_t* outExcCode) {
    if (outExcCode) *outExcCode = 0;
    __try {
        return fn(args, size);
    } __except (outExcCode ? (*outExcCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
                           : EXCEPTION_EXECUTE_HANDLER) {
        return kTickSehSentinel;
    }
}
} // namespace

ScriptHost& ScriptHost::get() {
    static ScriptHost host;
    return host;
}

Result ScriptHost::init(Vm* vm, const std::string& bootstrapDllPath) {
    if (!vm) return Result::fail(7, "ScriptHost::init: null vm");
    vm_ = vm;
    bootstrapDll_ = bootstrapDllPath;

    Result r = vm->loadBootstrap(bootstrapDllPath);
    if (!r.ok) return r;

    if (!commandRegistered_) {
        // 命令注册（一次性；lambda 只捕获 this 所持单例指针，与进程同生命周期）。
        nl::core::console::Console::get().registerCommand(
            "console.reload-script",
            "reload current script DLL (collectible ALC swap; state stays in C++)",
            [](const std::vector<std::string>& args) -> nl::core::console::CommandResult {
                ScriptHost& host = get();
                std::string info;
                if (!args.empty()) {
                    // 带参：换路径装载（typeName 沿用记录；首次带参装载脚本 = 先有类型名，
                    // 故换路径场景要求已有脚本在载——首次装载走宿主侧 loadScript API）。
                    if (!host.hasScript())
                        return {false, "reload-script: no script loaded yet (host must loadScript first)"};
                    host.scriptDll_ = args[0];
                }
                if (!host.hasScript())
                    return {false, "reload-script: no script loaded"};
                const Result r = host.reloadScript(&info);
                return {r.ok, r.ok ? info : "reload-script: " + r.message};
            });
        commandRegistered_ = true;
    }
    return Result::success();
}

Result ScriptHost::loadScript(const std::string& scriptDllPath, const std::string& typeName,
                              const std::string& entryMethod) {
    if (!vm_) return Result::fail(7, "ScriptHost: init not called");

    // 已有脚本先安全卸载（load 语义 = 幂等换载；reload 路径复用同一编排）。
    if (hasScript()) {
        bool fully = false;
        // 指针生命周期铁律：tickFn_ 指向旧 ALC 内代码，unload 前先解绑。
        tickFn_ = nullptr;
        tickEnabled_ = false;
        scriptUnregisterAll();
        Result ur = vm_->scriptUnload(&fully);
        if (!ur.ok) return ur;
    }

    Result lr = vm_->scriptLoad(scriptDllPath);
    if (!lr.ok) return lr;

    // 重绑定：脚本 Entry 收 ApiTable*（M-SC1/M-SC2 口径）——新脚本自己重新
    // 注册命令 / 设 cvar（脚本自描述重绑定，Carrot load-callback 同构）。
    std::int32_t ret = 0;
    void* argPtr = const_cast<void*>(static_cast<const void*>(consoleApiTable()));
    Result cr = vm_->scriptCall(typeName, entryMethod, argPtr, 0, &ret);
    if (!cr.ok) {
        // 装载成功但 Entry 失败：脚本命令可能注册了一半——安全卸载回滚，不留半态。
        tickFn_ = nullptr;
        tickEnabled_ = false;
        scriptUnregisterAll();
        bool fully = false;
        vm_->scriptUnload(&fully);
        return cr;
    }
    if (ret != 0) {
        // Entry 返回非 0 = 脚本自报初始化失败（如目标实体不存在）——同样回滚，
        // 不允许「loaded 但半初始化」假绿态（M-GL4 实证：旧版吞掉返回值）。
        tickFn_ = nullptr;
        tickEnabled_ = false;
        scriptUnregisterAll();
        bool fully = false;
        vm_->scriptUnload(&fully);
        return Result::fail(8, "loadScript: script Entry returned " + std::to_string(ret) +
                                   " (rollback; type=" + typeName + ")");
    }

    // M-GL4：拉 OnTick——**必须经 bootstrap BootTickPtr**（collectible 正本）。
    // 禁 vm_->getEntryPoint 直拉脚本 dll：hostfxr 会把程序集再装一份进默认
    // ALC（静态字段独立一套，Engine.Init 状态不通——实证默认副本 OnTick 读
    // _exists=0 崩）。**可选契约**：缺失 = tickFn_ 空 = 宿主 tick 空转
    //（旧脚本零波及——M-SC0~SC3 的 hello/console/ecs 均无 OnTick，必须兼容）。
    tickFn_ = nullptr;
    void* tfn = nullptr;
    if (vm_->scriptTickFn(typeName, "OnTick", &tfn).ok && tfn) {
        tickFn_ = reinterpret_cast<EntryPointFn>(tfn);
        tickEnabled_ = true;
    } else {
        tickFn_ = nullptr;
        tickEnabled_ = false;
    }
    tickCount_ = 0;

    scriptDll_ = scriptDllPath;
    scriptType_ = typeName;
    return Result::success();
}

Result ScriptHost::reloadScript(std::string* outInfo) {
    if (!vm_) return Result::fail(7, "ScriptHost: init not called");
    if (!hasScript()) return Result::fail(8, "reloadScript: no script loaded");

    // 次序铁律（ScriptHost.hpp）：先解绑（旧 ALC thunk 引用清零）→ 再 unload。
    const auto unbound = scriptUnregisterAll();
    tickFn_ = nullptr;            // M-GL4：tick 指针与命令 thunk 同生命周期纪律
    tickEnabled_ = false;
    bool fully = false;
    Result ur = vm_->scriptUnload(&fully);
    if (!ur.ok) return ur;

    Result lr = vm_->scriptLoad(scriptDll_);
    if (!lr.ok) return lr;

    std::int32_t ret = 0;
    void* argPtr = const_cast<void*>(static_cast<const void*>(consoleApiTable()));
    Result cr = vm_->scriptCall(scriptType_, "Entry", argPtr, 0, &ret);
    if (!cr.ok) {
        scriptUnregisterAll();
        bool fully2 = false;
        vm_->scriptUnload(&fully2);
        scriptDll_.clear();
        scriptType_.clear();
        return cr;
    }

    // M-GL4：reload 后重取 OnTick（新 ALC 新指针；缺失 = 无 tick，旧脚本兼容）。
    tickFn_ = nullptr;
    if (vm_->getEntryPoint(scriptDll_, scriptType_, "OnTick", &tickFn_).ok && tickFn_) {
        tickEnabled_ = true;
    } else {
        tickFn_ = nullptr;
        tickEnabled_ = false;
    }
    tickCount_ = 0;

    if (outInfo) {
        std::string names;
        for (size_t i = 0; i < unbound.size(); ++i) {
            if (i) names += ", ";
            names += unbound[i];
        }
        *outInfo = "reload-script: unbound " + std::to_string(unbound.size()) + " command(s)" +
                   (names.empty() ? "" : " [" + names + "]") +
                   ", ALC fully unloaded (weakref dead), reloaded " + scriptDll_ +
                   " (Entry ret=" + std::to_string(ret) + ")";
    }
    return Result::success();
}

Result ScriptHost::unloadScript(bool* fullyUnloaded) {
    if (!vm_) return Result::fail(7, "ScriptHost: init not called");
    if (fullyUnloaded) *fullyUnloaded = false;
    if (!hasScript()) return Result::fail(8, "unloadScript: no script loaded");

    const auto unbound = scriptUnregisterAll();   // 次序铁律同 reload
    tickFn_ = nullptr;                            // M-GL4：unload 前先解绑 tick 指针
    tickEnabled_ = false;
    bool fully = false;
    Result ur = vm_->scriptUnload(&fully);
    if (!ur.ok) return ur;
    if (fullyUnloaded) *fullyUnloaded = fully;
    (void)unbound;
    scriptDll_.clear();
    scriptType_.clear();
    return Result::success();
}

Result ScriptHost::scriptTick(double dt) {
    if (!vm_) return Result::fail(7, "ScriptHost: init not called");
    if (!hasScript() || !tickFn_ || !tickEnabled_) return Result::success(); // 成功空操作

    std::uint32_t excCode = 0;
    const std::int32_t ret = callTickFn(tickFn_, &dt, sizeof(dt), &excCode);
    if (ret == kTickSehSentinel) {
        tickEnabled_ = false;
        char ebuf[48];
        std::snprintf(ebuf, sizeof ebuf, "0x%08x", excCode);
        return Result::fail(8, std::string("scriptTick: SEH exception in OnTick (code=") +
                                   ebuf + "; tick disabled; reload-script to recover)");
    }
    if (ret != 0) {
        tickEnabled_ = false;
        return Result::fail(8, "scriptTick: OnTick returned " + std::to_string(ret) +
                                   " (tick disabled; reload-script to recover)");
    }
    ++tickCount_;
    return Result::success();
}

} // namespace nl::script
