// P8 M-S0/M-S1: audio-tools — A-track tool deliverable (zero graphics symbols).
//   selftest                      deterministic assertions (offline engine + PCM render)
//   probe <wav>                   read-only WAV header probe
//   dump <wav> <outdir> [frames]  M-S0 offline f32 PCM dump -> waveform.tsv + provenance.txt
//   spatial <outdir>              M-S1 3D attenuation sweep + source mixing + clip cache
//   check <outdir>                re-render FROM provenance, per-sample compare (anti-false-green)
//   play <wav>                    realtime playback on the default device
// Exit codes: 0 = ok, 1 = runtime/assert failure, 2 = usage error.
#include "nl/audio/AudioWorld.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef NL_AUDIO_ASSET_DIR
#define NL_AUDIO_ASSET_DIR "assets/audio"
#endif
#ifndef NL_AUDIO_ROOT
#define NL_AUDIO_ROOT "."
#endif

namespace fs = std::filesystem;
using nl::audio::AttenuationModel;
using nl::audio::AudioConfig;
using nl::audio::AudioWorld;
using nl::audio::CacheStats;
using nl::audio::ClipDesc;
using nl::audio::SourceDesc;
using nl::audio::SpatialDesc;
using nl::audio::Vec3;

namespace {

const std::uint32_t kSampleRate = 48000;
const std::uint32_t kChannels   = 2;
const std::uint64_t kMaxFrames  = 48000 * 10;

// M-S1 sweep parameters. warmup must exceed ma_spatializer's
// gainSmoothTimeInFrames (360 @ 48 kHz) or we would measure the ramp, not the gain.
const std::uint32_t kWarmupFrames  = 512;
const std::uint32_t kMeasureFrames = 1024;
const std::uint32_t kMixFrames     = 512;
const std::uint32_t kDistanceSteps = 21;
const float kAttMinDistance = 1.0f;
const float kAttMaxDistance = 20.0f;
// rolloff=1 makes miniaudio's inverse and exponential curves mathematically
// identical (both minDistance/distance) -> only 3 distinguishable curves.
// 0.5 separates all four while keeping linear above zero until maxDistance.
const float kAttRolloff     = 0.5f;
const float kAttDistanceMax = 30.0f;   // grid ends beyond maxDistance -> silence proof

std::string defaultWav()  { return std::string(NL_AUDIO_ASSET_DIR) + "/test-tone.wav"; }
std::string defaultWavB() { return std::string(NL_AUDIO_ASSET_DIR) + "/test-tone-b.wav"; }

// Provenance must be portable: never persist this machine's absolute worktree path,
// otherwise `check` on the showcase (or any other checkout) cannot reopen the clip.
std::string portableWav(const std::string& wav) {
    std::error_code e1, e2, e3;
    const fs::path wa = fs::weakly_canonical(fs::path(wav), e1);
    const fs::path ra = fs::weakly_canonical(fs::path(NL_AUDIO_ROOT), e2);
    const fs::path rel = fs::relative(wa, ra, e3);
    if (!e1 && !e2 && !e3 && !rel.empty() && rel.string().rfind("..", 0) != 0) {
        return rel.generic_string();
    }
    return fs::path(wav).generic_string();
}

std::string resolveWav(const std::string& wav) {
    const fs::path p(wav);
    if (p.is_absolute()) return wav;
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(fs::path(NL_AUDIO_ROOT) / p, ec);
    return ec ? (fs::path(NL_AUDIO_ROOT) / p).string() : full.string();
}

void usage() {
    std::printf(
        "audio-tools - nl-engine P8 audio track (M-S0/M-S1)\n"
        "usage:\n"
        "  audio-tools selftest                       deterministic unit tests (exit 0 = PASS)\n"
        "  audio-tools probe <wav>                    read-only WAV header probe\n"
        "  audio-tools dump <wav> <outdir> [frames]   M-S0 offline f32 PCM dump + provenance\n"
        "  audio-tools spatial <outdir>               M-S1 attenuation/mixing/cache dump\n"
        "  audio-tools check <outdir>                 re-render from provenance, per-sample compare\n"
        "  audio-tools play <wav>                     realtime playback on default device\n"
        "exit codes: 0=ok 1=runtime/assert failure 2=usage error\n");
}

std::uint64_t fnv1a64(const std::string& bytes) {
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::string hex64(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

bool readFile(const fs::path& p, std::string* out, std::string* err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { if (err) *err = "cannot open: " + p.string(); return false; }
    std::ostringstream ss; ss << f.rdbuf();
    *out = ss.str();
    return true;
}

bool writeFile(const fs::path& p, const std::string& data, std::string* err) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write: " + p.string(); return false; }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) { if (err) *err = "write failed: " + p.string(); return false; }
    return true;
}

std::string fmtFloat(float v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v));
    return std::string(b);
}

std::string fmtDouble(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.9g", v);
    return std::string(b);
}

using KV = std::map<std::string, std::string>;

bool parseKV(const std::string& text, KV* out, std::string* err) {
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            if (err) *err = "provenance: malformed line: " + line;
            return false;
        }
        (*out)[line.substr(0, pos)] = line.substr(pos + 1);
    }
    return true;
}

struct RenderResult {
    bool ok = false;
    std::string err;
    std::vector<float> pcm;   // interleaved, frames * kChannels
    std::uint64_t frames = 0;
};

