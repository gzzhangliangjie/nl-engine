// M1.0：nanite-tools —— S1 起的离线网格处理工具。
// 子命令规划（S1-PLAN）：cluster（M1.1 切簇）/ simplify（S2 重切）/
// dag（S3 误差树）/ stat（M1.0 实现）/ selftest（M1.1 起的机械单测）。
// 只依赖 core（日志 + mesh_io + meshlet），零图形 API 符号——与 viewer 的纪律一致。

#include "nl/core/mesh_io.hpp"
#include "nl/core/meshlet.hpp"
#include "nl/core/meshlet_io.hpp"
#include "nl/core/grouping.hpp"
#include "nl/core/simplify.hpp"
#include "nl/core/lod.hpp"       // M2.2：逐层 LOD 循环

#include <meshoptimizer.h>   // M2.1：仅 simplify 的对照 oracle（正式 QEM 在 core/simplify）

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
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
        "  nanite-tools selftest cluster|nmesh|group|qem|loop  deterministic unit tests\n"
        "  nanite-tools simplify <model.nmesh> [-v] [-r 0.5] [--obj prefix]\n"
        "                        group QEM + boundary lock; --obj dumps before/after OBJ + locked pts\n"
        "  nanite-tools lod <model.nmesh> [-o prefix] [--max-levels 8] [-v]\n"
        "                        group/simplify/recluster loop; writes <prefix>.lodN.nmesh (v2 + DAG lineage)\n"
        "  nanite-tools check-orient <ref.nmesh> <target.nmesh>...\n"
        "                        orientation audit vs ref normals; exit 1 if any face inward\n"
        "  nanite-tools dag <prefix> [--export out.json]\n"
        "                        assemble + validate cluster DAG from <prefix>.lodN.nmesh; exit 1 on violation\n"
        "  nanite-tools selftest cluster|nmesh|group|qem|loop|dag  deterministic unit tests\n");
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

// ---- M2.3a：朝向验收（测试标准，先于一切修复）----
// 标准：任何三角形的面法线不得与其「参考法线」反向——反向 = 朝内 =
// 开背面剔除时的黑斑。inward == 0 才过关。参考法线 = 原始网格的外向
// 顶点法线；判据与数据源正交，量化为可执行的 exit code。
//
// 两条对比路径：
//   same-pool   简化 soup 的 canonical 点号与输入 weld 一一对应（点只是
//               移动，身份不变）→ 参考法线按点号直配；
//   nearest     分层 .nmesh 重切后编号全变 → 按位置最近邻取参考法线
//               （顶点位移 ≤ maxErr ≪ 网格尺度，语义等价）。
struct OrientationRef {
    std::uint32_t pointCount = 0;
    std::vector<float> pos;          // 3*pointCount，canonical 原始位置
    std::vector<float> nrm;          // 3*pointCount，原始法线和（外向）
};

OrientationRef buildOrientationRef(const nl::core::NMeshData& nm) {
    std::vector<std::uint32_t> canon;
    OrientationRef ref;
    ref.pointCount = nl::core::weldPositions(nm.vertices, canon);
    ref.pos.assign(std::size_t(ref.pointCount) * 3, 0.f);
    ref.nrm.assign(std::size_t(ref.pointCount) * 3, 0.f);
    std::vector<std::uint8_t> posSeen(std::size_t(ref.pointCount), 0);
    for (std::size_t i = 0; i < nm.vertices.size(); ++i) {
        const std::uint32_t p = canon[i];
        // 位置只赋值一次：同位置多份窗口顶点，累加会被放大 N 倍（法线累加是对的）
        if (!posSeen[p]) {
            for (int k = 0; k < 3; ++k)
                ref.pos[std::size_t(p) * 3 + k] = nm.vertices[i].pos[k];
            posSeen[p] = 1;
        }
        for (int k = 0; k < 3; ++k)
            ref.nrm[std::size_t(p) * 3 + k] += nm.vertices[i].normal[k];
    }
    return ref;
}

// 单三角形朝向余弦：面法线 vs 三角参考法线和（两者归一化）。
static float triOrientCos(const float* a, const float* b, const float* c,
                          const float* s0, const float* s1, const float* s2) {
    const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    const float n[3] = {
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    };
    const float s[3] = { s0[0] + s1[0] + s2[0],
                         s0[1] + s1[1] + s2[1],
                         s0[2] + s1[2] + s2[2] };
    const float nn = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const float sn = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (nn <= 0.f || sn <= 0.f) return 1.f;   // 退化不判
    return (n[0] * s[0] + n[1] * s[1] + n[2] * s[2]) / (nn * sn);
}

// 同池：soup 点号即 ref 点号（qem/simplify 的 canonical soup，ours 与
// oracle 同口径可比）。
int countInwardSamePool(const std::vector<float>& pos,
                        const std::vector<std::uint32_t>& idx,
                        const OrientationRef& ref,
                        float* outWorstCos) {
    int inward = 0;
    float worst = 1.f;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const std::uint32_t i[3] = { idx[t], idx[t + 1], idx[t + 2] };
        const float cosv = triOrientCos(
            &pos[std::size_t(i[0]) * 3], &pos[std::size_t(i[1]) * 3], &pos[std::size_t(i[2]) * 3],
            &ref.nrm[std::size_t(i[0]) * 3], &ref.nrm[std::size_t(i[1]) * 3], &ref.nrm[std::size_t(i[2]) * 3]);
        worst = std::min(worst, cosv);
        if (cosv < 0.f) ++inward;
    }
    if (outWorstCos) *outWorstCos = worst;
    return inward;
}

// 跨池：target canonical 点与 ref 无对应，按位置最近邻（brute force，
// 组内规模与层数都很小，不值得建空间索引）。maxDist 顺带报位移上界。
int countInwardNearest(const std::vector<float>& pos,
                       const std::vector<std::uint32_t>& idx,
                       const OrientationRef& ref,
                       float* outWorstCos, float* outMaxDist) {
    int inward = 0;
    float worst = 1.f, maxDist = 0.f;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const std::uint32_t i[3] = { idx[t], idx[t + 1], idx[t + 2] };
        const float* p[3] = {
            &pos[std::size_t(i[0]) * 3], &pos[std::size_t(i[1]) * 3], &pos[std::size_t(i[2]) * 3],
        };
        float s[3][3];
        for (int k = 0; k < 3; ++k) {
            float best = 1e30f;
            std::uint32_t bi = 0;
            for (std::uint32_t r = 0; r < ref.pointCount; ++r) {
                const float* rp = &ref.pos[std::size_t(r) * 3];
                const float d[3] = { rp[0] - p[k][0], rp[1] - p[k][1], rp[2] - p[k][2] };
                const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (dd < best) { best = dd; bi = r; }
            }
            maxDist = std::max(maxDist, std::sqrt(best));
            for (int c = 0; c < 3; ++c) s[k][c] = ref.nrm[std::size_t(bi) * 3 + c];
        }
        const float cosv = triOrientCos(p[0], p[1], p[2], s[0], s[1], s[2]);
        worst = std::min(worst, cosv);
        if (cosv < 0.f) ++inward;
    }
    if (outWorstCos) *outWorstCos = worst;
    if (outMaxDist) *outMaxDist = maxDist;
    return inward;
}

// check-orient <ref.nmesh> <target.nmesh>...
// 标准（先于一切修复）：每层朝内三角形数必须为 0，任何 >0 exit 1。
int cmdCheckOrient(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "check-orient: need <ref.nmesh> <target.nmesh>...\n");
        return 1;
    }
    nl::core::NMeshData refMesh;
    std::string error;
    if (!nl::core::loadNMesh(argv[0], refMesh, error)) {
        std::fprintf(stderr, "check-orient: ref %s: %s\n", argv[0], error.c_str());
        return 1;
    }
    const OrientationRef ref = buildOrientationRef(refMesh);

    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        nl::core::NMeshData nm;
        if (!nl::core::loadNMesh(argv[a], nm, error)) {
            std::fprintf(stderr, "check-orient: %s: %s\n", argv[a], error.c_str());
            rc = 1;
            continue;
        }
        std::vector<std::uint32_t> canon, idx;
        const std::uint32_t pc = nl::core::weldPositions(nm.vertices, canon);
        std::vector<float> pos(std::size_t(pc) * 3, 0.f);
        for (std::size_t i = 0; i < nm.vertices.size(); ++i)
            for (int k = 0; k < 3; ++k)
                pos[std::size_t(canon[i]) * 3 + k] = nm.vertices[i].pos[k];
        nl::core::buildCanonicalTriangles(nm, idx);
        float worst = 1.f, maxDist = 0.f;
        const int inward = countInwardNearest(pos, idx, ref, &worst, &maxDist);
        std::printf("%-34s tris %-6u inward %-4d worst cos %+.3f  maxPosDrift %.4f  %s\n",
                    argv[a], static_cast<unsigned>(idx.size() / 3), inward, worst, maxDist,
                    inward == 0 ? "PASS" : "FAIL");
        if (inward != 0) rc = 1;
    }
    return rc;
}

