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
//   stack [options]        M-P2: stacking stability TSV (baseline/no_sleep/no_friction)
//   check-stack <tsv>      re-verify the stacking dump
//   debug [options]        M-P3: collision-shape wireframe TSV (real Jolt
//                          DebugRenderer line stream: floor + box/sphere/capsule)
//   check-debug <tsv>      re-verify the wireframe dump (topology + settle anchors)
//   character [options]    M-P4: character walk-through TSV (Jolt CharacterVirtual,
//                          CharacterMovement-style: 30deg ramp + 3 steps + 60deg block)
//   check-character <tsv>  re-verify the walk dump (course + parking anchors)
//   check-jobs [options]   M-J5c: JobSystem adapter matrix (N=1 bit-identical
//                          hard proof + N>1 run-to-run determinism, drop+stack)
//   contact-trace [opts]   M-D0: ContactListener -> Jolt-free POD event TSV
//   contact-check <tsv>    M-D0: re-verify the contact dump from data rows
//
// Evidence rule: example/phys-p0/*.png, example/phys-p1/*.png,
// example/phys-p2/*.png and example/phys-p3/*.png are generated from the REAL
// TSV by the matching make-phys*-evidence.py. No hand-drawn diagrams.

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
    std::printf("  phys-tools stack [--out <tsv>] [--frames N] [--boxes N] [--gap G]\n");
    std::printf("  phys-tools check-stack <tsv>\n");
    std::printf("  phys-tools debug [--out <tsv>] [--frames N]\n");
    std::printf("  phys-tools check-debug <tsv>\n");
    std::printf("  phys-tools character [--out <tsv>] [--frames N]\n");
    std::printf("  phys-tools check-character <tsv>\n");
    std::printf("  phys-tools check-jobs [--threads N] [--frames N]  (M-J5c JobSystem adapter)\n");
    std::printf("  phys-tools contact-trace [--out <tsv>] [--frames N]  (M-D0 contact events)\n");
    std::printf("  phys-tools contact-check <tsv>\n");
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


// ---- M-P2: stacking stability (3 real solver cases, long run) ----
struct StackCase {
    const char* name;
    float friction;
    bool  allowSleeping;
    float lateralSpeed;
};

const StackCase kStackCases[] = {
    {"baseline",    0.6f, true,  0.0f},  // friction + sleeping: tower should go quiet
    {"no_sleep",    0.6f, false, 0.0f},  // same tower, solver keeps iterating forever
    {"no_friction", 0.0f, true,  0.5f},  // top cube pushed sideways with no friction
};
constexpr int kStackCaseCount = 3;
constexpr int kStackTail      = 60;

nl::physics::StackResult runStackCase(const StackCase& c,
                                      const nl::physics::StackConfig& base) {
    nl::physics::StackConfig cfg = base;
    cfg.friction      = c.friction;
    cfg.allowSleeping = c.allowSleeping;
    cfg.lateralSpeed  = c.lateralSpeed;
    nl::physics::World world;
    return world.simulateStack(cfg);
}

float frameTopDrift(const nl::physics::StackFrame& f) {
    if (f.bodies.empty()) return 0.0f;
    const nl::math::Vec3& p = f.bodies.back().position;
    return std::sqrt(p.x * p.x + p.z * p.z);
}

std::string stackTSV(const nl::physics::StackConfig& cfg,
                     const std::vector<nl::physics::StackResult>& rs) {
    std::ostringstream os;
    os << "# nl-physics M-P2 stacking stability (real Jolt " << nl::physics::kJoltVersion
       << " solver output)\n";
    os << "# jolt=" << nl::physics::kJoltVersion
       << " job_system=single_threaded"
       << " dt=" << cfg.dt << " gravity=" << cfg.gravity
       << " boxes=" << cfg.boxes << " half_extent=" << cfg.halfExtent
       << " gap=" << cfg.gap << " frames=" << cfg.frames
       << " tail=" << kStackTail << "\n";
    for (int i = 0; i < kStackCaseCount; ++i) {
        const nl::physics::StackResult& r = rs[static_cast<std::size_t>(i)];
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "# case %s allow_sleep=%d friction=%.6f lateral=%.6f final_active=%u "
            "max_tail_spread=%.6f max_tail_speed=%.6f max_drift=%.6f top_drift=%.6f "
            "top_y=%.6f collapsed=%d bodies=%zu\n",
            kStackCases[i].name, kStackCases[i].allowSleeping ? 1 : 0,
            kStackCases[i].friction, kStackCases[i].lateralSpeed,
            r.finalActive, r.maxTailSpread, r.maxTailSpeed, r.maxDrift,
            r.topDrift, r.topY, r.collapsed ? 1 : 0,
            r.frames.empty() ? 0 : r.frames.back().bodies.size());
        os << buf;
    }
    os << "frame\tt";
    for (int c = 0; c < kStackCaseCount; ++c)
        for (std::uint32_t b = 0; b < cfg.boxes; ++b) os << "\t" << kStackCases[c].name << "_y" << b;
    for (int c = 0; c < kStackCaseCount; ++c) os << "\t" << kStackCases[c].name << "_active";
    os << "\tbase_top_drift\tnofric_top_drift\n";

    const std::size_t n = rs[0].frames.size();
    for (std::size_t f = 0; f < n; ++f) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%d\t%.6f",
                      rs[0].frames[f].frame, rs[0].frames[f].t);
        os << buf;
        for (int c = 0; c < kStackCaseCount; ++c) {
            const auto& fr = rs[static_cast<std::size_t>(c)].frames[f];
            for (std::uint32_t b = 0; b < cfg.boxes; ++b) {
                std::snprintf(buf, sizeof(buf), "\t%.6f", fr.bodies[b].position.y);
                os << buf;
            }
        }
        for (int c = 0; c < kStackCaseCount; ++c)
            os << "\t" << rs[static_cast<std::size_t>(c)].frames[f].activeBodies;
        std::snprintf(buf, sizeof(buf), "\t%.6f\t%.6f\n",
                      frameTopDrift(rs[0].frames[f]), frameTopDrift(rs[2].frames[f]));
        os << buf;
    }
    return os.str();
}

int runStack(int argc, char** argv) {
    nl::physics::StackConfig cfg;
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
        int n = 0; double d = 0.0;
        if      (key == "--out")    { out = val; }
        else if (key == "--frames") { if (!parseInt(val, &n)) return fail("bad --frames"); cfg.frames = n; }
        else if (key == "--boxes")  { if (!parseInt(val, &n)) return fail("bad --boxes"); cfg.boxes = static_cast<std::uint32_t>(n); }
        else if (key == "--gap")    { if (!parseReal(val, &d)) return fail("bad --gap"); cfg.gap = static_cast<float>(d); }
        else if (key == "--jobs")   { if (!parseInt(val, &n)) return fail("bad --jobs"); cfg.jobsThreads = n; }
        else return fail("unknown option " + key);
    }
    std::string err;
    if (!nl::physics::validateStack(cfg, &err)) return fail(err);

    std::vector<nl::physics::StackResult> rs;
    rs.reserve(kStackCaseCount);
    for (int i = 0; i < kStackCaseCount; ++i) {
        auto r = runStackCase(kStackCases[i], cfg);
        if (!r.ok()) {
            std::printf("stack case %s failed: %s\n", kStackCases[i].name, r.error.c_str());
            return 1;
        }
        rs.push_back(std::move(r));
    }
    const std::string tsv = stackTSV(cfg, rs);
    if (out.empty()) { std::fputs(tsv.c_str(), stdout); return 0; }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { std::printf("cannot write %s\n", out.c_str()); return 1; }
    f << tsv;
    f.close();
    std::printf("wrote %s cases=%d boxes=%u frames=%d\n",
                out.c_str(), kStackCaseCount, cfg.boxes, cfg.frames);
    return 0;
}

