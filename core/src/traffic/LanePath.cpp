// nl/traffic/LanePath.cpp — P8 M-T0.1：车道图 A*（确定性线性扫 open 表）
// 口径见 LanePath.hpp 头注与 docs/worldgen/TRAFFIC-PLAN.md §3。

#include "nl/traffic/LanePath.hpp"

#include <cmath>

namespace nl::traffic {
namespace {

// lane 末折线点（行进方向终点 = A* 启发/代价锚）
LanePoint laneEnd(const Lane& lane) {
    return lane.points.back();
}

float dist(const LanePoint& a, const LanePoint& b) {
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

PathStatus findPathOnLanes(const LaneGraph& graph, int32_t startLane, int32_t goalLane,
                           std::vector<int32_t>* outLanePath, std::string* err) {
    if (err) err->clear();
    if (outLanePath) outLanePath->clear();
    const int32_t n = graph.laneCount();
    if (startLane < 0 || startLane >= n || goalLane < 0 || goalLane >= n) {
        if (err) *err = "start/goal lane index out of range";
        return PathStatus::BadLane;
    }

    // g 值 = 起点 lane 入场到当前 lane 入场的弧长累计；parent 重建路径
    std::vector<float> g(static_cast<size_t>(n), -1.0f);
    std::vector<int32_t> parent(static_cast<size_t>(n), -1);
    std::vector<uint8_t> closed(static_cast<size_t>(n), 0);
    // open 表：线性扫最小，禁堆（堆序不稳定破坏逐位确定性）；插入序 = 扩展序
    std::vector<int32_t> open;
    open.push_back(startLane);
    g[static_cast<size_t>(startLane)] = 0.0f;

    const LanePoint goalEnd = laneEnd(graph.lanes[static_cast<size_t>(goalLane)]);
    bool found = (startLane == goalLane);

    while (!found && !open.empty()) {
        // 取 f = g + h 最小者；同 f 保持先插入者（scan 用严格小于）——确定性锚
        size_t bestIdx = 0;
        float bestF = -1.0f;
        for (size_t i = 0; i < open.size(); ++i) {
            const Lane& lane = graph.lanes[static_cast<size_t>(open[i])];
            const float f = g[static_cast<size_t>(open[i])] + dist(laneEnd(lane), goalEnd);
            if (bestF < 0.0f || f < bestF) {
                bestF = f;
                bestIdx = i;
            }
        }
        const int32_t cur = open[bestIdx];
        open[bestIdx] = open.back();
        open.pop_back();
        if (closed[static_cast<size_t>(cur)]) continue;
        closed[static_cast<size_t>(cur)] = 1;
        if (cur == goalLane) {
            found = true;
            break;
        }
        for (const LaneLink& link : graph.lanes[static_cast<size_t>(cur)].outgoing) {
            const size_t d = static_cast<size_t>(link.destLane);
            if (closed[d]) continue;
            const float ng = g[static_cast<size_t>(cur)] +
                             graph.lanes[d].length;
            if (g[d] < 0.0f) {
                g[d] = ng;
                parent[d] = cur;
                open.push_back(link.destLane);
            } else if (ng < g[d]) {
                g[d] = ng;
                parent[d] = cur;
                // 已在 open，不重复插入（g 已更新，扫最小自然吃到新值）
            }
        }
    }

    if (!found) {
        if (err) *err = "goal lane not reachable from start lane";
        return PathStatus::Unreachable;
    }

    if (outLanePath) {
        std::vector<int32_t> rev;
        int32_t cur = goalLane;
        while (cur >= 0) {
            rev.push_back(cur);
            if (cur == startLane) break;
            cur = parent[static_cast<size_t>(cur)];
        }
        outLanePath->assign(rev.rbegin(), rev.rend());
    }
    return PathStatus::Ok;
}

float pathLength(const LaneGraph& graph, const std::vector<int32_t>& lanePath) {
    float sum = 0.0f;
    for (const int32_t id : lanePath) {
        if (id < 0 || id >= graph.laneCount()) return 0.0f;
        sum += graph.lanes[static_cast<size_t>(id)].length;
    }
    return sum;
}

} // namespace nl::traffic
