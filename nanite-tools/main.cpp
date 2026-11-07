// M1.0：nanite-tools —— S1 起的离线网格处理工具。
// 子命令规划（S1-PLAN）：cluster（M1.1 切簇）/ simplify（S2 重切）/
// dag（S3 误差树）/ stat（M1.0 实现）/ selftest（M1.1 起的机械单测）。
// 只依赖 core（日志 + mesh_io + meshlet），零图形 API 符号——与 viewer 的纪律一致。

#include "nl/core/mesh_io.hpp"
#include "nl/core/meshlet.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::printf(
        "nanite-tools - nl-engine offline mesh pipeline (S1)\n"
        "usage:\n"
        "  nanite-tools stat <model.gltf|.glb>       vertex/tri/submesh summary\n"
        "  nanite-tools cluster <model> [options]    meshlet build (M1.1)\n"
        "  nanite-tools selftest cluster             deterministic unit tests\n"
        "  nanite-tools simplify <model> [options]   cluster re-cut (S2, not yet)\n"
        "  nanite-tools dag <model> [options]        error DAG (S3, not yet)\n");
}

// stat：<path> 的顶点 / 三角 / 子网格汇总 + 每子网格明细 + 世界空间包围盒。
// S1 切簇的前置事实核对：变换烘焙是否生效、三角形规模是否与已知口径一致。
int cmdStat(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "stat: missing <model.gltf|.glb>\n");
        return 1;
    }
    const char* path = argv[0];

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

int cmdCluster(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "cluster: missing <model.gltf|.glb>\n");
        return 1;
    }
    const char* path = argv[0];
    const bool verbose = argc >= 2 && std::string(argv[1]) == "-v";

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
    for (size_t i = 0; i < data.meshes.size(); ++i) {
        nl::core::BuiltMeshlets built;
        std::string reason;
        if (!clusterPrimitive(data.meshes[i], built, reason)) {
            std::fprintf(stderr, "cluster: [%zu] %s\n", i, reason.c_str());
            ++failures;
            continue;
        }
        if (!printClusterResult(path, data.meshes[i], built, verbose)) ++failures;
        if (i + 1 < data.meshes.size()) std::printf("\n");
    }
    return failures == 0 ? 0 : 1;
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "stat") return cmdStat(argc - 2, argv + 2);
    if (cmd == "cluster") return cmdCluster(argc - 2, argv + 2);
    if (cmd == "selftest") {
        if (argc >= 3 && std::string(argv[2]) == "cluster") return selftestCluster();
        std::fprintf(stderr, "selftest: unknown suite (available: cluster)\n");
        return 1;
    }
    if (cmd == "simplify" || cmd == "dag") {
        std::fprintf(stderr, "%s: not implemented yet (S2 / S3)\n", cmd.c_str());
        return 2;
    }
    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    printUsage();
    return 1;
}
