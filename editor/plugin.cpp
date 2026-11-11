// plugin.cpp — 编辑器插件加载器（进程内注册 + 内建样例）
#include "plugin.hpp"
#include <imgui.h>

namespace nl::editor::ui {

// 复用编辑器宿主侧的类型别名（位于兄弟命名空间 nl::editor::host）
using nl::editor::host::RpcHost;
using nl::editor::host::RpcValue;
using nl::editor::host::RpcError;
using nl::editor::host::RpcResult;

// M-E3：相机工具插件——「编辑器 UI → RPC call → 引擎相机状态变化 → 嵌入画面动」
// 的最小可见闭环。操作后回读 camera.getState 对齐 UI（引擎侧 clamp，回读为准）。
class CameraPlugin : public Plugin {
public:
    const char* name() const override { return "Camera"; }
    void draw(RpcHost* host) override {
        if (!host) { ImGui::TextDisabled("(no rpc host)"); return; }

        // ---- 状态区（camera.getState 回读）----
        if (ImGui::Button("Refresh")) refresh(host);
        ImGui::SameLine();
        if (have_) {
            ImGui::Text("fov %.1f  dist %.2f  yaw %.1f  pitch %.1f", fov_, dist_, yaw_, pitch_);
        } else {
            ImGui::TextDisabled("state: not fetched (engine offline or call failed)");
        }

        // ---- 控制区 ----
        float fv = fov_;
        if (ImGui::SliderFloat("FOV", &fv, 20.0f, 120.0f, "%.1f deg")) {
            if (callField(host, "camera.setFov", "fov", fv, fov_)) refresh(host);
        }
        float dv = dist_;
        if (ImGui::SliderFloat("Distance", &dv, 0.45f, 20.0f, "%.2f")) {
            if (callField(host, "camera.setDistance", "dist", dv, dist_)) refresh(host);
        }
        if (ImGui::Button("Orbit L")) { orbitDeg(host, -15.0f, 0.0f); }
        ImGui::SameLine();
        if (ImGui::Button("Orbit R")) { orbitDeg(host, 15.0f, 0.0f); }
        ImGui::SameLine();
        if (ImGui::Button("Orbit Up")) { orbitDeg(host, 0.0f, -10.0f); }
        ImGui::SameLine();
        if (ImGui::Button("Orbit Dn")) { orbitDeg(host, 0.0f, 10.0f); }
        if (ImGui::Button("Reset")) {
            auto r = host->call("camera.reset", RpcValue());
            logErr(r, "camera.reset");
            refresh(host);
        }
        ImGui::Separator();
        ImGui::TextDisabled("orbit: L/R +-15deg yaw, Up/Dn -+10deg pitch (deg->px 换算在编辑器侧)");
    }

private:
    // params 统一命名 Map（RPC-SPEC）；single-field 快捷封装
    static bool callField(RpcHost* host, const char* method, const char* key,
                          double val, float& outActual) {
        msgpack::Map m; m[key] = val;
        auto r = host->call(method, RpcValue(std::move(m)));
        if (std::holds_alternative<RpcValue>(r)) {
            const auto& v = std::get<RpcValue>(r);
            if (v.isDouble()) { outActual = (float)v.asDouble(); return true; }
            if (v.isInt())    { outActual = (float)v.asInt(); return true; }
            return true; // ack 类型不符也当成功（refresh 会校正）
        }
        const auto& e = std::get<RpcError>(r);
        ImGui::Text("error %d: %s", e.code, e.message.c_str());
        return false;
    }

    // 引擎 camera.orbit 收像素增量（kOrbitGain=0.007 rad/px）；插件按度数语义，
    // deg -> px 换算在编辑器侧：px = deg / (0.007 rad/px * 57.2958 deg/rad) ≈ deg * 2.4913
    void orbitDeg(RpcHost* host, double degX, double degY) {
        constexpr double kPxPerDeg = 1.0 / (0.007 * 57.2958);   // ≈ 2.4913
        msgpack::Map m;
        m["dx"] = degX * kPxPerDeg;
        m["dy"] = degY * kPxPerDeg;
        auto r = host->call("camera.orbit", RpcValue(std::move(m)));
        logErr(r, "camera.orbit");
        refresh(host);
    }

    void refresh(RpcHost* host) {
        auto r = host->call("camera.getState", RpcValue());
        if (!std::holds_alternative<RpcValue>(r)) {
            logErr(r, "camera.getState"); have_ = false; return;
        }
        const auto& v = std::get<RpcValue>(r);
        if (!v.isMap()) { have_ = false; return; }
        auto get = [&](const char* k, double def) {
            auto it = v.asMap().find(k);
            return (it != v.asMap().end() && it->second.isDouble()) ? it->second.asDouble() : def;
        };
        fov_   = (float)get("fov", 60.0);
        dist_  = (float)get("dist", 4.2);
        yaw_   = (float)get("yaw", 0.0);
        pitch_ = (float)get("pitch", 35.0);
        have_  = true;
    }

    static void logErr(const RpcResult& r, const char* what) {
        if (!std::holds_alternative<RpcError>(r)) return;
        const auto& e = std::get<RpcError>(r);
        ImGui::Text("[%s] error %d: %s", what, e.code, e.message.c_str());
    }

    bool   have_ = false;
    float  fov_ = 60.0f, dist_ = 4.2f, yaw_ = 0.0f, pitch_ = 35.0f;
};

void PluginManager::registerPlugin(std::unique_ptr<Plugin> p) {
    plugins_.push_back(std::move(p));
}

void PluginManager::discover() {
    // M-E3 内建相机插件；动态扫描 plugins/ 目录留后续里程碑。
    registerPlugin(std::make_unique<CameraPlugin>());
}

void PluginManager::drawAll(RpcHost* host) {
    for (auto& p : plugins_) {
        if (ImGui::Begin(p->name())) p->draw(host);
        ImGui::End();
    }
}

} // namespace nl::editor::ui