int cmdSimplify(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "simplify: missing <model.nmesh>\n");
        return 1;
    }
    const char* path = argv[0];
    bool verbose = false;
    float ratio = nl::core::kSimplifyDefaultRatio;
    std::string objPrefix;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.empty()) continue;  // 展示台 select 空选项展开成空 arg，视为未传
        if (a == "-v") verbose = true;
        else if (a == "--obj") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "simplify: --obj needs a path prefix\n");
                return 1;
            }
            objPrefix = argv[++i];
        }
        else if (a == "-r") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "simplify: -r needs a ratio in (0,1]\n");
                return 1;
            }
            ratio = float(std::atof(argv[++i]));
        } else {
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
    std::vector<std::uint32_t> sizeOf(g.groupCount, 0);
    for (std::uint32_t c = 0; c < nm.meshlets.size(); ++c)
        ++sizeOf[g.clusterGroup[c]];
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

    // ---- M2.1：组内 QEM 简化 + 边界锁定 ----
    nl::core::SimplifyResult sr;
    if (!nl::core::simplifyGroups(nm, g, sr, error, ratio)) {
        std::fprintf(stderr, "simplify: %s\n", error.c_str());
        return 1;
    }
    std::printf("qem ratio   : %.0f%%\n", double(ratio) * 100.0);
    std::printf("boundary pts: %zu locked (S3 DAG 沿用)\n", sr.lockedPoints.size());
    std::printf("simplify    : %u -> %u tris (%.1f%%)  collapses %u  maxErr %.4f (world)\n",
                sr.total.inputTriangles, sr.total.outputTriangles,
                sr.total.inputTriangles
                    ? 100.0 * double(sr.total.outputTriangles) / double(sr.total.inputTriangles)
                    : 0.0,
                sr.total.collapses, sr.total.maxError);
    if (verbose) {
        std::printf("per-group   :\n");
        for (std::uint32_t grp = 0; grp < g.groupCount; ++grp) {
            const auto& st = sr.perGroup[grp];
            std::printf("  [%3u] clusters=%-3u tris %-6u -> %-6u (%5.1f%%)  collapses %-6u  maxErr %.4f\n",
                        grp, sizeOf[grp], st.inputTriangles, st.outputTriangles,
                        st.inputTriangles
                            ? 100.0 * double(st.outputTriangles) / double(st.inputTriangles)
                            : 0.0,
                        st.collapses, st.maxError);
        }
    }

    // ---- meshopt 对照 oracle（同口径 LockBorder；正式实现是 core/simplify 的 QEM）----
    // 注：meshopt 锁的是喂给它的子网格的**全部**拓扑边界（含模型外缘），
    // 我们只锁组间边界 → oracle 通常更保守，简化率略低是预期。
    {
        std::vector<std::uint32_t> canon;
        const std::uint32_t pointCount = nl::core::weldPositions(nm.vertices, canon);
        std::vector<float> positions(std::size_t(pointCount) * 3, 0.f);
        for (std::size_t i = 0; i < nm.vertices.size(); ++i)
            for (int k = 0; k < 3; ++k)
                positions[std::size_t(canon[i]) * 3 + k] = nm.vertices[i].pos[k];

        // 按组拆分 canonical 三角形（重复点退化三角剔除，与 simplify 同口径）
        std::vector<std::vector<std::uint32_t>> groupIdx(g.groupCount);
        for (const auto& m : nm.meshlets) {
            const std::uint32_t grp = g.clusterGroup[&m - nm.meshlets.data()];
            for (std::uint32_t t = 0; t < m.triangleCount; ++t) {
                const std::size_t base = m.triangleOffset + std::size_t(t) * 3;
                const std::uint32_t ids[3] = {
                    canon[m.vertexOffset + nm.triangles[base + 0]],
                    canon[m.vertexOffset + nm.triangles[base + 1]],
                    canon[m.vertexOffset + nm.triangles[base + 2]],
                };
                if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2]) continue;
                groupIdx[grp].insert(groupIdx[grp].end(), { ids[0], ids[1], ids[2] });
            }
        }

        std::uint32_t oracleIn = 0, oracleOut = 0;
        double oracleErrSum = 0, oracleErrMax = 0;
        std::uint32_t oracleGroups = 0;
        std::vector<std::uint32_t> oracleIdx;   // M2.3a：oracle 输出 soup（同 canonical 池）
        for (const auto& gi : groupIdx) {
            if (gi.size() < 3) continue;
            oracleIn += std::uint32_t(gi.size() / 3);
            std::vector<std::uint32_t> dst(gi.size());
            const std::size_t targetIdx = ((gi.size() / 2) / 3) * 3;
            float err = 0.f;
            const std::size_t n = meshopt_simplify(
                dst.data(), gi.data(), gi.size(),
                positions.data(), pointCount, 12, targetIdx, 1e6f,
                meshopt_SimplifyLockBorder, &err);
            oracleOut += std::uint32_t(n / 3);
            oracleIdx.insert(oracleIdx.end(), dst.begin(), dst.begin() + n);
            oracleErrSum += err;
            oracleErrMax = std::max(oracleErrMax, double(err));
            ++oracleGroups;
        }
        std::printf("oracle      : meshopt LockBorder %u -> %u tris (%.1f%%)  rel err avg %.4f max %.4f\n",
                    oracleIn, oracleOut,
                    oracleIn ? 100.0 * double(oracleOut) / double(oracleIn) : 0.0,
                    oracleGroups ? oracleErrSum / oracleGroups : 0.0, oracleErrMax);

        // M2.3a：同判据朝向对比（同一分组 / 同一 50% 硬目标 / 同一参考法线）。
        // oracle = 官方实现的地面真值；ours > oracle → 我们的简化器漏了官方
        // 的某道保护；两者都 > 0 → 是 per-group 硬目标管线本身的问题。
        const OrientationRef oref = buildOrientationRef(nm);
        float worstOurs = 1.f, worstOracle = 1.f;
        const int inwardOurs = countInwardSamePool(sr.pointPositions, sr.indices, oref, &worstOurs);
        const int inwardOracle = countInwardSamePool(positions, oracleIdx, oref, &worstOracle);
        std::printf("orientation : ours inward %-4d worst %+.3f | oracle inward %-4d worst %+.3f  %s\n",
                    inwardOurs, worstOurs, inwardOracle, worstOracle,
                    inwardOurs == 0 && inwardOracle == 0 ? "PASS"
                        : inwardOracle == 0 ? "OURS-FAILS-ORACLE-PASS" : "BOTH-FAIL");
    }

    // ---- 可视化验收导出：真实几何（M2.1 补图；M2.2 落盘同口径）----
    // --obj <prefix> 写 <prefix>.before.obj / .after.obj / .locked：
    // before = 输入的 canonical 三角 soup，after = QEM 简化结果，
    // locked = 组间边界 canonical 点编号（0-based，逐行一个）。
    if (!objPrefix.empty()) {
        const auto parent = std::filesystem::path(objPrefix).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        std::vector<std::uint32_t> canon;
        const std::uint32_t pcBefore = nl::core::weldPositions(nm.vertices, canon);
        std::vector<float> beforePos(std::size_t(pcBefore) * 3, 0.f);
        for (std::size_t i = 0; i < nm.vertices.size(); ++i)
            for (int k = 0; k < 3; ++k)
                beforePos[std::size_t(canon[i]) * 3 + k] = nm.vertices[i].pos[k];
        std::vector<std::uint32_t> beforeIdx;
        nl::core::buildCanonicalTriangles(nm, beforeIdx);

        auto writeObj = [](const std::string& file, const std::vector<float>& pos,
                           const std::vector<std::uint32_t>& idx) {
            std::FILE* f = std::fopen(file.c_str(), "wb");
            if (!f) return false;
            for (std::size_t i = 0; i < pos.size(); i += 3)
                std::fprintf(f, "v %.7g %.7g %.7g\n", pos[i], pos[i + 1], pos[i + 2]);
            for (std::size_t i = 0; i < idx.size(); i += 3)
                std::fprintf(f, "f %u %u %u\n", idx[i] + 1, idx[i + 1] + 1, idx[i + 2] + 1);
            return std::fclose(f) == 0;
        };
        const std::string beforeFile = objPrefix + ".before.obj";
        const std::string afterFile = objPrefix + ".after.obj";
        const std::string lockedFile = objPrefix + ".locked";
        bool ok = writeObj(beforeFile, beforePos, beforeIdx)
               && writeObj(afterFile, sr.pointPositions, sr.indices);
        if (ok) {
            std::FILE* f = std::fopen(lockedFile.c_str(), "wb");
            if (!f) ok = false;
            else {
                for (std::uint32_t p : sr.lockedPoints) std::fprintf(f, "%u\n", p);
                ok = std::fclose(f) == 0;
            }
        }
        if (!ok) {
            std::fprintf(stderr, "simplify: --obj failed to write %s*\n", objPrefix.c_str());
            return 1;
        }
        std::printf("obj dump   : %s.before.obj (%u pts / %zu tris), %s.after.obj (%u pts / %u tris), %s.locked (%zu pts)\n",
                    objPrefix.c_str(), pcBefore, beforeIdx.size() / 3,
                    objPrefix.c_str(), sr.pointCount, sr.total.outputTriangles,
                    objPrefix.c_str(), sr.lockedPoints.size());
    }

    return bad == 0 ? 0 : 1;
}

