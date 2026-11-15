// M-SP0 sprite-tools — headless acceptance for the core-side 2D cluster.
//
// Scope: atlas grid UV math, flipbook timeline, and texture-keyed batch
// binning. All three are pure CPU PODs in nl_core; NO graphics API here.
// Exit codes: 0 = PASS, 1 = assertion failure, 2 = usage error.
//
// Commands:
//   selftest                      run all three oracle checks in-process
//   atlas-trace     --out <tsv>   dump deterministic atlas UV rows
//   atlas-check     <tsv>         verify the TSV against an independent oracle
//   flipbook-trace  --out <tsv>   dump deterministic timeline samples
//   flipbook-check  <tsv>         verify the TSV
//   batch-trace     --out <tsv>   dump sorted sprites + buckets
//   batch-check     <tsv>         verify the TSV

#include <nl/sprite/atlas.hpp>
#include <nl/sprite/flipbook.hpp>
#include <nl/sprite/sprite_desc.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr float kEps = 1.0e-5f;

void usage() {
    std::fprintf(stderr,
        "sprite-tools (M-SP0) usage:\n"
        "  selftest\n"
        "  atlas-trace    --out <tsv> | atlas-check <tsv>\n"
        "  flipbook-trace --out <tsv> | flipbook-check <tsv>\n"
        "  batch-trace    --out <tsv> | batch-check <tsv>\n"
        "exit: 0=PASS 1=assert 2=usage\n");
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

bool near(float a, float b) { return std::fabs(a - b) <= kEps; }

// ---------------------------------------------------------------- atlas ----

struct AtlasRow {
    std::string name;
    std::uint32_t dimX = 0, dimY = 0, addr = 0, index = 0;
    int   ok = 0;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

std::vector<AtlasRow> buildAtlasRows() {
    struct Case { const char* name; std::uint32_t dx, dy; nl::sprite::Addressing addr;
                  std::vector<std::uint32_t> indices; };
    const std::vector<Case> cases = {
        {"grid4x4_throw",  4, 4, nl::sprite::Addressing::Throw,  {0,1,4,5,15,16,99}},
        {"grid4x4_repeat", 4, 4, nl::sprite::Addressing::Repeat, {0,15,16,17,99}},
        {"grid4x4_clamp",  4, 4, nl::sprite::Addressing::Clamp,  {0,15,16,99}},
        {"grid2x3_throw",  2, 3, nl::sprite::Addressing::Throw,  {0,1,2,5,6}},
        {"grid1x1_throw",  1, 1, nl::sprite::Addressing::Throw,  {0,1}},
        {"grid0x4_throw",  0, 4, nl::sprite::Addressing::Throw,  {0}},
    };
    std::vector<AtlasRow> rows;
    for (const Case& c : cases) {
        nl::sprite::AtlasGrid grid;
        grid.dimX = c.dx; grid.dimY = c.dy; grid.addressing = c.addr;
        for (std::uint32_t idx : c.indices) {
            AtlasRow r;
            r.name = c.name;
            r.dimX = c.dx; r.dimY = c.dy;
            r.addr = static_cast<std::uint32_t>(c.addr);
            r.index = idx;
            // AtlasGrid::uvRect() leaves rect untouched on failure, and UvRect's
            // defaults are (0,0,1,1) -- so seed an explicit all-zero rect to make
            // the failure case distinguishable in the TSV.
            nl::sprite::UvRect uv{0.0f, 0.0f, 0.0f, 0.0f};
            r.ok = grid.uvRect(idx, &uv) ? 1 : 0;
            r.u0 = uv.u0; r.v0 = uv.v0; r.u1 = uv.u1; r.v1 = uv.v1;
            rows.push_back(r);
        }
    }
    return rows;
}

// Independent oracle: recompute expected cell purely with integer arithmetic,
// never calling AtlasGrid. Also asserts geometric invariants.
bool checkAtlasRows(const std::vector<AtlasRow>& rows, std::string* err) {
    if (rows.empty()) { *err = "atlas: empty trace"; return false; }
    for (const AtlasRow& r : rows) {
        const std::uint32_t count = r.dimX * r.dimY;
        std::uint32_t i = r.index;
        bool expectOk = true;
        if (r.dimX == 0u || r.dimY == 0u) {
            expectOk = false;
        } else if (i >= count) {
            if (r.addr == 0u) expectOk = false;
            else if (r.addr == 1u) i %= count;
            else i = count - 1u;
        }
        if (r.ok != (expectOk ? 1 : 0)) {
            *err = "atlas: ok mismatch at " + r.name + "/" + std::to_string(r.index);
            return false;
        }
        if (!expectOk) {
            if (!(near(r.u0, 0.0f) && near(r.v0, 0.0f) &&
                  near(r.u1, 0.0f) && near(r.v1, 0.0f))) {
                *err = "atlas: failed row must leave rect zeroed at " + r.name;
                return false;
            }
            continue;
        }
        const float dx = 1.0f / static_cast<float>(r.dimX);
        const float dy = 1.0f / static_cast<float>(r.dimY);
        const float eu0 = static_cast<float>(i % r.dimX) * dx;
        const float ev0 = static_cast<float>(i / r.dimX) * dy;
        if (!(near(r.u0, eu0) && near(r.v0, ev0) &&
              near(r.u1, eu0 + dx) && near(r.v1, ev0 + dy))) {
            *err = "atlas: rect mismatch at " + r.name + "/" + std::to_string(r.index);
            return false;
        }
        if (r.u0 < -kEps || r.v0 < -kEps || r.u1 > 1.0f + kEps || r.v1 > 1.0f + kEps ||
            r.u1 <= r.u0 || r.v1 <= r.v0) {
            *err = "atlas: rect out of bounds at " + r.name;
            return false;
        }
    }
    // Cross-case: Repeat(16) == Throw(0), Clamp(99) == Throw(15).
    auto find = [&](const char* name, std::uint32_t idx) -> const AtlasRow* {
        for (const AtlasRow& r : rows)
            if (r.name == name && r.index == idx) return &r;
        return nullptr;
    };
    const AtlasRow* repeat16 = find("grid4x4_repeat", 16);
    const AtlasRow* throw0 = find("grid4x4_throw", 0);
    const AtlasRow* clamp99 = find("grid4x4_clamp", 99);
    const AtlasRow* throw15 = find("grid4x4_throw", 15);
    if (!repeat16 || !throw0 || !clamp99 || !throw15) {
        *err = "atlas: cross-case rows missing"; return false;
    }
    if (!(near(repeat16->u0, throw0->u0) && near(repeat16->v0, throw0->v0) &&
          near(clamp99->u0, throw15->u0) && near(clamp99->v0, throw15->v0))) {
        *err = "atlas: addressing cross-case mismatch"; return false;
    }
    return true;
}

bool writeAtlas(const char* path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "name\tdimX\tdimY\taddr\tindex\tok\tu0\tv0\tu1\tv1\n";
    char buf[256];
    for (const AtlasRow& r : buildAtlasRows()) {
        std::snprintf(buf, sizeof(buf), "%.6f\t%.6f\t%.6f\t%.6f",
                      r.u0, r.v0, r.u1, r.v1);
        f << r.name << '\t' << r.dimX << '\t' << r.dimY << '\t' << r.addr << '\t'
          << r.index << '\t' << r.ok << '\t' << buf << '\n';
    }
    return f.good();
}

bool readAtlas(const char* path, std::vector<AtlasRow>* rows, std::string* err) {
    std::ifstream f(path);
    if (!f) { *err = "atlas: cannot open trace"; return false; }
    std::string line;
    std::getline(f, line);   // header
    if (line.rfind("name\tdimX", 0) != 0) { *err = "atlas: bad header"; return false; }
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const auto c = splitTabs(line);
        if (c.size() != 10) { *err = "atlas: bad column count"; return false; }
        AtlasRow r;
        r.name = c[0];
        r.dimX = static_cast<std::uint32_t>(std::stoul(c[1]));
        r.dimY = static_cast<std::uint32_t>(std::stoul(c[2]));
        r.addr = static_cast<std::uint32_t>(std::stoul(c[3]));
        r.index = static_cast<std::uint32_t>(std::stoul(c[4]));
        r.ok = std::stoi(c[5]);
        r.u0 = std::stof(c[6]); r.v0 = std::stof(c[7]);
        r.u1 = std::stof(c[8]); r.v1 = std::stof(c[9]);
        rows->push_back(r);
    }
    return true;
}

// ------------------------------------------------------------- flipbook ----

struct FlipRow {
    std::string name;
    int step = 0, state = 0, finished = 0;
    std::uint32_t frame = 0;
    float time = 0.0f;
};

std::vector<FlipRow> buildFlipRows() {
    nl::sprite::Clip clip;
    clip.fps = 12.0f;
    clip.runs = {{0u, 2u}, {3u, 1u}, {7u, 3u}};  // total 6 timeline frames
    const auto emit = [&](std::vector<FlipRow>* out, const char* name,
                          nl::sprite::FlipbookPlayer* p, int step) {
        FlipRow r;
        r.name = name; r.step = step;
        r.state = static_cast<int>(p->state());
        r.finished = p->finished() ? 1 : 0;
        r.frame = p->frame();
        r.time = p->timeFrames();
        out->push_back(r);
    };
    std::vector<FlipRow> rows;
    {
        nl::sprite::FlipbookPlayer p;
        p.setClip(&clip); p.setLoop(true); p.play();
        for (int i = 0; i <= 20; ++i) {
            emit(&rows, "loop_on", &p, i);
            p.tick(0.1f);
        }
    }
    {
        nl::sprite::FlipbookPlayer p;
        p.setClip(&clip); p.setLoop(false); p.play();
        for (int i = 0; i <= 12; ++i) {
            emit(&rows, "loop_off", &p, i);
            p.tick(0.1f);
        }
    }
    {
        nl::sprite::FlipbookPlayer p;
        p.setClip(&clip); p.setLoop(true); p.setReverse(true); p.play();
        for (int i = 0; i <= 8; ++i) {
            emit(&rows, "reverse", &p, i);
            p.tick(0.1f);
        }
    }
    {
        nl::sprite::FlipbookPlayer p;
        p.setClip(&clip); p.play();
        p.tick(0.1f);
        p.pause();
        emit(&rows, "paused", &p, 0);
        p.tick(0.5f);
        emit(&rows, "paused", &p, 1);
        p.play();
        p.tick(0.1f);
        emit(&rows, "resumed", &p, 0);
        p.stop();
        emit(&rows, "stopped", &p, 0);
    }
    return rows;
}

bool checkFlipRows(const std::vector<FlipRow>& rows, std::string* err) {
    if (rows.empty()) { *err = "flipbook: empty trace"; return false; }
    const std::uint32_t kTotal = 6u;
    bool sawFinished = false;
    std::uint32_t loopOnFirst = 0;
    bool haveLoopFirst = false;
    for (const FlipRow& r : rows) {
        if (!(r.frame == 0u || r.frame == 3u || r.frame == 7u)) {
            *err = "flipbook: frame outside clip set at " + r.name; return false;
        }
        if (r.time < -kEps || r.time > static_cast<float>(kTotal) + kEps) {
            *err = "flipbook: time out of range at " + r.name; return false;
        }
        if (r.name == "loop_on") {
            if (r.state != static_cast<int>(nl::sprite::FlipbookPlayer::Playing) ||
                r.finished != 0) {
                *err = "flipbook: loop must stay Playing and never latch finished";
                return false;
            }
            if (!haveLoopFirst) { loopOnFirst = r.frame; haveLoopFirst = true; }
            if (r.step == 20 && r.frame != loopOnFirst && r.name == "loop_on") {
                // 20 * 0.1 * 12 = 24 frames = exactly 4 loops -> first frame again.
                *err = "flipbook: loop did not return to start after 4 exact periods";
                return false;
            }
        }
        if (r.name == "loop_off") {
            if (r.finished) sawFinished = true;
            if (sawFinished && r.state != static_cast<int>(nl::sprite::FlipbookPlayer::Stopped)) {
                *err = "flipbook: finished must latch Stopped"; return false;
            }
        }
        if (r.name == "stopped" && (r.time != 0.0f || r.finished != 0 ||
                                    r.state != static_cast<int>(nl::sprite::FlipbookPlayer::Stopped))) {
            *err = "flipbook: stop must reset time/finished/state"; return false;
        }
    }
    if (!sawFinished) { *err = "flipbook: non-looping clip never finished"; return false; }
    // Pause must not advance time.
    const FlipRow *pa = nullptr, *pb = nullptr;
    for (const FlipRow& r : rows) {
        if (r.name == "paused" && r.step == 0) pa = &r;
        if (r.name == "paused" && r.step == 1) pb = &r;
    }
    if (!pa || !pb || !near(pa->time, pb->time)) {
        *err = "flipbook: pause advanced the timeline"; return false;
    }
    return true;
}

bool writeFlip(const char* path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "name\tstep\tstate\tfinished\tframe\ttime\n";
    for (const FlipRow& r : buildFlipRows()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", r.time);
        f << r.name << '\t' << r.step << '\t' << r.state << '\t' << r.finished
          << '\t' << r.frame << '\t' << buf << '\n';
    }
    return f.good();
}

bool readFlip(const char* path, std::vector<FlipRow>* rows, std::string* err) {
    std::ifstream f(path);
    if (!f) { *err = "flipbook: cannot open trace"; return false; }
    std::string line;
    std::getline(f, line);
    if (line.rfind("name\tstep", 0) != 0) { *err = "flipbook: bad header"; return false; }
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const auto c = splitTabs(line);
        if (c.size() != 6) { *err = "flipbook: bad column count"; return false; }
        FlipRow r;
        r.name = c[0]; r.step = std::stoi(c[1]); r.state = std::stoi(c[2]);
        r.finished = std::stoi(c[3]);
        r.frame = static_cast<std::uint32_t>(std::stoul(c[4]));
        r.time = std::stof(c[5]);
        rows->push_back(r);
    }
    return true;
}

