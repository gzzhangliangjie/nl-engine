// M2.1：组内 QEM 简化 + 边界锁定实现。流程：
//   1) weldPositions → canonical 点；簇展开成 canonical 三角形（重复点三角剔除）
//   2) 边界判定看组归属：canonical 点被 ≥2 个组的簇引用 → 锁死（永不移动、
//      永不折叠、其单三角形接缝边不被第三方折叠摧毁）
//   3) 逐组 QEM：面积加权平面二次型累积 + 有序集折叠队列（代价平局按点编号
//      决胜 → 完全确定）；link condition + 法线翻转检查拒绝坏折叠
//   4) 折叠到 round(inputTris * ratio) 即停；存活三角按序输出 canonical soup
//
// 确定性：unordered_map 只做查找不做结果序迭代；队列是有序集；同输入同输出。
// 复杂度：组内规模 ≤ ~1024 tri（8 簇/组），O(n²) 邻接重算也绰绰有余。

#include "nl/core/simplify.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <set>
#include <tuple>
#include <unordered_map>

namespace nl::core {

namespace {

using std::uint32_t;

constexpr float kMinTriArea = 1e-14f;   // 退化三角形不贡献平面（法向权重 clamp）
constexpr float kMinDetRel  = 1e-10f;   // 3x3 求解失败阈值（相对元素尺度）
constexpr float kFlipEps    = 1e-12f;   // 法线翻转判定（非归一化点积）

inline std::uint64_t edgeKey(uint32_t a, uint32_t b) {
    return (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
}

// 对称 4x4 二次型（全 16 项存储，索引 i*4+j；规模小不省这点内存）。
struct Quadric {
    float q[16] = {};
};

// 面积加权的平面二次型：K = w * pl ⊗ pl，pl = [n̂, d] 单位平面。
void accumulatePlane(Quadric& Q, const float pl[4], float w) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            Q.q[i * 4 + j] += w * pl[i] * pl[j];
}

// v^T Q v，v = [x,y,z,1]。
float evalQuadric(const Quadric& Q, const float p[3]) {
    const float v[4] = { p[0], p[1], p[2], 1.f };
    float sum = 0.f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            sum += v[i] * Q.q[i * 4 + j] * v[j];
    return sum;
}

// 二次型最优点：解 A x = b（A = 左上 3x3，b = -第四列前三项）。
// 奇异/近奇异返回 false（调用方回退端点/中点候选）。
bool optimalPosition(const Quadric& Q, float out[3]) {
    const float a[3][3] = {
        { Q.q[0],  Q.q[1],  Q.q[2]  },
        { Q.q[4],  Q.q[5],  Q.q[6]  },
        { Q.q[8],  Q.q[9],  Q.q[10] },
    };
    const float b[3] = { -Q.q[3], -Q.q[7], -Q.q[11] };
    float scale = 0.f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            scale = std::max(scale, std::fabs(a[i][j]));
    if (scale <= 0.f) return false;
    const float det =
        a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
        a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::fabs(det) > kMinDetRel * scale * scale * scale)) return false;
    const float invDet = 1.f / det;
    const float x[3] = {
        (b[0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
         a[0][1] * (b[1] * a[2][2] - a[1][2] * b[2]) +
         a[0][2] * (b[1] * a[2][1] - a[1][1] * b[2])) * invDet,
        (a[0][0] * (b[1] * a[2][2] - a[1][2] * b[2]) -
         b[0] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
         a[0][2] * (a[1][0] * b[2] - b[1] * a[2][0])) * invDet,
        (a[0][0] * (a[1][1] * b[2] - b[1] * a[2][1]) -
         a[0][1] * (a[1][0] * b[2] - b[1] * a[2][0]) +
         b[0] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) * invDet,
    };
    if (!std::isfinite(x[0]) || !std::isfinite(x[1]) || !std::isfinite(x[2]))
        return false;
    out[0] = x[0]; out[1] = x[1]; out[2] = x[2];
    return true;
}

// 组内简化器：局部点编号 0..P-1（canonical → 局部由装配层映射）。
struct Simplifier {
    std::vector<float> pos;                 // 3*P，可变（内部点可移到 QEM 最优点）
    std::vector<std::uint8_t> locked;       // 组间边界点
    std::vector<Quadric> quad;
    std::vector<std::array<uint32_t, 3>> tri;
    std::vector<std::uint8_t> alive;
    uint32_t aliveCount = 0;
    std::unordered_map<std::uint64_t, std::vector<uint32_t>> edgeTris;
    std::vector<std::vector<uint32_t>> vertTris;
    uint32_t collapses = 0;
    float maxError = 0.f;
};

void triNormal(const Simplifier& s, uint32_t t, const float* overridePos,
               uint32_t overrideIdx, float n[3]) {
    const auto& T = s.tri[t];
    const float* p0 = (overridePos && overrideIdx == T[0]) ? overridePos : &s.pos[T[0] * 3];
    const float* p1 = (overridePos && overrideIdx == T[1]) ? overridePos : &s.pos[T[1] * 3];
    const float* p2 = (overridePos && overrideIdx == T[2]) ? overridePos : &s.pos[T[2] * 3];
    const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
}

// 折叠位置：锁定端点优先（位置不变）；否则 QEM 最优点，退化回退端点/中点取误差最小者。
void choosePosition(const Simplifier& s, uint32_t a, uint32_t b, float out[3]) {
    if (s.locked[a]) {
        std::copy_n(&s.pos[a * 3], 3, out);
        return;
    }
    if (s.locked[b]) {
        std::copy_n(&s.pos[b * 3], 3, out);
        return;
    }
    Quadric Q;
    for (int i = 0; i < 16; ++i) Q.q[i] = s.quad[a].q[i] + s.quad[b].q[i];
    if (optimalPosition(Q, out)) return;
    const float* pa = &s.pos[a * 3];
    const float* pb = &s.pos[b * 3];
    const float cand[3][3] = {
        { pa[0], pa[1], pa[2] },
        { pb[0], pb[1], pb[2] },
        { (pa[0] + pb[0]) * 0.5f, (pa[1] + pb[1]) * 0.5f, (pa[2] + pb[2]) * 0.5f },
    };
    int best = 0;
    float bestCost = evalQuadric(Q, cand[0]);
    for (int i = 1; i < 3; ++i) {
        const float c = evalQuadric(Q, cand[i]);
        if (c < bestCost) { bestCost = c; best = i; }
    }
    std::copy_n(cand[best], 3, out);
}

// 边 (a,b) 折叠可行性 + 代价。返回 false = 永不折叠（拓扑/边界锁/翻转拒绝）。
bool evaluateEdge(const Simplifier& s, uint32_t a, uint32_t b, float& outCost) {
    const auto it = s.edgeTris.find(edgeKey(a, b));
    if (it == s.edgeTris.end() || it->second.empty()) return false;
    const std::vector<uint32_t>& abTris = it->second;
    if (abTris.size() > 2) return false;                    // 非流形边
    if (s.locked[a] && s.locked[b]) return false;           // 双锁定边 = 接缝本体
    if (s.aliveCount <= abTris.size()) return false;        // 不许清空整组

    // link condition：N(a) ∩ N(b) == edge(ab) 的对顶点集合
    auto neighbors = [&s](uint32_t v, std::vector<uint32_t>& out) {
        out.clear();
        for (uint32_t t : s.vertTris[v]) {
            if (!s.alive[t]) continue;
            const auto& T = s.tri[t];
            for (uint32_t u : T)
                if (u != v) out.push_back(u);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    };
    std::vector<uint32_t> na, nb, common, opp;
    neighbors(a, na);
    neighbors(b, nb);
    std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(),
                          std::back_inserter(common));
    for (uint32_t t : abTris) {
        const auto& T = s.tri[t];
        for (uint32_t u : T)
            if (u != a && u != b) opp.push_back(u);
    }
    std::sort(opp.begin(), opp.end());
    if (common != opp) return false;

    // 接缝保护：被摧毁的三角形不得是某条双锁定边（≠ 本边）的唯一三角形——
    // 否则那条接缝边在本组一侧就没有三角形了，跨组裂缝由此产生。
    for (uint32_t t : abTris) {
        const auto& T = s.tri[t];
        for (int k = 0; k < 3; ++k) {
            const uint32_t u = T[k], v = T[(k + 1) % 3];
            if ((u == a && v == b) || (u == b && v == a)) continue;
            if (s.locked[u] && s.locked[v]) {
                const auto e2 = s.edgeTris.find(edgeKey(u, v));
                if (e2 != s.edgeTris.end() && e2->second.size() == 1) return false;
            }
        }
    }

    // 法线翻转检查：所有存活邻接三角形，折叠后法线不得反向
    float p[3];
    choosePosition(s, a, b, p);
    for (uint32_t v : { a, b }) {
        for (uint32_t t : s.vertTris[v]) {
            if (!s.alive[t]) continue;
            if (std::find(abTris.begin(), abTris.end(), t) != abTris.end()) continue;
            float n0[3], n1[3];
            triNormal(s, t, nullptr, 0, n0);
            triNormal(s, t, p, v, n1);
            if (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] <= kFlipEps)
                return false;
        }
    }