// ---- M2.2：逐层 LOD 循环（聚组→组内 QEM 简化→重切→分层落盘）----
int cmdLod(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "lod: missing <model.nmesh>\n");
        return 1;
    }
    const char* path = argv[0];
    std::string outPrefix;
    nl::core::LodOptions opt;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.empty()) continue;  // 展示台 select 空选项展开成空 arg
        if (a == "-v") verbose = true;
        else if (a == "-o" || a == "--out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lod: -o needs a path prefix\n");
                return 1;
            }
            outPrefix = argv[++i];
        } else if (a == "--max-levels") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lod: --max-levels needs an integer >= 1\n");
                return 1;
            }
            opt.maxLevels = std::uint32_t(std::atoi(argv[++i]));
        } else if (a == "--ratio") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lod: --ratio needs a value in (0,1]\n");
                return 1;
            }
            opt.ratio = float(std::atof(argv[++i]));
        } else {
            std::fprintf(stderr, "lod: unknown option %s\n", argv[i]);
            return 1;
        }
    }
    if (outPrefix.empty()) {
        std::string p(path);
        if (p.size() >= 6 && p.compare(p.size() - 6, 6, ".nmesh") == 0)
            p.resize(p.size() - 6);
        outPrefix = p + ".lod";
    }

    std::string error;
    nl::core::NMeshData nm;
    if (!nl::core::loadNMesh(path, nm, error)) {
        std::fprintf(stderr, "lod: %s\n", error.c_str());
        return 1;
    }
    nl::core::LodHierarchy h;
    if (!nl::core::buildLodHierarchy(nm, h, error, opt)) {
        std::fprintf(stderr, "lod: %s\n", error.c_str());
        return 1;
    }

    std::printf("file        : %s\n", path);
    std::printf("out prefix  : %s\n", outPrefix.c_str());
    std::printf("qem ratio   : %.0f%% (per-group hard target)  maxLevels %u  minTris %u\n",
                double(opt.ratio) * 100.0, opt.maxLevels, opt.minTriangles);
    std::printf("levels      : %zu (L0..L%zu)\n", h.levels.size(), h.levels.size() - 1);
    for (const auto& lv : h.levels) {
        std::printf("  L%-2u clusters=%-5u tris=%-7u groups=%-4u locked=%-6u "
                    "collapses=%-6u maxErr=%.4f (parent, world)\n",
                    lv.level, lv.clusters, lv.triangles, lv.groups, lv.lockedPoints,
                    lv.collapses, lv.maxError);
    }

    const auto parent = std::filesystem::path(outPrefix).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::uint32_t written = 0;
    for (const auto& lv : h.levels) {
        const std::string file = outPrefix + ".lod" + std::to_string(lv.level) + ".nmesh";
        if (!nl::core::saveNMesh(file.c_str(), lv.mesh, error)) {
            std::fprintf(stderr, "lod: %s\n", error.c_str());
            return 1;
        }
        ++written;
        if (verbose) std::printf("  wrote     : %s\n", file.c_str());
    }
    std::printf("lod files   : %u written (%s.lod0.nmesh .. %s.lod%zu.nmesh, .nmesh v2)\n",
                written, outPrefix.c_str(), outPrefix.c_str(), h.levels.size() - 1);
    return 0;
}

// ---- M3.0：dag 子命令 —— 装配 + 校验 cluster DAG（S3 验收核心）----
//
// 数据来自分层资产的 lodMeta DAG 血缘（.nmesh meta version 2，lod 命令产出）。
// 校验不变量（三家口径，见 core/lod.hpp 头注释）：
//   membership  每簇恰属一组（成员方向覆盖完整）
//   generation  非顶层各组 gen 区间连续无缝铺满下一层全部簇 → 每个父簇
//               恰有一个生成组（按组重切的声音性；UE 组原子 cut 的前提）
//   error chain 组 error == max(simplifyError, 全部成员簇误差)，逐位精确
//               （UE ClusterDAG.cpp:1441+:1587）
//   parent err  L≥1 每簇 bounds.error == 生成组 error（UE :1593 统一携带）
//   monotonic   组 error ≥ 其成员的生成组 error（误差链沿 DAG 单调不减）
//   acyclic     组图 DFS 染色无环（S3-PLAN 自检项）

struct DagLevelData {
    nl::core::NMeshData mesh;
    std::uint32_t clusters = 0;
    std::uint32_t triangles = 0;
};

struct DagCheckReport {
    bool membershipOk = true;   // 每簇恰属一组
    bool generationOk = true;   // gen 区间铺满下一层，每父簇恰一生成组
    bool errorChainOk = true;   // 组 error == max(simplifyError, 成员误差) 逐位精确
    bool parentErrOk = true;    // L≥1 爰簇 bounds.error 携带生成组误差
    bool monotonicOk = true;    // 组 error ≥ 生成组 error（沿链单调）
    bool acyclicOk = true;      // 组图 DFS 无环
    bool ok() const { return membershipOk && generationOk && errorChainOk &&
                             parentErrOk && monotonicOk && acyclicOk; }
    std::string firstFailure;        // 第一条失败原因（各分项照常统计）
    std::size_t groupsTotal = 0;
    std::size_t errorChainChecks = 0;
    std::size_t parentErrorChecks = 0;
    std::size_t monotonicChecks = 0;
};

namespace dag_detail {

// L≥1 层：簇 → 生成组（上一层组号）。gen 区间连续铺满本层，直接扫区间。
// nextClusters = 本层（被生成层）的簇数——genFirst/genCount 索引下一层簇号，
// 数组必须按下一层开尺寸，不是 prev.clusters（M3.0 自检抓出的越界 bug）。
std::vector<std::uint32_t> buildGeneratingMap(const DagLevelData& prev,
                                              std::uint32_t nextClusters) {
    std::vector<std::uint32_t> gen(nextClusters, 0xFFFFFFFFu);
    for (std::uint32_t gi = 0; gi < prev.mesh.lodMeta.dagGroups.size(); ++gi) {
        const auto& dg = prev.mesh.lodMeta.dagGroups[gi];
        for (std::uint32_t k = 0; k < dg.genCount; ++k) {
            const std::uint32_t c = dg.genFirst + k;
            if (c < gen.size()) gen[c] = gi;
        }
    }
    return gen;
}

} // namespace dag_detail

