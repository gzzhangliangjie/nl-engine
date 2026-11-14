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
#include "nl/traffic/TrafficCarSystem.hpp"  // P8 M-T3：ECS 交通车系统 selftest ⑪
#include "nl/scene/TrafficCarComponent.hpp" // P8 M-T3：组件序列化 roundtrip

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

    // --- ⑨ M-T1 调度器（A2 信号 + B1 占用锁）：时钟锚 + 锁单元锚 + 仿真锚 ---
    {
        std::string serr;
        // ① lock-only 构建：4 节点全受控（各 2 进入边），信号位 false 恒 Green；
        //   分组轴锚（node3 正交 e4=+X 组0 / e6=+Z 组1；node1 共线 e0/e5 同组0）
        nl::traffic::TrafficControl ctl;
        T0_CHECK(ctl.buildFromRoadnetJson(fixture.data(), fixture.size(), &g, &err));
        T0_CHECK(ctl.nodes.size() == 4);
        T0_CHECK(ctl.findNode(0) == 0 && ctl.findNode(3) == 3 && ctl.findNode(9) < 0);
        {
            const size_t n3 = static_cast<size_t>(ctl.findNode(3));
            const size_t n1 = static_cast<size_t>(ctl.findNode(1));
            // 方格轴锚：e4(1->3)=+Z 竖边 -> 组1；e6(2->3)=+X 横边 -> 组0
            T0_CHECK(ctl.nodes[n3].groups[0].size() == 1 &&
                     ctl.nodes[n3].groups[0][0] == 6);
            T0_CHECK(ctl.nodes[n3].groups[1].size() == 1 &&
                     ctl.nodes[n3].groups[1][0] == 4);
            T0_CHECK(ctl.nodes[n1].groups[0].size() == 1); // e0(+X) 组0
            T0_CHECK(ctl.nodes[n1].groups[1].size() == 1); // e5(-Z) 组1
            for (size_t i = 0; i < ctl.nodes.size(); ++i) {
                T0_CHECK(!ctl.nodes[i].signal);
                T0_CHECK(ctl.stateForEdge(i, 0) == nl::traffic::SignalState::Green);
            }
        }

        // ② 信号时钟锚（A2 黄灯）：node3 信号化，g=0.5 y=0.2 c=0.1（周期 1.6s）
        std::string sigFixture = fixture;
        const std::string n3from =
            "\"id\": 3, \"x\": 10.0, \"z\": 10.0, \"kind\": \"intersection\", \"signal\": false";
        const std::string n3to =
            "\"id\": 3, \"x\": 10.0, \"z\": 10.0, \"kind\": \"intersection\", \"signal\": true";
        T0_CHECK(sigFixture.find(n3from) != std::string::npos);
        sigFixture.replace(sigFixture.find(n3from), n3from.size(), n3to);
        nl::traffic::TrafficControl cs;
        T0_CHECK(cs.buildFromRoadnetJson(sigFixture.data(), sigFixture.size(), &g, &err));
        const size_t n3 = static_cast<size_t>(cs.findNode(3));
        T0_CHECK(cs.nodes[n3].signal);
        cs.nodes[n3].greenS = 0.5f;
        cs.nodes[n3].yellowS = 0.2f;
        cs.nodes[n3].clearS = 0.1f;
        cs.frame = 0; // t=0.000 -> 组0(e6 横向) 绿 / 组1(e4 纵向) 红
        T0_CHECK(cs.stateForEdge(n3, 6) == nl::traffic::SignalState::Green);
        T0_CHECK(cs.stateForEdge(n3, 4) == nl::traffic::SignalState::Red);
        cs.frame = 30; // t=0.500 -> 组0 黄（黄灯不放行新进入的判定在 sim 侧）
        T0_CHECK(cs.stateForEdge(n3, 6) == nl::traffic::SignalState::Yellow);
        cs.frame = 42; // t=0.700 -> 全红清空
        T0_CHECK(cs.stateForEdge(n3, 6) == nl::traffic::SignalState::Red);
        T0_CHECK(cs.stateForEdge(n3, 4) == nl::traffic::SignalState::Red);
        cs.frame = 48; // t=0.800 -> 组1 绿
        T0_CHECK(cs.stateForEdge(n3, 4) == nl::traffic::SignalState::Green);
        T0_CHECK(cs.stateForEdge(n3, 6) == nl::traffic::SignalState::Red);
        cs.frame = 78; // t=1.300 -> 组1 黄
        T0_CHECK(cs.stateForEdge(n3, 4) == nl::traffic::SignalState::Yellow);
        cs.frame = 96; // t=1.600 -> 周期回卷组0 绿
        T0_CHECK(cs.stateForEdge(n3, 6) == nl::traffic::SignalState::Green);
        // phases 契约覆盖（只加不减）：node3 green_s=1.0 生效；未知节点负向
        std::string phOk = sigFixture;
        phOk.replace(phOk.find("\"phases\": []"), 12,
                     "\"phases\": [{\"node\": 3, \"green_s\": 1.0}]");
        nl::traffic::TrafficControl cp;
        T0_CHECK(cp.buildFromRoadnetJson(phOk.data(), phOk.size(), &g, &err));
        T0_CHECK(cp.nodes[static_cast<size_t>(cp.findNode(3))].greenS == 1.0f);
        std::string phBad = sigFixture;
        phBad.replace(phBad.find("\"phases\": []"), 12, "\"phases\": [{\"node\": 9}]");
        T0_CHECK(!cp.buildFromRoadnetJson(phBad.data(), phBad.size(), &g, &err));
        T0_CHECK(!err.empty());

        // ③ 锁单元锚（B1 FIFO，M-T2 组锁接口；同组申请序 = 旧单持有器语义）：
        // 授予 / 入队 / 队首优先 / 越圈释放 / 未越圈不释放
        nl::traffic::TrafficControl cl;
        T0_CHECK(cl.buildFromRoadnetJson(fixture.data(), fixture.size(), &g, &err));
        const size_t nl3 = static_cast<size_t>(cl.findNode(3));
        T0_CHECK(cl.acquire(nl3, 5, 0, false)); // 空闲 -> 授予（组 0 共享集）
        T0_CHECK(cl.hasAccess(nl3, 5));
        T0_CHECK(cl.acquire(nl3, 6, 0, false)); // 同组第二车 = 直接共享授予（M-T2 并行）
        T0_CHECK(cl.hasAccess(nl3, 6));
        T0_CHECK(cl.acquire(nl3, 5, 0, false)); // 已持有重复申请 = true
        T0_CHECK(!cl.acquire(nl3, 7, 1, false)); // 跨组：g0 非空 -> 入队
        T0_CHECK(!cl.acquire(nl3, 8, 0, true)); // 转弯：组非空 -> 入队
        T0_CHECK(!cl.hasAccess(nl3, 7) && !cl.hasAccess(nl3, 8));
        cl.releaseCleared(5, 100.0f, 100.0f); // 远离 -> 释放车 5
        T0_CHECK(!cl.hasAccess(nl3, 5));
        T0_CHECK(cl.hasAccess(nl3, 6)); // 6 仍在共享集
        T0_CHECK(!cl.hasAccess(nl3, 7)); // 跨组仍互斥（6 未清）
        cl.releaseCleared(6, 100.0f, 100.0f);
        T0_CHECK(cl.hasAccess(nl3, 7)); // g0 清空 -> 队首 7（组 1）授予
        T0_CHECK(!cl.hasAccess(nl3, 8)); // turn 等待者存在（8 在 7 之前入队? 否——
        // 8 在 7 之后入队；FIFO 队首已到 7，7 授予后队首 = 8，组授予暂停逻辑
        // 只影响新组申请；此处 8 尚未轮到）
        cl.releaseCleared(7, 100.0f, 100.0f);
        T0_CHECK(cl.hasAccess(nl3, 8)); // 全清 -> 队首 8 转弯独占授予
        T0_CHECK(!cl.acquire(nl3, 9, 0, false)); // 独占期组申请入队
        cl.releaseCleared(8, 100.0f, 100.0f);
        T0_CHECK(cl.hasAccess(nl3, 9)); // turn 清空 -> 9 授予

        // ④ 仿真锚：红灯刹停（大 clear 窗口锁死长红），停车线 10-2.5=7.5 逐位
        {
            nl::traffic::TrafficControl c4;
            T0_CHECK(c4.buildFromRoadnetJson(sigFixture.data(), sigFixture.size(), &g, &err));
            const size_t m3 = static_cast<size_t>(c4.findNode(3));
            c4.nodes[m3].greenS = 0.5f;
            c4.nodes[m3].yellowS = 0.1f;
            c4.nodes[m3].clearS = 3599.0f; // t>=0.6s 起组0 长红
            c4.frame = 35;
            nl::traffic::TrafficSim s4;
            s4.graph = &g;
            s4.control = &c4;
            T0_CHECK(s4.addCar({laneByEdge(g, 4), laneByEdge(g, 5)}, 0.0f, &serr));
            for (int f = 0; f < 600; ++f) s4.step();
            T0_CHECK(s4.cars[0].state.lane == laneByEdge(g, 4)); // 未放行未穿
            T0_CHECK(s4.cars[0].state.s == 7.5f); // 硬钳停车线逐位
            T0_CHECK(s4.cars[0].state.speed == 0.0f);
            T0_CHECK(s4.cars[0].haltFrames > 0);
        }

        // ⑤ 仿真锚：冲突双车串行过锁（车序 0 先到先得）+ 全帧跨车道互斥下界
        nl::traffic::TrafficSim s5;
        s5.graph = &g;
        s5.control = &ctl; // lock-only（① 构建）
        T0_CHECK(s5.addCar({laneByEdge(g, 4), laneByEdge(g, 5)}, 0.0f, &serr));
        T0_CHECK(s5.addCar({laneByEdge(g, 6), laneByEdge(g, 7)}, 0.0f, &serr));
        float minPair2 = 1e30f;
        for (int f = 0; f < 900; ++f) {
            s5.step();
            for (size_t a = 0; a < s5.cars.size(); ++a)
                for (size_t b = a + 1; b < s5.cars.size(); ++b)
                    if (s5.cars[a].state.lane != s5.cars[b].state.lane) {
                        nl::traffic::LanePoint pa, pb;
                        T0_CHECK(s5.carPosition(a, &pa) && s5.carPosition(b, &pb));
                        const float dx = pa.x - pb.x;
                        const float dz = pa.z - pb.z;
                        minPair2 = std::fmin(minPair2, dx * dx + dz * dz);
                    }
        }
        T0_CHECK(std::sqrt(minPair2) >= 1.0f); // 跨车道不穿模（C1 检查下界 1.0m）
        T0_CHECK(s5.cars[0].routeIdx == 1); // 双双过锁
        T0_CHECK(s5.cars[1].routeIdx == 1);
        T0_CHECK(s5.cars[0].haltFrames == 0); // 先到者（车序 0）免停
        T0_CHECK(s5.cars[1].haltFrames > 0); // 后到者等锁

        // ⑥ 控制全开双跑逐位（状态数组 memcmp + 停滞计数）
        nl::traffic::TrafficSim s6;
        s6.graph = &g;
        s6.control = &ctl;
        T0_CHECK(s6.addCar({laneByEdge(g, 4), laneByEdge(g, 5)}, 0.0f, &serr));
        T0_CHECK(s6.addCar({laneByEdge(g, 6), laneByEdge(g, 7)}, 0.0f, &serr));
        for (int f = 0; f < 900; ++f) s6.step();
        T0_CHECK(s6.cars.size() == s5.cars.size());
        for (size_t i = 0; i < s6.cars.size(); ++i) {
            T0_CHECK(std::memcmp(&s6.cars[i].state, &s5.cars[i].state,
                                 sizeof(nl::traffic::TrafficCarState)) == 0);
            T0_CHECK(s6.cars[i].haltFrames == s5.cars[i].haltFrames);
        }
    }

    // --- ⑩ M-T2 调度器增强（Q-T9 组锁 / Q-T10 gap-out / Q-T11 loop）：锚 ---
    {
        std::string terr;
        std::string serr;
        // 信号化夹具（同 ② 口径：node3 signal=true；replace 长度一律 needle.size()）
        std::string sigFixture = fixture;
        const std::string sigNeedle =
            "\"id\": 3, \"x\": 10.0, \"z\": 10.0, \"kind\": \"intersection\", \"signal\": false";
        sigFixture.replace(sigFixture.find(sigNeedle), sigNeedle.size(),
                           "\"id\": 3, \"x\": 10.0, \"z\": 10.0, \"kind\": "
                           "\"intersection\", \"signal\": true");
        // a) gap-out 纯控制锚：绿相零需求持续 gapOutS -> 提前切黄（整数帧确定）
        //    sigFixture node3：e6 组 0 / e4 组 1；greenS=8s(480帧) minGreen=2s(120帧)
        //    gapOutS=0.5s(30帧)：inPhase 120..149 累计 idle 1..30，frame=149 时触发
        //    跳帧 delta = 480-149 = 331 -> frame=480（黄起点）-> ++frame = 481
        nl::traffic::TrafficControl cg;
        T0_CHECK(cg.buildFromRoadnetJson(sigFixture.data(), sigFixture.size(), &g, &terr));
        const size_t g3 = static_cast<size_t>(cg.findNode(3));
        cg.nodes[g3].greenS = 8.0f;
        cg.nodes[g3].yellowS = 2.0f;
        cg.nodes[g3].clearS = 1.0f;
        cg.nodes[g3].gapOutS = 0.5f;
        for (int f = 0; f < 150; ++f) cg.advanceFrame();
        T0_CHECK(cg.frame == 481); // 固定周期应为 150；感应提前到黄
        T0_CHECK(cg.stateForEdge(g3, 6) == nl::traffic::SignalState::Yellow);
        // 需求反锚：绿相每帧上报等待 -> 不触发 gap-out，自然周期切换
        nl::traffic::TrafficControl cd;
        T0_CHECK(cd.buildFromRoadnetJson(sigFixture.data(), sigFixture.size(), &g, &terr));
        const size_t d3 = static_cast<size_t>(cd.findNode(3));
        cd.nodes[d3].gapOutS = 0.5f;
        for (int f = 0; f < 700; ++f) {
            cd.reportWaiting(d3, 6); // 组 0 持续有车在线等
            cd.advanceFrame();
        }
        T0_CHECK(cd.frame == 700); // 无跳帧
        T0_CHECK(cd.stateForEdge(d3, 4) == nl::traffic::SignalState::Green); // 组 1 正常接续
        // 最短绿时防抖：绿相前 120 帧内不触发（window [minGreen, gf) 语义）
        nl::traffic::TrafficControl cm;
        T0_CHECK(cm.buildFromRoadnetJson(sigFixture.data(), sigFixture.size(), &g, &terr));
        const size_t m3c = static_cast<size_t>(cm.findNode(3));
        cm.nodes[m3c].gapOutS = 0.01f; // 1 帧即触发阈值
        for (int f = 0; f < 119; ++f) cm.advanceFrame();
        T0_CHECK(cm.frame == 119); // minGreen 窗口前零跳帧

        // b) loop 环路线锚：方格夹具环 [e0,e4,e7,e3]（0->1->3->2->0 闭合）
        //    30s odometer > 50m（环长 40m = 至少 1.25 圈）+ 双跑 memcmp 逐位
        nl::traffic::TrafficSim sl;
        sl.graph = &g;
        sl.params.loop = true;
        const std::vector<int32_t> ring = {laneByEdge(g, 0), laneByEdge(g, 4),
                                           laneByEdge(g, 7), laneByEdge(g, 3)};
        T0_CHECK(sl.addCar(ring, 0.0f, &serr));
        for (int f = 0; f < 1800; ++f) sl.step();
        T0_CHECK(sl.cars[0].state.odometer > 50.0f);
        T0_CHECK(sl.cars[0].state.speed > 0.0f); // 环上永不停（lock-only 全过）
        nl::traffic::TrafficSim sl2;
        sl2.graph = &g;
        sl2.params.loop = true;
        T0_CHECK(sl2.addCar(ring, 0.0f, &serr));
        for (int f = 0; f < 1800; ++f) sl2.step();
        T0_CHECK(std::memcmp(&sl.cars[0].state, &sl2.cars[0].state,
                             sizeof(nl::traffic::TrafficCarState)) == 0);
        // loop=false 对照：同 route 单程停靠（odometer < 环长）
        nl::traffic::TrafficSim sn;
        sn.graph = &g;
        T0_CHECK(sn.addCar(ring, 0.0f, &serr));
        for (int f = 0; f < 1800; ++f) sn.step();
        T0_CHECK(sn.cars[0].state.speed == 0.0f);
        T0_CHECK(sl.cars[0].state.odometer > sn.cars[0].state.odometer);

        // c) 转弯独占仿真锚：e0(0->1 x) -> e4(1->3 z) = 90° 转弯 -> 节点独占；
        //    跨组直行车与转弯车串行过锁（不并行不穿模）+ 双跑一致
        nl::traffic::TrafficControl ct;
        T0_CHECK(ct.buildFromRoadnetJson(fixture.data(), fixture.size(), &g, &terr));
        nl::traffic::TrafficSim s7;
        s7.graph = &g;
        s7.control = &ct;
        T0_CHECK(s7.addCar({laneByEdge(g, 0), laneByEdge(g, 4)}, 0.0f, &serr)); // 转弯
        T0_CHECK(s7.addCar({laneByEdge(g, 2), laneByEdge(g, 6)}, 0.0f, &serr)); // e2->e6 转弯
        for (int f = 0; f < 900; ++f) s7.step();
        T0_CHECK(ct.heldNodeCount() == 0); // 无锁泄漏（独占与共享释放对称）
        nl::traffic::TrafficSim s8;
        s8.graph = &g;
        s8.control = &ct;
        T0_CHECK(s8.addCar({laneByEdge(g, 0), laneByEdge(g, 4)}, 0.0f, &serr));
        T0_CHECK(s8.addCar({laneByEdge(g, 2), laneByEdge(g, 6)}, 0.0f, &serr));
        for (int f = 0; f < 900; ++f) s8.step();
        T0_CHECK(std::memcmp(&s7.cars[0].state, &s8.cars[0].state,
                             sizeof(nl::traffic::TrafficCarState)) == 0);
        T0_CHECK(std::memcmp(&s7.cars[1].state, &s8.cars[1].state,
                             sizeof(nl::traffic::TrafficCarState)) == 0);
    }

    // --- ⑪ M-T3 TrafficCarSystem（TRAFFIC-PLAN §11）：ECS 写回 + 确定性 + 序列化 ---
    {
        // a) init + tick 写回：3 实体挂 TrafficCar+Transform，N tick 后位置落在
        //    lane 几何上（≈折线插值）、yaw 四元数单位长度、stepCount 累加。
        const int kCars = 3;
        const int kTicks = 120;   // 2s 逻辑步
        auto buildWorld = [&](nl::scene::World* w) {
            auto& clib = w->getComponentLibrary();
            clib.registerComponent<nl::scene::TransformComponent>();
            clib.registerComponent<nl::scene::TrafficCarComponent>();
            for (int k = 0; k < kCars; ++k) {
                auto e = w->newEntity("car_0" + std::to_string(k));
                e.addComponent(std::make_unique<nl::scene::TransformComponent>(e));
                e.addComponent(std::make_unique<nl::scene::TrafficCarComponent>(e));
            }
            w->flushEntityCreationAndRemoval();
            w->registerSystem(std::make_unique<nl::traffic::TrafficCarSystem>(*w));
        };
        nl::scene::World w1;
        buildWorld(&w1);
        auto* sys1 = static_cast<nl::traffic::TrafficCarSystem*>(
            w1.findSystem(nl::traffic::TrafficCarSystem::kName));
        T0_CHECK(sys1 != nullptr);
        nl::traffic::TrafficSimParams sp{};   // 默认参数，loop 由 init 覆写
        std::string ierr;
        T0_CHECK(sys1->init(g, sp, 12345, ierr));
        T0_CHECK(sys1->ready());
        T0_CHECK(sys1->carCount() == kCars);
        T0_CHECK(sys1->stepCount() == 0);
        for (int f = 0; f < kTicks; ++f) w1.tick(1.0 / 60.0);
        T0_CHECK(sys1->stepCount() == static_cast<std::uint64_t>(kTicks));
        for (int k = 0; k < kCars; ++k) {
            auto eo = w1.findEntityByName("car_0" + std::to_string(k));
            T0_CHECK(eo.has_value());
            const auto e = *eo;
            const auto* tc = w1.getComponent<nl::scene::TrafficCarComponent>(e.getID());
            T0_CHECK(tc != nullptr);
            T0_CHECK(tc->route.size() >= 1u);           // route 落表
            T0_CHECK(tc->speed > 0.0f);                 // 2s 后已起步（lock-only 夹具）
            const auto* t = w1.getComponent<nl::scene::TransformComponent>(e.getID());
            T0_CHECK(t != nullptr);
            // 位置在夹具路网包围盒内（夹具 nodes x,z ∈ [0,10]）
            T0_CHECK(t->local.position.x >= -1.0f && t->local.position.x <= 11.0f);
            T0_CHECK(t->local.position.z >= -1.0f && t->local.position.z <= 11.0f);
            // yaw 四元数单位长度（normalizeQuat 语义：|q| ≈ 1）
            const auto& q = t->local.rotation;
            const float qlen = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            T0_CHECK(std::fabs(qlen - 1.0f) < 1e-4f);
        }

        // b) 确定性：同 seed 重建 + 同 tick 数 -> Transform/组件逐位一致
        nl::scene::World w2;
        buildWorld(&w2);
        auto* sys2 = static_cast<nl::traffic::TrafficCarSystem*>(
            w2.findSystem(nl::traffic::TrafficCarSystem::kName));
        std::string ierr2;
        T0_CHECK(sys2->init(g, sp, 12345, ierr2));
        for (int f = 0; f < kTicks; ++f) w2.tick(1.0 / 60.0);
        for (int k = 0; k < kCars; ++k) {
            auto e1 = w1.findEntityByName("car_0" + std::to_string(k));
            auto e2 = w2.findEntityByName("car_0" + std::to_string(k));
            T0_CHECK(e1.has_value() && e2.has_value());
            const auto* t1 = w1.getComponent<nl::scene::TransformComponent>(e1->getID());
            const auto* t2 = w2.getComponent<nl::scene::TransformComponent>(e2->getID());
            T0_CHECK(std::memcmp(&t1->local, &t2->local, sizeof(t1->local)) == 0);
            const auto* c1 = w1.getComponent<nl::scene::TrafficCarComponent>(e1->getID());
            const auto* c2 = w2.getComponent<nl::scene::TrafficCarComponent>(e2->getID());
            T0_CHECK(c1->route == c2->route);
            T0_CHECK(std::memcmp(&c1->s, &c2->s, sizeof(float)) == 0);
            T0_CHECK(std::memcmp(&c1->speed, &c2->speed, sizeof(float)) == 0);
        }

        // c) 组件序列化 roundtrip（M-E2 纪律：float->json->float 无损）
        {
            auto e1 = w1.findEntityByName("car_00");
            T0_CHECK(e1.has_value());
            const auto* c1 = w1.getComponent<nl::scene::TrafficCarComponent>(e1->getID());
            const auto j = c1->serialise();
            const std::string jdump = j.dump();
            nl::scene::TrafficCarComponent c2(nl::scene::Entity{e1->getID(), &w1});
            c2.deserialise(nlohmann::json::parse(jdump));
            T0_CHECK(c2.route == c1->route);
            T0_CHECK(std::memcmp(&c2.s, &c1->s, sizeof(float)) == 0);
            T0_CHECK(std::memcmp(&c2.speed, &c1->speed, sizeof(float)) == 0);
            T0_CHECK(c2.variant == c1->variant && c2.enabled == c1->enabled);
        }

        // d) 卡顿帧防爆：单次 tick(10/60) 最多补 kMaxStepsPerTick 步
        nl::scene::World w3;
        buildWorld(&w3);
        auto* sys3 = static_cast<nl::traffic::TrafficCarSystem*>(
            w3.findSystem(nl::traffic::TrafficCarSystem::kName));
        std::string ierr3;
        T0_CHECK(sys3->init(g, sp, 777, ierr3));
        w3.tick(10.0 / 60.0);
        T0_CHECK(sys3->stepCount() ==
                 static_cast<std::uint64_t>(nl::traffic::TrafficCarSystem::kMaxStepsPerTick));

        // e) 负向：无 enabled 实体 = init 显式失败（不静默）
        {
            nl::scene::World w4;
            auto& clib = w4.getComponentLibrary();
            clib.registerComponent<nl::scene::TransformComponent>();
            clib.registerComponent<nl::scene::TrafficCarComponent>();
            auto e = w4.newEntity("car_00");
            e.addComponent(std::make_unique<nl::scene::TransformComponent>(e));
            auto comp = std::make_unique<nl::scene::TrafficCarComponent>(e);
            comp->enabled = false;
            e.addComponent(std::move(comp));
            w4.flushEntityCreationAndRemoval();
            w4.registerSystem(std::make_unique<nl::traffic::TrafficCarSystem>(w4));
            auto* sys4 = static_cast<nl::traffic::TrafficCarSystem*>(
                w4.findSystem(nl::traffic::TrafficCarSystem::kName));
            std::string ierr4;
            T0_CHECK(!sys4->init(g, sp, 12345, ierr4));
            T0_CHECK(ierr4.find("no enabled TrafficCar entities") != std::string::npos);
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
                 "                                    [--loop] [--actuated]\n"
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
        bool loopMode = false, actuated = false;
        std::string pngPath;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--cars") == 0 && i + 1 < argc) {
                nCars = std::atol(argv[++i]);
            } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
                frames = std::atol(argv[++i]);
            } else if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
                pngPath = argv[++i];
            } else if (std::strcmp(argv[i], "--loop") == 0) { // M-T2.2 环路线
                loopMode = true;
            } else if (std::strcmp(argv[i], "--actuated") == 0) { // M-T2.1 gap-out 2s
                actuated = true;
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
        nl::traffic::TrafficControl ctrl;
        if (!ctrl.buildFromRoadnetJson(body.data(), body.size(), &g, &err)) {
            std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
            return 1;
        }
        if (actuated) // M-T2.1：全信号节点开 gap-out 2s（phases 契约的 CLI 等价位）
            for (nl::traffic::NodeControl& n : ctrl.nodes)
                if (n.signal) n.gapOutS = 2.0f;
        nl::traffic::TrafficSim sim;
        sim.graph = &g;
        sim.control = &ctrl; // M-T1：路口控制面（信号相位 + 占用锁）默认挂接
        sim.params.loop = loopMode; // M-T2.2：环路线（环闭合才回绕）
        int looped = 0;
        for (int i = 0; i < nCars; ++i) {
            const int32_t start = laneByEdge(g, static_cast<int32_t>(i));
            const int32_t goal =
                laneByEdge(g, static_cast<int32_t>((i + nEdges / 2) % nEdges));
            std::vector<int32_t> route;
            if (nl::traffic::findPathOnLanes(g, start, goal, &route, &err) !=
                nl::traffic::PathStatus::Ok) {
                route.assign(1, start); // 不可达回落：原地单车道行驶
            }
            // M-T2.2 环路线：goal -> start 回程 path 去尾拼接（闭合判定在
            // sim::loopActive；单车道回落不可环）。不可环回落单程并显式报告。
            if (loopMode && route.size() >= 2) {
                std::vector<int32_t> back;
                if (nl::traffic::findPathOnLanes(g, route.back(), start, &back,
                                                 &err) ==
                    nl::traffic::PathStatus::Ok) {
                    // back 首元 = route.back（衔接去重）、末元 = start；
                    // 去掉末元 start 后闭合（末前驱 toNode == start.fromNode
                    // 由 A* 相邻性保证）
                    if (back.size() >= 2) {
                        for (size_t k = 1; k + 1 < back.size(); ++k)
                            route.push_back(back[k]);
                        ++looped;
                    }
                }
            }
            // 出生点外移：s=0 = 节点圆心区（clearRadius 内）无锁占位会与穿行车
            // 假碰撞——出生在区外（>=4m 或停车线内 1m 取小）
            const nl::traffic::Lane& sl = g.lanes[static_cast<size_t>(start)];
            const float stopLine =
                ctrl.findNode(sl.toNode) >= 0 ? sl.length - ctrl.entryStop : sl.length;
            const float spawnS = std::fmin(4.0f, std::fmax(stopLine - 1.0f, 0.0f));
            if (!sim.addCar(route, spawnS, &err)) {
                std::fprintf(stderr, "traffic-tools: %s\n", err.c_str());
                return 1;
            }
        }
        // 逐帧互斥扫描（C1 口径）：只查不同 lane 且**不同路段**的车对——
        // 同路段对向车道会车（横距 = 两 lane offset 和 ≈ 0.5m）是正常交通不是
        // 穿模；同 lane 对归跟车律锚。违反下界 1.0m = 穿模，显式退出 1。
        auto sameRoad = [](const nl::traffic::Lane& a, const nl::traffic::Lane& b) {
            const int32_t a0 = a.fromNode < a.toNode ? a.fromNode : a.toNode;
            const int32_t a1 = a.fromNode < a.toNode ? a.toNode : a.fromNode;
            const int32_t b0 = b.fromNode < b.toNode ? b.fromNode : b.toNode;
            const int32_t b1 = b.fromNode < b.toNode ? b.toNode : b.fromNode;
            return a0 == b0 && a1 == b1;
        };
        // M-T2.1 对向豁免升级：sameRoad 只认同一对节点——同一条街相邻路段
        //（共用路口节点）的对向车道不覆盖（城市实测：共线街 e4(+X)/e7(-X) 在
        // node3 两侧 0.557m = lane offset 设计值误报碰撞）。几何判据：
        // 方向精确相反（dot <= -0.999）且共线（|叉积|/|a| < 0.75m = offset 和
        // 0.5 + 余量；最小平行街距 4m 不受影响）= 同一条街的对向车道 -> 豁免。
        auto oppositeLane = [](const nl::traffic::Lane& a, const nl::traffic::Lane& b,
                               const nl::traffic::LanePoint& pa,
                               const nl::traffic::LanePoint& pb) {
            const float dx1 = a.points.back().x - a.points.front().x;
            const float dz1 = a.points.back().z - a.points.front().z;
            const float dx2 = b.points.back().x - b.points.front().x;
            const float dz2 = b.points.back().z - b.points.front().z;
            const float l1 = std::sqrt(dx1 * dx1 + dz1 * dz1);
            const float l2 = std::sqrt(dx2 * dx2 + dz2 * dz2);
            if (l1 <= 0.0f || l2 <= 0.0f) return false;
            if ((dx1 * dx2 + dz1 * dz2) / (l1 * l2) > -0.999f) return false;
            const float cross =
                dx1 * (pb.z - a.points.front().z) - dz1 * (pb.x - a.points.front().x);
            return std::fabs(cross) / l1 < 0.75f;
        };
        float minPair2 = 1e30f;
        int minPairFrame = -1;
        struct PairInfo {
            int lane, edge, fn, tn;
            float x, z, spd;
        } minPairA{}, minPairB{};
        for (int f = 0; f < frames; ++f) {
            sim.step();
            for (size_t a = 0; a < sim.cars.size(); ++a)
                for (size_t b = a + 1; b < sim.cars.size(); ++b) {
                    const nl::traffic::Lane& la =
                        g.lanes[static_cast<size_t>(sim.cars[a].state.lane)];
                    const nl::traffic::Lane& lb =
                        g.lanes[static_cast<size_t>(sim.cars[b].state.lane)];
                    if (la.id == lb.id) continue;
                    nl::traffic::LanePoint pa, pb;
                    if (!sim.carPosition(a, &pa) || !sim.carPosition(b, &pb)) continue;
                    if (sameRoad(la, lb) || oppositeLane(la, lb, pa, pb)) continue;
                    const float dx = pa.x - pb.x;
                    const float dz = pa.z - pb.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < minPair2) {
                        minPair2 = d2;
                        // 实证诊断：记录最小对几何（帧 / 两车 lane 与位置）
                        minPairFrame = f;
                        minPairA = {la.id, la.edgeId, la.fromNode, la.toNode, pa.x, pa.z,
                                    sim.cars[a].state.speed};
                        minPairB = {lb.id, lb.edgeId, lb.fromNode, lb.toNode, pb.x, pb.z,
                                    sim.cars[b].state.speed};
                    }
                }
        }
        int moving = 0;
        int arrived = 0;
        int maxHalt = 0;
        double distSum = 0.0;
        for (const nl::traffic::TrafficCar& c : sim.cars) {
            if (c.state.speed > 0.0f) ++moving;
            distSum += c.state.odometer;
            if (c.haltFrames > maxHalt) maxHalt = c.haltFrames;
            const nl::traffic::Lane& last =
                g.lanes[static_cast<size_t>(c.route.back())];
            const bool capped = ctrl.findNode(last.toNode) >= 0;
            const float cap = (capped ? last.length - ctrl.entryStop : last.length);
            if (c.routeIdx + 1 == c.route.size() && c.state.s >= cap - 1e-3f)
                ++arrived; // 到达停靠帽（仅 route 末 lane 上的车计入）
        }
        int signalNodes = 0;
        for (const nl::traffic::NodeControl& n : ctrl.nodes)
            if (n.signal) ++signalNodes;
        std::printf("cars        : %d\n", static_cast<int>(sim.cars.size()));
        std::printf("frames      : %d (dt=%.4f)\n", frames, sim.params.dt);
        std::printf("signals     : %d (lock-only %d / signal %d)\n",
                    static_cast<int>(ctrl.nodes.size()),
                    static_cast<int>(ctrl.nodes.size()) - signalNodes, signalNodes);
        if (loopMode)
            std::printf("loop routes : %d / %d (closed rings)\n", looped, nCars);
        std::printf("moving end  : %d\n", moving);
        std::printf("arrived     : %d / %d\n", arrived,
                    static_cast<int>(sim.cars.size()));
        std::printf("distance    : %.1f m\n", distSum);
        int lockedEnd = ctrl.heldNodeCount();
        std::printf("min gap     : %.2f m (cross-road)\n",
                     minPair2 < 1e29f ? std::sqrt(minPair2) : -1.0f);
        std::printf("locked end  : %d\n", lockedEnd);
        std::printf("max halt    : %.1f s\n",
                    static_cast<float>(maxHalt) * sim.params.dt);
        if (minPair2 < 1.0f) {
            std::fprintf(stderr,
                         "traffic-tools: cross-road collision (%.3f m < 1.0 m)\n",
                         std::sqrt(minPair2));
            std::fprintf(stderr,
                         "  closest pair @frame %d:\n"
                         "    A lane %d (e%d %d->%d) pos=(%.2f,%.2f) v=%.2f\n"
                         "    B lane %d (e%d %d->%d) pos=(%.2f,%.2f) v=%.2f\n"
                         "    sameRoad=%d\n",
                         minPairFrame, minPairA.lane, minPairA.edge, minPairA.fn,
                         minPairA.tn, minPairA.x, minPairA.z, minPairA.spd,
                         minPairB.lane, minPairB.edge, minPairB.fn, minPairB.tn,
                         minPairB.x, minPairB.z, minPairB.spd,
                         static_cast<int>(minPairA.fn == minPairB.fn &&
                                          minPairA.tn == minPairB.tn) ||
                             static_cast<int>(minPairA.fn == minPairB.tn &&
                                              minPairA.tn == minPairB.fn));
            return 1;
        }
        // 死锁判定（M-T2 修正）：持锁车「停滞在圈内」（speed==0 且距节点
        // <= clearEff）才算死锁；loop 环上车流不消散，终帧持锁穿越车
        // （speed>0）是正常交通不是死锁。
        int stalledHeld = 0;
        for (size_t i = 0; i < ctrl.nodes.size(); ++i) {
            const nl::traffic::NodeControl& nn = ctrl.nodes[i];
            auto stalled = [&](int32_t h) {
                if (h < 0 || static_cast<size_t>(h) >= sim.cars.size()) return false;
                const nl::traffic::TrafficCar& c = sim.cars[static_cast<size_t>(h)];
                if (c.state.speed != 0.0f) return false;
                nl::traffic::LanePoint pp;
                if (!sim.carPosition(static_cast<size_t>(h), &pp)) return false;
                const float d2 = (pp.x - nn.x) * (pp.x - nn.x) +
                                 (pp.z - nn.z) * (pp.z - nn.z);
                return d2 <= nn.clearEff * nn.clearEff;
            };
            if (stalled(ctrl.turnHolder[i])) ++stalledHeld;
            for (const int32_t h : ctrl.groupHolders[0][i])
                if (stalled(h)) ++stalledHeld;
            for (const int32_t h : ctrl.groupHolders[1][i])
                if (stalled(h)) ++stalledHeld;
        }
        if (stalledHeld != 0) {
            for (size_t i = 0; i < ctrl.nodes.size(); ++i) {
                bool held = ctrl.turnHolder[i] >= 0 ||
                            !ctrl.groupHolders[0][i].empty() ||
                            !ctrl.groupHolders[1][i].empty();
                if (!held) continue;
                const nl::traffic::NodeControl& nn = ctrl.nodes[i];
                // 诊断行：逐持有车 dump 状态（组共享可能多车）
                auto dumpCar = [&](int32_t h) {
                    if (h < 0 ||
                        static_cast<size_t>(h) >= sim.cars.size())
                        return;
                    const nl::traffic::TrafficCar& c =
                        sim.cars[static_cast<size_t>(h)];
                    const nl::traffic::Lane& cl =
                        g.lanes[static_cast<size_t>(c.state.lane)];
                    nl::traffic::LanePoint pp;
                    sim.carPosition(static_cast<size_t>(h), &pp);
                    std::printf(
                        "LOCK node %d (in e%d/e%d) held by car %d: lane %d"
                        "(e%d %d->%d) s=%.2f speed=%.2f route %d/%d"
                        " pos=(%.1f,%.1f) distToNode=%.2f\n",
                        nn.nodeId,
                        nn.groups[0].empty() ? -1 : nn.groups[0].front(),
                        nn.groups[1].empty() ? -1 : nn.groups[1].front(),
                        h, cl.id, cl.edgeId, cl.fromNode, cl.toNode,
                        c.state.s, c.state.speed, c.routeIdx,
                        static_cast<int>(c.route.size()), pp.x, pp.z,
                        std::sqrt((pp.x - nn.x) * (pp.x - nn.x) +
                                  (pp.z - nn.z) * (pp.z - nn.z)));
                };
                dumpCar(ctrl.turnHolder[i]);
                for (const int32_t h : ctrl.groupHolders[0][i]) dumpCar(h);
                for (const int32_t h : ctrl.groupHolders[1][i]) dumpCar(h);
            }
            std::fprintf(stderr,
                         "traffic-tools: %d stalled car(s) holding node lock(s) at end (deadlock)\n",
                         stalledHeld);
            return 1;
        }
        if (!pngPath.empty()) {
            Raster r;
            r.w = 640;
            r.h = 480;
            r.rgb.assign(static_cast<size_t>(r.w) * r.h * 3, 255);
            const WorldMap m = worldMapFor(g, r.w, r.h);
            rasterLanes(r, g, m);
            // 信号节点状态方块（终帧相位；lock-only = 灰）
            for (const nl::traffic::NodeControl& n : ctrl.nodes) {
                unsigned char rr = 150, gg = 150, bb = 150;
                if (n.signal) {
                    const int32_t probe = n.groups[0].empty() ? (n.groups[1].empty() ? -1 : n.groups[1].front())
                                                              : n.groups[0].front();
                    const nl::traffic::SignalState st =
                        probe < 0 ? nl::traffic::SignalState::Red
                                  : ctrl.stateForEdge(static_cast<size_t>(&n - ctrl.nodes.data()), probe);
                    if (st == nl::traffic::SignalState::Green) { rr = 60; gg = 170; bb = 70; }
                    else if (st == nl::traffic::SignalState::Yellow) { rr = 240; gg = 200; bb = 40; }
                    else { rr = 210; gg = 60; bb = 50; }
                }
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        drawLine(r, m.px(n.x) + dx, m.py(n.z) + dy,
                                 m.px(n.x) + dx, m.py(n.z) + dy, rr, gg, bb);
            }
            for (const nl::traffic::TrafficCar& c : sim.cars) {
                nl::traffic::LanePoint p;
                if (!sim.carPosition(static_cast<size_t>(&c - sim.cars.data()), &p))
                    continue;
                // 3x3 车点（终帧）：行驶 = 红 / 滞留 = 琥珀
                const unsigned char rr = c.state.speed > 0.0f ? 220 : 235;
                const unsigned char gg = c.state.speed > 0.0f ? 30 : 170;
                const unsigned char bb = c.state.speed > 0.0f ? 30 : 40;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        drawLine(r, m.px(p.x) + dx, m.py(p.z) + dy,
                                 m.px(p.x) + dx, m.py(p.z) + dy, rr, gg, bb);
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
