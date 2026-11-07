// M2.1：组内 QEM 简化 + 边界锁定实现。流程：
//   1) weldPositions → canonical 点；簇展开成 canonical 三角形（重复点三角剔除）
//   2) 边界判定看组归属：canonical 点被 ≥2 个组的簇引用 → 锁死
//   3) 逐组折叠：结构语义对齐 meshoptimizer simplifier.cpp 主循环——
//      源端点并入目标端点（无最优点）；多 pass 主循环：pass 内每顶点至多
//      参与一次折叠且有序不重排，pass 间从当前拓扑重建候选（L2499-2552）；
//      link condition + cos75° 单步翻转预检拒绝坏折叠；再加绝对朝向锚
//      （超出官方的加码）：锚 = 变形后法线 vs 当前角点法线和，随折叠换角
//      同步更新、预判用更新后参照（与 check-orient 检查器同口径）。
//      M2.3a 判决实验定稿：摘锚纯官方口径（cos75°+多 pass+开放边界锁全抄）
//      单层 inward=1（oracle 官方同判据 2）但跨层累计 L1=1→L6=8（worst
//      cos -0.972）——官方两家没有逐层硬目标链场景，无现成机制可抄；
//      锚把全链 inward 压到 0，比率无损（50.1% vs oracle 49.5%）。
//   4) 折叠到 round(inputTris * ratio) 即停；存活三角按序输出 canonical soup
//
// 确定性：unordered_map 只做查找不做结果序迭代；候选表全排序（代价平局按
// 源/目标点编号决胜）；同输入同输出。复杂度：组内 ≤ ~1024 tri，足够。

#include "nl/core/simplify.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <tuple>
#include <unordered_map>

namespace nl::core {

namespace {

using std::uint32_t;

constexpr float kMinTriArea = 1e-14f;   // 退化三角形不贡献平面（法向权重 clamp）
constexpr float kMinDetRel  = 1e-10f;   // 3x3 求解失败阈值（相对元素尺度）
// 翻转预检阈值：转角 > ~75° 即拒绝（cos75° = 0.25，meshoptimizer
// simplifier.cpp:1230-1246 口径）。纯 90°（dot<=0）不够：一串各自
// 「合法」的近 90° 折叠累计起来照样把法线翻面（S2.0 画廊黑斑的根因）。
constexpr float kFlipCos = 0.25f;
// 绝对朝向锚：变形后法线与本三角「**当前**三个角点法线之和」的夹角不得
// 超过 ~87°（kOrientCos=0.05 ≈ 标准线 cos=0 留数值余量）。参考随折叠换角
// 同步更新（collapse 重定向分支），与 check-orient 检查器同口径——检查器
// 用最终网格顶点法线（rebuildMeshlets 继承 = L0 原始场），锚必须盯同一
// 参照系，否则检查器看到的朝内面锚看不见（M2.3a 判决实验抓到的逃逸
// 路径）。锚长度近零（输入无法线数据，如合成测试网格）→ 本三角锚
// 不适用，退化为纯官方 cos75° 判定。
constexpr float kOrientCos = 0.05f;
constexpr float kOrientMinLen = 1e-4f;

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

// 组内简化器：局部点编号 0..P-1（canonical → 局部由装配层映射）。
// M2.3a 起对齐 meshoptimizer simplifier.cpp 的结构性语义：
//   - 折叠 = 源端点并入目标端点，目标位置不动（无二次型最优点，
//     meshopt 主循环只有 collapse_remap[i0]=i1，位置从不创造）；
//   - 多 pass 主循环：pass 内每顶点至多参与一次折叠（collapse_locked →
//     moved，每 pass 清零）且有序不重排；pass 间从当前拓扑重建候选；
struct Simplifier {
    std::vector<float> pos;                 // 3*P，只读（源点消失，无新位置）
    std::vector<float> nrm;                 // 3*P，点的绝对朝向参考（输入法线和，恒外向）
    std::vector<std::uint8_t> locked;       // 组间边界点
    std::vector<std::uint8_t> moved;        // meshopt collapse_locked：参与过折叠即冻结
    std::vector<Quadric> quad;
    std::vector<std::array<float, 3>> triRef;   // 每三角绝对朝向锚 = 三点参考法线和
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

// 单方向折叠评估（meshopt 口径）：src 并入 dst，dst 位置不动，src 消失。
// 返回 false = 拒绝（拓扑/锁定/冻结/翻转/锚定）。cost = 合并二次型在 dst
// 位置的值（静态量：位置与二次型全程不变， meshopt 不中途重排的原因）。
bool evaluateCollapse(const Simplifier& s, uint32_t src, uint32_t dst, float& outCost) {
    const auto it = s.edgeTris.find(edgeKey(src, dst));
    if (it == s.edgeTris.end() || it->second.empty()) return false;
    const std::vector<uint32_t>& abTris = it->second;
    if (abTris.size() > 2) return false;                    // 非流形边
    if (s.locked[src]) return false;                        // 锁点不能动（不能当源；dst 锁定 = 并入接缝，合法）
    if (s.moved[src] || s.moved[dst]) return false;         // meshopt collapse_locked
    if (s.aliveCount <= abTris.size()) return false;        // 不许清空整组
    // link condition：N(src) ∩ N(dst) == edge(src,dst) 的对顶点集合
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
    neighbors(src, na);
    neighbors(dst, nb);
    std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(),
                          std::back_inserter(common));
    for (uint32_t t : abTris) {
        const auto& T = s.tri[t];
        for (uint32_t u : T)
            if (u != src && u != dst) opp.push_back(u);
    }
    std::sort(opp.begin(), opp.end());
    if (common != opp) return false;