// 全链校验。levels[i].mesh.lodMeta 必须已带 DAG 血缘（meta version 2）。
DagCheckReport validateDagChain(const std::vector<DagLevelData>& levels) {
    DagCheckReport rep;
    auto fail = [&](bool& flag, const std::string& why) {
        flag = false;
        if (rep.firstFailure.empty()) rep.firstFailure = why;
    };
    if (levels.empty()) {
        rep.membershipOk = rep.generationOk = rep.errorChainOk = false;
        rep.parentErrOk = rep.monotonicOk = rep.acyclicOk = false;
        rep.firstFailure = "no levels";
        return rep;
    }

    for (std::size_t L = 0; L < levels.size(); ++L) {
        const auto& lv = levels[L];
        const auto& meta = lv.mesh.lodMeta;
        const std::uint32_t G = std::uint32_t(meta.dagGroups.size());
        rep.groupsTotal += G;

        // membership：每簇恰属一组，组号合法，每组至少一簇
        if (meta.clusterGroup.size() != lv.mesh.meshlets.size()) {
            fail(rep.membershipOk, "L" + std::to_string(L) + ": clusterGroup size mismatch");
            continue;
        }
        std::vector<std::uint32_t> memberCount(G, 0);
        for (std::uint32_t c : meta.clusterGroup) {
            if (c >= G) {
                fail(rep.membershipOk, "L" + std::to_string(L) + ": cluster group id out of range");
                break;
            }
            ++memberCount[c];
        }
        for (std::uint32_t gi = 0; gi < G; ++gi)
            if (memberCount[gi] == 0)
                fail(rep.membershipOk, "L" + std::to_string(L) + ": group " +
                     std::to_string(gi) + " has no members");

        // generation：gen 区间铺满下一层（顶层全 0）
        const bool isTop = (L + 1 == levels.size());
        std::vector<std::uint32_t> genMap;
        if (!isTop) {
            std::uint32_t sum = 0;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;   // [first, first+count)
            for (std::uint32_t gi = 0; gi < G; ++gi) {
                const auto& dg = meta.dagGroups[gi];
                if (dg.genCount == 0)
                    fail(rep.generationOk, "L" + std::to_string(L) + ": non-top group " +
                         std::to_string(gi) + " generates no clusters");
                sum += dg.genCount;
                if (dg.genCount == 0) continue;
                spans.push_back({ dg.genFirst, dg.genFirst + dg.genCount });
            }
            if (sum != levels[L + 1].clusters)
                fail(rep.generationOk, "L" + std::to_string(L) + ": generated clusters " +
                     std::to_string(sum) + " != next level " +
                     std::to_string(levels[L + 1].clusters));
            std::sort(spans.begin(), spans.end());
            std::uint32_t expect = 0;
            for (const auto& s : spans) {
                if (s.first != expect || s.second <= s.first) {
                    fail(rep.generationOk, "L" + std::to_string(L) +
                         ": gen ranges not contiguous/overlap");
                    break;
                }
                expect = s.second;
            }
            if (expect != levels[L + 1].clusters)
                fail(rep.generationOk, "L" + std::to_string(L) +
                     ": gen ranges do not cover next level");
            genMap = dag_detail::buildGeneratingMap(levels[L], levels[L + 1].clusters);
            for (std::uint32_t c = 0; c < genMap.size(); ++c)
                if (genMap[c] == 0xFFFFFFFFu) {
                    fail(rep.generationOk, "L" + std::to_string(L + 1) + ": cluster " +
                         std::to_string(c) + " has no generating group");
                    break;
                }
        } else {
            for (std::uint32_t gi = 0; gi < G; ++gi)
                if (meta.dagGroups[gi].genCount != 0)
                    fail(rep.generationOk, "L" + std::to_string(L) + ": top group " +
                         std::to_string(gi) + " generates clusters");
        }

        // error chain（逐位精确）：组 error == max(simplifyError, 全部成员簇误差)
        for (std::uint32_t gi = 0; gi < G; ++gi) {
            const auto& dg = meta.dagGroups[gi];
            float memberMax = 0.f;
            for (std::uint32_t c = 0; c < lv.clusters; ++c) {
                if (meta.clusterGroup[c] != gi) continue;
                memberMax = std::max(memberMax, lv.mesh.bounds[c].error);
            }
            const float expected = std::max(dg.simplifyError, memberMax);
            ++rep.errorChainChecks;
            if (dg.error != expected)
                fail(rep.errorChainOk, "L" + std::to_string(L) + " group " +
                     std::to_string(gi) + ": error chain mismatch (stored " +
                     std::to_string(dg.error) + " expected " +
                     std::to_string(expected) + ")");
        }
        if (L > 0) {
            const std::vector<std::uint32_t> genMapPrev =
                dag_detail::buildGeneratingMap(levels[L - 1], lv.clusters);
            for (std::uint32_t c = 0; c < lv.clusters; ++c) {
                const std::uint32_t pg = genMapPrev[c];
                const float gerr = levels[L - 1].mesh.lodMeta.dagGroups[pg].error;
                ++rep.parentErrorChecks;
                if (lv.mesh.bounds[c].error != gerr)
                    fail(rep.parentErrOk, "L" + std::to_string(L) + " cluster " +
                         std::to_string(c) +
                         ": bounds.error does not carry generating group error");
                // monotonic：本层每组成员的生成组误差 ≤ 该组 error
                const std::uint32_t gi = lv.mesh.lodMeta.clusterGroup[c];
                ++rep.monotonicChecks;
                if (levels[L].mesh.lodMeta.dagGroups[gi].error + 1e-9f < gerr)
                    fail(rep.monotonicOk, "L" + std::to_string(L) + " group " +
                         std::to_string(gi) +
                         ": error below generating group error (non-monotonic)");
            }
        }
    }

    // acyclic：组图 DFS 染色（0=白 1=灰 2=黑），灰上回边 = 有环
    {
        const std::size_t N = rep.groupsTotal;
        std::vector<std::uint8_t> color(N, 0);
        std::vector<std::size_t> levelOffset(levels.size() + 1, 0);
        for (std::size_t L = 0; L < levels.size(); ++L)
            levelOffset[L + 1] = levelOffset[L] + levels[L].mesh.lodMeta.dagGroups.size();
        std::vector<std::vector<std::size_t>> adj(N);
        for (std::size_t L = 0; L + 1 < levels.size(); ++L) {
            const auto& meta = levels[L].mesh.lodMeta;
            for (std::uint32_t gi = 0; gi < meta.dagGroups.size(); ++gi) {
                const auto& dg = meta.dagGroups[gi];
                std::set<std::uint32_t> targets;
                for (std::uint32_t k = 0; k < dg.genCount; ++k) {
                    const std::uint32_t c = dg.genFirst + k;
                    if (c < levels[L + 1].clusters)
                        targets.insert(levels[L + 1].mesh.lodMeta.clusterGroup[c]);
                }
                for (std::uint32_t gj : targets)
                    adj[levelOffset[L] + gi].push_back(levelOffset[L + 1] + gj);
            }
        }
        bool cyclic = false;
        for (std::size_t root = 0; root < N && !cyclic; ++root) {
            if (color[root] != 0) continue;
            // 迭代式 DFS：栈元素 {node, done}（done=1 表示回溯时染黑）。
            // 灰色 = 当前 DFS 路径上的祖先（其 done 标记仍在栈下），灰上回边 = 有环。
            std::vector<std::pair<std::size_t, bool>> work;
            work.push_back({ root, false });
            while (!work.empty() && !cyclic) {
                auto [node, done] = work.back();
                work.pop_back();
                if (done) { color[node] = 2; continue; }
                if (color[node] == 1) continue;     // 已在栈上展开过
                if (color[node] == 2) continue;
                color[node] = 1;
                work.push_back({ node, true });
                for (std::size_t nxt : adj[node]) {
                    if (color[nxt] == 1) { cyclic = true; break; }
                    if (color[nxt] == 0) work.push_back({ nxt, false });
                }
            }
            if (cyclic) break;
        }
        if (cyclic) fail(rep.acyclicOk, "group graph has a cycle");
    }
    return rep;
}

// ---- dag 子命令入口 ----

