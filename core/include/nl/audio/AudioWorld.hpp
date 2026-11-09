#pragma once
// P8 M-S0/M-S1: nl/audio thin wrapper over vendored miniaudio.
// Core layer: ZERO graphics / NVRHI / ImGui symbols (AGENTS.md boundary).
//
// Object model follows Carrot engine/audio (AudioWorld / Clip / Source / Listener),
// but miniaudio owns decoding, mixing and spatialization: we do NOT add
// a second audio thread on top of it (P8-AUDIO-PLAN.md section 2).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nl::audio {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct AudioConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels   = 2;
    // true  = noDevice engine, PCM pulled manually with readFrames() (offline, deterministic).
    // false = engine creates the default playback device (realtime path).
    bool offline = true;
    // M-S1: enable ma_sound spatialization. DEFAULT FALSE on purpose: the M-S0
    // offline PCM baseline (MA_SOUND_FLAG_NO_SPATIALIZATION) must stay byte-identical.
    bool spatialization = false;
};

struct ClipDesc {
    std::string   path;
    std::uint32_t sampleRate    = 0;
    std::uint32_t channels      = 0;
    std::uint32_t bitsPerSample = 0;
    std::uint64_t frames        = 0;
    double        durationSeconds = 0.0;
};

// miniaudio's four native attenuation models (miniaudio.h:5154-5157).
// NOTE: `None` is NOT "spatialized but flat" - the library takes an unspatialized
// fast path (no distance gain, no panning) when it is selected.
enum class AttenuationModel : std::uint8_t {
    None        = 0,
    Inverse     = 1,
    Linear      = 2,
    Exponential = 3
};

const char* attenuationModelName(AttenuationModel m);
bool parseAttenuationModel(const std::string& s, AttenuationModel* out);

// Full 3D state of a source. `audible` follows UE UAudioComponent::IsInAudibleRange
// semantics: miniaudio clamps distance to maxDistance (never reaches zero gain),
// so nl/audio explicitly mutes sources beyond it (P8-AUDIO-PLAN section 4).
struct SpatialDesc {
    Vec3             position;
    Vec3             direction{0.0f, 0.0f, -1.0f};
    AttenuationModel model = AttenuationModel::Inverse;
    float            minDistance = 1.0f;
    float            maxDistance = 100.0f;
    float            rolloff     = 1.0f;
    bool             audible     = true;
};

struct SourceDesc {
    std::uint32_t id = 0;
    std::string   clip;
    std::string   entity;          // M-S2: owning ECS entity label
    float         gain    = 1.0f;
    float         effectiveGain = 1.0f;  // M-S2: post masterVolume/solo/audible
    bool          looping = false;
    bool          playing = false;
    SpatialDesc   spatial;
};

// M-S1 Clip cache accounting. Decoded PCM de-duplication itself is done by
// ma_engine's internal ma_resource_manager (same path loads only once); this
// registry owns path de-dup, reference counting and hit/miss evidence.
struct CacheStats {
    std::uint32_t clips        = 0;
    std::uint32_t misses       = 0;  // first load per path (probe + decode)
    std::uint32_t hits         = 0;  // reused clip, probe skipped
    std::uint32_t sources      = 0;
    std::uint32_t maxClipRefs  = 0;
};

// M-S2 debug snapshot: one row of the in-game audio debug panel / `debug` tool
// output. `effectiveGain` already folds in master volume + solo + out-of-range mute
// (computed by AudioWorld::refreshAudibility against AudioDebugController).
struct SourceSnapshot {
    std::uint32_t id = 0;
    std::string   entity;           // ECS entity label (or "src-<id>" fallback)
    Vec3          position{};
    Vec3          listenerPosition{};
    float         distance       = 0.0f;   // source -> listener
    float         baseGain       = 0.0f;   // configured gain
    float         effectiveGain  = 0.0f;   // post masterVolume / solo / audible
    bool          playing        = false;
    bool          audible        = true;   // within maxDistance (M-S1 semantics)
    bool          soloed         = false;  // this source is the solo target
    bool          muted          = false;  // muted because another source is soloed
    std::string   clip;
    AttenuationModel model = AttenuationModel::Inverse;
};

