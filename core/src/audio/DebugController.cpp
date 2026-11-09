// P8 M-S2: AudioDebugController implementation (zero graphics / zero console
// dependency — the console wiring lives in audio_cvars.cpp so this TU stays a
// pure core layer that audio-tools can link without dragging in the console).
#include "nl/audio/DebugController.hpp"
#include "nl/audio/AudioWorld.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace nl::audio {

AudioDebugController& AudioDebugController::get() {
    static AudioDebugController c;
    return c;
}

void AudioDebugController::reset() {
    masterVolume_ = 1.0f;
    soloId_       = 0;
    frozen_       = false;
    debug_        = false;
    // bound_ left as-is: reset is called by tool bookkeeping, not by the viewer.
}

void AudioDebugController::setMasterVolume(float v) {
    // Clamp to the AudioWorld gain envelope [0,2]; out-of-range is treated as 1.0
    // (same guard configureSource uses) so a stray CVar value cannot clip/silence
    // the whole mix silently.
    masterVolume_ = (v >= 0.0f && v <= 2.0f) ? v : 1.0f;
}

void AudioDebugController::setSolo(std::int64_t id) {
    soloId_ = id;   // <=0 clears solo (hasSolo() returns false)
}

void AudioDebugController::bind(AudioWorld* w)   { bound_ = w; }
void AudioDebugController::unbind(AudioWorld* w) { if (bound_ == w) bound_ = nullptr; }

float AudioDebugController::sourceGainFactor(std::uint32_t id) const {
    if (hasSolo() && id != static_cast<std::uint32_t>(soloId_)) return 0.0f;
    return 1.0f;
}

std::vector<SourceSnapshot> AudioDebugController::snapshot() const {
    if (!bound_) return {};
    return bound_->snapshot();
}

std::string AudioDebugController::debugText() const {
    std::ostringstream ss;
    const auto snap = snapshot();
    ss << "audio debug: " << snap.size() << " active source(s)\n";
    ss << "# id entity clip model pos(x,y,z) dist gain(eff) state\n";
    for (const auto& s : snap) {
        char line[512];
        std::snprintf(line, sizeof(line),
            "%u %s %s %s (%.2f,%.2f,%.2f) %.3f %.4f %s%s%s%s\n",
            s.id, s.entity.c_str(), s.clip.c_str(),
            attenuationModelName(s.model),
            static_cast<double>(s.position.x), static_cast<double>(s.position.y),
            static_cast<double>(s.position.z), static_cast<double>(s.distance),
            static_cast<double>(s.effectiveGain),
            s.playing ? "play" : "stop",
            s.audible ? "" : " silent",
            s.soloed ? " solo" : "", s.muted ? " muted" : "");
        ss << line;
    }
    return ss.str();
}

} // namespace nl::audio
