// P8 M-C1：开发者控制台 ImGui 前端实现。口径见 console_ui.hpp 头注。

#include "console_ui.hpp"

#include "nl/core/console.hpp"

#include <imgui.h>
#include <GLFW/glfw3.h>   // GLFW key code 原值（KeyEvent.key 携带；仅取常量，不碰窗口）

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace nl::renderer::detail {
namespace {

// ---- GLFW key -> ImGuiKey 映射（ImGui 官方 win32/glfw 后端同款口径，v1 子集）----
// Tab 不映射：viewer 用 Key::Tab 边沿走 consoleTab 通道（补全），Tab 进 ImGuiIO
// 会触发焦点导航（keyboard nav 默认关，但双通道没必要）。
ImGuiKey mapImGuiKey(int glfwKey) {
    if (glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z)
        return static_cast<ImGuiKey>(ImGuiKey_A + (glfwKey - GLFW_KEY_A));
    if (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9)
        return static_cast<ImGuiKey>(ImGuiKey_0 + (glfwKey - GLFW_KEY_0));
    switch (glfwKey) {
        case GLFW_KEY_SPACE:          return ImGuiKey_Space;
        case GLFW_KEY_ENTER:          return ImGuiKey_Enter;
        case GLFW_KEY_ESCAPE:         return ImGuiKey_Escape;
        case GLFW_KEY_BACKSPACE:      return ImGuiKey_Backspace;
        case GLFW_KEY_DELETE:         return ImGuiKey_Delete;
        case GLFW_KEY_INSERT:         return ImGuiKey_Insert;
        case GLFW_KEY_LEFT:           return ImGuiKey_LeftArrow;
        case GLFW_KEY_RIGHT:          return ImGuiKey_RightArrow;
        case GLFW_KEY_UP:             return ImGuiKey_UpArrow;
        case GLFW_KEY_DOWN:           return ImGuiKey_DownArrow;
        case GLFW_KEY_PAGE_UP:        return ImGuiKey_PageUp;
        case GLFW_KEY_PAGE_DOWN:      return ImGuiKey_PageDown;
        case GLFW_KEY_HOME:           return ImGuiKey_Home;
        case GLFW_KEY_END:            return ImGuiKey_End;
        case GLFW_KEY_MINUS:          return ImGuiKey_Minus;
        case GLFW_KEY_EQUAL:          return ImGuiKey_Equal;
        case GLFW_KEY_COMMA:          return ImGuiKey_Comma;
        case GLFW_KEY_PERIOD:         return ImGuiKey_Period;
        case GLFW_KEY_SEMICOLON:      return ImGuiKey_Semicolon;
        case GLFW_KEY_APOSTROPHE:     return ImGuiKey_Apostrophe;
        case GLFW_KEY_SLASH:          return ImGuiKey_Slash;
        case GLFW_KEY_BACKSLASH:      return ImGuiKey_Backslash;
        case GLFW_KEY_LEFT_BRACKET:   return ImGuiKey_LeftBracket;
        case GLFW_KEY_RIGHT_BRACKET:  return ImGuiKey_RightBracket;
        case GLFW_KEY_GRAVE_ACCENT:   return ImGuiKey_GraveAccent;
        case GLFW_KEY_CAPS_LOCK:      return ImGuiKey_CapsLock;
        case GLFW_KEY_LEFT_SHIFT:  case GLFW_KEY_RIGHT_SHIFT:  return ImGuiKey_ModShift;
        case GLFW_KEY_LEFT_CONTROL: case GLFW_KEY_RIGHT_CONTROL: return ImGuiKey_ModCtrl;
        case GLFW_KEY_LEFT_ALT:     case GLFW_KEY_RIGHT_ALT:     return ImGuiKey_ModAlt;
        case GLFW_KEY_LEFT_SUPER:   case GLFW_KEY_RIGHT_SUPER:   return ImGuiKey_ModSuper;
        default: break;
    }
    if (glfwKey >= GLFW_KEY_F1 && glfwKey <= GLFW_KEY_F12)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (glfwKey - GLFW_KEY_F1));
    return ImGuiKey_None;
}

// ---- 输入行状态（单 viewer 单窗口，进程级静态即可，hud.cpp 同款）----
char                  g_input[256] = {};
std::vector<std::string> g_history;    // 旧 -> 新
int                   g_histPos = -1;   // -1 = 不在翻历史（草稿态）
std::string           g_draft;          // 翻历史前的草稿
bool                  g_wasOpen = false;
bool                  g_prefilled = false;
std::size_t           g_prevLogCount = 0;

// ↑↓ 历史（ImGuiInputTextFlags_CallbackHistory，ImGui 内建通道）
int historyCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
    if (g_history.empty()) return 0;
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (g_histPos < 0) { g_draft = data->Buf; g_histPos = static_cast<int>(g_history.size()) - 1; }
        else if (g_histPos > 0) --g_histPos;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (g_histPos >= 0) {
            ++g_histPos;
            if (g_histPos >= static_cast<int>(g_history.size())) g_histPos = -1;  // 回到草稿
        } else {
            return 0;
        }
    } else {
        return 0;
    }
    const std::string& s = (g_histPos >= 0) ? g_history[static_cast<std::size_t>(g_histPos)] : g_draft;
    if (std::strcmp(data->Buf, s.c_str()) != 0) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, s.c_str());
    }
    return 0;
}