class AudioWorld {
public:
    explicit AudioWorld(const AudioConfig& cfg = AudioConfig{});
    ~AudioWorld();
    AudioWorld(const AudioWorld&) = delete;
    AudioWorld& operator=(const AudioWorld&) = delete;

    bool               ok() const;
    bool               isOffline() const;
    bool               spatializationEnabled() const;
    const std::string& error() const;
    std::uint32_t      sampleRate() const;
    std::uint32_t      channels() const;

    // Clip: read-only WAV header probe (PCM 16-bit mono). Drives dump provenance.
    static bool probeWav(const std::string& path, ClipDesc* out, std::string* err = nullptr);

    // Source: synchronous full decode. Returns 1-based id, 0 = explicit failure.
    std::uint32_t createSource(const std::string& wavPath, std::string* err = nullptr);
    bool configureSource(std::uint32_t id, float gain, bool looping, std::string* err = nullptr);
    bool startSource(std::uint32_t id, std::string* err = nullptr);
    bool stopSource(std::uint32_t id, std::string* err = nullptr);
    bool isPlaying(std::uint32_t id) const;
    bool sourceDesc(std::uint32_t id, SourceDesc* out) const;
    std::uint32_t sourceCount() const;

    // M-S2: label a source with its owning ECS entity (panel / `debug` readability).
    bool setSourceEntity(std::uint32_t id, const std::string& name, std::string* err = nullptr);

    // M-S1 3D source state. Requires AudioConfig::spatialization = true.
    bool setSourcePosition(std::uint32_t id, const Vec3& pos, std::string* err = nullptr);
    bool setSourceDirection(std::uint32_t id, const Vec3& dir, std::string* err = nullptr);
    bool setSourceCone(std::uint32_t id, float innerRadians, float outerRadians,
                       float outerGain, std::string* err = nullptr);
    bool setSourceAttenuation(std::uint32_t id, AttenuationModel model,
                              float minDistance, float maxDistance, float rolloff,
                              std::string* err = nullptr);
    bool spatialDesc(std::uint32_t id, SpatialDesc* out) const;

    // Listener: position (M-S0) + orientation (M-S1).
    bool setListenerPosition(const Vec3& pos, std::string* err = nullptr);
    bool setListenerDirection(const Vec3& dir, std::string* err = nullptr);
    bool setListenerWorldUp(const Vec3& up, std::string* err = nullptr);
    Vec3 listenerPosition() const;
    Vec3 listenerDirection() const;
    Vec3 listenerWorldUp() const;

    // M-S1 Clip cache observability.
    CacheStats     cacheStats() const;
    std::uint32_t  clipCount() const;
    std::uint32_t  clipRefCount(const std::string& wavPath) const;

    // Offline path: pull interleaved f32 PCM. Returns frames actually read.
    // Honors AudioDebugController::frozen() — when frozen, returns 0 and does NOT
    // advance the engine clock (M-S2 single-step debugging).
    std::uint64_t readFrames(float* dst, std::uint64_t frameCount);

    // M-S2: advance the mix regardless of the frozen flag (single-step when paused).
    std::uint64_t stepFrames(float* dst, std::uint64_t frameCount);

    // M-S2: snapshot every active source for the debug panel / `debug` tool.
    std::vector<SourceSnapshot> snapshot() const;

    // Realtime path: play one WAV on the default device, block until it ends.
    bool playRealtimeBlocking(const std::string& wavPath, std::string* err = nullptr);

    // M-S2: bind this world as the one the debug panel / audio.dump inspects.
    void bindToDebug();
    void unbindFromDebug();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::audio
