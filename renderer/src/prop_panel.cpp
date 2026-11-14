// P8 M-SC10：脚本属性面板实现（prop_panel.hpp 口径；ImGui 符号只在 renderer/）。
#include "nl/renderer/prop_panel.hpp"

#include "imgui.h"

namespace nl::renderer {

void PropPanel::drawWindow(State& st) {
    if (!st.active) return;

    ImGui::SetNextWindowSize(ImVec2(380.f, 480.f), ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags kWin =
        ImGuiWindowFlags_NoCollapse;   // HUD 面板纪律：标题区恒可见（M-T5 SignalPanel 同款）
    if (!ImGui::Begin("script props", nullptr, kWin)) {
        ImGui::End();
        return;
    }

    // 状态行：描述表代数 + 行数 + 通道态（ASCII）。
    if (st.channelAttached)
        ImGui::TextDisabled("gen %u  %d/%d props  channel ok",
                            st.gen, st.rowCount, st.totalProps);
    else
        ImGui::TextDisabled("gen %u  %d/%d props  channel missing (no PropGet/PropSet)",
                            st.gen, st.rowCount, st.totalProps);
    if (st.status[0] != '\0')
        ImGui::TextDisabled("%s", st.status);
    ImGui::Separator();

    // 行区滚动子区（HUD 高纪律：窗口 480 默认高，子区吃满剩余，<=560）。
    ImGui::BeginChild("props_rows", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
    for (int r = 0; r < st.rowCount; ++r) {
        Row& row = st.rows[r];
        ImGui::PushID(r);
        // 左列字段名（截断 63 由描述表保证），右列控件同排。
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", row.name);
        ImGui::SameLine(150.f);
        bool set = false;
        switch (row.kind) {
            case kFloat:
                if (row.min < row.max) {
                    float v = row.f;
                    if (ImGui::SliderFloat("##v", &v, row.min, row.max, "%.3f")) {
                        row.f = v;
                        set = true;
                    }
                } else {
                    set = ImGui::InputFloat("##v", &row.f, 0.f, 0.f, "%.3f");
                }
                break;
            case kInt:
                if (row.min < row.max) {
                    int v = row.i;
                    if (ImGui::SliderInt("##v", &v, static_cast<int>(row.min),
                                         static_cast<int>(row.max))) {
                        row.i = v;
                        set = true;
                    }
                } else {
                    set = ImGui::InputInt("##v", &row.i);
                }
                break;
            case kBool:
                set = ImGui::Checkbox("##v", &row.b);
                break;
            case kString:
                set = ImGui::InputText("##v", row.s, sizeof row.s,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
                break;
            default:
                ImGui::TextDisabled("?");
                break;
        }
        if (set) row.dirty = true;   // 边沿：viewer 帧尾消费 propSet 后清零
        ImGui::PopID();
    }
    if (st.totalProps > st.rowCount)
        ImGui::TextDisabled("... %d more (panel cap %d)", st.totalProps - st.rowCount, kMaxRows);
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace nl::renderer