// Tab 补全（P8-CONSOLE-PLAN 判据：前缀匹配 + 唯一候选直接补全）。
// 只补最后一个 token（v1：命令名 / CVar 名）；多候选补到最长公共前缀，无进展不动。
void applyTabCompletion() {
    const std::string line(g_input);
    const std::size_t start = line.find_last_of(" \t");
    const std::string prefix = (start == std::string::npos) ? line : line.substr(start + 1);
    if (prefix.empty()) return;
    const auto cands = nl::core::console::Console::get().completePrefix(prefix);
    if (cands.empty()) return;
    std::string repl;
    if (cands.size() == 1) {
        repl = cands[0] + " ";   // 唯一候选：直接补全并留出敲参数的位置
    } else {
        repl = cands[0];
        for (const auto& c : cands) {
            std::size_t k = 0;
            while (k < repl.size() && k < c.size() && repl[k] == c[k]) ++k;
            repl.resize(k);
        }
        if (repl == prefix) return;   // 公共前缀无进展：不刷屏
    }
    std::string out = (start == std::string::npos ? std::string() : line.substr(0, start + 1)) + repl;
    std::snprintf(g_input, sizeof(g_input), "%s", out.c_str());
}

// 日志级别配色（IMGUI-GUIDE：HUD 字符串 ASCII；Carrot 同款 severity 色）
ImVec4 severityColor(nl::core::console::Severity sev) {
    using nl::core::console::Severity;
    switch (sev) {
        case Severity::Warn:  return ImVec4(1.00f, 0.85f, 0.30f, 1.0f);   // 黄
        case Severity::Error: return ImVec4(1.00f, 0.40f, 0.35f, 1.0f);   // 红
        case Severity::Info:  default: return ImVec4(0.90f, 0.92f, 0.95f, 1.0f);  // 灰白
    }
}

} // namespace