// ---------------------------------------------------------------- batch ----

struct BatchSpriteRow {
    std::uint32_t pos = 0, layer = 0, textureId = 0, sequence = 0;
};
struct BatchBucketRow {
    std::uint32_t begin = 0, count = 0, textureId = 0, layer = 0;
};

struct BatchTrace {
    std::vector<BatchSpriteRow> sprites;
    std::vector<BatchBucketRow> buckets;
};

BatchTrace buildBatchTrace() {
    // Deliberately interleaved: same textureId in two layers, same layer with
    // three textures, and a kInvalidTextureId entry (must split into its own bucket).
    const std::uint32_t ids[] = {2, 1, 2, 1, 0, 2, 0xFFFFFFFFu, 1, 0, 2};
    const std::uint32_t layers[] = {0, 0, 1, 0, 0, 1, 0, 1, 0, 0};
    nl::sprite::SpriteBatch batch;
    for (std::size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        nl::sprite::SpriteDesc s;
        s.textureId = ids[i];
        s.layer = layers[i];
        s.x = static_cast<float>(i);
        batch.add(s);
    }
    batch.sortByTexture();
    BatchTrace t;
    for (std::size_t i = 0; i < batch.sprites().size(); ++i) {
        const auto& s = batch.sprites()[i];
        t.sprites.push_back({static_cast<std::uint32_t>(i), s.layer,
                             s.textureId, s.sequence});
    }
    for (const auto& b : batch.buckets()) {
        std::uint32_t tid = 0xFFFFFFFFu, layer = 0;
        if (b.count > 0u && b.begin < t.sprites.size()) {
            tid = t.sprites[b.begin].textureId;
            layer = t.sprites[b.begin].layer;
        }
        t.buckets.push_back({b.begin, b.count, tid, layer});
    }
    return t;
}

