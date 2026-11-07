// M1.0：nanite-tools —— S1 起的离线网格处理工具。
// 子命令规划（S1-PLAN）：cluster（M1.1 切簇）/ simplify（S2 重切）/
// dag（S3 误差树）/ stat（M1.0 实现）/ selftest（M1.1 起的机械单测）。
// 只依赖 core（日志 + mesh_io + meshlet），零图形 API 符号——与 viewer 的纪律一致。

#include "nl/core/mesh_io.hpp"
#include "nl/core/meshlet.hpp"
#include "nl/core/meshlet_io.hpp"
#include "nl/core/grouping.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void printUsage() {
    std::printf(
        "nanite-tools - nl-engine offline mesh pipeline (S1)\n"
        "usage:\n"
        "  nanite-tools stat <model>                 summary (.glb/.gltf 或 .nmesh)\n"
        "  nanite-tools cluster <model> [-v] [-o out.nmesh]   meshlet build + save\n"
        "  nanite-tools selftest cluster|nmesh|group deterministic unit tests\n"
        "  nanite-tools simplify <model.nmesh> [-v]  cluster grouping (S2 M2.0)\n"
        "  nanite-tools dag <model> [options]        error DAG (S3, not yet)\n");
}

// stat 与 cluster 共用的簇统计（定义在后，前向声明供 cmdStat 用）
void printNMeshStats(const nl::core::NMeshData& nm);

// stat：<path> 的顶点 / 三角 / 子网格汇总 + 每子网格明细 + 世界空间包围盒。
// M1.2 起：也直接吃 .nmesh（簇统计 + 误差直方图）。
int cmdStat(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "stat: missing <model.gltf|.glb|.nmesh>\n");
        return 1;
    }
    const char* path = argv[0];

    // .nmesh 输入：簇视角的统计（M1.2）
    const std::string pathStr = path;
    if (pathStr.size() >= 6 && pathStr.compare(pathStr.size() - 6, 6, ".nmesh") == 0) {
        nl::core::NMeshData nm;
        std::string error;
        if (!nl::core::loadNMesh(path, nm, error)) {
            std::fprintf(stderr, "stat: %s\n", error.c_str());
            return 1;
        }
        std::printf("file      : %s\n", path);
        printNMeshStats(nm);
        return 0;
    }

    nl::core::SceneMeshData data;
    std::string error;
    if (!nl::core::loadGltfScene(path, data, error)) {
        std::fprintf(stderr, "stat: %s\n", error.c_str());
        return 1;
    }
    if (!error.empty()) std::fprintf(stderr, "stat: warning: %s\n", error.c_str());

    size_t totalVertices = 0, totalTriangles = 0;
    float bboxMin[3] = { 0, 0, 0 }, bboxMax[3] = { 0, 0, 0 };
    bool haveBbox = false;
    for (const auto& prim : data.meshes) {
        totalVertices += prim.vertices.size();
        totalTriangles += prim.indices.size() / 3;
        for (const auto& v : prim.vertices) {
            if (!haveBbox) {
                bboxMin[0] = bboxMax[0] = v.pos[0];
                bboxMin[1] = bboxMax[1] = v.pos[1];
                bboxMin[2] = bboxMax[2] = v.pos[2];
                haveBbox = true;
            }
            for (int k = 0; k < 3; ++k) {
                bboxMin[k] = v.pos[k] < bboxMin[k] ? v.pos[k] : bboxMin[k];
                bboxMax[k] = v.pos[k] > bboxMax[k] ? v.pos[k] : bboxMax[k];
            }
        }
    }

    std::printf("file      : %s\n", path);
    std::printf("submeshes : %zu\n", data.meshes.size());
    std::printf("vertices  : %zu\n", totalVertices);
    std::printf("triangles : %zu\n", totalTriangles);
    if (haveBbox) {
        std::printf("aabb      : min(%.4f, %.4f, %.4f) max(%.4f, %.4f, %.4f)\n",
                    bboxMin[0], bboxMin[1], bboxMin[2],
                    bboxMax[0], bboxMax[1], bboxMax[2]);
        std::printf("aabb size : %.4f x %.4f x %.4f\n",
                    bboxMax[0] - bboxMin[0], bboxMax[1] - bboxMin[1],
                    bboxMax[2] - bboxMin[2]);
    }
    for (size_t i = 0; i < data.meshes.size(); ++i) {
        const auto& prim = data.meshes[i];
        std::printf("  [%zu] %-24s vertices=%-7zu triangles=%-7zu tex=%s\n",
                    i, prim.name.c_str(), prim.vertices.size(),
                    prim.indices.size() / 3,
                    prim.hasBaseColor ? "yes" : "no");
    }
    return 0;
}

// ---- .nmesh 的簇统计（stat 与 cluster 共用）----

// 误差直方图：errMin..errMax 等宽 10 桶，# 数量按桶内占比归一化到 60 列
void printErrorHistogram(const nl::core::NMeshData& nm) {
    double mn = 0, mx = 0;
    bool first = true;
    for (const auto& b : nm.bounds) {
        if (first) { mn = mx = b.error; first = false; }
        mn = std::min(mn, (double)b.error);
        mx = std::max(mx, (double)b.error);
    }
    if (first) return;
    if (mx <= mn) mx = mn + 1e-6f;  // 单值退化：保一个桶
    constexpr int kBuckets = 10;
    size_t count[kBuckets] = {};
    for (const auto& b : nm.bounds) {
        int idx = int((b.error - mn) / (mx - mn) * kBuckets);
        idx = std::clamp(idx, 0, kBuckets - 1);
        ++count[idx];
    }
    const size_t maxCount = *std::max_element(count, count + kBuckets);
    std::printf("error histogram (world space, max edge):\n");
    for (int i = 0; i < kBuckets; ++i) {
        const double lo = mn + (mx - mn) * i / kBuckets;
        const double hi = mn + (mx - mn) * (i + 1) / kBuckets;
        const int bars = maxCount > 0 ? int(count[i] * 60 / maxCount) : 0;
        std::printf("  [%.4f,%.4f) %-6zu ", lo, hi, count[i]);
        for (int j = 0; j < bars; ++j) std::printf("#");
        std::printf("\n");
    }
}