int runCheckStack(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }

    struct CaseRow {
        std::string name;
        int    allowSleep = 0, finalActive = -1, collapsed = -1, bodies = -1;
        double tailSpread = -1.0, topDrift = -1.0, topY = 0.0;
    };
    std::vector<CaseRow> rows;
    int frames = -1, boxes = -1, tail = 60;
    double halfExtent = 0.5;
    bool headerSeen = false;
    std::vector<std::vector<double>> baseY;  // [box][frame]
    std::vector<std::vector<int>> active;    // [case][frame]
    std::vector<double> nofricDrift;
    int dataRows = 0;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# jolt=", 0) == 0) {
            std::istringstream is(line);
            std::string tok;
            while (is >> tok) {
                const auto eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
                if      (k == "frames")      frames     = std::atoi(v.c_str());
                else if (k == "boxes")       boxes      = std::atoi(v.c_str());
                else if (k == "tail")        tail       = std::atoi(v.c_str());
                else if (k == "half_extent") halfExtent = std::strtod(v.c_str(), nullptr);
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
                if      (k == "allow_sleep")      r.allowSleep  = std::atoi(v.c_str());
                else if (k == "final_active")     r.finalActive = std::atoi(v.c_str());
                else if (k == "max_tail_spread")  r.tailSpread  = std::strtod(v.c_str(), nullptr);
                else if (k == "top_drift")        r.topDrift    = std::strtod(v.c_str(), nullptr);
                else if (k == "top_y")            r.topY        = std::strtod(v.c_str(), nullptr);
                else if (k == "collapsed")        r.collapsed   = std::atoi(v.c_str());
                else if (k == "bodies")           r.bodies      = std::atoi(v.c_str());
            }
            rows.push_back(r);
            continue;
        }
        if (line.rfind("frame\t", 0) == 0) { headerSeen = true; continue; }
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> cols;
        std::istringstream is(line);
        std::string cell;
        while (std::getline(is, cell, '\t')) cols.push_back(cell);
        const std::size_t expected = 3 * static_cast<std::size_t>(boxes < 0 ? 0 : boxes) + 7;
        if (boxes > 0 && cols.size() != expected) {
            std::printf("M-P2 dump column count mismatch at data row %d: %zu (expect %zu)\n",
                        dataRows, cols.size(), expected);
            return 1;
        }
        if (boxes > 0) {
            if (baseY.empty()) {
                baseY.assign(static_cast<std::size_t>(boxes), {});
                active.assign(kStackCaseCount, {});
            }
            for (int b = 0; b < boxes; ++b)
                baseY[static_cast<std::size_t>(b)].push_back(std::strtod(cols[2 + b].c_str(), nullptr));
            for (int c = 0; c < kStackCaseCount; ++c)
                active[static_cast<std::size_t>(c)].push_back(
                    std::atoi(cols[2 + 3 * boxes + c].c_str()));
            nofricDrift.push_back(std::strtod(cols[2 + 3 * boxes + 4].c_str(), nullptr));
        }
        ++dataRows;
    }

    expectTrue(headerSeen, "M-P2 dump has TSV column header");
    expectTrue(frames > 0 && boxes >= 2 && dataRows == frames + 1,
               "M-P2 dump row count = frames+1");
    expectTrue(static_cast<int>(rows.size()) == kStackCaseCount, "M-P2 dump has 3 case rows");
    if (g_failures || boxes < 2 || dataRows != frames + 1) {
        std::printf("phys-p2 check-stack: FAIL (%d)\n", g_failures);
        return 1;
    }

    // Independently recompute stability from the DATA columns (never trust the
    // summary rows alone -- that would be the M-P0/M-P1 "fake green" trap).
    double dataSpread = 0.0;
    const int from = std::max(0, dataRows - tail);
    for (int b = 0; b < boxes; ++b) {
        const auto& v = baseY[static_cast<std::size_t>(b)];
        const double finalY = v.back();
        const double expectedY = halfExtent + b * 2.0 * halfExtent;
        expectTrue(std::abs(finalY - expectedY) < 0.15,
                   ("M-P2 baseline cube " + std::to_string(b) + " held its slot").c_str());
        for (int k = from; k < dataRows; ++k)
            dataSpread = std::max(dataSpread, std::abs(v[static_cast<std::size_t>(k)] - finalY));
    }
    for (int b = 0; b + 1 < boxes; ++b) {
        const double pitch = baseY[static_cast<std::size_t>(b + 1)].back() -
                             baseY[static_cast<std::size_t>(b)].back();
        expectTrue(std::abs(pitch - 2.0 * halfExtent) < 0.1,
                   ("M-P2 layer pitch " + std::to_string(b) + " intact").c_str());
    }
    expectTrue(dataSpread < 0.005, "M-P2 baseline tail converged (no jitter)");
    expectTrue(active[0].back() == 0, "M-P2 baseline island went to sleep");
    expectTrue(active[1].back() > 0, "M-P2 no_sleep island stays active");
    expectTrue(nofricDrift.back() > 0.3, "M-P2 no_friction top cube slid away");

    for (int i = 0; i < kStackCaseCount; ++i) {
        const CaseRow& r = rows[static_cast<std::size_t>(i)];
        expectTrue(r.name == kStackCases[i].name, "M-P2 case name");
        expectTrue(r.finalActive == active[static_cast<std::size_t>(i)].back(),
                   ("M-P2 case " + r.name + " final_active matches data").c_str());
        expectTrue(r.bodies == boxes, ("M-P2 case " + r.name + " body count").c_str());
    }
    expectTrue(std::abs(rows[0].tailSpread - dataSpread) < 1e-3,
               "M-P2 summary tail spread matches recomputed data");
    expectTrue(std::abs(rows[2].topDrift - nofricDrift.back()) < 1e-3,
               "M-P2 summary top_drift matches recomputed data");
    expectTrue(rows[0].collapsed == 0, "M-P2 baseline tower not collapsed");

    if (g_failures) {
        std::printf("phys-p2 check-stack: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-p2 check-stack: PASS (%s, 3 cases, %d boxes, %d frames)\n",
                path, boxes, frames);
    return 0;
}


// ---- M-P3: collision-shape debug wireframe (real Jolt DebugRenderer line stream) ----
// 与 viewer --physics-debug 共用 nl::physics::DebugScene：每帧 step + extractLines，
// 把线流按帧汇总成 TSV（不落 25k 顶点原始流，只落可独立复算的每帧几何度量）。
constexpr double kDebugDt   = 1.0 / 60.0;
constexpr int    kDebugTail = 30;   // 静止判定窗口（帧）

struct DebugFrameRow {
    int           frame    = 0;
    double        t        = 0.0;
    std::uint32_t vertices = 0;
    double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
    std::uint32_t boxV = 0, sphereV = 0, capsuleV = 0, floorV = 0;
};

// 分色参考值必须与 core/src/physics/World.cpp extractLines 的分色一致：
// 盒绿 / 球橙 / 胶囊品红 / 地板蓝。线流颜色是 0..1 float，按最近邻归类（穷举，
// 保证四桶之和恒等于顶点总数）。
struct RefColor { float r, g, b; };
const RefColor kRefColors[4] = {
    { 80.0f / 255.0f, 220.0f / 255.0f,  80.0f / 255.0f},  // box
    {255.0f / 255.0f, 160.0f / 255.0f,  40.0f / 255.0f},  // sphere
    {230.0f / 255.0f,  80.0f / 255.0f, 230.0f / 255.0f},  // capsule
    { 60.0f / 255.0f, 120.0f / 255.0f, 255.0f / 255.0f},  // floor
};

int classifyVertex(const float* v) {  // v -> px,py,pz,r,g,b
    const float r = v[3], g = v[4], b = v[5];
    int   best  = 0;
    float bestD = 1e9f;
    for (int i = 0; i < 4; ++i) {
        const float dr = r - kRefColors[i].r;
        const float dg = g - kRefColors[i].g;
        const float db = b - kRefColors[i].b;
        const float d  = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

DebugFrameRow measureFrame(const nl::physics::DebugLines& dl, int frame, double t) {
    DebugFrameRow r;
    r.frame = frame;
    r.t     = t;
    r.vertices = dl.vertexCount;
    double mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9, mnz = 1e9, mxz = -1e9;
    std::uint32_t buckets[4] = {0, 0, 0, 0};
    for (std::uint32_t i = 0; i < dl.vertexCount; ++i) {
        const float* v = dl.data + static_cast<std::size_t>(i) * 6u;
        const double x = v[0], y = v[1], z = v[2];
        mnx = std::min(mnx, x); mxx = std::max(mxx, x);
        mny = std::min(mny, y); mxy = std::max(mxy, y);
        mnz = std::min(mnz, z); mxz = std::max(mxz, z);
        ++buckets[classifyVertex(v)];
    }
    r.minX = mnx; r.maxX = mxx; r.minY = mny; r.maxY = mxy; r.minZ = mnz; r.maxZ = mxz;
    r.boxV = buckets[0]; r.sphereV = buckets[1]; r.capsuleV = buckets[2]; r.floorV = buckets[3];
    return r;
}

std::string debugTSV(int frames, std::uint32_t bodies,
                     const std::vector<DebugFrameRow>& rows) {
    std::ostringstream os;
    os << "# nl-physics M-P3 collision-shape debug wireframe (real Jolt "
       << nl::physics::kJoltVersion << " DebugRenderer line stream)\n";
    os << "# jolt=" << nl::physics::kJoltVersion
       << " job_system=single_threaded"
       << " dt=" << kDebugDt << " frames=" << frames
       << " scenario=0 bodies=" << bodies << "\n";
    os << "# colors: box=80,220,80 sphere=255,160,40 capsule=230,80,230 floor=60,120,255\n";
    os << "frame\tt\tvertices\tmin_y\tmax_y\tmin_x\tmax_x\tmin_z\tmax_z\t"
          "box_v\tsphere_v\tcapsule_v\tfloor_v\n";
    for (const DebugFrameRow& r : rows) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%d\t%.6f\t%u\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%u\t%u\t%u\t%u\n",
            r.frame, r.t, r.vertices,
            r.minY, r.maxY, r.minX, r.maxX, r.minZ, r.maxZ,
            r.boxV, r.sphereV, r.capsuleV, r.floorV);
        os << buf;
    }
    return os.str();
}

