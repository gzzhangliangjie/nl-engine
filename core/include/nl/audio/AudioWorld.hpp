#pragma once
// P8 M-S0: nl/audio thin wrapper over vendored miniaudio.
// Core layer: ZERO graphics / NVRHI / ImGui symbols (AGENTS.md boundary).
//
// Object model follows Carrot engine/audio (AudioWorld / Clip / Source / Listener),
// but miniaudio owns decoding, mixing and (from M-S1) spatialization: we do NOT add
// a second audio thread on top of it (P8-AUDIO-PLAN.md section 2).
#include <cstdint>
#include <memory>
#include <string>

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
};

struct ClipDesc {
    std::string   path;
    std::uint32_t sampleRate    = 0;
    std::uint32_t channels      = 0;
    std::uint32_t bitsPerSample = 0;
    std::uint64_t frames        = 0;
    double        durationSeconds = 0.0;
};

struct SourceDesc {
    std::uint32_t id = 0;
    std::string   clip;
    float         gain    = 1.0f;
    bool          looping = false;
    bool          playing = false;
    Vec3          position;
};

class AudioWorld {
public:
    explicit AudioWorld(const AudioConfig& cfg = AudioConfig{});
    ~AudioWorld();
    AudioWorld(const AudioWorld&) = delete;
    AudioWorld& operator=(const AudioWorld&) = delete;

    bool               ok() const;
    bool               isOffline() const;
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

    // Listener: M-S0 keeps position only; orientation / cones belong to M-S1.
    bool setListenerPosition(const Vec3& pos, std::string* err = nullptr);
    Vec3 listenerPosition() const;

    // Offline path: pull interleaved f32 PCM. Returns frames actually read.
    std::uint64_t readFrames(float* dst, std::uint64_t frameCount);

    // Realtime path: play one WAV on the default device, block until it ends.
    bool playRealtimeBlocking(const std::string& wavPath, std::string* err = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::audio
