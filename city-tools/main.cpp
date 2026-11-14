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
//   M-W0.3 最终形态：① 36 shade → ② 288 楼体档位 → ③ noiseSeed u32（1 次 next）
//   → ④ 树位置（次数由密度档决定，流尾最后）。③④ 纯追加，①② 逐字节不动。
//   M-W2.0 楼型变体零 RNG 消费（fnv1a64 纯函数，M-W1.1 材质档同构先例）——
//   本头注释的消费序契约自 M-W0.3 起冻结，M-W2 未追加任何消费点。
//   M-W3.0 树/车/路灯摆件：树沿用 ④ 段 RNG 位置（零新增消费），树型/车型/
//   街灯摆放全部 fnv1a64 纯函数（assetVariantHash，W3-PLAN 拍板 B1）——
//   RNG 流契约继续冻结，无新增消费点。
//
// M-W3.0 外部资产通道（docs/worldgen/W3-PLAN.md 拍板 A1+B1+C1+D1）：树池 12
//   种（thirdparty/kenney-nature-kit/，名字表 kTreeVariantNames selftest 锁）+
//   车池 8 种（kenney-car-kit，沿路网边停靠 t∈[0.2,0.8]）+ 路灯（poly-
//   streetlight，每路口 1 盏）。glb 本体由 drive 从 vendored 目录装配
//   （city-tools 零资产依赖，A1 拍板）；车运动归 M-T0 交通轨。
//
// M-W2.0 楼型池（docs/worldgen/W2-PLAN.md §2 拍板：A1 程序化楼型池 + B1 纯函数
//   选型）：CityBuilding 加 variant 字段，fnv1a64(seed‖block‖building) % 池容量
//   按材质档映射 {tier1:{单盒,退台,L形}, tier2/3:{单盒,退台,裙楼塔}}；裙楼在
//   h<6m 纯函数降级为退台。serializeBlockObj 按 buildingParts 展开（每 part
//   8 顶点 12 三角，UV 沿 W1.1 立面平铺规则）。bevy_city 楼型池 = 外部 glb
//   资产池（kenney building-type-N.glb），本轨几何自研不走外部资产（kenney
//   色板图集前科 + W2-PLAN §2 A2 否决记录）。
//
// M-W0.3 噪声口径（2026-11-13 三处实读留档，docs/worldgen/CITY-TOOLS-GUIDE.md §4）：
//   simplex 梯度噪声（OpenSimplex 结构同构：F2=(√3-1)/2、G2=(3-√3)/6 数学推导常数，
//   梯度表 8 方向 (±1,±2)/(±2,±1)，径向核 t=t²·t²，t⁴·dot），perm 表由独立
//   Pcg32(noiseSeed) Fisher-Yates 洗牌——W3-A 全自研拍板范围，Python 参考实现
//   独立复算已知答案（eval 锚 + perm 头锚）防自证。kNoiseNorm=40 经验归一化
//   （129² 采样实测 max|raw|=0.0218 → 归一后 max≈0.87，|v|≤1 断言余量 13%）。
//   密度阈值 bevy_city 同源：density<0.45 forest / <0.6 low / <0.7 medium / else
//   high（noise_scale 0.025→0.06 显式偏离：bevy 值在 33m 跨度 size6 下噪声域
//   仅 0.83 周期、四档不可见，判据①要求可见）。树 = forest 每块 8 棵 / low 4 棵，
//   仅俯视图 + 统计层，不进 OBJ/city.scene（保 w0-m2 冻结基线产物）。
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
#include <fstream>
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

// M-W0.3 噪声 / 密度常数（bevy_city 同构 + 两处显式偏离，见文件头口径段）。
constexpr double kNoiseScale = 0.06;  // bevy 0.025 → 0.06（size6 分区可见性，留档）
constexpr double kNoiseNorm = 40.0;   // 经验归一化（实测 raw max 0.0218 → 0.87）
constexpr float kDensityForest = 0.45f;  // bevy 同源阈值
constexpr float kDensityLow = 0.6f;
constexpr float kDensityMedium = 0.7f;
constexpr std::uint32_t kTreesForest = 8; // forest 每块 8 棵
constexpr std::uint32_t kTreesLow = 4;    // low 每块 4 棵

// SimplexNoise —— 自研 simplex 梯度噪声（OpenSimplex 结构同构，W3-A 拍板零依赖；
// 算法公共领域口径，常数/梯度表/perm 初始化均由本文件自定，Python 参考实现
// 独立复算已知答案防自证）。内部纯 double 计算（Python 复算路径一致），输出 float。
struct SimplexNoise {
    std::uint8_t perm[512]; // 256 洗牌 + 复制一倍（索引回绕）

    static constexpr double kF2 = 0.3660254037844386;  // (√3-1)/2（数学推导）
    static constexpr double kG2 = 0.21132486540518713; // (3-√3)/6
    // 8 方向梯度（(±1,±2)/(±2,±1)，非归一化；方向集自定，Python 复算一致）
    static constexpr double kGrad[8][2] = {
        {1, 2}, {2, 1}, {-1, 2}, {-2, 1},
        {1, -2}, {2, -1}, {-1, -2}, {-2, -1},
    };

    void seed(std::uint32_t noiseSeed) {
        Pcg32 rng;
        rng.seed(noiseSeed);
        std::uint8_t base[256];
        for (std::uint32_t i = 0; i < 256; ++i) base[i] = static_cast<std::uint8_t>(i);
        for (std::uint32_t i = 255; i >= 1; --i) { // Fisher-Yates（below 拒绝采样）
            const std::uint32_t j = rng.below(i + 1u);
            const std::uint8_t t = base[i];
            base[i] = base[j];
            base[j] = t;
        }
        for (std::uint32_t i = 0; i < 512; ++i) perm[i] = base[i & 255u];
    }

    static int fastFloor(double v) {
        const int fi = static_cast<int>(v);
        return (v < static_cast<double>(fi)) ? fi - 1 : fi;
    }

    static double contrib(int gi, double dx, double dy) {
        double t = 0.5 - dx * dx - dy * dy;
        if (t <= 0.0) return 0.0;
        t *= t; // t^2 再平方 = t^4 径向核（Perlin simplex 风格）
        return t * t * (kGrad[gi & 7][0] * dx + kGrad[gi & 7][1] * dy);
    }

    float evaluate(double x, double y) const {
        const double s = (x + y) * kF2; // skew 到 simplex 格
        const int i = fastFloor(x + s);
        const int j = fastFloor(y + s);
        const double t = static_cast<double>(i + j) * kG2;
        const double x0 = x - (static_cast<double>(i) - t);
        const double y0 = y - (static_cast<double>(j) - t);
        const int i1 = (x0 > y0) ? 1 : 0; // 上下三角分支
        const int j1 = (x0 > y0) ? 0 : 1;
        const double x1 = x0 - static_cast<double>(i1) + kG2;
        const double y1 = y0 - static_cast<double>(j1) + kG2;
        const double x2 = x0 - 1.0 + 2.0 * kG2;
        const double y2 = y0 - 1.0 + 2.0 * kG2;
        const int ii = i & 255;
        const int jj = j & 255;
        const int gi0 = perm[perm[ii] + jj] & 7;          // 双重查表（Python 参考同口径）
        const int gi1 = perm[perm[ii + i1] + jj + j1] & 7;
        const int gi2 = perm[perm[ii + 1] + jj + 1] & 7;
        const double raw = contrib(gi0, x0, y0) +
                           contrib(gi1, x1, y1) +
                           contrib(gi2, x2, y2);
        return static_cast<float>(raw * kNoiseNorm);
    }
};

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
    std::uint32_t trees = 0;        // M-W0.3：树总数（forest 8/块 + low 4/块）
    std::uint32_t forestCells = 0;  // M-W0.3：密度分档 cell 计数（和恒 = blocks）
    std::uint32_t lowCells = 0;
    std::uint32_t mediumCells = 0;
    std::uint32_t highCells = 0;
    std::uint32_t cars = 0;
    std::uint32_t fences = 0;
    std::uint32_t paths = 0;
    std::uint32_t props = 0; // M-W3.0：街具数（路灯，每路口 1 盏）
};

// M-W0.2 楼体（block 局部坐标，y-up，米；center = cell 世界中心）。
struct CityBuilding {
    float cx = 0.f;   // 楼底面中心 x（block 局部）
    float cz = 0.f;   // 楼底面中心 z（block 局部）
    float hx = 0.f;   // 半宽 x
    float hz = 0.f;   // 半深 z
    float height = 0.f;
    // M-W2.0 楼型变体（W2-PLAN §2.1）：0=单盒 1=退台 2=裙楼塔 3=L 形。
    // 选取 = fnv1a64 纯函数（零 RNG 消费），生成期一次定死并进规范序列化。
    std::uint32_t variant = 0;
};

// M-W0.3 树（block 局部坐标）。M-W3.0：进 city.scene（W3-PLAN 拍板 B1），
// 变体 = fnv1a64 纯函数选型（零 RNG；位置仍是 M-W0.3 RNG 流尾数据，不动）。
struct CityTree {
    float cx = 0.f; // 树中心 x（block 局部）
    float cz = 0.f;
    // M-W3.0 树型池索引（kTreeVariantNames；W3-PLAN §2 C1 拍板 12 池）。
    std::uint32_t variant = 0;
};

// M-W3.0 车（静态停靠；运动系统归 M-T0 交通轨）：沿路网有向边采样。
// 位置 = 边线性插值（t ∈ [0.2,0.8] 停靠区间避开路口交叉区）+ 垂边偏移
// 0.3m（路缘内）；yaw = 边朝向（车头贴行驶方向，kenney car 模型 -z 朝前
// 口径，渲染验收校正点）。
struct CityCar {
    float x = 0.f;
    float z = 0.f;
    float yaw = 0.f;
    std::uint32_t variant = 0; // kCarVariantNames 池索引（8 池）
    std::uint32_t edgeId = 0;  // 溯源（M-T0 交通消费接口预埋）
};

