// P8 M-S0/M-S1: nl/audio implementation. miniaudio declarations only in this TU;
// MINIAUDIO_IMPLEMENTATION is compiled once in thirdparty/miniaudio/miniaudio.c.
#include "nl/audio/AudioWorld.hpp"
#include "nl/audio/DebugController.hpp"   // M-S2: master volume / solo / freeze

#if defined(_MSC_VER)
#pragma warning(push, 0)   // vendored 4 MB single header: keep /W4 output clean
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>
#include <vector>

namespace nl::audio {

// The public enum deliberately mirrors miniaudio's ordering; guard it so a
// vendored upgrade cannot silently scramble the four attenuation curves.
static_assert(static_cast<int>(ma_attenuation_model_none)        == 0, "ma_attenuation_model_none changed");
static_assert(static_cast<int>(ma_attenuation_model_inverse)     == 1, "ma_attenuation_model_inverse changed");
static_assert(static_cast<int>(ma_attenuation_model_linear)      == 2, "ma_attenuation_model_linear changed");
static_assert(static_cast<int>(ma_attenuation_model_exponential) == 3, "ma_attenuation_model_exponential changed");

const char* attenuationModelName(AttenuationModel m) {
    switch (m) {
        case AttenuationModel::None:        return "none";
        case AttenuationModel::Inverse:     return "inverse";
        case AttenuationModel::Linear:      return "linear";
        case AttenuationModel::Exponential: return "exponential";
    }
    return "unknown";
}

bool parseAttenuationModel(const std::string& s, AttenuationModel* out) {
    if (!out) return false;
    if (s == "none")        { *out = AttenuationModel::None;        return true; }
    if (s == "inverse")     { *out = AttenuationModel::Inverse;     return true; }
    if (s == "linear")      { *out = AttenuationModel::Linear;      return true; }
    if (s == "exponential") { *out = AttenuationModel::Exponential; return true; }
    return false;
}

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

float distanceBetween(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

struct ClipSlot {
    ClipDesc      desc;
    std::uint32_t refs = 0;
};

struct SourceSlot {
    std::uint32_t id = 0;
    std::unique_ptr<ma_sound> sound;
    std::string  clip;
    std::string  entity;          // M-S2: owning ECS entity label
    float        gain    = 1.0f;
    float        effectiveGain = 1.0f;  // M-S2: post masterVolume/solo/audible
    bool         looping = false;
    SpatialDesc  spatial{};
};

struct AudioWorld::Impl {
    AudioConfig cfg;
    std::string error;
    bool        ok           = false;
    bool        engineInited = false;
    ma_engine   engine{};
    Vec3        listenerPos{};
    Vec3        listenerDir{0.0f, 0.0f, -1.0f};
    Vec3        listenerUp{0.0f, 1.0f, 0.0f};
    std::vector<std::unique_ptr<SourceSlot>> sources;
    std::map<std::string, ClipSlot>          clips;
    std::uint32_t misses = 0;
    std::uint32_t hits   = 0;

    SourceSlot* at(std::uint32_t id) {
        if (id == 0 || id > sources.size()) return nullptr;
        return sources[id - 1].get();
    }

    // UE UAudioComponent::IsInAudibleRange semantics. miniaudio clamps the
    // distance to maxDistance, so gain never reaches zero on its own; nl/audio
    // mutes the source explicitly when the listener moves out of range.
    // M-S2: also folds in master volume + solo from AudioDebugController.
    void refreshAudibility(SourceSlot* s) {
        if (!s || !s->sound) return;
        const SpatialDesc& sp = s->spatial;
        const bool hasRange = cfg.spatialization &&
                              sp.model != AttenuationModel::None &&
                              sp.maxDistance > 0.0f;
        const bool inRange = !hasRange ||
                             distanceBetween(sp.position, listenerPos) <= sp.maxDistance;
        s->spatial.audible = inRange;
        const auto& dbg = AudioDebugController::get();
        const float soloFactor = dbg.sourceGainFactor(s->id);
        s->effectiveGain = s->gain * dbg.masterVolume() * soloFactor;
        ma_sound_set_volume(s->sound.get(), inRange ? s->effectiveGain : 0.0f);
    }

    void refreshAll() { for (auto& s : sources) refreshAudibility(s.get()); }

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
    // Make the listener frame explicit rather than relying on engine defaults.
    ma_engine_listener_set_direction(&impl_->engine, 0,
                                     impl_->listenerDir.x, impl_->listenerDir.y, impl_->listenerDir.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0,
                                    impl_->listenerUp.x, impl_->listenerUp.y, impl_->listenerUp.z);
}

AudioWorld::~AudioWorld() = default;

bool AudioWorld::ok() const { return impl_->ok; }
bool AudioWorld::isOffline() const { return impl_->cfg.offline; }
bool AudioWorld::spatializationEnabled() const { return impl_->cfg.spatialization; }
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
            if (ch != 1)         { setErr(err, "clip must be mono: " + path); return false; }
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

    // M-S1 Clip cache: a path is probed/decoded once; ma_sound_init_from_file
    // goes through ma_engine's resource manager, which de-duplicates the decoded
    // PCM data for the same path while giving each ma_sound its own read cursor.
    auto it = impl_->clips.find(wavPath);
    const bool hit = (it != impl_->clips.end());
    if (!hit) {
        ClipDesc d;
        if (!probeWav(wavPath, &d, err)) return 0;
        it = impl_->clips.emplace(wavPath, ClipSlot{d, 0u}).first;
        ++impl_->misses;
    } else {
        ++impl_->hits;   // probe skipped: clip metadata is served from the registry
    }
    ++it->second.refs;

    auto slot = std::make_unique<SourceSlot>();
    slot->id = static_cast<std::uint32_t>(impl_->sources.size() + 1);
    slot->sound = std::make_unique<ma_sound>();
    // DECODE = synchronous full decode (no async resource-manager job in the read path,
    // hence bit-deterministic offline rendering). M-S0 keeps NO_SPATIALIZATION so its
    // PCM baseline stays byte-identical; M-S1 opts in via AudioConfig::spatialization.
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE |
        (impl_->cfg.spatialization ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION));
    ma_result r = ma_sound_init_from_file(&impl_->engine, wavPath.c_str(), flags,
                                          nullptr, nullptr, slot->sound.get());
    if (r != MA_SUCCESS) {
        --it->second.refs;
        if (hit) { --impl_->hits; }
        else     { --impl_->misses; impl_->clips.erase(it); }
        setErr(err, "ma_sound_init_from_file failed (" + maCode(r) + ") for " + wavPath);
        return 0;
    }
    if (impl_->cfg.spatialization) {
        // Keep the ma_sound state in lockstep with SpatialDesc's defaults.
        ma_sound_set_position(slot->sound.get(), 0.0f, 0.0f, 0.0f);
        ma_sound_set_direction(slot->sound.get(), slot->spatial.direction.x,
                               slot->spatial.direction.y, slot->spatial.direction.z);
        ma_sound_set_attenuation_model(slot->sound.get(), ma_attenuation_model_inverse);
        ma_sound_set_min_distance(slot->sound.get(), slot->spatial.minDistance);
        ma_sound_set_max_distance(slot->sound.get(), slot->spatial.maxDistance);
        ma_sound_set_rolloff(slot->sound.get(), slot->spatial.rolloff);
    }
    slot->clip = wavPath;
    impl_->refreshAudibility(slot.get());
    impl_->sources.push_back(std::move(slot));
    return static_cast<std::uint32_t>(impl_->sources.size());
}

