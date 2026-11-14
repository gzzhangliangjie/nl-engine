// main.cpp — 编辑器宿主入口（C++ / Dear ImGui，独立进程）
// 流程：建 GLFW 窗 + ImGui(GL2 后端) + 起 RpcHost(server) + 每帧泵 update + 面板布局 + 插件。
// M-E6：endpoint 可选 TCP/命名管道（--rpc-endpoint）；一个 host 同时接纳多个引擎
//       client，按 engine.announce.viewId 把各自 HWND 嵌进独立视口面板（平铺）。
#include <windows.h>            // M-E5：GetModuleFileNameA（plugins/ 目录定位；须先于 glfw3.h 防 APIENTRY C4005）
#include "imgui.h"
#include "imgui_backend.hpp"
#include "rpc_host.hpp"
#include "viewport.hpp"
#include "plugin.hpp"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>   // glfwGetWin32Window：M-E2 挂窗需要编辑器顶层 HWND
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using RpcValue = nl::editor::host::RpcValue;
using RpcError = nl::editor::host::RpcError;
using RpcResult = nl::editor::host::RpcResult;

nl::editor::host::RpcHost   g_host;
nl::editor::ui::PluginManager g_plugins;

struct ViewInfo {
    std::unique_ptr<nl::editor::ui::Viewport> vp;
    uint64_t hwnd = 0;
    int      pid = 0;
    int      proto = 0;
    std::string caps;
};

std::map<std::string, ViewInfo> g_views;      // viewId → 视口（M-E6 多视口）
nl::editor::ui::Viewport g_placeholder{"main"};   // 无连接时的占位面板
HWND     g_editorHwnd = nullptr;
std::string g_activeView = "main";
std::string g_endpointText = "tcp://127.0.0.1:29110";
std::vector<std::string> g_log;

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap);
    g_log.push_back(buf);
    if (g_log.size() > 200) g_log.erase(g_log.begin());
}

std::string fieldStr(const RpcValue& v, const char* key, const char* def) {
    if (!v.isMap()) return def;
    auto it = v.asMap().find(key);
    return (it != v.asMap().end() && it->second.isStr()) ? it->second.asStr() : def;
}

std::string capsSummary(const RpcValue& caps) {
    if (!caps.isMap()) return "";
    const auto& m = caps.asMap();
    auto num = [&m](const char* k) -> long long {
        auto it = m.find(k);
        if (it == m.end()) return -1;
        if (it->second.isInt()) return it->second.asInt();
        if (it->second.isDouble()) return (long long)it->second.asDouble();
        return -1;
    };
    std::string backend = "?";
    auto bit = m.find("backend");
    if (bit != m.end() && bit->second.isStr()) backend = bit->second.asStr();
    char buf[128];
    snprintf(buf, sizeof buf, "%lldx%lld %s", num("w"), num("h"), backend.c_str());
    return buf;
}

} // namespace

// M-E8：引擎 sceneChanged 推送旗标——定义须在匿名 namespace 外（plugin.hpp 的
// extern 是全局外部链接；落进匿名 namespace = 内部链接双实体 = C2872 不明确符号）。
std::atomic<bool> g_sceneTreeDirty{false};

// M-E9/M-E10：outliner 选中实体（同 g_sceneTreeDirty 链接口径；-1 = 无选中）。
std::atomic<long long> g_sceneSelEntity{-1};

static void glfwErrCb(int code, const char* msg) {
    fprintf(stderr, "[glfw] %d: %s\n", code, msg ? msg : "(null)");
}