    Quadric Q;
    for (int i = 0; i < 16; ++i) Q.q[i] = s.quad[a].q[i] + s.quad[b].q[i];
    outCost = evalQuadric(Q, p);
    return std::isfinite(outCost);
}

// 折叠执行：other 并入 survivor（锁定端点优先，否则小 id）。
void collapse(Simplifier& s, uint32_t a, uint32_t b, const float p[3]) {
    uint32_t surv = a, other = b;
    if (s.locked[b] && !s.locked[a]) { surv = b; other = a; }

    for (int i = 0; i < 16; ++i)
        s.quad[surv].q[i] += s.quad[other].q[i];
    if (!s.locked[surv])
        std::copy_n(p, 3, &s.pos[surv * 3]);

    const std::vector<uint32_t> list = s.vertTris[other];   // 拷贝：边遍历边改
    for (uint32_t t : list) {
        if (!s.alive[t]) continue;
        const auto T = s.tri[t];   // 原始三点
        const bool hasSurv = T[0] == surv || T[1] == surv || T[2] == surv;
        if (hasSurv) {
            // 同时含 surv 和 other = 本边的邻接三角形 → 折叠后退化为重复三角 → 杀
            s.alive[t] = 0;
            --s.aliveCount;
            for (int k = 0; k < 3; ++k) {
                const uint32_t u = T[k], v = T[(k + 1) % 3];
                const auto e = s.edgeTris.find(edgeKey(u, v));
                if (e != s.edgeTris.end()) {
                    e->second.erase(std::remove(e->second.begin(), e->second.end(), t),
                                    e->second.end());
                    if (e->second.empty()) s.edgeTris.erase(e);
                }
            }
            for (uint32_t u : T)
                if (u != other) {
                    auto& vt = s.vertTris[u];
                    vt.erase(std::remove(vt.begin(), vt.end(), t), vt.end());
                }
        } else {
            // 重定向：other → surv；边 (other,x) 变 (surv,x)
            int oi = T[0] == other ? 0 : T[1] == other ? 1 : 2;
            const int xi = (oi + 1) % 3, yi = (oi + 2) % 3;
            const uint32_t x = T[xi], y = T[yi];
            s.tri[t][oi] = surv;
            for (uint32_t w : { x, y }) {
                const auto e = s.edgeTris.find(edgeKey(other, w));
                if (e != s.edgeTris.end()) {
                    e->second.erase(std::remove(e->second.begin(), e->second.end(), t),
                                    e->second.end());
                    if (e->second.empty()) s.edgeTris.erase(e);
                }
                s.edgeTris[edgeKey(surv, w)].push_back(t);
            }
            auto& vtO = s.vertTris[other];
            vtO.erase(std::remove(vtO.begin(), vtO.end(), t), vtO.end());
            s.vertTris[surv].push_back(t);
        }
    }
    s.vertTris[other].clear();
    s.edgeTris.erase(edgeKey(a, b));
    ++s.collapses;
}

} // namespace