int cmdDag(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "dag: missing <prefix> (layered assets <prefix>.lodN.nmesh)\n");
        return 1;
    }
    std::string prefix = argv[0];
    std::string exportPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.empty()) continue;  // 展示台 select 空选项展开成空 arg，视为未传
        if (a == "--export") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "dag: --export needs a json path\n");
                return 1;
            }
            exportPath = argv[++i];
        } else {
            std::fprintf(stderr, "dag: unknown option %s\n", argv[i]);
            return 1;
        }
    }

    // 连续探测 <prefix>.lodN.nmesh（N=0..），与 renderer 的探测同一约定
    std::vector<DagLevelData> levels;
    std::string error;
    for (std::uint32_t n = 0;; ++n) {
        const std::string file = prefix + ".lod" + std::to_string(n) + ".nmesh";
        DagLevelData lv;
        if (!nl::core::loadNMesh(file.c_str(), lv.mesh, error)) {
            if (n == 0) {
                std::fprintf(stderr, "dag: %s\n", error.c_str());
                return 1;
            }
            break;
        }
        if (!lv.mesh.lodMeta.present || !lv.mesh.lodMeta.dagPresent) {
            std::fprintf(stderr,
                         "dag: %s has no DAG lineage (meta version 2); "
                         "re-run: nanite-tools lod <model.nmesh> -o %s\n",
                         file.c_str(), prefix.c_str());
            return 1;
        }
        lv.clusters = std::uint32_t(lv.mesh.meshlets.size());
        lv.triangles = 0;
        for (const auto& m : lv.mesh.meshlets) lv.triangles += m.triangleCount;
        if (lv.mesh.lodMeta.level != std::uint16_t(n)) {
            std::fprintf(stderr, "dag: %s claims level %u (expected %u)\n",
                         file.c_str(), lv.mesh.lodMeta.level, n);
            return 1;
        }
        levels.push_back(std::move(lv));
    }

    std::printf("file        : %s\n", prefix.c_str());
    std::printf("levels      : %zu (L0..L%zu)\n", levels.size(), levels.size() - 1);
    for (std::size_t L = 0; L < levels.size(); ++L) {
        const auto& lv = levels[L];
        const auto& meta = lv.mesh.lodMeta;
        std::uint32_t genTotal = 0;
        for (const auto& dg : meta.dagGroups) genTotal += dg.genCount;
        std::printf("  L%-2zu clusters=%-5u tris=%-7u groups=%-4u parentClusters=%-5u "
                    "topErr=%.4f\n",
                    L, lv.clusters, lv.triangles,
                    std::uint32_t(meta.dagGroups.size()), genTotal,
                    meta.maxError);
    }

    const DagCheckReport rep = validateDagChain(levels);
    std::printf("dag checks  :\n");
    std::printf("  membership    : %s (every cluster in exactly one group)\n",
                rep.membershipOk ? "PASS" : "FAIL");
    std::printf("  generation    : %s (per-parent single generating group, spans tile next level)\n",
                rep.generationOk ? "PASS" : "FAIL");
    std::printf("  error chain   : %s (%zu groups, group err == max(simplifyErr, members))\n",
                rep.errorChainOk ? "PASS" : "FAIL", rep.errorChainChecks);
    std::printf("  parent error  : %s (%zu clusters carry group error, UE ClusterDAG.cpp:1593)\n",
                rep.parentErrOk ? "PASS" : "FAIL", rep.parentErrorChecks);
    std::printf("  monotonic     : %s (%zu edges, group err >= generating group err)\n",
                rep.monotonicOk ? "PASS" : "FAIL", rep.monotonicChecks);
    std::printf("  acyclic (DFS) : %s\n", rep.acyclicOk ? "PASS" : "FAIL");
    if (!rep.ok()) {
        std::fprintf(stderr, "dag: FAIL: %s\n", rep.firstFailure.c_str());
        return 1;
    }
    std::printf("dag         : OK (%zu groups across %zu levels)\n",
                rep.groupsTotal, levels.size());

    if (!exportPath.empty()) {
        std::FILE* fp = std::fopen(exportPath.c_str(), "wb");
        if (!fp) {
            std::fprintf(stderr, "dag: cannot open for write: %s\n", exportPath.c_str());
            return 1;
        }
        // JSON 字符串转义：Windows 反斜杠路径必须写 \\，否则导出非法 JSON
        //（run.bat 从 build\showcase\runs 实跑时抓出）。
        std::string prefixJson;
        prefixJson.reserve(prefix.size());
        for (char ch : prefix) {
            if (ch == '\\' || ch == '"') prefixJson.push_back('\\');
            prefixJson.push_back(ch);
        }
        std::fprintf(fp, "{\n  \"prefix\": \"%s\",\n  \"levels\": [\n",
                     prefixJson.c_str());
        for (std::size_t L = 0; L < levels.size(); ++L) {
            const auto& lv = levels[L];
            const auto& meta = lv.mesh.lodMeta;
            std::fprintf(fp, "    {\"level\": %zu, \"clusters\": %u, \"triangles\": %u, "
                             "\"groups\": [\n",
                         L, lv.clusters, lv.triangles);
            for (std::uint32_t gi = 0; gi < meta.dagGroups.size(); ++gi) {
                const auto& dg = meta.dagGroups[gi];
                std::fprintf(fp,
                             "      {\"id\": %u, \"error\": %.6g, \"simplifyError\": %.6g, "
                             "\"sphere\": [%.6g, %.6g, %.6g, %.6g], \"children\": [",
                             gi, double(dg.error), double(dg.simplifyError),
                             double(dg.sphere[0]), double(dg.sphere[1]),
                             double(dg.sphere[2]), double(dg.sphere[3]));
                bool firstC = true;
                for (std::uint32_t c = 0; c < lv.clusters; ++c) {
                    if (meta.clusterGroup[c] != gi) continue;
                    std::fprintf(fp, "%s%u", firstC ? "" : ", ", c);
                    firstC = false;
                }
                std::fprintf(fp, "], \"parents\": [");
                for (std::uint32_t k = 0; k < dg.genCount; ++k)
                    std::fprintf(fp, "%s%u", k ? ", " : "", dg.genFirst + k);
                std::fprintf(fp, "]}%s\n",
                             gi + 1 < meta.dagGroups.size() ? "," : "");
            }
            std::fprintf(fp, "    ]}%s\n", L + 1 < levels.size() ? "," : "");
        }
        std::fprintf(fp, "  ]\n}\n");
        std::fclose(fp);
        std::printf("export      : %s\n", exportPath.c_str());
    }
    return 0;
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

// ---- selftest qem：QEM 简化 + 边界锁定机械单测（M2.1 验收核心）----
// 灵魂断言（S2-PLAN 指定）：两个相邻组各自简化后，公共边界**逐顶点完全一致**
// —— 自动对比，不靠肉眼。边界集合与接缝边存活在 selftest 侧独立重算，
// 与 simplify.cpp 交叉验证防同错。