int main(int argc, char** argv) {
    std::string endpoint = "tcp://127.0.0.1:29110";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rpc-endpoint" && i + 1 < argc) endpoint = argv[++i];
    }

    glfwSetErrorCallback(glfwErrCb);
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    // 强制 GL 2.1 兼容上下文，保证 glOrtho/固定管线可用（GL2 后端依赖）
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    GLFWwindow* w = glfwCreateWindow(1280, 800, "nl-engine Editor", nullptr, nullptr);
    if (!w) { fprintf(stderr, "glfwCreateWindow failed\n"); return 1; }
    glfwMakeContextCurrent(w);
    // 无 GPU/无桌面会话（headless）时 GL 上下文可能为 NULL；显式判空，避免首个 GL 调用段错误
    if (!glfwGetCurrentContext()) {
        fprintf(stderr, "OpenGL context unavailable (no GPU/display?). Editor needs a desktop session.\n");
        glfwDestroyWindow(w); glfwTerminate(); return 1;
    }
    g_editorHwnd = glfwGetWin32Window(w);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!nl::editor::ui::imguiInit(w)) logf("imgui backend init failed");

    // ---- RPC host（server）----
    g_host.registerHandler("editor.echo", [](const RpcValue& p) { return p; });
    g_host.registerNotify("editor.log", [](const RpcValue& p) {
        if (p.isStr()) logf("[engine] %s", p.asStr().c_str());
    });
    g_host.registerNotify("rpc.ping", [](const RpcValue&) {}); // pong 由 host 定向回
    // M-E8：场景变更推送 → 置 dirty 旗标，SceneOutlinerPlugin 下一帧自动刷新
    // （拉增量 = 重发 scene.getTree；200ms 防抖在插件侧做，防引擎连变刷爆 UI）。
    g_host.registerNotify("editor.sceneChanged", [](const RpcValue&) {
        g_sceneTreeDirty.store(true);
    });

    // M-E6：视图注册表（探针/插件可查当前连入的引擎进程）
    g_host.registerHandler("editor.views", [](const RpcValue&) -> RpcResult {
        nl::editor::msgpack::Array arr;
        arr.reserve(g_views.size());
        for (const auto& kv : g_views) {
            nl::editor::msgpack::Map m;
            m["viewId"]  = RpcValue(kv.first);
            m["hwnd"]    = RpcValue((int64_t)kv.second.hwnd);
            m["pid"]     = RpcValue((int64_t)kv.second.pid);
            m["proto"]   = RpcValue((int64_t)kv.second.proto);
            m["attached"] = RpcValue(kv.second.vp && kv.second.vp->attached());
            arr.push_back(RpcValue(std::move(m)));
        }
        return RpcValue(std::move(arr));
    });
    // M-E6：定向投递验证（探针用：只有目标 view 收到 editor.probe notify）
    g_host.registerHandler("editor.pingView", [](const RpcValue& p) -> RpcResult {
        const std::string view = fieldStr(p, "view", "");
        nl::editor::msgpack::Map note;
        note["view"] = RpcValue(view);
        note["from"] = RpcValue(std::string("editor"));
        const bool sent = g_host.notifyTo(view, "editor.probe", RpcValue(std::move(note)));
        nl::editor::msgpack::Map r;
        r["sent"] = RpcValue(sent);
        r["view"] = RpcValue(view);
        return RpcValue(std::move(r));
    });
    // M-E6：跨视图转发一个 RPC（多视口下显式指定目标引擎进程）
    g_host.registerHandler("editor.callView", [](const RpcValue& p) -> RpcResult {
        const std::string view = fieldStr(p, "view", "");
        const std::string method = fieldStr(p, "method", "");
        if (view.empty() || method.empty()) return RpcError{100, "view/method required"};
        RpcValue params;
        if (p.isMap()) {
            auto it = p.asMap().find("params");
            if (it != p.asMap().end()) params = it->second;
        }
        RpcResult r = g_host.callTo(view, method, params, 3000);
        if (auto* e = std::get_if<RpcError>(&r)) return RpcResult(*e);
        return RpcResult(std::get<RpcValue>(r));
    });

    g_host.onEngineReady = []() { logf("engine connected"); };
    g_host.onEngineClose = [](const std::string& viewId) {
        auto it = g_views.find(viewId);
        if (it != g_views.end()) g_views.erase(it);   // Viewport 析构自动拆 WS_CHILD 容器
        logf("engine closed view=%s (crash-isolation: editor keeps running)", viewId.c_str());
    };
    g_host.onEngineAnnounce = [](const std::string& viewId, uint64_t hwnd, int pid,
                                 int proto, const RpcValue& caps) {
        auto& v = g_views[viewId];
        if (!v.vp) v.vp = std::make_unique<nl::editor::ui::Viewport>(viewId);
        v.hwnd = hwnd; v.pid = pid; v.proto = proto;
        v.caps = capsSummary(caps);
        logf("engine announced view=%s hwnd=0x%llx pid=%d proto=%d",
             viewId.c_str(), (unsigned long long)hwnd, pid, proto);
        // M-E2：编辑器主动 SetParent（§6 拍板）——建容器子窗挂引擎 HWND
        v.vp->attachEngine(hwnd, g_editorHwnd);
        // 首个视图自动设为活动视图（插件的 call/notify 默认打给它）
        if (g_views.size() == 1) { g_activeView = viewId; g_host.setActiveView(viewId); }
    };
    if (!g_host.start(endpoint)) logf("RpcHost.start failed (%s)", endpoint.c_str());
    g_endpointText = g_host.endpoint();

    g_plugins.discover();

    // M-E5：plugins/ 目录动态扫描（exe 同目录 plugins/*.dll，C ABI 校验，坏 DLL 跳过）
    {
        char exePath[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        const size_t slash = dir.find_last_of("\\/");
        dir = (slash == std::string::npos) ? "." : dir.substr(0, slash);
        g_plugins.discoverDlls(dir + "\\plugins", [](const char* msg) {
            logf("%s", msg);
            fprintf(stdout, "[plugins] %s\n", msg);
            fflush(stdout);
        });
    }

    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        nl::editor::ui::imguiNewFrame(w);

        ImGuiIO& io = ImGui::GetIO();
        const float panelW = 300.0f;
        const float logH  = 180.0f;

        // Connection（左上面板）
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(panelW, 250.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Connection")) {
            ImGui::Text("endpoint  : %s", g_endpointText.c_str());
            ImGui::Text("clients   : %zu / views: %zu", g_host.clientCount(), g_views.size());
            ImGui::Text("heartbeat : ping 2s / timeout 6s");
            ImGui::Separator();
            const std::vector<std::string> ids = g_host.viewIds();
            if (ImGui::BeginCombo("active view", g_activeView.c_str())) {
                for (const auto& id : ids) {
                    const bool sel = (id == g_activeView);
                    if (ImGui::Selectable(id.c_str(), sel)) {
                        g_activeView = id; g_host.setActiveView(id);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            for (const auto& kv : g_views) {
                ImGui::BulletText("%s  pid=%d proto=%d", kv.first.c_str(), kv.second.pid, kv.second.proto);
                if (!kv.second.caps.empty()) ImGui::Text("        caps : %s", kv.second.caps.c_str());
                ImGui::Text("        embed: %s", kv.second.vp && kv.second.vp->attached()
                                                 ? "SetParent ok" : "pending");
            }
        }
        ImGui::End();

        // Plugins（左下，随引擎状态变灰）
        ImGui::SetNextWindowPos(ImVec2(0.0f, 250.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(panelW, io.DisplaySize.y - 250.0f), ImGuiCond_FirstUseEver);
        g_plugins.drawAll(&g_host);

        // Viewport（右上）：M-E6 多视口平铺；每个面板一个 WS_CHILD 容器 + 一个引擎 HWND
        const float vx = panelW, vy = 0.0f;
        const float vw = io.DisplaySize.x - panelW, vh = io.DisplaySize.y - logH;
        const int n = (int)g_views.size();
        if (n == 0) {
            ImGui::SetNextWindowPos(ImVec2(vx, vy), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(vw, vh), ImGuiCond_Always);
            g_placeholder.draw(false);
        } else {
            const int cols = (n <= 2) ? n : 2;
            const int rows = (n + cols - 1) / cols;
            const float cw = vw / cols, ch = vh / rows;
            int k = 0;
            for (auto& kv : g_views) {
                const int c = k % cols, r = k / cols;
                ImGui::SetNextWindowPos(ImVec2(vx + c * cw, vy + r * ch), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(cw, ch), ImGuiCond_Always);
                kv.second.vp->draw(true);
                ++k;
            }
        }

        // Log（右下）
        ImGui::SetNextWindowPos(ImVec2(panelW, io.DisplaySize.y - logH), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - panelW, logH), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Log")) {
            for (auto& l : g_log) ImGui::TextUnformatted(l.c_str());
        }
        ImGui::End();

        ImGui::Render();
        int ww = 0, wh = 0; glfwGetFramebufferSize(w, &ww, &wh);
        glViewport(0, 0, ww, wh);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nl::editor::ui::imguiRenderDrawData();
        glfwSwapBuffers(w);

        g_host.update();
    }

    g_views.clear();     // 先拆所有容器，再停 host
    g_host.stop();
    nl::editor::ui::imguiShutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
