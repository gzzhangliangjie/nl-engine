// P8 M-S0: nl/audio implementation. miniaudio declarations only in this TU;
// MINIAUDIO_IMPLEMENTATION is compiled once in thirdparty/miniaudio/miniaudio.c.
#include "nl/audio/AudioWorld.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)   // vendored 4 MB single header: keep /W4 output clean
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace nl::audio {
namespace {

void setErr(std::string* err, const std::string& msg) { if (err) *err = msg; }
std::string maCode(ma_result r) { return std::to_string(static_cast<int>(r)); }

bool rd16(std::istream& f, std::uint16_t& v) {
    unsigned char b[2];
    if (!f.read(reinterpret_cast<char*>(b), 2)) return false;
    v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[0]) |
                                   (static_cast<std::uint16_t>(b[1]) << 8));
    return true;
}

bool rd32(std::istream& f, std::uint32_t& v) {
    unsigned char b[4];
    if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
    v = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
        (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
}

bool rd4cc(std::istream& f, char out[4]) { return static_cast<bool>(f.read(out, 4)); }

} // namespace

struct SourceSlot {
    std::unique_ptr<ma_sound> sound;
    std::string clip;
    float gain    = 1.0f;
    bool  looping = false;
};

struct AudioWorld::Impl {
    AudioConfig cfg;
    std::string error;
    bool        ok           = false;
    bool        engineInited = false;
    ma_engine   engine{};
    Vec3        listenerPos{};
    std::vector<std::unique_ptr<SourceSlot>> sources;

    SourceSlot* at(std::uint32_t id) {
        if (id == 0 || id > sources.size()) return nullptr;
        return sources[id - 1].get();
    }

    ~Impl() {
        // ma_sound objects must be uninitialised BEFORE the engine that owns their node graph.
        for (auto& s : sources) {
            if (s && s->sound) {
                ma_sound_uninit(s->sound.get());
                s->sound.reset();
            }
        }
        sources.clear();
        if (engineInited) {
            ma_engine_uninit(&engine);
            engineInited = false;
        }
    }
};

AudioWorld::AudioWorld(const AudioConfig& cfg) : impl_(new Impl) {
    impl_->cfg = cfg;
    ma_engine_config ec = ma_engine_config_init();
    if (cfg.offline) {
        // Official noDevice contract (miniaudio.h 1086-1099): channels and sampleRate
        // MUST be set explicitly, otherwise the engine has no device to infer them from.
        ec.noDevice   = MA_TRUE;
        ec.channels   = cfg.channels;
        ec.sampleRate = cfg.sampleRate;
    }
    ma_result r = ma_engine_init(&ec, &impl_->engine);
    if (r != MA_SUCCESS) {
        impl_->error = "ma_engine_init failed (" + maCode(r) + ")";
        return;
    }
    impl_->engineInited = true;
    impl_->ok = true;
}

AudioWorld::~AudioWorld() = default;

bool AudioWorld::ok() const { return impl_->ok; }
bool AudioWorld::isOffline() const { return impl_->cfg.offline; }
const std::string& AudioWorld::error() const { return impl_->error; }
std::uint32_t AudioWorld::sampleRate() const {
    return impl_->engineInited ? ma_engine_get_sample_rate(&impl_->engine) : 0u;
}
std::uint32_t AudioWorld::channels() const {
    return impl_->engineInited ? ma_engine_get_channels(&impl_->engine) : 0u;
}

bool AudioWorld::probeWav(const std::string& path, ClipDesc* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { setErr(err, "cannot open WAV: " + path); return false; }

    char cc[4]{};
    std::uint32_t u32 = 0;
    if (!rd4cc(f, cc) || std::memcmp(cc, "RIFF", 4) != 0) {
        setErr(err, "not a RIFF file: " + path); return false;
    }
    if (!rd32(f, u32)) { setErr(err, "truncated RIFF size: " + path); return false; }
    if (!rd4cc(f, cc) || std::memcmp(cc, "WAVE", 4) != 0) {
        setErr(err, "not a WAVE file: " + path); return false;
    }

    ClipDesc d;
    d.path = path;
    bool haveFmt = false, haveData = false;
    for (int guard = 0; guard < 32 && !haveData; ++guard) {
        if (!rd4cc(f, cc)) break;
        std::uint32_t chunkSize = 0;
        if (!rd32(f, chunkSize)) break;
        auto cur = f.tellg();

        if (std::memcmp(cc, "fmt ", 4) == 0) {
            std::uint16_t format = 0, ch = 0, align = 0, bits = 0;
            std::uint32_t rate = 0, byteRate = 0;
            if (!rd16(f, format) || !rd16(f, ch) || !rd32(f, rate) ||
                !rd32(f, byteRate) || !rd16(f, align) || !rd16(f, bits)) {
                setErr(err, "truncated fmt chunk: " + path); return false;
            }
            if (format != 1)     { setErr(err, "WAV is not integer PCM: " + path); return false; }
            if (bits != 16)      { setErr(err, "WAV must be 16-bit PCM: " + path); return false; }
            if (ch != 1)         { setErr(err, "M-S0 clip must be mono: " + path); return false; }
            if (rate == 0)       { setErr(err, "WAV sample rate is zero: " + path); return false; }
            if (align != ch * bits / 8) { setErr(err, "WAV block align mismatch: " + path); return false; }
            d.sampleRate = rate; d.channels = ch; d.bitsPerSample = bits;
            haveFmt = true;
        } else if (std::memcmp(cc, "data", 4) == 0) {
            if (!haveFmt) { setErr(err, "data chunk before fmt: " + path); return false; }
            d.frames = chunkSize / (d.channels * d.bitsPerSample / 8);
            haveData = true;
        }

        // Skip to the next chunk (fmt may be 18 bytes; all chunks are word aligned).
        std::uint32_t consumed = chunkSize + (chunkSize & 1u);
        f.seekg(static_cast<std::streamoff>(cur) + static_cast<std::streamoff>(consumed), std::ios::beg);
        if (!f) { setErr(err, "truncated chunk body: " + path); return false; }
    }

    if (!haveFmt || !haveData) { setErr(err, "missing fmt/data chunk: " + path); return false; }
    if (d.frames == 0)         { setErr(err, "WAV has no PCM frames: " + path); return false; }
    d.durationSeconds = static_cast<double>(d.frames) / static_cast<double>(d.sampleRate);
    if (out) *out = d;
    return true;
}