// M-W3.0 街具（通道试点：路灯 ×1 种）。每路口 1 盏，四角之一（hash 选角），
// 贴路缘外 0.15m。后续加街具 = 新 kind + 名字表条目，纯数据扩展。
struct CityProp {
    float x = 0.f;
    float z = 0.f;
    float yaw = 0.f;
    std::uint32_t kind = 0; // 0 = streetlight（kStreetLightName）
};

struct CityCell {
    std::int32_t gx = 0;   // 网格索引
    std::int32_t gz = 0;
    std::uint32_t shade = 0; // 地面灰度档位 0..15（M-W0.0 种子敏感元，见文件头 RNG 纪律）
    std::uint32_t densityTier = 0; // M-W0.3：0=forest 1=low 2=medium 3=high
    std::vector<CityBuilding> buildings; // M-W0.2（block 局部坐标）
    std::vector<CityTree> trees;         // M-W0.3（block 局部坐标）
};

struct CityLayout {
    CityParams params;
    std::int32_t lo = 0; // 网格低索引（-size/2）
    std::vector<CityCell> cells;
    std::vector<CityCar> cars;   // M-W3.0（世界坐标，沿路网）
    std::vector<CityProp> props; // M-W3.0（世界坐标，沿路口）
    CityStats stats;
    std::uint32_t noiseSeed = 0; // M-W0.3：噪声种子（主流第 ③ 段消费的 u32）
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

// 材质分配（确定性，不消费 RNG）：tier 复用密度档（forest 归 tier1）。
int buildingTier(int densityTier) {
    return densityTier == 0 ? 1 : densityTier;
}

// ---------------------------------------------------------------------------
// M-W2.0 楼型池（W2-PLAN §2 拍板：A1 程序化楼型池 + B1 纯函数选型）。
// 变体原语：0=单盒（现状） 1=退台（下段全 footprint，上段 0.62 缩放居中）
//           2=裙楼塔（裙楼 0..0.30h 全 footprint，塔楼 0.55 缩放居中）
//           3=L 形（主体全宽×0.62 深；翼 0.55 宽×全深，x0 内缩 0.06m 消共面）
// 分档池（材质档 1..3 → 池，容量均 3）：tier1 低层住宅 {单盒,退台,L形}，
// tier2/3 {单盒,退台,裙楼塔}。选取 = fnv1a64(seed‖block‖building) % 3 纯函数
// （零 RNG 消费，M-W1.1 材质档同构）；裙楼在 h<6m 纯函数降级为退台（3m 楼配
// 0.9m 裙楼观感崩，降级判定与 RNG 无关）。
// bevy_city 对应（2026-11-13 实读留档）：其池 = 每密度档 Buildings{meshes,
// materials} + kenney building-type-N.glb 外部资产 + rng.random_range 选型；
// 本轨几何自研不走外部资产（kenney 色板图集前科 + W2-PLAN §2 A2 否决）。
// ---------------------------------------------------------------------------
constexpr std::uint32_t kVariantSingle = 0;
constexpr std::uint32_t kVariantSetback = 1;
constexpr std::uint32_t kVariantPodium = 2;
constexpr std::uint32_t kVariantLShape = 3;

// 池容量恒 3；kVariantPool[tier-1] = {V0, V1, V2 或 V3}。
constexpr std::uint32_t kVariantPool[3][3] = {
    {kVariantSingle, kVariantSetback, kVariantLShape}, // tier1
    {kVariantSingle, kVariantSetback, kVariantPodium}, // tier2
    {kVariantSingle, kVariantSetback, kVariantPodium}, // tier3
};

std::uint32_t buildingVariant(std::uint64_t seed, std::uint32_t blockIndex,
                              std::uint32_t buildingIndex, int matTier,
                              float height) {
    std::uint8_t buf[16];
    for (int i = 0; i < 8; ++i)
        buf[i] = static_cast<std::uint8_t>(seed >> (8 * i));
    for (int i = 0; i < 4; ++i) {
        buf[8 + i] = static_cast<std::uint8_t>(blockIndex >> (8 * i));
        buf[12 + i] = static_cast<std::uint8_t>(buildingIndex >> (8 * i));
    }
    std::uint64_t h = 14695981039346656037ULL;
    for (std::uint8_t v : buf) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    const int tierIdx = matTier < 1 ? 0 : (matTier > 3 ? 2 : matTier - 1);
    std::uint32_t picked = kVariantPool[tierIdx][h % 3u];
    // 裙楼降级：h < 6m 时塔楼段只剩 <4.2m，观感崩 → 纯函数改退台。
    if (picked == kVariantPodium && height < 6.0f)
        picked = kVariantSetback;
    return picked;
}

// ---------------------------------------------------------------------------
// M-W3.0 外部资产摆放（W3-PLAN 拍板 A1+B1+C1：drive 层装配 / city-tools 产
// 摆放数据 / 树+车+路灯试点）。名字表与 thirdparty/{kenney-nature-kit,
// kenney-car-kit, poly-streetlight}/ 一一对应（selftest 锁池界 + 覆盖；
// A1 拍板 city-tools 零资产依赖——只写名字不读文件，glb 本体由 drive 从
// vendored 目录拷进 assets/models/）。变体选取全部 fnv1a64 纯函数（零 RNG，
// M-W2.0 楼型同构），RNG 流契约 ①②③④ 逐字节不动。
// ---------------------------------------------------------------------------
constexpr std::uint32_t kTreeVariantCount = 12;
const char* kTreeVariantNames[kTreeVariantCount] = {
    "tree_blocks.glb",   "tree_cone.glb",    "tree_default.glb",
    "tree_detailed.glb", "tree_oak.glb",     "tree_palm.glb",
    "tree_pineDefaultA.glb", "tree_pineRoundA.glb", "tree_plateau.glb",
    "tree_simple.glb",   "tree_small.glb",   "tree_tall.glb",
};
constexpr std::uint32_t kCarVariantCount = 8;
const char* kCarVariantNames[kCarVariantCount] = {
    "sedan.glb", "sedan-sports.glb", "suv.glb", "taxi.glb",
    "police.glb", "van.glb", "truck.glb", "delivery.glb",
};
constexpr std::uint32_t kPropKindCount = 1;
const char* kPropKindNames[kPropKindCount] = {"streetlight.glb"};

// 通用纯函数选型：fnv1a64(seed ‖ tag ‖ idx)（与 buildingVariant 同构 16B
// 逐字节流）。tag = 域分隔（树 'tree' / 车 'car' / 街灯 'lite'）。
std::uint64_t assetVariantHash(std::uint64_t seed, std::uint64_t tag,
                               std::uint32_t idx) {
    std::uint8_t buf[20];
    for (int i = 0; i < 8; ++i)
        buf[i] = static_cast<std::uint8_t>(seed >> (8 * i));
    for (int i = 0; i < 8; ++i)
        buf[8 + i] = static_cast<std::uint8_t>(tag >> (8 * i));
    for (int i = 0; i < 4; ++i)
        buf[16 + i] = static_cast<std::uint8_t>(idx >> (8 * i));
    std::uint64_t h = 14695981039346656037ULL;
    for (std::uint8_t v : buf) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    return h;
}

// 选型 helper 定义在 M-W3.0 名字表段（buildingVariant 之后）；generateCity
// 尾部调用，buildRoadnet 定义在其后——前向声明。
void generateStreetAssets(CityLayout& lay);

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

    // M-W0.3 第三遍：noiseSeed（1 次 next，纯追加在楼体消费之后——② 段逐字节
    // 不动）+ 密度分区（噪声场，无 RNG 消费）。
    const std::uint32_t noiseSeed = rng.next();
    lay.noiseSeed = noiseSeed;
    SimplexNoise noise;
    noise.seed(noiseSeed);
    for (CityCell& cell : lay.cells) {
        const double d =
            noise.evaluate((static_cast<double>(cell.gx) + 0.5) * kBlockX * kNoiseScale,
                           (static_cast<double>(cell.gz) + 0.5) * kBlockZ * kNoiseScale) *
                0.5 +
            0.5;
        cell.densityTier = d < kDensityForest  ? 0u
                           : d < kDensityLow   ? 1u
                           : d < kDensityMedium ? 2u
                                                : 3u;
    }

    // M-W0.3 第四遍（流尾最后）：树。forest 每块 8 棵 / low 每块 4 棵，每棵消费
    // 2 次 below（先 dx 后 dz）；位置叶域内抖动（次数由密度决定——依赖噪声而
    // 噪声不依赖本段，无循环依赖）。
    for (CityCell& cell : lay.cells) {
        const std::uint32_t nt = cell.densityTier == 0u ? kTreesForest
                                 : cell.densityTier == 1u ? kTreesLow
                                                          : 0u;
        for (std::uint32_t k = 0; k < nt; ++k) {
            CityTree tr;
            tr.cx = (static_cast<float>(rng.below(45u)) - 22.f) * 0.1f; // [-2.2, 2.2]
            tr.cz = (static_cast<float>(rng.below(31u)) - 15.f) * 0.1f; // [-1.5, 1.5]
            cell.trees.push_back(tr);
        }
    }

    // M-W2.0 第五遍（零 RNG）：楼型变体回填。排在密度分区之后——变体选型读
    // 材质档（= 密度档）定池、读 height 做裙楼降级；fnv1a64 纯函数不碰 rng，
    // 位置放流尾之后对消费序契约无影响。
    for (std::size_t i = 0; i < lay.cells.size(); ++i) {
        CityCell& cell = lay.cells[i];
        const int matTier = buildingTier(static_cast<int>(cell.densityTier));
        for (std::uint32_t k = 0; k < cell.buildings.size(); ++k) {
            CityBuilding& bd = cell.buildings[k];
            bd.variant = buildingVariant(params.seed,
                                         static_cast<std::uint32_t>(i), k,
                                         matTier, bd.height);
        }
    }

