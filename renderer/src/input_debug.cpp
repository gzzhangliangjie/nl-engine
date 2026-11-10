// P8 M-I2: input-hud debug panel ImGui implementation.
// Companion to console_ui.cpp / audio_debug_ui.cpp; mirrors their "additive
// sibling window" placement (drawn inside hud.build()'s NewFrame..Render span,
// after the console and audio windows, before ImGui::Render) and their
// fixed-position/fixed-size determinism discipline (no imgui.ini dependency,
// screenshot baselines reproduce byte-for-byte).
//
// Data flow (see input_debug.hpp): viewer fills the display snapshot each frame
// start (from the injection-path InputState / ActionMap frozen values), this
// function draws it and writes injection requests back; the viewer converts
// request state changes into feedKey/feedMouseButton edges next frame start.
// The single-consumer ironclad rule holds: the viewer never calls
// updateFromWindow, so the existing hotkey / ImGui keyboard channels are
// untouched.

#include "nl/renderer/input_debug.hpp"

#include <imgui.h>

#include <cstdio>   // std::snprintf（MSVC 不经 <cstdio> 间接引入）

namespace nl::renderer {
namespace {

// 三态格子的文字 + 颜色（HUD 字符串 ASCII 纪律；颜色即语义——
// 绿=持续按下 / 黄=本帧按下边沿 / 橙=本帧释放边沿）。
void triStateCell(bool down, bool pressed, bool released) {
    if (pressed)
        ImGui::TextColored(ImVec4(1.00f, 0.90f, 0.20f, 1.0f), "PRESSED ");
    else if (released)
        ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.10f, 1.0f), "RELEASED");
    else if (down)
        ImGui::TextColored(ImVec4(0.30f, 1.00f, 0.40f, 1.0f), "down    ");
    else
        ImGui::TextDisabled("-");
}

// [-1,+1] 动作值 -> [0,1] 进度条 + 原始带符号值做 overlay（数值可断言，
// 条形只做视觉指引；ProgressBar 负值会 clamp，所以先线性搬进 [0,1]）。
void signedBar(const char* label, float v) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(80.0f);
    const float frac = (v + 1.0f) * 0.5f;
    char overlay[16];
    std::snprintf(overlay, sizeof(overlay), "%+.2f", v);
    ImGui::ProgressBar(frac, ImVec2(150.0f, 0.0f), overlay);
}

void boolBar(const char* label, float v, bool pressed, bool released) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(80.0f);
    ImGui::ProgressBar(v > 0.5f ? 1.0f : 0.0f, ImVec2(110.0f, 0.0f), v > 0.5f ? "1" : "0");
    ImGui::SameLine();
    // ⚠️ 三个分支必须都有 item：SameLine() 悬空会把后续行全部折叠到本行
    //（M-I2 实测教训——无边沿帧 pause 行整个消失并与 handbrake 行重叠）。
    if (pressed)
        ImGui::TextColored(ImVec4(1.00f, 0.90f, 0.20f, 1.0f), "PRESSED ");
    else if (released)
        ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.10f, 1.0f), "RELEASED");
    else
        ImGui::TextDisabled("-       ");
}

} // namespace

void drawInputDebugUi(InputDebugUi& ui) {
    // 固定位置 + 固定尺寸：常驻 HUD 面板占 x∈[14,344]，本面板排右侧；
    // 960x540 截图画布下完整可见（356+272=628 < 960，14+500=514 < 540）。
    ImGui::SetNextWindowPos(ImVec2(356.0f, 14.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(272.0f, 500.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::Begin("Input HUD (M-I2)", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::TextWrapped("injection-path input state (M-I0) + driving action map (M-I1)");
    ImGui::Separator();

    // ---- 键态矩阵（三态；注入路径驱动，见 input_debug.hpp 文件头）----
    ImGui::SeparatorText("keys (injected)");
    for (int i = 0; i < ui.rowCount && i < InputDebugUi::kMaxRows; ++i) {
        const InputDebugUi::KeyRow& r = ui.rows[i];
        ImGui::Text("%-6s", r.label);
        ImGui::SameLine(72.0f);
        triStateCell(r.down, r.pressed, r.released);
    }

    // ---- 动作值（驾驶映射表冻结值；组关停 = 清零语义在这里直接可见）----
    ImGui::SeparatorText("actions (driving map)");
    signedBar("throttle", ui.throttle);
    signedBar("steer", ui.steer);
    ImGui::Text("look");
    ImGui::SameLine(80.0f);
    ImGui::Text("(%+.2f, %+.2f)", ui.lookX, ui.lookY);
    boolBar("handbrake", ui.handbrake, ui.handbrakePressed, ui.handbrakeReleased);
    boolBar("pause", ui.pause, ui.pausePressed, ui.pauseReleased);
    ImGui::Text("sets   : driving[%c] ui[%c]",
                ui.drivingSetActive ? '*' : ' ', ui.uiSetActive ? '*' : ' ');

    // ---- 注入请求（checkbox 双向态写回；viewer 帧首转 feed 边沿）----
    ImGui::SeparatorText("inject (synthetic events)");
    ImGui::Checkbox("W", &ui.reqW);
    ImGui::SameLine();
    ImGui::Checkbox("A", &ui.reqA);
    ImGui::SameLine();
    ImGui::Checkbox("S", &ui.reqS);
    ImGui::SameLine();
    ImGui::Checkbox("D", &ui.reqD);
    ImGui::Checkbox("Space", &ui.reqSpace);
    ImGui::SameLine();
    ImGui::Checkbox("Esc", &ui.reqEsc);
    ImGui::SameLine();
    ImGui::Checkbox("LMB", &ui.reqLmb);
    if (ImGui::Button("release all")) {
        // 只清请求态：viewer 的边沿转换逻辑自动补发 release 事件。
        ui.reqW = ui.reqA = ui.reqS = ui.reqD = false;
        ui.reqSpace = ui.reqEsc = ui.reqLmb = false;
    }
    ImGui::Checkbox("ui set active", &ui.reqUiSetActive);
    ImGui::TextDisabled("demo: --input-demo");
    ImGui::TextDisabled("window-path input: off");

    ImGui::End();
}

} // namespace nl::renderer