int selftestQem() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name, const std::string& detail = {}) {
        if (ok) std::printf("  ok   %s\n", name);
        else { std::printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str()); ++failures; }
    };

    // 独立重算：canonical 点 → 引用它的组集合（≥2 组 = 边界）；边界点位置代表元
    auto independentBoundary =
        [](const nl::core::NMeshData& nm, const nl::core::GroupingResult& g,
           std::vector<std::uint32_t>& boundary,
           std::vector<std::array<std::uint32_t, 3>>& boundaryTrisPerPoint) {
            std::vector<std::uint32_t> canon;
            const std::uint32_t pointCount = nl::core::weldPositions(nm.vertices, canon);
            // 倒排：点 → 组集合（小集合用线性扫描，规模小无所谓）
            std::vector<std::vector<std::uint32_t>> groupsOf(pointCount);
            for (std::size_t c = 0; c < nm.meshlets.size(); ++c) {
                const auto& m = nm.meshlets[c];
                const std::uint32_t grp = g.clusterGroup[c];
                for (std::uint32_t i = 0; i < m.vertexCount; ++i) {
                    const std::uint32_t p = canon[m.vertexOffset + i];
                    if (std::find(groupsOf[p].begin(), groupsOf[p].end(), grp)
                        == groupsOf[p].end())
                        groupsOf[p].push_back(grp);
                }
            }
            boundary.clear();
            for (std::uint32_t p = 0; p < pointCount; ++p)
                if (groupsOf[p].size() >= 2) boundary.push_back(p);
            (void)boundaryTrisPerPoint;
            return groupsOf;
        };

    // 独立重算：接缝边（双边界端点）在每组的三角形覆盖数（简化前）
    struct SeamEdge { std::uint64_t key; std::vector<std::uint32_t> perGroup; };
    auto seamEdgesBefore =
        [](const nl::core::NMeshData& nm, const nl::core::GroupingResult& g,
           const std::vector<std::uint32_t>& boundary,
           std::vector<SeamEdge>& seams) {
            const auto keyOf = [](std::uint32_t a, std::uint32_t b) {
                return (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            };
            std::vector<std::uint8_t> isBoundary;
            {   // boundary 已排序
                std::vector<std::uint32_t> canon;
                const std::uint32_t pointCount = nl::core::weldPositions(nm.vertices, canon);
                isBoundary.assign(pointCount, 0);
                for (std::uint32_t p : boundary) isBoundary[p] = 1;
            }
            std::vector<std::uint32_t> canon;
            nl::core::weldPositions(nm.vertices, canon);
            std::map<std::uint64_t, std::vector<std::uint32_t>> acc;  // 有序 → 确定
            for (std::size_t c = 0; c < nm.meshlets.size(); ++c) {
                const auto& m = nm.meshlets[c];
                const std::uint32_t grp = g.clusterGroup[c];
                for (std::uint32_t t = 0; t < m.triangleCount; ++t) {
                    const std::size_t base = m.triangleOffset + std::size_t(t) * 3;
                    const std::uint32_t ids[3] = {
                        canon[m.vertexOffset + nm.triangles[base + 0]],
                        canon[m.vertexOffset + nm.triangles[base + 1]],
                        canon[m.vertexOffset + nm.triangles[base + 2]],
                    };
                    if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2]) continue;
                    for (int k = 0; k < 3; ++k) {
                        const std::uint32_t u = ids[k], v = ids[(k + 1) % 3];
                        if (!isBoundary[u] || !isBoundary[v]) continue;
                        auto& cnt = acc[keyOf(u, v)];
                        if (std::find(cnt.begin(), cnt.end(), grp) == cnt.end())
                            cnt.push_back(grp);
                    }
                }
            }
            seams.clear();
            for (auto& [k, gs] : acc)
                seams.push_back({ k, std::move(gs) });
        };

    // 简化后：接缝边在每组的三角形覆盖数
    auto seamEdgesAfter =
        [](const nl::core::SimplifyResult& sr, const std::vector<SeamEdge>& before,
           std::vector<std::vector<std::uint32_t>>& perGroupCount) {
            const auto keyOf = [](std::uint32_t a, std::uint32_t b) {
                return (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            };
            std::map<std::uint64_t, std::vector<std::uint32_t>> acc;
            for (std::uint32_t grp = 0; grp < sr.groupTriCount.size(); ++grp) {
                const std::uint32_t first = sr.groupFirstTri[grp];
                for (std::uint32_t t = 0; t < sr.groupTriCount[grp]; ++t) {
                    const std::size_t base = std::size_t(first + t) * 3;
                    const std::uint32_t ids[3] = {
                        sr.indices[base + 0], sr.indices[base + 1], sr.indices[base + 2],
                    };
                    for (int k = 0; k < 3; ++k) {
                        const std::uint64_t k2 = keyOf(ids[k], ids[(k + 1) % 3]);
                        auto& cnt = acc[k2];
                        if (std::find(cnt.begin(), cnt.end(), grp) == cnt.end())
                            cnt.push_back(grp);
                    }
                }
            }
            perGroupCount.clear();
            for (const auto& se : before) {
                std::vector<std::uint32_t> cnt;
                auto it = acc.find(se.key);
                if (it != acc.end()) cnt = it->second;
                perGroupCount.push_back(std::move(cnt));
            }
        };

    // Case 1：连续 64x64 网格 → 多组相邻 → 简化。灵魂断言全在这里。
    {
        std::printf("[connected 64x64 grid, group QEM 50%%]\n");
        nl::core::NMeshData nm;
        std::string reason;
        if (!makeNMesh(makeTestMesh(1, 64, 64), nm, reason)) {
            check(false, "makeNMesh", reason);
        } else {
            nl::core::GroupingResult g;
            if (!nl::core::groupClusters(nm, g, reason)) {
                check(false, "groupClusters", reason);
            } else {
                std::vector<std::uint32_t> boundary;
                std::vector<std::array<std::uint32_t, 3>> unused;
                const auto groupsOf = independentBoundary(nm, g, boundary, unused);

                nl::core::SimplifyResult sr;
                if (!nl::core::simplifyGroups(nm, g, sr, reason)) {
                    check(false, "simplifyGroups", reason);
                } else {
                    // 1) 锁定点集合 == 独立重算的边界集合（交叉验证）
                    bool lockedOk = sr.lockedPoints == boundary;
                    check(lockedOk, "locked set == independent boundary set",
                          "locked=" + std::to_string(sr.lockedPoints.size()) +
                          " independent=" + std::to_string(boundary.size()));

                    // 2) 灵魂断言：边界点位置逐点位级不变
                    bool positionsOk = true;
                    std::string posDetail;
                    {
                        std::vector<std::uint32_t> canon;
                        nl::core::weldPositions(nm.vertices, canon);
                        std::vector<std::uint32_t> rep(nm.vertices.size(), ~0u);
                        for (std::size_t i = 0; i < nm.vertices.size(); ++i) {
                            const std::uint32_t p = canon[i];
                            if (rep[p] == ~0u) rep[p] = std::uint32_t(i);
                        }
                        for (std::uint32_t p : sr.lockedPoints) {
                            const float* orig = nm.vertices[rep[p]].pos;
                            const float* now = &sr.pointPositions[std::size_t(p) * 3];
                            if (orig[0] != now[0] || orig[1] != now[1] || orig[2] != now[2]) {
                                positionsOk = false;
                                posDetail = "point " + std::to_string(p) + " moved";
                                break;
                            }
                        }
                    }
                    check(positionsOk, "boundary vertices bit-identical", posDetail);

                    // 3) 接缝边两侧都有三角形（无裂缝的拓扑前提）
                    std::vector<SeamEdge> seams;
                    seamEdgesBefore(nm, g, boundary, seams);
                    std::vector<std::vector<std::uint32_t>> after;
                    seamEdgesAfter(sr, seams, after);
                    bool seamsOk = true;
                    std::string seamDetail;
                    for (std::size_t i = 0; i < seams.size() && seamsOk; ++i) {
                        for (std::uint32_t grp : seams[i].perGroup) {
                            const bool alive =
                                std::find(after[i].begin(), after[i].end(), grp)
                                != after[i].end();
                            if (!alive) {
                                seamsOk = false;
                                seamDetail = "seam edge #" + std::to_string(i) +
                                             " lost in group " + std::to_string(grp);
                                break;
                            }
                        }
                    }
                    check(seamsOk, "seam edges alive on both sides", seamDetail);

                    // 4) 简化率：接近 50%（拓扑约束下允许小幅偏差），每组非空
                    bool ratioOk = sr.total.inputTriangles > 0;
                    const double r = ratioOk
                        ? double(sr.total.outputTriangles) / double(sr.total.inputTriangles)
                        : 0.0;
                    ratioOk = ratioOk && r >= 0.30 && r <= 0.60;
                    bool nonEmpty = true;
                    for (std::uint32_t c : sr.groupTriCount)
                        if (c == 0) nonEmpty = false;
                    check(ratioOk, "output ratio in [30%,60%]",
                          "ratio=" + std::to_string(r));
                    check(nonEmpty, "every group keeps triangles");

                    // 5) 索引合法
                    bool idxOk = true;
                    for (std::uint32_t idx : sr.indices)
                        if (idx >= sr.pointCount) { idxOk = false; break; }
                    check(idxOk, "indices in range");

                    // 6) 确定性：同输入重跑逐字节一致
                    nl::core::SimplifyResult sr2;
                    std::string reason2;
                    if (nl::core::simplifyGroups(nm, g, sr2, reason2)) {
                        check(sr2.indices == sr.indices &&
                              sr2.lockedPoints == sr.lockedPoints &&
                              sr2.groupTriCount == sr.groupTriCount &&
                              sr2.total.collapses == sr.total.collapses,
                              "deterministic (rerun identical)");
                    } else {
                        check(false, "deterministic (rerun identical)", reason2);
                    }

                    std::printf("  info groups=%u boundary=%zu tris %u -> %u (%.1f%%)\n",
                                g.groupCount, sr.lockedPoints.size(),
                                sr.total.inputTriangles, sr.total.outputTriangles,
                                100.0 * double(sr.total.outputTriangles) /
                                double(sr.total.inputTriangles));
                }
            }
        }
    }

    // Case 2：单四边形（1 组）——开放边界锁（meshopt LockBorder 语义）
    // 把 4 条外缘端点全部锁死，孤四边形不折叠（out=2）：对角线折叠会同时
    // 杀掉两个三角形，边界折叠被锁拒绝。meshopt LockBorder 对同一输入
    // 同样不折叠。折叠能力由 Case 1 网格覆盖。
    {
        std::printf("[single quad, no boundary]\n");
        nl::core::NMeshData nm;
        std::string reason;
        if (!makeNMesh(makeTestMesh(1, 2, 2), nm, reason)) {
            check(false, "makeNMesh", reason);
        } else {
            nl::core::GroupingResult g;
            if (!nl::core::groupClusters(nm, g, reason)) {
                check(false, "groupClusters", reason);
            } else {
                check(g.groupCount == 1, "single quad -> 1 group");
                nl::core::SimplifyResult sr;
                if (!nl::core::simplifyGroups(nm, g, sr, reason)) {
                    check(false, "simplifyGroups", reason);
                } else {
                    check(sr.lockedPoints.empty(), "no boundary points");
                    check(sr.total.outputTriangles == 2 && sr.total.inputTriangles == 2,
                          "open-border quad stays 2 tris (LockBorder)",
                          "out=" + std::to_string(sr.total.outputTriangles));
                }
            }
        }
    }

    // Case 3：断连双块 —— 无边界点（块间无共享几何点），各组独立简化
    {
        std::printf("[2 disconnected blocks]\n");
        nl::core::NMeshData nm;
        std::string reason;
        if (!makeNMesh(makeTestMesh(2, 32, 32), nm, reason)) {
            check(false, "makeNMesh", reason);
        } else {
            nl::core::GroupingResult g;
            if (!nl::core::groupClusters(nm, g, reason)) {
                check(false, "groupClusters", reason);
            } else {
                nl::core::SimplifyResult sr;
                if (!nl::core::simplifyGroups(nm, g, sr, reason)) {
                    check(false, "simplifyGroups", reason);
                } else {
                    // 块内相邻簇可能分属不同组 → 块内可有边界点（正常）；
                    // 真正要防的是锁定点跨块引用（块间无共享几何点，不可能出现，
                    // 出现即数据坏了）。锁定点坐标必须落在某一块的 x 区间内。
                    bool lockedInRange = true;
                    {
                        std::vector<std::uint32_t> canon;
                        nl::core::weldPositions(nm.vertices, canon);
                        std::vector<std::uint8_t> lockedPoint(~0u, 0);
                        lockedPoint.assign(sr.pointCount, 0);
                        for (std::uint32_t p : sr.lockedPoints) lockedPoint[p] = 1;
                        for (std::size_t i = 0; i < nm.vertices.size() && lockedInRange; ++i)
                            if (lockedPoint[canon[i]]) {
                                const float x = nm.vertices[i].pos[0];
                                if (x > 45.f && x < 95.f) lockedInRange = false;  // 块间空档
                            }
                    }
                    check(lockedInRange, "locked points have per-block positions");
                    check(sr.groupTriCount.size() == g.groupCount &&
                          sr.indices.size() == std::size_t(sr.total.outputTriangles) * 3,
                          "output accounting consistent");
                    const double r = sr.total.inputTriangles
                        ? double(sr.total.outputTriangles) / double(sr.total.inputTriangles)
                        : 0.0;
                    check(r >= 0.30 && r <= 0.60, "output ratio in [30%,60%]",
                          "ratio=" + std::to_string(r));
                }
            }
        }
    }

    // Case 4：负向 —— 非法 ratio / 聚组结果不匹配 / 空网格
    std::printf("[input validation]\n");
    {
        nl::core::NMeshData nm;
        std::string reason;
        if (makeNMesh(makeTestMesh(1, 16, 16), nm, reason)) {
            nl::core::GroupingResult g;
            nl::core::SimplifyResult sr;
            if (nl::core::groupClusters(nm, g, reason)) {
                check(!nl::core::simplifyGroups(nm, g, sr, reason, 0.f),
                      "ratio 0 rejected", reason);
                check(!nl::core::simplifyGroups(nm, g, sr, reason, 1.5f),
                      "ratio >1 rejected", reason);
                nl::core::GroupingResult bogus;
                bogus.clusterGroup.assign(nm.meshlets.size() + 1, 0);
                bogus.groupCount = 1;
                check(!nl::core::simplifyGroups(nm, bogus, sr, reason),
                      "grouping size mismatch rejected", reason);
            } else {
                check(false, "groupClusters (precondition)", reason);
            }
        } else {
            check(false, "makeNMesh (precondition)", reason);
        }
        nl::core::NMeshData empty;
        nl::core::GroupingResult g2;
        nl::core::SimplifyResult sr2;
        check(!nl::core::simplifyGroups(empty, g2, sr2, reason),
              "empty mesh rejected", reason);
    }

    if (failures == 0) {
        std::printf("qem selftest: PASS\n");
        return 0;
    }
    std::printf("qem selftest: FAIL (%d case(s))\n", failures);
    return 1;
}

