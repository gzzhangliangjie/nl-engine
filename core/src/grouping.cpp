// M2.0：簇聚组实现。流程：
//   1) 全局位置 weld（位级相等 → canonical 几何点，UV 缝忽略）
//   2) 簇邻接图：节点=簇、边=共享几何点、边权=共享点数（对称 CSR）
//   3) METIS_PartGraphKway：vwgt=簇三角数、固定 seed、nparts≈簇数/目标组大小
//   4) 连通性 fixup：METIS 分区不保证组内连通，不连通的组按共享边图
//      连通分量拆开（拆出的组只会更小，不影响简化正确性）
//
// bevy 0.15 实战校准点（docs/blogs/jms55-bevy-meshlet.md §2.1）：
//   - 分组只看顶点位置、忽略 UV 缝（本文件第 1 步）
//   - METIS 传固定 seed → 完全确定（本文件第 3 步）
//   - METIS 偶发超发/不连通 → 靠 fixup 兜底（第 4 步）

#include "nl/core/grouping.hpp"

#include <metis.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace nl::core {

bool groupClusters(const NMeshData& mesh, GroupingResult& out, std::string& outError) {
    out = GroupingResult{};
    const std::size_t clusterCount = mesh.meshlets.size();
    if (clusterCount == 0) {
        outError = "grouping: mesh has no meshlets";
        return false;
    }

    // ---- 1) 全局位置 weld：排序 + 位级相等归并成 canonical 连续编号 ----
    const std::size_t vertexCount = mesh.vertices.size();
    if (vertexCount == 0) {
        outError = "grouping: mesh has no vertices";
        return false;
    }
    std::vector<std::uint32_t> canon(vertexCount);
    {
        std::vector<std::uint32_t> order(vertexCount);
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&mesh](std::uint32_t a, std::uint32_t b) {
            for (int k = 0; k < 3; ++k)
                if (mesh.vertices[a].pos[k] != mesh.vertices[b].pos[k])
                    return mesh.vertices[a].pos[k] < mesh.vertices[b].pos[k];
            return a < b;
        });
        std::uint32_t canonId = 0;
        canon[order[0]] = 0;
        for (std::size_t i = 1; i < vertexCount; ++i) {
            const MeshVertex& a = mesh.vertices[order[i - 1]];
            const MeshVertex& b = mesh.vertices[order[i]];
            if (b.pos[0] != a.pos[0] || b.pos[1] != a.pos[1] || b.pos[2] != a.pos[2])
                ++canonId;
            canon[order[i]] = canonId;
        }
    }

    // ---- 2) 簇邻接：共享几何点 → 两两簇边累积边权 ----
    // canonical 点 → 出现过的簇列表（倒排）。canon 按全局顶点索引寻址。
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> pointToClusters;
    pointToClusters.reserve(vertexCount * 2);
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(clusterCount); ++c) {
        const Meshlet& m = mesh.meshlets[c];
        for (std::uint32_t i = 0; i < m.vertexCount; ++i)
            pointToClusters[canon[m.vertexOffset + i]].push_back(c);
    }

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edgeWeight;
    for (const auto& [pt, clusters] : pointToClusters) {
        if (clusters.size() < 2) continue;
        // 同一几何点被多个簇引用 → 两两相邻；权重按出现次数计
        for (std::size_t i = 0; i < clusters.size(); ++i)
            for (std::size_t j = i + 1; j < clusters.size(); ++j) {
                auto lo = std::minmax(clusters[i], clusters[j]);
                ++edgeWeight[{lo.first, lo.second}];
            }
    }

    // ---- 3) CSR 图 + METIS k-way 分区 ----
    std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> adj(clusterCount);
    for (const auto& [key, w] : edgeWeight) {
        adj[key.first].push_back({key.second, w});
        adj[key.second].push_back({key.first, w});
    }
    for (auto& a : adj)
        std::sort(a.begin(), a.end());  // CSR 确定性（unordered_map 迭代序不定）

    std::vector<idx_t> xadj(clusterCount + 1, 0);
    std::vector<idx_t> adjncy, adjwgt;
    std::vector<idx_t> vwgt(clusterCount);
    for (std::size_t c = 0; c < clusterCount; ++c) {
        xadj[c + 1] = xadj[c] + static_cast<idx_t>(adj[c].size());
        for (const auto& [nb, w] : adj[c]) {
            adjncy.push_back(static_cast<idx_t>(nb));
            adjwgt.push_back(static_cast<idx_t>(w));
        }
        vwgt[c] = static_cast<idx_t>(mesh.meshlets[c].triangleCount);  // 按三角量平衡
    }

    std::vector<std::uint32_t> part(clusterCount, 0);
    std::uint32_t groupCount = 1;
    if (clusterCount > 1) {
        // 划分数：向上取整防「组数不足导致末组堆积」；METIS 偶发超发由 fixup 拆分兜底
        const std::uint32_t target = kTargetMeshletsPerGroup;
        std::uint32_t nparts = static_cast<std::uint32_t>(
            (clusterCount + target - 1) / target);
        nparts = std::max(nparts, 1u);

        idx_t nvtxs = static_cast<idx_t>(clusterCount);
        idx_t ncon = 1;
        idx_t npartsIdx = static_cast<idx_t>(nparts);
        idx_t objval = 0;
        real_t ubvec[1] = { 1.02f };  // 组间轻微不平衡换组内简化空间（默认 1.001 太紧）
        idx_t options[METIS_NOPTIONS];
        METIS_SetDefaultOptions(options);
        options[METIS_OPTION_SEED] = 42;      // 固定 seed → 完全确定（bevy 0.15 校准点）

        // CONTIG（让每分区尽量连通）要求输入图整体连通，断连网格会直接报
        // rc=METIS_ERROR_INPUT；先 BFS 检测，连通才开（不连通靠 fixup 拆分兜底）
        {
            std::vector<std::uint8_t> reach(clusterCount, 0);
            std::vector<std::uint32_t> stack{0u};
            reach[0] = 1;
            std::uint32_t reached = 1;
            while (!stack.empty()) {
                const std::uint32_t c = stack.back();
                stack.pop_back();
                for (idx_t k = xadj[c]; k < xadj[c + 1]; ++k) {
                    const std::uint32_t nb = static_cast<std::uint32_t>(adjncy[k]);
                    if (!reach[nb]) { reach[nb] = 1; ++reached; stack.push_back(nb); }
                }
            }
            if (reached == clusterCount)
                options[METIS_OPTION_CONTIG] = 1;
        }

        std::vector<idx_t> metisPart(clusterCount, 0);
        const int rc = METIS_PartGraphKway(
            &nvtxs, &ncon, xadj.data(), adjncy.data(),
            vwgt.data(), nullptr, adjwgt.data(),
            &npartsIdx, nullptr, ubvec, options,
            &objval, metisPart.data());
        if (rc != METIS_OK) {
            outError = "grouping: METIS_PartGraphKway failed (rc=" + std::to_string(rc) + ")";
            return false;
        }
        for (std::size_t c = 0; c < clusterCount; ++c)
            part[c] = static_cast<std::uint32_t>(metisPart[c]);
        groupCount = nparts;
    }

    // ---- 4) 连通性 fixup：只沿组内共享边 BFS；不连通的组拆成连通分量 ----
    std::vector<std::uint32_t> finalGroup(clusterCount);
    std::uint32_t nextGroup = 0;
    for (std::uint32_t g = 0; g < groupCount; ++g) {
        // 收集本组成员
        std::vector<std::uint32_t> members;
        for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(clusterCount); ++c)
            if (part[c] == g) members.push_back(c);
        if (members.empty()) continue;  // METIS 可能给出空分区（罕见但合法）

        // 组内邻接表
        std::unordered_set<std::uint32_t> memberSet(members.begin(), members.end());
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> inGroup;
        for (std::uint32_t c : members)
            for (const auto& [nb, w] : adj[c])
                if (memberSet.count(nb)) inGroup[c].push_back(nb);

        // BFS 拆连通分量
        std::vector<std::uint8_t> seen(clusterCount, 0);
        std::uint32_t compCount = 0;
        for (std::uint32_t root : members) {
            if (seen[root]) continue;
            ++compCount;
            if (compCount > 1) ++out.splitFixups;
            std::vector<std::uint32_t> stack{root};
            seen[root] = 1;
            while (!stack.empty()) {
                const std::uint32_t c = stack.back();
                stack.pop_back();
                finalGroup[c] = nextGroup;
                for (std::uint32_t nb : inGroup[c])
                    if (!seen[nb]) { seen[nb] = 1; stack.push_back(nb); }
            }
            ++nextGroup;
        }
    }
    groupCount = nextGroup;
    if (groupCount == 0) {
        outError = "grouping: produced zero groups";
        return false;
    }

    // ---- 统计 + 输出 ----
    out.clusterGroup = std::move(finalGroup);
    out.groupCount = groupCount;
    std::vector<std::uint32_t> groupSize(groupCount, 0);
    for (std::uint32_t g : out.clusterGroup) ++groupSize[g];
    out.maxGroupSize = *std::max_element(groupSize.begin(), groupSize.end());
    return true;
}

} // namespace nl::core
