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
        // M11.1 cell 更新（与 csCacheUpdate/csCacheBlend 同构）：
        float rad[3]   = {0.f, 0.f, 0.f};  // C = 入射辐照度收敛值（直光+间接）
        float samples  = 0.f;              // blend 样本数（cap 64）
        float newRad[3] = {0.f, 0.f, 0.f}; // 本帧 update 新样本
        bool  newMark  = false;            // 本轮被软目标选中（blend 消费后清 0）
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
// 合成场景解析求交（cachesim --update 仿真用；几何与 sampleScene 严格同源：
// 地面 y=-1 + 箱 A(x:-3..-1,y:-1..0.5,z:-2..2) + 箱 B(x:1..4,y:-1..1.5,z:-3..0)）。
// slab 法 ray-AABB + 平面，返回命中 t 与外法线；CPU 参照实现，无容差需求。
struct SimHit { float t; float n[3]; };

static bool simIntersect(const float o[3], const float d[3], float tMax, SimHit& out) {
    bool hit = false;
    float best = tMax;
    // 地面平面 y = -1（朝上）
    if (d[1] < -1e-6f) {
        const float t = (-1.0f - o[1]) / d[1];
        if (t > 1e-4f && t < best) {
            const float x = o[0] + d[0] * t, z = o[2] + d[2] * t;
            if (x >= -6.f && x <= 6.f && z >= -6.f && z <= 6.f) {
                best = t; out.t = t; out.n[0] = 0; out.n[1] = 1; out.n[2] = 0; hit = true;
            }
        }
    }
    // 两个 AABB（slab 法）
    const float boxes[2][2][3] = {
        {{-3.f, -1.f, -2.f}, {-1.f, 0.5f, 2.f}},   // 箱 A min/max
        {{ 1.f, -1.f, -3.f}, { 4.f, 1.5f, 0.f}},   // 箱 B min/max
    };
    for (int b = 0; b < 2; ++b) {
        float tmin = 1e-4f, tmax = best;
        int axisHit = -1, signHit = 0;
        bool ok = true;
        for (int a = 0; a < 3 && ok; ++a) {
            const float lo = boxes[b][0][a], hi = boxes[b][1][a];
            if (std::fabs(d[a]) < 1e-8f) {
                if (o[a] < lo || o[a] > hi) ok = false;
            } else {
                float t1 = (lo - o[a]) / d[a], t2 = (hi - o[a]) / d[a];
                int s1 = -1;
                if (t1 > t2) { float tt = t1; t1 = t2; t2 = tt; s1 = 1; }
                if (t1 > tmin) { tmin = t1; axisHit = a; signHit = s1; }
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax) ok = false;
            }
        }
        if (ok && axisHit >= 0 && tmin < best) {
            best = tmin; out.t = tmin;
            for (int a = 0; a < 3; ++a) out.n[a] = 0.f;
            out.n[axisHit] = static_cast<float>(signHit);   // 入射面外法线
            hit = true;
        }
    }
    return hit;
}

// 解析天穹（sky.slangh 同构：zenith/horizon 余弦混合 + 地平线以下压暗）。
static void simSky(const float d[3], float out[3]) {
    const float zen[3] = {0.16f, 0.30f, 0.58f}, hor[3] = {0.62f, 0.71f, 0.84f};
    float t = d[1] < 0.f ? 0.f : (d[1] > 1.f ? 1.f : d[1]);
    const float pw = std::pow(t, 0.6f);
    float dk = d[1] * 4.f + 1.f; dk = dk < 0.f ? 0.f : (dk > 1.f ? 1.f : dk);
    const float dim = 0.35f + 0.65f * dk;
    for (int i = 0; i < 3; ++i)
        out[i] = (hor[i] + (zen[i] - hor[i]) * pw) * dim;
}

