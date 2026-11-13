// city-tools —— nl-engine 大世界生成工具（M-W0 citygen，A 类并行轨道 feat/worldgen）
// M-W0.0：CLI 骨架 + 自研确定性 RNG（pcg32）+ 规则网格 block 布局 + 空白城俯视 PNG
//         （纯 CPU 光栅化）+ CityStats 计数锚骨架 + seed 对称确定性 selftest。
// M-W0.1：grid 路网拓扑 + 车道语义 → roadnet.json（W0-PLAN §1 字段契约，
//         Q-W2 拍板的 citygen ↔ 第 8 步交通接口铁律）+ 俯视图路网叠加。
// M-W0.2：OBB 地块递归细分 + 程序化楼体（RNG 分档高度/占地）→ 每 block 一份
//         OBJ 中间件 + city.scene（E1 World JSON，M-E2 World::serialise 形态）
//         索引全部分块与元数据。.nmesh 化走 nanite-tools / asset-ingest 通道
//         （不自写 meshing/DAG）；实测通道未就绪（nanite-tools 无 OBJ 导入，
//         asset-ingest 在 FBX→GLB），缺口登记给 asset-ingest 轨道，本轨不自修。
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

#include <algorithm>
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

// M-W0.2 楼体口径（W3-A 全自研最小算法；bevy_city 低密度每 block 4 栋同构）：
//   内区 4.5x3.0 递归细分（先 x 后 z 阈值分割）→ 4 叶，每叶一栋；
//   高度三档 / 占地两档（RNG 分档，消费序见文件头 RNG 纪律——排流尾）。
constexpr float kSplitXMax = 2.3f;  // 内区宽超过此值 → 对半分 x
constexpr float kSplitZMax = 1.6f;  // 内区深超过此值 → 对半分 z
constexpr float kInsetSmall = 0.15f; // 占地档 1：叶内缩（楼间隙小）
constexpr float kInsetLarge = 0.30f; // 占地档 0：叶内缩（楼间隙大）
constexpr float kHeightTiers[3] = {3.0f, 8.0f, 14.0f}; // RNG 分档高度（米）

constexpr std::uint32_t kMinSize = 1;
constexpr std::uint32_t kMaxSize = 64; // 64*5.5*32px = 11264px 画布上限内的机动余量

struct CityParams {
    std::uint64_t seed = 12345;
    std::uint32_t size = 6;
};

// CityStats —— bevy_city generate_city.rs CityStats 同构计数锚骨架。
// M-W0.0 只有布局计数；buildings/trees/cars/fences/paths 是 M-W0.2/0.3 的挂零锚。
// roadSegments（72@size6）是 bevy 逐 cell 口径（每 cell 一横一竖）；
// roadnetNodes/roadnetEdges（M-W0.1）是全跨度路网口径（见 buildRoadnet），
// 两种口径并存各有对应物：前者对齐 bevy CityStats.roads，后者对齐 roadnet.json。
struct CityStats {
    std::uint32_t blocks = 0;
    std::uint32_t groundTiles = 0;
    std::uint32_t roadSegments = 0;
    std::uint32_t crossroads = 0;
    std::uint32_t roadnetNodes = 0; // M-W0.1：(size+1)^2 交点
    std::uint32_t roadnetEdges = 0; // M-W0.1：有向边 4*size*(size+1)
    std::uint32_t buildings = 0;
    std::uint32_t trees = 0;
    std::uint32_t cars = 0;
    std::uint32_t fences = 0;
    std::uint32_t paths = 0;
};

// M-W0.2 楼体（block 局部坐标，y-up，米；center = cell 世界中心）。
struct CityBuilding {
    float cx = 0.f;   // 楼底面中心 x（block 局部）
    float cz = 0.f;   // 楼底面中心 z（block 局部）
    float hx = 0.f;   // 半宽 x
    float hz = 0.f;   // 半深 z
    float height = 0.f;
};

struct CityCell {
    std::int32_t gx = 0;   // 网格索引
    std::int32_t gz = 0;
    std::uint32_t shade = 0; // 地面灰度档位 0..15（M-W0.0 种子敏感元，见文件头 RNG 纪律）
    std::vector<CityBuilding> buildings; // M-W0.2（block 局部坐标）
};

struct CityLayout {
    CityParams params;
    std::int32_t lo = 0; // 网格低索引（-size/2）
    std::vector<CityCell> cells;
    CityStats stats;
    float minX = 0.f, maxX = 0.f, minZ = 0.f, maxZ = 0.f; // 城市外延（米）
};