bool AudioWorld::configureSource(std::uint32_t id, float gain, bool looping, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    if (!(gain >= 0.0f && gain <= 2.0f)) { setErr(err, "gain out of [0,2]"); return false; }
    s->gain = gain;
    s->looping = looping;
    ma_sound_set_looping(s->sound.get(), looping ? MA_TRUE : MA_FALSE);
    impl_->refreshAudibility(s);   // may force volume to 0 when out of range
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
    const auto* s = const_cast<Impl*>(impl_.get())->at(id);
    return s && ma_sound_is_playing(s->sound.get()) == MA_TRUE;
}

bool AudioWorld::sourceDesc(std::uint32_t id, SourceDesc* out) const {
    auto* s = impl_->at(id);
    if (!s || !out) return false;
    out->id = id;
    out->clip = s->clip;
    out->entity = s->entity;
    out->gain = s->gain;
    out->effectiveGain = s->effectiveGain;
    out->looping = s->looping;
    out->playing = ma_sound_is_playing(s->sound.get()) == MA_TRUE;
    out->spatial = s->spatial;
    return true;
}

std::uint32_t AudioWorld::sourceCount() const {
    return static_cast<std::uint32_t>(impl_->sources.size());
}

bool AudioWorld::setSourceEntity(std::uint32_t id, const std::string& name, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    s->entity = name;
    return true;
}

