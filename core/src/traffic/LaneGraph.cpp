// nl/traffic/LaneGraph.cpp — P8 M-T0.0：roadnet.json -> 车道级展开图
// 拍板/口径见 docs/worldgen/TRAFFIC-PLAN.md §0/§2（Q-T2=B1，UE ZoneGraph 口径对照）。
// json 解析用 vendored nlohmann（thirdparty/tinygltf/json.hpp，P8-ECS-PLAN 拍板
// 零新依赖；core/src/nav/SceneGeo.cpp 同款 include 先例）。

#include "nl/traffic/LaneGraph.hpp"

#include "json.hpp"

#include <cmath>
#include <map>

namespace nl::traffic {

namespace {

// right = up x f（XZ 面）：f=(fx,fz) -> right=(fz,-fx)。f=+Z 时 right=+X（头文件口径）。
LanePoint rightOf(float fx, float fz) {
    return LanePoint{fz, -fx};
}

struct RoadEdgeRec {
    int32_t id = -1;
    int32_t from = -1;
    int32_t to = -1;
    int32_t lanesPerDir = 1;
    float speedLimit = 0.0f;
    float width = 0.0f;
};

bool isFinite(float v) { return std::isfinite(v) != 0; }

} // namespace

bool buildLaneGraphFromRoadnetJson(const char* jsonBytes, size_t len,
                                   LaneGraph* out, std::string* err) {
    if (err) err->clear();
    if (out) *out = LaneGraph{};
    if (!jsonBytes || len == 0 || !out) {
        if (err) *err = "null input";
        return false;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(jsonBytes, jsonBytes + len);
    } catch (const std::exception& e) {
        if (err) *err = std::string("bad json: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        if (err) *err = "root is not an object";
        return false;
    }
    // 契约：version 只认 1（破坏性变更走递增，W0-PLAN §1 扩展规则）
    if (!root.contains("version") || !root["version"].is_number_integer() ||
        root["version"].get<int>() != 1) {
        if (err) *err = "unsupported roadnet version (contract v1)";
        return false;
    }
    if (!root.contains("nodes") || !root["nodes"].is_array() ||
        !root.contains("edges") || !root["edges"].is_array()) {
        if (err) *err = "missing nodes/edges array";
        return false;
    }
    // phases 预留位：存在必须是数组（内容 M-T0.2 信号相位起消费，M-T0.0 只收）
    if (root.contains("phases") && !root["phases"].is_array()) {
        if (err) *err = "phases must be an array";
        return false;
    }

    // nodes：id 唯一、坐标有限（kind/signal 本步不消费）
    std::map<int32_t, LanePoint> nodePos; // id -> 位置；std::map 保序遍历 = 确定性
    for (const auto& n : root["nodes"]) {
        if (!n.is_object() || !n.contains("id") || !n["id"].is_number_integer() ||
            !n.contains("x") || !n["x"].is_number() ||
            !n.contains("z") || !n["z"].is_number()) {
            if (err) *err = "node missing id/x/z";
            return false;
        }
        const int32_t id = n["id"].get<int32_t>();
        const float x = n["x"].get<float>();
        const float z = n["z"].get<float>();
        if (!isFinite(x) || !isFinite(z)) {
            if (err) *err = "node coordinate not finite";
            return false;
        }
        if (!nodePos.emplace(id, LanePoint{x, z}).second) {
            if (err) *err = "duplicate node id";
            return false;
        }
    }

    // edges：文件序即确定性遍历序；from/to 引用闭合 + 语义字段合法性
    std::vector<RoadEdgeRec> edges;
    {
        std::map<int32_t, size_t> edgeIds; // id 唯一性（溯源键，不许撞）
        for (const auto& e : root["edges"]) {
            if (!e.is_object() || !e.contains("id") || !e["id"].is_number_integer() ||
                !e.contains("from") || !e["from"].is_number_integer() ||
                !e.contains("to") || !e["to"].is_number_integer() ||
                !e.contains("lanes_per_dir") || !e["lanes_per_dir"].is_number_integer() ||
                !e.contains("speed_limit") || !e["speed_limit"].is_number() ||
                !e.contains("width") || !e["width"].is_number()) {
                if (err) *err = "edge missing id/from/to/lanes_per_dir/speed_limit/width";
                return false;
            }
            RoadEdgeRec rec;
            rec.id = e["id"].get<int32_t>();
            rec.from = e["from"].get<int32_t>();
            rec.to = e["to"].get<int32_t>();
            rec.lanesPerDir = e["lanes_per_dir"].get<int32_t>();
            rec.speedLimit = e["speed_limit"].get<float>();
            rec.width = e["width"].get<float>();
            if (nodePos.find(rec.from) == nodePos.end() ||
                nodePos.find(rec.to) == nodePos.end()) {
                if (err) *err = "edge from/to does not reference a node";
                return false;
            }
            if (rec.lanesPerDir < 1) {
                if (err) *err = "lanes_per_dir must be >= 1";
                return false;
            }
            if (!isFinite(rec.width) || rec.width <= 0.0f ||
                !isFinite(rec.speedLimit) || rec.speedLimit <= 0.0f) {
                if (err) *err = "width/speed_limit must be positive";
                return false;
            }
            if (!edgeIds.emplace(rec.id, edges.size()).second) {
                if (err) *err = "duplicate edge id";
                return false;
            }
            edges.push_back(rec);
        }
    }

    // 展开：edges 文件序 x laneIndex 升序 = lanes 下标序（确定性遍历序契约）
    out->lanes.clear();
    out->lanes.reserve(edges.size() * 2);
    std::vector<int32_t> edgeFirstLane(edges.size(), -1); // edge -> 首条 lane 下标
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const RoadEdgeRec& e = edges[ei];
        const LanePoint a = nodePos[e.from];
        const LanePoint b = nodePos[e.to];
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float segLen = std::sqrt(dx * dx + dz * dz);
        if (!(segLen > 0.0f)) {
            if (err) *err = "degenerate edge (zero length)";
            return false;
        }
        const float fx = dx / segLen;
        const float fz = dz / segLen;
        const LanePoint right = rightOf(fx, fz);
        const float halfW = e.width * 0.5f;
        const float laneW = halfW / static_cast<float>(e.lanesPerDir);
        edgeFirstLane[ei] = static_cast<int32_t>(out->lanes.size());
        for (int32_t li = 0; li < e.lanesPerDir; ++li) {
            Lane lane;
            lane.id = static_cast<int32_t>(out->lanes.size());
            lane.edgeId = e.id;
            lane.fromNode = e.from;
            lane.toNode = e.to;
            lane.laneIndex = li;
            lane.speedLimit = e.speedLimit;
            lane.offset = halfW - (static_cast<float>(li) + 0.5f) * laneW;
            lane.length = segLen;
            // 折线 = [from + right*off, to + right*off]，点序 = 行进方向
            lane.points.push_back(LanePoint{a.x + right.x * lane.offset,
                                            a.z + right.z * lane.offset});
            lane.points.push_back(LanePoint{b.x + right.x * lane.offset,
                                            b.z + right.z * lane.offset});
            out->lanes.push_back(lane);
        }
    }

    // 连接表（禁掉头）：lane(u->v) outgoing = e'.from==v && e'.to!=u 的全部车道，
    // 按 lanes 下标序（= edges 文件序 x laneIndex 升序）遍历；incoming 对称。
    for (Lane& lane : out->lanes) {
        for (const Lane& other : out->lanes) {
            if (other.fromNode != lane.toNode) continue;
            if (other.toNode == lane.fromNode) continue; // 掉头不建链
            lane.outgoing.push_back(LaneLink{other.id});
        }
    }
    for (Lane& lane : out->lanes) {
        for (const Lane& other : out->lanes) {
            if (other.toNode != lane.fromNode) continue;
            if (other.fromNode == lane.toNode) continue;
            lane.incoming.push_back(LaneLink{other.id});
        }
    }
    (void)edgeFirstLane;
    return true;
}

std::string dumpLaneGraphJson(const LaneGraph& graph) {
    nlohmann::json j;
    j["version"] = 1;
    j["laneCount"] = graph.laneCount();
    j["lanes"] = nlohmann::json::array();
    for (const Lane& lane : graph.lanes) {
        nlohmann::json jl;
        jl["id"] = lane.id;
        jl["edgeId"] = lane.edgeId;
        jl["fromNode"] = lane.fromNode;
        jl["toNode"] = lane.toNode;
        jl["laneIndex"] = lane.laneIndex;
        jl["speedLimit"] = static_cast<double>(lane.speedLimit);
        jl["offset"] = static_cast<double>(lane.offset);
        jl["length"] = static_cast<double>(lane.length);
        nlohmann::json pts = nlohmann::json::array();
        for (const LanePoint& p : lane.points) {
            pts.push_back(nlohmann::json{{"x", static_cast<double>(p.x)},
                                         {"z", static_cast<double>(p.z)}});
        }
        jl["points"] = pts;
        nlohmann::json outs = nlohmann::json::array();
        for (const LaneLink& l : lane.outgoing) outs.push_back(l.destLane);
        jl["outgoing"] = outs;
        nlohmann::json ins = nlohmann::json::array();
        for (const LaneLink& l : lane.incoming) ins.push_back(l.destLane);
        jl["incoming"] = ins;
        j["lanes"].push_back(jl);
    }
    return j.dump(2) + "\n";
}

} // namespace nl::traffic