    // M-W3.0 第六遍（零 RNG）：树型变体回填（全局树序 = cell 行主序 × 树序，
    // fnv1a64(seed‖'tree'‖globalIdx) % 12，W3-PLAN §2 B1 拍板）。
    {
        std::uint32_t treeIdx = 0;
        for (CityCell& cell : lay.cells) {
            for (CityTree& tr : cell.trees) {
                tr.variant = static_cast<std::uint32_t>(
                    assetVariantHash(params.seed, 0x74726565ULL /*"tree"*/,
                                     treeIdx) %
                    kTreeVariantCount);
                ++treeIdx;
            }
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
        lay.stats.trees += static_cast<std::uint32_t>(cell.trees.size()); // M-W0.3
        ++(cell.densityTier == 0u   ? lay.stats.forestCells
           : cell.densityTier == 1u ? lay.stats.lowCells
           : cell.densityTier == 2u ? lay.stats.mediumCells
                                    : lay.stats.highCells); // M-W0.3
    }
    // M-W3.0：cars/props 由 generateStreetAssets 填（内部重建路网拓扑；
    // 纯函数零 RNG，所有 generateCity 调用点自动一致）。fences/paths 保持 0：
    // 未消费（超出 M-W0 范围，挂零锚）。
    generateStreetAssets(lay);

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
    // kind 恒 "intersection"；signal = 进入边>=2（M-T4 起由 serializeRoadnet
    // 填充，消费端 TrafficControl 受控口径同构；W0-PLAN §1「citygen 恒 false」
    // 旧口径已废止——见 TRAFFIC-PLAN §12 补段）。
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

// ---------------------------------------------------------------------------
// M-W3.0 街道资产摆放（W3-PLAN 拍板 B1：city-tools 产 canonical 摆放数据，
// 零 RNG——全部 fnv1a64 纯函数）。generateCity 之后、serializeCityScene/
// serializeRoadnet 之前调用（依赖 buildRoadnet 拓扑）。
//
//   车：每有向边 hash(seed‖'car'‖edgeId) % 4 == 0 → 停 1 辆；沿边参数
//       t = 0.2 + (h>>8 % 61)/100 ∈ [0.2,0.8]（避开路口交叉区）；垂边偏移
//       0.3m（hash 选左右侧，路缘内停靠）；车型 hash % 8。期望密度
//       168 边 × 1/4 = 42 辆（size 6）。
//   路灯：每路口 1 盏；四角之一（hash % 4），偏移 (±0.65, ±0.65)（路带半宽
//       0.5 + 0.15 路缘外）。49 口（size 6）恒 49 盏。
// ---------------------------------------------------------------------------
void generateStreetAssets(CityLayout& lay) {
    const Roadnet net = buildRoadnet(lay);
    for (const RoadnetEdge& e : net.edges) {
        const std::uint64_t h =
            assetVariantHash(lay.params.seed, 0x63617200ULL /*"car\0"*/, e.id);
        if (h % 4u != 0u) continue;
        const RoadnetNode& na = net.nodes[e.from];
        const RoadnetNode& nb = net.nodes[e.to];
        const float t = 0.2f + static_cast<float>((h >> 8) % 61u) * 0.01f;
        const float dx = nb.x - na.x;
        const float dz = nb.z - na.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-4f) continue; // 不可能（grid 边恒 5.5/4.0m），防御
        CityCar c;
        c.x = na.x + dx * t + (-dz / len) * 0.3f;
        c.z = na.z + dz * t + (dx / len) * 0.3f;
        c.yaw = std::atan2(dx, dz); // 车头贴行驶方向（模型 -z 朝前口径）
        c.variant = static_cast<std::uint32_t>((h >> 16) % kCarVariantCount);
        c.edgeId = e.id;
        lay.cars.push_back(c);
    }
    for (const RoadnetNode& nd : net.nodes) {
        const std::uint64_t h = assetVariantHash(lay.params.seed,
                                                 0x6c697465ULL /*"lite"*/,
                                                 nd.id);
        static const float kCorner[4][2] = {{0.65f, 0.65f}, {-0.65f, 0.65f},
                                            {-0.65f, -0.65f}, {0.65f, -0.65f}};
        CityProp p;
        p.x = nd.x + kCorner[h % 4u][0];
        p.z = nd.z + kCorner[h % 4u][1];
        p.yaw = 0.f;
        p.kind = 0u;
        lay.props.push_back(p);
    }
    lay.stats.cars = static_cast<std::uint32_t>(lay.cars.size());
    lay.stats.props = static_cast<std::uint32_t>(lay.props.size());
}
std::string serializeRoadnet(const Roadnet& net) {
    std::string s;
    char buf[160];
    // M-T4（用户拍板「感应信号 citygen 填充 一起做吧」，TRAFFIC-PLAN §12 补）：
    // 进入边 >= 2 的节点 signal=true（与 TrafficControl 受控口径一致——端点
    // 死路节点进不了控制面，填 false）；时序走消费端 NodeControl 缺省
    //（8/2/1s 固定周期），signals[] 显式条目契约不变（只加不减）。
    std::vector<unsigned> inDeg(net.nodes.size(), 0u);
    for (const RoadnetEdge& e : net.edges)
        if (e.to < inDeg.size()) ++inDeg[e.to];
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
                      "\"kind\": \"intersection\", \"signal\": %s }%s\n",
                      n.id, n.x, n.z,
                      inDeg[n.id] >= 2u ? "true" : "false",
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
// ---------------------------------------------------------------------------
// M-W1.1 建筑贴图 —— ambientCG CC0 照片级立面纹理（用户拍板：kenney 实测为
// 色板图集无窗户图案后改道）。资产溯源 thirdparty/ambientcg-facades/VENDOR.md：
// tier1=Facade018A 砖墙白窗（低层住宅）/ tier2=Facade001 蓝灰幕墙窗格（中层
// 办公）/ tier3=Facade002 深灰幕墙暖黄亮窗（高层塔楼）。city-tools 零像素生
// 成：generate 把 exe 旁 textures/ 的入库 JPG 逐字节拷贝为产物贴图（构建期
// CMake POST_BUILD 拷贝到 $<TARGET_FILE_DIR>/textures/），材质分配 = 密度档
// 确定函数（forest 归 tier1），零 RNG 消费（M-W0 RNG 流契约不动）。
// ---------------------------------------------------------------------------
constexpr int kTexTierCount = 3; // tier1 low / tier2 medium / tier3 high

// 每档立面 UV 平铺单位（米/tile）：Facade 纹理一周期覆盖的世界尺寸，按图内
// 窗排数校准（tier1 砖墙约 4 层窗/tile ≈ 12m；tier2 幕墙细格 ≈ 8m；tier3
// 大格亮窗 ≈ 10m）。renderer 采样 Wrap 寻址，非整数跨度的半格窗被女儿墙
// 位置裁切属预期观感。
const float kWindowUnit[kTexTierCount] = {12.0f, 8.0f, 10.0f};

const char* kTexTierNames[kTexTierCount] = {"tier1", "tier2", "tier3"};

// 材质分配 buildingTier 定义在 generateCity 之前（M-W2.0 变体回填也消费它）。

// 产物贴图写出：从 srcDir（exe 旁 textures/，CMake 拷贝的入库资产）逐字节
// 拷贝 tier1/2/3.jpg 到 outDir（<out>/textures/）。JPG 不改字节（CC0 资产
// 原样再分发）。任一缺失/拷贝失败 = false。
bool writeCityTextures(const std::string& srcDir, const std::string& outDir) {
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) return false;
    for (int tier = 1; tier <= kTexTierCount; ++tier) {
        char name[32];
        std::snprintf(name, sizeof name, "tier%d.jpg", tier);
        const std::filesystem::path src =
            std::filesystem::path(srcDir) / name;
        const std::filesystem::path dst =
            std::filesystem::path(outDir) / name;
        std::ifstream in(src, std::ios::binary);
        if (!in) return false;
        std::ofstream out(dst, std::ios::binary);
        if (!out) return false;
        out << in.rdbuf();
        if (!out.good()) return false;
    }
    return true;
}

// city.mtl：3 个 newmtl（Kd 白，本色由贴图承载；map_Kd basename tierN.jpg ——
// obj2glb find_texture 按 basename 在 --maps 目录解析）。
std::string serializeCityMtl() {
    std::string s;
    s += "# city-tools M-W1.1 building material library (ambientCG CC0 facades,\n";
    s += "# see thirdparty/ambientcg-facades/VENDOR.md for provenance)\n";
    for (int tier = 1; tier <= kTexTierCount; ++tier) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "newmtl tier%d\nKd 1.000 1.000 1.000\nmap_Kd tier%d.jpg\n",
                      tier, tier);
        s += buf;
    }
    return s;
}

// M-W2.0 楼型几何展开（纯函数，楼型唯一真相源）：variant → 盒体段列表。
// 每段 = 一个 8 顶点盒（y0..y1）；所有段都落在原 footprint [cx±hx, cz±hz] 内
// （保 M-W0.2「块内两两不重叠」不变量）。L 形翼的 x0 内缩 0.06m 让翼的 -x 面
// 落进主体体积内（隐藏）而非与主体 -x 面共面（z-fighting）。
struct BoxPart { float cx, cz, hx, hz, y0, y1; };