int runDebug(int argc, char** argv) {
    std::string out;
    int frames = 240;
    auto fail = [](const std::string& e) -> int {
        std::printf("invalid arguments: %s\n", e.c_str());
        return 2;
    };
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 >= argc) return fail("missing value for " + key);
        const char* val = argv[++i];
        int n = 0;
        if      (key == "--out")    { out = val; }
        else if (key == "--frames") { if (!parseInt(val, &n)) return fail("bad --frames"); frames = n; }
        else return fail("unknown option " + key);
    }

    nl::physics::DebugScene scene;
    std::string err;
    if (!scene.build(0, 0, &err)) {
        std::printf("debug scene build failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<DebugFrameRow> rows;
    rows.reserve(static_cast<std::size_t>(frames) + 1u);
    double t = 0.0;
    for (int f = 0; f <= frames; ++f) {
        if (f > 0) { scene.step(kDebugDt); t += kDebugDt; }
        const nl::physics::DebugLines dl = scene.extractLines();
        if (dl.data == nullptr || dl.vertexCount == 0) {
            std::printf("debug wireframe empty at frame %d\n", f);
            return 1;
        }
        rows.push_back(measureFrame(dl, f, t));
    }
    const std::string tsv = debugTSV(frames, scene.bodyCount(), rows);
    if (out.empty()) { std::fputs(tsv.c_str(), stdout); return 0; }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { std::printf("cannot write %s\n", out.c_str()); return 1; }
    f << tsv;
    f.close();
    std::printf("wrote %s frames=%d bodies=%u vertices=%u\n",
                out.c_str(), frames, scene.bodyCount(), rows.back().vertices);
    return 0;
}

int runCheckDebug(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }

    struct Row {
        std::uint32_t vertices = 0, boxV = 0, sphereV = 0, capsuleV = 0, floorV = 0;
        double minY = 0, maxY = 0, minX = 0, maxX = 0;
    };
    std::vector<Row> rows;
    int  frames = -1, bodies = -1, dataRows = 0;
    bool headerSeen = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# jolt=", 0) == 0) {
            std::istringstream is(line);
            std::string tok;
            while (is >> tok) {
                const auto eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
                if      (k == "frames") frames = std::atoi(v.c_str());
                else if (k == "bodies") bodies = std::atoi(v.c_str());
            }
            continue;
        }
        if (line.rfind("frame\t", 0) == 0) { headerSeen = true; continue; }
        if (line.empty() || line[0] == '#') continue;
        Row r;
        int frame = 0; double t = 0, minZ = 0, maxZ = 0;
        if (std::sscanf(line.c_str(),
                        "%d %lf %u %lf %lf %lf %lf %lf %lf %u %u %u %u",
                        &frame, &t, &r.vertices,
                        &r.minY, &r.maxY, &r.minX, &r.maxX, &minZ, &maxZ,
                        &r.boxV, &r.sphereV, &r.capsuleV, &r.floorV) != 13) {
            std::printf("bad TSV row at data row %d\n", dataRows);
            return 1;
        }
        rows.push_back(r);
        ++dataRows;
    }

    expectTrue(headerSeen, "M-P3 dump has TSV column header");
    expectTrue(frames > 0 && dataRows == frames + 1, "M-P3 dump row count = frames+1");
    expectTrue(bodies == 4, "M-P3 scene has 4 bodies (floor + box/sphere/capsule)");
    if (g_failures || rows.empty()) {
        std::printf("phys-p3 check-debug: FAIL (%d)\n", g_failures);
        return 1;
    }

    // 结构锚（从数据列独立复算，不信头注释）：线框拓扑与刚体位姿无关，
    // 顶点数与分色桶必须逐帧恒定。
    bool constant = true;
    for (const Row& r : rows) {
        constant = constant && r.vertices == rows[0].vertices &&
                   r.boxV == rows[0].boxV && r.sphereV == rows[0].sphereV &&
                   r.capsuleV == rows[0].capsuleV && r.floorV == rows[0].floorV;
    }
    expectTrue(constant, "M-P3 wireframe topology constant across frames");
    expectTrue(rows[0].vertices % 2 == 0, "M-P3 LINE_LIST vertex pairs");
    bool sumOk = true;
    for (const Row& r : rows)
        sumOk = sumOk && (r.boxV + r.sphereV + r.capsuleV + r.floorV) == r.vertices;
    expectTrue(sumOk, "M-P3 color buckets partition all vertices");
    expectTrue(rows[0].boxV > 0 && rows[0].sphereV > 0 &&
               rows[0].capsuleV > 0 && rows[0].floorV > 0,
               "M-P3 all four shape classes present");

    // 物理锚：场景几何已知——地板半 extent 5（x 覆盖 ±5）、地板底 -1；
    // 胶囊(0.5,0.3) 半高 0.8，从 y=4 落下 → 初始顶 4.8、静止顶 1.6。
    const Row& first = rows.front();
    const Row& last  = rows.back();
    expectTrue(std::abs(first.maxY - 4.8) < 0.05,
               "M-P3 initial top = capsule apex 4.8");
    expectTrue(std::abs(last.minY + 1.0) < 1e-3, "M-P3 floor bottom reaches -1");
    expectTrue(std::abs(last.maxY - 1.6) < 0.05, "M-P3 settled capsule apex 1.6");
    expectTrue(last.maxY < first.maxY - 2.0, "M-P3 bodies fell from drop height");
    expectTrue(std::abs(last.minX + 5.0) < 1e-3 && std::abs(last.maxX - 5.0) < 1e-3,
               "M-P3 floor spans x = +-5");

    // 静止锚：末 30 帧 max_y 冻结（默认睡眠开启，岛入睡后位姿逐位不动）。
    const int from = std::max(0, static_cast<int>(rows.size()) - kDebugTail);
    double tailSpread = 0.0;
    for (std::size_t i = static_cast<std::size_t>(from); i < rows.size(); ++i)
        tailSpread = std::max(tailSpread, std::abs(rows[i].maxY - last.maxY));
    expectTrue(tailSpread < 1e-4, "M-P3 tail 30 frames frozen (sleeping island)");

    if (g_failures) {
        std::printf("phys-p3 check-debug: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-p3 check-debug: PASS (%s, %d frames, %u vertices/frame)\n",
                path, frames, last.vertices);
    return 0;
}

// ---- M-P4: character walk-through TSV (CharacterMovement-style) -------------
// 与 core CharacterHarness 同一场地/节奏：地板 -> 30° 斜坡 -> 平台 -> 3 级台阶
// -> 平台 -> 60° 斜坡（挡住）。position 列 = CharacterVirtual mPosition（胶囊
// 几何中心；脚底 = py - (halfCyl + radius - padding) = py - 1.08，见 World.cpp
// 位置语义注释）。groundState 数值 = JPH::CharacterBase::EGroundState 枚举序。
constexpr double kCharDt        = 1.0 / 60.0;
constexpr int    kCharTail      = 60;      // 静止判定窗口（帧）
constexpr double kCharRampTop   = 2.3094;  // 与 World.cpp kCharRampTop 同源
constexpr double kCharStepTop   = 3.2094;  // 与 World.cpp kCharStepTop 同源
// 贴地时中心高度 = top + (halfCyl + radius - padding) + padding = top + 1.10
// （CharacterVirtual 保持 padding 悬距；实测 2026-11-09：平台1 3.4094 / 平台2 4.3094）。
constexpr double kCharPlat1PosY = kCharRampTop + 1.10;  // 3.4094
constexpr double kCharPlat2PosY = kCharStepTop + 1.10;  // 4.3094
// 台阶爬升窗口：胶囊半径 0.5 使前缘在 x≈17.5 就接触第一级立面（WalkStairs 提前），
// 所以窗口从 17.3 起算（含平台1 尾段），到 20.2（平台2 头段）。
constexpr double kCharStepWinLo = 4.3;   // 场地中心化后台阶窗口（前缘提前接触）
constexpr double kCharStepWinHi = 7.2;