    // 接缝保护（我们的必要补充：meshopt 锁全部拓扑边界，我们只锁组间边界）：
    // 被摧毁的三角形不得是某条双锁定边（≠ 本边）的唯一三角形——否则那条
    // 接缝边在本组一侧就没有三角形了，跨组裂缝由此产生。
    for (uint32_t t : abTris) {
        const auto& T = s.tri[t];
        for (int k = 0; k < 3; ++k) {
            const uint32_t u = T[k], v = T[(k + 1) % 3];
            if ((u == src && v == dst) || (u == dst && v == src)) continue;
            if (s.locked[u] && s.locked[v]) {
                const auto e2 = s.edgeTris.find(edgeKey(u, v));
                if (e2 != s.edgeTris.end() && e2->second.size() == 1) return false;
            }
        }
    }

    // 翻转预检，两道（都在折叠应用前，检不过 = 拒绝折叠）：
    //   1) 单步 cos75°（meshoptimizer hasTriangleFlip 口径）：含 src 的
    //      三角形折叠后法线转角不得超过 ~75°（dst 不动，只有 src 侧变形）；
    //   2) 绝对朝向锚（超出官方的加码，M2.3a 判决实验定稿保留）：变形后
    //      法线与本三角的「三点当前法线和」夹角不得超过 ~87°，把贴着 90°
    //      线的折叠拒掉——纯官方口径跨层累计 L1=1→L6=8，锚压到 0。
    // 零向量（原三角退化 / 折叠后退化）时退化为拒绝，方向保守正确。
    const float* pDst = &s.pos[dst * 3];
    for (uint32_t t : s.vertTris[src]) {
        if (!s.alive[t]) continue;
        if (std::find(abTris.begin(), abTris.end(), t) != abTris.end()) continue;
        float n0[3], n1[3];
        triNormal(s, t, nullptr, 0, n0);
        triNormal(s, t, pDst, src, n1);
        const float ndp = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2];
        const float abc = n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2];
        const float abd = n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2];
        if (ndp <= kFlipCos * std::sqrt(abc * abd))
            return false;   // flip
        const float ref[3] = {
            s.triRef[t][0] + s.nrm[std::size_t(dst) * 3 + 0] - s.nrm[std::size_t(src) * 3 + 0],
            s.triRef[t][1] + s.nrm[std::size_t(dst) * 3 + 1] - s.nrm[std::size_t(src) * 3 + 1],
            s.triRef[t][2] + s.nrm[std::size_t(dst) * 3 + 2] - s.nrm[std::size_t(src) * 3 + 2],
        };
        const float refLen = std::sqrt(ref[0] * ref[0] + ref[1] * ref[1] + ref[2] * ref[2]);
        if (refLen > kOrientMinLen) {
            const float ndpRef = n1[0] * ref[0] + n1[1] * ref[1] + n1[2] * ref[2];
            const float n1Len = std::sqrt(abd);
            if (ndpRef <= kOrientCos * n1Len * refLen)
                return false;   // anchor
        }
        // refLen 退化（合成网格 / 输入无法线数据）→ 本三角锚不适用，
        // 退化为纯官方 cos75° 判定（锚只在有法线信息时加约束）。
    }

    Quadric Q;
    for (int i = 0; i < 16; ++i) Q.q[i] = s.quad[src].q[i] + s.quad[dst].q[i];
    outCost = evalQuadric(Q, pDst);
    return std::isfinite(outCost);
}

