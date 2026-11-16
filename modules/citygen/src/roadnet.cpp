// citygen roadnet 转换实现(M-CG0.1)。拓扑口径见 roadnet.hpp 头注。
#include "citygen/roadnet.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace citygen {

namespace {

// 非车行道类别(M-CG0 只做机动车路网;行人路网归属另行拍板后扩)。
bool isNonVehicleClass(const std::string& cls) {
    static const char* kNonVehicle[] = {
        "footway", "steps", "path", "cycleway", "pedestrian", "track",
        "footway_link", "bridleway", "corridor", "proposed", "construction",
    };
    for (const char* c : kNonVehicle)
        if (cls == c) return true;
    return false;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

struct PointKeyHash {
    std::size_t operator()(const std::pair<double, double>& p) const {
        auto h1 = std::hash<double>{}(p.first);
        auto h2 = std::hash<double>{}(p.second);
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL);
    }
};
using PointKey = std::pair<double, double>;  // 精确位模式相等(lat,lon)

}  // namespace

bool highwayClassParams(const std::string& cls, double& speed, double& width) {
    // 缺省表(CG0-PLAN §3):城市驾驶口径,单位 m/s;width = 车行道总宽。
    struct Entry { const char* cls; double speed; double width; };
    static const Entry kTable[] = {
        {"motorway", 33.3, 12.0},
        {"motorway_link", 26.7, 6.0},
        {"trunk", 22.2, 10.0},
        {"trunk_link", 16.7, 5.0},
        {"primary", 16.7, 10.0},
        {"primary_link", 12.5, 5.0},
        {"secondary", 13.9, 8.0},
        {"secondary_link", 11.1, 5.0},
        {"tertiary", 11.1, 7.0},
        {"tertiary_link", 8.3, 4.0},
        {"residential", 8.3, 6.0},
        {"unclassified", 8.3, 5.5},
        {"living_street", 5.6, 5.0},
        {"service", 5.6, 4.0},
    };
    for (const Entry& e : kTable) {
        if (cls == e.cls) { speed = e.speed; width = e.width; return true; }
    }
    return false;  // 未知类别丢弃(计数),不猜
}

