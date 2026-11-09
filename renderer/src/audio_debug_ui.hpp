#pragma once
// P8 M-S2: in-game audio source debug panel (renderer layer, ImGui).
//
// A SEPARATE window from the resident "nl-engine HUD" — it is default-OFF and
// only appears when the `audio.debug` CVar is set true, so it never forces a
// re-shoot of the HUD baseline (AGENTS.md M-S2 hard constraint). All data comes
// from nl::audio::AudioDebugController (core, zero graphics) so this TU only
// draws; it never pushes audio state of its own.
namespace nl::renderer::detail {
void drawAudioDebugWindow();
} // namespace nl::renderer::detail