void printNMeshStats(const nl::core::NMeshData& nm) {
    size_t maxTri = 0, maxVert = 0, totalTri = 0;
    double sumTri = 0, sumVert = 0;
    double errSum = 0, errMin = 0, errMax = 0;
    double cutoffSum = 0, cutoffMin = 1.0;
    size_t degenerate = 0;
    for (const auto& m : nm.meshlets) {
        maxTri = std::max<size_t>(maxTri, m.triangleCount);
        maxVert = std::max<size_t>(maxVert, m.vertexCount);
        totalTri += m.triangleCount;
        sumTri += m.triangleCount;
        sumVert += m.vertexCount;
    }
    bool first = true;
    for (const auto& b : nm.bounds) {
        if (first) { errMin = b.error; first = false; }
        errMin = std::min(errMin, (double)b.error);
        errMax = std::max(errMax, (double)b.error);
        errSum += b.error;
        if (b.coneCutoff < 0.f) ++degenerate;
        else { cutoffSum += b.coneCutoff; cutoffMin = std::min(cutoffMin, (double)b.coneCutoff); }
    }
    const size_t n = nm.meshlets.size();
    std::printf("name       : %s\n", nm.name.c_str());
    std::printf("meshlets   : %zu\n", n);
    std::printf("vertices   : %zu\n", nm.vertices.size());
    std::printf("triangles  : %zu\n", totalTri);
    std::printf("tri/cluster: avg %.1f  max %zu  (limit %u)\n",
                n ? sumTri / n : 0.0, maxTri, nl::core::kMeshletMaxTriangles);
    std::printf("vert/cluster: avg %.1f  max %zu  (limit %u)\n",
                n ? sumVert / n : 0.0, maxVert, nl::core::kMeshletMaxVertices);
    if (n) {
        std::printf("error      : min %.4f  avg %.4f  max %.4f\n",
                    errMin, errSum / n, errMax);
        std::printf("cone       : avg cutoff %.3f  min %.3f  (degenerate: %zu)\n",
                    n - degenerate ? cutoffSum / (n - degenerate) : 0.0,
                    degenerate == n ? 0.0 : cutoffMin, degenerate);
    }
    printErrorHistogram(nm);
}

// ---- cluster：对每个子网格贪心切簇，打印簇统计（M1.1 的交代物）----

struct ClusterSummary {
    size_t meshlets = 0;
    size_t tris = 0;
    size_t verts = 0;          // 全局顶点数（输入口径）
    size_t emittedTris = 0;    // 簇展开后的三角形数（覆盖率核对）
    uint32_t maxTri = 0, maxVert = 0;
    double avgTri = 0, avgVert = 0;
    double errMin = 0, errAvg = 0, errMax = 0;
    double cutoffAvg = 0, cutoffMin = 1.0;
    size_t degenerateCones = 0;  // cutoff < 0 的簇数（法线杂乱）
};

// 对单个 primitive 切簇并核对覆盖率 / 上限。失败返回 false（failReason 带原因）。
bool clusterPrimitive(const nl::core::PrimitiveMesh& prim,
                      nl::core::BuiltMeshlets& built, std::string& failReason) {
    if (!nl::core::buildMeshlets(&prim.vertices[0].pos[0],
                                 sizeof(nl::core::MeshVertex) / sizeof(float),
                                 prim.vertices.size(), prim.indices.data(),
                                 prim.indices.size(), built)) {
        failReason = "buildMeshlets rejected input";
        return false;
    }
    return true;
}

// 覆盖率核对：所有簇展开后的全局索引三元组多重集 == 输入索引三元组多重集。
bool verifyCoverage(const nl::core::PrimitiveMesh& prim,
                    const nl::core::BuiltMeshlets& built, std::string& failReason) {
    std::vector<std::uint32_t> expanded;
    expanded.reserve(prim.indices.size());
    for (const auto& m : built.meshlets) {
        for (std::uint32_t t = 0; t < m.triangleCount; ++t) {
            const std::size_t base = m.triangleOffset + std::size_t(t) * 3;
            for (int k = 0; k < 3; ++k)
                expanded.push_back(built.vertices[built.triangles[base + k] +
                                                  m.vertexOffset]);
        }
    }
    std::vector<std::uint32_t> origin = prim.indices;
    std::sort(expanded.begin(), expanded.end());
    std::sort(origin.begin(), origin.end());
    if (expanded != origin) {
        failReason = "coverage mismatch: cluster expansion != input indices";
        return false;
    }
    return true;
}

