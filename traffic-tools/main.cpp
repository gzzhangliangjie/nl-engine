// traffic-tools — P8 M-T0.0：交通 lane graph 无头验收工具（A 类轨道 feat/worldgen）
// 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 nav-tools / city-tools / 展示台 expectExit）。
// 只链 nl_core（nl/traffic 零图形依赖）；拍板与口径 docs/worldgen/TRAFFIC-PLAN.md。
//
// 子命令：
//   build <roadnet.json> [--dump out.json] [--stats]   构建 lane graph（确定性）
//   selftest [--emit-fixture path]                     数值锚 selftest（确定性逐位）
//   help                                               用法

#include "nl/traffic/LaneGraph.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_asserts = 0;

#define T0_CHECK(cond)                                                       \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "traffic selftest: FAIL at %s:%d  %s\n",    \
                         __FILE__, __LINE__, #cond);                         \
            return 1;                                                        \
        }                                                                    \
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

    return usage();
}
