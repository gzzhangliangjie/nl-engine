// city-tools —— nl-engine 大世界生成工具（M-W0 citygen，A 类并行轨道 feat/worldgen）
// M-W0.0：CLI 骨架 + 自研确定性 RNG（pcg32）+ 规则网格 block 布局 + 空白城俯视 PNG
//         （纯 CPU 光栅化）+ CityStats 计数锚骨架 + seed 对称确定性 selftest。
//
// 依据 docs/worldgen/W0-PLAN.md（2026-11-12 五问全拍板）：
//   Q-W3 = W3-A 全自研最小算法（确定性 RNG，零新依赖，C++17，与 bevy_city 同构）。
//   A 类工具版交代：不碰 kStage / 不进 renderer / 不改 .nmesh / 不进渲染回归总闸。
//   退出码契约（对齐 phys-tools / 展示台 expectExit）：0 = PASS / 1 = 断言失败 / 2 = 用法错误。
//
// 布局口径（bevy examples/large_scenes/bevy_city 同构，2026-11-12 三处实读留档）：
//   block 尺寸 5.5m x 4.0m（与 roadnet.json 契约 "blocks":{"x":5.5,"z":4.0} 同源）；
//   size = 每边 block 数，网格按 size 居中（lo = -(size/2)）；
//   路网 = (size+1) 条横线 + (size+1) 条竖线（全跨度，1.0m 宽）——比 bevy 的
//   「逐 cell 线段 + crossroad 补丁」拓扑更规整（无开边、无死路），M-W0.1 roadnet
//   直接映射 (size+1)^2 交点；block 内区 = 4.5m x 3.0m（cell 减路带）。
//
// RNG 纪律（M-W0.0 起，后续里程碑只许追加消费、不许重排既有消费序）：
//   单一 Pcg32 流，seed() 后按 cell 行主序（gx 外层 / gz 内层）每 cell 消费一次
//   below(16) 作地面灰度档位（0..15）——这是 M-W0.0 使「不同 seed 必不同」成立
//   的最小种子敏感元；M-W0.3 OpenSimplex 密度分区接管视觉分区后保留此流前缀序。
//   M-W0.1 roadnet 车道语义、M-W0.2 楼体分档的消费一律排在此流之后（追加不改前缀）。
//
// 输出 ASCII only；PNG 编码走 vendored stb_image_write.h（public domain，
// renderer/src/png_writer.cpp 同源先例），stb zlib 压缩确定性 → 同参数逐字节一致。

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace citygen {

// ---------------------------------------------------------------------------
// Pcg32 —— O'Neill pcg32（最小自研确定性 RNG，W3-A 拍板：零依赖）。
// 乘子 / 初始化序取自 pcg32 规范实现（public domain 参考口径，抄算法不抄文件）。
// 纪律：只实现 next / nextFloat / below 三个消费面；流前缀序是跨里程碑契约。
// ---------------------------------------------------------------------------
struct Pcg32 {
    std::uint64_t state = 0;
    std::uint64_t inc = 0;

    static constexpr std::uint64_t kMultiplier = 6364136223846793005ULL;
    static constexpr std::uint64_t kDefaultSeq = 0x14057B7EF767814FULL; // pcg32 默认流

    void seed(std::uint64_t initstate) {
        state = 0;
        inc = ((kDefaultSeq << 1) | 1ULL);
        next();
        state += initstate;
        next();
    }

    std::uint32_t next() {
        const std::uint64_t old = state;
        state = old * kMultiplier + (inc | 1ULL);
        // 与规范一致：64 位中间值截断到 32 位再旋转。
        const std::uint32_t xorshifted =
            static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // [0,1) 定点浮点，24 位精度；跨平台位一致（纯整数缩放，无 libm）。
    float nextFloat() {
        return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f);
    }

    // [0, bound)，bound >= 1；拒绝采样去模偏差（纪律：selftest 锁确定性）。
    std::uint32_t below(std::uint32_t bound) {
        const std::uint32_t threshold =
            static_cast<std::uint32_t>(-static_cast<std::int32_t>(bound)) % bound;
        for (;;) {
            const std::uint32_t r = next();
            if (r >= threshold) return r % bound;
        }
    }
};

// ---------------------------------------------------------------------------
// 城市参数 / 统计 / 布局
// ---------------------------------------------------------------------------
constexpr float kBlockX = 5.5f;     // roadnet.json 契约 blocks.x
constexpr float kBlockZ = 4.0f;     // roadnet.json 契约 blocks.z
constexpr float kRoadWidth = 1.0f;  // M-W0.0 路带总宽（俯视图示意口径）
constexpr std::uint32_t kMinSize = 1;
constexpr std::uint32_t kMaxSize = 64; // 64*5.5*32px = 11264px 画布上限内的机动余量

struct CityParams {
    std::uint64_t seed = 12345;
    std::uint32_t size = 6;
};

// CityStats —— bevy_city generate_city.rs CityStats 同构计数锚骨架。
// M-W0.0 只有布局计数；buildings/trees/cars/fences/paths 是 M-W0.2/0.3 的挂零锚。
struct CityStats {
    std::uint32_t blocks = 0;
    std::uint32_t groundTiles = 0;
    std::uint32_t roadSegments = 0;
    std::uint32_t crossroads = 0;
    std::uint32_t buildings = 0;
    std::uint32_t trees = 0;
    std::uint32_t cars = 0;
    std::uint32_t fences = 0;
    std::uint32_t paths = 0;
};

struct CityCell {
    std::int32_t gx = 0;   // 网格索引
    std::int32_t gz = 0;
    std::uint32_t shade = 0; // 地面灰度档位 0..15（M-W0.0 种子敏感元，见文件头 RNG 纪律）
};

struct CityLayout {
    CityParams params;
    std::int32_t lo = 0; // 网格低索引（-size/2）
    std::vector<CityCell> cells;
    CityStats stats;
    float minX = 0.f, maxX = 0.f, minZ = 0.f, maxZ = 0.f; // 城市外延（米）
};

// 生成确定性布局：RNG 消费序 = cell 行主序（文件头纪律），布局本身无其他随机源。
CityLayout generateCity(const CityParams& params) {
    CityLayout lay;
    lay.params = params;
    lay.lo = -static_cast<std::int32_t>(params.size / 2u);
    const std::int32_t hi = lay.lo + static_cast<std::int32_t>(params.size);

    Pcg32 rng;
    rng.seed(params.seed);
    for (std::int32_t gx = lay.lo; gx < hi; ++gx) {
        for (std::int32_t gz = lay.lo; gz < hi; ++gz) {
            CityCell cell;
            cell.gx = gx;
            cell.gz = gz;
            cell.shade = rng.below(16u); // 0..15 档位，单次消费，行主序
            lay.cells.push_back(cell);
        }
    }

    lay.stats.blocks = params.size * params.size;
    lay.stats.groundTiles = lay.stats.blocks;
    lay.stats.roadSegments = 2u * lay.stats.blocks; // 每 cell 一横一竖
    lay.stats.crossroads = (params.size + 1u) * (params.size + 1u); // 网格线交点
    // buildings/trees/cars/fences/paths 保持 0：M-W0.2 / M-W0.3 消费。

    lay.minX = static_cast<float>(lay.lo) * kBlockX;
    lay.maxX = static_cast<float>(hi) * kBlockX;
    lay.minZ = static_cast<float>(lay.lo) * kBlockZ;
    lay.maxZ = static_cast<float>(hi) * kBlockZ;
    return lay;
}

// ---------------------------------------------------------------------------
// 规范序列化 + FNV-1a 64（确定性 selftest 的逐字节比对基底）
// ---------------------------------------------------------------------------
void appendU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}
void appendU64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void appendI32(std::vector<std::uint8_t>& b, std::int32_t v) {
    appendU32(b, static_cast<std::uint32_t>(v));
}