RenderResult render(const std::string& wav, std::uint64_t frames, float gain, bool loop) {
    RenderResult r;
    AudioConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.channels   = kChannels;
    cfg.offline    = true;
    AudioWorld world(cfg);
    if (!world.ok()) { r.err = "noDevice engine: " + world.error(); return r; }
    if (world.sampleRate() != kSampleRate || world.channels() != kChannels) {
        r.err = "engine format mismatch";
        return r;
    }
    std::string e;
    const std::uint32_t id = world.createSource(wav, &e);
    if (!id) { r.err = e; return r; }
    if (!world.configureSource(id, gain, loop, &e)) { r.err = e; return r; }
    if (!world.startSource(id, &e)) { r.err = e; return r; }

    r.pcm.assign(static_cast<std::size_t>(frames * kChannels), 0.0f);
    std::uint64_t done = 0;
    while (done < frames) {
        const std::uint64_t n = world.readFrames(
            r.pcm.data() + done * kChannels, frames - done);
        if (n == 0) break;
        done += n;
    }
    if (done != frames) {
        r.err = "offline read short: " + std::to_string(done) + " of " + std::to_string(frames);
        return r;
    }
    r.frames = frames;
    r.ok = true;
    return r;
}

bool dumpCore(const std::string& wav, const fs::path& outdir, std::uint64_t frames,
              std::string* summary, std::string* err) {
    ClipDesc d;
    if (!AudioWorld::probeWav(wav, &d, err)) return false;
    if (frames == 0) frames = d.frames;
    if (frames == 0 || frames > kMaxFrames) {
        if (err) *err = "invalid frame count";
        return false;
    }
    const RenderResult r = render(wav, frames, 1.0f, false);
    if (!r.ok) { if (err) *err = r.err; return false; }

    std::error_code ec;
    fs::create_directories(outdir, ec);
    if (ec) { if (err) *err = "cannot create outdir: " + outdir.string(); return false; }

    std::string tsv = "# frame left right\n";
    tsv.reserve(static_cast<std::size_t>(frames) * 30);
    char line[128];
    float peak = 0.0f;
    double sumSq = 0.0;
    std::uint64_t nonZero = 0;
    for (std::uint64_t i = 0; i < frames; ++i) {
        const float L = r.pcm[static_cast<std::size_t>(i * kChannels)];
        const float R = r.pcm[static_cast<std::size_t>(i * kChannels + 1)];
        std::snprintf(line, sizeof(line), "%llu %s %s\n",
                      static_cast<unsigned long long>(i), fmtFloat(L).c_str(), fmtFloat(R).c_str());
        tsv += line;
        peak = (std::max)(peak, (std::max)(std::fabs(L), std::fabs(R)));
        sumSq += static_cast<double>(L) * L + static_cast<double>(R) * R;
        if (L != 0.0f || R != 0.0f) ++nonZero;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(frames * kChannels));
    const std::string hash = hex64(fnv1a64(tsv));

    if (!writeFile(outdir / "waveform.tsv", tsv, err)) return false;
    std::string prov;
    prov += "format=1\n";
    prov += "tool=audio-tools\n";
    prov += "milestone=M-S0\n";
    prov += "engine=noDevice\n";
    prov += "wav=" + portableWav(wav) + "\n";
    prov += "sampleRate=" + std::to_string(kSampleRate) + "\n";
    prov += "channels=" + std::to_string(kChannels) + "\n";
    prov += "frames=" + std::to_string(frames) + "\n";
    prov += "gain=" + fmtFloat(1.0f) + "\n";
    prov += "loop=0\n";
    prov += "flags=decode|no-spatialization\n";
    prov += "waveformHash=" + hash + "\n";
    if (!writeFile(outdir / "provenance.txt", prov, err)) return false;

    if (summary) {
        std::snprintf(line, sizeof(line),
                      "dump: frames=%llu rate=%u ch=%u peak=%.6f rms=%.6f nonzero=%llu",
                      static_cast<unsigned long long>(frames), kSampleRate, kChannels,
                      static_cast<double>(peak), rms, static_cast<unsigned long long>(nonZero));
        *summary = std::string(line) + "\ndump: waveform.tsv bytes=" +
                   std::to_string(tsv.size()) + " hash=" + hash + "\ndump: PASS";
    }
    return true;
}

