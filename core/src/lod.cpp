// M2.2：逐层 LOD 循环实现。标准做法见 lod.hpp 的源码对照，不引入自造机制。
// M3.0：重切从「全局一次 buildMeshlets」改为**按组独立重切**（UE ReduceGroup
// 组内 Merged+SplitCluster / bevy from_mesh.rs:154 每组独立 compute_meshlets），
// 父簇绝不跨组 → 每个父簇恰有一个生成组，S5 的组原子 cut 才成立；同时记录
// DAG 血缘（组 error = max(组简化误差, 成员簇误差)，UE :1441+:1587）并统一
// 覆写父簇 bounds.error（UE :1593）。

#include "nl/core/lod.hpp"

#include "nl/core/grouping.hpp"
#include "nl/core/jobs.hpp"    // P8 M-J3：按组重切 / 组误差并行（拼接串行）
#include "nl/core/meshlet.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nl::core {
namespace {

std::uint32_t triangleCount(const NMeshData& mesh) {
    std::uint32_t n = 0;
    for (const auto& m : mesh.meshlets) n += m.triangleCount;
    return n;
}

// 两球包围并（UE FSphere3f(Children_Bounds) 同口径）：包含关系直接吞并，
// 否则沿连心线取 (d + r1 + r2) / 2。确定性，成员按簇号顺序折叠。
void sphereUnion(const float a[4], const float b[4], float out[4]) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d + a[3] <= b[3]) { std::memcpy(out, b, sizeof(float) * 4); return; }
    if (d + b[3] <= a[3]) { std::memcpy(out, a, sizeof(float) * 4); return; }
    if (d <= 1e-12f) {
        out[0] = a[0]; out[1] = a[1]; out[2] = a[2];
        out[3] = std::max(a[3], b[3]);
        return;
    }
    const float r = 0.5f * (d + a[3] + b[3]);
    const float t = (r - a[3]) / d;   // 新球心 = a + t * (b - a)
    out[0] = a[0] + dx * t;
    out[1] = a[1] + dy * t;
    out[2] = a[2] + dz * t;
    out[3] = r;
}

// 按组重切（M3.0）：每组独立 buildMeshlets，父簇窗口按组序拼接（组 g 的父簇
// 在 out.meshlets 中连续无缝）。同时算组 error（= max(组简化误差, 全部成员簇
// 误差)，成员误差对 L0 = 叶子最长边、对 L≥1 = 生成组 error——bounds.error
// 上一轮已覆写）并覆写父簇 bounds.error（UE :1593）。
struct GroupSplit {
    std::uint32_t firstCluster = 0;   // 本组父簇区间起点（下一层文件簇号）
    std::uint32_t clusterCount = 0;
    float error = 0.f;                // 组 LODError（ParentLODError）
    float simplifyError = 0.f;        // 本次组内简化误差
    float sphere[4];                  // 成员簇包围球并（UE :1445）
};