// ---- selftest loop：逐层 LOD 循环机械单测（M2.2 验收核心）----
// 灵魂断言：上一层（更细 LOD）的接缝点，凡是仍存在于下一层网格里的，
// 必须在下一层继续被锁——否则「相邻层拼接无裂缝」在离线侧就没有保证。
int selftestLoop() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name, const std::string& detail = {}) {
        if (ok) std::printf("  ok   %s\n", name);
        else { std::printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str()); ++failures; }
    };
    using Pos = std::array<float, 3>;
    auto lockSet = [](const std::vector<float>& p) {
        std::set<Pos> s;
        for (std::size_t i = 0; i + 2 < p.size(); i += 3)
            s.insert({ p[i], p[i + 1], p[i + 2] });
        return s;
    };
    // 独立重算某层的组间边界：重新聚组 + 按组归属找被 ≥2 组引用的 canonical 点。
    // 与 core/simplify.cpp 的边界判定同口径、不同代码路径（交叉验证防同错）。
    // 这正是 bevy lock_group_borders「每轮按当前分组全量重算」的语义。
    auto recomputeBoundary = [](const nl::core::NMeshData& mesh, std::string& why) {
        std::set<Pos> s;
        why.clear();
        nl::core::GroupingResult g;
        if (!nl::core::groupClusters(mesh, g, why)) return s;
        std::vector<std::uint32_t> canon;
        nl::core::weldPositions(mesh.vertices, canon);
        std::uint32_t pointCount = 0;
        for (std::uint32_t c : canon) pointCount = std::max(pointCount, c + 1);
        std::vector<int> first(pointCount, -1);
        for (std::size_t c = 0; c < mesh.meshlets.size(); ++c) {
            const auto& m = mesh.meshlets[c];
            const int grp = int(g.clusterGroup[c]);
            for (std::uint32_t i = 0; i < m.vertexCount; ++i) {
                const std::uint32_t p = canon[m.vertexOffset + i];
                const auto& v = mesh.vertices[m.vertexOffset + i];
                if (first[p] < 0) first[p] = grp;
                else if (first[p] != grp) s.insert({ v.pos[0], v.pos[1], v.pos[2] });
            }
        }
        return s;
    };

    std::printf("[connected 64x64 grid, LOD loop]\n");
    std::string reason;
    nl::core::NMeshData nm;
    const bool inOk = makeNMesh(makeTestMesh(1, 64, 64), nm, reason);
    check(inOk, "synthetic .nmesh built", reason);
    if (inOk) {
        nl::core::LodOptions opt;
        opt.maxLevels = 6;
        opt.minTriangles = 64;
        nl::core::LodHierarchy h;
        const bool built = nl::core::buildLodHierarchy(nm, h, reason, opt);
        check(built, "buildLodHierarchy", reason);
        if (built) {
            check(h.levels.size() >= 3, "at least 3 LOD levels");
            bool decreasing = h.levels.size() >= 2, halfOk = true;
            for (std::size_t k = 1; k < h.levels.size(); ++k) {
                const auto& a = h.levels[k - 1];
                const auto& b = h.levels[k];
                if (b.triangles >= a.triangles) decreasing = false;
                const double r = double(b.triangles) / double(a.triangles);
                // 标准做法：每组硬目标 50%；拓扑约束下允许 30~80% 波动。
                // M3.0 起按组重切（bevy:154 / UE ReduceGroup 口径）：父簇必须
                // 整簇产出，组粒度取整让保留率上界从 70% 抬到 ~80%
                // （8 簇/组 × 每组减半后簇数向上取整），bevy/UE 同样存在。
                // 末层例外：开放边界锁（meshopt LockBorder 语义）在小规模
                // 网格上把剩余三角形锁死属自然收敛停滞，比率可更高，
                // 但必须是最后一层且仍在严格递减；中间层超界仍然 FAIL。
                if (k + 1 < h.levels.size() && (r < 0.30 || r > 0.80)) halfOk = false;
            }
            check(decreasing, "triangle count strictly decreases");
            check(halfOk, "each level ~50% of previous (30~80%, last level may stall)");

            bool limits = true, metaOk = true;
            for (std::size_t k = 0; k < h.levels.size(); ++k) {
                const auto& lv = h.levels[k];
                if (!lv.mesh.lodMeta.present || lv.mesh.lodMeta.level != k) metaOk = false;
                for (const auto& m : lv.mesh.meshlets)
                    if (m.triangleCount > nl::core::kMeshletMaxTriangles ||
                        m.vertexCount > nl::core::kMeshletMaxVertices) limits = false;
            }
            check(limits, "meshlet limits respected");
            check(metaOk, "per-level lodMeta present with correct level");

            // 灵魂断言：每层的锁点集合 == 按该层分组独立重算的组间边界集合。
            // 若存在跨层继承，这里会多出历史锁点而立刻不等。
            // 顶层没有更粗层要拼，故只验到 k = n-2。
            bool boundaryOk = true;
            for (std::size_t k = 0; k + 1 < h.levels.size(); ++k) {
                std::string why;
                const auto expected = recomputeBoundary(h.levels[k].mesh, why);
                const auto actual = lockSet(h.levels[k].mesh.lodMeta.lockedPositions);
                if (!why.empty() || expected != actual) boundaryOk = false;
            }
            check(boundaryOk, "locks == independent per-level boundary recompute");
            check(h.levels.back().mesh.lodMeta.lockedPositions.empty(),
                  "top level has no outgoing seams");

            // UE ClusterDAG.cpp:1441 / bevy :146-151：parent error 单调不减，
            // 否则 S3 的 DAG cut 误差判据不成立。
            bool errMono = true;
            for (std::size_t k = 1; k < h.levels.size(); ++k)
                if (h.levels[k].maxError + 1e-6f < h.levels[k - 1].maxError ||
                    std::abs(h.levels[k].mesh.lodMeta.maxError - h.levels[k].maxError) > 1e-6f)
                    errMono = false;
            check(errMono, "parent error monotonic across levels");

            for (const auto& lv : h.levels)
                std::printf("  info L%u tris=%-6u clusters=%-4u locked=%-5u maxErr=%.4f\n",
                            lv.level, lv.triangles, lv.clusters, lv.lockedPoints,
                            lv.maxError);

            nl::core::LodHierarchy h2;
            const bool again = nl::core::buildLodHierarchy(nm, h2, reason, opt);
            bool deterministic = again && h2.levels.size() == h.levels.size();
            if (deterministic)
                for (std::size_t k = 0; k < h.levels.size(); ++k) {
                    const auto& a = h.levels[k];
                    const auto& b = h2.levels[k];
                    if (a.triangles != b.triangles || a.clusters != b.clusters ||
                        a.lockedPoints != b.lockedPoints ||
                        a.mesh.lodMeta.lockedPositions != b.mesh.lodMeta.lockedPositions)
                        deterministic = false;
                }
            check(deterministic, "deterministic (rerun identical)");

            // version 2 落盘读回 + version 1 向后兼容
            const char* tmp2 = "lod-selftest-v2.tmp.nmesh";
            const auto& mid = h.levels[h.levels.size() / 2].mesh;
            bool rt = nl::core::saveNMesh(tmp2, mid, reason);
            nl::core::NMeshData back2;
            if (rt) rt = nl::core::loadNMesh(tmp2, back2, reason);
            if (rt)
                rt = back2.lodMeta.present && back2.lodMeta.level == mid.lodMeta.level &&
                     back2.lodMeta.lockedPositions == mid.lodMeta.lockedPositions &&
                     std::abs(back2.lodMeta.maxError - mid.lodMeta.maxError) < 1e-6f &&
                     back2.meshlets.size() == mid.meshlets.size() &&
                     back2.triangles == mid.triangles;
            check(rt, "v2 .nmesh round-trip keeps lodMeta", reason);
            std::remove(tmp2);

            const char* tmp1 = "lod-selftest-v1.tmp.nmesh";
            nl::core::NMeshData plain = nm;
            plain.lodMeta = nl::core::NMeshLodMeta{};
            bool v1 = nl::core::saveNMesh(tmp1, plain, reason);
            nl::core::NMeshData back1;
            if (v1) v1 = nl::core::loadNMesh(tmp1, back1, reason) && !back1.lodMeta.present;
            check(v1, "v1 .nmesh still readable", reason);
            std::remove(tmp1);
        }
    }

    std::printf("[input validation]\n");
    bool rejected = true;
    {
        const auto bad = [](const nl::core::NMeshData& input, nl::core::LodOptions o) {
            nl::core::LodHierarchy h;
            std::string err;
            return !nl::core::buildLodHierarchy(input, h, err, o) && !err.empty();
        };
        nl::core::LodOptions o;
        o.ratio = 0.f;              rejected &= bad(nm, o);
        o = nl::core::LodOptions{}; o.ratio = 2.f;     rejected &= bad(nm, o);
        o = nl::core::LodOptions{}; o.maxLevels = 0;   rejected &= bad(nm, o);
        o = nl::core::LodOptions{}; o.minGroups = 0;   rejected &= bad(nm, o);
        o = nl::core::LodOptions{}; nl::core::NMeshData empty;
        rejected &= bad(empty, o);
    }
    check(rejected, "ratio / maxLevels / minGroups / empty mesh rejected");

    if (failures == 0) {
        std::printf("loop selftest: PASS\n");
        return 0;
    }
    std::printf("loop selftest: FAIL (%d case(s))\n", failures);
    return 1;
}