bool checkCore(const fs::path& outdir, std::string* summary, std::string* err) {
    std::string text;
    if (!readFile(outdir / "provenance.txt", &text, err)) return false;
    KV kv;
    if (!parseKV(text, &kv, err)) return false;
    auto get = [&](const char* k) -> std::string {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    };

    if (get("format") != "1")              { if (err) *err = "provenance: unsupported format"; return false; }
    if (get("engine") != "noDevice")       { if (err) *err = "provenance: engine must be noDevice"; return false; }
    const unsigned long rate = std::strtoul(get("sampleRate").c_str(), nullptr, 10);
    if (rate != kSampleRate) {
        if (err) *err = "provenance: sampleRate " + std::to_string(rate) +
                        " != engine " + std::to_string(kSampleRate);
        return false;
    }
    const unsigned long ch = std::strtoul(get("channels").c_str(), nullptr, 10);
    if (ch != kChannels) { if (err) *err = "provenance: channels mismatch"; return false; }
    const unsigned long long frames = std::strtoull(get("frames").c_str(), nullptr, 10);
    if (frames == 0 || frames > kMaxFrames) { if (err) *err = "provenance: invalid frames"; return false; }
    const float gain = std::strtof(get("gain").c_str(), nullptr);
    if (!(gain >= 0.0f && gain <= 2.0f)) { if (err) *err = "provenance: gain out of [0,2]"; return false; }
    const std::string loopStr = get("loop");
    if (loopStr != "0" && loopStr != "1") { if (err) *err = "provenance: invalid loop"; return false; }
    const std::string wavRaw = get("wav");
    if (wavRaw.empty()) { if (err) *err = "provenance: missing wav"; return false; }
    const std::string wav = resolveWav(wavRaw);
    ClipDesc d;
    if (!AudioWorld::probeWav(wav, &d, err)) return false;

    std::string tsv;
    if (!readFile(outdir / "waveform.tsv", &tsv, err)) return false;
    const std::string hash = hex64(fnv1a64(tsv));
    if (hash != get("waveformHash")) { if (err) *err = "waveform hash mismatch"; return false; }

    std::istringstream is(tsv);
    std::vector<float> ref;
    ref.reserve(static_cast<std::size_t>(frames * kChannels));
    std::string ln;
    while (std::getline(is, ln)) {
        if (ln.empty() || ln[0] == '#') continue;
        unsigned long long idx = 0;
        char lb[64] = {}, rb[64] = {};
        if (std::sscanf(ln.c_str(), "%llu %63s %63s", &idx, lb, rb) == 3) {
            ref.push_back(std::strtof(lb, nullptr));
            ref.push_back(std::strtof(rb, nullptr));
        }
    }
    if (ref.size() != frames * kChannels) {
        if (err) *err = "waveform.tsv sample count mismatch";
        return false;
    }

    // Anti-false-green: DO NOT trust the stored waveform. Re-render from the
    // provenance parameters and compare every single float bit-for-bit.
    const RenderResult r = render(wav, frames, gain, loopStr == "1");
    if (!r.ok) { if (err) *err = r.err; return false; }
    for (std::size_t i = 0; i < ref.size(); ++i) {
        if (r.pcm[i] != ref[i]) {
            if (err) *err = "sample mismatch at index " + std::to_string(i);
            return false;
        }
    }

    if (summary) {
        *summary = "check: PASS (frames=" + std::to_string(frames) +
                   ", samples=" + std::to_string(ref.size()) +
                   ", hash=" + hash + ", per-sample identical)";
    }
    return true;
}

// ================== M-S1: 3D spatialization evidence ==================

struct SpatialParams {
    std::string   wavA, wavB;
    std::uint32_t warmup      = kWarmupFrames;
    std::uint32_t measure     = kMeasureFrames;
    std::uint32_t mixFrames   = kMixFrames;
    std::uint32_t steps       = kDistanceSteps;
    float         minDistance = kAttMinDistance;
    float         maxDistance = kAttMaxDistance;
    float         rolloff     = kAttRolloff;
    float         distanceMax = kAttDistanceMax;
};

const AttenuationModel kModels[4] = {
    AttenuationModel::None, AttenuationModel::Inverse,
    AttenuationModel::Linear, AttenuationModel::Exponential
};
const char* const kModelNames[4] = {"none", "inverse", "linear", "exponential"};

SpatialParams defaultSpatialParams() {
    SpatialParams p;
    p.wavA = defaultWav();
    p.wavB = defaultWavB();
    return p;
}

// One (model, distance) point. Listener on +Z, source at the origin, listener
// forward = (0,0,-1): the source is dead ahead, so panning is symmetric and the
// measured RMS is the pure distance gain applied to the SAME clip window.
// warmup > ma_spatializer gainSmoothTimeInFrames(360) skips the gain ramp.
struct RmsResult { bool ok = false; std::string err; double rms = 0.0; };

RmsResult measureRms(const SpatialParams& p, const std::string& wav,
                     AttenuationModel model, float distance) {
    RmsResult out;
    AudioConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.channels   = kChannels;
    cfg.offline    = true;
    cfg.spatialization = true;
    AudioWorld w(cfg);
    if (!w.ok()) { out.err = "noDevice engine: " + w.error(); return out; }
    std::string e;
    if (!w.setListenerDirection(Vec3{0.0f, 0.0f, -1.0f}, &e) ||
        !w.setListenerWorldUp(Vec3{0.0f, 1.0f, 0.0f}, &e)) { out.err = e; return out; }
    const std::uint32_t id = w.createSource(wav, &e);
    if (!id) { out.err = e; return out; }
    if (!w.setSourcePosition(id, Vec3{0.0f, 0.0f, 0.0f}, &e) ||
        !w.setSourceAttenuation(id, model, p.minDistance, p.maxDistance, p.rolloff, &e) ||
        !w.setListenerPosition(Vec3{0.0f, 0.0f, distance}, &e) ||
        !w.configureSource(id, 1.0f, false, &e) ||
        !w.startSource(id, &e)) { out.err = e; return out; }

    std::vector<float> warm(static_cast<std::size_t>(p.warmup) * kChannels, 0.0f);
    if (w.readFrames(warm.data(), p.warmup) != p.warmup) {
        out.err = "warmup read short"; return out;
    }
    std::vector<float> buf(static_cast<std::size_t>(p.measure) * kChannels, 0.0f);
    if (w.readFrames(buf.data(), p.measure) != p.measure) {
        out.err = "measure read short"; return out;
    }
    double sum = 0.0;
    for (float v : buf) sum += static_cast<double>(v) * v;
    out.rms = std::sqrt(sum / static_cast<double>(buf.size()));
    out.ok = true;
    return out;
}