int runCharacter(int argc, char** argv) {
    std::string out;
    int frames = 1200;
    auto fail = [](const std::string& e) -> int {
        std::printf("invalid arguments: %s\n", e.c_str());
        return 2;
    };
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 >= argc) return fail("missing value for " + key);
        const char* val = argv[++i];
        int n = 0;
        if      (key == "--out")    { out = val; }
        else if (key == "--frames") { if (!parseInt(val, &n)) return fail("bad --frames"); frames = n; }
        else return fail("unknown option " + key);
    }

    nl::physics::CharacterConfig cfg;
    cfg.frames = frames;
    nl::physics::World world;
    const nl::physics::CharacterResult r = world.simulateCharacter(cfg);
    if (!r.ok()) { std::printf("character simulate failed: %s\n", r.error.c_str()); return 1; }

    std::ostringstream os;
    os << "# nl-physics M-P4 character walk-through (Jolt " << nl::physics::kJoltVersion
       << " CharacterVirtual ExtendedUpdate, CharacterMovement-style)\n";
    os << "# jolt=" << nl::physics::kJoltVersion
       << " job_system=single_threaded dt=" << kCharDt
       << " frames=" << cfg.frames
       << " walk=1.5 max_slope=50 radius=0.5 half_cyl=0.6 padding=0.02\n";
    os << "# groundState 0=OnGround 1=OnSteepGround 2=NotSupported 3=InAir; "
          "position = capsule center (feet = py - 1.08)\n";
    os << "# course: start x=-13 -> 30deg ramp x[-7,-3] -> platform -> 3 steps x[5,6.8]"
          " -> platform -> 60deg slope x[11,13] (blocked)\n";
    os << "frame\tt\tpx\tpy\tpz\tvx\tvy\tvz\tstate\tgroundNy\n";
    for (const nl::physics::CharacterSample& s : r.samples) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%d\t%.6f\n",
            s.frame, s.t, s.position.x, s.position.y, s.position.z,
            s.velocity.x, s.velocity.y, s.velocity.z,
            s.groundState, s.groundNormalY);
        os << buf;
    }
    if (out.empty()) { std::fputs(os.str().c_str(), stdout); return 0; }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { std::printf("cannot write %s\n", out.c_str()); return 1; }
    f << os.str();
    f.close();
    std::printf("wrote %s frames=%d final_x=%.4f final_y=%.4f\n",
                out.c_str(), cfg.frames, r.finalX, r.finalY);
    return 0;
}

int runCheckCharacter(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }

    struct Row {
        int frame = 0;
        double t = 0, px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0;
        int state = -1;
        double groundNy = 0;
    };
    std::vector<Row> rows;
    int  frames = -1, dataRows = 0;
    bool headerSeen = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# jolt=", 0) == 0) {
            std::istringstream is(line);
            std::string tok;
            while (is >> tok) {
                const auto eq = tok.find('=');
                if (eq == std::string::npos) continue;
                if (tok.substr(0, eq) == "frames") frames = std::atoi(tok.substr(eq + 1).c_str());
            }
            continue;
        }
        if (line.rfind("frame\t", 0) == 0) { headerSeen = true; continue; }
        if (line.empty() || line[0] == '#') continue;
        Row r;
        double gny = 0;
        if (std::sscanf(line.c_str(),
                        "%d %lf %lf %lf %lf %lf %lf %lf %d %lf",
                        &r.frame, &r.t, &r.px, &r.py, &r.pz,
                        &r.vx, &r.vy, &r.vz, &r.state, &gny) != 10) {
            std::printf("bad TSV row at data row %d\n", dataRows);
            return 1;
        }
        r.groundNy = gny;
        rows.push_back(r);
        ++dataRows;
    }

    expectTrue(headerSeen, "M-P4 dump has TSV column header");
    expectTrue(frames > 0 && dataRows == frames, "M-P4 dump row count = frames");
    expectTrue(!rows.empty(), "M-P4 dump non-empty");
    if (g_failures || rows.empty()) {
        std::printf("phys-p4 check-character: FAIL (%d)\n", g_failures);
        return 1;
    }

    // 结构锚：state 全程在枚举值域内（0=OnGround 1=OnSteepGround 2=NotSupported 3=InAir）。
    bool statesValid = true;
    for (const Row& r : rows)
        statesValid = statesValid && r.state >= 0 && r.state <= 3;
    expectTrue(statesValid, "M-P4 groundState within enum range");

    // 物理锚（从数据列独立复算，不信头注释；脚底 = py - 1.08）：
    const Row& first = rows.front();
    const Row& last  = rows.back();
    expectTrue(first.state == 0, "M-P4 starts on ground");
    // 无穿地：全程脚底 >= -0.01
    bool noPenetration = true;
    for (const Row& r : rows) noPenetration = noPenetration && (r.py - 1.08) >= -0.01;
    expectTrue(noPenetration, "M-P4 no floor penetration");
    // 30° 斜坡段（x in [6.2,9.8]）y 非降：爬得上去
    bool rampMonotonic = true;
    double prevY = -1e9;
    for (const Row& r : rows) {
        if (r.px >= -6.8 && r.px <= -3.2) {
            rampMonotonic = rampMonotonic && r.py >= prevY - 1e-9;
            prevY = r.py;
        }
    }
    expectTrue(rampMonotonic, "M-P4 climbs 30deg ramp (y monotonic)");
    // 平台1：x in [11,17] 存在样本 py ≈ rampTop + feetDelta
    bool onPlat1 = false;
    for (const Row& r : rows)
        if (r.px >= -2.0 && r.px <= 4.0 && std::abs(r.py - kCharPlat1PosY) < 0.06) onPlat1 = true;
    expectTrue(onPlat1, "M-P4 crosses platform1 (feet on ramp top)");
    // 台阶：x in [18,19.9] y 覆盖 >= 0.85（三级台阶 0.9，容差 0.05）
    double stepMinY = 1e9, stepMaxY = -1e9;
    for (const Row& r : rows)
        if (r.px >= kCharStepWinLo && r.px <= kCharStepWinHi) {
            stepMinY = std::min(stepMinY, r.py);
            stepMaxY = std::max(stepMaxY, r.py);
        }
    expectTrue(stepMaxY - stepMinY >= 0.85, "M-P4 walks all 3 steps (0.9 total rise)");
    // 平台2：x in [20,23.5] 存在样本 py ≈ stepTop + feetDelta
    bool onPlat2 = false;
    for (const Row& r : rows)
        if (r.px >= 7.0 && r.px <= 10.5 && std::abs(r.py - kCharPlat2PosY) < 0.06) onPlat2 = true;
    expectTrue(onPlat2, "M-P4 crosses platform2 (feet on step top)");
    // 60° 斜坡挡住：finalX 停在坡脚（没爬上去、也没穿过去）
    expectTrue(last.px >= 10.0 && last.px <= 11.6,
               "M-P4 blocked by 60deg slope (final x at slope foot)");
    expectTrue(std::abs(last.py - kCharPlat2PosY) < 0.08,
               "M-P4 parked at platform2 height");

    // 静止锚：末 60 帧 px/py 冻结（被挡后 walk 推进失败，位置不再变化）。
    const int from = std::max(0, static_cast<int>(rows.size()) - kCharTail);
    double tailSpread = 0.0;
    for (std::size_t i = static_cast<std::size_t>(from); i < rows.size(); ++i) {
        tailSpread = std::max(tailSpread, std::abs(rows[i].px - last.px));
        tailSpread = std::max(tailSpread, std::abs(rows[i].py - last.py));
    }
    expectTrue(tailSpread < 1e-3, "M-P4 tail 60 frames parked (blocked by steep slope)");

    if (g_failures) {
        std::printf("phys-p4 check-character: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-p4 check-character: PASS (%s, %d frames, final x=%.3f)\n",
                path, frames, last.px);
    return 0;
}


// ===== P8 M-D0: contact event stream TSV + trace/check loop =====
// Wire format (ASCII, tab separated). '#' lines are provenance ONLY -- the
// checker recomputes everything from the E/T data rows (M-P2 anti-false-green
// rule: never trust the summary lines).
std::string contactsTSV(const nl::physics::ContactProbeConfig& cfg,
                        const nl::physics::ContactProbeResult& r) {
    std::ostringstream os;
    os << "# nl-physics M-D0 contact events (real Jolt ContactListener output)\n";
    os << "# jolt=" << nl::physics::kJoltVersion
       << " job_system=single_threaded"
       << " dt=" << cfg.dt << " gravity=" << cfg.gravity
       << " radius=" << cfg.radius << " start_height=" << cfg.startHeight
       << " blocker_y=" << cfg.blockerY << " restitution=" << cfg.restitution
       << " frames=" << cfg.frames
       << " bodies=" << r.bodyCount
       << " events=" << r.events.size()
       << " cache_misses=" << r.removedCacheMisses << "\n";
    os << "# kind: 0=Added 1=Persisted 2=Removed; body/owner A is the low-id side\n";
    os << "# events: E frame kind ownerA ownerB bodyIdA bodyIdB px py pz normalY\n";
    char buf[320];
    for (const auto& e : r.events) {
        std::snprintf(buf, sizeof(buf),
            "E\t%d\t%d\t%llu\t%llu\t%u\t%u\t%.6f\t%.6f\t%.6f\t%.6f\n",
            e.frame, static_cast<int>(e.kind),
            static_cast<unsigned long long>(e.ownerA),
            static_cast<unsigned long long>(e.ownerB),
            e.bodyIdA, e.bodyIdB,
            e.worldPos.x, e.worldPos.y, e.worldPos.z, e.normalY);
        os << buf;
    }
    os << "# trajectory: T frame t y vy\n";
    for (const auto& s : r.trajectory) {
        std::snprintf(buf, sizeof(buf), "T\t%d\t%.6f\t%.6f\t%.6f\n",
                      s.frame, s.t, s.y, s.vy);
        os << buf;
    }
    return os.str();
}