// ---- M3.0：dag selftest —— 合成网格上的 DAG 血缘机械单测 ----

int selftestDag() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name, const std::string& detail = {}) {
        if (ok) std::printf("  ok   %s\n", name);
        else { std::printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str()); ++failures; }
    };
    auto toLevels = [](const nl::core::LodHierarchy& h) {
        std::vector<DagLevelData> levels;
        for (const auto& lv : h.levels) {
            DagLevelData d;
            d.mesh = lv.mesh;
            d.clusters = std::uint32_t(lv.mesh.meshlets.size());
            d.triangles = lv.triangles;
            levels.push_back(std::move(d));
        }
        return levels;
    };

    std::printf("[connected 128x128 grid, DAG lineage]\n");
    std::string reason;
    nl::core::NMeshData nm;
    const bool inOk = makeNMesh(makeTestMesh(1, 128, 128), nm, reason);
    check(inOk, "synthetic .nmesh built", reason);
    if (inOk) {
        nl::core::LodOptions opt;
        opt.maxLevels = 6;
        opt.minTriangles = 64;
        nl::core::LodHierarchy h;
        const bool built = nl::core::buildLodHierarchy(nm, h, reason, opt);
        check(built, "buildLodHierarchy", reason);
        if (built) {
            check(h.levels.size() >= 3, "at least 3 LOD levels");

            // 全链不变量：membership / generation / error chain / parent err /
            // monotonic / acyclic，逐层逐组全量（合成网格规模小，不抽样）
            const DagCheckReport rep = validateDagChain(toLevels(h));
            check(rep.membershipOk && rep.generationOk && rep.errorChainOk &&
                  rep.parentErrOk && rep.monotonicOk && rep.acyclicOk,
                  "full-chain DAG invariants hold", rep.firstFailure);
            check(rep.errorChainChecks >= std::size_t(100),
                  "error chain exercised on >=100 groups",
                  "groups=" + std::to_string(rep.groupsTotal));

            // 顶层组 genCount=0（terminal group）
            bool terminalOk = true;
            for (const auto& dg : h.levels.back().mesh.lodMeta.dagGroups)
                if (dg.genCount != 0) terminalOk = false;
            check(terminalOk, "top-level groups are terminal (genCount=0)");

            // 非顶层组全部产出父簇（按组重切：每恰一生成组）
            bool genOk = true;
            for (std::size_t k = 0; k + 1 < h.levels.size(); ++k)
                for (const auto& dg : h.levels[k].mesh.lodMeta.dagGroups)
                    if (dg.genCount == 0) genOk = false;
            check(genOk, "every non-top group generates parents");

            // v3 落盘读回逐字节语义一致：分层全链各自 roundtrip 后再全量校验
            bool rtOk = true;
            std::vector<DagLevelData> reloaded;
            for (std::size_t k = 0; k < h.levels.size(); ++k) {
                const std::string file = "dag-selftest-l" + std::to_string(k) + ".tmp.nmesh";
                if (!nl::core::saveNMesh(file.c_str(), h.levels[k].mesh, reason)) {
                    rtOk = false; break;
                }
                DagLevelData d;
                if (!nl::core::loadNMesh(file.c_str(), d.mesh, reason)) {
                    rtOk = false; break;
                }
                d.clusters = std::uint32_t(d.mesh.meshlets.size());
                d.triangles = 0;
                for (const auto& m : d.mesh.meshlets) d.triangles += m.triangleCount;
                // 血缘字段逐项一致
                const auto& a = h.levels[k].mesh.lodMeta;
                const auto& b = d.mesh.lodMeta;
                if (b.clusterGroup != a.clusterGroup ||
                    b.dagGroups.size() != a.dagGroups.size())
                    rtOk = false;
                for (std::size_t gi = 0; rtOk && gi < a.dagGroups.size(); ++gi) {
                    const auto& ga = a.dagGroups[gi];
                    const auto& gb = b.dagGroups[gi];
                    if (ga.error != gb.error || ga.simplifyError != gb.simplifyError ||
                        ga.genFirst != gb.genFirst || ga.genCount != gb.genCount ||
                        std::memcmp(ga.sphere, gb.sphere, sizeof(ga.sphere)) != 0)
                        rtOk = false;
                }
                std::remove(file.c_str());
                reloaded.push_back(std::move(d));
            }
            check(rtOk, "v3 round-trip keeps DAG lineage");
            if (rtOk) {
                const DagCheckReport rep2 = validateDagChain(reloaded);
                check(rep2.ok(), "reloaded chain re-validates", rep2.firstFailure);
            }

            // 负向：篡改一个组误差 → error chain 精确校验必须 FAIL
            if (!reloaded.empty()) {
                std::vector<DagLevelData> corrupted = reloaded;
                corrupted[0].mesh.lodMeta.dagGroups[0].error *= 1.5f;
                const DagCheckReport rep3 = validateDagChain(corrupted);
                check(!rep3.errorChainOk, "tampered group error detected");
            }
        }
    }

    if (failures == 0) {
        std::printf("dag selftest: PASS\n");
        return 0;
    }
    std::printf("dag selftest: FAIL (%d case(s))\n", failures);
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
        if (argc >= 3 && std::string(argv[2]) == "qem") return selftestQem();
        if (argc >= 3 && std::string(argv[2]) == "loop") return selftestLoop();
        if (argc >= 3 && std::string(argv[2]) == "dag") return selftestDag();
        std::fprintf(stderr, "selftest: unknown suite (available: cluster, nmesh, group, qem, loop, dag)\n");
        return 1;
    }
    if (cmd == "simplify") return cmdSimplify(argc - 2, argv + 2);
    if (cmd == "check-orient") return cmdCheckOrient(argc - 2, argv + 2);
    if (cmd == "lod") return cmdLod(argc - 2, argv + 2);
    if (cmd == "dag") return cmdDag(argc - 2, argv + 2);
    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    printUsage();
    return 1;
}
