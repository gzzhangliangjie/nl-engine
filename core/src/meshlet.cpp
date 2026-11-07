// M1.1：meshlet 贪心构建实现。参考与取舍见 meshlet.hpp 头注释。
// 每三角预计算质心 + 单位法线 → CSR 顶点邻接（live 计数即未发射三角形数）
// → 拓扑优先级 + 空间评分贪心吸收 → 出界/无处可吸时收尾换簇。
// 覆盖率 100% 由算法结构保证：主循环只在发射时推进，兜底种子两路
// （簇边邻接 / 全局线性最近），任何未发射三角形最终都会被选中。

#include "nl/core/meshlet.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nl::core {
namespace {

struct TriCone {
    float px, py, pz;  // 质心
    float nx, ny, nz;  // 单位法线（退化三角形为 0 向量）
};

// meshoptimizer getMeshletScore：距离近 + 法线与簇锥同向者优先。
// spread = dot(三角法线, 簇锥轴)；cone 因子夹到 >= 1e-3 防零除。
float meshletScore(float distance, float spread, float coneWeight,
                   float expectedRadius) {
    float cone = 1.f - spread * coneWeight;
    cone = cone < 1e-3f ? 1e-3f : cone;
    return (1.f + distance / expectedRadius * (1.f - coneWeight)) * cone;
}

struct Builder {
    const float* pos = nullptr;
    std::size_t posStride = 0;  // floats
    const std::uint32_t* indices = nullptr;
    std::size_t vertexCount = 0;
    std::size_t faceCount = 0;
    std::uint32_t maxTri = 0;
    std::uint32_t maxVert = 0;
    float coneWeight = 0.f;

    std::vector<TriCone> cones;
    float expectedRadius = 0.f;

    // CSR 顶点邻接：按 canonical（位置 weld 后的）顶点建——glTF 导出常见
    // 每面拆顶点（Suzanne 11808 = 3936×3，索引零共享），几何连通性必须按位置。
    std::vector<std::uint32_t> canon;   // 原始顶点 → 位置相同的代表顶点
    std::vector<std::uint32_t> adjOffsets;
    std::vector<std::uint32_t> adjData;
    std::vector<std::uint32_t> live;       // 每 canonical 顶点的未发射三角形数
    std::vector<std::uint8_t> emitted;     // 每 triangle
    std::vector<std::uint32_t> usedStamp;  // 按【原始】索引：簇内条目去重（GPU 上限口径）
    std::vector<std::uint32_t> localIndex; // 按【原始】索引：簇内局部编号
    std::vector<std::uint32_t> canonStamp; // 按 canonical：curVerts 去重
    std::uint32_t stamp = 1;  // 从 1 起步：0 与 usedStamp 的零初始化撞车（已踩）

    BuiltMeshlets* out = nullptr;

    // 当前簇状态
    std::uint32_t curTri = 0, curVert = 0;
    std::uint32_t vertOffset = 0, triOffset = 0;
    float acc[6] = {};  // 质心和 xyz + 法线和 xyz
    std::vector<std::uint32_t> curVerts;  // 当前簇用到的全局顶点（算 bounds / 种子用）
    float lastCenter[3] = { 0, 0, 0 };
    bool haveLastCenter = false;
    std::uint32_t emittedCount = 0;  // 已发射三角形总数（主循环终止判据）

    const float* positionOf(std::uint32_t v) const {
        return pos + static_cast<std::size_t>(v) * posStride;
    }

    // ---- 当前簇锥：质心均值 + 归一化法线和 ----
    void coneOf(float outCone[6]) const {
        const float inv = curTri > 0 ? 1.f / static_cast<float>(curTri) : 0.f;
        for (int k = 0; k < 3; ++k) outCone[k] = acc[k] * inv;
        float len = std::sqrt(acc[3] * acc[3] + acc[4] * acc[4] + acc[5] * acc[5]);
        if (len > 0.f) {
            for (int k = 0; k < 3; ++k) outCone[3 + k] = acc[3 + k] / len;
        } else {
            outCone[3] = 0.f; outCone[4] = 0.f; outCone[5] = 1.f;
        }
    }

