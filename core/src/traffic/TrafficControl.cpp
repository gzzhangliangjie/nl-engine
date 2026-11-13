// nl/traffic/TrafficControl.cpp — P8 M-T1.0 + M-T2.0（口径见 hpp：
// M-T1 相位时钟/自适应几何；M-T2 组共享 + 转弯独占 + gap-out 感应切相）
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
        bool axisPure = true;
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
            // 轴纯性：正交网格边一个分量精确为 0；斜边（两分量皆非 0）
            // 同组直行轨迹可能交叉 -> 该节点组锁退化单持有者
            if (std::fmin(dx, dz) > 1e-4f * std::fmax(1.0f, std::fmax(dx, dz)))
                axisPure = false;
        }
        nc.axisPure = axisPure;
        // 自适应释放半径：min(全局, 0.3*最小邻接边长)——密集路网上固定 3m 圈
        // 会与停车线/排队间距几何不相容（详见头注 M-T1 根修）
        nc.clearEff = std::fmin(clearRadius, 0.3f * minLen);
        nodes.push_back(nc);
    }

    // phases 可选覆盖（契约只加不减；citygen 不填 = 全默认 8/2/1 + gap-out 关）
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
            if (p.contains("gap_out_s")) { // M-T2.1：缺省 0 = 固定周期（M-T1 行为）
                nc.gapOutS = p.value("gap_out_s", 0.0f);
                if (!(nc.gapOutS >= 0.0f)) {
                    if (err) *err = "control build: gap_out_s must be >= 0";
                    return false;
                }
            }
        }
    }

    // 运行时清零
    const size_t n = nodes.size();
    groupHolders[0].assign(n, {});
    groupHolders[1].assign(n, {});
    turnHolder.assign(n, -1);
    waitQueue.assign(n, {});
    demandCount[0].assign(n, 0);
    demandCount[1].assign(n, 0);
    greenIdle.assign(n, 0);
    lastGreenSlot.assign(n, -1);
    frame = 0;
    return true;
}

int32_t TrafficControl::findNode(int32_t nodeId) const {
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].nodeId == nodeId) return static_cast<int32_t>(i);
    return -1;
}

int TrafficControl::groupOfEdge(size_t nodeIdx, int32_t edgeId) const {
    const NodeControl& n = nodes[nodeIdx];
    for (int gi = 0; gi < 2; ++gi)
        for (const int32_t e : n.groups[gi])
            if (e == edgeId) return gi;
    return -1;
}