std::vector<std::uint8_t> serializeLayout(const CityLayout& lay) {
    std::vector<std::uint8_t> b;
    const char magic[6] = {'N', 'L', 'C', 'T', '0', '1'};
    b.insert(b.end(), magic, magic + 6);
    appendU64(b, lay.params.seed);
    appendU32(b, lay.params.size);
    appendI32(b, lay.lo);
    appendU32(b, lay.stats.blocks);
    appendU32(b, lay.stats.groundTiles);
    appendU32(b, lay.stats.roadSegments);
    appendU32(b, lay.stats.crossroads);
    appendU32(b, lay.stats.buildings);
    appendU32(b, lay.stats.trees);
    appendU32(b, lay.stats.cars);
    appendU32(b, lay.stats.fences);
    appendU32(b, lay.stats.paths);
    for (const CityCell& c : lay.cells) {
        appendI32(b, c.gx);
        appendI32(b, c.gz);
        b.push_back(static_cast<std::uint8_t>(c.shade));
    }
    return b;
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& b) {
    std::uint64_t h = 14695981039346656037ULL;
    for (std::uint8_t v : b) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    return h;
}

// ---------------------------------------------------------------------------
// 纯 CPU 俯视光栅化（米 → 像素，固定 32 px/m，四边 1.5m 留白）
// ---------------------------------------------------------------------------
struct RasterConfig {
    float pxPerMeter = 32.0f;
    float marginM = 1.5f;
};