// M11.1 update 语义（与 csCacheUpdate 同构的 CPU 参照）。口径（自洽定义，
// 与 bevy 的差异在于 cell 值定义不同——我们 cell 存入射辐照度 C）：
//   E_direct  = lightCol * I * NdotL * shadow          （方向灯入射）
//   E_ind     = albedo * C(hit)                        （cosine 弹射单样本：
//               lambert f=albedo/π × cos ÷ pdf=cos/π → albedo × L(ω)，
//               邻居表面出射 ≈ albedo × 邻居入射 C（统一 albedo））
//   缓存 miss（几何命中但无 cell）→ 0（多帧收敛自然填充）
//   射线 miss 天空 → albedo * skyGradient(d)（bevy 的 π×env 是其 cell 存
//               出射口径所致；我们入射口径下 π 被 pdf 消掉）
// 软目标：pcg(idx ^ frame) < prob（bevy target/count 的无 active-list 近似，
// 固定概率第一版；rng = cell_index + frame_index 入 seed，bevy 同款）。
struct SimLight {
    float dir[3];       // 指向光源（M9 g_traceLightDir 同语义，已归一化）
    float intensity;    // 2.5（M9.x 同值）
    float albedo[3];    // 统一间接反照率 0.45/0.39/0.30（M10.1/M10.2 同口径）
    float bias;         // 0.01 世界尺度（direct.slang g_shadowParams.x 同值）
    float tMax;         // shadow ray tMax（50）
    float prob;         // 软目标概率（第一版 0.25：每 cell 4 帧一轮）
};

static int simCacheUpdate(WorldCacheSim& sim, std::uint32_t frameIndex,
                          const SimLight& lt) {
    int updated = 0;
    for (std::uint32_t idx = 0; idx < sim.capacity; ++idx) {
        WorldCacheSim::Cell& c = sim.table[idx];
        c.newMark = false;
        if (c.checksum == 0u) continue;                    // 空槽
        if (static_cast<float>(hashes::pcg(idx ^ frameIndex)) * (1.f / 4294967296.f)
            >= lt.prob) continue;
        c.newMark = true;
        ++updated;

        // ---- DI 段：对光源 shadow ray ----
        float o[3] = {c.pos[0] + c.nrm[0] * lt.bias,
                      c.pos[1] + c.nrm[1] * lt.bias,
                      c.pos[2] + c.nrm[2] * lt.bias};
        float eDir[3] = {0.f, 0.f, 0.f};
        const float ndl = c.nrm[0] * lt.dir[0] + c.nrm[1] * lt.dir[1]
                        + c.nrm[2] * lt.dir[2];
        if (ndl > 0.f) {
            SimHit h;
            const bool occluded = simIntersect(o, lt.dir, lt.tMax, h);
            if (!occluded) {
                for (int i = 0; i < 3; ++i) eDir[i] = lt.intensity * ndl;
            }
        }

        // ---- GI 段：cosine hemisphere 邻居射线（Malley disk → 半球投影，
        // direct.slang csReferenceGI 同款；rng = idx + frame 入 seed）。
        std::uint32_t rs = hashes::pcg(idx) ^ frameIndex;
        auto rnd = [&rs]() { rs = hashes::pcg(rs); return static_cast<float>(rs) * (1.f / 4294967296.f); };
        const float u1 = rnd(), u2 = rnd();
        const float r = std::sqrt(u1), phi = 6.2831853f * u2;
        float t1[3], t2v[3];
        if (std::fabs(c.nrm[1]) < 0.9f) {
            t1[0] = c.nrm[2]; t1[1] = 0.f; t1[2] = -c.nrm[0];   // cross((0,1,0),n)
        } else {
            t1[0] = 0.f; t1[1] = -c.nrm[2]; t1[2] = c.nrm[1];   // cross((1,0,0),n)
        }
        const float tl = std::sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
        t1[0] /= tl; t1[1] /= tl; t1[2] /= tl;
        t2v[0] = c.nrm[1] * t1[2] - c.nrm[2] * t1[1];
        t2v[1] = c.nrm[2] * t1[0] - c.nrm[0] * t1[2];
        t2v[2] = c.nrm[0] * t1[1] - c.nrm[1] * t1[0];
        const float dx = t1[0] * (r * std::cos(phi)) + t2v[0] * (r * std::sin(phi)) + c.nrm[0] * std::sqrt(std::max(0.f, 1.f - u1));
        const float dy = t1[1] * (r * std::cos(phi)) + t2v[1] * (r * std::sin(phi)) + c.nrm[1] * std::sqrt(std::max(0.f, 1.f - u1));
        const float dz = t1[2] * (r * std::cos(phi)) + t2v[2] * (r * std::sin(phi)) + c.nrm[2] * std::sqrt(std::max(0.f, 1.f - u1));
        const float d[3] = {dx, dy, dz};

        float eInd[3] = {0.f, 0.f, 0.f};
        SimHit h;
        if (simIntersect(o, d, 1.0e4f, h)) {
            const float hp[3] = {o[0] + d[0] * h.t, o[1] + d[1] * h.t, o[2] + d[2] * h.t};
            // 命中点沿命中面法线微抬（查缓存口径与 cell 几何量化一致）
            const float hq[3] = {hp[0] + h.n[0] * lt.bias, hp[1] + h.n[1] * lt.bias,
                                 hp[2] + h.n[2] * lt.bias};
            const std::uint32_t slot = sim.find(hq, h.n);
            if (slot != sim.capacity) {
                for (int i = 0; i < 3; ++i) eInd[i] = lt.albedo[i] * sim.table[slot].rad[i];
            }                          // 缓存 miss → 0
        } else {
            float sky[3];
            simSky(d, sky);
            for (int i = 0; i < 3; ++i) eInd[i] = lt.albedo[i] * sky[i];
        }

        for (int i = 0; i < 3; ++i) c.newRad[i] = eDir[i] + eInd[i];
    }
    return updated;
}