std::uint32_t AudioWorld::createSource(const std::string& wavPath, std::string* err) {
    if (!impl_->ok) { setErr(err, "world not initialised: " + impl_->error); return 0; }
    ClipDesc d;
    if (!probeWav(wavPath, &d, err)) return 0;

    auto slot = std::make_unique<SourceSlot>();
    slot->sound = std::make_unique<ma_sound>();
    // DECODE = synchronous full decode (no async resource-manager job in the read path,
    // hence bit-deterministic offline rendering). NO_SPATIALIZATION: 3D is M-S1's scope.
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_result r = ma_sound_init_from_file(&impl_->engine, wavPath.c_str(), flags,
                                          nullptr, nullptr, slot->sound.get());
    if (r != MA_SUCCESS) {
        setErr(err, "ma_sound_init_from_file failed (" + maCode(r) + ") for " + wavPath);
        return 0;
    }
    slot->clip = wavPath;
    impl_->sources.push_back(std::move(slot));
    return static_cast<std::uint32_t>(impl_->sources.size());
}

bool AudioWorld::configureSource(std::uint32_t id, float gain, bool looping, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    if (!(gain >= 0.0f && gain <= 2.0f)) { setErr(err, "gain out of [0,2]"); return false; }
    ma_sound_set_volume(s->sound.get(), gain);
    ma_sound_set_looping(s->sound.get(), looping ? MA_TRUE : MA_FALSE);
    s->gain = gain;
    s->looping = looping;
    return true;
}

bool AudioWorld::startSource(std::uint32_t id, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    ma_result r = ma_sound_start(s->sound.get());
    if (r != MA_SUCCESS) { setErr(err, "ma_sound_start failed (" + maCode(r) + ")"); return false; }
    return true;
}

bool AudioWorld::stopSource(std::uint32_t id, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    ma_result r = ma_sound_stop(s->sound.get());
    if (r != MA_SUCCESS) { setErr(err, "ma_sound_stop failed (" + maCode(r) + ")"); return false; }
    return true;
}

bool AudioWorld::isPlaying(std::uint32_t id) const {
    auto* s = impl_->at(id);
    return s && ma_sound_is_playing(s->sound.get()) == MA_TRUE;
}

bool AudioWorld::sourceDesc(std::uint32_t id, SourceDesc* out) const {
    auto* s = impl_->at(id);
    if (!s || !out) return false;
    out->id = id;
    out->clip = s->clip;
    out->gain = s->gain;
    out->looping = s->looping;
    out->playing = ma_sound_is_playing(s->sound.get()) == MA_TRUE;
    // M-S0 has no 3D positioning yet (M-S1); sources stay at the origin.
    out->position = Vec3{};
    return true;
}

std::uint32_t AudioWorld::sourceCount() const {
    return static_cast<std::uint32_t>(impl_->sources.size());
}

bool AudioWorld::setListenerPosition(const Vec3& pos, std::string* err) {
    if (!impl_->ok) { setErr(err, "world not initialised: " + impl_->error); return false; }
    ma_engine_listener_set_position(&impl_->engine, 0, pos.x, pos.y, pos.z);
    impl_->listenerPos = pos;
    return true;
}

Vec3 AudioWorld::listenerPosition() const { return impl_->listenerPos; }

std::uint64_t AudioWorld::readFrames(float* dst, std::uint64_t frameCount) {
    if (!impl_->ok) return 0;
    if (!impl_->cfg.offline) {
        impl_->error = "readFrames requires an offline (noDevice) world";
        return 0;
    }
    ma_uint64 read = 0;
    ma_result r = ma_engine_read_pcm_frames(&impl_->engine, dst, frameCount, &read);
    if (r != MA_SUCCESS) {
        impl_->error = "ma_engine_read_pcm_frames failed (" + maCode(r) + ")";
        return 0;
    }
    return static_cast<std::uint64_t>(read);
}

bool AudioWorld::playRealtimeBlocking(const std::string& wavPath, std::string* err) {
    if (impl_->cfg.offline) {
        setErr(err, "playRealtimeBlocking requires a realtime (device) world");
        return false;
    }
    std::uint32_t id = createSource(wavPath, err);
    if (!id) return false;
    if (!startSource(id, err)) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (isPlaying(id) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (isPlaying(id)) {
        stopSource(id);
        setErr(err, "realtime playback timed out after 10s");
        return false;
    }
    return true;
}

} // namespace nl::audio