void drawConsoleWindow(HudFrame& frame) {
    // ---- 输入通路：事件喂 ImGuiIO（窗口开着才喂文本/按键；不开时 ImGui 无输入行
    // 可消费，但按键状态仍需同步——Mod 键在 ImGui 内部有全局状态）----
    ImGuiIO& io = ImGui::GetIO();
    if (frame.keyEventCount > 0) {
        for (int i = 0; i < frame.keyEventCount; ++i) {
            const HudFrame::ConsoleKeyEvent& ev = frame.keyEvents[i];
            const ImGuiKey k = mapImGuiKey(ev.key);
            if (k == ImGuiKey_None) continue;
            io.AddKeyEvent(ImGuiKey_ModCtrl,  ev.ctrl);
            io.AddKeyEvent(ImGuiKey_ModShift, ev.shift);
            io.AddKeyEvent(ImGuiKey_ModAlt,   ev.alt);
            io.AddKeyEvent(k, ev.down);
        }
    }
    if (frame.consoleOpen && frame.textCharCount > 0) {
        for (int i = 0; i < frame.textCharCount; ++i) io.AddInputCharacter(frame.textChars[i]);
    }

    if (!frame.consoleOpen) { g_wasOpen = false; return; }

    // ---- 窗口布局：右上角，避开左侧 330 宽 HUD 面板；画布太窄就叠上去（min 兜底）----
    const float w = 470.0f, h = 300.0f;
    float x = frame.displayWidth - w - 14.0f;
    if (x < 358.0f) x = 358.0f;                 // HUD 右缘 (14+330+14) 之外
    if (x < 14.0f) x = 14.0f;                   // 极小画布兜底
    const float yh = (frame.displayHeight > 56.0f) ? std::min(h, frame.displayHeight - 28.0f) : h;
    ImGui::SetNextWindowPos(ImVec2(x, 14.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, yh), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.90f);
    ImGui::Begin("Console", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    // ---- 滚动日志区（新行自动滚底；手滚上去就不拽——开发者正在往上翻时别打扰）----
    const auto& log = nl::core::console::Console::get().log();
    const float inputH = ImGui::GetTextLineHeightWithSpacing() * 1.5f;
    ImGui::BeginChild("console log", ImVec2(0.0f, -inputH), ImGuiChildFlags_Borders);
    for (const auto& l : log) {
        ImGui::PushStyleColor(ImGuiCol_Text, severityColor(l.sev));
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
    }
    const bool grew = log.size() != g_prevLogCount;
    if (grew) {
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        if (atBottom || g_prevLogCount == 0) ImGui::SetScrollHereY(1.0f);
        g_prevLogCount = log.size();
    }
    ImGui::EndChild();

    // ---- Tab 补全（viewer 边沿；applyTabCompletion 幂等：无候选/无进展不动）----
    if (frame.consoleTab) applyTabCompletion();

    // ---- 输入行（呼出首帧抢焦点；Enter 执行 + 进历史）----
    const bool rising = !g_wasOpen;
    if (rising) ImGui::SetKeyboardFocusHere();
    // consolePrefill：空->非空边沿灌一次（演示 / 截图确定性用）
    if (!g_prefilled && frame.consolePrefill && frame.consolePrefill[0]) {
        std::snprintf(g_input, sizeof(g_input), "%s", frame.consolePrefill);
        g_prefilled = true;
    }
    ImGui::PushItemWidth(-1.0f);
    const bool submit = ImGui::InputText("##console cmd", g_input, sizeof(g_input),
                                         ImGuiInputTextFlags_EnterReturnsTrue |
                                         ImGuiInputTextFlags_CallbackHistory,
                                         historyCallback);
    ImGui::PopItemWidth();
    if (submit && g_input[0]) {
        nl::core::console::Console::get().exec(g_input);
        // 去重连续重复项再进历史（终端惯例）；指针回草稿态
        if (g_history.empty() || g_history.back() != g_input) g_history.emplace_back(g_input);
        g_histPos = -1;
        g_input[0] = '\0';
        ImGui::SetScrollHereY(1.0f);   // 新日志立即露底（child 已 End，作用于窗口滚动兜底）
    }
    g_wasOpen = true;
    ImGui::End();

    frame.wantCaptureKeyboard = io.WantCaptureKeyboard;
    // wantCaptureMouse 的 OR 归并在 hud.cpp 统一做（历史行为不变）。
}

} // namespace nl::renderer::detail
