// nl/traffic/TrafficControl.cpp — P8 M-T1.0：红绿灯相位 + 路口占用锁（口径见 hpp）
#include "nl/traffic/TrafficControl.hpp"

#include "json.hpp"

#include <algorithm>
#include <cmath>

namespace nl::traffic {

namespace {

struct RawNode {
    int32_t id = -1;
    float x = 0.0f, z = 0.0f;
    bool signal = false;
};

struct RawEdge {
    int32_t id = -1;
    int32_t from = -1, to = -1;
};

} // namespace

bool TrafficControl::buildFromRoadnetJson(const char* jsonBytes, size_t len,
                                          const LaneGraph* g, std::string* err) {
    if (err) err->clear();
    if (!jsonBytes || len == 0 || !g || g->laneCount() == 0) {
        if (err) *err = "control build: null input or empty lane graph";
        return false;
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(jsonBytes, jsonBytes + len);
    } catch (const std::exception&) {
        if (err) *err = "control build: bad json";
        return false;
    }
    if (!doc.is_object() || doc.value("version", 0) != 1 ||
        !doc.contains("nodes") || !doc.contains("edges")) {
        if (err) *err = "control build: bad roadnet (need version 1 + nodes + edges)";
        return false;
    }

    std::vector<RawNode> rnodes;
    for (const auto& j : doc["nodes"]) {
        RawNode n;
        n.id = j.value("id", -1);
        n.x = j.value("x", 0.0f);
        n.z = j.value("z", 0.0f);
        n.signal = j.value("signal", false);
        for (const RawNode& o : rnodes)
            if (o.id == n.id) {
                if (err) *err = "control build: duplicate node id";
                return false;
            }
        rnodes.push_back(n);
    }
    std::vector<RawEdge> redges;
    for (const auto& j : doc["edges"]) {
        RawEdge e;
        e.id = j.value("id", -1);
        e.from = j.value("from", -1);
        e.to = j.value("to", -1);
        redges.push_back(e);
    }

    // 节点坐标 / signal 位查表（edge 引用不闭合 = LaneGraph 已挡，此处防御）
    auto nodeOf = [&](int32_t id) -> const RawNode* {
        for (const RawNode& n : rnodes)
            if (n.id == id) return &n;
        return nullptr;
    };

    // 受控节点 = 进入边 >= 2（单进入边节点无跨边冲突，不设锁）
    nodes.clear();
    for (const RawNode& n : rnodes) {
        std::vector<int32_t> inEdges;
        for (const RawEdge& e : redges)
            if (e.to == n.id) inEdges.push_back(e.id);
        if (inEdges.size() < 2) continue;
        std::sort(inEdges.begin(), inEdges.end()); // edge id 升序 = 确定性
        NodeControl nc;
        nc.nodeId = n.id;
        nc.x = n.x;
        nc.z = n.z;
        nc.signal = n.signal;
        // 行进主轴分组：x 轴向组 0（东西向）/ z 轴向组 1（南北向）；对角 tie 归组 0
        float minLen = 1e30f;
        for (const int32_t eid : inEdges) {
            const RawEdge* e = nullptr;
            for (const RawEdge& r : redges)
                if (r.id == eid) e = &r;
            const RawNode* fn = nodeOf(e->from);
            const RawNode* tn = nodeOf(e->to);
            if (!fn || !tn) {
                if (err) *err = "control build: edge references unknown node";
                return false;
            }
            const float dx = std::fabs(tn->x - fn->x);
            const float dz = std::fabs(tn->z - fn->z);
            nc.groups[dx >= dz ? 0 : 1].push_back(eid);
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < minLen) minLen = len;
        }
        // 自适应释放半径：min(全局, 0.3*最小邻接边长)——密集路网上固定 3m 圈
        // 会与停车线/排队间距几何不相容（详见头注 M-T1 根修）
        nc.clearEff = std::fmin(clearRadius, 0.3f * minLen);
        nodes.push_back(nc);
    }