// Concurrent-source mixing. Both sources use model `none` (miniaudio's
// unspatialized fast path), so the node graph sum can be checked sample-for-sample
// against the two solo renders produced by the exact same code path.
struct PcmResult { bool ok = false; std::string err; std::vector<float> pcm; };

PcmResult renderSpatialPcm(const std::vector<std::string>& wavs,
                           std::uint32_t frames, const SpatialParams& p) {
    PcmResult out;
    AudioConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.channels   = kChannels;
    cfg.offline    = true;
    cfg.spatialization = true;
    AudioWorld w(cfg);
    if (!w.ok()) { out.err = "noDevice engine: " + w.error(); return out; }
    std::string e;
    for (const auto& wav : wavs) {
        const std::uint32_t id = w.createSource(wav, &e);
        if (!id) { out.err = e; return out; }
        if (!w.setSourceAttenuation(id, AttenuationModel::None,
                                    p.minDistance, p.maxDistance, p.rolloff, &e) ||
            !w.startSource(id, &e)) { out.err = e; return out; }
    }
    out.pcm.assign(static_cast<std::size_t>(frames) * kChannels, 0.0f);
    std::uint64_t done = 0;
    while (done < frames) {
        const std::uint64_t n = w.readFrames(out.pcm.data() + done * kChannels, frames - done);
        if (n == 0) break;
        done += n;
    }
    if (done != frames) { out.err = "spatial read short"; return out; }
    out.ok = true;
    return out;
}

struct MixResult {
    bool ok = false;
    std::string err;
    PcmResult a, b, mix;
    double maxAbsDiff = 0.0;
    std::uint64_t exactSamples = 0;
};

MixResult measureMix(const SpatialParams& p) {
    MixResult m;
    m.a   = renderSpatialPcm(std::vector<std::string>{p.wavA}, p.mixFrames, p);
    m.b   = renderSpatialPcm(std::vector<std::string>{p.wavB}, p.mixFrames, p);
    m.mix = renderSpatialPcm(std::vector<std::string>{p.wavA, p.wavB}, p.mixFrames, p);
    if (!m.a.ok || !m.b.ok || !m.mix.ok) {
        m.err = !m.a.ok ? m.a.err : (!m.b.ok ? m.b.err : m.mix.err);
        return m;
    }
    for (std::size_t i = 0; i < m.mix.pcm.size(); ++i) {
        const float sum = m.a.pcm[i] + m.b.pcm[i];
        const double diff = std::fabs(static_cast<double>(m.mix.pcm[i]) - sum);
        if (diff > m.maxAbsDiff) m.maxAbsDiff = diff;
        if (m.mix.pcm[i] == sum) ++m.exactSamples;
    }
    m.ok = true;
    return m;
}

struct CacheResult {
    bool ok = false;
    std::string err;
    CacheStats st{};
    std::uint32_t refA = 0, refB = 0;
};

CacheResult measureCache(const SpatialParams& p) {
    CacheResult c;
    AudioConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.channels   = kChannels;
    cfg.offline    = true;
    cfg.spatialization = true;
    AudioWorld w(cfg);
    if (!w.ok()) { c.err = "noDevice engine: " + w.error(); return c; }
    const std::string paths[4] = {p.wavA, p.wavB, p.wavA, p.wavB};
    std::string e;
    for (const auto& path : paths) {
        if (!w.createSource(path, &e)) { c.err = e; return c; }
    }
    c.st   = w.cacheStats();
    c.refA = w.clipRefCount(p.wavA);
    c.refB = w.clipRefCount(p.wavB);
    c.ok   = true;
    return c;
}

struct AttResult { bool ok = false; std::string err; std::string tsv; };

AttResult buildAttenuation(const SpatialParams& p) {
    AttResult a;
    a.tsv = "# distance none inverse linear exponential\n";
    for (std::uint32_t i = 0; i < p.steps; ++i) {
        const float d = p.distanceMax * static_cast<float>(i) /
                        static_cast<float>(p.steps - 1);
        std::string line = fmtDouble(d);
        for (int m = 0; m < 4; ++m) {
            const RmsResult r = measureRms(p, p.wavA, kModels[m], d);
            if (!r.ok) {
                a.err = "model=" + std::string(kModelNames[m]) + " d=" + fmtDouble(d) +
                        ": " + r.err;
                return a;
            }
            line += " " + fmtDouble(r.rms);
        }
        a.tsv += line + "\n";
    }
    a.ok = true;
    return a;
}

std::string buildMixingTsv(const MixResult& m, const SpatialParams& p) {
    std::string s = "# frame a b mix sum (left channel, f32)\n";
    char line[256];
    for (std::uint32_t i = 0; i < p.mixFrames; ++i) {
        const std::size_t k = static_cast<std::size_t>(i) * kChannels;
        const float a = m.a.pcm[k], b = m.b.pcm[k], mix = m.mix.pcm[k];
        std::snprintf(line, sizeof(line), "%u %s %s %s %s\n", i,
                      fmtFloat(a).c_str(), fmtFloat(b).c_str(),
                      fmtFloat(mix).c_str(), fmtFloat(a + b).c_str());
        s += line;
    }
    return s;
}

