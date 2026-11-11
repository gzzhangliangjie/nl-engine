#pragma once
// plugin_export.hpp — 插件 DLL 作者侧的 C ABI 导出宏（M-E5）
// 用法（插件源文件末尾一行）：
//     class MyPlugin : public nl::editor::ui::Plugin { ... };
//     NL_PLUGIN_EXPORTS(MyPlugin)
// 约定详见 plugin.hpp 头注释：同工具链编译 + nl_imgui_dll / nl_editor_common
# include "plugin.hpp"

#define NL_PLUGIN_EXPORTS(pluginClass)                                              \
    extern "C" __declspec(dllexport) int nl_plugin_abi() {                          \
        return nl::editor::ui::kPluginAbiVersion;                                   \
    }                                                                               \
    extern "C" __declspec(dllexport) nl::editor::ui::Plugin* nl_plugin_create() {   \
        return new pluginClass();                                                   \
    }                                                                               \
    extern "C" __declspec(dllexport) void nl_plugin_destroy(                        \
        nl::editor::ui::Plugin* p) {                                                \
        delete p;                                                                   \
    }
