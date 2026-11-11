#pragma once
// plugin.hpp — 编辑器插件扩展点（进程内注册；M-E3 起接 RPC 驱动引擎）
// 隔离原则（对齐 Sunshine PluginManager）：插件只通过 RpcHost 与引擎交互，互不 import。
#include "rpc_host.hpp"
#include <memory>
#include <string>
#include <vector>

namespace nl::editor::ui {

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual const char* name() const = 0;
    // 每帧绘制插件面板（ImGui）。host 可空（无引擎时禁用交互）。
    virtual void draw(nl::editor::host::RpcHost* host) = 0;
};

class PluginManager {
public:
    // 进程内自注册（M-E3 将扩展为扫描 plugins/ 动态加载 .dll）。
    void registerPlugin(std::unique_ptr<Plugin> p);
    // M-E0：注册内建样例插件（无需引擎即可启动）。
    void discover();
    void drawAll(nl::editor::host::RpcHost* host);

private:
    std::vector<std::unique_ptr<Plugin>> plugins_;
};

} // namespace nl::editor::ui