Roadnet buildRoadnetFromOsm(const OsmData& data, const BBox& box,
                            RoadnetBuildStats* stats) {
    RoadnetBuildStats st{};
    // ① 端点 + 跨 way 共享点统计(精确坐标聚类)。
    std::unordered_map<PointKey, int, PointKeyHash> wayCount;   // 经过的 way 条数
    std::unordered_map<PointKey, int, PointKeyHash> endpoint;   // 是否端点
    for (const OsmWay& w : data.ways) {
        if (w.pts.size() < 2) continue;
        const double s0 = w.tags.count("maxspeed") ? 0.0 : 0.0;  // (占位,见下)
        (void)s0;
        const std::string cls = w.tags.count("highway") ? w.tags.at("highway") : "";
        double sp, wd;
        if (isNonVehicleClass(cls) || !highwayClassParams(cls, sp, wd)) {
            ++st.droppedNonVehicle;
            continue;
        }
        std::unordered_map<PointKey, int, PointKeyHash> seenInThisWay;
        for (std::size_t i = 0; i < w.pts.size(); ++i) {
            const PointKey k{w.pts[i].first, w.pts[i].second};
            if (i == 0 || i + 1 == w.pts.size()) endpoint[k] = 1;
            if (++seenInThisWay[k] == 1) ++wayCount[k];  // 同 way 重复点只计一次
        }
    }
    // ② 节点收集:端点 ∪ 被 >=2 条不同 way 经过的点。编号 = 首次出现序
    //   (way 输入序 × way 内点序,确定性)。
    std::unordered_map<PointKey, std::uint32_t, PointKeyHash> nodeId;
    Roadnet net;
    net.anchorLat = box.centerLat();
    net.anchorLon = box.centerLon();
    auto ensureNode = [&](const PointKey& k) -> std::uint32_t {
        auto it = nodeId.find(k);
        if (it != nodeId.end()) return it->second;
        const std::uint32_t id = static_cast<std::uint32_t>(net.nodes.size());
        RoadnetNode n;
        n.id = id;
        n.lat = k.first;
        n.lon = k.second;
        std::tie(n.x, n.z) = toLocalMeters(box, n.lat, n.lon);
        net.nodes.push_back(n);
        nodeId.emplace(k, id);
        return id;
    };
    for (const OsmWay& w : data.ways) {
        if (w.pts.size() < 2) continue;
        const std::string cls = w.tags.count("highway") ? w.tags.at("highway") : "";
        double sp, wd;
        if (isNonVehicleClass(cls) || !highwayClassParams(cls, sp, wd)) continue;
        for (std::size_t i = 0; i < w.pts.size(); ++i) {
            const PointKey k{w.pts[i].first, w.pts[i].second};
            const bool isEnd = (i == 0 || i + 1 == w.pts.size());
            if (isEnd || wayCount[k] >= 2) {
                ++st.junctionCandidates;
                ensureNode(k);
            }
        }
    }
    // ③ 切边:way 的相邻节点间形状点合成一段 edge。
    std::uint32_t nextEdge = 0;
    for (const OsmWay& w : data.ways) {
        if (w.pts.size() < 2) continue;
        const std::string cls = w.tags.count("highway") ? w.tags.at("highway") : "";
        double sp, wd;
        if (isNonVehicleClass(cls) || !highwayClassParams(cls, sp, wd)) continue;
        const auto maxIt = w.tags.find("maxspeed");
        if (maxIt != w.tags.end()) {
            // 简单数字口径(kmh;带 unit/zone 复杂标签不解析,走类别缺省)
            double v = 0.0;
            if (std::sscanf(maxIt->second.c_str(), "%lf", &v) == 1 && v > 0.0 && v < 200.0)
                sp = v / 3.6;
        }
        std::uint32_t lanes = 1;
        const auto laneIt = w.tags.find("lanes");
        if (laneIt != w.tags.end()) {
            unsigned lv = 0;
            if (std::sscanf(laneIt->second.c_str(), "%u", &lv) == 1 && lv >= 1 && lv <= 12)
                lanes = lv;
        }
        const bool oneWayFwd = (w.tags.count("oneway") && w.tags.at("oneway") == "yes");
        const bool oneWayRev = (w.tags.count("oneway") && w.tags.at("oneway") == "-1");
        const std::string name =
            (w.tags.count("name") ? w.tags.at("name") : std::string{});
        // 沿 way 找节点序列(带索引,切边时收集中间形状点)。
        std::vector<std::pair<std::uint32_t, std::size_t>> path;  // (nodeId, ptIdx)
        for (std::size_t i = 0; i < w.pts.size(); ++i) {
            const PointKey k{w.pts[i].first, w.pts[i].second};
            const bool isEnd = (i == 0 || i + 1 == w.pts.size());
            if (isEnd || wayCount[k] >= 2)
                path.emplace_back(ensureNode(k), i);
        }
        if (path.size() < 2) { ++st.droppedDegenerate; continue; }
        auto shapeSlice = [&](std::size_t ia, std::size_t ib) {
            // way 点 [ia..ib] → 局部米(from→to 方向)。
            std::vector<std::pair<double, double>> pts;
            pts.reserve(ib - ia + 1);
            for (std::size_t i = ia; i <= ib; ++i)
                pts.emplace_back(toLocalMeters(box, w.pts[i].first, w.pts[i].second));
            return pts;
        };
        const bool fwd = !oneWayRev;
        const bool rev = !oneWayFwd;
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            const std::uint32_t a = path[i].first, b = path[i + 1].first;
            const std::size_t ia = path[i].second, ib = path[i + 1].second;
            if (a == b) continue;
            if (fwd) {
                RoadnetEdge e;
                e.id = nextEdge++;
                e.from = a; e.to = b;
                e.lanesPerDir = lanes; e.speedLimit = sp; e.width = wd;
                e.osmName = name; e.cls = cls;
                e.shape = shapeSlice(ia, ib);
                net.edges.push_back(e);
            }
            if (rev) {
                RoadnetEdge e;
                e.id = nextEdge++;
                e.from = b; e.to = a;
                e.lanesPerDir = lanes; e.speedLimit = sp; e.width = wd;
                e.osmName = name; e.cls = cls;
                // 反向形状点 = 正向切片反转。
                e.shape = shapeSlice(ia, ib);
                std::reverse(e.shape.begin(), e.shape.end());
                net.edges.push_back(e);
            }
        }
    }
    // ④ signal 位:进入边 >= 2(M-T4 口径,消费端 TrafficControl 受控面一致)。
    std::vector<unsigned> inDeg(net.nodes.size(), 0u);
    for (const RoadnetEdge& e : net.edges)
        if (e.to < inDeg.size()) ++inDeg[e.to];
    for (RoadnetNode& n : net.nodes) n.isSignal = inDeg[n.id] >= 2u;
    if (stats) *stats = st;
    return net;
}

std::string serializeRoadnet(const Roadnet& net) {
    std::string s;
    char buf[320];
    s += "{\n";
    s += "  \"version\": 1,\n";
    std::snprintf(buf, sizeof buf, "  \"seed\": 0,\n");
    s += buf;
    s += "  \"units\": \"meters\",\n";
    // OSM 版 blocks 槽位 = bbox 尺寸元信息(city-tools 同名字段口径:网格块尺寸;
    // OSM 无网格,落 bbox 宽高如实标注,消费端不依赖本字段——契约「只加不减」下
    // 语义由注释/消费方确认,traffic 侧仅读取 nodes/edges/phases)。
    double minx = 1e300, minz = 1e300, maxx = -1e300, maxz = -1e300;
    for (const RoadnetNode& n : net.nodes) {
        minx = std::min(minx, n.x); maxx = std::max(maxx, n.x);
        minz = std::min(minz, n.z); maxz = std::max(maxz, n.z);
    }
    std::snprintf(buf, sizeof buf,
                  "  \"blocks\": { \"x\": %.2f, \"z\": %.2f },\n",
                  maxx - minx, maxz - minz);
    s += buf;
    s += "  \"nodes\": [\n";
    for (std::size_t i = 0; i < net.nodes.size(); ++i) {
        const RoadnetNode& n = net.nodes[i];
        std::snprintf(buf, sizeof buf,
                      "    { \"id\": %u, \"x\": %.2f, \"z\": %.2f, "
                      "\"kind\": \"intersection\", \"signal\": %s }%s\n",
                      n.id, n.x, n.z, n.isSignal ? "true" : "false",
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
    (void)jsonEscape;  // osm_name 进 city.scene/补充产物时启用;本文件字段全数值
    return s;
}

}  // namespace citygen
