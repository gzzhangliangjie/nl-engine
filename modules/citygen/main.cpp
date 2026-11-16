// citygen CLI 壳 —— 游戏③广州地图 M-CG0(feat/citygen,docs/citygen/CG0-PLAN.md)
//
// 退出码契约(对齐 city-tools / 展示台 expectExit):0 = PASS / 1 = 断言失败 / 2 = 用法错误。
// 命令面:
//   citygen selftest                      内置解析/bbox/坐标断言(M-CG0.0)
//   citygen stats --input <overpass.json> [--bbox s,w,n,e]   解析统计锚
//   (M-CG0.1 convert → roadnet.json;M-CG0.2 俯视 PNG / city.scene)
#include "citygen/osm.hpp"
#include "citygen/roadnet.hpp"
#include "citygen/buildings.hpp"
#include "citygen/topview.hpp"
#include "citygen/scene.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
        "citygen - nl-engine city generation (OSM input, M-CG0)\n"
        "usage:\n"
        "  citygen selftest\n"
        "  citygen stats --input <overpass.json> [--bbox south,west,north,east]\n"
        "  citygen buildings --input <overpass.json> [--bbox south,west,north,east]\n"
        "  citygen convert --input <overpass.json> --bbox south,west,north,east\n"
        "                  --out <roadnet.json>\n"
        "  citygen export --input <osm.json> --buildings <osm-bld.json>\n"
        "                 --bbox south,west,north,east --out <dir>\n"
        "                 (产出 roadnet.json / city.scene / buildings/box.obj /\n"
        "                  top.png / ODbL-attribution.txt)\n");
    return 2;
}

bool readTextFile(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 内容未变则跳过(mtime 不动,便于产物目录做逐字节回归锚)。
bool writeFileIfChangedOrNew(const std::string& path, const std::string& data) {
    std::string old;
    if (readTextFile(path.c_str(), old) && old == data) return true;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

bool parseBBox(const char* arg, citygen::BBox& box) {
    double v[4];
    if (std::sscanf(arg, "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3]) != 4) return false;
    box = citygen::BBox{v[0], v[1], v[2], v[3]};
    return box.valid();
}

// ---- selftest:内置小样本断言(M-CG0.0 验收判据①的解析基线) ----
const char* kMiniOverpass = R"JSON({
  "version": 0.6,
  "elements": [
    {"type": "node", "id": 1, "lat": 23.11, "lon": 113.32},
    {"type": "way", "id": 100,
     "geometry": [{"lat": 23.110, "lon": 113.320}, {"lat": 23.112, "lon": 113.322}],
     "tags": {"highway": "primary", "name": "测试路"}},
    {"type": "way", "id": 101,
     "geometry": [{"lat": 23.113, "lon": 113.321}, {"lat": 23.115, "lon": 113.323}, {"lat": 23.116, "lon": 113.325}]},
    {"type": "way", "id": 102},
    {"type": "way", "id": 103, "geometry": [{"lat": 23.14, "lon": 113.40}, {"lat": 23.15, "lon": 113.41}]}
  ]
})JSON";

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "citygen selftest: FAIL line %d: %s\n",  \
                         __LINE__, #cond);                                 \
            return 1;                                                      \
        }                                                                  \
    } while (0)