std::string buildProvenanceS1(const SpatialParams& p, const std::string& attHash,
                              const std::string& mixHash, const MixResult& mix,
                              const CacheResult& cache) {
    std::string s;
    s += "format=2\n";
    s += "tool=audio-tools\n";
    s += "milestone=M-S1\n";
    s += "engine=noDevice|spatialization\n";
    s += "wavA=" + portableWav(p.wavA) + "\n";
    s += "wavB=" + portableWav(p.wavB) + "\n";
    s += "sampleRate=" + std::to_string(kSampleRate) + "\n";
    s += "channels=" + std::to_string(kChannels) + "\n";
    s += "warmupFrames=" + std::to_string(p.warmup) + "\n";
    s += "measureFrames=" + std::to_string(p.measure) + "\n";
    s += "mixFrames=" + std::to_string(p.mixFrames) + "\n";
    s += "distanceSteps=" + std::to_string(p.steps) + "\n";
    s += "distanceMax=" + fmtDouble(p.distanceMax) + "\n";
    s += "minDistance=" + fmtDouble(p.minDistance) + "\n";
    s += "maxDistance=" + fmtDouble(p.maxDistance) + "\n";
    s += "rolloff=" + fmtDouble(p.rolloff) + "\n";
    s += "models=none,inverse,linear,exponential\n";
    s += "attenuationHash=" + attHash + "\n";
    s += "mixingHash=" + mixHash + "\n";
    s += "mixingMaxAbsDiff=" + fmtDouble(mix.maxAbsDiff) + "\n";
    s += "mixingExactSamples=" + std::to_string(mix.exactSamples) + "\n";
    s += "clips=" + std::to_string(cache.st.clips) + "\n";
    s += "misses=" + std::to_string(cache.st.misses) + "\n";
    s += "hits=" + std::to_string(cache.st.hits) + "\n";
    s += "sources=" + std::to_string(cache.st.sources) + "\n";
    s += "maxClipRefs=" + std::to_string(cache.st.maxClipRefs) + "\n";
    return s;
}

bool cacheInvariantsHold(const CacheResult& c) {
    return c.st.clips == 2 && c.st.misses == 2 && c.st.hits == 2 &&
           c.st.sources == 4 && c.st.maxClipRefs == 2 &&
           c.refA == 2 && c.refB == 2;
}

bool dumpSpatialCore(const fs::path& outdir, std::string* summary, std::string* err) {
    const SpatialParams p = defaultSpatialParams();
    ClipDesc d;
    if (!AudioWorld::probeWav(p.wavA, &d, err) ||
        !AudioWorld::probeWav(p.wavB, &d, err)) return false;

    const AttResult att = buildAttenuation(p);
    if (!att.ok) { if (err) *err = "attenuation sweep: " + att.err; return false; }
    const MixResult mix = measureMix(p);
    if (!mix.ok) { if (err) *err = "mixing: " + mix.err; return false; }
    if (!(mix.maxAbsDiff <= 1e-6)) {
        if (err) *err = "mixing is not linear: maxAbsDiff=" + fmtDouble(mix.maxAbsDiff);
        return false;
    }
    const CacheResult cache = measureCache(p);
    if (!cache.ok) { if (err) *err = "clip cache: " + cache.err; return false; }
    if (!cacheInvariantsHold(cache)) {
        if (err) *err = "clip cache invariants violated";
        return false;
    }

    const std::string mixTsv = buildMixingTsv(mix, p);
    const std::string attHash = hex64(fnv1a64(att.tsv));
    const std::string mixHash = hex64(fnv1a64(mixTsv));
    std::error_code ec;
    fs::create_directories(outdir, ec);
    if (ec) { if (err) *err = "cannot create outdir: " + outdir.string(); return false; }
    if (!writeFile(outdir / "attenuation.tsv", att.tsv, err) ||
        !writeFile(outdir / "mixing.tsv", mixTsv, err) ||
        !writeFile(outdir / "provenance.txt",
                   buildProvenanceS1(p, attHash, mixHash, mix, cache), err)) return false;

    if (summary) {
        char b[256];
        std::snprintf(b, sizeof(b),
            "spatial: steps=%u dMax=%s minDist=%s maxDist=%s rolloff=%s warmup=%u measure=%u",
            p.steps, fmtDouble(p.distanceMax).c_str(), fmtDouble(p.minDistance).c_str(),
            fmtDouble(p.maxDistance).c_str(), fmtDouble(p.rolloff).c_str(),
            p.warmup, p.measure);
        *summary = std::string(b) +
            "\nspatial: attenuation.tsv bytes=" + std::to_string(att.tsv.size()) +
            " hash=" + attHash +
            "\nspatial: mixing.tsv bytes=" + std::to_string(mixTsv.size()) +
            " hash=" + mixHash +
            "\nspatial: mix maxAbsDiff=" + fmtDouble(mix.maxAbsDiff) +
            " exactSamples=" + std::to_string(mix.exactSamples) +
            "\nspatial: cache clips=" + std::to_string(cache.st.clips) +
            " misses=" + std::to_string(cache.st.misses) +
            " hits=" + std::to_string(cache.st.hits) +
            " sources=" + std::to_string(cache.st.sources) +
            "\nspatial: PASS";
    }
    return true;
}