bool checkBatchTrace(const BatchTrace& t, std::string* err) {
    if (t.sprites.empty() || t.buckets.empty()) {
        *err = "batch: empty trace"; return false;
    }
    // Global order: (layer, textureId, sequence) non-decreasing.
    for (std::size_t i = 1; i < t.sprites.size(); ++i) {
        const auto& a = t.sprites[i - 1];
        const auto& b = t.sprites[i];
        const bool ordered = a.layer < b.layer ||
            (a.layer == b.layer && a.textureId < b.textureId) ||
            (a.layer == b.layer && a.textureId == b.textureId &&
             a.sequence < b.sequence);
        if (!ordered) { *err = "batch: sprites not stably sorted"; return false; }
        if (b.pos != i) { *err = "batch: sprite positions not contiguous"; return false; }
    }
    // Buckets are contiguous, non-empty, cover the whole batch, and every
    // element inside a bucket shares (layer, textureId).
    std::uint32_t expectedBegin = 0;
    for (const auto& b : t.buckets) {
        if (b.count == 0u || b.begin != expectedBegin) {
            *err = "batch: buckets not contiguous/non-empty"; return false;
        }
        if (b.begin + b.count > t.sprites.size()) {
            *err = "batch: bucket overruns sprite array"; return false;
        }
        for (std::uint32_t k = b.begin; k < b.begin + b.count; ++k) {
            if (t.sprites[k].textureId != b.textureId ||
                t.sprites[k].layer != b.layer) {
                *err = "batch: bucket mixes textures or layers"; return false;
            }
        }
        expectedBegin += b.count;
    }
    if (expectedBegin != t.sprites.size()) {
        *err = "batch: buckets do not cover all sprites"; return false;
    }
    return true;
}

