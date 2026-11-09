// gl-tools —— P8 游戏循环（M-GL0，A 类并行轨道）自测与确定性 trace 工具。
// 消费 nl::core::GameClock（双帧模型，见 core/include/nl/core/game_clock.hpp）。
// 约定沿用 phys-tools：exit 0=PASS 1=FAIL 2=usage error；输出纯 ASCII。
// 用法：
//   gl-tools selftest [--verbose]
//   gl-tools trace [--out <tsv>] [--frames N] [--dt MS] [--dts a,b,c]
//                  [--scale S] [--pause-at N] [--resume-at N]
#include "nl/core/game_clock.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    std::printf("gl-tools - nl-engine P8 game-loop tools (M-GL0, GameClock dual-frame model)\n");
    std::printf("usage:\n");
    std::printf("  gl-tools selftest [--verbose]\n");
    std::printf("  gl-tools trace [--out <tsv>] [--frames N] [--dt MS] [--dts a,b,c]\n");
    std::printf("                 [--scale S] [--pause-at N] [--resume-at N]\n");
    std::printf("exit: 0=PASS 1=FAIL 2=usage error\n");
}

bool parseDouble(const char* s, double* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool parseInt(const char* s, long long* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (!end || *end != '\0') return false;
    *out = v;
    return true;
}

// 近似相等（双精度聚合误差容忍；确定性锚点用逐位比较另行覆盖）。
bool nearEq(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
// selftest —— 机械锚点（手算可验 + 2026-11-10 python 同参数预演核对）
// ---------------------------------------------------------------------------
struct TickRecord {
    double dt;
    std::uint64_t index;
};

std::vector<TickRecord> recordTicks(nl::core::GameClock* clock, double realDtMs) {
    std::vector<TickRecord> rec;
    clock->tick(realDtMs, [&](double dt, std::uint64_t idx) {
        rec.push_back({dt, idx});
    });
    return rec;
}

void selftest() {
    using nl::core::GameClock;
    const double kStep = GameClock::kStep;

    // [1] 零 / 负 / NaN realDt 按防御性 0 处理：不 tick、simTime 不动。
    {
        GameClock c;
        expectTrue(recordTicks(&c, 0.0).empty(), "zero-dt frame produces no tick");
        expectTrue(recordTicks(&c, -5.0).empty(), "negative-dt frame produces no tick");
        expectTrue(recordTicks(&c, std::nan("")).empty(), "NaN-dt frame produces no tick");
        expectTrue(c.simTime() == 0.0 && c.tickCount() == 0, "invalid dt leaves clock untouched");
    }

    // [2] 单帧累积：16ms（<1/60）不 tick；16.7ms tick 一次；余量进 accumulator。
    {
        GameClock c;
        expectTrue(recordTicks(&c, 16.0).empty(), "16ms < step: no tick");
        expectTrue(nearEq(c.accumulator(), 0.016, 1e-12), "16ms accumulates");
        auto rec = recordTicks(&c, 16.7);
        expectTrue(rec.size() == 1 && rec[0].dt == kStep && rec[0].index == 0,
                   "16.7ms produces exactly one step tick at index 0");
        expectTrue(nearEq(c.simTime(), kStep, 1e-12), "simTime == one step");
        expectTrue(c.alpha() >= 0.0 && c.alpha() < 1.0, "alpha in [0,1)");
    }

    // [3] 长跑节奏：100 x 16.7ms = 1.67s 逻辑时间 -> floor(100.2) = 100 tick。
    {
        GameClock c;
        std::uint64_t sum = 0;
        for (int i = 0; i < 100; ++i) sum += recordTicks(&c, 16.7).size();
        expectTrue(c.tickCount() == 100 && sum == 100, "100x16.7ms -> exactly 100 ticks");
        expectTrue(nearEq(c.simTime(), 100.0 / 60.0, 1e-9), "simTime == 100 ticks worth");
        expectTrue(nearEq(c.simTime(), c.tickCount() * kStep, 1e-9),
                   "invariant simTime == tickCount*step");
    }

    // [4] maxDelta 截断（双重用途之一）：单帧 5000ms 只补 250ms = 15 tick（python 预演 15）。
    {
        GameClock c;
        auto rec = recordTicks(&c, 5000.0);
        expectTrue(rec.size() == 15, "5000ms clamped to 250ms -> exactly 15 ticks");
        expectTrue(nearEq(c.simTime(), 15.0 * kStep, 1e-12), "simTime == 15 steps after burst");
        expectTrue(nearEq(c.frameDt(), GameClock::kMaxDelta, 1e-12), "frameDt reports clamped delta");
    }

    // [5] 暂停语义（bevy：时钟停摆，非 Carrot：烧 accumulator）：
    //     暂停期间一切冻结；恢复后从冻结点继续，无追帧爆发。
    {
        GameClock c;
        recordTicks(&c, 16.7); // 1 tick
        const double accBefore = c.accumulator();
        const double simBefore = c.simTime();
        c.setPaused(true);
        expectTrue(recordTicks(&c, 10000.0).empty(), "paused frame produces no tick");
        expectTrue(nearEq(c.accumulator(), accBefore, 0.0) && nearEq(c.simTime(), simBefore, 0.0),
                   "paused frame freezes accumulator and simTime exactly");
        expectTrue(c.frameDt() == 0.0, "paused frameDt is 0");
        c.setPaused(false);
        auto rec = recordTicks(&c, 16.7);
        expectTrue(rec.size() == 1 && rec[0].index == 1, "resume continues tick index without burst");
    }

    // [6] timeScale 倍速：x2 → 60 帧 x16.7ms → 120 tick；x0.5 → 30 tick（python 预演一致）。
    {
        GameClock c;
        c.setTimeScale(2.0);
        for (int i = 0; i < 60; ++i) recordTicks(&c, 16.7);
        expectTrue(c.tickCount() == 120, "scale x2: 60x16.7ms -> 120 ticks");
        expectTrue(nearEq(c.simTime(), 2.0, 1e-9), "scale x2: simTime advances 2x real time");
    }
    {
        GameClock c;
        c.setTimeScale(0.5);
        for (int i = 0; i < 60; ++i) recordTicks(&c, 16.7);
        expectTrue(c.tickCount() == 30, "scale x0.5: 60x16.7ms -> 30 ticks");
    }

    // [7] maxDelta 双重用途之二：高倍速 + 中帧也被封顶（scale 10 x 100ms = 1s 逻辑，
    //     只补 250ms = 15 tick，防止死亡螺旋）。
    {
        GameClock c;
        c.setTimeScale(10.0);
        auto rec = recordTicks(&c, 100.0);
        expectTrue(rec.size() == 15, "scale 10 x 100ms clamped to 250ms -> 15 ticks");
    }

    // [8] timeScale 边界：负值/NaN → 0（冻结）；0 → 冻结；越界钳到 kMaxTimeScale。
    {
        GameClock c;
        c.setTimeScale(-1.0);
        expectTrue(c.timeScale() == 0.0, "negative scale clamped to 0");
        c.setTimeScale(std::nan(""));
        expectTrue(c.timeScale() == 0.0, "NaN scale clamped to 0");
        c.setTimeScale(1e9);
        expectTrue(c.timeScale() == GameClock::kMaxTimeScale, "huge scale clamped to kMaxTimeScale");
        c.setTimeScale(0.0);
        expectTrue(recordTicks(&c, 100.0).empty(), "scale 0 freezes clock");
    }

    // [9] 确定性：同一 realDt 序列两次运行，tick 索引序列与 simTime 逐位一致。
    {
        const double dts[] = {16.7, 16.7, 33.4, 8.0, 5000.0, 16.7, 4.0, 4.0, 16.7};
        GameClock a, b;
        std::vector<std::uint64_t> ia, ib;
        for (double d : dts) for (const auto& r : recordTicks(&a, d)) ia.push_back(r.index);
        for (double d : dts) for (const auto& r : recordTicks(&b, d)) ib.push_back(r.index);
        bool same = (ia.size() == ib.size()) && (a.simTime() == b.simTime());
        for (size_t i = 0; same && i < ia.size(); ++i) same = (ia[i] == ib[i]);
        expectTrue(same && !ia.empty(), "identical dt sequence reproduces identical tick trace");
    }

    // [10] reset：时间线归零（simTime/accumulator/tickCount/frameDt），控制量保留。
    {
        GameClock c;
        for (int i = 0; i < 30; ++i) recordTicks(&c, 16.7);
        c.setTimeScale(2.0);
        c.setPaused(true);
        c.reset();
        expectTrue(c.simTime() == 0.0 && c.tickCount() == 0 && c.accumulator() == 0.0,
                   "reset zeroes the timeline");
        expectTrue(c.timeScale() == 2.0 && c.paused(), "reset keeps timeScale/paused");
    }

    std::printf("gl-m0 selftest: %s (%s)\n", g_failures ? "FAIL" : "PASS",
                g_failures ? "see FAIL lines above" : "GameClock anchors");
}

// ---------------------------------------------------------------------------
// trace —— 确定性 trace（TSV：frame/realDtMs/ticks/tickCount/simTime/alpha/paused）
// ---------------------------------------------------------------------------

struct TraceOpts {
    std::vector<double> dts;   // 每渲染帧 realDt（ms）；--frames 模式下为常值
    double scale = 1.0;
    long long pauseAt = -1;    // 从该帧（0 起）起暂停
    long long resumeAt = -1;   // 从该帧（0 起）起恢复
    const char* out = nullptr;
};

int cmdTrace(const TraceOpts& o) {
    nl::core::GameClock c;
    c.setTimeScale(o.scale);

    std::FILE* f = nullptr;
    if (o.out) {
        f = std::fopen(o.out, "wb");
        if (!f) {
            std::printf("gl-m0 trace: FAIL (cannot open %s)\n", o.out);
            return 1;
        }
        std::fprintf(f, "frame\trealDtMs\tticks\ttickCount\tsimTime\talpha\tpaused\n");
    }

    std::uint64_t totalTicks = 0;
    const long long n = static_cast<long long>(o.dts.size());
    for (long long i = 0; i < n; ++i) {
        if (o.pauseAt >= 0 && i >= o.pauseAt && (o.resumeAt < 0 || i < o.resumeAt)) {
            c.setPaused(true);
        } else if (o.resumeAt >= 0 && i >= o.resumeAt) {
            c.setPaused(false);
        }
        const std::uint64_t before = c.tickCount();
        c.tick(o.dts[static_cast<size_t>(i)], nullptr);
        const std::uint64_t ticks = c.tickCount() - before;
        totalTicks += ticks;
        if (f) {
            std::fprintf(f, "%lld\t%.6f\t%llu\t%llu\t%.9f\t%.9f\t%d\n",
                         i, o.dts[static_cast<size_t>(i)],
                         static_cast<unsigned long long>(ticks),
                         static_cast<unsigned long long>(c.tickCount()),
                         c.simTime(), c.alpha(), c.paused() ? 1 : 0);
        }
    }

    if (f) std::fclose(f);
    std::printf("gl-m0 trace: PASS (frames=%lld ticks=%llu simTime=%.6f alpha=%.4f)\n",
                n, static_cast<unsigned long long>(totalTicks), c.simTime(), c.alpha());
    return 0;
}

int run(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];

    if (cmd == "selftest") {
        for (int i = 2; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--verbose")) g_verbose = true;
            else { usage(); return 2; }
        }
        selftest();
        return g_failures ? 1 : 0;
    }

    if (cmd == "trace") {
        TraceOpts o;
        o.dts.push_back(16.7); // 默认 240 帧 x 16.7ms
        long long frames = 240;
        bool framesGiven = false, dtsGiven = false;
        for (int i = 2; i < argc; ++i) {
            const auto need = [&](const char* name) -> const char* {
                if (i + 1 >= argc) { std::printf("invalid arguments: missing value for %s\n", name); return nullptr; }
                return argv[++i];
            };
            if (!std::strcmp(argv[i], "--out")) {
                const char* v = need("--out"); if (!v) return 2;
                o.out = v;
            } else if (!std::strcmp(argv[i], "--frames")) {
                const char* v = need("--frames"); if (!v) return 2;
                long long n = 0;
                if (!parseInt(v, &n) || n < 1) {
                    std::printf("invalid arguments: bad --frames\n");
                    return 2;
                }
                frames = n; framesGiven = true;
            } else if (!std::strcmp(argv[i], "--dt")) {
                const char* v = need("--dt"); if (!v) return 2;
                double d = 0;
                if (!parseDouble(v, &d) || d < 0.0) {
                    std::printf("invalid arguments: bad --dt\n");
                    return 2;
                }
                o.dts.assign({d});
            } else if (!std::strcmp(argv[i], "--dts")) {
                const char* v = need("--dts"); if (!v) return 2;
                std::vector<double> list;
                const char* p = v;
                while (*p) {
                    char* end = nullptr;
                    const double d = std::strtod(p, &end);
                    if (end == p || !std::isfinite(d) || d < 0.0 || (*end && *end != ',')) {
                        std::printf("invalid arguments: bad --dts list\n");
                        return 2;
                    }
                    list.push_back(d);
                    p = (*end == ',') ? end + 1 : end;
                }
                if (list.empty()) { std::printf("invalid arguments: empty --dts list\n"); return 2; }
                o.dts = list; dtsGiven = true;
            } else if (!std::strcmp(argv[i], "--scale")) {
                const char* v = need("--scale"); if (!v) return 2;
                double s = 0;
                if (!parseDouble(v, &s) || s < 0.0) {
                    std::printf("invalid arguments: bad --scale\n");
                    return 2;
                }
                o.scale = s;
            } else if (!std::strcmp(argv[i], "--pause-at")) {
                const char* v = need("--pause-at"); if (!v) return 2;
                long long n = 0;
                if (!parseInt(v, &n) || n < 0) {
                    std::printf("invalid arguments: bad --pause-at\n");
                    return 2;
                }
                o.pauseAt = n;
            } else if (!std::strcmp(argv[i], "--resume-at")) {
                const char* v = need("--resume-at"); if (!v) return 2;
                long long n = 0;
                if (!parseInt(v, &n) || n < 0) {
                    std::printf("invalid arguments: bad --resume-at\n");
                    return 2;
                }
                o.resumeAt = n;
            } else {
                std::printf("invalid arguments: unknown option %s\n", argv[i]);
                return 2;
            }
        }
        if (framesGiven && dtsGiven) {
            std::printf("invalid arguments: --frames and --dts are mutually exclusive\n");
            return 2;
        }
        if (!dtsGiven) o.dts.assign(static_cast<size_t>(frames), o.dts[0]);
        if (o.resumeAt >= 0 && o.pauseAt >= 0 && o.resumeAt <= o.pauseAt) {
            std::printf("invalid arguments: --resume-at must be after --pause-at\n");
            return 2;
        }
        return cmdTrace(o);
    }

    usage();
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    return run(argc, argv);
}