bool AudioWorld::setSourcePosition(std::uint32_t id, const Vec3& pos, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    if (!impl_->cfg.spatialization) { setErr(err, "spatialization disabled in AudioConfig"); return false; }
    ma_sound_set_position(s->sound.get(), pos.x, pos.y, pos.z);
    s->spatial.position = pos;
    impl_->refreshAudibility(s);
    return true;
}

bool AudioWorld::setSourceDirection(std::uint32_t id, const Vec3& dir, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    if (!impl_->cfg.spatialization) { setErr(err, "spatialization disabled in AudioConfig"); return false; }
    ma_sound_set_direction(s->sound.get(), dir.x, dir.y, dir.z);
    s->spatial.direction = dir;
    return true;
}

bool AudioWorld::setSourceCone(std::uint32_t id, float innerRadians, float outerRadians,
                               float outerGain, std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    if (!impl_->cfg.spatialization) { setErr(err, "spatialization disabled in AudioConfig"); return false; }
    if (!(innerRadians >= 0.0f && outerRadians >= innerRadians &&
          outerRadians <= 6.283185f && outerGain >= 0.0f && outerGain <= 1.0f)) {
        setErr(err, "invalid cone parameters"); return false;
    }
    ma_sound_set_cone(s->sound.get(), innerRadians, outerRadians, outerGain);
    return true;
}

bool AudioWorld::setSourceAttenuation(std::uint32_t id, AttenuationModel model,
                                      float minDistance, float maxDistance, float rolloff,
                                      std::string* err) {
    auto* s = impl_->at(id);
    if (!s) { setErr(err, "invalid source id"); return false; }
    if (!impl_->cfg.spatialization) { setErr(err, "spatialization disabled in AudioConfig"); return false; }
    if (!(minDistance > 0.0f && maxDistance > minDistance && rolloff > 0.0f && rolloff <= 16.0f)) {
        setErr(err, "attenuation: require 0 < minDistance < maxDistance, 0 < rolloff <= 16");
        return false;
    }
    ma_sound_set_attenuation_model(s->sound.get(),
        static_cast<ma_attenuation_model>(static_cast<int>(model)));
    ma_sound_set_min_distance(s->sound.get(), minDistance);
    ma_sound_set_max_distance(s->sound.get(), maxDistance);
    ma_sound_set_rolloff(s->sound.get(), rolloff);
    s->spatial.model = model;
    s->spatial.minDistance = minDistance;
    s->spatial.maxDistance = maxDistance;
    s->spatial.rolloff = rolloff;
    impl_->refreshAudibility(s);
    return true;
}

bool AudioWorld::spatialDesc(std::uint32_t id, SpatialDesc* out) const {
    auto* s = impl_->at(id);
    if (!s || !out) return false;
    *out = s->spatial;
    return true;
}

bool AudioWorld::setListenerPosition(const Vec3& pos, std::string* err) {
    if (!impl_->ok) { setErr(err, "world not initialised: " + impl_->error); return false; }
    ma_engine_listener_set_position(&impl_->engine, 0, pos.x, pos.y, pos.z);
    impl_->listenerPos = pos;
    impl_->refreshAll();
    return true;
}

bool AudioWorld::setListenerDirection(const Vec3& dir, std::string* err) {
    if (!impl_->ok) { setErr(err, "world not initialised: " + impl_->error); return false; }
    if (dir.x == 0.0f && dir.y == 0.0f && dir.z == 0.0f) { setErr(err, "listener direction is zero"); return false; }
    ma_engine_listener_set_direction(&impl_->engine, 0, dir.x, dir.y, dir.z);
    impl_->listenerDir = dir;
    return true;
}

