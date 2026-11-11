// hello_plugin.cpp — M-E5 示例插件 DLL（plugins/ 目录动态扫描加载的验收物）
// 独立编译成 SHARED 库落 plugins/，宿主 discoverDlls() 扫描加载。
// 必链 nl_imgui_dll（共享 ImGuiContext）与 nl_editor_common（共享 RpcHost 实现）。
#include "plugin_export.hpp"
#include <imgui.h>

using nl::editor::host::RpcHost;
using nl::editor::host::RpcValue;
using nl::editor::host::RpcError;

class HelloPlugin : public nl::editor::ui::Plugin {
public:
    const char* name() const override { return "Hello (DLL)"; }
    void draw(RpcHost* host) override {
        ImGui::TextUnformatted("loaded from plugins\\hello_plugin.dll (dynamic scan)");
        const bool up = host && host->isConnected();
        ImGui::Text("engine: %s", up ? "connected" : "offline");
        if (up && ImGui::Button("engine.echo test")) {
            nl::editor::msgpack::Map m;
            m["msg"] = std::string("hello from dll plugin");
            // M-E6：与内建插件同口径，500ms 快速失败，避免卡住编辑器渲染线程
            auto r = host->call("engine.echo", RpcValue(std::move(m)), 500);
            if (std::holds_alternative<RpcValue>(r)) {
                const auto& v = std::get<RpcValue>(r);
                if (v.isMap()) {
                    auto it = v.asMap().find("msg");
                    lastEcho_ = (it != v.asMap().end() && it->second.isStr())
                                    ? it->second.asStr() : "(bad reply)";
                    echoOk_ = true;
                }
            } else {
                const auto& e = std::get<RpcError>(r);
                lastEcho_ = "error " + std::to_string(e.code) + ": " + e.message;
                echoOk_ = false;
            }
        }
        if (!lastEcho_.empty())
            ImGui::Text("echo: %s", lastEcho_.c_str());
    }

private:
    std::string lastEcho_;
    bool echoOk_ = false;
};

NL_PLUGIN_EXPORTS(HelloPlugin)