bool checkSpatialCore(const fs::path& outdir, std::string* summary, std::string* err) {
    std::string text;
    if (!readFile(outdir / "provenance.txt", &text, err)) return false;
    KV kv;
    if (!parseKV(text, &kv, err)) return false;
    auto get = [&](const char* k) -> std::string {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    };
    auto bad = [&](const std::string& why) { if (err) *err = "provenance: " + why; return false; };

    if (get("format") != "2")                         return bad("expected format=2");
    if (get("tool") != "audio-tools")                 return bad("tool mismatch");
    if (get("milestone") != "M-S1")                   return bad("milestone mismatch");
    if (get("engine") != "noDevice|spatialization")   return bad("engine mismatch");
    if (get("sampleRate") != std::to_string(kSampleRate)) return bad("sampleRate mismatch");
    if (get("channels") != std::to_string(kChannels)) return bad("channels mismatch");
    if (get("models") != "none,inverse,linear,exponential") return bad("models mismatch");

    SpatialParams p;
    p.wavA = resolveWav(get("wavA"));
    p.wavB = resolveWav(get("wavB"));
    if (get("wavA").empty() || get("wavB").empty()) return bad("missing wavA/wavB");
    p.warmup      = static_cast<std::uint32_t>(std::strtoul(get("warmupFrames").c_str(), nullptr, 10));
    p.measure     = static_cast<std::uint32_t>(std::strtoul(get("measureFrames").c_str(), nullptr, 10));
    p.mixFrames   = static_cast<std::uint32_t>(std::strtoul(get("mixFrames").c_str(), nullptr, 10));
    p.steps       = static_cast<std::uint32_t>(std::strtoul(get("distanceSteps").c_str(), nullptr, 10));
    p.distanceMax = std::strtof(get("distanceMax").c_str(), nullptr);
    p.minDistance = std::strtof(get("minDistance").c_str(), nullptr);
    p.maxDistance = std::strtof(get("maxDistance").c_str(), nullptr);
    p.rolloff     = std::strtof(get("rolloff").c_str(), nullptr);
    if (p.warmup < 360 || p.warmup > 48000 || p.measure == 0 || p.measure > 48000 ||
        p.mixFrames == 0 || p.mixFrames > 48000 || p.steps < 2 || p.steps > 128 ||
        !(p.minDistance > 0.0f && p.maxDistance > p.minDistance && p.rolloff > 0.0f) ||
        !(p.distanceMax > p.maxDistance)) return bad("invalid sweep parameters");

    ClipDesc d;
    if (!AudioWorld::probeWav(p.wavA, &d, err) ||
        !AudioWorld::probeWav(p.wavB, &d, err)) return false;

    std::string attTsv, mixTsv;
    if (!readFile(outdir / "attenuation.tsv", &attTsv, err) ||
        !readFile(outdir / "mixing.tsv", &mixTsv, err)) return false;
    if (hex64(fnv1a64(attTsv)) != get("attenuationHash")) return bad("attenuation hash mismatch");
    if (hex64(fnv1a64(mixTsv)) != get("mixingHash"))      return bad("mixing hash mismatch");

    // Anti-false-green: rebuild EVERY sample from provenance parameters.
    // Comparing the formatted text (not parsed floats) keeps the check bit-strict.
    const AttResult att = buildAttenuation(p);
    if (!att.ok) { if (err) *err = "re-render attenuation: " + att.err; return false; }
    if (att.tsv != attTsv) {
        if (err) *err = "attenuation.tsv mismatch after re-render from provenance";
        return false;
    }
    const MixResult mix = measureMix(p);
    if (!mix.ok) { if (err) *err = "re-render mixing: " + mix.err; return false; }
    const std::string rebuiltMix = buildMixingTsv(mix, p);
    if (rebuiltMix != mixTsv) {
        if (err) *err = "mixing.tsv mismatch after re-render from provenance";
        return false;
    }
    if (get("mixingMaxAbsDiff") != fmtDouble(mix.maxAbsDiff) ||
        get("mixingExactSamples") != std::to_string(mix.exactSamples)) {
        return bad("mixing statistics mismatch after re-render");
    }
    const CacheResult cache = measureCache(p);
    if (!cache.ok || !cacheInvariantsHold(cache)) {
        if (err) *err = "clip cache re-probe failed"; return false;
    }
    if (get("clips") != std::to_string(cache.st.clips) ||
        get("misses") != std::to_string(cache.st.misses) ||
        get("hits") != std::to_string(cache.st.hits) ||
        get("sources") != std::to_string(cache.st.sources) ||
        get("maxClipRefs") != std::to_string(cache.st.maxClipRefs)) {
        return bad("clip cache statistics mismatch after re-probe");
    }

    if (summary) {
        *summary = "check: PASS (format=2, " + std::to_string(p.steps) +
                   " distances x 4 models re-rendered, " +
                   std::to_string(p.mixFrames) +
                   " mixing frames + cache stats byte-identical)";
    }
    return true;
}