    // ---- 拓扑优先级 + 空间评分挑下一个三角形（meshoptimizer getNeighborTriangle 裁剪版）----
    // 只考虑能装进当前簇的未发射三角形；返回 ~0u 表示无。
    std::uint32_t neighborSearch() {
        float cone[6];
        coneOf(cone);
        float axis[3] = { cone[3], cone[4], cone[5] };

        std::uint32_t best = ~0u;
        int bestPriority = 5;
        float bestScore = std::numeric_limits<float>::max();

        for (std::uint32_t v : curVerts) {
            const std::uint32_t begin = adjOffsets[v], end = adjOffsets[v + 1];
            for (std::uint32_t t = begin; t < end; ++t) {
                const std::uint32_t tri = adjData[t];
                if (emitted[tri]) continue;

                const std::uint32_t a = canon[indices[tri * 3 + 0]];
                const std::uint32_t b = canon[indices[tri * 3 + 1]];
                const std::uint32_t c = canon[indices[tri * 3 + 2]];
                // extra 按【原始】索引计：GPU 上限口径是簇内条目数
                const std::uint32_t oa = indices[tri * 3 + 0];
                const std::uint32_t ob = indices[tri * 3 + 1];
                const std::uint32_t oc = indices[tri * 3 + 2];
                const int extra = (usedStamp[oa] != stamp) + (usedStamp[ob] != stamp) +
                                  (usedStamp[oc] != stamp);
                // 装不下直接跳过（簇已满的部分留给下一簇）
                if (curTri >= maxTri || curVert + static_cast<std::uint32_t>(extra) > maxVert)
                    continue;

                int priority;
                if (extra == 0)
                    priority = 0;  // 零新顶点最优先
                else if (live[a] == 1 || live[b] == 1 || live[c] == 1)
                    priority = 1;  // 悬挂三角形：留给下一簇代价高，先吸掉
                else if ((live[a] == 2) + (live[b] == 2) + (live[c] == 2) >= 2)
                    priority = 1 + extra;
                else
                    priority = 2 + extra;

                if (priority > bestPriority) continue;

                const TriCone& tc = cones[tri];
                const float dx = tc.px - cone[0], dy = tc.py - cone[1], dz = tc.pz - cone[2];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float spread = tc.nx * axis[0] + tc.ny * axis[1] + tc.nz * axis[2];
                const float score = meshletScore(distance, spread, coneWeight, expectedRadius);

                if (priority < bestPriority || score < bestScore) {
                    best = tri;
                    bestPriority = priority;
                    bestScore = score;
                }
            }
        }
        return best;
    }

    // ---- 发射一个三角形进当前簇。簇内条目按原始索引去重（局部号），
    // curVerts 按 canonical 去重（拓扑 / bounds / 种子用，位置相同不重复算）----
    void append(std::uint32_t tri) {
        std::uint8_t local[3];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = indices[tri * 3 + k];
            if (usedStamp[v] != stamp) {
                usedStamp[v] = stamp;
                localIndex[v] = static_cast<std::uint32_t>(curVert);
                out->vertices.push_back(v);
                ++curVert;
            }
            local[k] = static_cast<std::uint8_t>(localIndex[v]);

            const std::uint32_t cv = canon[v];
            if (canonStamp[cv] != stamp) {
                canonStamp[cv] = stamp;
                curVerts.push_back(cv);
            }
        }
        out->triangles.push_back(local[0]);
        out->triangles.push_back(local[1]);
        out->triangles.push_back(local[2]);
        ++curTri;

        const TriCone& tc = cones[tri];
        acc[0] += tc.px; acc[1] += tc.py; acc[2] += tc.pz;
        acc[3] += tc.nx; acc[4] += tc.ny; acc[5] += tc.nz;

