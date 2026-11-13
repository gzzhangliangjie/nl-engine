// traffic-tools — P8 M-T0.0：交通 lane graph 无头验收工具（A 类轨道 feat/worldgen）
// 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 nav-tools / city-tools / 展示台 expectExit）。
// 只链 nl_core（nl/traffic 零图形依赖）；拍板与口径 docs/worldgen/TRAFFIC-PLAN.md。
//
// 子命令：
//   build <roadnet.json> [--dump out.json] [--stats]   构建 lane graph（确定性）
//   selftest [--emit-fixture path]                     数值锚 selftest（确定性逐位）
//   help                                               用法

#include "nl/traffic/LaneGraph.hpp"
#include "nl/traffic/LanePath.hpp"
#include "nl/traffic/TrafficSim.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION // 单 TU 实现（city-tools 先例）
#include <stb_image_write.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_asserts = 0;
bool g_verbose = false;

#define T0_CHECK(cond)                                                       \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "traffic selftest: FAIL at %s:%d  %s\n",    \
                         __FILE__, __LINE__, #cond);                         \
            return 1;                                                        \
        }                                                                    \
        if (g_verbose)                                                       \
            std::printf("ok %04d %s:%d\n", g_asserts + 1, __FILE__, __LINE__);\
        ++g_asserts;                                                         \
    } while (0)

bool readFile(const std::string& path, std::string* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << body;
    return f.good();
}

// ---------------------------------------------------------------------------
// hermetic 夹具：4 节点方格（不依赖 city-tools 产物，selftest 自包含）。
// nodes: 0(0,0) 1(10,0) 2(0,10) 3(10,10)；edges 0~7 双向成对环；width 1.0 / 1 车道
// / 13.9 m/s。期望：8 lanes；edge0(0->1, f=+X) 车道在 -Z 侧（right=(0,-1)）。
std::string fixtureRoadnet() {
    return R"({
  "version": 1,
  "seed": 12345,
  "units": "meters",
  "blocks": { "x": 5.5, "z": 4.0 },
  "nodes": [
    { "id": 0, "x": 0.0,  "z": 0.0,  "kind": "intersection", "signal": false },
    { "id": 1, "x": 10.0, "z": 0.0,  "kind": "intersection", "signal": false },
    { "id": 2, "x": 0.0,  "z": 10.0, "kind": "intersection", "signal": false },
    { "id": 3, "x": 10.0, "z": 10.0, "kind": "intersection", "signal": false }
  ],
  "edges": [
    { "id": 0, "from": 0, "to": 1, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 },
    { "id": 1, "from": 1, "to": 0, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 },
    { "id": 2, "from": 0, "to": 2, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 },
    { "id": 3, "from": 2, "to": 0, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 },
    { "id": 4, "from": 1, "to": 3, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 },
    { "id": 5, "from": 3, "to": 1, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 },
    { "id": 6, "from": 2, "to": 3, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 },
    { "id": 7, "from": 3, "to": 2, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 1.0 }
  ],
  "phases": []
}
)";
}

int laneByEdge(const nl::traffic::LaneGraph& g, int32_t edgeId) {
    for (const nl::traffic::Lane& l : g.lanes)
        if (l.edgeId == edgeId) return l.id;
    return -1;
}

// ---------------------------------------------------------------------------
// 纯 CPU 光栅化（证据图 = 工具真实输出；city-tools stb_image_write 先例）
struct Raster {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
};

void drawLine(Raster& r, float x0, float y0, float x1, float y1,
              unsigned char rr, unsigned char gg, unsigned char bb) {
    // DDA 步进（证据图够用；整数化防越界）
    const int steps = static_cast<int>(std::fmax(std::fabs(x1 - x0), std::fabs(y1 - y0))) + 1;
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const int px = static_cast<int>(x0 + (x1 - x0) * t);
        const int py = static_cast<int>(y0 + (y1 - y0) * t);
        if (px < 0 || py < 0 || px >= r.w || py >= r.h) continue;
        const size_t off = (static_cast<size_t>(py) * r.w + px) * 3;
        r.rgb[off] = rr;
        r.rgb[off + 1] = gg;
        r.rgb[off + 2] = bb;
    }
}