bool rebuildMeshletsPerGroup(const NMeshData& current, const GroupingResult& g,
                             const SimplifyResult& sr, NMeshData& out,
                             std::vector<GroupSplit>& outGroups, std::string& outError,
                             unsigned threads) {
    out = NMeshData{};
    outGroups.clear();
    if (sr.indices.empty() || sr.pointCount == 0) {
        outError = "lod: simplified soup is empty";
        return false;
    }

    // 每组成员簇误差最大值（组误差下限，UE :1441 对 Children 的 LODError 取 max）
    std::vector<float> memberMax(g.groupCount, 0.f);
    for (std::size_t c = 0; c < current.meshlets.size(); ++c) {
        const std::uint32_t grp = g.clusterGroup[c];
        if (grp >= g.groupCount) {
            outError = "lod: cluster group id out of range";
            return false;
        }
        memberMax[grp] = std::max(memberMax[grp], current.bounds[c].error);
    }

    std::vector<std::uint32_t> vertexPoints;   // canonical 点号（最后转 MeshVertex）
    std::vector<std::uint8_t> localTris;
    outGroups.reserve(g.groupCount);

    // P8 M-J3 两阶段：并行算组（buildMeshlets 重切 + 组误差/组球），串行拼接。
    // 误差链口径（UE :1441+:1587）= max(成员簇误差, 简化误差)，per-group 独立可算；
    // 层级推进（L0→LN 自底向上）在外层 for 循环，保持串行——「合并链必须串行」。
    struct GroupBuild {
        BuiltMeshlets built;
        GroupSplit gs;
        bool ok = false;
        std::string error;
    };
    std::vector<GroupBuild> slots(g.groupCount);
    const auto perGroupBody = [&](std::size_t b, std::size_t e) {
      for (std::size_t gIdx = b; gIdx < e; ++gIdx) {
        const std::uint32_t grp = std::uint32_t(gIdx);
        GroupBuild& slot = slots[grp];
        const std::uint32_t firstTri = sr.groupFirstTri[grp];
        const std::uint32_t triCount = sr.groupTriCount[grp];
        if (triCount == 0) {
            slot.error = "lod: group " + std::to_string(grp) + " simplified to nothing";
            continue;
        }
        BuiltMeshlets built;
        if (!buildMeshlets(sr.pointPositions.data(), 3, sr.pointCount,
                           sr.indices.data() + std::size_t(firstTri) * 3,
                           std::size_t(triCount) * 3, built)) {
            slot.error = "lod: buildMeshlets rejected simplified soup (group " +
                         std::to_string(grp) + ")";
            continue;
        }
        if (built.meshlets.empty()) {
            slot.error = "lod: recluster produced no meshlets (group " +
                         std::to_string(grp) + ")";
            continue;
        }

        // 组 error（UE :1441+:1587），全部父簇统一携带（UE :1593）
        GroupSplit gs;
        gs.simplifyError = sr.perGroup[grp].maxError;
        gs.error = std::max(memberMax[grp], gs.simplifyError);
        for (auto& bd : built.bounds) bd.error = gs.error;

        // 组球 = 成员簇包围球并（UE :1445）
        {
            bool first = true;
            for (std::size_t c = 0; c < current.meshlets.size(); ++c) {
                if (g.clusterGroup[c] != grp) continue;
                if (first) {
                    std::memcpy(gs.sphere, current.bounds[c].sphere, sizeof(gs.sphere));
                    first = false;
                } else {
                    float u[4];
                    sphereUnion(gs.sphere, current.bounds[c].sphere, u);
                    std::memcpy(gs.sphere, u, sizeof(u));
                }
            }
        }

        slot.built = std::move(built);
        slot.gs = gs;
        slot.ok = true;
      }
    };
    {
        nl::core::jobs::ThreadPool pool(threads);
        pool.parallelFor(g.groupCount, perGroupBody, 1);
    }

    // ---- 串行拼接：窗口前缀（vertBase/triBase）依赖共享池当前 size ----
    for (std::uint32_t grp = 0; grp < g.groupCount; ++grp) {
        const GroupBuild& slot = slots[grp];
        if (!slot.ok) {
            outError = slot.error;
            return false;
        }
        BuiltMeshlets built = slot.built;   // 窗口平移就地改 offset

        // 窗口平移拼接：顶点/三角池共享，簇窗口偏移累加
        const std::uint32_t vertBase = std::uint32_t(vertexPoints.size());
        const std::uint32_t triBase = std::uint32_t(localTris.size());
        for (auto& m : built.meshlets) {
            m.vertexOffset += vertBase;
            m.triangleOffset += triBase;
        }

        GroupSplit gs = slot.gs;
        gs.firstCluster = std::uint32_t(out.meshlets.size());
        gs.clusterCount = std::uint32_t(built.meshlets.size());

        out.meshlets.insert(out.meshlets.end(), built.meshlets.begin(), built.meshlets.end());
        out.bounds.insert(out.bounds.end(), built.bounds.begin(), built.bounds.end());
        vertexPoints.insert(vertexPoints.end(), built.vertices.begin(), built.vertices.end());
        localTris.insert(localTris.end(), built.triangles.begin(), built.triangles.end());
        outGroups.push_back(gs);
    }

    out.triangles = std::move(localTris);
    out.vertices.resize(vertexPoints.size());
    for (std::size_t i = 0; i < vertexPoints.size(); ++i) {
        MeshVertex v{};
        const std::uint32_t p = vertexPoints[i];
        for (int k = 0; k < 3; ++k)
            v.pos[k] = sr.pointPositions[std::size_t(p) * 3 + k];
        // 法线继承：canonical 点法线和归一化后写入（长度近零保持零向量）。
        const float* n = &sr.pointNormals[std::size_t(p) * 3];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-20f)
            for (int k = 0; k < 3; ++k)
                v.normal[k] = n[k] / len;
        out.vertices[i] = v;
    }
    return true;
}

// 顶层/收敛层：没有生成更粗层，组记录照写但 genCount=0（terminal group，
// zeux clusterlod 口径），error = max 成员簇误差（无简化项）。
void recordTerminalGroups(const NMeshData& current, const GroupingResult& g,
                          std::vector<NMeshLodMeta::DagGroup>& outGroups) {
    outGroups.assign(g.groupCount, NMeshLodMeta::DagGroup{});
    for (std::uint32_t grp = 0; grp < g.groupCount; ++grp) {
        NMeshLodMeta::DagGroup& dg = outGroups[grp];
        bool first = true;
        for (std::size_t c = 0; c < current.meshlets.size(); ++c) {
            if (g.clusterGroup[c] != grp) continue;
            dg.error = std::max(dg.error, current.bounds[c].error);
            if (first) {
                std::memcpy(dg.sphere, current.bounds[c].sphere, sizeof(dg.sphere));
                first = false;
            } else {
                float u[4];
                sphereUnion(dg.sphere, current.bounds[c].sphere, u);
                std::memcpy(dg.sphere, u, sizeof(u));
            }
        }
    }
}

} // namespace

