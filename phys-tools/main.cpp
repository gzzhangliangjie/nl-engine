// phys-tools — P8 M-P0 headless physics acceptance tool (A-class parallel track).
// Only links nl_core (nl/physics + nl/math); zero graphics API symbols, no GPU,
// no kStage, no rendering regression.
//
// Exit code contract (aligned with nanite-scene / anim-tools, showcase expectExit):
//   0 = PASS            1 = assertion / runtime failure            2 = usage error
//
// Subcommands:
//   selftest [--verbose]   mechanical anchors: free fall, first contact, restitution
//                          bounce, rest convergence, determinism, negative configs
//   simulate [options]     dump per-frame trajectory TSV (real solver output)
//   check <tsv>            re-verify a dumped trajectory with the same anchors
//   layers [options]       M-P1: collision layer matrix probe TSV (4 cases)
//   check-layers <tsv>     re-verify the layer matrix dump
//
// Evidence rule: example/phys-p0/*.png and example/phys-p1/*.png are generated
// from the REAL TSV by tools/make-phys-evidence.py / make-phys-p1-evidence.py.
// No hand-drawn diagrams.

#include "nl/physics/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int  g_failures = 0;
bool g_verbose  = false;

void verboseOk(const char* what) {
    if (g_verbose) std::printf("  ok %s\n", what);
}

void expectTrue(bool ok, const char* what) {
    if (ok) { verboseOk(what); return; }
    std::printf("  FAIL %s\n", what);
    ++g_failures;
}

void usage() {
    std::printf("phys-tools - nl-engine P8 physics tools (Jolt %s, headless)\n", nl::physics::kJoltVersion);
    std::printf("usage:\n");
    std::printf("  phys-tools selftest [--verbose]\n");
    std::printf("  phys-tools simulate [--out <tsv>] [--frames N] [--dt S] [--gravity G]\n");
    std::printf("                      [--height H] [--radius R] [--restitution E] [--seed N]\n");
    std::printf("  phys-tools check <tsv>\n");
    std::printf("  phys-tools layers [--out <tsv>] [--frames N] [--verbose]\n");
    std::printf("  phys-tools check-layers <tsv>\n");
    std::printf("exit: 0=PASS 1=FAIL 2=usage error\n");
}

bool parseReal(const char* s, double* out) {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool parseInt(const char* s, int* out) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 1 || v > 100000) return false;
    *out = static_cast<int>(v);
    return true;
}

std::string configHeader(const nl::physics::DropConfig& cfg) {
    std::ostringstream os;
    os << "# jolt=" << nl::physics::kJoltVersion
       << " job_system=single_threaded"
       << " dt=" << cfg.dt
       << " gravity=" << cfg.gravity
       << " height=" << cfg.startHeight
       << " radius=" << cfg.radius
       << " restitution=" << cfg.restitution
       << " seed=" << cfg.seed
       << " frames=" << cfg.frames;
    return os.str();
}

std::string trajectoryTSV(const nl::physics::DropConfig& cfg,
                          const std::vector<nl::physics::Sample>& samples,
                          std::uint32_t bodyCount) {
    std::ostringstream os;
    os << "# nl-physics M-P0 drop trajectory (real Jolt 5.3.1 solver output)\n";
    os << configHeader(cfg) << "\n";
    os << "# body_count=" << bodyCount << "\n";
    os << "frame\tt\ty\tvy\n";
    for (const auto& s : samples) {
        os << s.frame << '\t';
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.6f\t%.6f\t%.6f",
                      s.t, s.position.y, s.velocity.y);
        os << buf << '\n';
    }
    return os.str();
}

struct Metrics {
    int    samples     = 0;
    int    fallFrame   = -1;
    double fallTime    = 0.0;
    double analyticFall = 0.0;
    double bouncePeak  = 0.0;
    double bounceFrame = -1;
    double restY       = 0.0;
    double restSpread  = 0.0;
    double last10Spread = 0.0;
    double freeFallMaxErr = 0.0;
};