int selftest() {
    using namespace citygen;

    std::fprintf(stderr, "S0 enter\n");
    // ① 解析:way 收齐 + 非法元素计数不静默。
    OsmData d = parseOverpassJson(kMiniOverpass);
    std::fprintf(stderr, "S1 parsed ways=%zu\n", d.ways.size());
    CHECK(d.ways.size() == 3);
    CHECK(d.droppedNonWay == 1);       // node 元素
    CHECK(d.droppedNoGeometry == 1);   // id=102 无 geometry
    CHECK(d.ways[0].id == 100);
    CHECK(d.ways[0].pts.size() == 2);
    CHECK(d.ways[0].tags.at("highway") == "primary");
    CHECK(d.ways[1].pts.size() == 3);

    // ② 结构性损坏必须炸,不许吞。
    bool threw = false;
    try { parseOverpassJson("{\"version\":0.6}"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parseOverpassJson("not json at all"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // ③ bbox 语义。
    BBox zjx{23.108, 113.316, 23.130, 113.342};
    CHECK(zjx.valid());
    CHECK(zjx.contains(23.120, 113.330));
    CHECK(!zjx.contains(23.140, 113.330));
    CHECK(!zjx.contains(23.120, 113.350));

    // ④ 局部坐标锚:0.001° 纬度 ≈ 110.54 m;0.001° 经度 ≈ cos(23.1°)*111.32 ≈ 102.35 m。
    auto [dxLat, dyLat] = toLocalMeters(zjx, zjx.centerLat() + 0.001, zjx.centerLon());
    CHECK(dyLat > 110.0 && dyLat < 111.0);   // 纬度增量 → 北向米
    CHECK(dxLat > -0.5 && dxLat < 0.5);      // 经度未动 → 东向 0
    auto [dxLon, dyLon] = toLocalMeters(zjx, zjx.centerLat(), zjx.centerLon() + 0.001);
    CHECK(dxLon > 101.9 && dxLon < 102.8);   // 经度增量 → 东向米
    CHECK(dyLon > -0.5 && dyLon < 0.5);      // 纬度未动 → 北向 0

    // ⑤ 过滤:样本内 bbox 内 1 条 / 外 1 条,非法计数透传。
    OsmData f = filterBBox(d, zjx);
    CHECK(f.ways.size() == 2);
    CHECK(f.droppedNoGeometry == d.droppedNoGeometry);
    CHECK(f.droppedNonWay == d.droppedNonWay);

    // ⑥ roadnet(M-CG0.1):primary 双向 → 2 有向边;无 highway 标签 way 丢弃
    //   (way 101);bbox 已滤掉 way 103。
    RoadnetBuildStats rs;
    Roadnet net = buildRoadnetFromOsm(f, zjx, &rs);
    std::fprintf(stderr, "S6 roadnet nodes=%zu edges=%zu dropNV=%zu deg=%zu\n",
                 net.nodes.size(), net.edges.size(), rs.droppedNonVehicle,
                 rs.droppedDegenerate);
    CHECK(net.nodes.size() == 2);
    CHECK(net.edges.size() == 2);
    CHECK(rs.droppedNonVehicle == 1);   // way 101(无 highway 标签)
    CHECK(net.edges[0].from == 0 && net.edges[0].to == 1);
    CHECK(net.edges[1].from == 1 && net.edges[1].to == 0);
    CHECK(net.edges[0].speedLimit > 16.6 && net.edges[0].speedLimit < 16.8);  // primary 缺省
    CHECK(net.edges[0].width > 9.9 && net.edges[0].width < 10.1);
    CHECK(net.anchorLat > 23.11 && net.anchorLat < 23.13);
    // 信号位:双向单边路两端入度各 1 → 无信号(M-T4 口径)。
    CHECK(!net.nodes[0].isSignal && !net.nodes[1].isSignal);

    // ⑦ roadnet 细节:oneway 单向 / maxspeed 标签优先于类别缺省 / lanes 透传 /
    //   序列化锚(version 1 + nodes/edges 计数一致)。
    const char* kOneway = R"JSON({"version": 0.6, "elements": [
      {"type": "way", "id": 1,
       "geometry": [{"lat": 23.110, "lon": 113.320}, {"lat": 23.112, "lon": 113.322}],
       "tags": {"highway": "residential", "oneway": "yes", "maxspeed": "30", "lanes": "2"}}]})JSON";
    OsmData od = parseOverpassJson(kOneway);
    Roadnet n2 = buildRoadnetFromOsm(od, zjx);
    std::fprintf(stderr, "S7 oneway edges=%zu spd=%.2f lanes=%u\n",
                 n2.edges.size(), n2.edges.empty() ? 0.0 : n2.edges[0].speedLimit,
                 n2.edges.empty() ? 0u : n2.edges[0].lanesPerDir);
    CHECK(n2.edges.size() == 1);        // oneway=yes → 只有 from→to
    CHECK(n2.edges[0].from == 0 && n2.edges[0].to == 1);
    CHECK(n2.edges[0].speedLimit > 8.3 && n2.edges[0].speedLimit < 8.4);  // 30kmh=8.333
    CHECK(n2.edges[0].lanesPerDir == 2);
    std::string ser = serializeRoadnet(n2);
    CHECK(ser.find("\"version\": 1") != std::string::npos);
    CHECK(ser.find("\"phases\": []") != std::string::npos);
    {
        int nodeCnt = 0, edgeCnt = 0;
        for (std::size_t i = 0; i + 6 < ser.size(); ++i)
            if (ser.compare(i, 6, "\"from\"") == 0) ++edgeCnt;
        CHECK(edgeCnt == 1);
        (void)nodeCnt;
    }

    // ⑧ buildings/scene/topview(M-CG0.2):height 标签 > levels 折算 > 缺省;
    //   退化 footprint 丢弃计数;块外接盒正;PNG 头锚。
    const char* kBld = R"JSON({"version": 0.6, "elements": [
      {"type": "way", "id": 10,
       "geometry": [{"lat": 23.1100, "lon": 113.3200}, {"lat": 23.1120, "lon": 113.3200},
                    {"lat": 23.1120, "lon": 113.3230}, {"lat": 23.1100, "lon": 113.3230}],
       "tags": {"building": "yes", "building:levels": "5", "name": "楼"}},
      {"type": "way", "id": 11,
       "geometry": [{"lat": 23.1140, "lon": 113.3260}, {"lat": 23.1155, "lon": 113.3260},
                    {"lat": 23.1155, "lon": 113.3280}],
       "tags": {"building": "commercial", "height": "42 m"}},
      {"type": "way", "id": 12,
       "geometry": [{"lat": 23.1120, "lon": 113.3220}]},
      {"type": "way", "id": 13,
       "geometry": [{"lat": 23.1160, "lon": 113.3300}, {"lat": 23.1162, "lon": 113.3300}],
       "tags": {"building": "yes"}}]})JSON";
    OsmData bd = parseOverpassJson(kBld);
    BuildingStats bs;
    std::vector<Building> blds = extractBuildings(bd, &bs);
    std::fprintf(stderr, "S8 buildings=%zu tag=%zu lv=%zu def=%zu drop=%zu\n",
                 blds.size(), bs.heightFromTag, bs.heightFromLevels,
                 bs.heightDefault, bs.droppedNoPoly);
    CHECK(blds.size() == 2);              // way 13(2 点) 丢弃;way 12 无 building 标签不进统计
    CHECK(bs.droppedNoPoly == 1);
    CHECK(blds[0].heightSrc == 2 && blds[0].heightM > 14.9 && blds[0].heightM < 15.1);
    CHECK(blds[1].heightSrc == 1 && blds[1].heightM > 41.9 && blds[1].heightM < 42.1);
    BlockBox bb;
    CHECK(buildingBlockBox(blds[0], zjx, bb));
    CHECK(bb.wx > 0.0 && bb.wz > 0.0);
    std::string scene = serializeCityScene(blds, zjx, "buildings/box.obj");
    CHECK(scene.find("\"nextID\": 3") != std::string::npos);
    CHECK(scene.find("bld_10") != std::string::npos);
    CHECK(scene.find("\"buildings/box.obj\"") != std::string::npos);
    // A 档:OSM name → 实体级 label(先 label 后 components;entity 对象内)。
    {
        const std::size_t lab = scene.find("\"label\": ");
        CHECK(lab != std::string::npos);
        CHECK(scene.find("\"components\"", lab) != std::string::npos);
        CHECK(scene.rfind("\"components\"", lab) ==
              std::string::npos);  // label 在实体级,其前无 components
    }
    CHECK(unitBoxObj().find("f 1//1 2//1 3//1") != std::string::npos);
    {
        TopViewConfig tvc;
        tvc.targetWidth = 256;
        std::vector<std::uint8_t> png;
        int pw = 0, ph = 0;
        TopViewStats tvs;
        CHECK(renderTopView(n2, blds, zjx, tvc, png, pw, ph, &tvs));
        std::fprintf(stderr, "S8 png %dx%d bytes=%zu bldPx=%zu roadPx=%zu\n",
                     pw, ph, png.size(), tvs.buildingPixels, tvs.roadPixels);
        CHECK(pw == 256 && ph > 0 && ph <= 16384);
        CHECK(png.size() > 8);
        CHECK(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
        CHECK(tvs.buildingPixels > 0);   // 内容锚:建筑真的画进画布
        CHECK(tvs.roadPixels > 0);
    }

    std::printf("citygen selftest: PASS (8 groups, mini overpass sample)\n");
    std::fflush(stdout);
    return 0;
}

