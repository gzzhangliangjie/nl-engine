// gi-tools —— S11 世界缓存（Surface Cache / Hash Grid）CPU 侧仿真与无头验收工具。
// 计划底线（docs/s11/S11-PLAN.md）：「先 CPU 仿真验证 hashmap 语义，再上 GPU」——
// 本工具的 WorldCacheSim 与 renderer/shaders/world_cache.slang 是同一套语义的
// CPU/GPU 双实现（扁平数组 + pcg 定槽 + iqint 校验和 + 3 步线性探测 + life 衰减
// 清零立即重用，Q11-0 拍板方案 A，跟 bevy/Carrot 口径）。
//
// 子命令（退出码 0=PASS / 1=断言失败 / 2=用法错误，对齐 nav-tools / 展示台）：
//   gi-tools selftest [cachesim]        CPU hashmap 语义单测（ctest 挂载点）
//   gi-tools cachesim --frames N --png out.png [--size N]
//                                       确定性占用仿真（合成场景表面采样 →
//                                       N 帧 touch+decay）+ 俯视占用热图 PNG
//
// 证据规则：example 卡引用的 PNG 一律来自本工具真实输出，不许手绘示意。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 与 world_cache.slang 同构的哈希（pcg / iqint 是公开通用算法，自写实现）。
namespace hashes {

std::uint32_t pcg(std::uint32_t input) {
    const std::uint32_t state = input * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

std::uint32_t iqint(std::uint32_t input) {
    const std::uint32_t n = (input << 13u) ^ input;
    return n * (n * n * 15731u + 789221u) + 1376312589u;
}

} // namespace hashes

// ---------------------------------------------------------------------------
// WorldCacheSim：GPU 世界缓存语义的 CPU 参照实现（单线程 = CAS 的退化形态）。
// 量化口径与 GPU 一致：cellSize=0.5m → floor(pos*2)；法线 floor(n+0.0001)。
struct WorldCacheSim {
    struct Cell {
        std::uint32_t checksum = 0;   // 0 = 空槽（bevy WORLD_CACHE_EMPTY_CELL 同义）
        std::uint32_t life     = 0;   // 衰减寿命；查询命中即续命
        float pos[3] = {0.f, 0.f, 0.f};
        float nrm[3] = {0.f, 1.f, 0.f};
    };

    std::uint32_t capacity;       // 2 的幂
    std::uint32_t mask;
    std::uint32_t lifetime;       // 未被查询可存活帧数（bevy 10 / Carrot 20）
    static constexpr std::uint32_t kMaxProbeSteps = 3;   // Q11-0：bevy 探测上限
    std::vector<Cell> table;

    // 最近一次 touch 的诊断（selftest / cachesim 断言用）。
    std::uint32_t lastProbes = 0;
    bool        lastHit    = false;

    explicit WorldCacheSim(std::uint32_t cap = 1u << 18, std::uint32_t life = 10)
        : capacity(cap), mask(cap - 1), lifetime(life), table(cap) {
        // capacity 必须是 2 的幂（wrap_key = key & (cap-1) 的前提）。
        if (cap == 0 || (cap & (cap - 1)) != 0) {
            std::fprintf(stderr, "WorldCacheSim: capacity must be power of two\n");
            std::exit(2);
        }
    }

    static std::uint32_t u(int v) { return static_cast<std::uint32_t>(v); }

    // 与 shader 同构：量化 → pcg 链定槽 / iqint 链出校验和（0 保留为空槽）。
    void makeKey(const float pos[3], const float nrm[3],
                 std::uint32_t& key, std::uint32_t& checksum) const {
        const int px = static_cast<int>(std::floor(pos[0] * 2.0f));
        const int py = static_cast<int>(std::floor(pos[1] * 2.0f));
        const int pz = static_cast<int>(std::floor(pos[2] * 2.0f));
        const int nx = static_cast<int>(std::floor(nrm[0] + 0.0001f));
        const int ny = static_cast<int>(std::floor(nrm[1] + 0.0001f));
        const int nz = static_cast<int>(std::floor(nrm[2] + 0.0001f));
        std::uint32_t k = hashes::pcg(u(px));
        k = hashes::pcg(k + u(py));
        k = hashes::pcg(k + u(pz));
        k = hashes::pcg(k + u(nx));
        k = hashes::pcg(k + u(ny));
        k = hashes::pcg(k + u(nz));
        key = k & mask;
        std::uint32_t c = hashes::iqint(u(px));
        c = hashes::iqint(c + u(py));
        c = hashes::iqint(c + u(pz));
        c = hashes::iqint(c + u(nx));
        c = hashes::iqint(c + u(ny));
        c = hashes::iqint(c + u(nz));
        checksum = std::max(c, 1u);   // 0 保留为空槽
    }