bool writeBatch(const char* path) {
    const BatchTrace t = buildBatchTrace();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "#type\tpos\tbegin\tcount\tlayer\ttextureId\tsequence\n";
    for (const auto& s : t.sprites)
        f << "S\t" << s.pos << "\t0\t0\t" << s.layer << '\t' << s.textureId
          << '\t' << s.sequence << '\n';
    for (const auto& b : t.buckets)
        f << "B\t0\t" << b.begin << '\t' << b.count << '\t' << b.layer
          << '\t' << b.textureId << "\t0\n";
    return f.good();
}

bool readBatch(const char* path, BatchTrace* t, std::string* err) {
    std::ifstream f(path);
    if (!f) { *err = "batch: cannot open trace"; return false; }
    std::string line;
    std::getline(f, line);
    if (line.rfind("#type", 0) != 0) { *err = "batch: bad header"; return false; }
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const auto c = splitTabs(line);
        if (c.size() != 7) { *err = "batch: bad column count"; return false; }
        if (c[0] == "S") {
            BatchSpriteRow r;
            r.pos = static_cast<std::uint32_t>(std::stoul(c[1]));
            r.layer = static_cast<std::uint32_t>(std::stoul(c[4]));
            r.textureId = static_cast<std::uint32_t>(std::stoul(c[5]));
            r.sequence = static_cast<std::uint32_t>(std::stoul(c[6]));
            t->sprites.push_back(r);
        } else if (c[0] == "B") {
            BatchBucketRow r;
            r.begin = static_cast<std::uint32_t>(std::stoul(c[2]));
            r.count = static_cast<std::uint32_t>(std::stoul(c[3]));
            r.layer = static_cast<std::uint32_t>(std::stoul(c[4]));
            r.textureId = static_cast<std::uint32_t>(std::stoul(c[5]));
            t->buckets.push_back(r);
        } else {
            *err = "batch: unknown row type"; return false;
        }
    }
    return true;
}