bool buildLodHierarchy(const NMeshData& input, LodHierarchy& out,
                       std::string& outError, const LodOptions& options) {
    out = LodHierarchy{};
    if (input.meshlets.empty() || input.vertices.empty()) {
        outError = "lod: input mesh has no meshlets/vertices";
        return false;
    }
    if (!(options.ratio > 0.f) || options.ratio > 1.f) {
        outError = "lod: target ratio must be in (0,1]";
        return false;
    }
    if (options.maxLevels == 0) {
        outError = "lod: maxLevels must be >= 1";
        return false;
    }
    if (options.minGroups == 0) {
        outError = "lod: minGroups must be >= 1";
        return false;
    }

    NMeshData current = input;
    current.lodMeta = NMeshLodMeta{};
    current.lodMeta.present = true;        // 连 L0 也写 version 2（level=0）
    current.lodMeta.level = 0;
    current.lodMeta.maxError = 0.f;        // L0 相对原网格误差 0

    for (std::uint32_t level = 0; ; ++level) {
        // ---- 每轮重新分组；边界锁随之在 simplifyGroups 内按当前分组重算 ----
        GroupingResult g;
        if (!groupClusters(current, g, outError)) return false;

        LodLevel lv;
        lv.level = level;
        lv.mesh = current;
        lv.triangles = triangleCount(current);
        lv.clusters = std::uint32_t(current.meshlets.size());
        lv.groups = g.groupCount;
        lv.mesh.lodMeta.level = std::uint16_t(level);
        lv.maxError = current.lodMeta.maxError;   // parent error（单调不减）

        // 本层簇的组号（成员方向血缘）随层落盘
        lv.mesh.lodMeta.dagPresent = true;
        lv.mesh.lodMeta.clusterGroup = g.clusterGroup;
        out.levels.push_back(lv);

        bool converging = level >= options.maxLevels ||
                          lv.triangles < options.minTriangles ||
                          g.groupCount < options.minGroups;

        if (converging) {
            // 顶层/收敛层：组 genCount=0（terminal group）
            recordTerminalGroups(current, g, out.levels.back().mesh.lodMeta.dagGroups);
            break;
        }

        // ---- 每组硬目标 ratio；不传任何历史层锁点 ----
        SimplifyResult sr;
        if (!simplifyGroups(current, g, sr, outError, options.ratio, options.threads)) return false;
        if (sr.total.outputTriangles >= lv.triangles) {   // 无法再下降 = 自然收敛
            recordTerminalGroups(current, g, out.levels.back().mesh.lodMeta.dagGroups);
            break;
        }

        // ---- 按组重切（M3.0）：父簇绝不跨组，血缘随重切产生 ----
        NMeshData next;
        std::vector<GroupSplit> splits;
        if (!rebuildMeshletsPerGroup(current, g, sr, next, splits, outError,
                                     options.threads)) return false;

        {
            std::vector<NMeshLodMeta::DagGroup>& dgs =
                out.levels.back().mesh.lodMeta.dagGroups;
            dgs.resize(splits.size());
            for (std::size_t i = 0; i < splits.size(); ++i) {
                NMeshLodMeta::DagGroup& dg = dgs[i];
                dg.error = splits[i].error;
                dg.simplifyError = splits[i].simplifyError;
                std::memcpy(dg.sphere, splits[i].sphere, sizeof(dg.sphere));
                dg.genFirst = splits[i].firstCluster;
                dg.genCount = splits[i].clusterCount;
            }
        }

        next.name = (input.name.empty() ? std::string("lod") : input.name) +
                    ".lod" + std::to_string(level + 1);
        next.lodMeta.present = true;
        next.lodMeta.level = std::uint16_t(level + 1);
        // UE ClusterDAG.cpp:1441 / bevy from_mesh.rs:146-151：父层误差不小于子层。
        next.lodMeta.maxError = std::max(current.lodMeta.maxError, sr.total.maxError);

        // 本层对更粗层的接缝承诺 = 本次简化按当前分组锁住的边界点。
        LodLevel& cur = out.levels.back();
        cur.mesh.lodMeta.lockedPositions = sr.lockedPositions;
        cur.lockedPoints = std::uint32_t(sr.lockedPositions.size() / 3);
        cur.collapses = sr.total.collapses;
        cur.simplifyError = sr.total.maxError;

        // 简化不足的组不特殊处理：重切已经改变簇划分，下一轮 groupClusters
        // 会给出一组全新的边界（bevy :190-192 stuck 重新入队同义）。
        current = std::move(next);
    }
    return true;
}

} // namespace nl::core