    // phases 可选覆盖（契约只加不减；citygen 不填 = 全默认 8/2/1）
    if (doc.contains("phases") && doc["phases"].is_array()) {
        for (const auto& p : doc["phases"]) {
            if (!p.is_object() || !p.contains("node")) {
                if (err) *err = "control build: phases entry needs node";
                return false;
            }
            const int32_t nodeId = p.value("node", -1);
            int32_t idx = -1;
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].nodeId == nodeId) idx = static_cast<int32_t>(i);
            if (idx < 0) {
                if (err) *err = "control build: phases references uncontrolled node";
                return false;
            }
            NodeControl& nc = nodes[static_cast<size_t>(idx)];
            if (p.contains("green_s")) {
                nc.greenS = p.value("green_s", 8.0f);
                if (!(nc.greenS > 0.0f)) {
                    if (err) *err = "control build: green_s must be > 0";
                    return false;
                }
            }
            if (p.contains("yellow_s")) {
                nc.yellowS = p.value("yellow_s", 2.0f);
                if (!(nc.yellowS >= 0.0f)) {
                    if (err) *err = "control build: yellow_s must be >= 0";
                    return false;
                }
            }
            if (p.contains("clear_s")) {
                nc.clearS = p.value("clear_s", 1.0f);
                if (!(nc.clearS >= 0.0f)) {
                    if (err) *err = "control build: clear_s must be >= 0";
                    return false;
                }
            }
        }
    }

    // 运行时清零
    const size_t n = nodes.size();
    holder.assign(n, -1);
    waitQueue.assign(n, {});
    frame = 0;
    return true;
}

int32_t TrafficControl::findNode(int32_t nodeId) const {
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].nodeId == nodeId) return static_cast<int32_t>(i);
    return -1;
}

SignalState TrafficControl::stateForEdge(size_t nodeIdx, int32_t edgeId) const {
    const NodeControl& n = nodes[nodeIdx];
    if (!n.signal) return SignalState::Green; // lock-only：无信号，仅锁管通行
    int group = -1;
    for (int gi = 0; gi < 2 && group < 0; ++gi)
        for (const int32_t e : n.groups[gi])
            if (e == edgeId) {
                group = gi;
                break;
            }
    if (group < 0) return SignalState::Red; // 防御：不属任何组的进入边不放行
    // 周期 = 2*(g+y+c)：前半组 0 活跃，后半组 1 活跃；tt<g 绿、<g+y 黄、否则全红
    const float phaseLen = n.greenS + n.yellowS + n.clearS;
    const float t = std::fmod(static_cast<float>(frame) * dt, 2.0f * phaseLen);
    const bool firstHalf = t < phaseLen;
    if ((group == 0) != firstHalf) return SignalState::Red;
    const float tt = std::fmod(t, phaseLen);
    if (tt < n.greenS) return SignalState::Green;
    if (tt < n.greenS + n.yellowS) return SignalState::Yellow;
    return SignalState::Red;
}

bool TrafficControl::acquire(size_t nodeIdx, int32_t carIdx) {
    if (holder[nodeIdx] == carIdx) return true;
    std::vector<int32_t>& q = waitQueue[nodeIdx];
    if (holder[nodeIdx] < 0 && (q.empty() || q.front() == carIdx)) {
        if (!q.empty()) q.erase(q.begin());
        holder[nodeIdx] = carIdx;
        return true;
    }
    const bool present = std::find(q.begin(), q.end(), carIdx) != q.end();
    if (!present) q.push_back(carIdx); // FIFO 入队（重复申请不重复入队）
    return false;
}

void TrafficControl::releaseCleared(int32_t carIdx, float carX, float carZ) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (holder[i] != carIdx) continue;
        const float dx = carX - nodes[i].x;
        const float dz = carZ - nodes[i].z;
        const float r = nodes[i].clearEff;
        if (dx * dx + dz * dz > r * r) holder[i] = -1;
    }
}

} // namespace nl::traffic