bool AudioWorld::setListenerWorldUp(const Vec3& up, std::string* err) {
    if (!impl_->ok) { setErr(err, "world not initialised: " + impl_->error); return false; }
    if (up.x == 0.0f && up.y == 0.0f && up.z == 0.0f) { setErr(err, "listener world up is zero"); return false; }
    ma_engine_listener_set_world_up(&impl_->engine, 0, up.x, up.y, up.z);
    impl_->listenerUp = up;
    return true;
}

Vec3 AudioWorld::listenerPosition() const { return impl_->listenerPos; }
Vec3 AudioWorld::listenerDirection() const { return impl_->listenerDir; }
Vec3 AudioWorld::listenerWorldUp() const { return impl_->listenerUp; }

CacheStats AudioWorld::cacheStats() const {
    CacheStats st;
    st.clips   = static_cast<std::uint32_t>(impl_->clips.size());
    st.misses  = impl_->misses;
    st.hits    = impl_->hits;
    st.sources = static_cast<std::uint32_t>(impl_->sources.size());
    for (const auto& kv : impl_->clips) st.maxClipRefs = (std::max)(st.maxClipRefs, kv.second.refs);
    return st;
}

std::uint32_t AudioWorld::clipCount() const {
    return static_cast<std::uint32_t>(impl_->clips.size());
}

std::uint32_t AudioWorld::clipRefCount(const std::string& wavPath) const {
    auto it = impl_->clips.find(wavPath);
    return it == impl_->clips.end() ? 0u : it->second.refs;
}

std::uint64_t AudioWorld::readFrames(float* dst, std::uint64_t frameCount) {
    if (!impl_->ok) return 0;
    if (!impl_->cfg.offline) {
        impl_->error = "readFrames requires an offline (noDevice) world";
        return 0;
    }
    // M-S2: frozen == paused. Do not advance the engine clock; return 0 so a
    // caller cannot observe (or consume) the same mix twice. stepFrames() is the
    // only path that advances while frozen (single-step debugging).
    if (AudioDebugController::get().frozen()) return 0;
    ma_uint64 read = 0;
    ma_result r = ma_engine_read_pcm_frames(&impl_->engine, dst, frameCount, &read);
    if (r != MA_SUCCESS) {
        impl_->error = "ma_engine_read_pcm_frames failed (" + maCode(r) + ")";
        return 0;
    }
    return static_cast<std::uint64_t>(read);
}

std::uint64_t AudioWorld::stepFrames(float* dst, std::uint64_t frameCount) {
    if (!impl_->ok) return 0;
    if (!impl_->cfg.offline) {
        impl_->error = "stepFrames requires an offline (noDevice) world";
        return 0;
    }
    // Advance regardless of frozen (single-step). Same code path as readFrames
    // minus the freeze guard.
    ma_uint64 read = 0;
    ma_result r = ma_engine_read_pcm_frames(&impl_->engine, dst, frameCount, &read);
    if (r != MA_SUCCESS) {
        impl_->error = "ma_engine_read_pcm_frames failed (" + maCode(r) + ")";
        return 0;
    }
    return static_cast<std::uint64_t>(read);
}

std::vector<SourceSnapshot> AudioWorld::snapshot() const {
    std::vector<SourceSnapshot> out;
    const auto& dbg = AudioDebugController::get();
    for (const auto& s : impl_->sources) {
        if (!s || !s->sound) continue;
        SourceSnapshot sn;
        sn.id              = s->id;
        sn.entity          = s->entity.empty() ? ("src-" + std::to_string(s->id)) : s->entity;
        sn.position        = s->spatial.position;
        sn.listenerPosition = impl_->listenerPos;
        sn.distance        = distanceBetween(s->spatial.position, impl_->listenerPos);
        sn.baseGain        = s->gain;
        sn.effectiveGain   = s->effectiveGain;
        sn.playing         = ma_sound_is_playing(s->sound.get()) == MA_TRUE;
        sn.audible         = s->spatial.audible;
        sn.soloed          = dbg.hasSolo() && static_cast<std::int64_t>(s->id) == dbg.soloId();
        sn.muted           = dbg.hasSolo() && !sn.soloed;
        sn.clip            = s->clip;
        sn.model           = s->spatial.model;
        out.push_back(sn);
    }
    return out;
}

void AudioWorld::bindToDebug()   { AudioDebugController::get().bind(this); }
void AudioWorld::unbindFromDebug() { AudioDebugController::get().unbind(this); }

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
