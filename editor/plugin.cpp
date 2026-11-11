// plugin.cpp — 编辑器插件加载器（进程内注册 + 内建样例）
#include "plugin.hpp"
#include <imgui.h>

namespace nl::editor::ui {

// 复用编辑器宿主侧的类型别名（位于兄弟命名空间 nl::editor::host）
using nl::editor::host::RpcHost;
using nl::editor::host::RpcValue;
using nl::editor::host::RpcError;

// 内建样例：相机工具桩（M-E3 替换为真实相机控制；此处仅演示「插件 → RPC → 引擎」链路）。
class CameraStubPlugin : public Plugin {
public:
    const char* name() const override { return "Camera (stub)"; }
    void draw(RpcHost* host) override {
        ImGui::Text("Camera tool stub — drives engine via RPC (M-E3).");
        if (ImGui::Button("Ping engine (echo)") && host) {
            auto r = host->call("engine.echo", msgpack::Value(std::string("ping")));
            if (std::holds_alternative<RpcValue>(r)) {
                auto& v = std::get<RpcValue>(r);
                last_ = v.isStr() ? v.asStr() : "(non-string result)";
            } else {
                auto& e = std::get<RpcError>(r);
                last_ = "error " + std::to_string(e.code) + ": " + e.message;
            }
        }
        ImGui::Text("last: %s", last_.c_str());
    }
private:
    std::string last_;
};

void PluginManager::registerPlugin(std::unique_ptr<Plugin> p) {
    plugins_.push_back(std::move(p));
}

void PluginManager::discover() {
    // M-E0 仅内建样例；M-E3 起改为扫描 plugins/ 目录动态加载。
    registerPlugin(std::make_unique<CameraStubPlugin>());
}

void PluginManager::drawAll(RpcHost* host) {
    for (auto& p : plugins_) {
        if (ImGui::Begin(p->name())) p->draw(host);
        ImGui::End();
    }
}

} // namespace nl::editor::ui
