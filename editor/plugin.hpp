#pragma once
// plugin.hpp — 编辑器插件扩展点（进程内注册 + M-E5 plugins/ 目录 DLL 动态扫描）
// 隔离原则（对齐 Sunshine PluginManager）：插件只通过 RpcHost 与引擎交互，互不 import。
//
// M-E5 动态加载约定（C ABI，见 plugin_export.hpp 的 NL_PLUGIN_EXPORTS 宏）：
//   每个插件 DLL 必须导出三个 C 符号：
//     int  nl_plugin_abi()                      — 返回 kPluginAbiVersion，不符则拒载
//     Plugin* nl_plugin_create()                — 堆上构造插件实例
//     void nl_plugin_destroy(Plugin*)           — 在 DLL 内部 delete（跨模块 new/delete 必须配对）
//   ABI 前提：DLL 与宿主同仓同 MSVC 工具链编译，且链接 nl_imgui_dll（共享
//   同一 ImGuiContext）与 nl_editor_common（共享 RpcHost 实现）。库内有任一
//   静态副本即双 ImGuiContext / 状态分裂，属必炸路径。
#include "rpc_host.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// M-E8：引擎 sceneChanged 推送的共享 dirty 旗标——editor/main.cpp 的
// registerNotify("editor.sceneChanged") 置 true，插件每帧消费并复位。
// RPC host 回调跑在主线程 poll 点，无跨线程竞争，atomic 仅作防御。
extern std::atomic<bool> g_sceneTreeDirty;

namespace nl::editor::ui {

// M-E5：插件 ABI 版本。布局/语义破坏性变更时 +1，宿主对不匹配 DLL 拒载并记日志。
inline constexpr int kPluginAbiVersion = 1;

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual const char* name() const = 0;
    // 每帧绘制插件面板（ImGui）。host 可空（无引擎时禁用交互）。
    virtual void draw(nl::editor::host::RpcHost* host) = 0;
};

class PluginManager {
public:
    ~PluginManager();   // M-E5：DLL 插件先 destroy（DLL 内 delete）再 FreeLibrary

    // 进程内自注册（内建插件）。
    void registerPlugin(std::unique_ptr<Plugin> p);
    // M-E0：注册内建样例插件（无需引擎也可启动）。
    void discover();
    // M-E5：扫描 <dir>/*.dll 动态加载（C ABI 校验，失败跳过并经 log 上报）。
    void discoverDlls(const std::string& dir, const std::function<void(const char*)>& log);
    void drawAll(nl::editor::host::RpcHost* host);

private:
    struct DllEntry {
        void*  handle  = nullptr;             // HMODULE
        void (*destroy)(Plugin*) = nullptr;   // DLL 导出的 nl_plugin_destroy
        size_t slot    = 0;                   // plugins_ 中的下标（析构按 slot 逆序摘除）
    };
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<DllEntry> dlls_;
};

} // namespace nl::editor::ui
