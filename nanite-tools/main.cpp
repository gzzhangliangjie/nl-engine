// M1.0：nanite-tools —— S1 起的离线网格处理工具。
// 子命令规划（S1-PLAN）：cluster（M1.1 切簇）/ simplify（S2 重切）/
// dag（S3 误差树）/ stat（本里程碑实现）。
// 只依赖 core（日志 + mesh_io），零图形 API 符号——与 viewer 的纪律一致。

#include "nl/core/mesh_io.hpp"

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
        "  nanite-tools cluster <model> [options]    meshlet build (M1.1, not yet)\n"
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "stat") return cmdStat(argc - 2, argv + 2);
    if (cmd == "cluster" || cmd == "simplify" || cmd == "dag") {
        std::fprintf(stderr, "%s: not implemented yet (S1 M1.1+ / S2 / S3)\n", cmd.c_str());
        return 2;
    }
    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    printUsage();
    return 1;
}