bool printClusterResult(const char* path, const nl::core::PrimitiveMesh& prim,
                        nl::core::BuiltMeshlets& built, bool verbose) {
    std::string reason;
    if (!verifyCoverage(prim, built, reason)) {
        std::fprintf(stderr, "cluster: %s\n", reason.c_str());
        return false;
    }

    ClusterSummary s;
    s.tris = prim.indices.size() / 3;
    s.verts = prim.vertices.size();
    s.meshlets = built.meshlets.size();
    double errSum = 0, cutoffSum = 0;
    for (const auto& m : built.meshlets) {
        s.maxTri = std::max(s.maxTri, m.triangleCount);
        s.maxVert = std::max(s.maxVert, m.vertexCount);
        s.emittedTris += m.triangleCount;
        const auto& bd = built.bounds[&m - built.meshlets.data()];
        errSum += bd.error;
        s.errMin = s.errMin == 0 ? bd.error : std::min(s.errMin, (double)bd.error);
        s.errMax = std::max(s.errMax, (double)bd.error);
        if (bd.coneCutoff < 0.f) ++s.degenerateCones;
        else { cutoffSum += bd.coneCutoff; s.cutoffMin = std::min(s.cutoffMin, (double)bd.coneCutoff); }
    }
    s.avgTri = double(s.emittedTris) / double(s.meshlets);
    s.avgVert = double(built.vertices.size()) / double(s.meshlets);
    s.errAvg = errSum / double(s.meshlets);
    s.cutoffAvg = cutoffSum / double(s.meshlets - s.degenerateCones);

    std::printf("file       : %s\n", path);
    std::printf("submesh    : %s\n", prim.name.c_str());
    std::printf("input      : %zu verts / %zu tris\n", s.verts, s.tris);
    std::printf("meshlets   : %zu\n", s.meshlets);
    std::printf("tri/cluster: avg %.1f  max %u  (limit %u)\n",
                s.avgTri, s.maxTri, nl::core::kMeshletMaxTriangles);
    std::printf("vert/cluster: avg %.1f  max %u  (limit %u)\n",
                s.avgVert, s.maxVert, nl::core::kMeshletMaxVertices);
    std::printf("coverage   : %zu/%zu = %.2f%%\n", s.emittedTris, s.tris,
                100.0 * double(s.emittedTris) / double(s.tris));
    std::printf("error      : min %.4f  avg %.4f  max %.4f  (world space, max edge)\n",
                s.errMin, s.errAvg, s.errMax);
    std::printf("cone       : avg cutoff %.3f  min %.3f  (degenerate: %zu)\n",
                s.cutoffAvg, s.cutoffMin, s.degenerateCones);
    const bool limitsOk = s.maxTri <= nl::core::kMeshletMaxTriangles &&
                          s.maxVert <= nl::core::kMeshletMaxVertices;
    std::printf("limits     : %s\n", limitsOk ? "PASS" : "FAIL");
    if (verbose) {
        const size_t show = std::min<size_t>(built.meshlets.size(), 5);
        std::printf("first %zu meshlets:\n", show);
        for (size_t i = 0; i < show; ++i) {
            const auto& m = built.meshlets[i];
            const auto& bd = built.bounds[i];
            std::printf("  [%zu] tri=%-3u vert=%-3u sphere r=%.4f cutoff=%.3f err=%.4f\n",
                        i, m.triangleCount, m.vertexCount, bd.sphere[3],
                        bd.coneCutoff, bd.error);
        }
    }
    return limitsOk;
}

// BuiltMeshlets（引用原网格）→ NMeshData（自包含）：顶点窗口按序 remap 成真实顶点，
// 局部索引不变（窗口序不变，只是数组内容从全局索引变成顶点数据）。
bool toNMeshData(const nl::core::PrimitiveMesh& prim,
                 const nl::core::BuiltMeshlets& built,
                 nl::core::NMeshData& out, std::string& failReason) {
    out = nl::core::NMeshData{};
    out.name = prim.name;
    out.meshlets = built.meshlets;
    out.bounds = built.bounds;
    out.vertices.resize(built.vertices.size());
    for (size_t i = 0; i < built.vertices.size(); ++i)
        out.vertices[i] = prim.vertices[built.vertices[i]];
    out.triangles = built.triangles;
    // 窗口合法性自检（remap 不改索引，这里防上游数据坏了难排查）
    for (const auto& m : out.meshlets) {
        if (size_t(m.vertexOffset) + m.vertexCount > out.vertices.size() ||
            size_t(m.triangleOffset) + size_t(m.triangleCount) * 3 > out.triangles.size()) {
            failReason = "built meshlet window out of range";
            return false;
        }
    }
    return true;
}

int cmdCluster(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "cluster: missing <model.gltf|.glb>\n");
        return 1;
    }
    const char* path = argv[0];
    const char* outPath = nullptr;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v") verbose = true;
        else if (a == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cluster: -o needs a path\n");
                return 1;
            }
            outPath = argv[++i];
        } else {
            std::fprintf(stderr, "cluster: unknown option %s\n", argv[i]);
            return 1;
        }
    }

    nl::core::SceneMeshData data;
    std::string error;
    if (!nl::core::loadGltfScene(path, data, error)) {
        std::fprintf(stderr, "cluster: %s\n", error.c_str());
        return 1;
    }
    if (data.meshes.empty()) {
        std::fprintf(stderr, "cluster: %s has no mesh primitive\n", path);
        return 1;
    }

    int failures = 0;
    if (outPath && data.meshes.size() != 1) {
        // M1.2 只支持单子网格落盘（当前资产都是单子网格）；多子网格等真出现了再定容器格式
        std::fprintf(stderr, "cluster: -o needs exactly one submesh (got %zu)\n",
                     data.meshes.size());
        return 1;
    }
    for (size_t i = 0; i < data.meshes.size(); ++i) {
        nl::core::BuiltMeshlets built;
        std::string reason;
        if (!clusterPrimitive(data.meshes[i], built, reason)) {
            std::fprintf(stderr, "cluster: [%zu] %s\n", i, reason.c_str());
            ++failures;
            continue;
        }
        if (!printClusterResult(path, data.meshes[i], built, verbose)) ++failures;
        if (outPath) {
            nl::core::NMeshData nm;
            if (!toNMeshData(data.meshes[i], built, nm, reason)) {
                std::fprintf(stderr, "cluster: [%zu] %s\n", i, reason.c_str());
                ++failures;
                continue;
            }
            if (!nl::core::saveNMesh(outPath, nm, reason)) {
                std::fprintf(stderr, "cluster: save %s: %s\n", outPath, reason.c_str());
                ++failures;
                continue;
            }
            // 立即读回自检：写出的文件必须能被同版本工具读回且窗口合法
            nl::core::NMeshData back;
            if (!nl::core::loadNMesh(outPath, back, reason)) {
                std::fprintf(stderr, "cluster: reload %s: %s\n", outPath, reason.c_str());
                ++failures;
                continue;
            }
            std::printf("saved      : %s (reload ok)\n", outPath);
        }
        if (i + 1 < data.meshes.size()) std::printf("\n");
    }
    return failures == 0 ? 0 : 1;
}