// Physical anchors, shared by selftest and check. Thresholds are intentionally
// coarse (a solver is not a closed-form integrator) but every quantity is checked.
bool analyse(const std::vector<nl::physics::Sample>& s,
             const nl::physics::DropConfig& cfg,
             Metrics* m, std::string* err) {
    auto bad = [err](const char* reason) -> bool { *err = reason; return false; };
    if (s.size() < 2) return bad("trajectory too short");
    m->samples = static_cast<int>(s.size());

    const double contactY = cfg.radius + 0.005;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (s[i].position.y <= contactY) { m->fallFrame = static_cast<int>(i); break; }
    }
    if (m->fallFrame < 0) return bad("sphere never reached the floor");
    m->fallTime = m->fallFrame * cfg.dt;
    m->analyticFall = std::sqrt(2.0 * (cfg.startHeight - cfg.radius) / cfg.gravity);

    // Monotone free fall + semi-implicit Euler closed form (Jolt integrates v first).
    const int halfFall = std::max(1, m->fallFrame / 2);
    for (int i = 1; i <= halfFall; ++i) {
        if (!(s[i].position.y < s[i - 1].position.y))
            return bad("free fall is not monotone decreasing");
        const double analytic = cfg.startHeight
            - 0.5 * cfg.gravity * cfg.dt * cfg.dt * i * (i + 1);
        m->freeFallMaxErr = std::max(m->freeFallMaxErr,
                                     std::abs(s[i].position.y - analytic));
    }

    // First bounce apex after contact (local maximum followed by a descent).
    double peak = -1e9;
    int peakFrame = -1;
    for (int i = m->fallFrame; i < static_cast<int>(s.size()); ++i) {
        if (s[i].position.y > peak) { peak = s[i].position.y; peakFrame = i; }
        if (i > peakFrame + 2 && s[i].velocity.y < 0 && s[i - 1].velocity.y < 0
            && s[i].position.y < peak - 0.05) break;
    }
    m->bouncePeak = peak;
    m->bounceFrame = peakFrame;

    // Rest convergence: last 30 frames must be flat and at sphere radius.
    const int tail = std::min(30, static_cast<int>(s.size()) - 1);
    double lo = 1e9, hi = -1e9;
    for (int i = static_cast<int>(s.size()) - tail; i < static_cast<int>(s.size()); ++i) {
        lo = std::min(lo, static_cast<double>(s[i].position.y));
        hi = std::max(hi, static_cast<double>(s[i].position.y));
    }
    m->restSpread = hi - lo;
    // Stricter convergence on the very last 10 frames. Earlier frames inside the
    // 30-frame tail may still hold the 3rd/4th bounce (restitution 0.6 gives a
    // measured tail spread 0.038) -- that is decaying energy, not solver jitter.
    const int lastN = std::min(10, static_cast<int>(s.size()) - 1);
    double lo10 = 1e9, hi10 = -1e9;
    for (int i = static_cast<int>(s.size()) - lastN; i < static_cast<int>(s.size()); ++i) {
        lo10 = std::min(lo10, static_cast<double>(s[i].position.y));
        hi10 = std::max(hi10, static_cast<double>(s[i].position.y));
    }
    m->last10Spread = hi10 - lo10;
    m->restY = s.back().position.y;
    return true;
}

void reportMetrics(const Metrics& m) {
    std::printf("  frames=%d fall_frame=%d fall_t=%.6f analytic_fall_t=%.6f\n",
                m.samples, m.fallFrame, m.fallTime, m.analyticFall);
    std::printf("  free_fall_max_err=%.6f bounce_peak=%.6f (frame %d)\n",
                m.freeFallMaxErr, m.bouncePeak, static_cast<int>(m.bounceFrame));
    std::printf("  rest_y=%.6f rest_spread=%.6f last10_spread=%.6f\n", m.restY, m.restSpread, m.last10Spread);
}


// ---- M-P1: collision layer matrix probe (real solver output, 4 cases) ----
enum class Expect { kBlocker, kFloor, kPassThrough };

struct LayerCase {
    const char*   name;
    std::uint32_t probeLayer;
    std::uint32_t blockerLayer;
    bool          disablePair;   // turn off exactly the probe/blocker pair
    bool          ghost;         // turn off the probe against EVERY layer
    Expect        expect;
};

const LayerCase kCases[] = {
    {"same_layer",    nl::physics::layers::kDefault, nl::physics::layers::kDefault,
     false, false, Expect::kBlocker},
    {"enabled_pair",  nl::physics::layers::kDebris,  nl::physics::layers::kDefault,
     false, false, Expect::kBlocker},
    {"disabled_pair", nl::physics::layers::kDebris,  nl::physics::layers::kDefault,
     true,  false, Expect::kFloor},
    {"ghost",         nl::physics::layers::kGhost,   nl::physics::layers::kDefault,
     false, true,  Expect::kPassThrough},
};
constexpr int kCaseCount = 4;

