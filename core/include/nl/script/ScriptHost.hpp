#pragma once
// M-SC3：脚本热更宿主编排（P8-SCRIPT-PLAN.md M-SC3 定稿）。
//
// 职责（单一）：把 Vm 热更四件套 + Bindings 脚本命令登记编排成一次安全的
// 换载，并向 console 注册 `console.reload-script` 命令（生产宿主 / 工具同款入口）。
//
// 口径来源（三处对照，2026-11-09 留痕）：
//   - Carrot（抄结构不抄底座）：热更重绑定 = registerGameAssemblyLoadCallback /
//     UnloadCallback 程序集换载时命令/组件重 init——对应物 = scriptUnregisterAll()
//     + 新 ALC 装载后重调脚本 Entry 重注册（C++ 侧不动，脚本自描述重绑定）。
//   - UE（只读对照）：Live Coding/热更由模块管理器承载，控制台无脚本热更命令；
//     `console.reload-script` 与 r.* cvar 同定位（引擎开关面），据实标注无对应物。
//   - bevy：无脚本 crate，据实标注无对应物。
//
// 热更安全根基（UE/鸣潮共同口径）：状态全在 C++——reload 不碰 cvar 值 / 实体树 /
// 内建命令，只换脚本程序集。卸载次序铁律（Vm.hpp 口径）：先 scriptUnregisterAll()
// 解除对旧 ALC thunk 的全部引用，再 vm->scriptUnload()。
//
// 纪律：core 零图形 API；单例（.NET 单运行时硬限制 → 每进程一个脚本宿主）；
// 命令本体只依赖 Vm/Bindings/console，无 hostfxr 符号（PIMPL 纪律不破）。

#include <string>

#include "nl/script/Vm.hpp"

namespace nl::script {

class ScriptHost {
public:
    // 进程单例（Vm 同款硬限制口径）。
    static ScriptHost& get();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // 幂等初始化：装载 bootstrap（Vm::loadBootstrap）+ 注册 console.reload-script
    // 命令。重复调用只刷新 vm 指针 / bootstrap 路径，不重复注册命令。
    // bootstrapDllPath = script/bootstrap dotnet build 产出的 NlScriptBootstrap.dll。
    Result init(Vm* vm, const std::string& bootstrapDllPath);

    // 装载脚本（若已有脚本先安全卸载）+ 调脚本 Entry 传入 ApiTable*（重绑定 =
    // 新脚本自己重新注册命令 / 设 cvar，脚本自描述口径）。记录路径供 reload。
    // typeName 默认约定按调用方给出（工具 = NlScript.Examples.<X>Script）。
    Result loadScript(const std::string& scriptDllPath, const std::string& typeName,
                      const std::string& entryMethod = "Entry");

    // 热重载当前脚本：scriptUnregisterAll（解绑旧命令）→ scriptUnload（weakref
    // 校验）→ 同路径重新装载 + Entry 重注册。*outInfo 返回人类可读摘要
    //（解绑命令数与名单、卸载判据、重装载结果）。
    Result reloadScript(std::string* outInfo = nullptr);

    // 卸载当前脚本（不解绑 World / cvar / 内建命令）。*fullyUnloaded = weakref 校验。
    Result unloadScript(bool* fullyUnloaded = nullptr);

    bool hasScript() const { return !scriptDll_.empty(); }
    const std::string& scriptDll() const { return scriptDll_; }
    const std::string& scriptType() const { return scriptType_; }

private:
    ScriptHost() = default;

    Vm* vm_ = nullptr;
    std::string bootstrapDll_;
    bool commandRegistered_ = false;   // console.reload-script 幂等闸
    std::string scriptDll_;
    std::string scriptType_;
};

} // namespace nl::script