// ---- simplify（M2.0）：簇聚组 ----
// S2 第一步：读 .nmesh，把相邻簇（共享几何点）用 METIS 聚成组并打印统计。
// 组内 QEM 简化 + 重切在 M2.1/M2.2 落地；本步先验证「组连通 + 组大小合理」。

// 独立重算组内连通性（与 grouping.cpp 的 fixup 交叉验证）：对每组沿
// 共享几何点边 BFS，能访问全组员才算连通。返回不连通的组数。
int countDisconnectedGroups(const nl::core::NMeshData& nm,
                            const nl::core::GroupingResult& g) {
    if (nm.meshlets.empty()) return 0;
    const size_t n = nm.meshlets.size();
    // canonical 点 → 簇倒排（与 grouping.cpp 同口径：位级相等的位置）
    std::vector<std::uint32_t> canon(nm.vertices.size());
    {
        std::vector<std::uint32_t> order(nm.vertices.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = std::uint32_t(i);
        std::sort(order.begin(), order.end(), [&nm](std::uint32_t a, std::uint32_t b) {
            for (int k = 0; k < 3; ++k)
                if (nm.vertices[a].pos[k] != nm.vertices[b].pos[k])
                    return nm.vertices[a].pos[k] < nm.vertices[b].pos[k];
            return a < b;
        });
        std::uint32_t id = 0;
        canon[order[0]] = 0;
        for (size_t i = 1; i < order.size(); ++i) {
            const auto& a = nm.vertices[order[i - 1]].pos;
            const auto& b = nm.vertices[order[i]].pos;
            if (b[0] != a[0] || b[1] != a[1] || b[2] != a[2]) ++id;
            canon[order[i]] = id;
        }
    }
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> pointToClusters;
    for (std::uint32_t c = 0; c < n; ++c)
        for (std::uint32_t i = 0; i < nm.meshlets[c].vertexCount; ++i)
            pointToClusters[canon[nm.meshlets[c].vertexOffset + i]].push_back(c);
    std::vector<std::vector<std::uint32_t>> adj(n);
    for (const auto& [pt, cs] : pointToClusters)
        for (size_t i = 0; i + 1 < cs.size(); ++i)
            for (size_t j = i + 1; j < cs.size(); ++j) {
                adj[cs[i]].push_back(cs[j]);
                adj[cs[j]].push_back(cs[i]);
            }

    int bad = 0;
    std::vector<std::uint8_t> seen(n, 0);
    std::vector<std::uint32_t> stack;
    for (std::uint32_t grp = 0; grp < g.groupCount; ++grp) {
        std::uint32_t memberCount = 0, visited = 0;
        std::uint32_t root = 0;
        bool haveRoot = false;
        for (std::uint32_t c = 0; c < n; ++c) {
            if (g.clusterGroup[c] != grp) continue;
            ++memberCount;
            if (!haveRoot) { root = c; haveRoot = true; }
        }
        if (memberCount == 0) continue;
        stack = {root};
        std::fill(seen.begin(), seen.end(), std::uint8_t(0));
        seen[root] = 1;
        while (!stack.empty()) {
            const std::uint32_t c = stack.back();
            stack.pop_back();
            ++visited;
            for (std::uint32_t nb : adj[c])
                if (!seen[nb] && g.clusterGroup[nb] == grp) { seen[nb] = 1; stack.push_back(nb); }
        }
        if (visited != memberCount) ++bad;
    }
    return bad;
}

int cmdSimplify(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "simplify: missing <model.nmesh>\n");
        return 1;
    }
    const char* path = argv[0];
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.empty()) continue;  // 展示台 select 空选项展开成空 arg，视为未传
        if (a == "-v") verbose = true;
        else {
            std::fprintf(stderr, "simplify: unknown option %s\n", argv[i]);
            return 1;
        }
    }

    nl::core::NMeshData nm;
    std::string error;
    if (!nl::core::loadNMesh(path, nm, error)) {
        std::fprintf(stderr, "simplify: %s\n", error.c_str());
        return 1;
    }

    nl::core::GroupingResult g;
    if (!nl::core::groupClusters(nm, g, error)) {
        std::fprintf(stderr, "simplify: %s\n", error.c_str());
        return 1;
    }

    // 组大小统计（簇数/组 + 三角数/组）
    size_t totalTri = 0;
    for (const auto& m : nm.meshlets) totalTri += m.triangleCount;
    std::vector<std::uint32_t> sizeOf(g.groupCount, 0), triOf(g.groupCount, 0);
    for (std::uint32_t c = 0; c < nm.meshlets.size(); ++c) {
        ++sizeOf[g.clusterGroup[c]];
        triOf[g.clusterGroup[c]] += nm.meshlets[c].triangleCount;
    }
    std::uint32_t sizeMin = ~0u, sizeMax = 0;
    double sizeSum = 0;
    for (std::uint32_t s : sizeOf) {
        sizeMin = std::min(sizeMin, s);
        sizeMax = std::max(sizeMax, s);
        sizeSum += s;
    }
    const double sizeAvg = g.groupCount ? sizeSum / g.groupCount : 0.0;

    std::printf("file        : %s\n", path);
    std::printf("clusters    : %zu (%zu triangles)\n", nm.meshlets.size(), totalTri);
    std::printf("target      : %u meshlets/group\n", nl::core::kTargetMeshletsPerGroup);
    std::printf("groups      : %u\n", g.groupCount);
    std::printf("group size  : min %u avg %.1f max %u\n",
                g.groupCount ? sizeMin : 0u, sizeAvg, sizeMax);
    std::printf("split fixups: %u\n", g.splitFixups);

    const int bad = countDisconnectedGroups(nm, g);
    std::printf("connectivity: %s (%d disconnected group(s))\n",
                bad == 0 ? "PASS" : "FAIL", bad);
    if (verbose) {
        std::printf("per-group   :\n");
        for (std::uint32_t grp = 0; grp < g.groupCount; ++grp)
            std::printf("  [%3u] clusters=%-4u triangles=%-6u\n",
                        grp, sizeOf[grp], triOf[grp]);
    }
    return bad == 0 ? 0 : 1;
}