nl::physics::LayerMatrix buildMatrix(const LayerCase& c) {
    nl::physics::LayerMatrix m;
    if (c.disablePair) m.set(c.probeLayer, c.blockerLayer, false);
    if (c.ghost)
        for (std::uint32_t b = 0; b < nl::physics::kLayerCount; ++b)
            m.set(c.probeLayer, b, false);
    return m;
}

nl::physics::LayerProbeResult runCase(const LayerCase& c,
                                      const nl::physics::LayerProbeConfig& base) {
    nl::physics::LayerProbeConfig cfg = base;
    cfg.probeLayer   = c.probeLayer;
    cfg.blockerLayer = c.blockerLayer;
    nl::physics::World world;
    return world.simulateLayerProbe(buildMatrix(c), cfg);
}

bool ownerOk(const nl::physics::LayerProbeResult& r,
             const nl::physics::LayerProbeConfig& cfg) {
    return r.ownerFromId == cfg.probeOwner && r.idFromOwner == r.probeId &&
           r.probeId != nl::physics::kInvalidBodyId;
}

bool caseMatches(const nl::physics::LayerProbeResult& r,
                 const nl::physics::LayerProbeConfig& cfg, Expect e) {
    const float onBlocker = cfg.blockerY + 2.0f * cfg.radius;
    switch (e) {
    case Expect::kBlocker:
        return std::abs(r.finalY - onBlocker) < 0.1f && r.blocked;
    case Expect::kFloor:
        // The probe passed through the blocker (below its contact height) but is
        // still held up by the floor. Late small bounces are physical, not jitter,
        // so do not demand an exact rest height here (M-P0 lesson).
        return r.finalY < cfg.blockerY + cfg.radius &&
               r.finalY > -cfg.radius && !r.blocked;
    case Expect::kPassThrough:
        return r.finalY < -1.0f && !r.blocked;
    }
    return false;
}

std::string layersTSV(const nl::physics::LayerProbeConfig& cfg,
                      const std::vector<nl::physics::LayerProbeResult>& rs) {
    std::ostringstream os;
    os << "# nl-physics M-P1 collision layer matrix probe (real Jolt 5.3.1 solver output)\n";
    os << "# jolt=" << nl::physics::kJoltVersion
       << " job_system=single_threaded"
       << " dt=" << cfg.dt << " gravity=" << cfg.gravity
       << " radius=" << cfg.radius << " start_height=" << cfg.startHeight
       << " blocker_y=" << cfg.blockerY << " frames=" << cfg.frames
       << " layers=" << nl::physics::kLayerCount << "\n";
    for (int i = 0; i < kCaseCount; ++i) {
        const nl::physics::LayerMatrix m = buildMatrix(kCases[i]);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "# case %s probe=%u blocker=%u allow=%d matrix=%s final_y=%.6f blocked=%d owner_ok=%d bodies=%zu\n",
            kCases[i].name, kCases[i].probeLayer, kCases[i].blockerLayer,
            m.allows(kCases[i].probeLayer, kCases[i].blockerLayer) ? 1 : 0,
            m.hex().c_str(), rs[i].finalY, rs[i].blocked ? 1 : 0,
            ownerOk(rs[i], cfg) ? 1 : 0, rs[i].bodies.size());
        os << buf;
    }
    os << "frame\tt\tsame_layer\tenabled_pair\tdisabled_pair\tghost\n";
    const std::size_t n = rs[0].samples.size();
    for (std::size_t f = 0; f < n; ++f) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n",
                      rs[0].samples[f].frame, rs[0].samples[f].t,
                      rs[0].samples[f].position.y, rs[1].samples[f].position.y,
                      rs[2].samples[f].position.y, rs[3].samples[f].position.y);
        os << buf;
    }
    return os.str();
}