    // 查询即插入（bevy 口径）：命中 → 续命；空槽 → 占用初始化；碰撞 → 线性探测。
    // 返回 false = 探测上限内没找到也没抢到空槽（表满/链满，丢弃本次样本）。
    bool touch(const float pos[3], const float nrm[3]) {
        std::uint32_t key, checksum;
        makeKey(pos, nrm, key, checksum);
        for (std::uint32_t i = 0; i < kMaxProbeSteps; ++i) {
            Cell& c = table[key];
            if (c.checksum == checksum) {           // 命中：续命
                c.life = lifetime;
                lastProbes = i + 1;
                lastHit = true;
                return true;
            }
            if (c.checksum == 0) {                  // 空槽：占用初始化（CAS 成功态）
                c.checksum = checksum;
                c.life = lifetime;
                std::memcpy(c.pos, pos, sizeof(c.pos));
                std::memcpy(c.nrm, nrm, sizeof(c.nrm));
                lastProbes = i + 1;
                lastHit = false;
                return true;
            }
            key = (key + 1u) & mask;                // 碰撞：线性探测下一格
        }
        lastProbes = kMaxProbeSteps;
        lastHit = false;
        return false;                               // 桶满（Carrot none 同语义）
    }

    // 只查不插（viz 口径）：命中返回槽号，未命中返回 capacity。
    // 与 touch 的差别：不占用空槽、不续命（M11.0 viz 的 query-only 形态）。
    std::uint32_t find(const float pos[3], const float nrm[3]) const {
        std::uint32_t key, checksum;
        makeKey(pos, nrm, key, checksum);
        for (std::uint32_t i = 0; i < kMaxProbeSteps; ++i) {
            const Cell& c = table[key];
            if (c.checksum == checksum) return key;
            if (c.checksum == 0) return capacity;   // 探测链撞到空槽 = 必不存在
            key = (key + 1u) & mask;
        }
        return capacity;
    }

    // 每帧全表衰减（bevy decay_world_cache）：life-1，到 0 清 checksum 立即重用。
    void decay() {
        for (Cell& c : table) {
            if (c.life > 0u) {
                c.life -= 1u;
                if (c.life == 0u) c.checksum = 0;
            }
        }
    }

    std::uint32_t usedSlots() const {
        std::uint32_t n = 0;
        for (const Cell& c : table) if (c.checksum != 0) ++n;
        return n;
    }
    std::uint32_t activeCells() const {
        std::uint32_t n = 0;
        for (const Cell& c : table) if (c.life != 0) ++n;
        return n;
    }
};

// ---------------------------------------------------------------------------
// 确定性 LCG（selftest / cachesim 共用；固定种子 = 字节级可复现）。
struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(s >> 32);
    }
    float f() { return static_cast<float>(next()) * (1.0f / 4294967296.0f); }
    float range(float a, float b) { return a + (b - a) * f(); }
};