int buildingParts(const CityBuilding& b, BoxPart* out) {
    int n = 0;
    if (b.variant == kVariantSetback) {
        const float ySplit = b.height * 0.55f;
        out[n++] = {b.cx, b.cz, b.hx, b.hz, 0.f, ySplit};
        out[n++] = {b.cx, b.cz, b.hx * 0.62f, b.hz * 0.62f, ySplit, b.height};
    } else if (b.variant == kVariantPodium) {
        const float yPod = b.height * 0.30f;
        out[n++] = {b.cx, b.cz, b.hx, b.hz, 0.f, yPod};
        out[n++] = {b.cx, b.cz, b.hx * 0.55f, b.hz * 0.55f, yPod, b.height};
    } else if (b.variant == kVariantLShape) {
        const float x0 = b.cx - b.hx;
        const float w = 2.f * b.hx;
        const float d = 2.f * b.hz;
        const float z0 = b.cz - b.hz;
        // 主体：全宽 × 0.62 深（z0 起步）。
        out[n++] = {b.cx, z0 + d * 0.31f, b.hx, d * 0.31f, 0.f, b.height};
        // 翼：0.55 宽 × 全深，x0 内缩 0.06m。
        const float wx0 = x0 + 0.06f;
        const float wx1 = x0 + w * 0.55f;
        out[n++] = {(wx0 + wx1) * 0.5f, b.cz, (wx1 - wx0) * 0.5f, b.hz,
                    0.f, b.height};
    } else { // kVariantSingle 与未知值兜底（单盒）
        out[n++] = {b.cx, b.cz, b.hx, b.hz, 0.f, b.height};
    }
    return n;
}

std::string serializeBlockObj(const CityCell& cell, std::uint32_t blockIndex,
                              std::uint64_t seed) {
    std::string s;
    char buf[128];
    std::snprintf(buf, sizeof buf,
                  "# city-tools M-W2.0 block mesh (block-local, y-up, meters)\n"
                  "# seed %llu block %u gx %d gz %d buildings %u\n"
                  "mtllib city.mtl\n",
                  static_cast<unsigned long long>(seed), blockIndex, cell.gx,
                  cell.gz, static_cast<unsigned>(cell.buildings.size()));
    s += buf;
    std::uint32_t base = 1;  // OBJ 顶点索引从 1 起
    std::uint32_t vtBase = 1; // vt 索引同从 1 起
    for (std::size_t bi = 0; bi < cell.buildings.size(); ++bi) {
        const CityBuilding& b = cell.buildings[bi];
        const int matTier = buildingTier(static_cast<int>(cell.densityTier));
        std::snprintf(buf, sizeof buf, "o b%u\nusemtl tier%d\n",
                      static_cast<unsigned>(bi), matTier);
        s += buf;
        BoxPart parts[4];
        const int np = buildingParts(b, parts);
        const float unit = kWindowUnit[matTier - 1];
        for (int pi = 0; pi < np; ++pi) {
            const BoxPart& p = parts[pi];
            const float hx = p.hx, hz = p.hz;
            const float vx[8] = {p.cx - hx, p.cx + hx, p.cx + hx, p.cx - hx,
                                 p.cx - hx, p.cx + hx, p.cx + hx, p.cx - hx};
            const float vy[8] = {p.y0, p.y0, p.y0, p.y0,
                                 p.y1, p.y1, p.y1, p.y1};
            const float vz[8] = {p.cz - hz, p.cz - hz, p.cz + hz, p.cz + hz,
                                 p.cz - hz, p.cz - hz, p.cz + hz, p.cz + hz};
            for (int i = 0; i < 8; ++i) {
                std::snprintf(buf, sizeof buf, "v %.3f %.3f %.3f\n", vx[i], vy[i],
                              vz[i]);
                s += buf;
            }
            // 每 quad 4 vt：(u0,v0)(u1,v0)(u1,v1)(u0,v1)。立面按本档世界尺寸
            // 平铺（kWindowUnit[matTier-1] 米/tile）；顶/底面收缩到纹理左下角
            // 微区域（0.01~0.02，采样恒定像素色，不占立面图案区域）。段立面
            // v 跨度 = 段净高/unit（退台/裙楼各段独立计高）。
            const float eu = 2.f * hx / unit; // z 面 u 跨度
            const float ez = 2.f * hz / unit; // x 面 u 跨度
            const float ev = (p.y1 - p.y0) / unit; // 立面 v 跨度
            struct FaceQuad { int quad[4]; float u0, v0, u1, v1; };
            const FaceQuad faces[6] = {
                {{0, 1, 2, 3}, 0.01f, 0.01f, 0.02f, 0.02f},  // 底 -y
                {{4, 5, 6, 7}, 0.01f, 0.01f, 0.02f, 0.02f},  // 顶 +y
                {{0, 1, 5, 4}, 0.f,   0.f,   eu,   ev  },    // z-
                {{2, 3, 7, 6}, 0.f,   0.f,   eu,   ev  },    // z+
                {{1, 2, 6, 5}, 0.f,   0.f,   ez,   ev  },    // x+
                {{0, 3, 7, 4}, 0.f,   0.f,   ez,   ev  },    // x-
            };
            for (const FaceQuad& fq : faces) {
                for (int qi = 0; qi < 4; ++qi) {
                    const float u = qi == 0 || qi == 3 ? fq.u0 : fq.u1;
                    const float v = qi == 0 || qi == 1 ? fq.v0 : fq.v1;
                    std::snprintf(buf, sizeof buf, "vt %.3f %.3f\n", u, v);
                    s += buf;
                }
            }
            // 两三角/面，顶点 local idx -> quad 内 vt 位移（保持 M-W0.2 外向
            // 绕序，vt 与顶点一一对应）。
            static const int kTriVt[12][3] = {
                {0, 2, 3}, {0, 1, 2},   // 底：quad 0,1,2,3
                {0, 2, 1}, {0, 3, 2},   // 顶：quad 4,5,6,7
                {0, 2, 1}, {0, 3, 2},   // z-：quad 0,1,5,4
                {0, 2, 1}, {0, 3, 2},   // z+：quad 2,3,7,6
                {0, 2, 1}, {0, 3, 2},   // x+：quad 1,2,6,5
                {0, 2, 1}, {0, 1, 2},   // x-：quad 0,3,7,4
            };
            // 每面 quad vt 基 = vtBase + face*4；三角 kTriVt[f] 给出 quad 内位移。
            static const std::uint32_t kTris[12][3] = {
                {1, 3, 4}, {1, 2, 3},   // 底面 (-y)
                {5, 8, 7}, {5, 7, 6},   // 顶面 (+y)
                {1, 6, 2}, {1, 5, 6},   // z- 面
                {3, 8, 4}, {3, 7, 8},   // z+ 面
                {2, 7, 3}, {2, 6, 7},   // x+ 面
                {1, 8, 5}, {1, 4, 8},   // x- 面
            };
            for (int fi = 0; fi < 12; ++fi) {
                const std::uint32_t vb =
                    vtBase + static_cast<std::uint32_t>(fi / 2) * 4u;
                std::snprintf(buf, sizeof buf, "f %u/%u %u/%u %u/%u\n",
                              base + kTris[fi][0] - 1,
                              vb + static_cast<std::uint32_t>(kTriVt[fi][0]),
                              base + kTris[fi][1] - 1,
                              vb + static_cast<std::uint32_t>(kTriVt[fi][1]),
                              base + kTris[fi][2] - 1,
                              vb + static_cast<std::uint32_t>(kTriVt[fi][2]));
                s += buf;
            }
            base += 8;
            vtBase += 24;
        }
    }
    return s;
}