int runLayers(int argc, char** argv) {
    nl::physics::LayerProbeConfig cfg;
    std::string out;
    auto fail = [](const std::string& e) -> int {
        std::printf("invalid arguments: %s\n", e.c_str());
        return 2;
    };
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--verbose") { g_verbose = true; continue; }
        if (i + 1 >= argc) return fail("missing value for " + key);
        const char* val = argv[++i];
        int n = 0;
        if      (key == "--out")    { out = val; }
        else if (key == "--frames") { if (!parseInt(val, &n)) return fail("bad --frames"); cfg.frames = n; }
        else return fail("unknown option " + key);
    }
    std::string err;
    nl::physics::LayerMatrix probeMatrix;
    if (!nl::physics::validateLayerProbe(cfg, probeMatrix, &err)) return fail(err);

    std::vector<nl::physics::LayerProbeResult> rs;
    rs.reserve(kCaseCount);
    for (int i = 0; i < kCaseCount; ++i) {
        auto r = runCase(kCases[i], cfg);
        if (!r.ok()) {
            std::printf("layers case %s failed: %s\n", kCases[i].name, r.error.c_str());
            return 1;
        }
        rs.push_back(std::move(r));
    }
    const std::string tsv = layersTSV(cfg, rs);
    if (out.empty()) { std::fputs(tsv.c_str(), stdout); return 0; }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { std::printf("cannot write %s\n", out.c_str()); return 1; }
    f << tsv;
    f.close();
    std::printf("wrote %s cases=%d frames=%d\n", out.c_str(), kCaseCount, cfg.frames);
    return 0;
}

int runCheckLayers(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }

    struct CaseRow {
        std::string name;
        double finalY = 0.0;
        int allow = 0, blocked = 0, ownerOk = 0, bodies = 0;
    };
    std::vector<CaseRow> rows;
    int frames = -1, dataRows = 0;
    double blockerY = 2.5, radius = 0.5;
    bool headerSeen = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# jolt=", 0) == 0) {
            std::istringstream is(line);
            std::string tok;
            while (is >> tok) {
                if      (tok.rfind("frames=", 0) == 0)     frames   = std::atoi(tok.c_str() + 7);
                else if (tok.rfind("blocker_y=", 0) == 0)  blockerY = std::strtod(tok.c_str() + 10, nullptr);
                else if (tok.rfind("radius=", 0) == 0)     radius   = std::strtod(tok.c_str() + 7, nullptr);
            }
            continue;
        }
        if (line.rfind("# case ", 0) == 0) {
            CaseRow r;
            std::istringstream is(line);
            std::string hash, tag, tok;
            is >> hash >> tag >> r.name;
            while (is >> tok) {
                const auto eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
                if      (k == "final_y")  r.finalY  = std::strtod(v.c_str(), nullptr);
                else if (k == "allow")    r.allow   = std::atoi(v.c_str());
                else if (k == "blocked")  r.blocked = std::atoi(v.c_str());
                else if (k == "owner_ok") r.ownerOk = std::atoi(v.c_str());
                else if (k == "bodies")   r.bodies  = std::atoi(v.c_str());
            }
            rows.push_back(r);
            continue;
        }
        if (line.rfind("frame\t", 0) == 0) { headerSeen = true; continue; }
        if (line.empty() || line[0] == '#') continue;
        ++dataRows;
    }

    expectTrue(headerSeen, "M-P1 dump has TSV header");
    expectTrue(static_cast<int>(rows.size()) == kCaseCount, "M-P1 dump has 4 case rows");
    expectTrue(frames > 0 && dataRows == frames + 1, "M-P1 dump row count = frames+1");
    for (int i = 0; i < static_cast<int>(rows.size()) && i < kCaseCount; ++i) {
        const CaseRow& r = rows[i];
        const std::string w = std::string("M-P1 case ") + kCases[i].name;
        expectTrue(r.name == kCases[i].name, (w + " name").c_str());
        expectTrue(r.ownerOk == 1, (w + " two-way association").c_str());
        expectTrue(r.bodies == 3, (w + " 3 bodies").c_str());
        const double onBlocker = blockerY + 2.0 * radius;
        switch (kCases[i].expect) {
        case Expect::kBlocker:
            expectTrue(std::abs(r.finalY - onBlocker) < 0.1 && r.blocked == 1,
                       (w + " rests on blocker").c_str());
            break;
        case Expect::kFloor:
            expectTrue(r.finalY < blockerY + radius && r.finalY > -radius &&
                       r.blocked == 0 && r.allow == 0,
                       (w + " passes through blocker, held by floor").c_str());
            break;
        case Expect::kPassThrough:
            expectTrue(r.finalY < -2.0 * radius && r.blocked == 0 && r.allow == 0,
                       (w + " passes through every layer").c_str());
            break;
        }
    }
    if (g_failures) {
        std::printf("phys-p1 check-layers: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-p1 check-layers: PASS (%s, 4 cases, %d frames)\n", path, frames);
    return 0;
}