SignalState TrafficControl::stateForEdge(size_t nodeIdx, int32_t edgeId) const {
    const NodeControl& n = nodes[nodeIdx];
    if (!n.signal) return SignalState::Green; // lock-only：无信号，仅锁管通行
    const int group = groupOfEdge(nodeIdx, edgeId);
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

namespace {

// FIFO 授予循环：队首可授则出队并继续，遇不可授队首即停。
// 组授予条件 = 无转弯持有者 + 另一组无持有者（跨组直行轨迹交叉 = 冲突；
// 组内共享 = 对向/同向直行右行车道分离）。防转弯饿死由 FIFO 队序天然保证：
// turn 之前的组申请先授完，turn 排到队首后其后的组申请全部停住。
void drainQueue(TrafficControl& c, size_t ni) {
    std::vector<LockWaitEntry>& q = c.waitQueue[ni];
    while (!q.empty()) {
        const LockWaitEntry f = q.front();
        if (f.turn) {
            if (c.turnHolder[ni] < 0 && c.groupHolders[0][ni].empty() &&
                c.groupHolders[1][ni].empty()) {
                c.turnHolder[ni] = f.car;
                q.erase(q.begin());
                continue;
            }
        } else if (c.turnHolder[ni] < 0 &&
                   c.groupHolders[f.group ^ 1][ni].empty()) {
            // 组空即授（两种节点一致 = 单持有者语义，M-T1 兼容）；
            // 组非空仅 axisPure 节点共享（非纯轴同组直行轨迹可能交叉，退化串行）
            if (c.groupHolders[f.group][ni].empty() || c.nodes[ni].axisPure) {
                c.groupHolders[f.group][ni].push_back(f.car);
                q.erase(q.begin());
                continue;
            }
        }
        break;
    }
}

} // namespace

bool TrafficControl::acquire(size_t nodeIdx, int32_t carIdx, int group, bool turn) {
    if (hasAccess(nodeIdx, carIdx)) return true; // 已持有（防御重复申请）
    if (!turn && group < 0) return false; // 防御：直行申请须属组（stateForEdge 同口径）
    std::vector<LockWaitEntry>& q = waitQueue[nodeIdx];
    for (const LockWaitEntry& w : q)
        if (w.car == carIdx) return false; // 已在等（不重复入队）
    LockWaitEntry e;
    e.car = carIdx;
    e.group = static_cast<int8_t>(group);
    e.turn = turn;
    q.push_back(e);
    drainQueue(*this, nodeIdx);
    return hasAccess(nodeIdx, carIdx);
}

bool TrafficControl::hasAccess(size_t nodeIdx, int32_t carIdx) const {
    if (turnHolder[nodeIdx] == carIdx) return true;
    for (const int32_t c : groupHolders[0][nodeIdx])
        if (c == carIdx) return true;
    for (const int32_t c : groupHolders[1][nodeIdx])
        if (c == carIdx) return true;
    return false;
}

void TrafficControl::releaseCleared(int32_t carIdx, float carX, float carZ) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!hasAccess(i, carIdx)) continue;
        const float dx = carX - nodes[i].x;
        const float dz = carZ - nodes[i].z;
        const float r = nodes[i].clearEff;
        if (dx * dx + dz * dz > r * r) {
            if (turnHolder[i] == carIdx) turnHolder[i] = -1;
            auto drop = [&](std::vector<int32_t>& v) {
                v.erase(std::remove(v.begin(), v.end(), carIdx), v.end());
            };
            drop(groupHolders[0][i]);
            drop(groupHolders[1][i]);
            drainQueue(*this, i); // 释放可能解锁队首
        }
    }
}

void TrafficControl::reportWaiting(size_t nodeIdx, int32_t edgeId) {
    const int g = groupOfEdge(nodeIdx, edgeId);
    if (g >= 0) ++demandCount[g][nodeIdx];
}

void TrafficControl::advanceFrame() {
    // gap-out 检查（用上帧 demand + 当前 frame 相位；整数帧确定性）
    for (size_t i = 0; i < nodes.size(); ++i) {
        const NodeControl& n = nodes[i];
        if (!n.signal || n.gapOutS <= 0.0f) continue;
        const int gf = static_cast<int>(std::lround(n.greenS / dt));
        const int half =
            gf + static_cast<int>(std::lround((n.yellowS + n.clearS) / dt));
        const int cycle = 2 * half;
        int64_t ff = frame % cycle;
        const bool firstHalf = ff < half;
        const int active = firstHalf ? 0 : 1;
        const int inPhase = static_cast<int>(firstHalf ? ff : ff - half);
        const int minGreenF = std::min(gf, static_cast<int>(std::lround(2.0f / dt)));
        if (inPhase < gf && inPhase >= minGreenF) {
            // 绿相内：换相清零 / 需求清零 / 空闲累计
            if (lastGreenSlot[i] != active) {
                greenIdle[i] = 0;
                lastGreenSlot[i] = active;
            }
            if (demandCount[active][i] > 0) {
                greenIdle[i] = 0;
            } else {
                ++greenIdle[i];
                if (static_cast<float>(greenIdle[i]) * dt >= n.gapOutS) {
                    // 跳帧到本相黄灯起点（stateForEdge 纯函数自动跟进）
                    const int greenStart = firstHalf ? 0 : half;
                    frame += static_cast<int64_t>(greenStart + gf - inPhase);
                    greenIdle[i] = 0; // 单跳足够：黄/红相不累计，下个绿相重计
                }
            }
        } else {
            greenIdle[i] = 0; // 非绿相（含最短绿时窗口内）不累计
        }
    }
    ++frame;
    demandCount[0].assign(demandCount[0].size(), 0);
    demandCount[1].assign(demandCount[1].size(), 0);
}

int TrafficControl::heldNodeCount() const {
    int k = 0;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (turnHolder[i] >= 0 || !groupHolders[0][i].empty() ||
            !groupHolders[1][i].empty())
            ++k;
    return k;
}

} // namespace nl::traffic
