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
    return LanePoint{fz, 0.0f, -fx};
}

struct RoadEdgeRec {
    int32_t id = -1;
    int32_t from = -1;
    int32_t to = -1;
    int32_t lanesPerDir = 1;
    float speedLimit = 0.0f;
    float width = 0.0f;
    // 可选折线（scene x/z，含首尾；M-CITY-ROAD）：真实 OSM 路网的 shape。
    // 空 = 直线边（旧口径，2 点 lane，逐位保形）。
    std::vector<LanePoint> shape;
};

bool isFinite(float v) { return std::isfinite(v) != 0; }

// 单位方向 a->b
LanePoint unitDir(const LanePoint& a, const LanePoint& b) {
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float l = std::sqrt(dx * dx + dz * dz);
    if (!(l > 0.0f)) return LanePoint{0.0f, 0.0f, 0.0f};
    return LanePoint{dx / l, 0.0f, dz / l};
}

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
        // M-RD-LAYER：节点高程（缺省 0 = 地面层；分层路网里 bridge/tunnel 节点
        // 带 y = layer * 7.0）。缺失或非数字按 0 处理，旧 roadnet 原样可用。
        const float y = (n.contains("y") && n["y"].is_number())
                            ? n["y"].get<float>() : 0.0f;
        if (!isFinite(x) || !isFinite(z) || !isFinite(y)) {
            if (err) *err = "node coordinate not finite";
            return false;
        }
        if (!nodePos.emplace(id, LanePoint{x, y, z}).second) {
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
            // 可选折线形状：[[x,z], ...]。端点必须与引用节点吻合（<=1 m），
            // 否则视为脏数据静默回退直线边（不失败：形状是装饰性的）。
            if (e.contains("shape")) {
                if (!e["shape"].is_array()) {
                    if (err) *err = "edge shape must be an array";
                    return false;
                }
                bool ok = true;
                std::vector<LanePoint> sh;
                for (const auto& p : e["shape"]) {
                    if (!p.is_array() || p.size() != 2 || !p[0].is_number() ||
                        !p[1].is_number()) {
                        ok = false;
                        break;
                    }
                    const float sx = p[0].get<float>();
                    const float sz = p[1].get<float>();
                    if (!isFinite(sx) || !isFinite(sz)) {
                        ok = false;
                        break;
                    }
                    sh.push_back(LanePoint{sx, 0.0f, sz});
                }
                if (!ok) {
                    if (err) *err = "edge shape point must be [x,z] numbers";
                    return false;
                }
                if (sh.size() >= 2) {
                    const LanePoint pa = nodePos[rec.from];
                    const LanePoint pb = nodePos[rec.to];
                    const float d0 = std::sqrt((sh.front().x - pa.x) * (sh.front().x - pa.x) +
                                               (sh.front().z - pa.z) * (sh.front().z - pa.z));
                    const float d1 = std::sqrt((sh.back().x - pb.x) * (sh.back().x - pb.x) +
                                               (sh.back().z - pb.z) * (sh.back().z - pb.z));
                    if (d0 <= 1.0f && d1 <= 1.0f) rec.shape = std::move(sh);
                }
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
        // 折线 = 真实 shape（M-CITY-ROAD）或两点直线（旧口径）。两点时下面的
        // miter 退化为「单一 right 向量」，与旧实现逐位等价。
        const LanePoint a = nodePos[e.from];
        const LanePoint b = nodePos[e.to];
        std::vector<LanePoint> poly;
        poly.reserve(e.shape.size() >= 2 ? e.shape.size() : 2);
        if (e.shape.size() >= 2) poly = e.shape;
        else { poly.push_back(a); poly.push_back(b); }
        // M-RD-LAYER：shape 顶点只有 x/z（roadnet 不存折线高程），按累计弧长
        // 在起终点节点的 y 之间线性插值。同层桥面（两端 y 相同）恒等于该层
        // 高度；跨层 connector（无 shape，两点）两端各取自己的 y；旧全 0
        // roadnet 插值后仍全 0（历史车道图逐位不变）。
        if (poly.size() >= 2) {
            std::vector<float> cum(poly.size(), 0.0f);
            for (size_t i = 1; i < poly.size(); ++i) {
                const float ex = poly[i].x - poly[i - 1].x;
                const float ez = poly[i].z - poly[i - 1].z;
                cum[i] = cum[i - 1] + std::sqrt(ex * ex + ez * ez);
            }
            const float total = cum.back();
            for (size_t i = 0; i < poly.size(); ++i) {
                if (total > 0.0f) {
                    poly[i].y = a.y + (b.y - a.y) * (cum[i] / total);
                } else {
                    // 平面投影完全重合（跨层 connector）：两端各自取节点高程。
                    poly[i].y = (i + 1 == poly.size()) ? b.y : a.y;
                }
            }
        }
        const size_t np = poly.size();
        // 顶点法线：端点用本段方向；内部点用相邻段方向的平均（miter），
        // 使偏移边在拐点连续（否则车道折线会在路口断开）。
        std::vector<LanePoint> nrm(np);
        float polyLen = 0.0f;
        for (size_t i = 0; i + 1 < np; ++i) {
            const float dx = poly[i + 1].x - poly[i].x;
            const float dz = poly[i + 1].z - poly[i].z;
            const float dy = poly[i + 1].y - poly[i].y;
            // M-RD-LAYER：弧长含 y——跨层 connector 的上下端点平面投影可以完全
            // 重合（实测 350 对 0.00 m），只看 x/z 会判成退化边（零长度）直接
            // 构建失败。同层路面 dy=0，数值与旧实现逐位相同。
            const float seg = std::sqrt(dx * dx + dy * dy + dz * dz);
            polyLen += seg;
            const LanePoint d = unitDir(poly[i], poly[i + 1]);
            const LanePoint r = rightOf(d.x, d.z);
            if (i == 0) nrm[0] = r;
            nrm[i + 1] = r;
        }
        for (size_t i = 1; i + 1 < np; ++i) {
            const LanePoint d0 = unitDir(poly[i - 1], poly[i]);
            const LanePoint d1 = unitDir(poly[i], poly[i + 1]);
            float mx = d0.x + d1.x;
            float mz = d0.z + d1.z;
            const float ml = std::sqrt(mx * mx + mz * mz);
            if (ml > 1e-6f) {
                mx /= ml;
                mz /= ml;
                nrm[i] = rightOf(mx, mz);   // ml≈0（原路折返）时保留前段法线
            }
        }
        if (!(polyLen > 0.0f)) {
            if (err) *err = "degenerate edge (zero length)";
            return false;
        }
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
            lane.length = polyLen;
            // 点序 = 行进方向；每个折线顶点沿该顶点法线平移 offset
            lane.points.reserve(np);
            for (size_t i = 0; i < np; ++i) {
                lane.points.push_back(LanePoint{
                    poly[i].x + nrm[i].x * lane.offset,
                    poly[i].y,
                    poly[i].z + nrm[i].z * lane.offset});
            }
            out->lanes.push_back(lane);
        }
    }

    // 连接表（禁掉头）：lane(u->v) outgoing = e'.from==v && e'.to!=u 的全部车道，
    // 按 lanes 下标序（= edges 文件序 x laneIndex 升序）遍历；incoming 对称。
    // M-CITY-ROAD：上面的「每条 lane 全扫」是 O(n^2)——真实路网（3931 边 /
    // ~1 万 lane）要 1e8 次；改成按节点索引后只扫本节点出发/到达的车道。
    // 索引 vector 内 lane id 天然升序，集合与顺序与全扫逐位一致。
    std::map<int32_t, std::vector<int32_t>> fromIndex;
    std::map<int32_t, std::vector<int32_t>> toIndex;
    for (const Lane& lane : out->lanes) {
        fromIndex[lane.fromNode].push_back(lane.id);
        toIndex[lane.toNode].push_back(lane.id);
    }
    for (Lane& lane : out->lanes) {
        bool hasForward = false;
        const auto it = fromIndex.find(lane.toNode);
        if (it != fromIndex.end()) {
            for (const int32_t oid : it->second) {
                if (out->lanes[static_cast<size_t>(oid)].toNode == lane.fromNode)
                    continue;                   // 掉头不建链
                lane.outgoing.push_back(LaneLink{oid});
                hasForward = true;
            }
            // 死胡同例外：目标节点除回头外无任何出边时，真实驾驶必然掉头；
            // 不放行则该 lane 在有向图上彻底孤立（实测真实路网 1066 条边因
            // 断头孤立，最大强连通分量只有 10%）。只放开这一种情形——
            // 有正常出边的路口仍然禁掉头（M-T0.1 口径不变）。
            if (!hasForward) {
                for (const int32_t oid : it->second) {
                    if (out->lanes[static_cast<size_t>(oid)].toNode == lane.fromNode)
                        lane.outgoing.push_back(LaneLink{oid});
                }
            }
        }
    }
    for (Lane& lane : out->lanes) {
        const auto it = toIndex.find(lane.fromNode);
        if (it == toIndex.end()) continue;
        for (const int32_t oid : it->second) {
            if (out->lanes[static_cast<size_t>(oid)].fromNode == lane.toNode)
                continue;
            lane.incoming.push_back(LaneLink{oid});
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
                                         {"y", static_cast<double>(p.y)},
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

// ---------------------------------------------------------------------------
// M-CITY-ROAD: 车道吸附（寻路起终点）。真实路网里玩家/AI 的起终点是任意
// 世界坐标，A* 只认 lane 下标，所以需要「最近车道」这一层。
// ---------------------------------------------------------------------------
float laneDistanceTo(const Lane& lane, float x, float z, float y) {
    if (lane.points.size() < 2) return -1.0f;
    float best = -1.0f;
    for (size_t i = 0; i + 1 < lane.points.size(); ++i) {
        const LanePoint& a = lane.points[i];
        const LanePoint& b = lane.points[i + 1];
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float seg2 = dx * dx + dz * dz;
        float t = 0.0f;
        if (seg2 > 0.0f) {
            t = ((x - a.x) * dx + (z - a.z) * dz) / seg2;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }
        const float px = a.x + dx * t - x;
        const float py = (a.y + (b.y - a.y) * t) - y;   // M-RD-LAYER：三维距离
        const float pz = a.z + dz * t - z;
        const float d = std::sqrt(px * px + py * py + pz * pz);
        if (best < 0.0f || d < best) best = d;
    }
    return best;
}

int32_t findNearestLane(const LaneGraph& graph, float x, float z, float maxDist,
                        float y) {
    if (!isFinite(x) || !isFinite(z) || !isFinite(y) || !(maxDist > 0.0f)) return -1;
    int32_t bestLane = -1;
    float bestD = maxDist;
    for (const Lane& lane : graph.lanes) {
        const float d = laneDistanceTo(lane, x, z, y);
        // 严格小于 + lanes 下标升序遍历 => 同距离保留最小 lane id（确定性）
        if (d >= 0.0f && d < bestD) {
            bestD = d;
            bestLane = lane.id;
        }
    }
    return bestLane;
}

} // namespace nl::traffic