int runContactTrace(int argc, char** argv) {
    nl::physics::ContactProbeConfig cfg;
    std::string out;
    auto fail = [](const std::string& e) -> int {
        std::printf("invalid arguments: %s\n", e.c_str());
        return 2;
    };
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 >= argc) return fail("missing value for " + key);
        const char* val = argv[++i];
        int n = 0;
        if      (key == "--out")    { out = val; }
        else if (key == "--frames") { if (!parseInt(val, &n)) return fail("bad --frames"); cfg.frames = n; }
        else return fail("unknown option " + key);
    }
    std::string err;
    if (!nl::physics::validateContactProbe(cfg, &err)) return fail(err);

    nl::physics::World world;
    const nl::physics::ContactProbeResult r = world.simulateContactProbe(cfg);
    if (!r.ok()) { std::printf("contact probe failed: %s\n", r.error.c_str()); return 1; }
    const std::string tsv = contactsTSV(cfg, r);
    if (out.empty()) { std::fputs(tsv.c_str(), stdout); return 0; }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { std::printf("cannot write %s\n", out.c_str()); return 1; }
    f << tsv;
    f.close();
    std::printf("wrote %s events=%zu frames=%d\n", out.c_str(), r.events.size(), cfg.frames);
    return 0;
}

struct ContactRow {
    int    frame = 0;
    int    kind  = -1;
    unsigned long long ownerA = 0, ownerB = 0;
    unsigned int  bodyA = 0, bodyB = 0;
    float  px = 0.0f, py = 0.0f, pz = 0.0f, normalY = 0.0f;
};

struct TrajectoryRow {
    int    frame = 0;
    double t = 0.0;
    float  y = 0.0f, vy = 0.0f;
};

int runContactCheck(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }

    std::vector<ContactRow> evs;
    std::vector<TrajectoryRow> traj;
    int    frames = -1, bodies = -1, cacheMisses = -1, declaredEvents = -1;
    double startHeight = -1.0;
    bool   provenance = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.find("# jolt=") == 0) provenance = true;
            auto grabInt = [&](const char* key, int* out) {
                const std::string k = std::string(key) + "=";
                const std::size_t p = line.find(k);
                if (p != std::string::npos) *out = std::atoi(line.c_str() + p + k.size());
            };
            auto grabReal = [&](const char* key, double* out) {
                const std::string k = std::string(key) + "=";
                const std::size_t p = line.find(k);
                if (p != std::string::npos) *out = std::strtod(line.c_str() + p + k.size(), nullptr);
            };
            grabInt("frames", &frames);
            grabInt("bodies", &bodies);
            grabInt("cache_misses", &cacheMisses);
            grabInt("events", &declaredEvents);
            grabReal("start_height", &startHeight);
            continue;
        }
        std::istringstream is(line);
        std::string tag;
        is >> tag;
        if (tag == "E") {
            ContactRow e;
            if (!(is >> e.frame >> e.kind >> e.ownerA >> e.ownerB
                      >> e.bodyA >> e.bodyB >> e.px >> e.py >> e.pz >> e.normalY)) {
                std::printf("phys-d0 contact-check: FAIL (malformed E row)\n");
                return 1;
            }
            evs.push_back(e);
        } else if (tag == "T") {
            TrajectoryRow s;
            if (!(is >> s.frame >> s.t >> s.y >> s.vy)) {
                std::printf("phys-d0 contact-check: FAIL (malformed T row)\n");
                return 1;
            }
            traj.push_back(s);
        } else {
            std::printf("phys-d0 contact-check: FAIL (unknown row tag)\n");
            return 1;
        }
    }

    expectTrue(provenance, "provenance header present");
    expectTrue(frames > 0 && bodies == 3, "provenance frames>0 and bodies=3");
    expectTrue(cacheMisses == 0, "Q2 owner cache: zero Removed misses");
    expectTrue(declaredEvents >= 0 && static_cast<int>(evs.size()) == declaredEvents,
               "declared event count matches data rows");
    expectTrue(!evs.empty(), "event stream is non-empty");
    expectTrue(traj.size() == static_cast<std::size_t>(frames) + 1,
               "trajectory length = frames + 1");
    if (!traj.empty()) {
        expectTrue(traj.front().frame == 0 && traj.back().frame == frames,
                   "trajectory frame range 0..frames");
        expectTrue(std::abs(traj.front().y - startHeight) < 1e-4,
                   "initial probe height matches provenance");
    }

    int added = 0, persisted = 0, removed = 0, firstAdded = -1, firstRemoved = -1;
    bool badKind = false, badOwner = false, badId = false, badFrame = false, badOrder = false;
    for (std::size_t i = 0; i < evs.size(); ++i) {
        const ContactRow& e = evs[i];
        if (e.kind < 0 || e.kind > 2) badKind = true;
        if (e.kind == 0) { ++added; if (firstAdded < 0) firstAdded = e.frame; }
        if (e.kind == 1) ++persisted;
        if (e.kind == 2) { ++removed; if (firstRemoved < 0) firstRemoved = e.frame; }
        // Every event, INCLUDING Removed, must carry a resolved owner pair:
        // that is the observable proof of the Q2 cache (no body access allowed).
        if (e.ownerA == 0 || e.ownerB == 0) badOwner = true;
        if (e.bodyA == nl::physics::kInvalidBodyId ||
            e.bodyB == nl::physics::kInvalidBodyId || e.bodyA == e.bodyB) badId = true;
        if (e.frame < 0 || e.frame > frames) badFrame = true;
        if (i > 0) {
            const ContactRow& p = evs[i - 1];
            if (e.frame < p.frame) badFrame = true;
            if (e.frame == p.frame) {
                auto key = [](const ContactRow& x) {
                    return std::make_pair(
                        std::make_pair(std::min(x.bodyA, x.bodyB), std::max(x.bodyA, x.bodyB)),
                        x.kind);
                };
                if (key(e) < key(p)) badOrder = true;
            }
        }
    }
    expectTrue(!badKind, "kind values in {0,1,2}");
    expectTrue(!badOwner, "every event (incl. Removed) has both owners");
    expectTrue(!badId, "body id pairs valid and distinct");
    expectTrue(!badFrame, "event frames in range and non-decreasing");
    expectTrue(!badOrder, "frame-end stable sort order holds");
    expectTrue(added > 0 && persisted > 0 && removed > 0,
               "Added / Persisted / Removed all observed");
    expectTrue(firstAdded > 0, "first Added occurs after frame 0");
    expectTrue(firstRemoved > firstAdded, "first Removed occurs after first Added");

    // MEASURED ANCHORS -- derived from the real 240-frame trace, not hand-written.
    // Standard course only; a non-standard --frames value still gets every
    // structural assertion above (event sequences are solver-derived, so their
    // numeric anchors cannot be recomputed from provenance alone).
    if (frames == 240) {
        expectTrue(static_cast<int>(evs.size()) == 41,
                   "measured: 41 events @240 frames");
        expectTrue(added == 6 && persisted == 29 && removed == 6,
                   "measured: Added/Persisted/Removed = 6/29/6");
        expectTrue(firstAdded == 44, "measured: first Added at frame 44");
        expectTrue(firstRemoved == 46, "measured: first Removed at frame 46");
        const int lastEventFrame = evs.empty() ? -1 : evs.back().frame;
        expectTrue(lastEventFrame == 185, "measured: last event at frame 185");
        int lateEvents = 0;
        for (const auto& e : evs) if (e.frame > 210) ++lateEvents;
        expectTrue(lateEvents == 0, "measured: quiet final 30 frames");
        bool contactOk = true;
        for (const auto& e : evs)
            if (std::abs(e.py - 3.000000f) > 1e-3f ||
                std::abs(e.normalY - 1.000000f) > 1e-3f) contactOk = false;
        expectTrue(contactOk, "measured: contact point y=3.0 / normalY=1.0");
        expectTrue(!traj.empty() && std::abs(traj.back().y - 3.500000f) < 1e-3f,
                   "measured: probe rests at y=3.500000");
    }

    if (g_failures) {
        std::printf("phys-d0 contact-check: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-d0 contact-check: PASS (%s, %zu events, %zu trajectory rows)\n",
                path, evs.size(), traj.size());
    return 0;
}