// 世界(XZ) -> 像素映射：等比缩放进 WxH 留边 16px，z 轴向上（行号翻转）
struct WorldMap {
    float scale = 1.0f;
    float minX = 0.0f, minZ = 0.0f;
    int w = 0, h = 0;
    float px(float x) const { return 16.0f + (x - minX) * scale; }
    float py(float z) const { return static_cast<float>(h - 1) - (16.0f + (z - minZ) * scale); }
};

WorldMap worldMapFor(const nl::traffic::LaneGraph& g, int w, int h) {
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (const nl::traffic::Lane& l : g.lanes)
        for (const nl::traffic::LanePoint& p : l.points) {
            minX = std::fmin(minX, p.x);
            maxX = std::fmax(maxX, p.x);
            minZ = std::fmin(minZ, p.z);
            maxZ = std::fmax(maxZ, p.z);
        }
    WorldMap m;
    m.w = w;
    m.h = h;
    m.minX = minX;
    m.minZ = minZ;
    const float dx = (maxX - minX) > 1e-6f ? (maxX - minX) : 1.0f;
    const float dz = (maxZ - minZ) > 1e-6f ? (maxZ - minZ) : 1.0f;
    m.scale = std::fmin(static_cast<float>(w - 32) / dx, static_cast<float>(h - 32) / dz);
    return m;
}

void rasterLanes(Raster& r, const nl::traffic::LaneGraph& g, const WorldMap& m) {
    for (const nl::traffic::Lane& l : g.lanes)
        for (size_t i = 0; i + 1 < l.points.size(); ++i)
            drawLine(r, m.px(l.points[i].x), m.py(l.points[i].z),
                     m.px(l.points[i + 1].x), m.py(l.points[i + 1].z), 120, 120, 120);
}

bool writePng(const std::string& path, const Raster& r) {
    return stbi_write_png(path.c_str(), r.w, r.h, 3, r.rgb.data(), r.w * 3) != 0;
}

