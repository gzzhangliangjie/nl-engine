#pragma once
// P8 M-S2: game-internal audio debug state (A-class track, zero graphics symbols).
//
// A process-wide singleton consulted by AudioWorld on every gain refresh and on
// readFrames. Defaults are pure passthrough (masterVolume=1, no solo, not frozen,
// panel off) so M-S0/M-S1 offline baselines stay byte-identical until something
// touches a CVar. The console CVars (audio.volume / audio.debug / audio.solo /
// audio.freeze / audio.dump) are wired to this controller in audio_cvars.cpp.
//
// Reference field set (UE AudioDebug.cpp read-only, EULA / MIT incompatible):
//   au.3dVisualize.ActiveSounds/Listeners/SpatialSources/VirtualLoops,
//   au.Debug.SoundCues.* (ShowPath/ShowDistance/Sort/Minimal),
//   au.DumpActiveSounds, au.ClearMutesAndSolos, command-level solo/mute.
// We borrow the *field vocabulary* (entity / position / distance / gain / state /
// solo / mute), not any code.
#include <cstdint>
#include <string>
#include <vector>

namespace nl::audio {

class AudioWorld;  // opaque: the bound world under debug

struct SourceSnapshot;  // defined in AudioWorld.hpp (included below)

class AudioDebugController {
public:
    static AudioDebugController& get();

    // Back to passthrough defaults: masterVolume=1, solo cleared, not frozen,
    // panel off, bound world released. Used by tool invocations between checks so
    // one test never bleeds into the next.
    void reset();

    // masterVolume: 0..2 multiplier applied to every source's configured gain.
    void  setMasterVolume(float v);
    float masterVolume() const { return masterVolume_; }

    // soloId: >0 solos that source id (all others forced to gain 0); <=0 clears.
    void        setSolo(std::int64_t id);
    std::int64_t soloId() const { return soloId_; }
    bool        hasSolo() const { return soloId_ > 0; }

    // frozen: when true, AudioWorld::readFrames returns 0 without advancing the
    // engine clock; stepFrames() still advances (single-step debugging).
    void setFrozen(bool b) { frozen_ = b; }
    bool frozen()  const { return frozen_; }

    // debug: in-game panel visibility (default off — must NOT retake HUD baselines).
    void setDebug(bool b) { debug_ = b; }
    bool debug()  const { return debug_; }

    // Bind the world the panel / audio.dump inspect (the viewer sets this once
    // it creates the active AudioWorld).
    void bind(AudioWorld* w);
    void unbind(AudioWorld* w);
    AudioWorld* bound() const { return bound_; }

    // Per-source gain factor from masterVolume + solo (audible/in-range handled by
    // AudioWorld::refreshAudibility). 0.0 when another source is soloed.
    float sourceGainFactor(std::uint32_t id) const;

    // Snapshot from the bound world (empty if none bound).
    std::vector<SourceSnapshot> snapshot() const;

    // Text form of the snapshot (panel data, also written by the `audio.dump` command).
    std::string debugText() const;

private:
    AudioDebugController() = default;
    float       masterVolume_ = 1.0f;
    std::int64_t soloId_      = 0;
    bool        frozen_       = false;
    bool        debug_        = false;
    AudioWorld* bound_        = nullptr;
};

} // namespace nl::audio