        emitted[tri] = 1;
        ++emittedCount;  // 主循环推进的唯一步进——漏了它 while 永不退出（已踩）
        // live 按 canonical 扣减：几何意义上每位置只记一次
        std::uint32_t uniq[3];
        int uniqCount = 0;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t cv = canon[indices[tri * 3 + k]];
            bool dup = false;
            for (int j = 0; j < uniqCount; ++j) dup = dup || uniq[j] == cv;
            if (!dup) uniq[uniqCount++] = cv;
        }
        for (int j = 0; j < uniqCount; ++j) --live[uniq[j]];
    }

    // ---- 收尾当前簇：算 bounds、落盘记录、换代、选下一簇种子 ----
    void finalize() {
        if (curTri == 0) return;

        MeshletBounds bounds{};

        // Ritter 包围球（两趟）：AABB 中心 → 最远点 → 以该点为心取最远距离
        {
            float mn[3] = {  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max() };
            float mx[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
            for (std::uint32_t v : curVerts) {
                const float* p = positionOf(v);
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], p[k]);
                    mx[k] = std::max(mx[k], p[k]);
                }
            }
            float center[3];
            for (int k = 0; k < 3; ++k) center[k] = 0.5f * (mn[k] + mx[k]);
            float far1[3] = { center[0], center[1], center[2] };
            float d1 = -1.f;
            for (std::uint32_t v : curVerts) {
                const float* p = positionOf(v);
                const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d > d1) { d1 = d; far1[0] = p[0]; far1[1] = p[1]; far1[2] = p[2]; }
            }
            float d2 = -1.f;
            for (std::uint32_t v : curVerts) {
                const float* p = positionOf(v);
                const float dx = p[0] - far1[0], dy = p[1] - far1[1], dz = p[2] - far1[2];
                d2 = std::max(d2, dx * dx + dy * dy + dz * dz);
            }
            for (int k = 0; k < 3; ++k) bounds.sphere[k] = far1[k];
            bounds.sphere[3] = std::sqrt(std::max(d2, 0.f));
            lastCenter[0] = far1[0]; lastCenter[1] = far1[1]; lastCenter[2] = far1[2];
            haveLastCenter = true;
        }

        // 法线锥：轴 = 归一化的法线和；cutoff = min dot(三角法线, 轴)。
        // 叶子误差：簇内最长边长（世界空间）——M1.1 的确定性口径，S3 起被覆盖。
        bounds.coneAxis[0] = 0.f; bounds.coneAxis[1] = 0.f; bounds.coneAxis[2] = 1.f;
        bounds.coneCutoff = -1.f;
        bounds.error = 0.f;
        {
            float len = std::sqrt(acc[3] * acc[3] + acc[4] * acc[4] + acc[5] * acc[5]);
            bool axisValid = len > 1e-8f;
            float axis[3] = { 0.f, 0.f, 1.f };
            if (axisValid) {
                axis[0] = acc[3] / len; axis[1] = acc[4] / len; axis[2] = acc[5] / len;
            }
            float cutoff = 1.f;
            for (std::uint32_t i = 0; i < curTri; ++i) {
                const std::uint32_t tri = pendingTris[i];
                const TriCone& tc = cones[tri];
                if (axisValid) {
                    const float d = tc.nx * axis[0] + tc.ny * axis[1] + tc.nz * axis[2];
                    cutoff = std::min(cutoff, d);
                }
                const std::uint32_t a = indices[tri * 3 + 0];
                const std::uint32_t b = indices[tri * 3 + 1];
                const std::uint32_t c = indices[tri * 3 + 2];
                const float* pa = positionOf(a);
                const float* pb = positionOf(b);
                const float* pc = positionOf(c);
                for (int e = 0; e < 3; ++e) {
                    const float* p = (e == 0) ? pa : (e == 1) ? pb : pc;
                    const float* q = (e == 0) ? pb : (e == 1) ? pc : pa;
                    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
                    bounds.error = std::max(bounds.error, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            }
            if (axisValid) {
                bounds.coneAxis[0] = axis[0];
                bounds.coneAxis[1] = axis[1];
                bounds.coneAxis[2] = axis[2];
                bounds.coneCutoff = cutoff;  // 退化三角形法线 0 → dot=0，即夹到锥角 >= 90°
            } else {
                bounds.coneCutoff = -1.f;
            }
        }

        Meshlet rec{};
        rec.vertexOffset = vertOffset;
        rec.vertexCount = curVert;
        rec.triangleOffset = triOffset;
        rec.triangleCount = curTri;
        out->meshlets.push_back(rec);
        out->bounds.push_back(bounds);

        // 选下一簇种子：本簇顶点的邻接里未发射、live 和最小的三角形
        std::uint32_t seed = ~0u;
        std::uint32_t seedLive = std::numeric_limits<std::uint32_t>::max();
        for (std::uint32_t v : curVerts) {
            for (std::uint32_t t = adjOffsets[v]; t < adjOffsets[v + 1]; ++t) {
                const std::uint32_t tri = adjData[t];
                if (emitted[tri]) continue;
                const std::uint32_t l = live[indices[tri * 3 + 0]] +
                                        live[indices[tri * 3 + 1]] +
                                        live[indices[tri * 3 + 2]];
                if (l < seedLive) { seedLive = l; seed = tri; }
            }
        }
        nextSeed = seed;

        // 换代重置
        ++stamp;
        curTri = 0; curVert = 0;
        vertOffset = static_cast<std::uint32_t>(out->vertices.size());
        triOffset = static_cast<std::uint32_t>(out->triangles.size());
        for (int k = 0; k < 6; ++k) acc[k] = 0.f;
        curVerts.clear();
        pendingTris.clear();
    }

    std::uint32_t nextSeed = ~0u;
    std::vector<std::uint32_t> pendingTris;  // 当前簇已发射的三角形号（bounds 用）

    // append 时同步登记，finalize 后清空
    void recordPending(std::uint32_t tri) { pendingTris.push_back(tri); }
};

} // namespace