// 递归细分内区矩形（block 局部坐标）：先 x 后 z 阈值对半分，叶 = 楼地块。
// 4.5x3.0 内区 → 4 叶 2.25x1.5（确定性：子矩形左/前半先入列）。
struct PlotRect { float x0, x1, z0, z1; };
void subdividePlot(float x0, float x1, float z0, float z1,
                   std::vector<PlotRect>* out) {
    const float w = x1 - x0;
    const float d = z1 - z0;
    if (w > kSplitXMax) {
        const float m = (x0 + x1) * 0.5f;
        subdividePlot(x0, m, z0, z1, out);
        subdividePlot(m, x1, z0, z1, out);
        return;
    }
    if (d > kSplitZMax) {
        const float m = (z0 + z1) * 0.5f;
        subdividePlot(x0, x1, z0, m, out);
        subdividePlot(x0, x1, m, z1, out);
        return;
    }
    PlotRect r{x0, x1, z0, z1};
    out->push_back(r);
}

// 生成确定性布局：RNG 消费序 = 文件头纪律。第一遍 cell 行主序消费 shade
// （M-W0.0 前缀，逐字节不动）；第二遍 cell 行主序消费楼体档位（M-W0.2 追加
// 流尾：每叶先 below(3) 高度档后 below(2) 占地档，叶序 = 细分序）。
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

    // M-W0.2 第二遍：楼体（消费排在全部 shade 之后 = 前缀契约保持）。
    // 内区 = block 局部 [-kBlockX/2+kRoadWidth/2, +...] x 同 z（路带边缘起算）。
    const float ix0 = -kBlockX * 0.5f + kRoadWidth * 0.5f;
    const float ix1 = kBlockX * 0.5f - kRoadWidth * 0.5f;
    const float iz0 = -kBlockZ * 0.5f + kRoadWidth * 0.5f;
    const float iz1 = kBlockZ * 0.5f - kRoadWidth * 0.5f;
    for (CityCell& cell : lay.cells) {
        std::vector<PlotRect> plots;
        subdividePlot(ix0, ix1, iz0, iz1, &plots);
        for (const PlotRect& p : plots) {
            CityBuilding b;
            const std::uint32_t heightTier = rng.below(3u); // 高度三档
            const std::uint32_t insetTier = rng.below(2u);  // 占地两档
            const float inset = insetTier == 0 ? kInsetLarge : kInsetSmall;
            b.cx = (p.x0 + p.x1) * 0.5f;
            b.cz = (p.z0 + p.z1) * 0.5f;
            b.hx = (p.x1 - p.x0) * 0.5f - inset;
            b.hz = (p.z1 - p.z0) * 0.5f - inset;
            b.height = kHeightTiers[heightTier];
            cell.buildings.push_back(b);
        }
    }

    lay.stats.blocks = params.size * params.size;
    lay.stats.groundTiles = lay.stats.blocks;
    lay.stats.roadSegments = 2u * lay.stats.blocks; // 每 cell 一横一竖
    lay.stats.crossroads = (params.size + 1u) * (params.size + 1u); // 网格线交点
    // M-W0.1 全跨度路网口径：节点 = (size+1)^2 交点；有向边 = 2 * 无向路段数，
    // 无向路段 = 横 size*(size+1) + 竖 size*(size+1)。
    lay.stats.roadnetNodes = (params.size + 1u) * (params.size + 1u);
    lay.stats.roadnetEdges = 4u * params.size * (params.size + 1u);
    // M-W0.2：楼体计数（每 cell = 细分叶数，4.5x3.0 内区恒 4）。
    for (const CityCell& cell : lay.cells) {
        lay.stats.buildings += static_cast<std::uint32_t>(cell.buildings.size());
    }
    // trees/cars/fences/paths 保持 0：M-W0.3 消费。

    lay.minX = static_cast<float>(lay.lo) * kBlockX;
    lay.maxX = static_cast<float>(hi) * kBlockX;
    lay.minZ = static_cast<float>(lay.lo) * kBlockZ;
    lay.maxZ = static_cast<float>(hi) * kBlockZ;
    return lay;
}

// ---------------------------------------------------------------------------
// Roadnet —— M-W0.1：grid 路网拓扑 + 车道语义（W0-PLAN §1 字段契约，
// Q-W2 拍板：自包含 roadnet.json，citygen ↔ 第 8 步交通的接口铁律）。
//
//   节点 = (size+1)^2 个网格线交点，id = row*(size+1)+col（行主序），
//          坐标 x=(lo+col)*kBlockX / z=(lo+row)*kBlockZ；
//   无向路段 = 2*size*(size+1)（横、竖各 size*(size+1) 条，全跨度）；
//   有向边 = 无向 ×2（契约：edges 有向，双向路产两条 from→to / to→from）。
//   确定性序：先横后竖、行主序，每路段先 +向 后 −向，id 连续递增。
//   M-W0.1 无 RNG 消费（空白城车道语义全同质），文件头消费序契约不受影响。
//
//   与 bevy_city 的对应（2026-11-13 重读留档）：Road{start,end} 是每方向
//   1 车道直线段 → lanes_per_dir=1；bevy 车 Car{offset,dir} 沿路行驶的
//   车道语义由第 8 步交通系统在 edges 之上构建，citygen 只供几何 + 拓扑。
// ---------------------------------------------------------------------------
constexpr std::uint32_t kLanesPerDir = 1; // bevy_city Road 同构：每方向 1 车道
constexpr float kSpeedLimit = 13.9f;      // m/s（契约示例同源，约 50 km/h）