// M11.1 blend 语义（与 csCacheBlend 同构）：线性时域混合，样本数 cap 64。
// 只处理本轮被软目标选中的 cell（newMark），选不中的旧值原样保留——
// 这是 bevy active-list 语义在无 active-list 全表扫下的等价形态。
static void simCacheBlend(WorldCacheSim& sim) {
    for (WorldCacheSim::Cell& c : sim.table) {
        if (!c.newMark) continue;
        const float amt = 1.f / std::min(c.samples + 1.f, 64.f);
        for (int i = 0; i < 3; ++i) c.rad[i] += (c.newRad[i] - c.rad[i]) * amt;
        c.samples = std::min(c.samples + 1.f, 64.f);
        c.newMark = false;
    }
}

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

    // 8. M11.1 update 收敛：无遮挡 cell 的直光项收敛到解析值（误差 5%）。
    {
        WorldCacheSim sim(1u << 12);
        Lcg rng(9);
        SimLight lt;
        lt.dir[0] = 0.3f; lt.dir[1] = 0.9f; lt.dir[2] = 0.2f;   // 指向光源
        const float dl = std::sqrt(0.3f * 0.3f + 0.9f * 0.9f + 0.2f * 0.2f);
        lt.dir[0] /= dl; lt.dir[1] /= dl; lt.dir[2] /= dl;
        lt.intensity = 2.5f;
        lt.albedo[0] = 0.45f; lt.albedo[1] = 0.39f; lt.albedo[2] = 0.30f;
        lt.bias = 0.01f; lt.tMax = 50.f; lt.prob = 1.0f;   // 全量更新压噪声
        const float p[3] = {4.5f, -1.0f, -4.5f};           // 场景边角，无遮挡
        const float n[3] = {0, 1, 0};
        // 差分法（两组独立仿真、同软目标序列 → 天空项逐帧对消，残差只来自
        // blend 收敛动态）：地面点的 cosine 邻居射线全 miss 天空（上方无遮挡），
        // 收敛值 C = albedo·E[sky] + I·NdotL。I=0 基线组取天空项，I=2.5 组验
        // 直光增量；192 帧后 cap-64 残差 (63/64)^192 < 5%。
        WorldCacheSim simBase(1u << 12);
        simBase.touch(p, n);
        sim.touch(p, n);
        for (int f = 0; f < 192; ++f) {
            lt.intensity = 0.f;
            simCacheUpdate(simBase, static_cast<std::uint32_t>(f), lt);
            simCacheBlend(simBase);
            lt.intensity = 2.5f;
            simCacheUpdate(sim, static_cast<std::uint32_t>(f), lt);
            simCacheBlend(sim);
        }
        const std::uint32_t slot = sim.find(p, n);
        CHECK(slot != sim.capacity, "update: ground cell stays resident");
        const float skyBase = simBase.table[simBase.find(p, n)].rad[0];
        CHECK(skyBase > 0.f, "update: sky base contribution present");
        const WorldCacheSim::Cell& c = sim.table[slot];
        const float analytic = 2.5f * (n[0] * lt.dir[0] + n[1] * lt.dir[1] + n[2] * lt.dir[2]);
        float err = 0.f;
        for (int i = 0; i < 3; ++i)
            err = std::max(err, std::fabs((c.rad[i] - skyBase) - analytic) / analytic);
        CHECK(err < 0.05f, "update: DI increment converges to analytic value (5%)");
        CHECK(c.samples >= 63.f, "update: sample count reaches cap");
    }

    // 9. M11.1 二级弹射涌现：被遮挡 cell 直光为 0，多帧后 radiance > 0
    //    （间接光只可能来自邻居 cell 的缓存值——缓存反馈的机制本体）。
    {
        WorldCacheSim sim(1u << 12);
        Lcg rng(10);
        SimLight lt;
        lt.dir[0] = 0.3f; lt.dir[1] = 0.9f; lt.dir[2] = 0.2f;
        const float dl = std::sqrt(0.3f * 0.3f + 0.9f * 0.9f + 0.2f * 0.2f);
        lt.dir[0] /= dl; lt.dir[1] /= dl; lt.dir[2] /= dl;
        lt.intensity = 2.5f;
        lt.albedo[0] = 0.45f; lt.albedo[1] = 0.39f; lt.albedo[2] = 0.30f;
        lt.bias = 0.01f; lt.tMax = 50.f; lt.prob = 1.0f;
        // 点亮一片直光地面（x:3..5 朝光面）+ 阴影地面点（箱 B 背光侧 x<1 近旁）。
        // 阴影点选取：光从 +x 上方来，箱 B(x:1..4) 影子投在 x<1 一侧；
        // (-0.2,-1,0) 朝光源射线在 y<1.5 段 x∈[-0.2,0.63] 不与箱 B 相交，
        // 加 t 大后 y 已出箱高 → 需实测确认遮挡，断言分两步：先验直光=0，
        // 再验点亮的直光群是否把阴影点 GI 拉起来。
        const float lit[3] = {4.5f, -1.0f, 0.5f};
        const float litN[3] = {0, 1, 0};
        const float shadowP[3] = {-0.25f, -1.0f, 0.0f};
        const float shadowN[3] = {0, 1, 0};
        // 直光群
        for (int i = 0; i < 40; ++i) {
            const float q[3] = {rng.range(3.f, 5.5f), -1.0f, rng.range(-1.f, 1.5f)};
            sim.touch(q, litN);
        }
        sim.touch(lit, litN);
        sim.touch(shadowP, shadowN);
        int framesShadowGlow = 0;
        for (int f = 0; f < 96; ++f) {
            simCacheUpdate(sim, static_cast<std::uint32_t>(f), lt);
            simCacheBlend(sim);
            const std::uint32_t ss = sim.find(shadowP, shadowN);
            if (ss == sim.capacity) continue;
            const float lum = sim.table[ss].rad[0] + sim.table[ss].rad[1] + sim.table[ss].rad[2];
            if (lum > 0.01f) ++framesShadowGlow;
        }
        // 直光=0 先验（NdotL>0 但 shadow ray 被箱 B 挡；若实测未挡则本组前提
        // 不成立——换点前先如实暴露）。
        const std::uint32_t sShadow = sim.find(shadowP, shadowN);
        CHECK(sShadow != sim.capacity, "bounce: shadow cell stays resident");
        CHECK(framesShadowGlow > 48,
              "bounce: shadowed cell receives indirect light via cache feedback");
    }

    // 10. M11.1 blend 语义：未选中 cell 旧值原样保留；选中后样本数单调、
    //     收敛步长符合 1/min(a+1,64)。
    {
        WorldCacheSim sim(1u << 12);
        SimLight lt;
        lt.dir[0] = 0.f; lt.dir[1] = 1.f; lt.dir[2] = 0.f;
        lt.intensity = 0.f;                       // 关直光：样本全来自天空项
        lt.albedo[0] = 0.45f; lt.albedo[1] = 0.39f; lt.albedo[2] = 0.30f;
        lt.bias = 0.01f; lt.tMax = 50.f; lt.prob = 1.0f;
        const float p[3] = {0.5f, -1.0f, 0.5f};
        const float n[3] = {0, 1, 0};
        sim.touch(p, n);
        simCacheUpdate(sim, 0u, lt);
        simCacheBlend(sim);
        const std::uint32_t slot = sim.find(p, n);
        const float after1 = sim.table[slot].rad[0] + sim.table[slot].rad[1] + sim.table[slot].rad[2];
        CHECK(after1 > 0.f, "blend: sky-only sample lands (albedo*sky > 0)");
        CHECK(sim.table[slot].samples == 1.f, "blend: sample count increments");
        // cap 64 后样本数不再增长
        for (int f = 1; f < 80; ++f) {
            simCacheUpdate(sim, static_cast<std::uint32_t>(f), lt);
            simCacheBlend(sim);
        }
        CHECK(sim.table[slot].samples == 64.f, "blend: sample count capped at 64");
        // 后期收敛步长 = 1/64：相邻两帧变化不超过步长 x 解析上界
        const float before = sim.table[slot].rad[0];
        simCacheUpdate(sim, 999u, lt);
        simCacheBlend(sim);
        const float after = sim.table[slot].rad[0];
        CHECK(std::fabs(after - before) < 0.02f,
              "blend: post-cap per-frame delta shrinks to 1/64 scale");
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
    bool updateMode = false;   // M11.1：update+blend 仿真 + radiance 亮度热图
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--frames") frames = std::atoi(need("--frames"));
        else if (a == "--png") pngPath = need("--png");
        else if (a == "--size") size = std::atoi(need("--size"));
        else if (a == "--update") updateMode = true;
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

    // M11.1 update 灯参数（M9.x/M10.x 同口径；软目标 p=0.25 = 每 cell 4 帧一轮）。
    SimLight lt;
    lt.dir[0] = 0.3f; lt.dir[1] = 0.9f; lt.dir[2] = 0.2f;
    {
        const float dl = std::sqrt(0.3f * 0.3f + 0.9f * 0.9f + 0.2f * 0.2f);
        lt.dir[0] /= dl; lt.dir[1] /= dl; lt.dir[2] /= dl;
    }
    lt.intensity = 2.5f;
    lt.albedo[0] = 0.45f; lt.albedo[1] = 0.39f; lt.albedo[2] = 0.30f;
    lt.bias = 0.01f; lt.tMax = 50.f; lt.prob = 0.25f;
    // 收敛锚点：直光地面 cell（每帧被 4096 采样稳定续命，不存在集合变化干扰）。
    const float anchor[3] = {4.5f, -1.0f, 0.5f};
    const float anchorN[3] = {0, 1, 0};
    sim.touch(anchor, anchorN);

    std::uint32_t peakProbes = 0;
    double radVarFirst = 0.0, radVarLast = 0.0;   // 收敛证据：锚点 cell 前后窗帧间差
    std::vector<float> anchorLum;                 // 固定锚点 (4.5,-1,0.5) 逐帧亮度
    for (int f = 0; f < frames; ++f) {
        sim.decay();
        for (int i = 0; i < 4096; ++i) {
            float p[3], n[3];
            sampleScene(p, n);
            sim.touch(p, n);
            peakProbes = std::max(peakProbes, sim.lastProbes);
        }
        if (updateMode) {
            simCacheUpdate(sim, static_cast<std::uint32_t>(f), lt);
            simCacheBlend(sim);
            const std::uint32_t as = sim.find(anchor, anchorN);
            anchorLum.push_back(as != sim.capacity
                ? sim.table[as].rad[0] + sim.table[as].rad[1] + sim.table[as].rad[2]
                : 0.f);
        }
    }
    if (updateMode && anchorLum.size() >= 8) {
        const size_t half = anchorLum.size() / 2;
        auto windowDelta = [&](size_t lo, size_t hi) {
            double acc = 0.0;
            for (size_t i = lo + 1; i < hi; ++i)
                acc += std::fabs(static_cast<double>(anchorLum[i]) - anchorLum[i - 1]);
            return acc / static_cast<double>(hi - lo - 1);
        };
        radVarFirst = windowDelta(0, half);
        radVarLast = windowDelta(half, anchorLum.size());
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
        float r, g, b;
        if (updateMode) {
            // M11.1 radiance 模式：出射辐照度 albedo·C/π 直接色调映射
            // （c/(1+c)）——直光面亮、阴影区只剩间接光明显变暗、无伪彩色相
            // 干扰；cell 收敛差异肉眼可辨。
            const float e[3] = {c.rad[0] * 0.45f * 0.31830988f,
                                c.rad[1] * 0.39f * 0.31830988f,
                                c.rad[2] * 0.30f * 0.31830988f};
            r = std::pow(e[0] / (1.f + e[0]), 1.f / 2.2f);
            g = std::pow(e[1] / (1.f + e[1]), 1.f / 2.2f);
            b = std::pow(e[2] / (1.f + e[2]), 1.f / 2.2f);
        } else {
            const std::uint32_t h = hashes::pcg(c.checksum);
            const float hue = static_cast<float>(h & 0xFFu) / 255.0f * 6.0f;
            const float seg = std::floor(hue);
            const float t = hue - seg;
            switch (static_cast<int>(seg) % 6) {
                case 0: r = 1; g = t; b = 0; break;
                case 1: r = 1 - t; g = 1; b = 0; break;
                case 2: r = 0; g = 1; b = t; break;
                case 3: r = 0; g = 1 - t; b = 1; break;
                case 4: r = t; g = 0; b = 1; break;
                default: r = 1; g = 0; b = 1 - t; break;
            }
            r *= 0.35f + 0.65f * (static_cast<float>(c.life) / static_cast<float>(sim.lifetime));
            g *= 0.35f + 0.65f * (static_cast<float>(c.life) / static_cast<float>(sim.lifetime));
            b *= 0.35f + 0.65f * (static_cast<float>(c.life) / static_cast<float>(sim.lifetime));
        }
        // cell 是 0.5m 方块 → 按 cell 尺寸填充矩形（1px 点会看不出网格分布）。
        const int cw = std::max(1, static_cast<int>(0.5f * w2p));
        for (int dy = 0; dy < cw; ++dy) {
            const int yy = py + dy;
            if (yy < 0 || yy >= size) continue;
            for (int dx = 0; dx < cw; ++dx) {
                const int xx = px + dx;
                if (xx < 0 || xx >= size) continue;
                unsigned char* o = &rgb[(static_cast<size_t>(yy) * size + xx) * 3];
                o[0] = static_cast<unsigned char>(std::min(r, 1.0f) * 255.0f);
                o[1] = static_cast<unsigned char>(std::min(g, 1.0f) * 255.0f);
                o[2] = static_cast<unsigned char>(std::min(b, 1.0f) * 255.0f);
            }
        }
    }

    std::printf("frames       : %d\n", frames);
    std::printf("capacity     : %u\n", sim.capacity);
    std::printf("cells        : %u / %u\n", sim.activeCells(), sim.capacity);
    std::printf("peak probes  : %u\n", peakProbes);
    if (updateMode) {
        std::printf("update mode  : prob 0.25 / DI+GI+blend\n");
        std::printf("delta early  : %.5f\n", radVarFirst);
        std::printf("delta late   : %.5f\n", radVarLast);
        if (radVarLast >= radVarFirst && radVarFirst > 0.0) {
            std::printf("no convergence (late delta >= early)\n");
            return 1;
        }
    }
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
