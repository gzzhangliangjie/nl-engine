// main.cpp — 编辑器宿主入口（C++ / Dear ImGui，独立进程）
// 流程：建 GLFW 窗 + ImGui(GL2 后端) + 起 RpcHost(server) + 每帧泵 update + 面板布局 + 插件。
// 注：vendored imgui 为标准版（非 docking 分支），M-E0 用普通窗口初始布局，docking 后续再加。
#include "imgui.h"
#include "imgui_backend.hpp"
#include "rpc_host.hpp"
#include "viewport.hpp"
#include "plugin.hpp"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>   // glfwGetWin32Window：M-E2 挂窗需要编辑器顶层 HWND
#include <cstdio>
#include <string>
#include <vector>

namespace {

nl::editor::host::RpcHost  g_host;
nl::editor::ui::Viewport   g_viewport;
nl::editor::ui::PluginManager g_plugins;

bool     g_announced = false;
uint64_t g_hwnd = 0;
int      g_pid = 0;
int      g_proto = 0;
std::vector<std::string> g_log;

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap);
    g_log.push_back(buf);
    if (g_log.size() > 200) g_log.erase(g_log.begin());
}

} // namespace

static void glfwErrCb(int code, const char* msg) {
    fprintf(stderr, "[glfw] %d: %s\n", code, msg ? msg : "(null)");
}

int main() {
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
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!nl::editor::ui::imguiInit(w)) logf("imgui backend init failed");

    // ---- RPC host（server）----
    g_host.registerHandler("editor.echo", [](const nl::editor::host::RpcValue& p) { return p; });
    g_host.registerNotify("editor.log", [](const nl::editor::host::RpcValue& p) {
        if (p.isStr()) logf("[engine] %s", p.asStr().c_str());
    });
    g_host.registerNotify("rpc.ping", [](const nl::editor::host::RpcValue&) {}); // pong 自动回
    g_host.onEngineReady = []() { logf("engine connected"); };
    g_host.onEngineClose = []() {
        g_announced = false;
        g_viewport.detach();   // M-E2：引擎进程死了，拆容器回落占位（崩溃隔离可见证据）
        logf("engine closed (crash-isolation: editor keeps running)");
    };
    g_host.onEngineAnnounce = [&](uint64_t hwnd, int pid, int proto, const nl::editor::host::RpcValue&) {
        g_hwnd = hwnd; g_pid = pid; g_proto = proto; g_announced = true;
        logf("engine announced hwnd=0x%llx pid=%d proto=%d", (unsigned long long)hwnd, pid, proto);
        // M-E2：编辑器主动 SetParent（§6 拍板）——建容器子窗挂引擎 HWND
        g_viewport.attachEngine(hwnd, glfwGetWin32Window(w));
    };
    if (!g_host.start()) logf("RpcHost.start failed (port 29110)");

    g_plugins.discover();

        while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        nl::editor::ui::imguiNewFrame(w);

        ImGuiIO& io = ImGui::GetIO();
        const float panelW = 300.0f;
        const float logH  = 180.0f;

        // Connection（左上面板）
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(panelW, 200.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Connection")) {
            ImGui::Text("rpc host : %s", g_announced ? "connected" : "listening (29110)");
            if (g_announced) {
                ImGui::Text("engine hwnd : 0x%llx", (unsigned long long)g_hwnd);
                ImGui::Text("engine pid  : %d", g_pid);
                ImGui::Text("protocol    : %d", g_proto);
                ImGui::Text("embed       : %s", g_viewport.attached() ? "SetParent ok" : "pending");
            }
            ImGui::Text("heartbeat    : ping 2s / timeout 6s");
        }
        ImGui::End();

        // Plugins（左下，随引擎状态变灰）
        ImGui::SetNextWindowPos(ImVec2(0.0f, 200.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(panelW, io.DisplaySize.y - 200.0f), ImGuiCond_FirstUseEver);
        g_plugins.drawAll(&g_host);

        // Viewport（右上，嵌入引擎窗口的区域）
        ImGui::SetNextWindowPos(ImVec2(panelW, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - panelW, io.DisplaySize.y - logH), ImGuiCond_FirstUseEver);
        g_viewport.draw(g_announced);

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

        g_host.update(); // 泵 RPC：accept / dispatch / 心跳超时判掉线
    }

    g_host.stop();
    nl::editor::ui::imguiShutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