struct RoadnetNode {
    std::uint32_t id = 0;
    float x = 0.f;
    float z = 0.f;
    // kind 恒 "intersection"、signal 恒 false（W0-PLAN §1：M-W0 首版口径，
    // 全部节点度 >= 2，无 dead_end；signal 由第 8 步消费，citygen 恒 false）。
};

struct RoadnetEdge {
    std::uint32_t id = 0;
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    std::uint32_t lanesPerDir = kLanesPerDir;
    float speedLimit = kSpeedLimit;
    float width = kRoadWidth; // 车行道总宽 = 路带几何同源（1.0m）；
                              // 契约示例 3.0 是示意值，此处以实际布局为准
};

struct Roadnet {
    std::vector<RoadnetNode> nodes;
    std::vector<RoadnetEdge> edges; // 有向
    std::uint64_t seed = 0;
    std::uint32_t size = 0;
};

Roadnet buildRoadnet(const CityLayout& lay) {
    Roadnet net;
    net.seed = lay.params.seed;
    net.size = lay.params.size;
    const std::uint32_t stride = net.size + 1u;

    net.nodes.resize(stride * stride);
    for (std::uint32_t row = 0; row < stride; ++row) {
        for (std::uint32_t col = 0; col < stride; ++col) {
            RoadnetNode& n = net.nodes[row * stride + col];
            n.id = row * stride + col;
            n.x = static_cast<float>(lay.lo + static_cast<std::int32_t>(col)) *
                  kBlockX;
            n.z = static_cast<float>(lay.lo + static_cast<std::int32_t>(row)) *
                  kBlockZ;
        }
    }

    net.edges.reserve(4u * net.size * stride);
    std::uint32_t id = 0;
    // 横向路段（沿 +x，长 kBlockX）：row 0..size，col 0..size-1。
    for (std::uint32_t row = 0; row < stride; ++row) {
        for (std::uint32_t col = 0; col + 1u < stride; ++col) {
            const std::uint32_t a = row * stride + col;
            const std::uint32_t b = row * stride + col + 1u;
            RoadnetEdge f;
            f.id = id++;
            f.from = a;
            f.to = b;
            RoadnetEdge r = f;
            r.id = id++;
            r.from = b;
            r.to = a;
            net.edges.push_back(f);
            net.edges.push_back(r);
        }
    }
    // 纵向路段（沿 +z，长 kBlockZ）：row 0..size-1，col 0..size。
    for (std::uint32_t row = 0; row + 1u < stride; ++row) {
        for (std::uint32_t col = 0; col < stride; ++col) {
            const std::uint32_t a = row * stride + col;
            const std::uint32_t b = (row + 1u) * stride + col;
            RoadnetEdge f;
            f.id = id++;
            f.from = a;
            f.to = b;
            RoadnetEdge r = f;
            r.id = id++;
            r.from = b;
            r.to = a;
            net.edges.push_back(f);
            net.edges.push_back(r);
        }
    }
    return net;
}

// roadnet.json 确定性序列化（W0-PLAN §1 契约原文形态：2 空格缩进、固定字段序、
// %.1f 米单位，C locale 固定小数点）。ASCII only。
std::string serializeRoadnet(const Roadnet& net) {
    std::string s;
    char buf[160];
    s += "{\n";
    s += "  \"version\": 1,\n";
    std::snprintf(buf, sizeof buf, "  \"seed\": %llu,\n",
                  static_cast<unsigned long long>(net.seed));
    s += buf;
    s += "  \"units\": \"meters\",\n";
    std::snprintf(buf, sizeof buf,
                  "  \"blocks\": { \"x\": %.1f, \"z\": %.1f },\n", kBlockX,
                  kBlockZ);
    s += buf;
    s += "  \"nodes\": [\n";
    for (std::size_t i = 0; i < net.nodes.size(); ++i) {
        const RoadnetNode& n = net.nodes[i];
        std::snprintf(buf, sizeof buf,
                      "    { \"id\": %u, \"x\": %.1f, \"z\": %.1f, "
                      "\"kind\": \"intersection\", \"signal\": false }%s\n",
                      n.id, n.x, n.z,
                      i + 1 == net.nodes.size() ? "" : ",");
        s += buf;
    }
    s += "  ],\n";
    s += "  \"edges\": [\n";
    for (std::size_t i = 0; i < net.edges.size(); ++i) {
        const RoadnetEdge& e = net.edges[i];
        std::snprintf(buf, sizeof buf,
                      "    { \"id\": %u, \"from\": %u, \"to\": %u, "
                      "\"lanes_per_dir\": %u, \"speed_limit\": %.1f, "
                      "\"width\": %.1f }%s\n",
                      e.id, e.from, e.to, e.lanesPerDir, e.speedLimit, e.width,
                      i + 1 == net.edges.size() ? "" : ",");
        s += buf;
    }
    s += "  ],\n";
    s += "  \"phases\": []\n";
    s += "}\n";
    return s;
}