std::uint8_t* pixelAt(std::vector<std::uint8_t>& img, int w, int x, int y) {
    return img.data() + (static_cast<std::size_t>(y) * w + x) * 3;
}

// 实心矩形（像素坐标，边界裁剪到画布内）。
void fillRect(std::vector<std::uint8_t>& img, int w, int h,
              int x0, int y0, int x1, int y1,
              std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            std::uint8_t* p = pixelAt(img, w, x, y);
            p[0] = r; p[1] = g; p[2] = b;
        }
    }
}

// PNG 编码到内存（stb 回调），stb zlib 确定性 → 同输入逐字节一致。
void stbCallback(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(ctx);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

bool encodePng(const std::vector<std::uint8_t>& rgb, int w, int h,
               std::vector<std::uint8_t>& outPng) {
    outPng.clear();
    const int ok = stbi_write_png_to_func(stbCallback, &outPng, w, h, 3,
                                          rgb.data(), w * 3);
    return ok != 0 && !outPng.empty();
}

// 俯视图：底色 → 逐 cell block 内区灰度 → 全跨度路网线（横竖各 size+1 条）。
bool renderTopView(const CityLayout& lay, const RasterConfig& cfg,
                   std::vector<std::uint8_t>& outPng, int& outW, int& outH) {
    const float worldW = (lay.maxX - lay.minX) + 2.f * cfg.marginM;
    const float worldH = (lay.maxZ - lay.minZ) + 2.f * cfg.marginM;
    const int w = static_cast<int>(worldW * cfg.pxPerMeter + 0.5f);
    const int h = static_cast<int>(worldH * cfg.pxPerMeter + 0.5f);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;

    std::vector<std::uint8_t> img(static_cast<std::size_t>(w) * h * 3);
    // 底色（纸白）
    for (std::size_t i = 0; i < img.size(); i += 3) {
        img[i] = 246; img[i + 1] = 245; img[i + 2] = 241;
    }

    const auto toPx = [&](float world, float minWorld) {
        return static_cast<int>((world - minWorld + cfg.marginM) * cfg.pxPerMeter);
    };

    // block 内区（cell 减路带：4.5m x 3.0m），灰度 = 233 - 2*shade（233..203）。
    // 路带以网格线为中心占半宽（kRoadWidth*0.5），内区从路带边缘起算。
    for (const CityCell& c : lay.cells) {
        const float cx = static_cast<float>(c.gx) * kBlockX;
        const float cz = static_cast<float>(c.gz) * kBlockZ;
        const std::uint32_t v = 233u - 2u * c.shade;
        const float halfRoad = kRoadWidth * 0.5f;
        fillRect(img, w, h,
                 toPx(cx + halfRoad, lay.minX), toPx(cz + halfRoad, lay.minZ),
                 toPx(cx + kBlockX - halfRoad, lay.minX),
                 toPx(cz + kBlockZ - halfRoad, lay.minZ),
                 static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v),
                 static_cast<std::uint8_t>(v));
    }

    // 路网：全跨度横线 size+1 条（z = j*4.0），竖线 size+1 条（x = i*5.5），1.0m 宽。
    const std::int32_t hi = lay.lo + static_cast<std::int32_t>(lay.params.size);
    const float halfRoad = kRoadWidth * 0.5f;
    const std::uint8_t rr = 72, gg = 72, bb = 76;
    for (std::int32_t j = lay.lo; j <= hi; ++j) {
        const float z = static_cast<float>(j) * kBlockZ;
        fillRect(img, w, h, 0, toPx(z - halfRoad, lay.minZ), w,
                 toPx(z + halfRoad, lay.minZ), rr, gg, bb);
    }
    for (std::int32_t i = lay.lo; i <= hi; ++i) {
        const float x = static_cast<float>(i) * kBlockX;
        fillRect(img, w, h, toPx(x - halfRoad, lay.minX), 0,
                 toPx(x + halfRoad, lay.minX), h, rr, gg, bb);
    }

    outW = w;
    outH = h;
    return encodePng(img, w, h, outPng);
}