// ---------------------------------------------------------------------------
// 最小 PNG 写出（RGB8，zlib stored blocks + CRC32/adler32，零依赖、确定性字节）。
// 先例：nav-tools M-GL 轮次自写编码器（行切片，防 bytes(rgb[y]) 单字节误用）。
namespace png {

void putU32(std::vector<unsigned char>& v, std::uint32_t x) {
    v.push_back((x >> 24) & 0xFF);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back(x & 0xFF);
}

std::uint32_t crc32(const unsigned char* p, size_t n) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(const unsigned char* p, size_t n) {
    std::uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void chunk(std::vector<unsigned char>& out, const char* type,
           const unsigned char* data, size_t n) {
    putU32(out, static_cast<std::uint32_t>(n));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + n);
    const std::uint32_t c = crc32(&out[start], out.size() - start);
    putU32(out, c);
}

bool write(const char* path, int w, int h,
           const std::vector<unsigned char>& rgb, std::string& err) {
    // raw = 每行前置 filter byte 0 的 RGB 平面
    std::vector<unsigned char> raw(static_cast<size_t>(h) * (1 + w * 3));
    for (int y = 0; y < h; ++y) {
        raw[static_cast<size_t>(y) * (1 + w * 3)] = 0; // filter: None
        std::memcpy(&raw[static_cast<size_t>(y) * (1 + w * 3) + 1],
                    &rgb[static_cast<size_t>(y) * w * 3],
                    static_cast<size_t>(w) * 3);
    }
    // zlib stream：stored blocks（每块 <= 65535）
    std::vector<unsigned char> z;
    z.push_back(0x78); z.push_back(0x01); // CMF/FLG（无预设字典）
    size_t off = 0;
    while (off < raw.size()) {
        const size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF);
        z.push_back((n >> 8) & 0xFF);
        const std::uint32_t an = ~static_cast<std::uint32_t>(n) & 0xFFFF;
        z.push_back(an & 0xFF);
        z.push_back((an >> 8) & 0xFF);
        z.insert(z.end(), &raw[off], &raw[off] + n);
        off += n;
    }
    const std::uint32_t ad = adler32(raw.data(), raw.size());
    putU32(z, ad);

    std::vector<unsigned char> out;
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);
    unsigned char ihdr[13] = {0};
    const std::uint32_t iw = static_cast<std::uint32_t>(w);
    const std::uint32_t ih = static_cast<std::uint32_t>(h);
    ihdr[0] = (iw >> 24) & 0xFF; ihdr[1] = (iw >> 16) & 0xFF;
    ihdr[2] = (iw >> 8) & 0xFF;  ihdr[3] = iw & 0xFF;
    ihdr[4] = (ih >> 24) & 0xFF; ihdr[5] = (ih >> 16) & 0xFF;
    ihdr[6] = (ih >> 8) & 0xFF;  ihdr[7] = ih & 0xFF;
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 2;  // color type RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(out, "IHDR", ihdr, 13);
    chunk(out, "IDAT", z.data(), z.size());
    chunk(out, "IEND", nullptr, 0);

    std::FILE* f = std::fopen(path, "wb");
    if (!f) { err = std::string("cannot open for write: ") + path; return false; }
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) { err = "short write"; return false; }
    return true;
}

} // namespace png

// ---------------------------------------------------------------------------
// selftest：CPU hashmap 语义断言（插入/查找/回收/续命/碰撞自愈/容量边界/确定性）。
static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { std::printf("  ok   %s\n", msg); }                         \
        else { std::printf("  FAIL %s\n", msg); ++g_fail; }                    \
    } while (0)