// ---------------------------------------------------------------------------
// M-W0.2 产物：OBJ 中间件（每 block 一份）+ city.scene（E1 World JSON）。
// .nmesh 化走 nanite-tools / asset-ingest 通道（不自写 meshing/DAG）；
// 实测通道未就绪（viewer scene 模式白名单只认 .glb、nanite-tools 无 OBJ 导入），
// 缺口登记给 asset-ingest 轨道——city.scene 先引用 OBJ 中间件路径。
// ---------------------------------------------------------------------------

// 每 block 一份 OBJ：楼体 = 轴对齐盒（8 顶点 12 三角，外向绕序，y-up，
// block 局部坐标原点 = cell 中心）。确定性文本（%.3f，C locale，固定序）。
std::string serializeBlockObj(const CityCell& cell, std::uint32_t blockIndex,
                              std::uint64_t seed) {
    std::string s;
    char buf[128];
    std::snprintf(buf, sizeof buf,
                  "# city-tools M-W0.2 block mesh (block-local, y-up, meters)\n"
                  "# seed %llu block %u gx %d gz %d buildings %u\n",
                  static_cast<unsigned long long>(seed), blockIndex, cell.gx,
                  cell.gz, static_cast<unsigned>(cell.buildings.size()));
    s += buf;
    std::uint32_t base = 1; // OBJ 顶点索引从 1 起
    for (std::size_t bi = 0; bi < cell.buildings.size(); ++bi) {
        const CityBuilding& b = cell.buildings[bi];
        const float hx = b.hx, hz = b.hz, h = b.height;
        std::snprintf(buf, sizeof buf, "o b%u\n", static_cast<unsigned>(bi));
        s += buf;
        const float vx[8] = {b.cx - hx, b.cx + hx, b.cx + hx, b.cx - hx,
                             b.cx - hx, b.cx + hx, b.cx + hx, b.cx - hx};
        const float vy[8] = {0.f, 0.f, 0.f, 0.f, h, h, h, h};
        const float vz[8] = {b.cz - hz, b.cz - hz, b.cz + hz, b.cz + hz,
                             b.cz - hz, b.cz - hz, b.cz + hz, b.cz + hz};
        for (int i = 0; i < 8; ++i) {
            std::snprintf(buf, sizeof buf, "v %.3f %.3f %.3f\n", vx[i], vy[i],
                          vz[i]);
            s += buf;
        }
        static const std::uint32_t kTris[12][3] = {
            {1, 3, 4}, {1, 2, 3},   // 底面 (-y)
            {5, 8, 7}, {5, 7, 6},   // 顶面 (+y)
            {1, 6, 2}, {1, 5, 6},   // z- 面
            {3, 8, 4}, {3, 7, 8},   // z+ 面
            {2, 7, 3}, {2, 6, 7},   // x+ 面
            {1, 8, 5}, {1, 4, 8},   // x- 面
        };
        for (const auto& t : kTris) {
            std::snprintf(buf, sizeof buf, "f %u %u %u\n", base + t[0] - 1,
                          base + t[1] - 1, base + t[2] - 1);
            s += buf;
        }
        base += 8;
    }
    return s;
}