// ---------------------------------------------------------------------------
// selftest —— M-W0.0 验收判据①：seed 对称确定性（同 seed 双跑逐字节一致、
// 不同 seed 必不同），照 nav/phys 确定性铁律；另锁 pcg32 已知答案与布局锚。
// ---------------------------------------------------------------------------
struct SelftestResult {
    bool pass = false;
    int assertions = 0;
    std::string firstFailure;
};

SelftestResult runSelftest(bool verbose) {
    SelftestResult res;
    int n = 0;
    auto check = [&](bool ok, const char* name) {
        ++n;
        if (verbose) std::printf("ok %d - %s\n", n, name);
        if (!ok && res.firstFailure.empty()) res.firstFailure = name;
        if (!ok) res.pass = false;
    };

    bool allOk = true;
    auto expect = [&](bool ok, const char* name) {
        check(ok, name);
        allOk = allOk && ok;
    };

    // --- pcg32 流确定性 + 已知答案（seed 12345 首输出锁实现版本）---
    Pcg32 a, b;
    a.seed(12345);
    b.seed(12345);
    bool streamSame = true;
    for (int i = 0; i < 64; ++i) streamSame = streamSame && (a.next() == b.next());
    expect(streamSame, "pcg32: same seed -> identical 64-output stream");

    a.seed(12345);
    expect(a.next() == 1321476956u, "pcg32: known-answer first output (seed 12345)");
    expect(a.next() == 17539747u, "pcg32: known-answer second output");
    expect(a.next() == 3348728241u, "pcg32: known-answer third output");

    Pcg32 c;
    c.seed(12346);
    expect(c.next() != 1321476956u, "pcg32: different seed -> different first output");

    // --- 布局确定性（规范序列化字节 + FNV-1a）---
    CityParams p0;
    p0.seed = 12345;
    p0.size = 6;
    const std::uint64_t h1 = fnv1a64(serializeLayout(generateCity(p0)));
    const std::uint64_t h2 = fnv1a64(serializeLayout(generateCity(p0)));
    expect(h1 == h2, "layout: same seed double-run byte-identical (FNV-1a)");

    CityParams p1 = p0;
    p1.seed = 12346;
    const std::uint64_t h3 = fnv1a64(serializeLayout(generateCity(p1)));
    expect(h1 != h3, "layout: different seed -> different bytes");

    CityParams p2 = p0;
    p2.size = 8;
    const std::uint64_t h4 = fnv1a64(serializeLayout(generateCity(p2)));
    expect(h1 != h4, "layout: different size -> different bytes");

    // --- 布局锚（size 6：36 block / 36 ground tile / 72 路段 / 49 交点）---
    const CityLayout lay = generateCity(p0);
    expect(lay.cells.size() == 36u, "layout: size 6 -> 36 cells");
    expect(lay.stats.blocks == 36u && lay.stats.groundTiles == 36u,
           "stats: blocks == groundTiles == 36");
    expect(lay.stats.roadSegments == 72u, "stats: roadSegments == 72");
    expect(lay.stats.crossroads == 49u, "stats: crossroads == (size+1)^2 == 49");
    expect(lay.stats.buildings == 0u && lay.stats.trees == 0u &&
               lay.stats.cars == 0u && lay.stats.fences == 0u &&
               lay.stats.paths == 0u,
           "stats: M-W0.2/0.3 skeleton counters all zero (blank city)");

    // --- 契约锚：block 尺寸与 roadnet.json blocks 同源；内区 4.5x3.0 ---
    expect(kBlockX == 5.5f && kBlockZ == 4.0f,
           "contract: block dims match roadnet.json blocks {5.5, 4.0}");
    expect((kBlockX - kRoadWidth) == 4.5f && (kBlockZ - kRoadWidth) == 3.0f,
           "contract: block interior 4.5 x 3.0 (bevy ground tile dims)");
    expect(lay.maxX - lay.minX == 33.0f && lay.maxZ - lay.minZ == 24.0f,
           "layout: size 6 extent x=33m z=24m");
    expect(lay.lo == -3, "layout: size 6 centered grid lo == -3");

    // --- shade 档位界 ---
    bool shadeIn = true;
    for (const CityCell& cl : lay.cells) shadeIn = shadeIn && cl.shade < 16u;
    expect(shadeIn, "rng: all cell shades in [0,16)");

    // --- PNG 确定性 + 签名 ---
    RasterConfig cfg;
    std::vector<std::uint8_t> pngA, pngB, pngC;
    int wA = 0, hA = 0, wB = 0, hB = 0, wC = 0, hC = 0;
    const bool ra = renderTopView(lay, cfg, pngA, wA, hA);
    const bool rb = renderTopView(lay, cfg, pngB, wB, hB);
    const bool rc = renderTopView(generateCity(p1), cfg, pngC, wC, hC);
    expect(ra && rb && rc, "render: three top views encoded");
    expect(pngA == pngB, "png: same seed double-encode byte-identical");
    expect(pngA != pngC, "png: different seed -> different pixels");
    expect(wA == wB && hA == hB && wA == 1152 && hA == 864,
           "render: size 6 canvas 1152x864 px (32 px/m, 1.5m margin)");
    const bool sig = pngA.size() >= 8 &&
                     std::memcmp(pngA.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    expect(sig, "png: valid signature bytes");

    res.assertions = n;
    res.pass = allOk;
    return res;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
void printUsage() {
    std::printf(
        "city-tools - nl-engine worldgen tools (M-W0 citygen, offline CPU only)\n"
        "usage:\n"
        "  city-tools selftest [--verbose]\n"
        "  city-tools generate --seed N --size N --out DIR [--stats]\n"
        "options:\n"
        "  --seed N   city seed, any u64 (default 12345)\n"
        "  --size N   grid blocks per side, 1..64 (default 6)\n"
        "  --out DIR  output directory, receives city_top.png\n"
        "  --stats    print CityStats counter block\n"
        "exit codes: 0 = PASS / 1 = selftest assertion failed / 2 = invalid usage\n");
}

bool parseU64(const char* s, std::uint64_t* v) {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    const unsigned long long r = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *v = static_cast<std::uint64_t>(r);
    return true;
}

struct GenerateConfig {
    CityParams city;
    std::string outDir;
    bool stats = false;
};

// 返回 0 = OK / 2 = 用法错误；err 填人类可读原因（ASCII）。
int parseGenerate(const std::vector<std::string>& args, GenerateConfig* cfg,
                  std::string* err) {
    bool hasOut = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& k = args[i];
        const bool hasVal = i + 1 < args.size();
        if (k == "--stats") {
            cfg->stats = true;
        } else if (k == "--seed" && hasVal) {
            std::uint64_t s = 0;
            if (!parseU64(args[++i].c_str(), &s)) {
                *err = "invalid arguments: bad --seed value";
                return 2;
            }
            cfg->city.seed = s;
        } else if (k == "--size" && hasVal) {
            std::uint64_t s = 0;
            if (!parseU64(args[++i].c_str(), &s) || s < kMinSize || s > kMaxSize) {
                *err = "invalid arguments: --size out of range (1..64)";
                return 2;
            }
            cfg->city.size = static_cast<std::uint32_t>(s);
        } else if (k == "--out" && hasVal) {
            cfg->outDir = args[++i];
            hasOut = true;
        } else if (k == "--seed" || k == "--size" || k == "--out") {
            *err = "invalid arguments: missing value for " + k;
            return 2;
        } else {
            *err = "invalid arguments: unknown option " + k;
            return 2;
        }
    }
    if (!hasOut) {
        *err = "invalid arguments: --out DIR required";
        return 2;
    }
    return 0;
}

int runGenerate(const GenerateConfig& cfg) {
    const CityLayout lay = generateCity(cfg.city);
    RasterConfig raster;
    std::vector<std::uint8_t> png;
    int w = 0, h = 0;
    if (!renderTopView(lay, raster, png, w, h)) {
        std::printf("[nl:error] render failed (canvas too large?)\n");
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(cfg.outDir, ec);
    if (ec) {
        std::printf("[nl:error] cannot create out dir %s\n", cfg.outDir.c_str());
        return 1;
    }
    const std::string pngPath =
        (std::filesystem::path(cfg.outDir) / "city_top.png").string();
    {
        FILE* f = std::fopen(pngPath.c_str(), "wb");
        if (f == nullptr) {
            std::printf("[nl:error] cannot open %s for write\n", pngPath.c_str());
            return 1;
        }
        const std::size_t wrote = std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
        if (wrote != png.size()) {
            std::printf("[nl:error] short write on %s\n", pngPath.c_str());
            return 1;
        }
    }

    std::printf("nl-citygen M-W0.0 blank city top view\n");
    std::printf("seed=%llu size=%u blocks=%u\n",
                static_cast<unsigned long long>(cfg.city.seed), cfg.city.size,
                lay.stats.blocks);
    std::printf("extent: x=[%.1f,%.1f] z=[%.1f,%.1f] m\n", lay.minX, lay.maxX,
                lay.minZ, lay.maxZ);
    std::printf("wrote %s (%dx%d px, scale %.0f px/m)\n", pngPath.c_str(), w, h,
                raster.pxPerMeter);
    if (cfg.stats) {
        std::printf(
            "stats: blocks=%u groundTiles=%u roadSegments=%u crossroads=%u "
            "buildings=%u trees=%u cars=%u fences=%u paths=%u\n",
            lay.stats.blocks, lay.stats.groundTiles, lay.stats.roadSegments,
            lay.stats.crossroads, lay.stats.buildings, lay.stats.trees,
            lay.stats.cars, lay.stats.fences, lay.stats.paths);
    }
    return 0;
}

} // namespace citygen

int main(int argc, char** argv) {
    using namespace citygen;
    const std::vector<std::string> args(argv + (argc > 1 ? 1 : argc),
                                        argv + argc);

    if (args.empty()) {
        printUsage();
        return 2;
    }
    const std::string& cmd = args[0];
    if (cmd == "selftest") {
        bool verbose = false;
        std::string err;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--verbose") {
                verbose = true;
            } else {
                err = "invalid arguments: unknown option " + args[i];
                break;
            }
        }
        if (!err.empty()) {
            std::printf("%s\n", err.c_str());
            printUsage();
            return 2;
        }
        const SelftestResult r = runSelftest(verbose);
        if (!r.pass) {
            std::printf("citygen selftest: FAIL (%s)\n", r.firstFailure.c_str());
            return 1;
        }
        std::printf("citygen selftest: PASS (%d assertions)\n", r.assertions);
        return 0;
    }
    if (cmd == "generate") {
        GenerateConfig cfg;
        std::string err;
        const int rc = parseGenerate(std::vector<std::string>(args.begin() + 1,
                                                              args.end()),
                                     &cfg, &err);
        if (rc != 0) {
            std::printf("%s\n", err.c_str());
            return rc;
        }
        return runGenerate(cfg);
    }
    if (cmd == "help" || cmd == "--help") {
        printUsage();
        return 0;
    }
    std::printf("invalid arguments: unknown command %s\n", cmd.c_str());
    printUsage();
    return 2;
}