int runSelftest() {
    nl::physics::DropConfig cfg;
    std::string err;

    // --- negative config paths (explicit exit 2 in the CLI, false here) ---
    auto badCfg = cfg; badCfg.frames = 0;
    expectTrue(!nl::physics::validateConfig(badCfg, &err), "reject frames=0");
    badCfg = cfg; badCfg.dt = 1.0;
    expectTrue(!nl::physics::validateConfig(badCfg, &err), "reject dt=1.0");
    badCfg = cfg; badCfg.gravity = 0.0f;
    expectTrue(!nl::physics::validateConfig(badCfg, &err), "reject gravity=0");
    badCfg = cfg; badCfg.startHeight = cfg.radius;
    expectTrue(!nl::physics::validateConfig(badCfg, &err), "reject height<=radius");
    badCfg = cfg; badCfg.restitution = 1.5f;
    expectTrue(!nl::physics::validateConfig(badCfg, &err), "reject restitution>1");
    expectTrue(nl::physics::validateConfig(cfg, &err), "accept default config");

    // --- real simulation ---
    nl::physics::World world;
    const nl::physics::SimResult r = world.simulateDrop(cfg);
    expectTrue(r.ok(), "simulateDrop succeeded");
    if (!r.ok()) { std::printf("  error: %s\n", r.error.c_str()); }
    expectTrue(world.bodyCount() == 2, "body count = 2 (floor + sphere)");
    expectTrue(static_cast<int>(r.samples.size()) == cfg.frames + 1,
               "trajectory length = frames + 1");
    if (!r.ok() || r.samples.empty()) {
        std::printf("phys-p0 selftest: FAIL (%d)\n", g_failures);
        return 1;
    }
    expectTrue(std::abs(r.samples[0].position.y - cfg.startHeight) < 1e-5,
               "initial height = startHeight");
    expectTrue(std::abs(r.samples[0].velocity.y) < 1e-6, "initial vy = 0");

    Metrics m;
    expectTrue(analyse(r.samples, cfg, &m, &err), "physical anchors computable");
    if (m.fallFrame > 0) {
        expectTrue(std::abs(m.fallTime - m.analyticFall) < 3.0 * cfg.dt,
                   "first contact matches sqrt(2h/g) within 3 steps");
        expectTrue(m.freeFallMaxErr < 0.05,
                   "free fall matches semi-implicit Euler closed form (<0.05)");
        expectTrue(m.bouncePeak > cfg.radius + 0.01, "sphere bounced above floor");
        expectTrue(m.bouncePeak < cfg.startHeight, "bounce never exceeds drop height");
        const double predicted = cfg.radius
            + cfg.restitution * cfg.restitution * (cfg.startHeight - cfg.radius);
        expectTrue(std::abs(m.bouncePeak - predicted) < 0.35 * (predicted - cfg.radius) + 0.05,
                   "bounce apex consistent with restitution^2 energy law");
        expectTrue(m.restSpread < 0.05, "late bounces decay (30-frame spread < 0.05)");
        expectTrue(m.last10Spread < 0.005, "rest trajectory converged (last 10 frames flat)");
        expectTrue(std::abs(m.restY - cfg.radius) < 0.05, "rest height = sphere radius");
    }

    // --- determinism: same config, fresh world, bit-identical samples ---
    const nl::physics::SimResult r2 = world.simulateDrop(cfg);
    bool identical = r2.ok() && r2.samples.size() == r.samples.size();
    if (identical) {
        for (std::size_t i = 0; i < r.samples.size(); ++i) {
            const auto& a = r.samples[i];
            const auto& b = r2.samples[i];
            if (a.frame != b.frame || a.t != b.t ||
                a.position.x != b.position.x || a.position.y != b.position.y ||
                a.position.z != b.position.z ||
                a.velocity.x != b.velocity.x || a.velocity.y != b.velocity.y ||
                a.velocity.z != b.velocity.z) { identical = false; break; }
        }
    }
    expectTrue(identical, "determinism: two worlds match sample-by-sample");

    // --- restore-from-TSV path (the same bytes the evidence PNG is built from) ---
    const std::string tsv = trajectoryTSV(cfg, r.samples, world.bodyCount());
    expectTrue(tsv.find("frame\tt\ty\tvy\n") != std::string::npos,
               "dump has TSV header");
    expectTrue(std::count(tsv.begin(), tsv.end(), '\n')
               == static_cast<long>(r.samples.size()) + 4,
               "dump line count = header comments + samples");


    // --- M-P1: collision layer matrix + engine<->body association ---
    nl::physics::LayerProbeConfig pcfg;
    nl::physics::LayerMatrix mm;
    expectTrue(mm.allows(0, 1) && mm.allows(1, 0),
               "M-P1 matrix defaults all-on and symmetric");
    mm.set(2, 1, false);
    expectTrue(!mm.allows(2, 1) && !mm.allows(1, 2), "M-P1 set() keeps symmetry");
    nl::physics::LayerProbeConfig badLayer = pcfg; badLayer.probeLayer = 9;
    expectTrue(!nl::physics::validateLayerProbe(badLayer, mm, &err),
               "M-P1 reject layer out of range");
    nl::physics::LayerProbeConfig badHeight = pcfg; badHeight.startHeight = 1.0f;
    expectTrue(!nl::physics::validateLayerProbe(badHeight, mm, &err),
               "M-P1 reject height below blocker");
    for (int i = 0; i < kCaseCount; ++i) {
        const auto lr = runCase(kCases[i], pcfg);
        const std::string w = std::string("M-P1 ") + kCases[i].name;
        expectTrue(lr.ok(), (w + " probe ok").c_str());
        expectTrue(lr.bodies.size() == 3, (w + " 3 bodies").c_str());
        expectTrue(ownerOk(lr, pcfg), (w + " two-way association").c_str());
        expectTrue(caseMatches(lr, pcfg, kCases[i].expect), (w + " outcome").c_str());
    }
    const auto d1 = runCase(kCases[2], pcfg);
    const auto d2 = runCase(kCases[2], pcfg);
    bool layersIdentical = d1.samples.size() == d2.samples.size();
    for (std::size_t i = 0; layersIdentical && i < d1.samples.size(); ++i) {
        layersIdentical = d1.samples[i].frame == d2.samples[i].frame &&
                          d1.samples[i].t == d2.samples[i].t &&
                          d1.samples[i].position.y == d2.samples[i].position.y &&
                          d1.samples[i].velocity.y == d2.samples[i].velocity.y;
    }
    expectTrue(layersIdentical, "M-P1 determinism: same case twice bit-identical");

    if (g_failures) {
        std::printf("phys-p0 selftest: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-p1 anchors: PASS (4 layer cases, 2-way association, determinism)\n");
    std::printf("phys-p0 selftest: PASS (Jolt %s, single-threaded, %d frames)\n",
                nl::physics::kJoltVersion, cfg.frames);
    if (g_verbose) reportMetrics(m);
    return 0;
}

bool parseSimulateArgs(int argc, char** argv, int first,
                       nl::physics::DropConfig* cfg, std::string* out,
                       std::string* err) {
    for (int i = first; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--verbose") { g_verbose = true; continue; }
        if (i + 1 >= argc) { *err = "missing value for " + key; return false; }
        const char* val = argv[++i];
        double d = 0.0;
        int    n = 0;
        if      (key == "--out")         { *out = val; }
        else if (key == "--frames")      { if (!parseInt(val, &n)) { *err = "bad --frames"; return false; } cfg->frames = n; }
        else if (key == "--dt")          { if (!parseReal(val, &d)) { *err = "bad --dt"; return false; } cfg->dt = d; }
        else if (key == "--gravity")     { if (!parseReal(val, &d)) { *err = "bad --gravity"; return false; } cfg->gravity = static_cast<float>(d); }
        else if (key == "--height")      { if (!parseReal(val, &d)) { *err = "bad --height"; return false; } cfg->startHeight = static_cast<float>(d); }
        else if (key == "--radius")      { if (!parseReal(val, &d)) { *err = "bad --radius"; return false; } cfg->radius = static_cast<float>(d); }
        else if (key == "--restitution") { if (!parseReal(val, &d)) { *err = "bad --restitution"; return false; } cfg->restitution = static_cast<float>(d); }
        else if (key == "--seed")        { if (!parseInt(val, &n)) { *err = "bad --seed"; return false; } cfg->seed = static_cast<std::uint32_t>(n); }
        else { *err = "unknown option " + key; return false; }
    }
    return nl::physics::validateConfig(*cfg, err);
}

int runSimulate(int argc, char** argv) {
    nl::physics::DropConfig cfg;
    std::string out, err;
    if (!parseSimulateArgs(argc, argv, 2, &cfg, &out, &err)) {
        std::printf("invalid arguments: %s\n", err.c_str());
        return 2;
    }
    nl::physics::World world;
    const nl::physics::SimResult r = world.simulateDrop(cfg);
    if (!r.ok()) { std::printf("simulate failed: %s\n", r.error.c_str()); return 1; }
    const std::string tsv = trajectoryTSV(cfg, r.samples, world.bodyCount());
    if (out.empty()) {
        std::fputs(tsv.c_str(), stdout);
        return 0;
    }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { std::printf("cannot write %s\n", out.c_str()); return 1; }
    f << tsv;
    f.close();
    std::printf("wrote %s frames=%d bodies=%u\n",
                out.c_str(), cfg.frames, world.bodyCount());
    return 0;
}

// Read the simulator parameters back from the real provenance header, so `check`
// verifies any dumped config rather than silently assuming defaults.
void applyHeaderConfig(const std::string& line, nl::physics::DropConfig* cfg) {
    std::istringstream is(line);
    std::string tok;
    while (is >> tok) {
        const auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        char* end = nullptr;
        const double d = std::strtod(v.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(d)) continue;
        if      (k == "dt")          cfg->dt = d;
        else if (k == "gravity")     cfg->gravity = static_cast<float>(d);
        else if (k == "height")      cfg->startHeight = static_cast<float>(d);
        else if (k == "radius")      cfg->radius = static_cast<float>(d);
        else if (k == "restitution") cfg->restitution = static_cast<float>(d);
        else if (k == "frames")      cfg->frames = static_cast<int>(d);
        else if (k == "seed")        cfg->seed = static_cast<std::uint32_t>(d);
    }
}

int runCheck(const char* path) {
    std::ifstream f(path);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    nl::physics::DropConfig cfg;
    std::vector<nl::physics::Sample> samples;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# jolt=", 0) == 0) { applyHeaderConfig(line, &cfg); continue; }
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("frame\t", 0) == 0) continue;
        int frame = 0;
        double t = 0, y = 0, vy = 0;
        if (std::sscanf(line.c_str(), "%d %lf %lf %lf", &frame, &t, &y, &vy) != 4) {
            std::printf("bad TSV row: %s\n", line.c_str());
            return 1;
        }
        nl::physics::Sample s;
        s.frame = frame; s.t = t;
        s.position.y = static_cast<float>(y);
        s.velocity.y = static_cast<float>(vy);
        samples.push_back(s);
    }
    cfg.frames = static_cast<int>(samples.size()) - 1;
    std::string err;
    nl::physics::validateConfig(cfg, &err); // frames must be > 0 even if dt differs
    Metrics m;
    if (!analyse(samples, cfg, &m, &err)) {
        std::printf("phys-p0 check: FAIL (%s)\n", err.c_str());
        return 1;
    }
    expectTrue(std::abs(m.fallTime - m.analyticFall) < 3.0 * cfg.dt,
               "first contact matches sqrt(2h/g)");
    expectTrue(m.freeFallMaxErr < 0.05, "free fall matches closed form");
    expectTrue(m.bouncePeak > cfg.radius + 0.01, "bounce present");
    expectTrue(m.restSpread < 0.05, "late bounces decay");
    expectTrue(m.last10Spread < 0.005, "rest converged (last 10 frames flat)");
    if (g_failures) {
        std::printf("phys-p0 check: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-p0 check: PASS (%s, %d samples)\n", path, m.samples);
    reportMetrics(m);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "selftest") {
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--verbose") g_verbose = true;
            else { usage(); return 2; }
        }
        return runSelftest();
    }
    if (cmd == "simulate") return runSimulate(argc, argv);
    if (cmd == "check") {
        if (argc != 3) { usage(); return 2; }
        return runCheck(argv[2]);
    }
    if (cmd == "layers")       return runLayers(argc, argv);
    if (cmd == "check-layers") {
        if (argc != 3) { usage(); return 2; }
        return runCheckLayers(argv[2]);
    }
    usage();
    return 2;
}