// ---- selftest cluster：确定性合成网格上的机械单测 ----

// xorshift32：固定种子，跨平台确定性
std::uint32_t xorshift32(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

struct SelftestMesh {
    std::vector<float> positions;        // float[3] 紧凑
    std::vector<std::uint32_t> indices;
    size_t gridW = 0, gridH = 0;         // 顶点网格宽高
    size_t blocks = 0;                   // 独立连通块数（1 = 连续，2 = 断连）
};

// 生成 blocks 个 H×W 网格拼成的地形块（块间无共享顶点 → 断连），
// 高度 = 确定性伪随机噪声，让三角形面积/朝向不完全一致。
SelftestMesh makeTestMesh(size_t blocks, size_t w, size_t h) {
    SelftestMesh m;
    m.blocks = blocks;
    m.gridW = w;
    m.gridH = h;
    std::uint32_t rng = 0x9e3779b9u;
    const size_t vertsPerBlock = w * h;
    const size_t quadsPerBlock = (w - 1) * (h - 1);
    m.positions.reserve(blocks * vertsPerBlock * 3);
    m.indices.reserve(blocks * quadsPerBlock * 6);
    for (size_t blk = 0; blk < blocks; ++blk) {
        const size_t base = blk * vertsPerBlock;
        const float ox = float(blk) * 100.f;  // 块间平移，断开连通性
        for (size_t y = 0; y < h; ++y) {
            for (size_t x = 0; x < w; ++x) {
                const float fx = float(x) / float(w - 1);
                const float fy = float(y) / float(h - 1);
                const float noise = float(xorshift32(rng) % 1000) / 1000.f;
                m.positions.push_back(fx * 10.f + ox);
                m.positions.push_back(fy * 10.f);
                m.positions.push_back(noise * 2.f);
            }
        }
        for (size_t y = 0; y + 1 < h; ++y) {
            for (size_t x = 0; x + 1 < w; ++x) {
                const std::uint32_t v00 = std::uint32_t(base + y * w + x);
                const std::uint32_t v10 = v00 + 1;
                const std::uint32_t v01 = v00 + std::uint32_t(w);
                const std::uint32_t v11 = v01 + 1;
                m.indices.insert(m.indices.end(), { v00, v01, v11, v00, v11, v10 });
            }
        }
    }
    return m;
}

int selftestCluster() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name, const std::string& detail = {}) {
        if (ok) std::printf("  ok   %s\n", name);
        else { std::printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str()); ++failures; }
    };

    struct Case { const char* name; SelftestMesh mesh; };
    std::vector<Case> cases = {
        { "connected 64x64 grid (7938 tris)", makeTestMesh(1, 64, 64) },
        { "2 disconnected 32x32 blocks", makeTestMesh(2, 32, 32) },
        { "single quad", makeTestMesh(1, 2, 2) },
    };

    for (const Case& c : cases) {
        std::printf("[%s]\n", c.name);
        nl::core::BuiltMeshlets built;
        if (!nl::core::buildMeshlets(c.mesh.positions.data(), 3,
                                     c.mesh.positions.size() / 3,
                                     c.mesh.indices.data(),
                                     c.mesh.indices.size(), built)) {
            check(false, "buildMeshlets", "rejected valid input");
            continue;
        }

        // 1. 覆盖率：簇展开索引多重集 == 输入（复用 cluster 的核对逻辑）
        nl::core::PrimitiveMesh pseudo;
        pseudo.indices = c.mesh.indices;
        pseudo.vertices.resize(c.mesh.positions.size() / 3);
        for (size_t i = 0; i < pseudo.vertices.size(); ++i) {
            for (int k = 0; k < 3; ++k)
                pseudo.vertices[i].pos[k] = c.mesh.positions[i * 3 + k];
        }
        std::string reason;
        check(verifyCoverage(pseudo, built, reason), "coverage 100%", reason);
        if (!reason.empty()) {
            // 诊断输出：前两个簇的窗口与展开
            std::printf("  diag: meshlets=%zu\n", built.meshlets.size());
            for (size_t i = 0; i < std::min<size_t>(built.meshlets.size(), 2); ++i) {
                const auto& m = built.meshlets[i];
                std::printf("  diag[%zu]: vo=%u vc=%u to=%u tc=%u\n", i,
                            m.vertexOffset, m.vertexCount, m.triangleOffset, m.triangleCount);
                for (std::uint32_t t = 0; t < m.triangleCount && t < 3; ++t) {
                    const std::size_t base = m.triangleOffset + std::size_t(t) * 3;
                    std::printf("    t%u locals=%u,%u,%u globals=%u,%u,%u\n", t,
                                built.triangles[base], built.triangles[base + 1],
                                built.triangles[base + 2],
                                built.vertices[built.triangles[base] + m.vertexOffset],
                                built.vertices[built.triangles[base + 1] + m.vertexOffset],
                                built.vertices[built.triangles[base + 2] + m.vertexOffset]);
                }
            }
        }

        // 2. 上限：每簇 tri <= 128 且去重 vert <= 64
        bool limitsOk = true;
        size_t worstTri = 0, worstVert = 0;
        std::vector<std::uint32_t> seen;
        for (const auto& m : built.meshlets) {
            worstTri = std::max<size_t>(worstTri, m.triangleCount);
            seen.assign(m.vertexCount, 0);
            std::vector<std::uint32_t> uniq;
            for (std::uint32_t k = 0; k < m.vertexCount; ++k)
                uniq.push_back(built.vertices[m.vertexOffset + k]);
            std::sort(uniq.begin(), uniq.end());
            uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
            worstVert = std::max<size_t>(worstVert, uniq.size());
            if (m.triangleCount > nl::core::kMeshletMaxTriangles ||
                m.vertexCount > nl::core::kMeshletMaxVertices || uniq.size() != m.vertexCount)
                limitsOk = false;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "max tri %zu/128, max vert %zu/64",
                      worstTri, worstVert);
        check(limitsOk, "cluster limits", buf);

        // 3. bounds 合理性：球有限正半径、cutoff 在 [-1, 1]、error 有限正、
        //    包围球确实包住簇内全部顶点（1e-3 容差）
        bool boundsOk = true;
        std::string boundsDetail;
        for (const auto& m : built.meshlets) {
            const auto& bd = built.bounds[&m - built.meshlets.data()];
            const float r = bd.sphere[3];
            if (!(r > 0.f) || !std::isfinite(r) || bd.coneCutoff < -1.001f ||
                bd.coneCutoff > 1.001f || !(bd.error > 0.f) || !std::isfinite(bd.error)) {
                boundsOk = false;
                boundsDetail = "bad sphere/cutoff/error values";
                break;
            }
            for (std::uint32_t k = 0; k < m.vertexCount; ++k) {
                const float* p = &c.mesh.positions[
                    std::size_t(built.vertices[m.vertexOffset + k]) * 3];
                const float dx = p[0] - bd.sphere[0];
                const float dy = p[1] - bd.sphere[1];
                const float dz = p[2] - bd.sphere[2];
                if (std::sqrt(dx * dx + dy * dy + dz * dz) > r + 1e-3f) {
                    boundsOk = false;
                    boundsDetail = "sphere does not contain a cluster vertex";
                    break;
                }
            }
            if (!boundsOk) break;
        }
        check(boundsOk, "bounds sane", boundsDetail);
    }

    // 4. 负向：非法参数必须被拒（不崩溃、不产出脏数据）
    std::printf("[input validation]\n");
    nl::core::BuiltMeshlets built;
    const std::uint32_t idx[3] = { 0, 1, 999 };  // 越界索引
    const float pos[4 * 3] = {};
    bool rejected = !nl::core::buildMeshlets(pos, 3, 4, idx, 3, built);
    check(rejected, "out-of-range index rejected");
    rejected = !nl::core::buildMeshlets(pos, 3, 4, idx, 4, built);  // indexCount 非三倍数
    check(rejected, "index count % 3 != 0 rejected");

    if (failures == 0) {
        std::printf("cluster selftest: PASS\n");
        return 0;
    }
    std::printf("cluster selftest: FAIL (%d case(s))\n", failures);
    return 1;
}