static bool selftestCachesim() {
    std::printf("gi-tools cachesim selftest:\n");

    // 1. 插入/查找往返 + 二次 touch = 命中。
    {
        WorldCacheSim sim(1u << 12);
        Lcg rng(1);
        bool allNew = true, allHit = true;
        for (int i = 0; i < 100; ++i) {
            const float p[3] = {rng.range(-8, 8), rng.range(-8, 8), rng.range(-8, 8)};
            const float n[3] = {0, 1, 0};
            if (!sim.touch(p, n) || sim.lastHit) allNew = false;
            if (!sim.touch(p, n) || !sim.lastHit) allHit = false;
        }
        CHECK(allNew, "insert: 100 first touches are all new");
        CHECK(allHit, "insert: 100 second touches are all hits");
        CHECK(sim.usedSlots() == 100, "insert: 100 distinct slots used");
    }

    // 2. 校验和区分度：1000 个不同 cell 不共享 checksum（双哈希防假命中的前提）。
    {
        WorldCacheSim sim(1u << 14);
        Lcg rng(2);
        std::vector<std::uint32_t> seen;
        seen.reserve(1000);
        int collisions = 0;
        for (int i = 0; i < 1000; ++i) {
            const float p[3] = {rng.range(-32, 32), rng.range(-32, 32), rng.range(-32, 32)};
            const float n[3] = {1, 0, 0};
            std::uint32_t k1, c1;
            sim.makeKey(p, n, k1, c1);
            if (std::find(seen.begin(), seen.end(), c1) != seen.end()) ++collisions;
            seen.push_back(c1);
        }
        CHECK(collisions == 0, "checksum: 1000 distinct cells have distinct checksums");
    }

    // 3. 回收：不续命的 cell 在 lifetime 帧后清零，重插回同槽（自愈）。
    {
        WorldCacheSim sim(1u << 12);
        const float p[3] = {0.3f, -1.2f, 2.7f};
        const float n[3] = {0, 0, 1};
        sim.touch(p, n);
        const std::uint32_t slot = sim.find(p, n);
        CHECK(slot != sim.capacity, "decay: cell findable right after insert");
        for (std::uint32_t f = 0; f < sim.lifetime; ++f) sim.decay();
        CHECK(sim.find(p, n) == sim.capacity, "decay: cell gone after lifetime frames");
        CHECK(sim.usedSlots() == 0, "decay: slot cleared for immediate reuse");
        sim.touch(p, n);
        CHECK(sim.find(p, n) == slot, "decay: re-insert lands back in same slot");
    }

    // 4. 续命：每帧 touch 的 cell 存活远超 lifetime。
    {
        WorldCacheSim sim(1u << 12);
        const float p[3] = {1.5f, 0.25f, -0.75f};
        const float n[3] = {0, 1, 0};
        bool alive = true;
        for (int f = 0; f < static_cast<int>(sim.lifetime) * 5; ++f) {
            sim.touch(p, n);
            sim.decay();
            if (sim.find(p, n) == sim.capacity) { alive = false; break; }
        }
        CHECK(alive, "renew: touched cell survives 5x lifetime");
    }

    // 5. 碰撞自愈：低负载（6.25%）下压出零星碰撞后全部仍可 find，无假命中。
    //    （3 步探测上限下高负载本就有真实失败率——α³ 量级，bevy 用 2^20 大表
    //    压负载同因；全落盘断言会假挂，故用低负载保证「落盘的全找得到」。）
    {
        WorldCacheSim sim(1u << 12);   // 4096 槽
        Lcg rng(3);
        std::vector<std::array<float, 3>> ps;
        std::vector<std::array<float, 3>> ns;
        int inserted = 0;
        for (int i = 0; i < 256; ++i) {
            std::array<float, 3> p = {rng.range(-16, 16), rng.range(-16, 16),
                                      rng.range(-16, 16)};
            std::array<float, 3> n = {0, 1, 0};
            if (sim.touch(p.data(), n.data())) {
                ++inserted;
                ps.push_back(p);
                ns.push_back(n);
            }
        }
        CHECK(inserted == 256, "probe: 256 cells into 4096 slots all land");
        bool allFound = true, geoOk = true;
        for (size_t i = 0; i < ps.size(); ++i) {
            const std::uint32_t s = sim.find(ps[i].data(), ns[i].data());
            if (s == sim.capacity) { allFound = false; break; }
            if (std::memcmp(sim.table[s].pos, ps[i].data(), sizeof(float) * 3) != 0)
                geoOk = false;   // 假命中实锤：槽里的几何不是查的这个 cell
        }
        CHECK(allFound, "probe: all inserted cells findable");
        CHECK(geoOk, "probe: no false hits (geometry matches query)");
    }

    // 6. 容量边界：表满后 touch 明确失败（丢弃样本），语义不崩；衰减后可复用。
    {
        WorldCacheSim sim(64);
        Lcg rng(4);
        int accepted = 0, rejected = 0;
        for (int i = 0; i < 200; ++i) {
            const float p[3] = {rng.range(-64, 64), rng.range(-64, 64), rng.range(-64, 64)};
            const float n[3] = {0, 1, 0};
            if (sim.touch(p, n)) ++accepted; else ++rejected;
        }
        CHECK(accepted <= 64 && accepted >= 60,
              "capacity: inserts stop at table capacity (no overcommit)");
        CHECK(rejected == 200 - accepted,
              "capacity: overflow touches rejected explicitly");
        CHECK(sim.usedSlots() == 64, "capacity: table not corrupted by overflow");
        for (std::uint32_t f = 0; f < sim.lifetime; ++f) sim.decay();
        const float p2[3] = {0.1f, 0.1f, 0.1f};
        const float n2[3] = {0, 1, 0};
        CHECK(sim.touch(p2, n2), "capacity: decay frees slots for reuse");
    }

    // 7. 确定性：同一输入序列两次仿真，占用指纹（fnv1a of checksums）逐位一致。
    {
        auto run = [](std::uint64_t seed) {
            WorldCacheSim sim(1u << 12);
            Lcg rng(seed);
            for (int f = 0; f < 30; ++f) {
                for (int i = 0; i < 256; ++i) {
                    const float p[3] = {rng.range(-8, 8), rng.range(-2, 2), rng.range(-8, 8)};
                    const float n[3] = {0, 1, 0};
                    sim.touch(p, n);
                }
                sim.decay();
            }
            std::uint64_t h = 1469598103934665603ull;
            for (const WorldCacheSim::Cell& c : sim.table) {
                const unsigned char* b = reinterpret_cast<const unsigned char*>(&c);
                for (size_t i = 0; i < sizeof(WorldCacheSim::Cell); ++i) {
                    h ^= b[i];
                    h *= 1099511628211ull;
                }
            }
            return h;
        };
        CHECK(run(7) == run(7), "determinism: same seed -> identical fingerprint");
        CHECK(run(7) != run(8), "determinism: different seed -> different fingerprint");
    }

    if (g_fail == 0) std::printf("gi-tools cachesim selftest: PASS\n");
    else std::printf("gi-tools cachesim selftest: FAIL (%d)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// cachesim：确定性占用仿真 + 俯视热图 PNG（工具真实输出 = example 证据图）。
// 场景：地面 + 两个轴对齐箱体，每帧表面采样 4096 点 touch；life 衰减同帧推进。
static void printUsage() {
    std::printf("gi-tools — S11 world cache CPU simulation (exit 0=PASS/1=FAIL/2=usage)\n"
                "  gi-tools selftest [cachesim]      CPU hashmap semantics unit tests\n"
                "  gi-tools cachesim --frames N --png out.png [--size N]\n"
                "                                    deterministic occupancy sim + heatmap\n");
}

static int cmdCachesim(int argc, char** argv) {
    int frames = 120;
    int size = 512;
    const char* pngPath = nullptr;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--frames") frames = std::atoi(need("--frames"));
        else if (a == "--png") pngPath = need("--png");
        else if (a == "--size") size = std::atoi(need("--size"));
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return 2; }
    }
    if (frames <= 0 || size <= 0) { std::fprintf(stderr, "bad --frames/--size\n"); return 2; }

    WorldCacheSim sim(1u << 18, 10);
    Lcg rng(20261111u);

    // 合成场景：地面 y=-1（-6..6）² + 两个箱（顶 + 两个可见侧面）。
    auto sampleScene = [&](float p[3], float n[3]) {
        const float u = rng.f();
        if (u < 0.5f) {                     // 地面
            p[0] = rng.range(-6, 6); p[1] = -1.0f; p[2] = rng.range(-6, 6);
            n[0] = 0; n[1] = 1; n[2] = 0;
        } else if (u < 0.75f) {             // 箱 A（x:-3..-1, y:-1..0.5, z:-2..2）
            const float v = rng.f();
            if (v < 0.5f) {
                p[0] = rng.range(-3, -1); p[1] = 0.5f; p[2] = rng.range(-2, 2);
                n[0] = 0; n[1] = 1; n[2] = 0;
            } else if (v < 0.75f) {
                p[0] = -3.0f; p[1] = rng.range(-1, 0.5f); p[2] = rng.range(-2, 2);
                n[0] = -1; n[1] = 0; n[2] = 0;
            } else {
                p[0] = rng.range(-3, -1); p[1] = rng.range(-1, 0.5f); p[2] = 2.0f;
                n[0] = 0; n[1] = 0; n[2] = 1;
            }
        } else {                            // 箱 B（x:1..4, y:-1..1.5, z:-3..0）
            const float v = rng.f();
            if (v < 0.5f) {
                p[0] = rng.range(1, 4); p[1] = 1.5f; p[2] = rng.range(-3, 0);
                n[0] = 0; n[1] = 1; n[2] = 0;
            } else if (v < 0.75f) {
                p[0] = 4.0f; p[1] = rng.range(-1, 1.5f); p[2] = rng.range(-3, 0);
                n[0] = 1; n[1] = 0; n[2] = 0;
            } else {
                p[0] = rng.range(1, 4); p[1] = rng.range(-1, 1.5f); p[2] = -3.0f;
                n[0] = 0; n[1] = 0; n[2] = -1;
            }
        }
    };

    std::uint32_t peakProbes = 0;
    for (int f = 0; f < frames; ++f) {
        sim.decay();
        for (int i = 0; i < 4096; ++i) {
            float p[3], n[3];
            sampleScene(p, n);
            sim.touch(p, n);
            peakProbes = std::max(peakProbes, sim.lastProbes);
        }
    }

    // 俯视热图：直接遍历表内 active cells 做 XY 投影（这才是「cell 分布」本义；
    // 逐像素 find 会因查询高度/法线与 cell 不符而漏——cell key 是 pos+normal 三维）。
    std::vector<unsigned char> rgb(static_cast<size_t>(size) * size * 3, 0);
    const int half = size / 2;
    const float w2p = static_cast<float>(half) / 6.0f;   // 世界米 → 像素
    for (const WorldCacheSim::Cell& c : sim.table) {
        if (c.life == 0u) continue;
        const int px = static_cast<int>(c.pos[0] * w2p) + half;
        const int py = static_cast<int>(c.pos[2] * w2p) + half;
        if (px < 0 || px >= size || py < 0 || py >= size) continue;
        const std::uint32_t h = hashes::pcg(c.checksum);
        const float hue = static_cast<float>(h & 0xFFu) / 255.0f * 6.0f;
        const float seg = std::floor(hue);
        const float t = hue - seg;
        float r, g, b;
        switch (static_cast<int>(seg) % 6) {
            case 0: r = 1; g = t; b = 0; break;
            case 1: r = 1 - t; g = 1; b = 0; break;
            case 2: r = 0; g = 1; b = t; break;
            case 3: r = 0; g = 1 - t; b = 1; break;
            case 4: r = t; g = 0; b = 1; break;
            default: r = 1; g = 0; b = 1 - t; break;
        }
        const float v = 0.35f + 0.65f *
            (static_cast<float>(c.life) / static_cast<float>(sim.lifetime));
        // cell 是 0.5m 方块 → 按 cell 尺寸填充矩形（1px 点会看不出网格分布）。
        const int cw = std::max(1, static_cast<int>(0.5f * w2p));
        for (int dy = 0; dy < cw; ++dy) {
            const int yy = py + dy;
            if (yy < 0 || yy >= size) continue;
            for (int dx = 0; dx < cw; ++dx) {
                const int xx = px + dx;
                if (xx < 0 || xx >= size) continue;
                unsigned char* o = &rgb[(static_cast<size_t>(yy) * size + xx) * 3];
                o[0] = static_cast<unsigned char>(r * v * 255.0f);
                o[1] = static_cast<unsigned char>(g * v * 255.0f);
                o[2] = static_cast<unsigned char>(b * v * 255.0f);
            }
        }
    }

    std::printf("frames       : %d\n", frames);
    std::printf("capacity     : %u\n", sim.capacity);
    std::printf("cells        : %u / %u\n", sim.activeCells(), sim.capacity);
    std::printf("peak probes  : %u\n", peakProbes);
    bool pngOk = false;
    std::string err;
    if (pngPath) {
        std::vector<unsigned char> flipped(static_cast<size_t>(size) * size * 3);
        // 俯视惯例：世界 +z 朝画面下方（图像 y 增大向下）→ 翻转 y 行序。
        for (int y = 0; y < size; ++y)
            std::memcpy(&flipped[static_cast<size_t>(y) * size * 3],
                        &rgb[static_cast<size_t>(size - 1 - y) * size * 3],
                        static_cast<size_t>(size) * 3);
        pngOk = png::write(pngPath, size, size, flipped, err);
        std::printf("png          : %s\n", pngOk ? pngPath : err.c_str());
    }
    if (pngPath && !pngOk) return 1;
    if (sim.activeCells() == 0) { std::printf("no cells populated\n"); return 1; }
    return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "selftest") {
        if (argc >= 3 && std::strcmp(argv[2], "cachesim") == 0) return selftestCachesim();
        if (argc >= 3) { std::fprintf(stderr, "unknown selftest suite: %s\n", argv[2]); return 2; }
        return selftestCachesim();
    }
    if (cmd == "cachesim") return cmdCachesim(argc - 2, argv + 2);
    if (cmd == "help" || cmd == "--help") { printUsage(); return 0; }
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    printUsage();
    return 2;
}