bool buildMeshlets(const float* positions, std::size_t positionStrideFloats,
                   std::size_t vertexCount,
                   const std::uint32_t* indices, std::size_t indexCount,
                   BuiltMeshlets& out,
                   std::uint32_t maxTriangles, std::uint32_t maxVertices,
                   float coneWeight) {
    if (!positions || !indices || positionStrideFloats < 3 ||
        indexCount == 0 || indexCount % 3 != 0 ||
        maxTriangles == 0 || maxTriangles > 512 ||
        maxVertices < 3 || maxVertices > 256 ||
        coneWeight < 0.f || coneWeight > 1.f)
        return false;
    for (std::size_t i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount) return false;

    out = BuiltMeshlets{};

    Builder b;
    b.pos = positions;
    b.posStride = positionStrideFloats;
    b.indices = indices;
    b.vertexCount = vertexCount;
    b.faceCount = indexCount / 3;
    b.maxTri = maxTriangles;
    b.maxVert = maxVertices;
    b.coneWeight = coneWeight;
    b.out = &out;

    // ---- 三角形锥（质心 + 单位法线）与期望半径 ----
    b.cones.resize(b.faceCount);
    float meshArea2 = 0.f;  // |cross| 和 = 2 × 面积和
    for (std::size_t i = 0; i < b.faceCount; ++i) {
        const std::uint32_t a = indices[i * 3 + 0];
        const std::uint32_t bb = indices[i * 3 + 1];
        const std::uint32_t c = indices[i * 3 + 2];
        const float* p0 = b.positionOf(a);
        const float* p1 = b.positionOf(bb);
        const float* p2 = b.positionOf(c);
        const float u[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const float w[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        const float nx = u[1] * w[2] - u[2] * w[1];
        const float ny = u[2] * w[0] - u[0] * w[2];
        const float nz = u[0] * w[1] - u[1] * w[0];
        const float area = std::sqrt(nx * nx + ny * ny + nz * nz);
        const float inv = area > 0.f ? 1.f / area : 0.f;
        TriCone& tc = b.cones[i];
        tc.px = (p0[0] + p1[0] + p2[0]) / 3.f;
        tc.py = (p0[1] + p1[1] + p2[1]) / 3.f;
        tc.pz = (p0[2] + p1[2] + p2[2]) / 3.f;
        tc.nx = nx * inv; tc.ny = ny * inv; tc.nz = nz * inv;
        meshArea2 += area;
    }
    const float avgArea = b.faceCount > 0 ? meshArea2 / static_cast<float>(b.faceCount) * 0.5f : 0.f;
    // 假设簇近似方形补丁：期望半径 = sqrt(平均面积 × 上限) / 2
    b.expectedRadius = std::sqrt(avgArea * static_cast<float>(maxTriangles)) * 0.5f;

    // ---- 位置 weld：位级相等的位置归到同一代表顶点（glTF 拆缝顶点共享几何）----
    b.canon.resize(vertexCount);
    {
        std::vector<std::uint32_t> order(vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i)
            order[i] = static_cast<std::uint32_t>(i);
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t bb) {
            const float* pa = b.positionOf(a);
            const float* pb = b.positionOf(bb);
            for (int k = 0; k < 3; ++k)
                if (pa[k] != pb[k]) return pa[k] < pb[k];
            return a < bb;
        });
        std::uint32_t head = order[0];
        b.canon[head] = head;
        for (std::size_t i = 1; i < vertexCount; ++i) {
            const std::uint32_t v = order[i];
            const float* p = b.positionOf(v);
            const float* q = b.positionOf(head);
            if (p[0] == q[0] && p[1] == q[1] && p[2] == q[2]) {
                b.canon[v] = head;
            } else {
                head = v;
                b.canon[v] = v;
            }
        }
    }

    // ---- CSR 顶点邻接（按 canonical）；live 初始 = 含该位置的三角形数 ----
    b.adjOffsets.assign(vertexCount + 1, 0);
    for (std::size_t i = 0; i < indexCount; ++i)
        ++b.adjOffsets[b.canon[indices[i]] + 1];
    for (std::size_t v = 0; v < vertexCount; ++v) b.adjOffsets[v + 1] += b.adjOffsets[v];
    b.adjData.resize(indexCount);
    {
        std::vector<std::uint32_t> cursor(b.adjOffsets.begin(), b.adjOffsets.end() - 1);
        for (std::size_t t = 0; t < b.faceCount; ++t) {
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t cv = b.canon[indices[t * 3 + k]];
                b.adjData[cursor[cv]++] = static_cast<std::uint32_t>(t);
            }
        }
        // 重复（canonical）顶点的退化三角形会在同一列表里出现多次——live 按
        // 去重扣减、emitted 跳过保证正确性，这里不去重（规模不敏感）。
        b.live.assign(b.adjOffsets.begin(), b.adjOffsets.end() - 1);
    }
    b.emitted.assign(b.faceCount, 0);
    b.usedStamp.assign(vertexCount, 0);
    b.localIndex.assign(vertexCount, 0);
    b.canonStamp.assign(vertexCount, 0);

    // ---- 主循环 ----
    // 首个种子：meshoptimizer 式“角落”法——质心离包围盒最小角最近的三角形
    float corner[3] = { std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max() };
    for (const TriCone& tc : b.cones) {
        corner[0] = std::min(corner[0], tc.px);
        corner[1] = std::min(corner[1], tc.py);
        corner[2] = std::min(corner[2], tc.pz);
    }

    while (b.emittedCount < b.faceCount) {
        std::uint32_t best = ~0u;
        if (b.curTri == 0) {
            if (b.nextSeed != ~0u && !b.emitted[b.nextSeed]) {
                best = b.nextSeed;
            } else {
                // 兜底：断连组件 / 邻接耗尽——全局线性找离参考点最近的未发射三角形
                const float* ref = b.haveLastCenter ? b.lastCenter : corner;
                float bestD = std::numeric_limits<float>::max();
                for (std::size_t t = 0; t < b.faceCount; ++t) {
                    if (b.emitted[t]) continue;
                    const TriCone& tc = b.cones[t];
                    const float dx = tc.px - ref[0], dy = tc.py - ref[1], dz = tc.pz - ref[2];
                    const float d = dx * dx + dy * dy + dz * dz;
                    if (d < bestD) { bestD = d; best = static_cast<std::uint32_t>(t); }
                }
            }
            b.recordPending(best);
            b.append(best);
        } else {
            best = b.neighborSearch();
            if (best == ~0u) {
                b.finalize();
                continue;
            }
            b.recordPending(best);
            b.append(best);
        }

        if (b.curTri >= b.maxTri) b.finalize();  // 自然满簇即收尾
    }
    b.finalize();  // 最后一个未满簇

    return true;
}

} // namespace nl::core