// ---- selftest nmesh：写盘 → 读回 roundtrip（M1.2 验收核心）----

int selftestNMesh() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name, const std::string& detail = {}) {
        if (ok) std::printf("  ok   %s\n", name);
        else { std::printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str()); ++failures; }
    };

    const char* tmpA = "_nmesh_selftest_a.nmesh";
    const char* tmpB = "_nmesh_selftest_b.nmesh";

    // 本地字节读取（core 的 readFileAll 是匿名命名空间，不导出）
    auto readAll = [](const char* p, std::vector<std::uint8_t>& out) {
        std::FILE* fp = std::fopen(p, "rb");
        if (!fp) return false;
        std::fseek(fp, 0, SEEK_END);
        const long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        out.resize(sz > 0 ? size_t(sz) : 0);
        const bool ok = sz <= 0 || std::fread(out.data(), 1, size_t(sz), fp) == size_t(sz);
        std::fclose(fp);
        return ok;
    };

    // 正向：确定性网格 → 切簇 → 写盘 → 读回
    nl::core::NMeshData written;
    {
        SelftestMesh mesh = makeTestMesh(2, 40, 40);  // 断连块，覆盖兑底种子路径
        nl::core::BuiltMeshlets built;
        if (!nl::core::buildMeshlets(mesh.positions.data(), 3,
                                     mesh.positions.size() / 3,
                                     mesh.indices.data(), mesh.indices.size(), built)) {
            std::printf("  FAIL buildMeshlets (precondition)\n");
            return 1;
        }
        nl::core::PrimitiveMesh prim;
        prim.name = "selftest#0";
        prim.indices = mesh.indices;
        prim.vertices.resize(mesh.positions.size() / 3);
        for (size_t i = 0; i < prim.vertices.size(); ++i)
            for (int k = 0; k < 3; ++k) prim.vertices[i].pos[k] = mesh.positions[i * 3 + k];
        std::string reason;
        if (!toNMeshData(prim, built, written, reason)) {
            std::printf("  FAIL toNMeshData: %s\n", reason.c_str());
            return 1;
        }
        if (!nl::core::saveNMesh(tmpA, written, reason)) {
            std::printf("  FAIL saveNMesh: %s\n", reason.c_str());
            return 1;
        }
    }

    nl::core::NMeshData back;
    std::string reason;
    check(nl::core::loadNMesh(tmpA, back, reason), "loadNMesh", reason);

    bool fieldsOk = back.name == written.name &&
                    back.meshlets.size() == written.meshlets.size() &&
                    back.vertices.size() == written.vertices.size() &&
                    back.triangles.size() == written.triangles.size();
    for (size_t i = 0; fieldsOk && i < written.meshlets.size(); ++i) {
        const auto& a = written.meshlets[i];
        const auto& b = back.meshlets[i];
        const auto& ba = written.bounds[i];
        const auto& bb = back.bounds[i];
        fieldsOk = std::memcmp(&a, &b, sizeof(a)) == 0 &&
                   std::memcmp(&ba, &bb, sizeof(ba)) == 0;
    }
    for (size_t i = 0; fieldsOk && i < written.vertices.size(); ++i)
        fieldsOk = std::memcmp(&written.vertices[i], &back.vertices[i],
                               sizeof(nl::core::MeshVertex)) == 0;
    for (size_t i = 0; fieldsOk && i < written.triangles.size(); ++i)
        fieldsOk = written.triangles[i] == back.triangles[i];
    check(fieldsOk, "fields identical after roundtrip");

    // 字节级 roundtrip：读回再重写，两文件逐字节一致
    if (nl::core::saveNMesh(tmpB, back, reason)) {
        std::vector<std::uint8_t> rawA, rawB;
        bool bytesOk = nl::core::loadNMesh(tmpA, back, reason) &&
                       readAll(tmpA, rawA) && readAll(tmpB, rawB) && rawA == rawB;
        check(bytesOk, "byte-identical roundtrip");
    } else {
        check(false, "byte-identical roundtrip", "resave failed");
    }

    // 负向：坏 magic / 截断
    {
        std::vector<std::uint8_t> raw;
        readAll(tmpA, raw);
        raw[0] = 'X';  // 破坏 magic
        std::FILE* fp = std::fopen(tmpB, "wb");
        std::fwrite(raw.data(), 1, raw.size(), fp);
        std::fclose(fp);
        nl::core::NMeshData nm;
        check(!nl::core::loadNMesh(tmpB, nm, reason), "bad magic rejected", reason);
        std::vector<std::uint8_t> truncated(raw.begin(), raw.begin() + raw.size() / 2);
        // 恢复 magic 再截断，命中 need() 分支
        truncated[0] = 'N';
        fp = std::fopen(tmpB, "wb");
        std::fwrite(truncated.data(), 1, truncated.size(), fp);
        std::fclose(fp);
        check(!nl::core::loadNMesh(tmpB, nm, reason), "truncated file rejected", reason);
    }

    std::remove(tmpA);
    std::remove(tmpB);
    if (failures == 0) {
        std::printf("nmesh selftest: PASS\n");
        return 0;
    }
    std::printf("nmesh selftest: FAIL (%d case(s))\n", failures);
    return 1;
}