std::uint32_t buildCanonicalTriangles(const NMeshData& mesh,
                                      std::vector<std::uint32_t>& outIndices) {
    outIndices.clear();
    std::vector<uint32_t> canon;
    const uint32_t pointCount = weldPositions(mesh.vertices, canon);
    if (pointCount == 0) return 0;
    for (const auto& m : mesh.meshlets) {
        for (uint32_t t = 0; t < m.triangleCount; ++t) {
            const std::size_t base = m.triangleOffset + std::size_t(t) * 3;
            uint32_t ids[3];
            bool degenerate = false;
            for (int k = 0; k < 3; ++k) {
                ids[k] = canon[m.vertexOffset + mesh.triangles[base + k]];
                if (k > 0 && ids[k] == ids[0]) degenerate = true;
            }
            if (ids[1] == ids[2]) degenerate = true;
            if (!degenerate)
                outIndices.insert(outIndices.end(), { ids[0], ids[1], ids[2] });
        }
    }
    return pointCount;
}

bool simplifyGroups(const NMeshData& mesh, const GroupingResult& grouping,
                    SimplifyResult& out, std::string& outError,
                    float targetRatio) {
    out = SimplifyResult{};
    if (mesh.meshlets.empty() || mesh.vertices.empty()) {
        outError = "simplify: mesh has no meshlets/vertices";
        return false;
    }
    if (!(targetRatio > 0.f) || targetRatio > 1.f) {
        outError = "simplify: target ratio must be in (0,1]";
        return false;
    }
    if (grouping.clusterGroup.size() != mesh.meshlets.size() ||
        grouping.groupCount == 0) {
        outError = "simplify: grouping result does not match mesh";
        return false;
    }

    // ---- 1) canonical 三角形 soup + 组归属 ----
    std::vector<uint32_t> canon;
    const uint32_t pointCount = weldPositions(mesh.vertices, canon);
    if (pointCount == 0) {
        outError = "simplify: mesh has no vertices";
        return false;
    }
    std::vector<std::vector<std::array<uint32_t, 3>>> groupTris(grouping.groupCount);
    for (const auto& m : mesh.meshlets) {
        const uint32_t g = grouping.clusterGroup[&m - mesh.meshlets.data()];
        for (uint32_t t = 0; t < m.triangleCount; ++t) {
            const std::size_t base = m.triangleOffset + std::size_t(t) * 3;
            std::array<uint32_t, 3> ids = {
                canon[m.vertexOffset + mesh.triangles[base + 0]],
                canon[m.vertexOffset + mesh.triangles[base + 1]],
                canon[m.vertexOffset + mesh.triangles[base + 2]],
            };
            if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2]) continue;
            groupTris[g].push_back(ids);
        }
    }

    // ---- 2) 边界判定（组归属口径）：点被 ≥2 个组的簇引用 → 锁 ----
    std::vector<std::uint8_t> isBoundary(pointCount, 0);
    {
        std::unordered_map<uint32_t, uint32_t> firstGroup;   // 点 → 首见组
        for (const auto& m : mesh.meshlets) {
            const uint32_t g = grouping.clusterGroup[&m - mesh.meshlets.data()];
            for (uint32_t i = 0; i < m.vertexCount; ++i) {
                const uint32_t p = canon[m.vertexOffset + i];
                auto [it, inserted] = firstGroup.emplace(p, g);
                if (!inserted && it->second != g) isBoundary[p] = 1;
            }
        }
    }

    // ---- 3) 点位置初值（原位置；内部点折叠后覆盖）----
    out.pointPositions.assign(std::size_t(pointCount) * 3, 0.f);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        for (int k = 0; k < 3; ++k)
            out.pointPositions[std::size_t(canon[i]) * 3 + k] = mesh.vertices[i].pos[k];
    out.pointCount = pointCount;

    // 边界集合到此为止：只由本次 grouping 的组归属决定，不接收历史层锁点
    // （bevy from_mesh.rs lock_group_borders 每轮全量覆写；UE LockPosition 同理）。
    for (std::uint32_t p = 0; p < pointCount; ++p) {
        if (!isBoundary[p]) continue;
        out.lockedPoints.push_back(p);
        for (int k = 0; k < 3; ++k)
            out.lockedPositions.push_back(out.pointPositions[std::size_t(p) * 3 + k]);
    }

    // ---- 4) 逐组 QEM ----
    out.groupFirstTri.assign(grouping.groupCount, 0);
    out.groupTriCount.assign(grouping.groupCount, 0);
    out.perGroup.resize(grouping.groupCount);
    std::vector<std::vector<uint32_t>> localToCanon(grouping.groupCount);

    for (uint32_t g = 0; g < grouping.groupCount; ++g) {
        // 局部化：canonical → local
        std::unordered_map<uint32_t, uint32_t> localOf;
        Simplifier s;
        localToCanon[g].reserve(groupTris[g].size() * 3);
        for (const auto& T : groupTris[g])
            for (uint32_t p : T)
                if (localOf.emplace(p, uint32_t(s.locked.size())).second) {
                    localToCanon[g].push_back(p);
                    const float* pp = &out.pointPositions[std::size_t(p) * 3];
                    s.pos.insert(s.pos.end(), { pp[0], pp[1], pp[2] });
                    s.locked.push_back(0);
                    s.quad.emplace_back();
                    s.vertTris.emplace_back();
                }
        for (uint32_t p : out.lockedPoints) {
            auto it = localOf.find(p);
            if (it != localOf.end()) s.locked[it->second] = 1;
        }

        // 三角 + 邻接
        s.tri.reserve(groupTris[g].size());
        for (const auto& T : groupTris[g]) {
            const std::array<uint32_t, 3> L = {
                localOf[T[0]], localOf[T[1]], localOf[T[2]],
            };
            const uint32_t t = uint32_t(s.tri.size());
            s.tri.push_back(L);
            s.alive.push_back(1);
            ++s.aliveCount;
            for (int k = 0; k < 3; ++k)
                s.vertTris[L[k]].push_back(t);
            for (int k = 0; k < 3; ++k)
                s.edgeTris[edgeKey(L[k], L[(k + 1) % 3])].push_back(t);
        }

        // 二次型累积（面积加权平面）
        for (uint32_t t = 0; t < s.tri.size(); ++t) {
            const auto& T = s.tri[t];
            const float* p0 = &s.pos[T[0] * 3];
            const float* p1 = &s.pos[T[1] * 3];
            const float* p2 = &s.pos[T[2] * 3];
            const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
            const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
            const float n[3] = {
                e1[1] * e2[2] - e1[2] * e2[1],
                e1[2] * e2[0] - e1[0] * e2[2],
                e1[0] * e2[1] - e1[1] * e2[0],
            };
            const float area = 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (area < kMinTriArea) continue;   // 退化三角不贡献平面（法向权重 clamp）
            const float invLen = 0.5f / area;   // 1/|n|
            const float pl[4] = {
                n[0] * invLen, n[1] * invLen, n[2] * invLen,
                -(n[0] * p0[0] + n[1] * p0[1] + n[2] * p0[2]) * invLen,
            };
            for (uint32_t p : T) accumulatePlane(s.quad[p], pl, area);
        }

        // 折叠循环
        const uint32_t inputTris = s.aliveCount;
        const uint32_t targetTri =
            std::max<uint32_t>(1, uint32_t(std::lround(float(inputTris) * targetRatio)));
        std::set<std::tuple<float, uint32_t, uint32_t>> queue;
        for (const auto& [key, list] : s.edgeTris) {
            (void)list;
            float c;
            if (evaluateEdge(s, uint32_t(key >> 32), uint32_t(key & ~0u), c))
                queue.emplace(c, uint32_t(key >> 32), uint32_t(key & ~0u));
        }
        // 安全阀：病态输入下防死循环（正常远用不到）
        const std::size_t maxPops = std::size_t(s.tri.size()) * 64 + 100000;

        while (s.aliveCount > targetTri && !queue.empty() &&
               s.collapses < maxPops) {
            const auto [cost, a, b] = *queue.begin();
            queue.erase(queue.begin());
            if (!s.edgeTris.count(edgeKey(a, b))) continue;   // 已折叠/删除
            float fresh;
            if (!evaluateEdge(s, a, b, fresh)) continue;      // 拓扑拒绝（永久）
            if (fresh > cost) { queue.emplace(fresh, a, b); continue; }   // 陈旧低价条目
            float p[3];
            choosePosition(s, a, b, p);
            // survivor 与 collapse() 同规则：锁定端点优先，否则小 id
            const uint32_t surv = (s.locked[b] && !s.locked[a]) ? b : a;
            collapse(s, a, b, p);
            s.maxError = std::max(s.maxError, std::sqrt(std::max(fresh, 0.f)));
            // 重入队 survivor 的全部邻边（其余边状态未变）
            std::vector<uint32_t> nbs;
            for (uint32_t t : s.vertTris[surv]) {
                if (!s.alive[t]) continue;
                for (uint32_t u : s.tri[t])
                    if (u != surv) nbs.push_back(u);
            }
            std::sort(nbs.begin(), nbs.end());
            nbs.erase(std::unique(nbs.begin(), nbs.end()), nbs.end());
            for (uint32_t x : nbs) {
                float c;
                if (evaluateEdge(s, std::min(surv, x), std::max(surv, x), c))
                    queue.emplace(c, std::min(surv, x), std::max(surv, x));
            }
        }

        // 统计 + 输出（存活三角按序）
        out.groupFirstTri[g] = uint32_t(out.indices.size() / 3);
        out.groupTriCount[g] = s.aliveCount;
        for (uint32_t t = 0; t < s.tri.size(); ++t) {
            if (!s.alive[t]) continue;
            for (uint32_t L : s.tri[t])
                out.indices.push_back(localToCanon[g][L]);
        }
        auto& st = out.perGroup[g];
        st.inputTriangles = inputTris;
        st.outputTriangles = s.aliveCount;
        st.collapses = s.collapses;
        st.maxError = s.maxError;
    }

    // ---- 汇总 ----
    for (const auto& st : out.perGroup) {
        out.total.inputTriangles += st.inputTriangles;
        out.total.outputTriangles += st.outputTriangles;
        out.total.collapses += st.collapses;
        out.total.maxError = std::max(out.total.maxError, st.maxError);
    }
    return true;
}

} // namespace nl::core