// city.scene（E1 World JSON，M-E2 World::serialise 形态；语义同构、手写固定
// 序确定性文本——citygen 产物，非 ECS roundtrip dump，键序不锁字母序）。
// 实体 = 每 block 一个（行主序），Transform 位置 = block 世界中心，
// Model.mesh 引用 blocks/block_XX.obj（OBJ 中间件，通道就绪后由转换步替换）。
std::string serializeCityScene(const CityLayout& lay) {
    // buf 必须容纳整段实体文本（~280B）：M-W1.0 实锤 buf[192] 在
    // "position": [%.2f 处截断，产出的 city.scene 是非法 JSON（36 实体全伤，
    // 截断确定性导致 fc 逐字节段与 findstr 锚全绿、无人发现——viewer M-W1.1
    // 才成为第一个 JSON 解析消费者）。M-W3.0 树/车/路灯实体 ~240B，512 富余。
    std::string s;
    char buf[512];
    const std::uint32_t n = static_cast<std::uint32_t>(lay.cells.size());
    const std::uint32_t treeTotal = lay.stats.trees;
    const std::uint32_t carTotal = static_cast<std::uint32_t>(lay.cars.size());
    const std::uint32_t propTotal =
        static_cast<std::uint32_t>(lay.props.size());
    const std::uint32_t total = n + treeTotal + carTotal + propTotal;
    s += "{\n";
    s += "  \"version\": 1,\n";
    std::snprintf(buf, sizeof buf, "  \"nextID\": %u,\n", total + 1u);
    s += buf;
    s += "  \"entities\": [\n";
    std::uint32_t id = 1;
    const char* sep = ",";
    for (std::uint32_t i = 0; i < n; ++i) {
        const CityCell& c = lay.cells[i];
        const float cx =
            (static_cast<float>(c.gx) + 0.5f) * kBlockX; // block 世界中心
        const float cz = (static_cast<float>(c.gz) + 0.5f) * kBlockZ;
        std::snprintf(buf, sizeof buf,
                      "    { \"id\": %u, \"name\": \"block_%02u\", \"flags\": 0, "
                      "\"parent\": 0, \"components\": {\n"
                      "      \"Model\": { \"mesh\": \"blocks/block_%02u.obj\", "
                      "\"tint\": [1.0, 1.0, 1.0] },\n"
                      "      \"Transform\": { \"position\": [%.2f, 0.0, %.2f], "
                      "\"scale\": [1.0, 1.0, 1.0], "
                      "\"rotation\": [0.0, 0.0, 0.0, 1.0] }\n"
                      "    } }%s\n",
                      id, i, i, cx, cz, i + 1 == n && carTotal == 0u &&
                              propTotal == 0u && treeTotal == 0u
                          ? "" : sep);
        s += buf;
        ++id;
    }
    // M-W3.0 树实体（mesh = kTreeVariantNames[variant] 扁平白名单名；drive
    // 从 vendored 目录拷入 assets/models/，A1 拍板）。位置 = cell 中心 +
    // block 局部树心（树高度下探为负？kenney 树原点在根底，y=0 落地）。
    std::uint32_t treeId = 0;
    for (std::size_t ci = 0; ci < lay.cells.size(); ++ci) {
        const CityCell& c = lay.cells[ci];
        const float cx = (static_cast<float>(c.gx) + 0.5f) * kBlockX;
        const float cz = (static_cast<float>(c.gz) + 0.5f) * kBlockZ;
        for (const CityTree& tr : c.trees) {
            const bool last =
                treeId + 1u == treeTotal && carTotal == 0u && propTotal == 0u;
            std::snprintf(buf, sizeof buf,
                          "    { \"id\": %u, \"name\": \"tree_%02u\", "
                          "\"flags\": 0, \"parent\": 0, \"components\": {\n"
                          "      \"Model\": { \"mesh\": \"%s\", "
                          "\"tint\": [1.0, 1.0, 1.0] },\n"
                          "      \"Transform\": { \"position\": [%.2f, 0.0, "
                          "%.2f], \"scale\": [1.0, 1.0, 1.0], "
                          "\"rotation\": [0.0, 0.0, 0.0, 1.0] }\n"
                          "    } }%s\n",
                          id, treeId, kTreeVariantNames[tr.variant],
                          cx + tr.cx, cz + tr.cz, last ? "" : ",");
            s += buf;
            ++id;
            ++treeId;
        }
    }
    // M-W3.0 车实体（rotation = 绕 Y 轴四元数 [0, sin(yaw/2), 0, cos(yaw/2)]；
    // 边停靠朝向）。
    for (std::uint32_t i = 0; i < carTotal; ++i) {
        const CityCar& c = lay.cars[i];
        const bool last = i + 1u == carTotal && propTotal == 0u;
        const float hy = c.yaw * 0.5f;
        std::snprintf(buf, sizeof buf,
                      "    { \"id\": %u, \"name\": \"car_%02u\", "
                      "\"flags\": 0, \"parent\": 0, \"components\": {\n"
                      "      \"Model\": { \"mesh\": \"%s\", "
                      "\"tint\": [1.0, 1.0, 1.0] },\n"
                      "      \"Transform\": { \"position\": [%.2f, 0.0, %.2f], "
                      "\"scale\": [1.0, 1.0, 1.0], "
                      "\"rotation\": [0.0, %.4f, 0.0, %.4f] }\n"
                      "    } }%s\n",
                      id, i, kCarVariantNames[c.variant], c.x, c.z,
                      std::sin(hy), std::cos(hy), last ? "" : ",");
        s += buf;
        ++id;
    }
    // M-W3.0 街具实体（kind 0 = streetlight）。
    for (std::uint32_t i = 0; i < propTotal; ++i) {
        const CityProp& p = lay.props[i];
        const bool last = i + 1u == propTotal;
        const float hy = p.yaw * 0.5f;
        std::snprintf(buf, sizeof buf,
                      "    { \"id\": %u, \"name\": \"streetlight_%02u\", "
                      "\"flags\": 0, \"parent\": 0, \"components\": {\n"
                      "      \"Model\": { \"mesh\": \"%s\", "
                      "\"tint\": [1.0, 1.0, 1.0] },\n"
                      "      \"Transform\": { \"position\": [%.2f, 0.0, %.2f], "
                      "\"scale\": [1.0, 1.0, 1.0], "
                      "\"rotation\": [0.0, %.4f, 0.0, %.4f] }\n"
                      "    } }%s\n",
                      id, i, kPropKindNames[p.kind], p.x, p.z,
                      std::sin(hy), std::cos(hy), last ? "" : ",");
        s += buf;
        ++id;
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
    appendU32(b, lay.stats.forestCells); // M-W0.3
    appendU32(b, lay.stats.lowCells);
    appendU32(b, lay.stats.mediumCells);
    appendU32(b, lay.stats.highCells);
    appendU32(b, lay.stats.cars);
    appendU32(b, lay.stats.fences);
    appendU32(b, lay.stats.paths);
    appendU32(b, lay.noiseSeed); // M-W0.3
    for (const CityCell& c : lay.cells) {
        appendI32(b, c.gx);
        appendI32(b, c.gz);
        b.push_back(static_cast<std::uint8_t>(c.shade));
        b.push_back(static_cast<std::uint8_t>(c.densityTier)); // M-W0.3
        // M-W0.2：楼体数据进规范序列化（float 按 IEEE 位原样写入，确定性）。
        // M-W2.0：variant 追加在 5 个 float 之后（规范字节格式变更，确定性锚
        // 全部动态比对，无硬编码 hash 波及）。
        appendU32(b, static_cast<std::uint32_t>(c.buildings.size()));
        for (const CityBuilding& bd : c.buildings) {
            float fields[5] = {bd.cx, bd.cz, bd.hx, bd.hz, bd.height};
            for (float f : fields) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, 4);
                appendU32(b, bits);
            }
            appendU32(b, bd.variant); // M-W2.0
        }
        // M-W0.3：树位置进规范序列化（M-W3.0 增 variant）。
        appendU32(b, static_cast<std::uint32_t>(c.trees.size()));
        for (const CityTree& tr : c.trees) {
            float fields[2] = {tr.cx, tr.cz};
            for (float f : fields) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, 4);
                appendU32(b, bits);
            }
            appendU32(b, tr.variant); // M-W3.0
        }
    }
    // M-W3.0：车段（沿路网停靠，世界坐标）。
    appendU32(b, static_cast<std::uint32_t>(lay.cars.size()));
    for (const CityCar& c : lay.cars) {
        float fields[3] = {c.x, c.z, c.yaw};
        for (float f : fields) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, 4);
            appendU32(b, bits);
        }
        appendU32(b, c.variant);
        appendU32(b, c.edgeId);
    }
    // M-W3.0：街具段（路口路灯）。
    appendU32(b, static_cast<std::uint32_t>(lay.props.size()));
    for (const CityProp& p : lay.props) {
        float fields[3] = {p.x, p.z, p.yaw};
        for (float f : fields) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, 4);
            appendU32(b, bits);
        }
        appendU32(b, p.kind);
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

    // block 内区（cell 减路带：4.5m x 3.0m）。
    // M-W0.3 密度基调四档（bevy 地面材质分档同构：forest 绿 / low 浅 / medium 中
    // / high 深）+ shade 微抖（每通道 -=shade，M-W0.0 种子敏感元视觉作用保留）。
    // 路带以网格线为中心占半宽（kRoadWidth*0.5），内区从路带边缘起算。
    for (const CityCell& c : lay.cells) {
        const float cx = static_cast<float>(c.gx) * kBlockX;
        const float cz = static_cast<float>(c.gz) * kBlockZ;
        std::uint8_t br, bg, bb2;
        if (c.densityTier == 0u) { br = 150; bg = 184; bb2 = 150; }      // forest
        else if (c.densityTier == 1u) { br = 222; bg = 226; bb2 = 214; } // low
        else if (c.densityTier == 2u) { br = 202; bg = 208; bb2 = 200; } // medium
        else { br = 182; bg = 188; bb2 = 184; }                          // high
        const std::uint8_t vr = static_cast<std::uint8_t>(br - c.shade);
        const std::uint8_t vg = static_cast<std::uint8_t>(bg - c.shade);
        const std::uint8_t vb = static_cast<std::uint8_t>(bb2 - c.shade);
        const float halfRoad = kRoadWidth * 0.5f;
        fillRect(img, w, h,
                 toPx(cx + halfRoad, lay.minX), toPx(cz + halfRoad, lay.minZ),
                 toPx(cx + kBlockX - halfRoad, lay.minX),
                 toPx(cz + kBlockZ - halfRoad, lay.minZ), vr, vg, vb);
    }

    // M-W0.3 树（俯视标记层）：3x3 深绿块，画在楼之下（楼是主体）。
    for (const CityCell& c : lay.cells) {
        const float ccx = (static_cast<float>(c.gx) + 0.5f) * kBlockX;
        const float ccz = (static_cast<float>(c.gz) + 0.5f) * kBlockZ;
        for (const CityTree& tr : c.trees) {
            fillRect(img, w, h, toPx(ccx + tr.cx - 0.15f, lay.minX),
                     toPx(ccz + tr.cz - 0.15f, lay.minZ),
                     toPx(ccx + tr.cx + 0.15f, lay.minX),
                     toPx(ccz + tr.cz + 0.15f, lay.minZ), 60, 110, 60);
        }
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

    // M-W0.3：异 seed 密度分布必异（seed 12346 = 19/4/4/9，Python 复算回填）。
    {
        const CityLayout layD = generateCity(p1);
        expect(layD.noiseSeed == 3832654775u,
               "noise: noiseSeed(12346,6) == 3832654775 (Python reference)");
        expect(layD.stats.forestCells == 19u && layD.stats.lowCells == 4u &&
                   layD.stats.mediumCells == 4u && layD.stats.highCells == 9u &&
                   layD.stats.trees == 168u,
               "noise: density distribution differs across seeds (19/4/4/9, 168 trees)");
    }
    // M-W0.3：size 1 边界（lo = -(1/2) = 0 截断口径 → cell(0,0) 中心 d=0.3125
    // → forest 档 8 树；Python 参考复算曾因 floor 除法 lo=-1 错位，已勘误回填）。
    {
        CityParams psz = p0;
        psz.size = 1;
        const CityLayout laySz = generateCity(psz);
        expect(laySz.lo == 0,
               "layout: size 1 truncating lo == 0 (not floor division)");
        expect(laySz.stats.forestCells == 1u && laySz.stats.lowCells == 0u &&
                   laySz.stats.mediumCells == 0u && laySz.stats.highCells == 0u &&
                   laySz.stats.trees == 8u && laySz.noiseSeed == 1603148953u,
               "noise: size 1 boundary (forest tier, 8 trees, noiseSeed anchor)");
    }

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
    expect(lay.stats.trees == 188u, "stats: trees == 188 (forest 21*8 + low 5*4, M-W0.3)");
    // M-W3.0 改写：cars 由挂零锚变为街车计数（42 = 168 边 × 1/4 命率，期望
    // 值恰中——纯函数分布锚）；fences/paths 仍挂零（未消费）。
    expect(lay.stats.cars == 42u && lay.stats.fences == 0u &&
               lay.stats.paths == 0u && lay.stats.props == 49u,
           "stats: cars=42 (168 edges x 1/4 hit, M-W3.0) props=49 "
           "(one per crossroads) fences/paths still zero");
    expect(lay.stats.forestCells == 21u && lay.stats.lowCells == 5u &&
               lay.stats.mediumCells == 6u && lay.stats.highCells == 4u,
           "stats: density tiers 21/5/6/4 (Python reference cross-check, M-W0.3)");
    expect(lay.stats.forestCells + lay.stats.lowCells +
                   lay.stats.mediumCells + lay.stats.highCells ==
               lay.stats.blocks,
           "stats: density tier counts sum to blocks (36)");
    expect(lay.noiseSeed == 2109909543u,
           "noise: noiseSeed(12345,6) == 2109909543 (Python reference, stream segment 3)");

    // --- M-W0.3 噪声统计不变量（Q11-1 拍板 A 口径：统计锚 + 已知答案）---
    {
        SimplexNoise nz;
        nz.seed(lay.noiseSeed);
        double sum = 0.0;
        double mx = 0.0;
        int cnt = 0;
        for (int i = -64; i <= 64; ++i) {
            for (int j = -64; j <= 64; ++j) {
                const float v = nz.evaluate(i * 0.05, j * 0.05);
                sum += v;
                mx = std::max(mx, static_cast<double>(std::fabs(v)));
                ++cnt;
            }
        }
        expect(mx <= 1.0, "noise: |v| <= 1.0 over 129x129 grid (norm margin 13%)");
        expect(std::fabs(sum / cnt) < 0.35,
               "noise: mean near zero over grid (simplex balance)");
        // 已知答案（Python 参考实现独立复算回填，noiseSeed 2109909543）
        const float e1 = nz.evaluate(0.3, 0.7);
        const float e2 = nz.evaluate(3.125, -1.75);
        const float e3 = nz.evaluate(2.05, -0.85);
        expect(e1 > 0.4539f && e1 < 0.4541f,
               "noise: eval(0.3,0.7) == 0.454030110 (known answer)");
        expect(e2 > 0.0091f && e2 < 0.0092f,
               "noise: eval(3.125,-1.75) == 0.009114384 (known answer)");
        expect(e3 > -0.5214f && e3 < -0.5212f,
               "noise: eval(2.05,-0.85) == -0.521328681 (known answer)");
        expect(nz.evaluate(0.0, 0.0) == 0.0f, "noise: eval(0,0) == 0 (symmetry origin)");
        // perm 头锚（Python make_perm 独立复算）
        expect(nz.perm[0] == 182 && nz.perm[1] == 218 && nz.perm[2] == 150 &&
                   nz.perm[3] == 255,
               "noise: perm head (182,218,150,255) for noiseSeed (known answer)");
    }

    // --- M-W0.3 树锚（位置 = 流尾第 ④ 段消费）---
    {
        // 首块（gx=-3,gz=-2，forest 档）首棵树 (2.1, 0.5)（Python 复算回填）。
        const CityCell& first = lay.cells[1]; // 行主序 [0]=(−3,−3) high 档，[1]=(−3,−2) forest
        const bool firstForest = first.densityTier == 0u;
        expect(firstForest && first.gx == -3 && first.gz == -2 && first.trees.size() == 8u,
               "trees: cell(−3,−2) is forest with 8 trees (M-W0.3)");
        if (firstForest && first.trees.size() == 8u) {
            const auto near1 = [](float a, float b) {
                return std::fabs(a - b) < 0.001f;
            };
            expect(near1(first.trees[0].cx, 2.1f) && near1(first.trees[0].cz, 0.5f),
                   "trees: first tree position (2.1, 0.5) (Python reference, fp tolerance)");
        }
        // 树数公式：trees == forestCells*8 + lowCells*4
        expect(lay.stats.trees ==
                   lay.stats.forestCells * 8u + lay.stats.lowCells * 4u,
               "trees: count formula forest*8 + low*4 == trees");
    }

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

    // --- M-W2.0 楼型变体（W2-PLAN §2：FNV-1a 纯函数选型，零 RNG 消费）---
    {
        // ① 池界 + 分档约束：variant ∈ 该材质档池（tier1 不产裙楼、tier2/3 不产 L 形）。
        bool poolOk = true;
        for (const CityCell& cl : lay.cells) {
            const int mt = buildingTier(static_cast<int>(cl.densityTier));
            for (const CityBuilding& bd : cl.buildings) {
                const std::uint32_t v = bd.variant;
                const bool ok = mt == 1 ? (v == kVariantSingle ||
                                           v == kVariantSetback ||
                                           v == kVariantLShape)
                                        : (v == kVariantSingle ||
                                           v == kVariantSetback ||
                                           v == kVariantPodium);
                poolOk = poolOk && ok;
            }
        }
        expect(poolOk, "variant: all buildings inside their tier pool (M-W2.0)");

        // ② 覆盖度：四种变体同城至少各一次（144 楼期望分布 单盒≈48/退台≈48/裙楼≈9/L≈34）。
        bool seen[4] = {false, false, false, false};
        for (const CityCell& cl : lay.cells)
            for (const CityBuilding& bd : cl.buildings)
                seen[bd.variant < 4u ? bd.variant : 0u] = true;
        expect(seen[0] && seen[1] && seen[2] && seen[3],
               "variant: all four shapes occur in the seed-12345 city (M-W2.0)");

        // ③ seed 敏感：异 seed 变体分配必异（serializeLayout 已含 variant，此处直查）。
        const CityLayout layV = generateCity(p1);
        bool variantDiff = false;
        for (std::size_t i = 0; i < lay.cells.size() && !variantDiff; ++i) {
            for (std::size_t k = 0; k < lay.cells[i].buildings.size(); ++k) {
                if (lay.cells[i].buildings[k].variant !=
                    layV.cells[i].buildings[k].variant) {
                    variantDiff = true;
                    break;
                }
            }
        }
        expect(variantDiff,
               "variant: different seed -> different assignment (M-W2.0)");

        // ④ 裙楼降级：h < 6m 无裙楼（纯函数降级，非 RNG）。
        bool podiumGuard = true;
        for (const CityCell& cl : lay.cells)
            for (const CityBuilding& bd : cl.buildings)
                if (bd.variant == kVariantPodium && bd.height < 6.f)
                    podiumGuard = false;
        expect(podiumGuard,
               "variant: podium never picked below 6m height (pure downgrade)");

        // ⑤ OBJ part 展开守恒：v 行 = Σparts*8，vt 行 = Σparts*24，f 行 = Σparts*12
        // （全城 36 block 逐份核对）。
        bool objOk = true;
        for (std::size_t i = 0; i < lay.cells.size(); ++i) {
            const std::string obj = serializeBlockObj(
                lay.cells[i], static_cast<std::uint32_t>(i), p0.seed);
            std::int64_t nv = 0, nvt = 0, nf = 0;
            for (const char* p = obj.c_str(); *p != '\0';) {
                if (p[0] == 'v' && p[1] == ' ') ++nv;
                else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') ++nvt;
                else if (p[0] == 'f' && p[1] == ' ') ++nf;
                while (*p != '\0' && *p != '\n') ++p;
                if (*p == '\n') ++p;
            }
            std::int64_t expV = 0, expVt = 0, expF = 0;
            for (const CityBuilding& bd : lay.cells[i].buildings) {
                BoxPart parts[4];
                const int np = buildingParts(bd, parts);
                expV += static_cast<std::int64_t>(np) * 8;
                expVt += static_cast<std::int64_t>(np) * 24;
                expF += static_cast<std::int64_t>(np) * 12;
            }
            objOk = objOk && nv == expV && nvt == expVt && nf == expF;
        }
        expect(objOk,
               "variant: OBJ part expansion v/vt/f conservation over 36 blocks (M-W2.0)");

        // ⑥ part 落原 footprint 内（保 M-W0.2「块内两两不重叠」不变量）。
        bool containOk = true;
        for (const CityCell& cl : lay.cells) {
            for (const CityBuilding& bd : cl.buildings) {
                BoxPart parts[4];
                const int np = buildingParts(bd, parts);
                for (int pi = 0; pi < np; ++pi) {
                    const BoxPart& q = parts[pi];
                    const bool in = q.cx - q.hx >= bd.cx - bd.hx - 1e-4f &&
                                    q.cx + q.hx <= bd.cx + bd.hx + 1e-4f &&
                                    q.cz - q.hz >= bd.cz - bd.hz - 1e-4f &&
                                    q.cz + q.hz <= bd.cz + bd.hz + 1e-4f &&
                                    q.y0 >= -1e-4f && q.y1 <= bd.height + 1e-4f;
                    containOk = containOk && in;
                }
            }
        }
        expect(containOk,
               "variant: all parts within original footprint (non-overlap invariant)");
    }

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

    // M-T4：signal 填充锚——进入边 >= 2 的节点 signal=true，端点（进入边 < 2）
    // false；与 TrafficControl 受控口径同构。size 6 网格：角 2 / 边 3 / 内 4
    // 条进入边 -> 全部 49 节点受控（viewer 灯数 = Σ进入边 = 168）。
    {
        std::uint32_t sigTrue = 0, sigFalse = 0;
        std::size_t pos = 0;
        const std::string kt = "\"signal\": true";
        const std::string kf = "\"signal\": false";
        while ((pos = jsonA.find(kt, pos)) != std::string::npos) {
            ++sigTrue;
            pos += kt.size();
        }
        pos = 0;
        while ((pos = jsonA.find(kf, pos)) != std::string::npos) {
            ++sigFalse;
            pos += kf.size();
        }
        std::uint32_t inDeg2 = 0;
        std::vector<unsigned> deg(net.nodes.size(), 0u);
        for (const RoadnetEdge& e : net.edges)
            if (e.to < deg.size()) ++deg[e.to];
        for (unsigned v : deg)
            if (v >= 2u) ++inDeg2;
        expect(sigTrue == inDeg2 && sigTrue + sigFalse == 49u &&
                   sigTrue == 49u,
               "roadnet: signal fill = in-degree>=2 nodes (49 true / 0 false)");
    }

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
        // M-W0.3：完整重建 ①② 段（36 shade + 288 楼体档位）后紧接的 next()
        // 即 noiseSeed —— ③ 段位置锁定，①② 消费值逐字节不动。
        const CityLayout layFull = generateCity(p0);
        Pcg32 pr3;
        pr3.seed(p0.seed);
        for (std::uint32_t i = 0; i < p0.size * p0.size; ++i) pr3.below(16u);
        for (const CityCell& cl : layFull.cells) {
            for (const CityBuilding& bd : cl.buildings) {
                (void)bd; // 消费序与档位值无关：每楼固定 below(3)+below(2)
                pr3.below(3u);
                pr3.below(2u);
            }
        }
        expect(pr3.next() == layFull.noiseSeed,
               "rng: noiseSeed is segment 3 (rebuilt stream next() matches)");
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

    // OBJ：每块 4 objects，v/f/vt 行数 = buildingParts 动态求和（M-W2.0 变体
    // 楼型 part 数不再恒 1）；双跑逐字节、异 seed 必异。
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
        std::uint64_t expV = 0, expF = 0;
        for (const CityBuilding& bd : lay.cells[0].buildings) {
            BoxPart parts[4];
            const int np = buildingParts(bd, parts);
            expV += static_cast<std::uint64_t>(np) * 8;
            expF += static_cast<std::uint64_t>(np) * 12;
        }
        expect(vCount == expV && fCount == expF && oCount == 4u,
               "obj: 4 buildings -> parts-expanded verts/tris / 4 objects (M-W2.0)");
        expect(serializeBlockObj(generateCity(p1).cells[0], 0, p1.seed) != objA,
               "obj: different seed -> different bytes (tier draws)");
        // M-W1.1 纹理化锚（M-W2.0 行数按 part 展开）：vt 每 part 24（6 面 x
        // 4 角）、usemtl 每 building 1 且全部命中合法材质名、f 行必须带 vt 索引。
        std::uint32_t vtCount = 0, usemtlCount = 0, fWithVt = 0;
        for (std::size_t pos = objA.find("\nvt ");
             pos != std::string::npos; pos = objA.find("\nvt ", pos + 1)) ++vtCount;
        for (std::size_t pos = objA.find("\nusemtl ");
             pos != std::string::npos; pos = objA.find("\nusemtl ", pos + 1)) ++usemtlCount;
        for (std::size_t pos = objA.find("\nf ");
             pos != std::string::npos; pos = objA.find("\nf ", pos + 1)) {
            if (objA.compare(pos + 2, 2, "f ") == 0) {} // unreachable guard
            // f 行形态 "f v/vt v/vt v/vt"：数 '/' 出现 3 次即带 vt。
            std::size_t e = objA.find('\n', pos + 1);
            const std::string line = objA.substr(pos + 1, e - pos - 1);
            int slashes = 0;
            for (char ch : line) {
                if (ch == '/') ++slashes;
            }
            if (slashes == 3) ++fWithVt;
        }
        expect(vtCount == expV * 3 && usemtlCount == 4u && fWithVt == expF,
               "obj: parts-expanded vt / 4 usemtl / f-with-vt (M-W2.0)");
        expect(objA.find("mtllib city.mtl") != std::string::npos,
               "obj: mtllib city.mtl present");
        bool matNamesOk = true;
        for (std::size_t pos = objA.find("\nusemtl ");
             pos != std::string::npos && matNamesOk;
             pos = objA.find("\nusemtl ", pos + 1)) {
            const std::size_t e = objA.find('\n', pos + 1);
            const std::string mat = objA.substr(pos + 8, e - pos - 8);
            matNamesOk = mat == "tier1" || mat == "tier2" || mat == "tier3";
        }
        expect(matNamesOk, "obj: every usemtl resolves to tier1/2/3");
    }

    // MTL：3 newmtl + 3 map_Kd（basename tierN.jpg）。
    {
        const std::string mtl = serializeCityMtl();
        std::uint32_t mCount = 0, mapCount = 0;
        for (std::size_t pos = mtl.find("newmtl ");
             pos != std::string::npos; pos = mtl.find("newmtl ", pos + 1)) ++mCount;
        for (std::size_t pos = mtl.find("map_Kd ");
             pos != std::string::npos; pos = mtl.find("map_Kd ", pos + 1)) ++mapCount;
        expect(mCount == 3u && mapCount == 3u,
               "mtl: 3 materials with 3 map_Kd entries");
        expect(mtl.find("map_Kd tier1.jpg") != std::string::npos &&
                   mtl.find("map_Kd tier3.jpg") != std::string::npos,
               "mtl: map_Kd basenames follow tier<1..3>.jpg naming");
    }

    // 贴图拷贝：入库 JPG 存在 + JPEG SOI 头 + 双跑逐字节一致（不改字节）。
    {
        std::error_code ec;
        std::filesystem::path src = std::filesystem::temp_directory_path(ec) /
                                    "city-tools-tex-selftest-src";
        std::filesystem::path dst = std::filesystem::temp_directory_path(ec) /
                                    "city-tools-tex-selftest-dst";
        std::filesystem::create_directories(src / "textures", ec);
        std::filesystem::remove_all(dst, ec);
        // 伪造源（selftest 不依赖仓库资产布局）：最小 JPEG 头字节。
        const std::uint8_t jpegProbe[4] = {0xFF, 0xD8, 0xFF, 0xE0};
        for (int tier = 1; tier <= kTexTierCount; ++tier) {
            char name[32];
            std::snprintf(name, sizeof name, "tier%d.jpg", tier);
            FILE* f = std::fopen((src / "textures" / name).string().c_str(), "wb");
            if (f != nullptr) {
                std::fwrite(jpegProbe, 1, 4, f);
                std::fclose(f);
            }
        }
        expect(writeCityTextures((src / "textures").string(), dst.string()),
               "tex: 3 jpg copied from asset dir to output dir");
        bool bytesOk = true, soiOk = true;
        for (int tier = 1; tier <= kTexTierCount && bytesOk; ++tier) {
            char name[32];
            std::snprintf(name, sizeof name, "tier%d.jpg", tier);
            const std::filesystem::path p = dst / name;
            const auto sz1 = std::filesystem::file_size(p, ec);
            writeCityTextures((src / "textures").string(), dst.string());
            const auto sz2 = std::filesystem::file_size(p, ec);
            if (ec || sz1 != 4u || sz2 != sz1) bytesOk = false;
            std::uint8_t head[2] = {};
            FILE* f = std::fopen(p.string().c_str(), "rb");
            if (f != nullptr) {
                const std::size_t r = std::fread(head, 1, 2, f);
                std::fclose(f);
                if (r != 2 || head[0] != 0xFF || head[1] != 0xD8) soiOk = false;
            } else {
                soiOk = false;
            }
        }
        expect(bytesOk, "tex: copy is byte-faithful and size-stable");
        expect(soiOk, "tex: copied files carry JPEG SOI marker");
        std::filesystem::remove_all(src, ec);
        std::filesystem::remove_all(dst, ec);
    }

    // city.scene：E1 形态锚 + 引用闭合 + 确定性。
    {
        const std::string scA = serializeCityScene(lay);
        expect(scA == serializeCityScene(generateCity(p0)),
               "scene: same seed double-build byte-identical");
        // M-W3.0：nextID 37（36 block）→ 316 = 1 + 36 block + 188 tree +
        // 42 car + 49 streetlight。
        expect(scA.find("\"version\": 1") != std::string::npos &&
                   scA.find("\"nextID\": 316") != std::string::npos &&
                   scA.find("\"systems\": []") != std::string::npos,
               "scene: E1 World JSON anchors (version/nextID=316/systems)");
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
        // M-W3.0：block 实体仍 36（树/车/路灯实体单独计数见 M-W3 断言组）。
        expect(entCount == 36u && meshCount == 36u,
               "scene: 36 block entities with 36 OBJ mesh references");
        // JSON 结构有效性（M-W1.0 截断 bug 硬闸：buf 溢出确定性截断曾让
        // city.scene 非法 JSON 而锚全绿）。每个实体必须带完整 Transform
        // 尾段（rotation 收口），且全文花括号配平。M-W3.0：实体总数
        // 36 block + 188 tree + 42 car + 49 streetlight = 315，全部带
        // rotation 字段（车是绕 Y 旋转四元数，形态不同、M-W3 组另校验）。
        std::uint32_t rotCount = 0;
        pos = 0;
        while ((pos = scA.find("\"rotation\": [", pos)) != std::string::npos) {
            ++rotCount;
            pos += 10;
        }
        expect(rotCount == 315u,
               "scene: 315 rotation fields == entity count (36+188+42+49; "
               "no snprintf truncation)");
        std::int64_t braces = 0;
        bool negative = false;
        for (char ch : scA) {
            if (ch == '{') ++braces;
            else if (ch == '}') --braces;
            if (braces < 0) negative = true;
        }
        expect(!negative && braces == 0,
               "scene: braces balanced (JSON structural sanity)");
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
        // M-W3.0 改写：树/车 mesh 名与位置是 seed 的确定函数（变体分布 +
        // 停靠采样随 seed 变），原「seed-agnostic」断言失效。新语义 =
        // 异 seed 结构合法 + 实体内容必异。
        const std::string scP1 = serializeCityScene(generateCity(p1));
        expect(scP1 != scA,
               "scene: different seed -> different tree/car entity content "
               "(M-W3.0 variant/position draws)");
        {
            int bracesP1 = 0;
            bool negP1 = false;
            for (char ch : scP1) {
                if (ch == '{') ++bracesP1;
                if (ch == '}') --bracesP1;
                if (bracesP1 < 0) negP1 = true;
            }
            expect(!negP1 && bracesP1 == 0,
                   "scene: other-seed JSON braces balanced (structural)");
        }
    }

    // --- M-W3.0 街道资产（W3-PLAN 拍板 A1+B1+C1；全部 fnv1a64 纯函数）---
    {
        // 名字表守恒：池容量 + 白名单形态（扁平可打印 ASCII .glb，无路径分隔）。
        bool namesOk = true;
        for (std::uint32_t i = 0; i < kTreeVariantCount; ++i) {
            const std::string nm = kTreeVariantNames[i];
            namesOk = namesOk && nm.size() > 4u &&
                      nm.substr(nm.size() - 4u) == ".glb" &&
                      nm.find('/') == std::string::npos;
        }
        for (std::uint32_t i = 0; i < kCarVariantCount; ++i) {
            const std::string nm = kCarVariantNames[i];
            namesOk = namesOk && nm.size() > 4u &&
                      nm.substr(nm.size() - 4u) == ".glb" &&
                      nm.find('/') == std::string::npos;
        }
        expect(namesOk && std::string(kPropKindNames[0]) == "streetlight.glb",
               "street: asset name tables are flat whitelist .glb forms");
        // 树变体：池界 + 覆盖（188 棵 / 12 池，均匀化 hash 期望每种 ~16 棵；
        // 弱断言 >= 8 种防池退化为单型）。
        std::uint32_t tvSeen[kTreeVariantCount] = {};
        bool tvBounds = true;
        for (const CityCell& c : lay.cells)
            for (const CityTree& t : c.trees) {
                if (t.variant >= kTreeVariantCount) tvBounds = false;
                else ++tvSeen[t.variant];
            }
        std::uint32_t tvKinds = 0;
        for (std::uint32_t v = 0; v < kTreeVariantCount; ++v)
            if (tvSeen[v] > 0u) ++tvKinds;
        expect(tvBounds && tvKinds >= 8u,
               "street: tree variants in-bounds and cover >=8 of 12 pool");
        // 车：数量界（168 边 1/4 命率，期望 42；[24,64] 容忍 hash 涨落）+
        // 池界 + 位置在城界内 + edgeId 溯源界。
        const std::uint32_t carN = static_cast<std::uint32_t>(lay.cars.size());
        bool carOk = carN >= 24u && carN <= 64u;
        for (const CityCar& c : lay.cars) {
            carOk = carOk && c.variant < kCarVariantCount &&
                    c.edgeId < lay.stats.roadnetEdges &&
                    c.x >= lay.minX - 1.f && c.x <= lay.maxX + 1.f &&
                    c.z >= lay.minZ - 1.f && c.z <= lay.maxZ + 1.f;
        }
        expect(carOk, "street: cars count/variant/extent in-bounds");
        // 车确定性敏感：异 seed 首车必异（hash 链不同）。
        const CityLayout layC = generateCity(p1);
        bool carDiff = layC.cars.size() != lay.cars.size();
        if (!carDiff && !lay.cars.empty()) {
            const CityCar& a = lay.cars[0];
            const CityCar& b = layC.cars[0];
            carDiff = a.x != b.x || a.z != b.z || a.variant != b.variant ||
                      a.edgeId != b.edgeId;
        }
        expect(carDiff, "street: cars are seed-sensitive (pure fnv1a draws)");
        // 路灯：恒 = 路口数，位置在城界内。
        bool propOk = lay.props.size() == lay.stats.crossroads;
        for (const CityProp& p : lay.props) {
            propOk = propOk && p.x >= lay.minX - 1.f &&
                    p.x <= lay.maxX + 1.f && p.z >= lay.minZ - 1.f &&
                    p.z <= lay.maxZ + 1.f && p.kind < kPropKindCount;
        }
        expect(propOk, "street: one streetlight per crossroads, in-bounds");
        // city.scene 实体形态：树/车/路灯实体齐 + nextID 恒等式 + 车四元数。
        {
            const std::string scW3 = serializeCityScene(lay);
            const std::uint32_t expectNext =
                static_cast<std::uint32_t>(lay.cells.size()) + lay.stats.trees +
                lay.stats.cars + lay.stats.props + 1u;
            char bufS[96];
            std::snprintf(bufS, sizeof bufS, "\"nextID\": %u", expectNext);
            expect(scW3.find(bufS) != std::string::npos,
                   "scene: nextID == blocks + trees + cars + props + 1");
            expect(scW3.find("\"name\": \"tree_00\"") != std::string::npos &&
                       scW3.find("\"name\": \"car_00\"") != std::string::npos &&
                       scW3.find("\"name\": \"streetlight_00\"") !=
                           std::string::npos,
                   "scene: tree/car/streetlight entities present");
            expect(scW3.find("\"mesh\": \"streetlight.glb\"") !=
                       std::string::npos &&
                       scW3.find("\"mesh\": \"blocks/") != std::string::npos,
                   "scene: mesh refs mix vendored glb names and OBJ blocks");
        }
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
        "             + blocks/block_NN.obj + blocks/city.mtl + textures/tierN.jpg\n"
        "             (M-W1.1 textured buildings) + city.scene\n"
        "  --stats    print CityStats counter block (incl. density tiers, M-W0.3)\n"
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

// exe 所在目录（argv[0] 的 parent）：M-W1.1 贴图资产随 exe 分发
// （CMake POST_BUILD 拷贝 tier*.jpg 到 $<TARGET_FILE_DIR>/textures/）。
std::string exeAssetDir(const char* argv0) {
    if (argv0 == nullptr || *argv0 == '\0') return ".";
    return std::filesystem::path(argv0).parent_path().string();
}

int runGenerate(const GenerateConfig& cfg, const std::string& assetDir) {
    // exe 旁 textures/（CMake POST_BUILD 拷贝的 ambientCG 入库资产）。
    const std::string texSrcDir =
        (std::filesystem::path(assetDir) / "textures").string();
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
    // M-W1.1：材质库 city.mtl（与 OBJ 同目录，mtllib 引用）。
    const std::string mtl = serializeCityMtl();
    const std::string mtlPath =
        (std::filesystem::path(blocksDir) / "city.mtl").string();
    {
        FILE* f = std::fopen(mtlPath.c_str(), "wb");
        if (f == nullptr) {
            std::printf("[nl:error] cannot open %s for write\n", mtlPath.c_str());
            return 1;
        }
        const std::size_t wrote = std::fwrite(mtl.data(), 1, mtl.size(), f);
        std::fclose(f);
        if (wrote != mtl.size()) {
            std::printf("[nl:error] short write on %s\n", mtlPath.c_str());
            return 1;
        }
    }
    // M-W1.1：纹理集 textures/（ambientCG CC0 入库 JPG 逐字节拷贝；
    // obj2glb --maps 指这里）。
    const std::string texturesDir =
        (std::filesystem::path(cfg.outDir) / "textures").string();
    if (!writeCityTextures(texSrcDir, texturesDir)) {
        std::printf("[nl:error] texture copy failed: %s -> %s\n",
                    texSrcDir.c_str(), texturesDir.c_str());
        return 1;
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
    std::printf("wrote %s (materials=%d)\n", mtlPath.c_str(),
                kTexTierCount);
    std::printf("wrote %s (jpg=%d, source ambientCG CC0 facades)\n",
                texturesDir.c_str(), kTexTierCount);

    std::printf("nl-citygen M-W0.3 city top view + roadnet + blocks + scene + density\n");
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
            "fences=%u paths=%u forestCells=%u lowCells=%u mediumCells=%u "
            "highCells=%u noiseSeed=%u\n",
            lay.stats.blocks, lay.stats.groundTiles, lay.stats.roadSegments,
            lay.stats.crossroads, lay.stats.roadnetNodes,
            lay.stats.roadnetEdges, lay.stats.buildings, lay.stats.trees,
            lay.stats.cars, lay.stats.fences, lay.stats.paths,
            lay.stats.forestCells, lay.stats.lowCells, lay.stats.mediumCells,
            lay.stats.highCells, lay.noiseSeed);
        // M-W2.0 楼型变体分布（纯追加行，不破既有 findstr 锚；FNV-1a 选型
        // 确定性 → 分布是 seed 的确定函数）。
        std::uint32_t vCount[4] = {0, 0, 0, 0};
        for (const CityCell& c : lay.cells)
            for (const CityBuilding& b : c.buildings)
                if (b.variant < 4u) ++vCount[b.variant];
        std::printf(
            "variants: single=%u setback=%u podium=%u lshape=%u "
            "(M-W2.0 fnv1a pool)\n",
            vCount[0], vCount[1], vCount[2], vCount[3]);
        // M-W3.0 街道资产统计（纯追加行；车 = 沿边 1/4 命率 × 品 8 池，
        // 路灯 = 每路口 1 盏恒 crossroads，树变体分布 12 池）。
        std::uint32_t tvCount[kTreeVariantCount] = {};
        for (const CityCell& c : lay.cells)
            for (const CityTree& t : c.trees)
                if (t.variant < kTreeVariantCount) ++tvCount[t.variant];
        std::printf("street: cars=%u lights=%u treeVariants=", lay.stats.cars,
                    lay.stats.props);
        for (std::uint32_t i = 0; i < kTreeVariantCount; ++i)
            std::printf("%u%s", tvCount[i], i + 1u == kTreeVariantCount
                                                ? "\n"
                                                : ",");
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
        return runGenerate(cfg, exeAssetDir(argv[0]));
    }
    if (cmd == "help" || cmd == "--help") {
        printUsage();
        return 0;
    }
    std::printf("invalid arguments: unknown command %s\n", cmd.c_str());
    printUsage();
    return 2;
}