int selftest(const std::string& emitFixture) {
    namespace t = nl::traffic;

    const std::string fixture = fixtureRoadnet();

    // --- ① 构建成功 + 车道数网格公式锚（8 有向边 x 1 车道） ---
    t::LaneGraph g;
    std::string err;
    T0_CHECK(t::buildLaneGraphFromRoadnetJson(fixture.data(), fixture.size(), &g, &err));
    T0_CHECK(err.empty());
    T0_CHECK(g.laneCount() == 8);
    T0_CHECK(g.lanes[0].edgeId == 0 && g.lanes[1].edgeId == 1); // 文件序遍历锚

    // --- ② 偏移几何：edge0 f=+X，right=(0,-1)，off=0.25 -> 车道在 -Z 侧 ---
    const t::Lane& l0 = g.lanes[0];
    T0_CHECK(l0.offset == 0.25f);
    T0_CHECK(l0.points.size() == 2);
    T0_CHECK(l0.points[0].x == 0.0f && l0.points[0].z == -0.25f);
    T0_CHECK(l0.points[1].x == 10.0f && l0.points[1].z == -0.25f);
    T0_CHECK(l0.length == 10.0f);
    T0_CHECK(l0.speedLimit == 13.9f);
    // 反向 edge1 f=-X，right=(0,1) -> +Z 侧：两方向分居中线两侧（右行口径）
    const t::Lane& l1 = g.lanes[1];
    T0_CHECK(l1.offset == 0.25f);
    T0_CHECK(l1.points[0].z == 0.25f && l1.points[1].z == 0.25f);

    // --- ③ 多车道偏移序：width 2.0 / 2 车道 -> lane0(最内) 0.75 > lane1 0.25 ---
    std::string wide = fixture;
    const std::string from = "\"from\": 0, \"to\": 1, \"lanes_per_dir\": 1, \"speed_limit\": 13.9, \"width\": 1.0 }";
    const std::string to = "\"from\": 0, \"to\": 1, \"lanes_per_dir\": 2, \"speed_limit\": 13.9, \"width\": 2.0 }";
    const size_t pos = wide.find(from);
    T0_CHECK(pos != std::string::npos);
    wide.replace(pos, from.size(), to);
    t::LaneGraph gw;
    T0_CHECK(t::buildLaneGraphFromRoadnetJson(wide.data(), wide.size(), &gw, &err));
    T0_CHECK(gw.laneCount() == 9);
    T0_CHECK(gw.lanes[0].offset == 0.75f && gw.lanes[1].offset == 0.25f);
    T0_CHECK(gw.lanes[0].laneIndex == 0 && gw.lanes[1].laneIndex == 1);

    // --- ④ 连接闭合 + 禁掉头：edge0 lane(0->1) outgoing 只含 edge4(1->3)，
    //     edge1(1->0) 是掉头不建链；node1 出边里 5(3->1) from 不对也不链 ---
    T0_CHECK(l0.outgoing.size() == 1);
    T0_CHECK(l0.outgoing[0].destLane == laneByEdge(g, 4));
    T0_CHECK(l0.incoming.size() == 1);
    // 锚：edge0(0->1) 的 incoming = 终点为 0 的非掉头边 = edge3(2->0)；
    // edge1(1->0) 掉头排除、edge2(0->2) 终点是 2 不入
    T0_CHECK(l0.incoming[0].destLane == laneByEdge(g, 3));
    // 全图：每 lane 恰 1 outgoing（方格每节点 2 条路）+ 总链数 = 8
    int totalOut = 0;
    for (const t::Lane& l : g.lanes) {
        T0_CHECK(l.outgoing.size() == 1);
        totalOut += static_cast<int>(l.outgoing.size());
    }
    T0_CHECK(totalOut == 8);
    // 禁掉头全图扫描：任何 outgoing 不得指向反向对边（from/to 互换）的车道
    for (const t::Lane& l : g.lanes) {
        for (const t::LaneLink& link : l.outgoing) {
            const t::Lane& d = g.lanes[link.destLane];
            T0_CHECK(!(l.fromNode == d.toNode && l.toNode == d.fromNode));
        }
    }
    // 双向闭合：incoming 集合 == 反查 outgoing 包含自己的 lane 集合
    for (const t::Lane& l : g.lanes) {
        for (const t::LaneLink& in : l.incoming) {
            const t::Lane& src = g.lanes[in.destLane];
            bool found = false;
            for (const t::LaneLink& o : src.outgoing)
                if (o.destLane == l.id) found = true;
            T0_CHECK(found);
        }
    }

    // --- ⑤ 确定性：同输入双跑 dump 逐字节一致；异输入必异 ---
    const std::string dumpA = t::dumpLaneGraphJson(g);
    t::LaneGraph g2;
    T0_CHECK(t::buildLaneGraphFromRoadnetJson(fixture.data(), fixture.size(), &g2, &err));
    T0_CHECK(t::dumpLaneGraphJson(g2) == dumpA);
    std::string moved = fixture;
    const std::string mFrom = "\"x\": 10.0";
    const std::string mTo = "\"x\": 11.0";
    const size_t mp = moved.find(mFrom); // 首个匹配 = 节点 1（文件序）
    T0_CHECK(mp != std::string::npos);
    moved.replace(mp, mFrom.size(), mTo); // 节点 1 挪位 -> dump 必异
    t::LaneGraph g3;
    T0_CHECK(t::buildLaneGraphFromRoadnetJson(moved.data(), moved.size(), &g3, &err));
    T0_CHECK(t::dumpLaneGraphJson(g3) != dumpA);

    // --- ⑥ 负向：断引用 / 坏 version / 非法字段 / 坏 JSON 全部显式报错 ---
    {
        const std::string badVersion = "{\"version\":2,\"nodes\":[],\"edges\":[]}";
        const std::string dangling =
            "{\"version\":1,\"nodes\":[],\"edges\":[{\"id\":0,\"from\":0,\"to\":1,"
            "\"lanes_per_dir\":1,\"speed_limit\":1.0,\"width\":1.0}]}";
        T0_CHECK(!t::buildLaneGraphFromRoadnetJson(badVersion.data(), badVersion.size(), &g2, &err));
        T0_CHECK(!err.empty());
        T0_CHECK(!t::buildLaneGraphFromRoadnetJson(dangling.data(), dangling.size(), &g2, &err));
        T0_CHECK(!err.empty());
    }
    std::string badLanes = fixture;
    const std::string bNeedle = "\"lanes_per_dir\": 1, \"speed_limit\": 13.9, \"width\": 1.0 }";
    const std::string bRepl = "\"lanes_per_dir\": 0, \"speed_limit\": 13.9, \"width\": 1.0 }";
    const size_t bp = badLanes.find(bNeedle);
    T0_CHECK(bp != std::string::npos);
    badLanes.replace(bp, bNeedle.size(), bRepl);
    T0_CHECK(!t::buildLaneGraphFromRoadnetJson(badLanes.data(), badLanes.size(), &g2, &err));
    T0_CHECK(!err.empty());
    T0_CHECK(!t::buildLaneGraphFromRoadnetJson("not json", 8, &g2, &err));
    T0_CHECK(!err.empty());

    // --- ⑦ M-T0.1 A* 寻路：唯一路径逐位锚 + 同 lane / 不可达 / 越界负向 ---
    {
        const int32_t l0 = laneByEdge(g, 0);
        const int32_t l4 = laneByEdge(g, 4);
        const int32_t l3 = laneByEdge(g, 3);
        const int32_t l6 = laneByEdge(g, 6);
        // 直达：edge0(0->1) -> edge4(1->3) = [e0, e4]
        std::vector<int32_t> path;
        std::string perr;
        T0_CHECK(nl::traffic::findPathOnLanes(g, l0, l4, &path, &perr) ==
                 nl::traffic::PathStatus::Ok);
        T0_CHECK(path.size() == 2 && path[0] == l0 && path[1] == l4);
        // 多跳唯一路径：e0 -> e3 经 e4(1->3) e7(3->2)（方格图每节点 2 路，路唯一）
        T0_CHECK(nl::traffic::findPathOnLanes(g, l0, l3, &path, &perr) ==
                 nl::traffic::PathStatus::Ok);
        T0_CHECK(path.size() == 4);
        T0_CHECK(path[0] == l0 && path[1] == laneByEdge(g, 4) &&
                 path[2] == laneByEdge(g, 7) && path[3] == l3);
        // 弧长合计锚：4 条 10m 边 = 40
        T0_CHECK(nl::traffic::pathLength(g, path) == 40.0f);
        // 同 lane：单元素路径（独立变量，不覆盖 path——末尾确定性对比要用）
        std::vector<int32_t> pathOne;
        std::vector<int32_t> pathNeg; // 负向共用（Unreachable/BadLane 都会 clear out）
        T0_CHECK(nl::traffic::findPathOnLanes(g, l0, l0, &pathOne, &perr) ==
                 nl::traffic::PathStatus::Ok);
        T0_CHECK(pathOne.size() == 1 && pathOne[0] == l0);
        // 不可达：删掉 e4/e5（1<->3 整对）后 e0(0->1) 出边为空成孤端，
        // e6(2->3) 与 e0 连通域断开 -> Unreachable（合法 lane，非参数错误）
        std::string cutGraph = fixture;
        const std::string e4line = "    { \"id\": 4, \"from\": 1, \"to\": 3, \"lanes_per_dir\": 1, \"speed_limit\": 13.9, \"width\": 1.0 },\n";
        const std::string e5line = "    { \"id\": 5, \"from\": 3, \"to\": 1, \"lanes_per_dir\": 1, \"speed_limit\": 13.9, \"width\": 1.0 },\n";
        T0_CHECK(cutGraph.find(e4line) != std::string::npos);
        cutGraph.erase(cutGraph.find(e4line), e4line.size());
        T0_CHECK(cutGraph.find(e5line) != std::string::npos);
        cutGraph.erase(cutGraph.find(e5line), e5line.size());
        nl::traffic::LaneGraph gc;
        T0_CHECK(nl::traffic::buildLaneGraphFromRoadnetJson(cutGraph.data(), cutGraph.size(), &gc, &err));
        T0_CHECK(gc.laneCount() == 6);
        T0_CHECK(nl::traffic::findPathOnLanes(gc, laneByEdge(gc, 0), laneByEdge(gc, 6), &pathNeg, &perr) ==
                 nl::traffic::PathStatus::Unreachable);
        T0_CHECK(!perr.empty());
        (void)l6;
        // 越界：BadLane 显式
        T0_CHECK(nl::traffic::findPathOnLanes(g, -1, l0, &pathNeg, &perr) ==
                 nl::traffic::PathStatus::BadLane);
        T0_CHECK(nl::traffic::findPathOnLanes(g, l0, 999, &pathNeg, &perr) ==
                 nl::traffic::PathStatus::BadLane);
        // 确定性：同输入双跑路径逐位一致
        std::vector<int32_t> pathB;
        T0_CHECK(nl::traffic::findPathOnLanes(g, l0, l3, &pathB, &perr) ==
                 nl::traffic::PathStatus::Ok);
        T0_CHECK(pathB == path);
    }

    // --- ⑧ M-T0.2 直线跟车（C1）：确定性逐位 + 恒距巡航 + 停车不穿透 ---
    {
        // ① 确定性：3 车混合路径 300 帧双跑逐位一致（memcmp 口径，phys/audio 先例）
        nl::traffic::TrafficSimParams p; // 默认 dt=1/60 accel=2 brake=4 minGap=2
        nl::traffic::TrafficSim sa;
        sa.graph = &g;
        sa.params = p;
        std::string serr;
        T0_CHECK(sa.addCar({laneByEdge(g, 0), laneByEdge(g, 4)}, 0.0f, &serr));
        T0_CHECK(sa.addCar({laneByEdge(g, 2), laneByEdge(g, 6)}, 0.0f, &serr));
        T0_CHECK(sa.addCar({laneByEdge(g, 0), laneByEdge(g, 4), laneByEdge(g, 7), laneByEdge(g, 3)}, 0.0f, &serr));
        for (int f = 0; f < 300; ++f) sa.step();
        // 真正的双跑：从零重建同输入（状态数组逐位比；TrafficCar 内含 route
        // vector，指针不可 memcmp，只比 TrafficCarState POD 面）
        nl::traffic::TrafficSim sc;
        sc.graph = &g;
        sc.params = p;
        T0_CHECK(sc.addCar({laneByEdge(g, 0), laneByEdge(g, 4)}, 0.0f, &serr));
        T0_CHECK(sc.addCar({laneByEdge(g, 2), laneByEdge(g, 6)}, 0.0f, &serr));
        T0_CHECK(sc.addCar({laneByEdge(g, 0), laneByEdge(g, 4), laneByEdge(g, 7), laneByEdge(g, 3)}, 0.0f, &serr));
        for (int f = 0; f < 300; ++f) sc.step();
        T0_CHECK(sa.cars.size() == sc.cars.size());
        std::vector<nl::traffic::TrafficCarState> statesA, statesC;
        for (const nl::traffic::TrafficCar& c : sa.cars) statesA.push_back(c.state);
        for (const nl::traffic::TrafficCar& c : sc.cars) statesC.push_back(c.state);
        T0_CHECK(std::memcmp(statesA.data(), statesC.data(),
                             statesA.size() * sizeof(nl::traffic::TrafficCarState)) == 0);
        // ② 停车跟随：leader route [e0]（停车曲线刹在末端），follower 长路径；
        //    跟车律平衡间距 = minGap（+离散余量），40s 后双停、永不穿透、
        //    末间距落在 [1.0, 2.5]（minGap=2 + 离散 v*dt 滞后上界）
        nl::traffic::TrafficSim ss;
        ss.graph = &g;
        ss.params = p;
        T0_CHECK(ss.addCar({laneByEdge(g, 0)}, 0.0f, &serr));
        T0_CHECK(ss.addCar({laneByEdge(g, 0), laneByEdge(g, 4), laneByEdge(g, 7), laneByEdge(g, 3)}, 0.0f, &serr));
        for (int f = 0; f < 2400; ++f) {
            ss.step();
            T0_CHECK(ss.cars[1].state.lane != ss.cars[0].state.lane ||
                     ss.cars[1].state.s <= ss.cars[0].state.s); // 永不穿透
        }
        T0_CHECK(ss.cars[0].state.speed == 0.0f && ss.cars[0].state.s == 10.0f);
        T0_CHECK(ss.cars[1].state.speed == 0.0f);
        const float endGap = ss.cars[0].state.s - ss.cars[1].state.s;
        T0_CHECK(endGap >= 1.0f && endGap <= 2.5f);
        // ④ 限速帽：全部车全部帧 speed <= lane 限速（+1e-4 浮点容差）
        nl::traffic::TrafficSim sp;
        sp.graph = &g;
        sp.params = p;
        T0_CHECK(sp.addCar({laneByEdge(g, 1), laneByEdge(g, 5)}, 0.0f, &serr));
        T0_CHECK(sp.addCar({laneByEdge(g, 3), laneByEdge(g, 2)}, 0.0f, &serr));
        for (int f = 0; f < 600; ++f) {
            sp.step();
            for (const nl::traffic::TrafficCar& c : sp.cars)
                T0_CHECK(c.state.speed <=
                         g.lanes[static_cast<size_t>(c.state.lane)].speedLimit + 1e-4f);
        }
    }

    // --- 夹具落盘（ctest build 正向用例数据源，DEPENDS 链） ---
    if (!emitFixture.empty()) {
        if (!writeFile(emitFixture, fixture)) {
            std::fprintf(stderr, "traffic selftest: cannot write fixture %s\n",
                         emitFixture.c_str());
            return 1;
        }
        std::printf("wrote %s\n", emitFixture.c_str());
    }

    std::printf("traffic selftest: PASS (%d asserts)\n", g_asserts);
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  traffic-tools build <roadnet.json> [--dump out.json] [--stats]\n"
                 "  traffic-tools path <roadnet.json> --from <edgeId> --to <edgeId>\n"
                 "  traffic-tools lanes <roadnet.json> --png out.png [--size WxH]\n"
                 "  traffic-tools sim <roadnet.json> [--cars N] [--frames F] [--png out.png]\n"
                 "  traffic-tools selftest [--emit-fixture path]\n"
                 "  traffic-tools help\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];

    if (cmd == "help" || cmd == "--help") return usage();

    if (cmd == "selftest") {
        std::string emitFixture;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--emit-fixture") == 0 && i + 1 < argc) {
                emitFixture = argv[++i];
            } else if (std::strcmp(argv[i], "--verbose") == 0) {
                g_verbose = true; // 逐断言 ok 行（w0 证据日志口径）
            } else {
                return usage();
            }
        }
        return selftest(emitFixture);
    }

    if (cmd == "build") {
        if (argc < 3) return usage();
        const std::string roadnetPath = argv[2];
        std::string dumpPath;
        bool stats = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
                dumpPath = argv[++i];
            } else if (std::strcmp(argv[i], "--stats") == 0) {
                stats = true;
            } else {
                return usage();
            }
        }
        std::string body, err;
        if (!readFile(roadnetPath, &body, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        nl::traffic::LaneGraph g;
        if (!nl::traffic::buildLaneGraphFromRoadnetJson(body.data(), body.size(), &g, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        double lengthSum = 0.0;
        int linkCount = 0;
        for (const nl::traffic::Lane& l : g.lanes) {
            lengthSum += l.length;
            linkCount += static_cast<int>(l.outgoing.size());
        }
        std::printf("lanes       : %d\n", g.laneCount());
        std::printf("links       : %d (outgoing)\n", linkCount);
        std::printf("length sum  : %.1f m\n", lengthSum);
        if (stats) {
            double offAbsMax = 0.0;
            for (const nl::traffic::Lane& l : g.lanes)
                offAbsMax = std::fmax(offAbsMax, std::fabs(l.offset));
            std::printf("offset max  : %.3f m\n", offAbsMax);
        }
        if (!dumpPath.empty()) {
            if (!writeFile(dumpPath, nl::traffic::dumpLaneGraphJson(g))) {
                std::fprintf(stderr, "traffic-tools: cannot write %s\n", dumpPath.c_str());
                return 1;
            }
            std::printf("wrote %s\n", dumpPath.c_str());
        }
        return 0;
    }

    if (cmd == "path") {
        if (argc < 3) return usage();
        const std::string roadnetPath = argv[2];
        long fromEdge = -1, toEdge = -1;
        bool hasFrom = false, hasTo = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
                fromEdge = std::atol(argv[++i]);
                hasFrom = true;
            } else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
                toEdge = std::atol(argv[++i]);
                hasTo = true;
            } else {
                return usage();
            }
        }
        if (!hasFrom || !hasTo) return usage();
        std::string body, err;
        if (!readFile(roadnetPath, &body, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        nl::traffic::LaneGraph g;
        if (!nl::traffic::buildLaneGraphFromRoadnetJson(body.data(), body.size(), &g, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        // CLI 用 edgeId 语义（citygen 侧溯源键，CityCar.edgeId 同源）
        auto laneOfEdge = [&](long edgeId) -> int32_t {
            for (const nl::traffic::Lane& l : g.lanes)
                if (l.edgeId == static_cast<int32_t>(edgeId)) return l.id;
            return -1;
        };
        const int32_t from = laneOfEdge(fromEdge);
        const int32_t to = laneOfEdge(toEdge);
        if (from < 0 || to < 0) {
            std::fprintf(stderr, "traffic-tools: edge id %ld/%ld has no lane\n",
                         fromEdge, toEdge);
            return 1;
        }
        std::vector<int32_t> path;
        if (nl::traffic::findPathOnLanes(g, from, to, &path, &err) !=
            nl::traffic::PathStatus::Ok) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        std::string seq;
        for (size_t i = 0; i < path.size(); ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", path[i]);
            if (i) seq += " -> ";
            seq += buf;
        }
        std::printf("hops        : %d\n", static_cast<int>(path.size()));
        std::printf("lanes       : %s\n", seq.c_str());
        std::printf("length sum  : %.1f m\n",
                    nl::traffic::pathLength(g, path));
        return 0;
    }

    if (cmd == "lanes") {
        if (argc < 3) return usage();
        const std::string roadnetPath = argv[2];
        std::string pngPath;
        int w = 640, h = 480;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
                pngPath = argv[++i];
            } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
                if (std::sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w < 32 || h < 32)
                    return usage();
            } else {
                return usage();
            }
        }
        std::string body, err;
        if (!readFile(roadnetPath, &body, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        nl::traffic::LaneGraph g;
        if (!nl::traffic::buildLaneGraphFromRoadnetJson(body.data(), body.size(), &g, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        if (pngPath.empty()) return usage();
        Raster r;
        r.w = w;
        r.h = h;
        r.rgb.assign(static_cast<size_t>(w) * h * 3, 255);
        rasterLanes(r, g, worldMapFor(g, w, h));
        if (!writePng(pngPath, r)) {
            std::fprintf(stderr, "traffic-tools: cannot write %s\n", pngPath.c_str());
            return 1;
        }
        std::printf("lanes drawn : %d\n", g.laneCount());
        std::printf("wrote %s\n", pngPath.c_str());
        return 0;
    }

    if (cmd == "sim") {
        if (argc < 3) return usage();
        const std::string roadnetPath = argv[2];
        int nCars = 4, frames = 600;
        std::string pngPath;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--cars") == 0 && i + 1 < argc) {
                nCars = std::atol(argv[++i]);
            } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
                frames = std::atol(argv[++i]);
            } else if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
                pngPath = argv[++i];
            } else {
                return usage();
            }
        }
        if (nCars < 1 || frames < 1) return usage();
        std::string body, err;
        if (!readFile(roadnetPath, &body, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        nl::traffic::LaneGraph g;
        if (!nl::traffic::buildLaneGraphFromRoadnetJson(body.data(), body.size(), &g, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        // 确定性派车：第 i 辆 = 第 i 条边首车道出发，goal = 第 (i+n/2)%n 条边
        // 首车道（A* 路径；不可达回落单车道 route）。零 RNG，同输入同布车。
        const int nEdges = g.laneCount(); // 1 lane/edge 的契约内默认网格成立
        if (nCars > nEdges) {
            std::fprintf(stderr, "traffic-tools: --cars %d exceeds %d spawn lanes\n",
                         nCars, nEdges);
            return 1;
        }
        nl::traffic::TrafficSim sim;
        sim.graph = &g;
        for (int i = 0; i < nCars; ++i) {
            const int32_t start = laneByEdge(g, static_cast<int32_t>(i));
            const int32_t goal =
                laneByEdge(g, static_cast<int32_t>((i + nEdges / 2) % nEdges));
            std::vector<int32_t> route;
            if (nl::traffic::findPathOnLanes(g, start, goal, &route, &err) !=
                nl::traffic::PathStatus::Ok) {
                route.assign(1, start); // 不可达回落：原地单车道行驶
            }
            if (!sim.addCar(route, 0.0f, &err)) {
                std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
                return 1;
            }
        }
        for (int f = 0; f < frames; ++f) sim.step();
        int moving = 0;
        double distSum = 0.0;
        for (const nl::traffic::TrafficCar& c : sim.cars) {
            if (c.state.speed > 0.0f) ++moving;
            distSum += c.state.odometer;
        }
        std::printf("cars        : %d\n", static_cast<int>(sim.cars.size()));
        std::printf("frames      : %d (dt=%.4f)\n", frames, sim.params.dt);
        std::printf("moving end  : %d\n", moving);
        std::printf("distance    : %.1f m\n", distSum);
        if (!pngPath.empty()) {
            Raster r;
            r.w = 640;
            r.h = 480;
            r.rgb.assign(static_cast<size_t>(r.w) * r.h * 3, 255);
            const WorldMap m = worldMapFor(g, r.w, r.h);
            rasterLanes(r, g, m);
            for (const nl::traffic::TrafficCar& c : sim.cars) {
                nl::traffic::LanePoint p;
                if (!sim.carPosition(static_cast<size_t>(&c - sim.cars.data()), &p))
                    continue;
                // 3x3 红点（车流位置，终帧）
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        drawLine(r, m.px(p.x) + dx, m.py(p.z) + dy,
                                 m.px(p.x) + dx, m.py(p.z) + dy, 220, 30, 30);
            }
            if (!writePng(pngPath, r)) {
                std::fprintf(stderr, "traffic-tools: cannot write %s\n", pngPath.c_str());
                return 1;
            }
            std::printf("wrote %s\n", pngPath.c_str());
        }
        return 0;
    }

    return usage();
}
