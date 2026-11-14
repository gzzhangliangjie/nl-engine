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

    // ---- M-GL4：宿主级脚本 OnTick（逻辑帧驱动）----
    //
    // 口径来源（三处对照，2026-11-10 留痕）：
    //   - Carrot（抄结构不抄代码）：ecs/systems/CSharpLogicSystem——宿主按方法名
    //     拉脚本入口（findMethod "Tick"/"PrePhysicsTick"/"FirstTick"），C# 侧只
    //     约定名字、无注册魔法；热更 = 程序集装载回调后重新 findMethod。对应物 =
    //     Vm::scriptTickFn（bootstrap BootTickPtr 从 collectible 正本反射解析，
    //     delegate thunk 带本机指针），装载期取一次、reload 后重取。
    //     **禁 getEntryPoint 直拉脚本 dll**：hostfxr 会把程序集再装一份进默认
    //     ALC（静态字段独立一套，Engine.Init 状态不通——M-GL4 实证默认副本
    //     OnTick 读 _exists=0 崩）。
    //   - UE（只读对照）：脚本逻辑经 Actor Tick 由 TickTaskGraph 按逻辑帧驱动，
    //     tick 组排序属引擎调度面——本库单脚本无排序需求（P8-SCRIPT-PLAN M-SC2b
    //     per-entity ScriptComponent 维持「默认不做」，此处是宿主级单脚本，不冲突）。
    //   - bevy：无脚本 crate，据实标注无对应物。
    //
    // 指针生命周期铁律（与脚本命令 reverse thunk 同族）：tickFn_ 指向 collectible
    // ALC 内代码，unload 前必须先置空（「先解绑一切指向旧 ALC 的 C++ 侧引用，
    // 再 scriptUnload」）；loadScript/reloadScript Entry 成功后重取。OnTick 为
    // **可选契约**：缺失 = tickFn_ 空 = 宿主 tick 空转（旧脚本零波及——M-SC0~SC3
    // 的 hello/console/ecs 均无 OnTick，必须兼容；要接逻辑帧的脚本声明
    // public static unsafe int OnTick(IntPtr args, int sizeBytes)——经
    // BootTickPtr 委托 thunk 带出，无需 [UnmanagedCallersOnly]）。
    //
    // 脚本失败停用口径：OnTick 返回非 0 / SEH 异常 → 记一次失败 + 停用 tick
    //（防每帧刷屏），进程存活；作者修好源码 dotnet build 后 reload-script 恢复
    //（UE 脚本异常不停引擎同口径）。

    // 逻辑帧回调：宿主每个逻辑 tick 调一次（clock callback 内、world.tick 之前
    // ——脚本 spawn/remove 排队，tick 开头 flush 落盘，World「宿主 tick 边界
    // 动作」既有语义）。args = 指向 double dt（逻辑步长秒，kStep），size=8。
    // 无脚本/未启用 = 成功空操作。
    Result scriptTick(double dt);

    bool hasTick() const { return tickFn_ != nullptr; }
    bool tickEnabled() const { return tickEnabled_; }
    unsigned long long tickCount() const { return tickCount_; }

    // ---- M-SC10：props 可选契约通道（PropGet/PropSet）----
    //
    // BootTickPtr 协议复用（按名解析 public static int(IntPtr,int)，collectible
    // 正本 + thunk 保活同 OnTick）；生命周期纪律逐位同 tickFn_：缺失 = 通道惰性
    //（面板空转，旧脚本零波及）；load/reload Entry 成功后重取；unload 前置空。
    // 值 ABI（script-api PropsPush 同文锚）：buf[0..3]=int32 idx（本包装填），
    // buf[4..] 值区——标量 size=8；string size=260（UTF8 尾 0，get 截断 255）。
    // rc：ok / 7=init 未调 / 8=通道未接或脚本侧返回非 0 / 6=SEH / 2=BadArg。
    bool hasProps() const { return propsGetFn_ != nullptr && propsSetFn_ != nullptr; }
    Result propGet(std::int32_t idx, void* buf, std::int32_t size);
    Result propSet(std::int32_t idx, const void* buf, std::int32_t size);

private:
    ScriptHost() = default;

    Vm* vm_ = nullptr;
    std::string bootstrapDll_;
    bool commandRegistered_ = false;   // console.reload-script 幂等闸
    std::string scriptDll_;
    std::string scriptType_;
    EntryPointFn tickFn_ = nullptr;    // 脚本 OnTick（bootstrap BootTickPtr 带出；unload 前置空）
    EntryPointFn propsGetFn_ = nullptr;  // M-SC10：PropGet/PropSet（BootTickPtr 协议复用，同纪律）
    EntryPointFn propsSetFn_ = nullptr;
    void refreshPropsFn_(const std::string& typeName);  // Entry 成功后重取（缺失 = 静默置空）
    bool tickEnabled_ = false;         // 脚本失败停用闸
    unsigned long long tickCount_ = 0; // 宿主侧 tick 计数（机器证据 / game.script）
};

} // namespace nl::script