// -------------------------------------------------------------- driver ----

int runSelftest() {
    std::string err;
    if (!checkAtlasRows(buildAtlasRows(), &err)) {
        std::fprintf(stderr, "[FAIL] %s\n", err.c_str()); return 1;
    }
    std::printf("  [PASS] atlas grid UV rect + addressing (6 cases)\n");
    if (!checkFlipRows(buildFlipRows(), &err)) {
        std::fprintf(stderr, "[FAIL] %s\n", err.c_str()); return 1;
    }
    std::printf("  [PASS] flipbook timeline (loop/reverse/pause/stop/finished)\n");
    if (!checkBatchTrace(buildBatchTrace(), &err)) {
        std::fprintf(stderr, "[FAIL] %s\n", err.c_str()); return 1;
    }
    std::printf("  [PASS] sprite batch stable sort + texture buckets\n");
    // POD size anchors (must match renderer/src/sprite2d.hpp and sprite2d.slang).
    // Compile-time assertion: sizeof is a constant expression, an if() here trips C4127.
    static_assert(sizeof(nl::sprite::UvRect) == 16u, "UvRect must be 16B");
    std::printf("  [PASS] POD sizes (UvRect=16B)\n");
    std::printf("SELFTEST PASS (4/4)\n");
    return 0;
}

int runTrace(const char* kind, const char* path) {
    const std::string k(kind);
    const bool ok = k == "atlas" ? writeAtlas(path)
                  : k == "flipbook" ? writeFlip(path)
                  : writeBatch(path);
    if (!ok) { std::fprintf(stderr, "[FAIL] cannot write %s\n", path); return 1; }
    std::printf("%s trace -> %s\n", kind, path);
    return 0;
}

int runCheck(const char* kind, const char* path) {
    const std::string k(kind);
    std::string err;
    bool ok = false;
    if (k == "atlas") {
        std::vector<AtlasRow> rows;
        ok = readAtlas(path, &rows, &err) && checkAtlasRows(rows, &err);
    } else if (k == "flipbook") {
        std::vector<FlipRow> rows;
        ok = readFlip(path, &rows, &err) && checkFlipRows(rows, &err);
    } else {
        BatchTrace t;
        ok = readBatch(path, &t, &err) && checkBatchTrace(t, &err);
    }
    if (!ok) { std::fprintf(stderr, "[FAIL] %s\n", err.c_str()); return 1; }
    std::printf("%s check PASS -> %s\n", kind, path);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd(argv[1]);
    if (cmd == "selftest") {
        if (argc != 2) { usage(); return 2; }
        return runSelftest();
    }
    const std::string kind =
        cmd.rfind("atlas-", 0) == 0 ? "atlas" :
        cmd.rfind("flipbook-", 0) == 0 ? "flipbook" :
        cmd.rfind("batch-", 0) == 0 ? "batch" : "";
    if (kind.empty()) { usage(); return 2; }
    const bool isTrace = cmd == kind + "-trace";
    const bool isCheck = cmd == kind + "-check";
    if (!isTrace && !isCheck) { usage(); return 2; }
    if (isTrace) {
        if (argc != 4 || std::string(argv[2]) != "--out") { usage(); return 2; }
        return runTrace(kind.c_str(), argv[3]);
    }
    if (argc != 3) { usage(); return 2; }
    return runCheck(kind.c_str(), argv[2]);
}