// ---- selftest group：聚组机械单测（M2.0 验收核心）----
// 与 grouping.cpp 内部逻辑交叉验证：连通性判定在 selftest 侧独立重算，
// 不复用 countDisconnectedGroups 的对外口径（同一算法两份实现，防止同错）。

// 合成网格 → 切簇 → NMeshData（selftest 复用的装配线）
static bool makeNMesh(const SelftestMesh& mesh, nl::core::NMeshData& out, std::string& reason) {
    nl::core::BuiltMeshlets built;
    if (!nl::core::buildMeshlets(mesh.positions.data(), 3,
                                 mesh.positions.size() / 3,
                                 mesh.indices.data(), mesh.indices.size(), built)) {
        reason = "buildMeshlets rejected valid input";
        return false;
    }
    nl::core::PrimitiveMesh prim;
    prim.name = "selftest#0";
    prim.indices = mesh.indices;
    prim.vertices.resize(mesh.positions.size() / 3);
    for (size_t i = 0; i < prim.vertices.size(); ++i)
        for (int k = 0; k < 3; ++k) prim.vertices[i].pos[k] = mesh.positions[i * 3 + k];
    return toNMeshData(prim, built, out, reason);
}

int selftestGroup() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name, const std::string& detail = {}) {
        if (ok) std::printf("  ok   %s\n", name);
        else { std::printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str()); ++failures; }
    };

    struct Case { const char* name; SelftestMesh mesh; };
    std::vector<Case> cases = {
        { "single quad (2 tris)", makeTestMesh(1, 2, 2) },
        { "2 disconnected 32x32 blocks", makeTestMesh(2, 32, 32) },
        { "connected 64x64 grid", makeTestMesh(1, 64, 64) },
    };

    for (const Case& c : cases) {
        std::printf("[%s]\n", c.name);
        nl::core::NMeshData nm;
        std::string reason;
        if (!makeNMesh(c.mesh, nm, reason)) {
            check(false, "makeNMesh", reason);
            continue;
        }
        nl::core::GroupingResult g;
        if (!nl::core::groupClusters(nm, g, reason)) {
            check(false, "groupClusters", reason);
            continue;
        }

        // 1) 簇全覆盖：每个簇有组号，组号连续 0..groupCount-1 且每号出现 ≥1 次
        bool covered = g.clusterGroup.size() == nm.meshlets.size();
        std::vector<std::uint32_t> seen(g.groupCount, 0);
        for (std::uint32_t grp : g.clusterGroup)
            if (grp < g.groupCount) ++seen[grp];
            else covered = false;
        for (std::uint32_t grp : seen)
            if (grp == 0) covered = false;
        check(covered, "coverage (every cluster grouped, ids contiguous)");

        // 2) 组大小合理：单四边形恰好 1 组；其余 ≤ 3×目标（防组堆积）
        if (std::string(c.name) == "single quad (2 tris)") {
            check(g.groupCount == 1, "single quad -> exactly 1 group");
        } else {
            check(g.maxGroupSize <= 3 * nl::core::kTargetMeshletsPerGroup,
                  "max group size sane",
                  "max=" + std::to_string(g.maxGroupSize));
        }

        // 3) 组内连通（独立重算，交叉验证 fixup）
        {
            // canonical 点 → 簇倒排 → 邻接
            const size_t n = nm.meshlets.size();
            std::vector<std::uint32_t> canon(nm.vertices.size());
            std::vector<std::uint32_t> order(nm.vertices.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = std::uint32_t(i);
            std::sort(order.begin(), order.end(), [&nm](std::uint32_t a, std::uint32_t b) {
                for (int k = 0; k < 3; ++k)
                    if (nm.vertices[a].pos[k] != nm.vertices[b].pos[k])
                        return nm.vertices[a].pos[k] < nm.vertices[b].pos[k];
                return a < b;
            });
            std::uint32_t id = 0;
            canon[order[0]] = 0;
            for (size_t i = 1; i < order.size(); ++i) {
                const auto& p = nm.vertices[order[i - 1]].pos;
                const auto& q = nm.vertices[order[i]].pos;
                if (q[0] != p[0] || q[1] != p[1] || q[2] != p[2]) ++id;
                canon[order[i]] = id;
            }
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> pt2c;
            for (std::uint32_t cl = 0; cl < n; ++cl)
                for (std::uint32_t i = 0; i < nm.meshlets[cl].vertexCount; ++i)
                    pt2c[canon[nm.meshlets[cl].vertexOffset + i]].push_back(cl);
            std::vector<std::vector<std::uint32_t>> adj(n);
            for (const auto& [pt, cs] : pt2c)
                for (size_t i = 0; i + 1 < cs.size(); ++i)
                    for (size_t j = i + 1; j < cs.size(); ++j) {
                        adj[cs[i]].push_back(cs[j]);
                        adj[cs[j]].push_back(cs[i]);
                    }
            bool allConnected = true;
            std::vector<std::uint8_t> seenB(n, 0);
            std::vector<std::uint32_t> stack;
            for (std::uint32_t grp = 0; grp < g.groupCount && allConnected; ++grp) {
                std::uint32_t memberCount = 0, root = 0;
                bool haveRoot = false;
                for (std::uint32_t cl = 0; cl < n; ++cl)
                    if (g.clusterGroup[cl] == grp) {
                        ++memberCount;
                        if (!haveRoot) { root = cl; haveRoot = true; }
                    }
                std::fill(seenB.begin(), seenB.end(), std::uint8_t(0));
                stack = {root};
                seenB[root] = 1;
                std::uint32_t visited = 0;
                while (!stack.empty()) {
                    const std::uint32_t cl = stack.back();
                    stack.pop_back();
                    ++visited;
                    for (std::uint32_t nb : adj[cl])
                        if (!seenB[nb] && g.clusterGroup[nb] == grp) { seenB[nb] = 1; stack.push_back(nb); }
                }
                if (visited != memberCount) allConnected = false;
            }
            check(allConnected, "every group connected on shared-vertex graph");
        }

        // 4) 断连块 case：任何组不得跨块（块间 x 平移 100、每块宽 10）
        if (c.mesh.blocks == 2) {
            bool crossBlock = false;
            std::uint32_t blockOf0 = ~0u, blockOf1 = ~0u;  // 每组必须整体落在一块
            std::vector<int> blockOfGroup(g.groupCount, -2); // -2 未知
            for (std::uint32_t cl = 0; cl < nm.meshlets.size(); ++cl) {
                const int blk = nm.bounds[cl].sphere[0] < 50.f ? 0 : 1;
                int& bg = blockOfGroup[g.clusterGroup[cl]];
                if (bg == -2) bg = blk;
                else if (bg != blk) crossBlock = true;
            }
            (void)blockOf0; (void)blockOf1;
            check(!crossBlock, "no group spans disconnected blocks");
        }
        std::printf("  groups=%u max=%u fixups=%u\n",
                    g.groupCount, g.maxGroupSize, g.splitFixups);
    }

    // 5) 负向：空簇拒绝
    {
        nl::core::NMeshData empty;
        nl::core::GroupingResult g;
        std::string reason;
        check(!nl::core::groupClusters(empty, g, reason), "empty mesh rejected", reason);
    }

    if (failures == 0) {
        std::printf("group selftest: PASS\n");
        return 0;
    }
    std::printf("group selftest: FAIL (%d case(s))\n", failures);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "stat") return cmdStat(argc - 2, argv + 2);
    if (cmd == "cluster") return cmdCluster(argc - 2, argv + 2);
    if (cmd == "selftest") {
        if (argc >= 3 && std::string(argv[2]) == "cluster") return selftestCluster();
        if (argc >= 3 && std::string(argv[2]) == "nmesh") return selftestNMesh();
        if (argc >= 3 && std::string(argv[2]) == "group") return selftestGroup();
        std::fprintf(stderr, "selftest: unknown suite (available: cluster, nmesh, group)\n");
        return 1;
    }
    if (cmd == "simplify") return cmdSimplify(argc - 2, argv + 2);
    if (cmd == "dag") {
        std::fprintf(stderr, "%s: not implemented yet (S2 / S3)\n", cmd.c_str());
        return 2;
    }
    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    printUsage();
    return 1;
}