int cmdSpatial(const fs::path& outdir) {
    std::string summary, err;
    if (!dumpSpatialCore(outdir, &summary, &err)) {
        std::printf("spatial: FAIL %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", summary.c_str());
    return 0;
}

// ================== end M-S1 ==================

int cmdSelftest() {
    int total = 0, fail = 0;
    auto check = [&](bool ok, const char* name) {
        ++total;
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", name);
        if (!ok) ++fail;
    };
    std::printf("audio selftest (M-S0):\n");
    const std::string wav = defaultWav();
    std::string err;

    ClipDesc d;
    const bool probed = AudioWorld::probeWav(wav, &d, &err);
    check(probed && d.sampleRate == 48000 && d.channels == 1 &&
          d.bitsPerSample == 16 && d.frames == 24000,
          "clip: WAV header anchors (48000/1/16/24000)");
    check(!AudioWorld::probeWav(wav + ".missing", &d, &err) && !err.empty(),
          "clip: missing file explicitly rejected");

    AudioConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.channels   = kChannels;
    cfg.offline    = true;
    AudioWorld w1(cfg);
    check(w1.ok(), "world: noDevice engine init");
    check(w1.sampleRate() == kSampleRate && w1.channels() == kChannels,
          "world: sampleRate/channels readback");
    const std::uint32_t id = w1.createSource(wav, &err);
    check(id != 0, "source: createSource decodes WAV");
    check(w1.configureSource(id, 1.0f, false, &err), "source: configure gain/loop");
    check(w1.startSource(id, &err), "source: start");
    check(w1.isPlaying(id), "source: isPlaying after start");
    check(w1.sourceCount() == 1, "source: count");
    nl::audio::SourceDesc sd;
    check(w1.sourceDesc(id, &sd) && sd.playing && sd.gain == 1.0f && !sd.looping,
          "source: descriptor readback");
    check(w1.setListenerPosition(nl::audio::Vec3{1.0f, 2.0f, 3.0f}, &err) &&
          w1.listenerPosition().x == 1.0f, "listener: position roundtrip");

    const RenderResult a = render(wav, 24000, 1.0f, false);
    const RenderResult b = render(wav, 24000, 1.0f, false);
    check(a.ok && b.ok, "render: two independent offline renders succeeded");
    check(a.ok && b.ok && a.pcm.size() == b.pcm.size() &&
          std::memcmp(a.pcm.data(), b.pcm.data(), a.pcm.size() * sizeof(float)) == 0,
          "render: per-sample deterministic (bitwise)");

    if (a.ok) {
        float peak = 0.0f;
        std::uint64_t nonZero = 0;
        bool lrEqual = true;
        for (std::size_t i = 0; i < a.pcm.size(); i += 2) {
            peak = (std::max)(peak, (std::max)(std::fabs(a.pcm[i]), std::fabs(a.pcm[i + 1])));
            if (a.pcm[i] != 0.0f || a.pcm[i + 1] != 0.0f) ++nonZero;
            if (a.pcm[i] != a.pcm[i + 1]) lrEqual = false;
        }
        check(peak > 0.05f && peak <= 1.0f, "render: peak in (0.05, 1.0]");
        check(nonZero > 20000, "render: non-zero frames");
        check(lrEqual, "render: mono clip expands to identical L/R");
        auto rmsRange = [&](std::size_t begin, std::size_t end) {
            double s = 0.0;
            for (std::size_t i = begin; i < end; ++i) {
                const double v = a.pcm[i * 2];
                s += v * v;
            }
            return std::sqrt(s / static_cast<double>(end - begin));
        };
        const double head = rmsRange(0, 512);
        const double tail = rmsRange(23500, 24000);
        check(head > tail * 4.0, "render: exponential decay envelope (head RMS > 4x tail)");
    } else {
        check(false, "render: waveform anchors");
        check(false, "render: non-zero frames");
        check(false, "render: mono clip expands to identical L/R");
        check(false, "render: exponential decay envelope");
    }

    const fs::path tmp = fs::temp_directory_path() / "nl-audio-s0-selftest";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::string summary;
    check(dumpCore(wav, tmp, 2048, &summary, &err),
          "dump: waveform.tsv + provenance.txt written");
    check(checkCore(tmp, &summary, &err),
          "check: re-render from provenance is per-sample identical");

    std::string prov;
    if (readFile(tmp / "provenance.txt", &prov, &err)) {
        const std::string target = "sampleRate=48000";
        const std::size_t pos = prov.find(target);
        check(pos != std::string::npos, "provenance: sampleRate field present");
        if (pos != std::string::npos) {
            prov.replace(pos, target.size(), "sampleRate=44100");
            writeFile(tmp / "provenance.txt", prov, &err);
            check(!checkCore(tmp, &summary, &err),
                  "check: tampered provenance explicitly rejected (anti-false-green)");
        }
    } else {
        check(false, "provenance: sampleRate field present");
    }

    // ---- M-S1: 3D spatialization / mixing / clip cache ----
    std::printf("audio selftest (M-S1):\n");
    {
        AttenuationModel parsed = AttenuationModel::None;
        check(nl::audio::parseAttenuationModel("exponential", &parsed) &&
              parsed == AttenuationModel::Exponential &&
              std::string(nl::audio::attenuationModelName(parsed)) == "exponential" &&
              !nl::audio::parseAttenuationModel("bogus", &parsed),
              "M-S1 attenuation: model name/parse roundtrip + bogus rejected");

        AudioConfig s1cfg;
        s1cfg.offline = true;
        s1cfg.spatialization = true;
        AudioWorld s1(s1cfg);
        check(s1.ok() && s1.spatializationEnabled(), "M-S1 world: spatialization enabled");
        const std::uint32_t sid = s1.createSource(wav, &err);
        check(sid != 0, "M-S1 source: create in spatialized world");
        check(s1.setSourcePosition(sid, Vec3{2.0f, 3.0f, 4.0f}, &err) &&
              s1.setSourceDirection(sid, Vec3{0.0f, 0.0f, -1.0f}, &err),
              "M-S1 source: position/direction setters");
        SpatialDesc sd1;
        check(s1.spatialDesc(sid, &sd1) && sd1.position.x == 2.0f && sd1.position.z == 4.0f,
              "M-S1 source: spatial descriptor readback");
        check(s1.setSourceAttenuation(sid, AttenuationModel::Linear,
                                      1.0f, 20.0f, 1.0f, &err),
              "M-S1 attenuation: legal parameters accepted");
        check(!s1.setSourceAttenuation(sid, AttenuationModel::Linear,
                                       20.0f, 1.0f, 1.0f, &err),
              "M-S1 attenuation: minDistance >= maxDistance rejected");
        check(s1.setListenerDirection(Vec3{0.0f, 0.0f, -1.0f}, &err) &&
              s1.setListenerWorldUp(Vec3{0.0f, 1.0f, 0.0f}, &err) &&
              s1.listenerDirection().z == -1.0f && s1.listenerWorldUp().y == 1.0f,
              "M-S1 listener: direction/worldUp readback");

        SpatialParams sp = defaultSpatialParams();
        sp.warmup = 384; sp.measure = 256; sp.steps = 3;
        sp.distanceMax = 40.0f; sp.maxDistance = 20.0f;
        const RmsResult nearR = measureRms(sp, wav, AttenuationModel::Linear, 0.0f);
        const RmsResult farR  = measureRms(sp, wav, AttenuationModel::Linear, 40.0f);
        check(nearR.ok && farR.ok && nearR.rms > 0.001 && farR.rms < 1e-7,
              "M-S1 attenuation: beyond maxDistance is silent");
        const RmsResult noneNear = measureRms(sp, wav, AttenuationModel::None, 0.0f);
        const RmsResult noneFar  = measureRms(sp, wav, AttenuationModel::None, 40.0f);
        check(noneNear.ok && noneFar.ok &&
              std::fabs(noneNear.rms - noneFar.rms) < 1e-6,
              "M-S1 attenuation: `none` is flat (no spatialization by design)");
        const CacheResult cr = measureCache(sp);
        check(cr.ok && cacheInvariantsHold(cr),
              "M-S1 clip cache: 2 clips / 2 misses / 2 hits / 4 sources");
        const MixResult mr = measureMix(sp);
        check(mr.ok && mr.maxAbsDiff <= 1e-6,
              "M-S1 mixing: two concurrent sources sum linearly");
    }

    if (fail == 0) {
        std::printf("audio selftest: PASS (%d/%d)\n", total, total);
        return 0;
    }
    std::printf("audio selftest: FAIL (%d/%d)\n", fail, total);
    return 1;
}

int cmdProbe(const std::string& wav) {
    ClipDesc d;
    std::string err;
    if (!AudioWorld::probeWav(wav, &d, &err)) {
        std::printf("probe: FAIL %s\n", err.c_str());
        return 1;
    }
    std::printf("wav        : %s\n", wav.c_str());
    std::printf("sampleRate : %u\n", d.sampleRate);
    std::printf("channels   : %u\n", d.channels);
    std::printf("bits       : %u\n", d.bitsPerSample);
    std::printf("frames     : %llu\n", static_cast<unsigned long long>(d.frames));
    std::printf("duration   : %.6f s\n", d.durationSeconds);
    std::printf("probe: PASS\n");
    return 0;
}

int cmdDump(const std::string& wav, const fs::path& outdir, std::uint64_t frames) {
    std::string summary, err;
    if (!dumpCore(wav, outdir, frames, &summary, &err)) {
        std::printf("dump: FAIL %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", summary.c_str());
    return 0;
}

int cmdCheck(const fs::path& outdir) {
    std::string text, err;
    if (!readFile(outdir / "provenance.txt", &text, &err)) {
        std::printf("check: FAIL %s\n", err.c_str());
        return 1;
    }
    KV kv;
    if (!parseKV(text, &kv, &err)) {
        std::printf("check: FAIL %s\n", err.c_str());
        return 1;
    }
    const std::string fmt = kv.count("format") ? kv["format"] : std::string();
    std::string summary;
    bool ok = false;
    if (fmt == "1")      ok = checkCore(outdir, &summary, &err);
    else if (fmt == "2") ok = checkSpatialCore(outdir, &summary, &err);
    else                 err = "provenance: unknown format '" + fmt + "'";
    if (!ok) {
        std::printf("check: FAIL %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", summary.c_str());
    return 0;
}

int cmdPlay(const std::string& wav) {
    AudioConfig cfg;
    cfg.offline = false;
    AudioWorld world(cfg);
    if (!world.ok()) {
        std::printf("play: FAIL realtime device unavailable: %s\n", world.error().c_str());
        return 1;
    }
    std::string err;
    if (!world.playRealtimeBlocking(wav, &err)) {
        std::printf("play: FAIL %s\n", err.c_str());
        return 1;
    }
    std::printf("play: OK (%s) rate=%u ch=%u\n", wav.c_str(),
                world.sampleRate(), world.channels());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "selftest") {
        if (argc != 2) { usage(); return 2; }
        return cmdSelftest();
    }
    if (cmd == "probe") {
        if (argc != 3) { usage(); return 2; }
        return cmdProbe(argv[2]);
    }
    if (cmd == "dump") {
        if (argc != 4 && argc != 5) { usage(); return 2; }
        const std::uint64_t frames = argc == 5
            ? std::strtoull(argv[4], nullptr, 10) : 0ull;
        return cmdDump(argv[2], fs::path(argv[3]), frames);
    }
    if (cmd == "spatial") {
        if (argc != 3) { usage(); return 2; }
        return cmdSpatial(fs::path(argv[2]));
    }
    if (cmd == "check") {
        if (argc != 3) { usage(); return 2; }
        return cmdCheck(fs::path(argv[2]));
    }
    if (cmd == "play") {
        if (argc != 3) { usage(); return 2; }
        return cmdPlay(argv[2]);
    }
    std::printf("unknown subcommand: %s\n", cmd.c_str());
    usage();
    return 2;
}