// 折叠执行（meshopt 口径）：src 消失并入 dst；dst 位置不动，无新位置。
void collapse(Simplifier& s, uint32_t src, uint32_t dst) {
    for (int i = 0; i < 16; ++i)
        s.quad[dst].q[i] += s.quad[src].q[i];
    s.moved[src] = 1;
    s.moved[dst] = 1;   // meshopt collapse_locked：参与过折叠的顶点本轮冻结

    const std::vector<uint32_t> list = s.vertTris[src];   // 拷贝：边遍历边改
    for (uint32_t t : list) {
        if (!s.alive[t]) continue;
        const auto T = s.tri[t];   // 原始三点
        const bool hasDst = T[0] == dst || T[1] == dst || T[2] == dst;
        if (hasDst) {
            // 同时含 src 和 dst = 本边的邻接三角形 → 折叠后退化为重复三角 → 杀
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
                if (u != src) {
                    auto& vt = s.vertTris[u];
                    vt.erase(std::remove(vt.begin(), vt.end(), t), vt.end());
                }
        } else {
            // 重定向：src → dst；边 (src,x) 变 (dst,x)
            int si = T[0] == src ? 0 : T[1] == src ? 1 : 2;
            const int xi = (si + 1) % 3, yi = (si + 2) % 3;
            const uint32_t x = T[xi], y = T[yi];
            s.tri[t][si] = dst;
            // 绝对朝向锚同步：triRef 必须始终 = **当前**三个角点的法线和
            // （检查器/check-orient 的口径）。
            for (int k = 0; k < 3; ++k)
                s.triRef[t][k] += s.nrm[std::size_t(dst) * 3 + k]
                                - s.nrm[std::size_t(src) * 3 + k];
            for (uint32_t w : { x, y }) {
                const auto e = s.edgeTris.find(edgeKey(src, w));
                if (e != s.edgeTris.end()) {
                    e->second.erase(std::remove(e->second.begin(), e->second.end(), t),
                                    e->second.end());
                    if (e->second.empty()) s.edgeTris.erase(e);
                }
                s.edgeTris[edgeKey(dst, w)].push_back(t);
            }
            auto& vtS = s.vertTris[src];
            vtS.erase(std::remove(vtS.begin(), vtS.end(), t), vtS.end());
            s.vertTris[dst].push_back(t);
        }
    }
    s.vertTris[src].clear();
    s.edgeTris.erase(edgeKey(src, dst));
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
    // 同时汇总 canonical 点法线（输入网格顶点法线，恒外向）——绝对朝向锚
    // 的数据源。UV/法线接缝处同位置多份顶点，直接求和即可（方向一致）。
    std::vector<float> canonNormals(std::size_t(pointCount) * 3, 0.f);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        for (int k = 0; k < 3; ++k)
            canonNormals[std::size_t(canon[i]) * 3 + k] += mesh.vertices[i].normal[k];
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

    // ---- 2b) 模型开放边界锁（meshopt LockBorder 语义）：开放边（soup 中只挂
    // 1 个三角形的边）端点锁死——眼窝/轮廓等开放边界不随简化收缩（L0 眼窝
    // 本来就是开放边界，不锁的话轮廓逐层退缩、开口越粗越大）。不入
    // lockedPoints（那是 S3 组间接缝语义）；每层拓扑重新推导，无需持久化。
    std::vector<std::uint8_t> isModelBorder(pointCount, 0);
    {
        std::unordered_map<std::uint64_t, int> edgeCount;
        edgeCount.reserve(groupTris.size() * 64);
        for (const auto& gt : groupTris)
            for (const auto& T : gt)
                for (int k = 0; k < 3; ++k) {
                    const uint32_t a = T[k], b = T[(k + 1) % 3];
                    ++edgeCount[edgeKey(std::min(a, b), std::max(a, b))];
                }
        for (const auto& [key, cnt] : edgeCount) {
            if (cnt != 1) continue;   // 流形内边 / 非流形边都不算开放边界
            isModelBorder[uint32_t(key >> 32)] = 1;
            isModelBorder[uint32_t(key & ~0u)] = 1;
        }
    }

    // ---- 3) 点位置初值（原位置；内部点折叠后覆盖）----
    out.pointPositions.assign(std::size_t(pointCount) * 3, 0.f);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        for (int k = 0; k < 3; ++k)
            out.pointPositions[std::size_t(canon[i]) * 3 + k] = mesh.vertices[i].pos[k];
    // 点法线 = 本层输入法线和（rebuildMeshlets 继承它，不重算——见 simplify.hpp）
    out.pointNormals = canonNormals;
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
                    const float* nn = &canonNormals[std::size_t(p) * 3];
                    s.pos.insert(s.pos.end(), { pp[0], pp[1], pp[2] });
                    s.nrm.insert(s.nrm.end(), { nn[0], nn[1], nn[2] });
                    s.locked.push_back(0);
                    s.moved.push_back(0);
                    s.quad.emplace_back();
                    s.vertTris.emplace_back();
                }
        for (uint32_t p : out.lockedPoints) {
            auto it = localOf.find(p);
            if (it != localOf.end()) s.locked[it->second] = 1;
        }
        for (std::uint32_t p = 0; p < pointCount; ++p) {
            if (!isModelBorder[p]) continue;
            auto it = localOf.find(p);
            if (it != localOf.end()) s.locked[it->second] = 1;
        }

        // 三角 + 邻接 + 绝对朝向锚
        s.tri.reserve(groupTris[g].size());
        for (const auto& T : groupTris[g]) {
            const std::array<uint32_t, 3> L = {
                localOf[T[0]], localOf[T[1]], localOf[T[2]],
            };
            const uint32_t t = uint32_t(s.tri.size());
            s.tri.push_back(L);
            s.triRef.push_back({
                s.nrm[L[0] * 3 + 0] + s.nrm[L[1] * 3 + 0] + s.nrm[L[2] * 3 + 0],
                s.nrm[L[0] * 3 + 1] + s.nrm[L[1] * 3 + 1] + s.nrm[L[2] * 3 + 1],
                s.nrm[L[0] * 3 + 2] + s.nrm[L[1] * 3 + 2] + s.nrm[L[2] * 3 + 2],
            });
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

        // 折叠循环（meshopt 多 pass 主循环口径，simplifier.cpp:2499-2552）：
        // 每个 pass：collapse_locked 清零 → 从当前拓扑重建候选 → 排序 →
        // pass 内单遍有序执行**不重排**（每顶点至多参与一次折叠）；pass 间
        // 拓扑/二次型已变，全部重算。终止：达到目标 / 无候选 / 本 pass 零
        // 折叠（无进展）。误差软中止（error_goal，L1558-1567）是性能优化，
        // 不影响收敛结果，不抄。
        const uint32_t inputTris = s.aliveCount;
        const uint32_t targetTri =
            std::max<uint32_t>(1, uint32_t(std::lround(float(inputTris) * targetRatio)));
        struct Cand { float cost; uint32_t src, dst; };
        std::vector<Cand> cands;
        for (;;) {
            if (s.aliveCount <= targetTri) break;
            // moved 必须在候选构建前清零：上 pass 的冻结不能过滤本 pass 候选
            std::fill(s.moved.begin(), s.moved.end(), std::uint8_t(0));
            cands.clear();
            for (const auto& [key, list] : s.edgeTris) {
                (void)list;
                const uint32_t a = uint32_t(key >> 32), b = uint32_t(key & ~0u);
                float c;
                if (evaluateCollapse(s, a, b, c)) cands.push_back({ c, a, b });
                if (evaluateCollapse(s, b, a, c)) cands.push_back({ c, b, a });
            }
            if (cands.empty()) break;
            std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
                if (x.cost != y.cost) return x.cost < y.cost;
                if (x.src != y.src) return x.src < y.src;
                return x.dst < y.dst;
            });
            uint32_t done = 0;
            for (const Cand& cand : cands) {
                if (s.aliveCount <= targetTri) break;       // pass 内三角目标达成
                if (!s.edgeTris.count(edgeKey(cand.src, cand.dst))) continue;   // 本 pass 已折叠
                if (s.moved[cand.src] || s.moved[cand.dst]) continue;
                float fresh;
                if (!evaluateCollapse(s, cand.src, cand.dst, fresh)) continue;  // 拓扑/翻转拒绝
                collapse(s, cand.src, cand.dst);
                ++done;
                s.maxError = std::max(s.maxError, std::sqrt(std::max(fresh, 0.f)));
            }
            if (done == 0) break;   // 无进展（全被拓扑/翻转/锁拒绝）
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