// ===== M-J5c: JobSystem 适配器验收（两层一起）=====
// 提取数据行（跳过 '#' 头）：TSV 头含 config 但不含 jobs——jobs 是运行期旋钮，
// 不入 TSV 保 M-P0~P4 基线字节稳定；跨 job-system 对拍只在数据行上做。
std::string jobsDataRows(const std::string& tsv) {
    std::string out;
    std::istringstream is(tsv);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        out += line;
        out += '\n';
    }
    return out;
}

// M-J5c 验收矩阵（ASCII-only 报告，供日志证据图渲染）：
//   层1 硬证明：适配器 N=1 与 SingleThreaded 数据行逐字节一致（drop + stack）。
//   层2 并行：N=threads 两次独立 fresh-world 模拟数据行逐字节一致（run-to-run）。
//   信息行：并行 N 与串行是否数值一致（一致则说明 Jolt 岛间无共享累加，
//   并行调度不影响浮点结果；不一致也不 FAIL，如实记录差异量级）。
bool jobsMatrix(int threads, int frames, std::string* report) {
    bool all = true;
    auto line = [report](const std::string& s) { if (report) *report += s + "\n"; };


    nl::physics::World world;
    nl::physics::DropConfig d;
    d.frames = frames;

    nl::physics::SimResult rST  = world.simulateDrop(d);

    d.jobsThreads = 1;

    nl::physics::SimResult rN1  = world.simulateDrop(d);

    d.jobsThreads = threads;

    nl::physics::SimResult rPa  = world.simulateDrop(d);


    nl::physics::SimResult rPb  = world.simulateDrop(d);

    if (!rST.ok() || !rN1.ok() || !rPa.ok() || !rPb.ok()) {
        line("drop sim failed: " + (rST.ok() ? (rN1.ok() ? (rPa.ok() ? rPb.error : rPa.error) : rN1.error) : rST.error));
        return false;
    }
    const std::string a  = jobsDataRows(trajectoryTSV(d, rST.samples, world.bodyCount()));
    const std::string b  = jobsDataRows(trajectoryTSV(d, rN1.samples, world.bodyCount()));
    const std::string c1 = jobsDataRows(trajectoryTSV(d, rPa.samples, world.bodyCount()));
    const std::string c2 = jobsDataRows(trajectoryTSV(d, rPb.samples, world.bodyCount()));
    line("drop: frames=" + std::to_string(frames) + " threads=" + std::to_string(threads));
    if (a == b) line("drop N=1 adapter vs SingleThreaded: IDENTICAL (hard proof)");
    else { line("drop N=1 adapter vs SingleThreaded: MISMATCH"); all = false; }
    if (c1 == c2) line("drop N=" + std::to_string(threads) + " run-to-run: IDENTICAL");
    else { line("drop N=" + std::to_string(threads) + " run-to-run: MISMATCH"); all = false; }
    line(c1 == a ? "drop parallel vs serial: IDENTICAL (island-parallel determinism)"
                 : "drop parallel vs serial: differs (informational, recorded)");

    // stack 交叉确认：接触求解 + 休眠岛路径同样过一遍三层矩阵。
    // （stackTSV 按 case 索引 rs[0..2]，必须整批传入，不能逐 case 调用。）
    nl::physics::StackConfig s;
    s.frames = frames < 600 ? 600 : frames;
    std::vector<nl::physics::StackResult> rsST, rsN1, rsP1, rsP2;

    for (int i = 0; i < kStackCaseCount; ++i) {
        s.jobsThreads = 0;
        rsST.push_back(runStackCase(kStackCases[i], s));
        s.jobsThreads = 1;
        rsN1.push_back(runStackCase(kStackCases[i], s));
        s.jobsThreads = threads;
        rsP1.push_back(runStackCase(kStackCases[i], s));
        rsP2.push_back(runStackCase(kStackCases[i], s));
    }
    s.jobsThreads = 0;
    bool stackOk = true;
    for (int i = 0; i < kStackCaseCount; ++i)
        stackOk = stackOk && rsST[i].ok() && rsN1[i].ok() && rsP1[i].ok() && rsP2[i].ok();
    if (!stackOk) {
        line("stack sim failed (see case errors above)");
        return false;
    }
    const std::string stA  = jobsDataRows(stackTSV(s, rsST));
    const std::string stB  = jobsDataRows(stackTSV(s, rsN1));
    const std::string stC1 = jobsDataRows(stackTSV(s, rsP1));
    const std::string stC2 = jobsDataRows(stackTSV(s, rsP2));
    line("stack: cases=" + std::to_string(kStackCaseCount) + " frames=" + std::to_string(s.frames));
    if (stA == stB) line("stack N=1 adapter vs SingleThreaded: IDENTICAL (hard proof)");
    else { line("stack N=1 adapter vs SingleThreaded: MISMATCH"); all = false; }
    if (stC1 == stC2) line("stack N=" + std::to_string(threads) + " run-to-run: IDENTICAL");
    else { line("stack N=" + std::to_string(threads) + " run-to-run: MISMATCH"); all = false; }
    line(stC1 == stA ? "stack parallel vs serial: IDENTICAL (island-parallel determinism)"
                     : "stack parallel vs serial: differs (informational, recorded)");

    return all;
}