int cmdStats(int argc, char** argv) {
    using namespace citygen;
    const char* input = nullptr;
    bool haveBox = false;
    citygen::BBox box;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--input") && i + 1 < argc) {
            input = argv[++i];
        } else if (!std::strcmp(argv[i], "--bbox") && i + 1 < argc) {
            if (!parseBBox(argv[++i], box)) {
                std::fprintf(stderr, "citygen: bad --bbox (want south,west,north,east)\n");
                return 2;
            }
            haveBox = true;
        } else {
            std::fprintf(stderr, "citygen stats: unknown arg '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!input) return usage();

    std::string text;
    if (!readTextFile(input, text)) {
        std::fprintf(stderr, "citygen: cannot read '%s'\n", input);
        return 2;
    }
    try {
        citygen::OsmData d = parseOverpassJson(text);
        std::size_t kept = d.ways.size();
        if (haveBox) {
            d = filterBBox(d, box);
            std::printf("bbox %.4f,%.4f..%.4f,%.4f kept %zu of %zu ways\n",
                        box.south, box.west, box.north, box.east, d.ways.size(), kept);
            kept = d.ways.size();
        }
        std::printf("ways=%zu droppedNoGeometry=%zu droppedNonWay=%zu\n",
                    kept, d.droppedNoGeometry, d.droppedNonWay);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "citygen: parse error: %s\n", e.what());
        return 2;
    }
    return 0;
}