// city.scene（E1 World JSON，M-E2 World::serialise 形态；语义同构、手写固定
// 序确定性文本——citygen 产物，非 ECS roundtrip dump，键序不锁字母序）。
// 实体 = 每 block 一个（行主序），Transform 位置 = block 世界中心，
// Model.mesh 引用 blocks/block_XX.obj（OBJ 中间件，通道就绪后由转换步替换）。
std::string serializeCityScene(const CityLayout& lay) {
    std::string s;
    char buf[192];
    const std::uint32_t n = static_cast<std::uint32_t>(lay.cells.size());
    s += "{\n";
    s += "  \"version\": 1,\n";
    std::snprintf(buf, sizeof buf, "  \"nextID\": %u,\n", n + 1u);
    s += buf;
    s += "  \"entities\": [\n";
    for (std::uint32_t i = 0; i < n; ++i) {
        const CityCell& c = lay.cells[i];
        const float cx =
            (static_cast<float>(c.gx) + 0.5f) * kBlockX; // block 世界中心
        const float cz = (static_cast<float>(c.gz) + 0.5f) * kBlockZ;
        std::snprintf(buf, sizeof buf,
                      "    { \"id\": %u, \"name\": \"block_%02u\", \"flags\": 0, "
                      "\"parent\": 0, \"components\": {\n"
                      "      \"Model\": { \"mesh\": \"blocks/block_%02u.obj\", "
                      "\"tint\": [0.8, 0.8, 0.8] },\n"
                      "      \"Transform\": { \"position\": [%.2f, 0.0, %.2f], "
                      "\"scale\": [1.0, 1.0, 1.0], "
                      "\"rotation\": [0.0, 0.0, 0.0, 1.0] }\n"
                      "    } }%s\n",
                      i + 1u, i, i, cx, cz,
                      i + 1 == n ? "" : ",");
        s += buf;
    }
    s += "  ],\n";
    s += "  \"systems\": []\n";
    s += "}\n";
    return s;
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
    appendU32(b, lay.stats.roadnetNodes);
    appendU32(b, lay.stats.roadnetEdges);
    appendU32(b, lay.stats.buildings);
    appendU32(b, lay.stats.trees);
    appendU32(b, lay.stats.cars);
    appendU32(b, lay.stats.fences);
    appendU32(b, lay.stats.paths);
    for (const CityCell& c : lay.cells) {
        appendI32(b, c.gx);
        appendI32(b, c.gz);
        b.push_back(static_cast<std::uint8_t>(c.shade));
        // M-W0.2：楼体数据进规范序列化（float 按 IEEE 位原样写入，确定性）。
        appendU32(b, static_cast<std::uint32_t>(c.buildings.size()));
        for (const CityBuilding& bd : c.buildings) {
            float fields[5] = {bd.cx, bd.cz, bd.hx, bd.hz, bd.height};
            for (float f : fields) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, 4);
                appendU32(b, bits);
            }
        }
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

    // M-W0.2 楼体占地俯视（高度分档灰度：3m=208 / 8m=168 / 14m=128，越矮越浅）。
    for (const CityCell& c : lay.cells) {
        const float ccx = (static_cast<float>(c.gx) + 0.5f) * kBlockX;
        const float ccz = (static_cast<float>(c.gz) + 0.5f) * kBlockZ;
        for (const CityBuilding& b : c.buildings) {
            const std::uint8_t v = b.height == kHeightTiers[0]   ? 208
                                   : b.height == kHeightTiers[1] ? 168
                                                                 : 128;
            fillRect(img, w, h, toPx(ccx + b.cx - b.hx, lay.minX),
                     toPx(ccz + b.cz - b.hz, lay.minZ),
                     toPx(ccx + b.cx + b.hx, lay.minX),
                     toPx(ccz + b.cz + b.hz, lay.minZ), v, v, v);
        }
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

    // M-W0.1 路网叠加（验收判据①）：从 roadnet 数据结构反画有向边中心线与
    // 节点标记——证明 roadnet.json 与俯视几何同源，而非布局代码直接画。
    const Roadnet net = buildRoadnet(lay);
    const std::uint8_t er = 150, eg = 40, eb = 40; // 边中心线：暗红
    for (const RoadnetEdge& e : net.edges) {
        const RoadnetNode& a = net.nodes[e.from];
        const RoadnetNode& b = net.nodes[e.to];
        const int ax = toPx(a.x, lay.minX);
        const int az = toPx(a.z, lay.minZ);
        const int bx = toPx(b.x, lay.minX);
        const int bz = toPx(b.z, lay.minZ);
        fillRect(img, w, h, std::min(ax, bx), std::min(az, bz),
                 std::max(ax, bx) + 1, std::max(az, bz) + 1, er, eg, eb);
    }
    for (const RoadnetNode& n : net.nodes) {
        const int px = toPx(n.x, lay.minX);
        const int pz = toPx(n.z, lay.minZ);
        fillRect(img, w, h, px - 1, pz - 1, px + 2, pz + 2, 120, 30, 30); // 3x3 节点
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
    expect(lay.stats.buildings == 144u,
           "stats: buildings == 4 per block * 36 == 144 (M-W0.2)");
    expect(lay.stats.trees == 0u && lay.stats.cars == 0u &&
               lay.stats.fences == 0u && lay.stats.paths == 0u,
           "stats: M-W0.3 skeleton counters still zero");

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

    // --- M-W0.1 roadnet 契约（W0-PLAN §1 接口铁律，验收判据②）---
    const Roadnet net = buildRoadnet(lay);
    expect(net.nodes.size() == 49u,
           "roadnet: size 6 -> (size+1)^2 == 49 nodes");
    expect(net.edges.size() == 168u,
           "roadnet: size 6 -> directed edges 4*size*(size+1) == 168");
    expect(lay.stats.roadnetNodes == 49u && lay.stats.roadnetEdges == 168u,
           "stats: roadnetNodes=49 roadnetEdges=168 (directed)");

    bool closureOk = true;
    for (const RoadnetEdge& e : net.edges) {
        closureOk = closureOk && e.from < net.nodes.size() &&
                    e.to < net.nodes.size() && e.id < net.edges.size();
    }
    expect(closureOk,
           "roadnet: from/to/id references closed (indices in range)");

    // 双向对称：每条有向边必有反向边（契约：双向路产两条）。
    {
        const auto pairKey = [](std::uint32_t a, std::uint32_t b) {
            return (static_cast<std::uint64_t>(a) << 32) | b;
        };
        std::vector<std::uint64_t> keys;
        keys.reserve(net.edges.size());
        for (const RoadnetEdge& e : net.edges) {
            keys.push_back(pairKey(e.from, e.to));
        }
        bool symOk = true;
        for (std::uint64_t k : keys) {
            const std::uint64_t rev = pairKey(
                static_cast<std::uint32_t>(k & 0xFFFFFFFFu),
                static_cast<std::uint32_t>(k >> 32));
            symOk = symOk &&
                    std::find(keys.begin(), keys.end(), rev) != keys.end();
        }
        expect(symOk, "roadnet: bidirectional symmetry (every edge has reverse)");
    }

    // 几何：每条有向边恰好跨一个 block 间距（横 5.5 / 竖 4.0）。
    {
        bool geomOk = true;
        std::uint32_t horiz = 0, vert = 0;
        for (const RoadnetEdge& e : net.edges) {
            const RoadnetNode& na = net.nodes[e.from];
            const RoadnetNode& nb = net.nodes[e.to];
            const float dx = nb.x - na.x;
            const float dz = nb.z - na.z;
            if (dz == 0.f && (dx == kBlockX || dx == -kBlockX)) {
                ++horiz;
            } else if (dx == 0.f && (dz == kBlockZ || dz == -kBlockZ)) {
                ++vert;
            } else {
                geomOk = false;
            }
        }
        expect(geomOk, "roadnet: edge geometry spans exactly one block pitch");
        expect(horiz == 84u && vert == 84u,
               "roadnet: 84 horizontal + 84 vertical directed edges");
    }

    expect(net.nodes[0].x == -16.5f && net.nodes[0].z == -12.0f,
           "roadnet: node 0 at grid lo corner (-16.5, -12.0)");
    expect(net.nodes[48].x == 16.5f && net.nodes[48].z == 12.0f,
           "roadnet: node 48 at grid hi corner (16.5, 12.0)");

    bool attrsOk = true;
    for (const RoadnetEdge& e : net.edges) {
        attrsOk = attrsOk && e.lanesPerDir == kLanesPerDir &&
                  e.speedLimit == kSpeedLimit && e.width == kRoadWidth;
    }
    expect(attrsOk, "roadnet: lanes/speed/width uniform (1 / 13.9 / 1.0)");

    // JSON 文本锚 + 确定性。
    const std::string jsonA = serializeRoadnet(net);
    expect(jsonA == serializeRoadnet(buildRoadnet(generateCity(p0))),
           "roadnet: same seed double-build JSON byte-identical");
    expect(jsonA.find("\"version\": 1") != std::string::npos &&
               jsonA.find("\"units\": \"meters\"") != std::string::npos &&
               jsonA.find("\"phases\": []") != std::string::npos,
           "roadnet: contract header fields (version/units/phases)");
    {
        std::uint32_t kindCount = 0, laneCount = 0;
        std::size_t pos = 0;
        const std::string k1 = "\"kind\": \"intersection\"";
        while ((pos = jsonA.find(k1, pos)) != std::string::npos) {
            ++kindCount;
            pos += k1.size();
        }
        pos = 0;
        const std::string k2 = "\"lanes_per_dir\": 1";
        while ((pos = jsonA.find(k2, pos)) != std::string::npos) {
            ++laneCount;
            pos += k2.size();
        }
        expect(kindCount == 49u && laneCount == 168u,
               "roadnet: JSON anchors 49 intersections / 168 directed edges");
    }
    expect(serializeRoadnet(buildRoadnet(generateCity(p1))) != jsonA,
           "roadnet: different seed -> different JSON (seed field)");

    CityParams ps = p0;
    ps.size = 1;
    const Roadnet netS = buildRoadnet(generateCity(ps));
    expect(netS.nodes.size() == 4u && netS.edges.size() == 8u,
           "roadnet: size 1 -> 4 nodes / 8 directed edges (formula edge)");

    // --- M-W0.2 楼体 + OBJ + city.scene（验收判据：细分/RNG 档位/引用闭合）---
    // RNG 前缀契约：36 个 shade 仍是前缀（重建流逐个比对）。
    {
        Pcg32 pr;
        pr.seed(p0.seed);
        bool prefixOk = true;
        for (const CityCell& cl : lay.cells) {
            prefixOk = prefixOk && pr.below(16u) == cl.shade;
        }
        expect(prefixOk,
               "rng: cell shade stream is an untouched prefix (M-W0.2 appends)");
    }

    // 楼体口径：4 栋/块、高度恰为三档之一、占地两档之一、足迹在叶内不越内区、
    // 同块足迹两两不重叠。
    bool bldOk = true, tierOk = true, boundsOk = true, disjointOk = true;
    std::uint32_t tallCount = 0;
    for (const CityCell& cl : lay.cells) {
        if (cl.buildings.size() != 4u) bldOk = false;
        for (const CityBuilding& bd : cl.buildings) {
            const bool hOk = bd.height == kHeightTiers[0] ||
                             bd.height == kHeightTiers[1] ||
                             bd.height == kHeightTiers[2];
            tierOk = tierOk && hOk;
            if (bd.height == kHeightTiers[2]) ++tallCount;
            const float inset = 1.125f - bd.hx; // 叶半宽 1.125 - 内缩
            boundsOk = boundsOk &&
                       ((inset > kInsetSmall - 0.001f &&
                         inset < kInsetSmall + 0.001f) ||
                        (inset > kInsetLarge - 0.001f &&
                         inset < kInsetLarge + 0.001f));
            boundsOk = boundsOk && bd.hx > 0.f && bd.hz > 0.f &&
                       bd.cx - bd.hx >= -2.25f && bd.cx + bd.hx <= 2.25f &&
                       bd.cz - bd.hz >= -1.5f && bd.cz + bd.hz <= 1.5f;
            for (const CityBuilding& o : cl.buildings) {
                if (&o == &bd) continue;
                const bool sepX = bd.cx + bd.hx <= o.cx - o.hx + 1e-4f ||
                                  o.cx + o.hx <= bd.cx - bd.hx + 1e-4f;
                const bool sepZ = bd.cz + bd.hz <= o.cz - o.hz + 1e-4f ||
                                  o.cz + o.hz <= bd.cz - bd.hz + 1e-4f;
                disjointOk = disjointOk && (sepX || sepZ);
            }
        }
    }
    expect(bldOk, "buildings: 4 per block (subdivision leaves, bevy low-density)");
    expect(tierOk, "buildings: heights exactly on the 3 RNG tiers {3,8,14}");
    expect(boundsOk, "buildings: footprint inset on 2 tiers, inside interior");
    expect(disjointOk, "buildings: footprints pairwise disjoint per block");
    expect(tallCount > 0u && tallCount < 144u,
           "buildings: height tiers mixed across city (seed-sensitive)");

    // OBJ：每块 4 objects / 32 v / 48 f，双跑逐字节、异 seed 必异。
    {
        const std::string objA = serializeBlockObj(lay.cells[0], 0, p0.seed);
        const std::string objB =
            serializeBlockObj(generateCity(p0).cells[0], 0, p0.seed);
        expect(objA == objB, "obj: same seed double-build byte-identical");
        std::uint32_t vCount = 0, fCount = 0, oCount = 0;
        for (std::size_t pos = objA.find("\nv "); // 行首统计（首行是注释）
             pos != std::string::npos; pos = objA.find("\nv ", pos + 1)) ++vCount;
        for (std::size_t pos = objA.find("\nf ");
             pos != std::string::npos; pos = objA.find("\nf ", pos + 1)) ++fCount;
        for (std::size_t pos = objA.find("\no ");
             pos != std::string::npos; pos = objA.find("\no ", pos + 1)) ++oCount;
        expect(vCount == 32u && fCount == 48u && oCount == 4u,
               "obj: 4 buildings -> 32 verts / 48 tris / 4 objects");
        expect(serializeBlockObj(generateCity(p1).cells[0], 0, p1.seed) != objA,
               "obj: different seed -> different bytes (tier draws)");
    }

    // city.scene：E1 形态锚 + 引用闭合 + 确定性。
    {
        const std::string scA = serializeCityScene(lay);
        expect(scA == serializeCityScene(generateCity(p0)),
               "scene: same seed double-build byte-identical");
        expect(scA.find("\"version\": 1") != std::string::npos &&
                   scA.find("\"nextID\": 37") != std::string::npos &&
                   scA.find("\"systems\": []") != std::string::npos,
               "scene: E1 World JSON anchors (version/nextID/systems)");
        std::uint32_t entCount = 0, meshCount = 0;
        std::size_t pos = 0;
        while ((pos = scA.find("\"name\": \"block_", pos)) != std::string::npos) {
            ++entCount;
            pos += 15;
        }
        pos = 0;
        while ((pos = scA.find("\"mesh\": \"blocks/block_", pos)) != std::string::npos) {
            ++meshCount;
            pos += 21;
        }
        expect(entCount == 36u && meshCount == 36u,
               "scene: 36 entities with 36 mesh references (blocks=36)");
        // 引用闭合：每个 mesh 引用都有对应 OBJ 产物（由同一次生成写出）。
        bool refClosureOk = true;
        for (std::uint32_t i = 0; i < lay.cells.size(); ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "blocks/block_%02u.obj", i);
            refClosureOk = refClosureOk &&
                           scA.find(std::string("\"mesh\": \"") + name + "\"") !=
                               std::string::npos;
        }
        expect(refClosureOk,
               "scene: every mesh reference closed to an emitted OBJ");
        expect(serializeCityScene(generateCity(p1)) == scA,
               "scene: seed-agnostic index (E1 schema has no seed key; seed "
               "lives in mesh contents, not in the scene)");
    }

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
        "  --out DIR  output directory, receives city_top.png + roadnet.json\n"
        "             + blocks/block_NN.obj (M-W0.2) + city.scene\n"
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

    // M-W0.1：roadnet.json（W0-PLAN §1 契约产物，第 8 步交通的接口）。
    const Roadnet net = buildRoadnet(lay);
    const std::string json = serializeRoadnet(net);
    const std::string jsonPath =
        (std::filesystem::path(cfg.outDir) / "roadnet.json").string();
    {
        FILE* f = std::fopen(jsonPath.c_str(), "wb");
        if (f == nullptr) {
            std::printf("[nl:error] cannot open %s for write\n",
                        jsonPath.c_str());
            return 1;
        }
        const std::size_t wrote = std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        if (wrote != json.size()) {
            std::printf("[nl:error] short write on %s\n", jsonPath.c_str());
            return 1;
        }
    }

    // M-W0.2：每 block 一份 OBJ 中间件 + city.scene（E1 World JSON 索引）。
    // .nmesh 化走 nanite-tools / asset-ingest 通道（缺口已登记，本轨不自修）。
    const std::string blocksDir =
        (std::filesystem::path(cfg.outDir) / "blocks").string();
    std::filesystem::create_directories(blocksDir, ec);
    if (ec) {
        std::printf("[nl:error] cannot create blocks dir %s\n",
                    blocksDir.c_str());
        return 1;
    }
    for (std::uint32_t i = 0; i < lay.cells.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "block_%02u.obj", i);
        const std::string objPath =
            (std::filesystem::path(blocksDir) / name).string();
        const std::string obj =
            serializeBlockObj(lay.cells[i], i, cfg.city.seed);
        FILE* f = std::fopen(objPath.c_str(), "wb");
        if (f == nullptr) {
            std::printf("[nl:error] cannot open %s for write\n",
                        objPath.c_str());
            return 1;
        }
        const std::size_t wrote = std::fwrite(obj.data(), 1, obj.size(), f);
        std::fclose(f);
        if (wrote != obj.size()) {
            std::printf("[nl:error] short write on %s\n", objPath.c_str());
            return 1;
        }
        std::printf("wrote %s (%u buildings)\n", objPath.c_str(),
                    static_cast<unsigned>(lay.cells[i].buildings.size()));
    }
    const std::string scene = serializeCityScene(lay);
    const std::string scenePath =
        (std::filesystem::path(cfg.outDir) / "city.scene").string();
    {
        FILE* f = std::fopen(scenePath.c_str(), "wb");
        if (f == nullptr) {
            std::printf("[nl:error] cannot open %s for write\n",
                        scenePath.c_str());
            return 1;
        }
        const std::size_t wrote = std::fwrite(scene.data(), 1, scene.size(), f);
        std::fclose(f);
        if (wrote != scene.size()) {
            std::printf("[nl:error] short write on %s\n", scenePath.c_str());
            return 1;
        }
    }
    std::printf("wrote %s (entities=%u meshes=%u)\n", scenePath.c_str(),
                static_cast<unsigned>(lay.cells.size()),
                static_cast<unsigned>(lay.cells.size()));

    std::printf("nl-citygen M-W0.2 city top view + roadnet + blocks + scene\n");
    std::printf("seed=%llu size=%u blocks=%u\n",
                static_cast<unsigned long long>(cfg.city.seed), cfg.city.size,
                lay.stats.blocks);
    std::printf("extent: x=[%.1f,%.1f] z=[%.1f,%.1f] m\n", lay.minX, lay.maxX,
                lay.minZ, lay.maxZ);
    std::printf("wrote %s (%dx%d px, scale %.0f px/m)\n", pngPath.c_str(), w, h,
                raster.pxPerMeter);
    std::printf("wrote %s (nodes=%u edges=%u directed)\n", jsonPath.c_str(),
                static_cast<unsigned>(net.nodes.size()),
                static_cast<unsigned>(net.edges.size()));
    if (cfg.stats) {
        std::printf(
            "stats: blocks=%u groundTiles=%u roadSegments=%u crossroads=%u "
            "roadnetNodes=%u roadnetEdges=%u buildings=%u trees=%u cars=%u "
            "fences=%u paths=%u\n",
            lay.stats.blocks, lay.stats.groundTiles, lay.stats.roadSegments,
            lay.stats.crossroads, lay.stats.roadnetNodes,
            lay.stats.roadnetEdges, lay.stats.buildings, lay.stats.trees,
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