int runCheckJobs(int argc, char** argv) {
    int threads = 4, frames = 240;
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--verbose") { g_verbose = true; continue; }
        if (i + 1 >= argc) { std::printf("invalid arguments: missing value for %s\n", key.c_str()); return 2; }
        const char* val = argv[++i];
        int n = 0;
        if      (key == "--threads") { if (!parseInt(val, &n) || n < 1 || n > 64) { std::printf("invalid arguments: bad --threads\n"); return 2; } threads = n; }
        else if (key == "--frames")  { if (!parseInt(val, &n) || n < 1)          { std::printf("invalid arguments: bad --frames\n");  return 2; } frames = n; }
        else { std::printf("invalid arguments: unknown option %s\n", key.c_str()); return 2; }
    }
    std::string report;
    const bool pass = jobsMatrix(threads, frames, &report);
    std::fputs(report.c_str(), stdout);
    if (!pass) {
        std::printf("phys-j5c check-jobs: FAIL\n");
        return 1;
    }
    std::printf("phys-j5c check-jobs: PASS (adapter N=1 bit-identical, N=%d run-to-run deterministic)\n", threads);
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


    // --- M-P2: stacking stability, sleep island, determinism ---
    nl::physics::StackConfig scfg;
    std::string serr;
    nl::physics::StackConfig snegFrames = scfg; snegFrames.frames = 0;
    expectTrue(!nl::physics::validateStack(snegFrames, &serr),
               "M-P2 reject frames below 60");
    nl::physics::StackConfig snegBoxes = scfg; snegBoxes.boxes = 1;
    expectTrue(!nl::physics::validateStack(snegBoxes, &serr),
               "M-P2 reject tower with fewer than 2 boxes");
    nl::physics::StackConfig snegFriction = scfg; snegFriction.friction = 1.5f;
    expectTrue(!nl::physics::validateStack(snegFriction, &serr),
               "M-P2 reject friction out of range");
    nl::physics::StackConfig snegGap = scfg; snegGap.gap = 2.0f;
    expectTrue(!nl::physics::validateStack(snegGap, &serr),
               "M-P2 reject gap larger than half extent");

    std::vector<nl::physics::StackResult> srs;
    for (int i = 0; i < kStackCaseCount; ++i) {
        auto sr = runStackCase(kStackCases[i], scfg);
        const std::string w = std::string("M-P2 ") + kStackCases[i].name;
        expectTrue(sr.ok(), (w + " world ok").c_str());
        expectTrue(static_cast<int>(sr.frames.size()) == scfg.frames + 1,
                   (w + " frame count").c_str());
        expectTrue(!sr.frames.empty() && sr.frames.back().bodies.size() == scfg.boxes,
                   (w + " body count").c_str());
        srs.push_back(std::move(sr));
    }
    const nl::physics::StackResult& sbase = srs[0];
    const nl::physics::StackResult& snosleep = srs[1];
    const nl::physics::StackResult& snofric = srs[2];
    expectTrue(!sbase.collapsed, "M-P2 baseline tower did not collapse");
    expectTrue(sbase.maxTailSpread < 0.005f, "M-P2 baseline tail converged (no jitter)");
    expectTrue(sbase.maxTailSpeed < 0.05f, "M-P2 baseline tail at rest (no velocity)");
    expectTrue(sbase.finalActive == 0, "M-P2 baseline island went to sleep");
    const nl::physics::StackFrame& slast = sbase.frames.back();
    for (std::uint32_t b = 0; b < scfg.boxes; ++b) {
        const float expectedY = scfg.halfExtent + static_cast<float>(b) * 2.0f * scfg.halfExtent;
        expectTrue(std::abs(slast.bodies[b].position.y - expectedY) < 0.15f,
                   ("M-P2 baseline cube " + std::to_string(b) + " held its slot").c_str());
    }
    expectTrue(!snosleep.collapsed && snosleep.maxTailSpread < 0.005f,
               "M-P2 no_sleep tower equally stable");
    expectTrue(snosleep.finalActive > 0, "M-P2 no_sleep island stays active");
    expectTrue(snofric.topDrift > 0.3f, "M-P2 no_friction top cube slid away");

    const auto st1 = runStackCase(kStackCases[0], scfg);
    const auto st2 = runStackCase(kStackCases[0], scfg);
    bool stackIdentical = st1.frames.size() == st2.frames.size();
    for (std::size_t k = 0; stackIdentical && k < st1.frames.size(); ++k) {
        const auto& a = st1.frames[k];
        const auto& b = st2.frames[k];
        stackIdentical = a.frame == b.frame && a.t == b.t &&
                         a.activeBodies == b.activeBodies && a.bodies.size() == b.bodies.size();
        for (std::size_t j = 0; stackIdentical && j < a.bodies.size(); ++j) {
            const auto& x = a.bodies[j];
            const auto& y = b.bodies[j];
            stackIdentical = x.position.x == y.position.x && x.position.y == y.position.y &&
                             x.position.z == y.position.z && x.velocity.x == y.velocity.x &&
                             x.velocity.y == y.velocity.y && x.velocity.z == y.velocity.z &&
                             x.active == y.active;
        }
    }
    expectTrue(stackIdentical, "M-P2 determinism: same tower twice bit-identical");


    // --- M-P3: collision-shape debug wireframe stream ---
    {
        nl::physics::DebugScene ds;
        std::string derr;
        expectTrue(ds.build(0, 0, &derr), "M-P3 debug scene builds");
        expectTrue(ds.bodyCount() == 4, "M-P3 debug scene has 4 bodies");
        const nl::physics::DebugLines l0 = ds.extractLines();
        expectTrue(l0.data != nullptr && l0.vertexCount > 0,
                   "M-P3 wireframe stream non-empty");
        expectTrue(l0.vertexCount % 2 == 0, "M-P3 LINE_LIST vertex pairs");
        const DebugFrameRow m0 = measureFrame(l0, 0, 0.0);
        expectTrue(m0.boxV > 0 && m0.sphereV > 0 && m0.capsuleV > 0 && m0.floorV > 0,
                   "M-P3 all four shape classes drawn");
        expectTrue((m0.boxV + m0.sphereV + m0.capsuleV + m0.floorV) == m0.vertices,
                   "M-P3 color buckets partition vertices");
        expectTrue(std::abs(m0.maxY - 4.8) < 0.05, "M-P3 initial top = capsule apex 4.8");

        for (int i = 0; i < 30; ++i) ds.step(kDebugDt);
        const nl::physics::DebugLines l30 = ds.extractLines();
        const DebugFrameRow m30 = measureFrame(l30, 30, 30.0 * kDebugDt);
        expectTrue(m30.vertices == m0.vertices && m30.boxV == m0.boxV &&
                   m30.sphereV == m0.sphereV && m30.capsuleV == m0.capsuleV &&
                   m30.floorV == m0.floorV,
                   "M-P3 wireframe topology independent of motion");
        // 30 步 = 0.5s，自由落体距离 0.5*g*t^2 ≈ 1.22m（触地发生在 ~0.81s）。
        expectTrue(m30.maxY < m0.maxY - 1.0, "M-P3 bodies fell after 30 steps");

        // 确定性对比必须在 ds 继续步进之前完成：l30 的 data 指向 ds 内部缓冲，
        // 再 extractLines 会复用同一缓冲（单帧消费模型）。
        nl::physics::DebugScene ds2;
        expectTrue(ds2.build(0, 0, &derr), "M-P3 second debug scene builds");
        for (int i = 0; i < 30; ++i) ds2.step(kDebugDt);
        const nl::physics::DebugLines l2 = ds2.extractLines();
        bool dbgIdentical = l2.vertexCount == l30.vertexCount;
        for (std::uint32_t k = 0; dbgIdentical && k < l30.vertexCount * 6u; ++k)
            dbgIdentical = l30.data[k] == l2.data[k];
        expectTrue(dbgIdentical, "M-P3 determinism: two scenes bit-identical");

        // 稳态：步进到 240 帧（默认睡眠开启），线框几何冻结在已知位姿。
        for (int i = 30; i < 240; ++i) ds.step(kDebugDt);
        const nl::physics::DebugLines lr = ds.extractLines();
        const DebugFrameRow mr = measureFrame(lr, 240, 240.0 * kDebugDt);
        expectTrue(std::abs(mr.minY + 1.0) < 1e-3, "M-P3 floor bottom at -1");
        expectTrue(std::abs(mr.maxY - 1.6) < 0.05, "M-P3 settled capsule apex 1.6");
    }

    // --- M-P4: character walk-through over ramp/steps/steep slope ---
    {
        nl::physics::CharacterConfig cc;   // 默认 1200 帧
        std::string cerr;

        // negative config paths
        auto badChar = cc; badChar.frames = 0;
        expectTrue(!nl::physics::validateCharacter(badChar, &cerr), "M-P4 reject frames=0");
        badChar = cc; badChar.dt = 1.0;
        expectTrue(!nl::physics::validateCharacter(badChar, &cerr), "M-P4 reject dt=1.0");
        badChar = cc; badChar.maxSlopeDeg = 90.0f;
        expectTrue(!nl::physics::validateCharacter(badChar, &cerr), "M-P4 reject maxSlopeDeg=90");

        nl::physics::World world;
        const nl::physics::CharacterResult r = world.simulateCharacter(cc);
        expectTrue(r.ok(), "M-P4 simulateCharacter runs");
        expectTrue(r.samples.size() == static_cast<std::size_t>(cc.frames),
                   "M-P4 sample count = frames");
        if (r.ok() && r.samples.size() == static_cast<std::size_t>(cc.frames)) {
            const nl::physics::CharacterSample& first = r.samples.front();
            const nl::physics::CharacterSample& last  = r.samples.back();
            expectTrue(first.groundState == 0, "M-P4 starts on ground");
            expectTrue(last.position.x >= 10.0f && last.position.x <= 11.6f,
                       "M-P4 blocked by 60deg slope at slope foot");
            expectTrue(std::abs(last.position.y - 4.3094) < 0.08,
                       "M-P4 parked at platform2 height (feet on step top)");

            bool noPen = true, statesValid = true, rampMono = true;
            double prevY = -1e9, stepMinY = 1e9, stepMaxY = -1e9;
            bool onPlat1 = false, onPlat2 = false;
            for (const nl::physics::CharacterSample& s : r.samples) {
                noPen      = noPen && (s.position.y - 1.08f) >= -0.01f;
                statesValid = statesValid && s.groundState >= 0 && s.groundState <= 3;
                if (s.position.x >= -6.8f && s.position.x <= -3.2f) {
                    rampMono = rampMono && s.position.y >= prevY - 1e-9f;
                    prevY = s.position.y;
                }
                if (s.position.x >= 4.3f && s.position.x <= 7.2f) {
                    stepMinY = std::min(stepMinY, static_cast<double>(s.position.y));
                    stepMaxY = std::max(stepMaxY, static_cast<double>(s.position.y));
                }
                if (s.position.x >= -2.0f && s.position.x <= 4.0f &&
                    std::abs(s.position.y - 3.4094) < 0.06) onPlat1 = true;
                if (s.position.x >= 7.0f && s.position.x <= 10.5f &&
                    std::abs(s.position.y - 4.3094) < 0.06) onPlat2 = true;
            }
            expectTrue(noPen, "M-P4 no floor penetration");
            expectTrue(statesValid, "M-P4 groundState within enum range");
            expectTrue(rampMono, "M-P4 climbs 30deg ramp (y monotonic)");
            expectTrue(onPlat1, "M-P4 crosses platform1");
            expectTrue(stepMaxY - stepMinY >= 0.85, "M-P4 walks all 3 steps");
            expectTrue(onPlat2, "M-P4 crosses platform2");

            // 静止锚：末 60 帧 px/py 冻结。
            double tailSpread = 0.0;
            const std::size_t from = r.samples.size() >= 60 ? r.samples.size() - 60 : 0;
            for (std::size_t i = from; i < r.samples.size(); ++i) {
                tailSpread = std::max(tailSpread,
                    std::abs(static_cast<double>(r.samples[i].position.x) - last.position.x));
                tailSpread = std::max(tailSpread,
                    std::abs(static_cast<double>(r.samples[i].position.y) - last.position.y));
            }
            expectTrue(tailSpread < 1e-3, "M-P4 tail 60 frames parked");
        }

        // 确定性：两次模拟逐位一致（M-P0 契约：fresh world + 固定步 + 单线程）。
        nl::physics::World world2;
        const nl::physics::CharacterResult r2 = world2.simulateCharacter(cc);
        bool charIdentical = r2.ok() && r2.samples.size() == r.samples.size();
        for (std::size_t i = 0; charIdentical && i < r.samples.size(); ++i) {
            const nl::physics::CharacterSample& a = r.samples[i];
            const nl::physics::CharacterSample& b = r2.samples[i];
            charIdentical = a.frame == b.frame && a.t == b.t &&
                            a.position.x == b.position.x && a.position.y == b.position.y &&
                            a.position.z == b.position.z && a.velocity.x == b.velocity.x &&
                            a.velocity.y == b.velocity.y && a.velocity.z == b.velocity.z &&
                            a.groundState == b.groundState &&
                            a.groundNormalY == b.groundNormalY;
        }
        expectTrue(charIdentical, "M-P4 determinism: same course twice bit-identical");
    }

    // M-J5c：JobSystem 适配器两层矩阵（N=1 逐位硬证明 + N=4 run-to-run）。
    {
        std::string report;
        // 【2026-11-10 崩溃加固】ThreadPool worker 的 std::thread 创建在系统
        // 资源紧张时间窗会抛 system_error（本机实测 ~30% 启动崩、WER 定位
        // ucrtbase fail-fast 0xC0000409、无缓冲定位崩在 jobsMatrix 首个
        // spawn——M-P4 determinism 行后零输出即死）。未捕获异常 = terminate =
        // abort fail-fast，整条 selftest 无声消失。这里转成显式 FAIL 行，
        // 崩溃率归零；确定性契约不受影响（逐位性与线程数无关，M-J5c/M-E1b.5）。
        bool j5c = false;
        try {
            j5c = jobsMatrix(4, 120, &report);
        } catch (const std::exception& e) {
            report += std::string("M-J5c aborted by exception: ") + e.what() + "\n";
        } catch (...) {
            report += "M-J5c aborted by unknown exception\n";
        }
        expectTrue(j5c,
                   "M-J5c adapter N=1 bit-identical + N=4 run-to-run deterministic");
        if (g_verbose) std::fputs(report.c_str(), stdout);
    }


    // ===== M-D0 contacts section: event bridge + Q2 cache + determinism =====
    {
        nl::physics::ContactProbeConfig cc;
        std::string cerr;
        auto badC = cc; badC.frames = 0;
        expectTrue(!nl::physics::validateContactProbe(badC, &cerr), "contacts: reject frames=0");
        badC = cc; badC.dt = 1.0;
        expectTrue(!nl::physics::validateContactProbe(badC, &cerr), "contacts: reject dt=1.0");
        badC = cc; badC.radius = 0.0f;
        expectTrue(!nl::physics::validateContactProbe(badC, &cerr), "contacts: reject radius=0");
        badC = cc; badC.startHeight = cc.blockerY;
        expectTrue(!nl::physics::validateContactProbe(badC, &cerr), "contacts: reject startHeight<=blocker");
        badC = cc; badC.restitution = 1.5f;
        expectTrue(!nl::physics::validateContactProbe(badC, &cerr), "contacts: reject restitution>1");
        expectTrue(nl::physics::validateContactProbe(cc, &cerr), "contacts: accept default config");

        nl::physics::World cw;
        const nl::physics::ContactProbeResult a = cw.simulateContactProbe(cc);
        expectTrue(a.ok(), "contacts: probe succeeded");
        expectTrue(a.bodyCount == 3, "contacts: 3 bodies (floor/blocker/probe)");
        expectTrue(a.trajectory.size() == static_cast<std::size_t>(cc.frames) + 1,
                   "contacts: trajectory length = frames+1");
        expectTrue(!a.events.empty(), "contacts: event stream non-empty");
        expectTrue(a.removedCacheMisses == 0, "contacts: Q2 owner cache zero misses");

        int ca = 0, cp = 0, cr = 0;
        bool ownersOk = true, idsOk = true, sortedOk = true;
        for (std::size_t i = 0; i < a.events.size(); ++i) {
            const auto& e = a.events[i];
            if (e.kind == nl::physics::ContactKind::Added) ++ca;
            else if (e.kind == nl::physics::ContactKind::Persisted) ++cp;
            else if (e.kind == nl::physics::ContactKind::Removed) ++cr;
            if (e.ownerA == 0 || e.ownerB == 0) ownersOk = false;
            if (e.bodyIdA == nl::physics::kInvalidBodyId ||
                e.bodyIdB == nl::physics::kInvalidBodyId || e.bodyIdA == e.bodyIdB) idsOk = false;
            if (i > 0 && e.frame < a.events[i - 1].frame) sortedOk = false;
        }
        expectTrue(ca > 0 && cp > 0 && cr > 0,
                   "contacts: Added/Persisted/Removed all observed");
        expectTrue(ownersOk, "contacts: every event carries a resolved owner pair");
        expectTrue(idsOk, "contacts: body id pairs valid");
        expectTrue(sortedOk, "contacts: events frame-ordered");

        // Determinism: fresh world, byte-identical POD stream (incl. float bits).
        const nl::physics::ContactProbeResult b = cw.simulateContactProbe(cc);
        bool identical = b.ok() && b.events.size() == a.events.size();
        if (identical) {
            for (std::size_t i = 0; i < a.events.size(); ++i) {
                const auto& x = a.events[i];
                const auto& y = b.events[i];
                if (x.frame != y.frame || x.kind != y.kind ||
                    x.ownerA != y.ownerA || x.ownerB != y.ownerB ||
                    x.bodyIdA != y.bodyIdA || x.bodyIdB != y.bodyIdB ||
                    x.worldPos.x != y.worldPos.x || x.worldPos.y != y.worldPos.y ||
                    x.worldPos.z != y.worldPos.z || x.normalY != y.normalY) {
                    identical = false; break;
                }
            }
        }
        expectTrue(identical, "contacts: two worlds produce bit-identical events");

        // MEASURED ANCHORS -- derived from the real 240-frame trace, not hand-written.
        expectTrue(ca == 6 && cp == 29 && cr == 6,
                   "contacts: measured kind counts 6/29/6");
        expectTrue(static_cast<int>(a.events.size()) == 41,
                   "contacts: measured 41 events");
        int firstA = -1, firstR = -1, lastF = -1;
        for (const auto& e : a.events) {
            if (firstA < 0 && e.kind == nl::physics::ContactKind::Added) firstA = e.frame;
            if (firstR < 0 && e.kind == nl::physics::ContactKind::Removed) firstR = e.frame;
            lastF = e.frame;
        }
        expectTrue(firstA == 44, "contacts: first Added at frame 44");
        expectTrue(firstR == 46, "contacts: first Removed at frame 46");
        expectTrue(lastF == 185, "contacts: last event at frame 185");
        if (g_verbose) {
            std::printf("[contacts] added=%d persisted=%d removed=%d total=%zu misses=%u\n",
                        ca, cp, cr, a.events.size(), a.removedCacheMisses);
        }
    }

    if (g_failures) {
        std::printf("phys-p0 selftest: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("phys-d0 contacts: PASS (ContactListener POD, added/persisted/removed, cache=0)\n");
    std::printf("phys-j5c anchors: PASS (PoolJobSystem adapter, N=1 bit-identical, N=4 parallel)\n");
    std::printf("phys-p4 anchors: PASS (character walk-through, ramp/steps/steep slope, determinism)\n");
    std::printf("phys-p3 anchors: PASS (debug wireframe stream, 4 bodies, determinism)\n");
    std::printf("phys-p2 anchors: PASS (3 stack cases, sleep island, determinism)\n");
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
        else if (key == "--jobs")        { if (!parseInt(val, &n)) { *err = "bad --jobs"; return false; } cfg->jobsThreads = n; }
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
    if (cmd == "stack") return runStack(argc, argv);
    if (cmd == "check-stack") {
        if (argc != 3) { usage(); return 2; }
        return runCheckStack(argv[2]);
    }
    if (cmd == "debug") return runDebug(argc, argv);
    if (cmd == "check-debug") {
        if (argc != 3) { usage(); return 2; }
        return runCheckDebug(argv[2]);
    }
    if (cmd == "character") return runCharacter(argc, argv);
    if (cmd == "check-character") {
        if (argc != 3) { usage(); return 2; }
        return runCheckCharacter(argv[2]);
    }
    if (cmd == "check-jobs") return runCheckJobs(argc, argv);
    if (cmd == "contact-trace") return runContactTrace(argc, argv);
    if (cmd == "contact-check") {
        if (argc != 3) { usage(); return 2; }
        return runContactCheck(argv[2]);
    }
    usage();
    return 2;
}