// ---- convert(M-CG0.1):OSM Overpass JSON → roadnet.json ----
int cmdConvert(int argc, char** argv) {
    using namespace citygen;
    const char* input = nullptr;
    const char* outPath = nullptr;
    bool haveBox = false;
    citygen::BBox box;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--input") && i + 1 < argc) {
            input = argv[++i];
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            outPath = argv[++i];
        } else if (!std::strcmp(argv[i], "--bbox") && i + 1 < argc) {
            if (!parseBBox(argv[++i], box)) {
                std::fprintf(stderr, "citygen: bad --bbox (want south,west,north,east)\n");
                return 2;
            }
            haveBox = true;
        } else {
            std::fprintf(stderr, "citygen convert: unknown arg '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!input || !outPath || !haveBox) return usage();

    std::string text;
    if (!readTextFile(input, text)) {
        std::fprintf(stderr, "citygen: cannot read '%s'\n", input);
        return 2;
    }
    try {
        citygen::OsmData d = parseOverpassJson(text);
        const std::size_t total = d.ways.size();
        d = filterBBox(d, box);
        RoadnetBuildStats rs;
        Roadnet net = buildRoadnetFromOsm(d, box, &rs);
        const std::string ser = serializeRoadnet(net);
        {
            std::ofstream f(outPath, std::ios::binary);
            if (!f) {
                std::fprintf(stderr, "citygen: cannot write '%s'\n", outPath);
                return 2;
            }
            f.write(ser.data(), static_cast<std::streamsize>(ser.size()));
            if (!f) {
                std::fprintf(stderr, "citygen: write failed '%s'\n", outPath);
                return 2;
            }
        }
        std::printf("bbox %.4f,%.4f..%.4f,%.4f ways=%zu of %zu\n"
                    "nodes=%zu edges=%zu droppedNonVehicle=%zu droppedDegenerate=%zu\n"
                    "wrote %s bytes=%zu\n",
                    box.south, box.west, box.north, box.east, d.ways.size(), total,
                    net.nodes.size(), net.edges.size(), rs.droppedNonVehicle,
                    rs.droppedDegenerate, outPath, ser.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "citygen: parse error: %s\n", e.what());
        return 2;
    }
    return 0;
}

// ---- buildings(M-CG0.2):建筑提取统计锚 ----
int cmdBuildings(int argc, char** argv) {
    using namespace citygen;
    const char* input = nullptr;
    bool haveBox = false;
    citygen::BBox box;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--input") && i + 1 < argc) {
            input = argv[++i];
        } else if (!std::strcmp(argv[i], "--bbox") && i + 1 < argc) {
            if (!parseBBox(argv[++i], box)) {
                std::fprintf(stderr, "citygen: bad --bbox (want south,west,north,east)\n");
                return 2;
            }
            haveBox = true;
        } else {
            std::fprintf(stderr, "citygen buildings: unknown arg '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!input) return usage();
    std::string text;
    if (!readTextFile(input, text)) {
        std::fprintf(stderr, "citygen: cannot read '%s'\n", input);
        return 2;
    }
    try {
        citygen::OsmData d = parseOverpassJson(text);
        if (haveBox) d = filterBBox(d, box);
        BuildingStats bs;
        std::vector<Building> blds = extractBuildings(d, &bs);
        std::printf("buildings=%zu heightFromTag=%zu heightFromLevels=%zu "
                    "heightDefault=%zu droppedNoPoly=%zu\n",
                    blds.size(), bs.heightFromTag, bs.heightFromLevels,
                    bs.heightDefault, bs.droppedNoPoly);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "citygen: parse error: %s\n", e.what());
        return 2;
    }
    return 0;
}

// ---- export(M-CG0.2):一次产 roadnet.json / city.scene / box.obj / top.png ----
int cmdExport(int argc, char** argv) {
    using namespace citygen;
    const char* input = nullptr;
    const char* bldInput = nullptr;
    const char* outDir = nullptr;
    bool haveBox = false;
    citygen::BBox box;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--input") && i + 1 < argc) {
            input = argv[++i];
        } else if (!std::strcmp(argv[i], "--buildings") && i + 1 < argc) {
            bldInput = argv[++i];
        } else if (!std::strcmp(argv[i], "--bbox") && i + 1 < argc) {
            if (!parseBBox(argv[++i], box)) {
                std::fprintf(stderr, "citygen: bad --bbox (want south,west,north,east)\n");
                return 2;
            }
            haveBox = true;
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            outDir = argv[++i];
        } else {
            std::fprintf(stderr, "citygen export: unknown arg '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!input || !bldInput || !outDir || !haveBox) return usage();

    std::string roadText, bldText;
    if (!readTextFile(input, roadText)) {
        std::fprintf(stderr, "citygen: cannot read '%s'\n", input);
        return 2;
    }
    if (!readTextFile(bldInput, bldText)) {
        std::fprintf(stderr, "citygen: cannot read '%s'\n", bldInput);
        return 2;
    }
    try {
        std::filesystem::create_directories(outDir);
        std::filesystem::create_directories(
            std::filesystem::path(outDir) / "buildings");
        OsmData d = parseOverpassJson(roadText);
        const std::size_t totalWays = d.ways.size();
        d = filterBBox(d, box);
        RoadnetBuildStats rs;
        Roadnet net = buildRoadnetFromOsm(d, box, &rs);
        const std::string roadnetText = serializeRoadnet(net);
        writeFileIfChangedOrNew((std::filesystem::path(outDir) / "roadnet.json").string(),
                                roadnetText);

        OsmData b = parseOverpassJson(bldText);
        if (!b.ways.empty() || bldText.size() > 64) b = filterBBox(b, box);
        BuildingStats bs;
        std::vector<Building> blds = extractBuildings(b, &bs);
        const std::string sceneText = serializeCityScene(blds, box, "buildings/box.obj");
        writeFileIfChangedOrNew((std::filesystem::path(outDir) / "city.scene").string(),
                                sceneText);
        writeFileIfChangedOrNew((std::filesystem::path(outDir) / "buildings" / "box.obj").string(),
                                unitBoxObj());

        TopViewConfig tvc;
        std::vector<std::uint8_t> png;
        int pw = 0, ph = 0;
        TopViewStats tvs;
        if (!renderTopView(net, blds, box, tvc, png, pw, ph, &tvs)) {
            std::fprintf(stderr, "citygen: top view render failed\n");
            return 1;
        }
        {
            std::ofstream f(std::filesystem::path(outDir) / "top.png",
                            std::ios::binary);
            f.write(reinterpret_cast<const char*>(png.data()),
                    static_cast<std::streamsize>(png.size()));
            if (!f) {
                std::fprintf(stderr, "citygen: write top.png failed\n");
                return 2;
            }
        }
        // ODbL 署名注记随产物落盘(CG0-PLAN §3 / S-PLAN §3)。
        writeFileIfChangedOrNew(
            (std::filesystem::path(outDir) / "ODbL-attribution.txt").string(),
            "Source: OpenStreetMap data via Overpass API (c) OpenStreetMap contributors, ODbL 1.0.\n"
            "Derived products in this directory must carry this attribution when distributed.\n");

        std::printf("nodes=%zu edges=%zu buildings=%zu\n"
                    "png=%dx%d bytes=%zu bldPx=%zu roadPx=%zu roadnet=%zu scene=%zu\n"
                    "wrote %s\n",
                    net.nodes.size(), net.edges.size(), blds.size(),
                    pw, ph, png.size(), tvs.buildingPixels, tvs.roadPixels,
                    roadnetText.size(), sceneText.size(),
                    outDir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "citygen: export error: %s\n", e.what());
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    if (!std::strcmp(argv[1], "selftest")) return selftest();
    if (!std::strcmp(argv[1], "stats")) return cmdStats(argc, argv);
    if (!std::strcmp(argv[1], "buildings")) return cmdBuildings(argc, argv);
    if (!std::strcmp(argv[1], "convert")) return cmdConvert(argc, argv);
    if (!std::strcmp(argv[1], "export")) return cmdExport(argc, argv);
    return usage();
}
