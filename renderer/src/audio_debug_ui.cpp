// P8 M-S2: in-game audio source debug panel ImGui implementation.
// Companion to console_ui.cpp; mirrors its "additive sibling window" placement
// (drawn after the console window, before ImGui::Render) and its default-off gate.
#include "audio_debug_ui.hpp"

#include "nl/audio/AudioWorld.hpp"
#include "nl/audio/DebugController.hpp"
#include "nl/core/console.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace nl::renderer::detail {
namespace {

// Plain-ASCII field labels: the panel is drawn with the project's Consolas/
// ASCII ImGui font, so any non-ASCII glyph would render as a tofu box.
const char* stateLabel(const nl::audio::SourceSnapshot& s) {
    if (!s.playing) return "stop";
    if (!s.audible) return "silent";   // out of maxDistance (M-S1 audibility)
    if (s.muted)    return "muted";    // another source is soloed
    return "play";
}

} // namespace

void drawAudioDebugWindow() {
    auto& ctrl = nl::audio::AudioDebugController::get();
    // Default-off gate: with audio.debug == false this window is never opened,
    // so it cannot affect the resident HUD baseline.
    if (!ctrl.debug()) return;

    const float w = 420.0f, h = 360.0f;
    ImGui::SetNextWindowPos(ImVec2(14.0f, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("Audio Debug");   // separate window title from the resident HUD

    // Master volume + freeze (wired to the controller; the world consults it).
    float vol = ctrl.masterVolume();
    if (ImGui::SliderFloat("master volume", &vol, 0.0f, 2.0f, "%.2f"))
        ctrl.setMasterVolume(vol);
    bool frozen = ctrl.frozen();
    if (ImGui::Checkbox("freeze mix", &frozen))
        ctrl.setFrozen(frozen);

    ImGui::Separator();
    ImGui::TextUnformatted("# entity  id  dist   gain(eff)  state");

    const auto snap = ctrl.snapshot();
    const bool offlineBound = ctrl.bound() && ctrl.bound()->isOffline();
    for (const auto& s : snap) {
        char row[256];
        std::snprintf(row, sizeof(row), "%-10s %2u  %6.2f  %6.3f   %s",
                      s.entity.c_str(), s.id, static_cast<double>(s.distance),
                      static_cast<double>(s.effectiveGain), stateLabel(s));
        ImGui::TextUnformatted(row);
        // Per-source solo button: solo this id (or clear if already soloed).
        ImGui::SameLine();
        if (ctrl.hasSolo() && static_cast<std::int64_t>(s.id) == ctrl.soloId()) {
            if (ImGui::SmallButton("clear solo"))
                ctrl.setSolo(0);
        } else {
            if (ImGui::SmallButton(("solo##" + std::to_string(s.id)).c_str()))
                ctrl.setSolo(static_cast<std::int64_t>(s.id));
        }
    }

    ImGui::Separator();
    // Single-step only advances an OFFLINE (noDevice) world; in-game realtime
    // freeze/step is a debug nicety and is disabled here to avoid fighting the
    // engine's own mixer thread.
    if (offlineBound) {
        if (ImGui::Button("step 256 frames")) {
            std::vector<float> buf(256 * 2, 0.0f);
            ctrl.bound()->stepFrames(buf.data(), 256);
        }
    } else {
        ImGui::TextUnformatted("(step available on offline world only)");
    }
    if (ctrl.hasSolo())
        ImGui::TextUnformatted(("solo: src " + std::to_string(ctrl.soloId())).c_str());
    else
        ImGui::TextUnformatted("solo: none");

    ImGui::End();
}

} // namespace nl::renderer::detail
