// M2.2：逐层 LOD 循环实现。标准做法见 lod.hpp 的源码对照，不引入自造机制。

#include "nl/core/lod.hpp"

#include "nl/core/grouping.hpp"
#include "nl/core/meshlet.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

// 简化输出的 canonical 三角 soup → 重新切簇 → 自包含 NMeshData。
// 对应 bevy from_mesh.rs:154 compute_meshlets / UE 简化后 Split 父簇。
// 顶点法线**继承**简化器的 canonical 点法线（= 本层输入法线和）：QEM 折叠
// 是位置并入、不创造新位置，法线场跨层不变 = L0 原始场，与 check-orient
// 的参照（输入法线和）同口径，逐层审计不漂移。若按邻面重算，法线随层
// 几何漂移，粗层桥接凹折痕会被误读成朝内（度量伪影）。UV 置零。
bool rebuildMeshlets(const SimplifyResult& sr, NMeshData& out, std::string& outError) {
    out = NMeshData{};
    if (sr.indices.empty() || sr.pointCount == 0) {
        outError = "lod: simplified soup is empty";
        return false;
    }
    BuiltMeshlets built;
    if (!buildMeshlets(sr.pointPositions.data(), 3, sr.pointCount,
                       sr.indices.data(), sr.indices.size(), built)) {
        outError = "lod: buildMeshlets rejected simplified soup";
        return false;
    }
    if (built.meshlets.empty()) {
        outError = "lod: recluster produced no meshlets";
        return false;
    }
    out.meshlets = built.meshlets;
    out.bounds = built.bounds;
    out.triangles = built.triangles;
    out.vertices.resize(built.vertices.size());
    for (std::size_t i = 0; i < built.vertices.size(); ++i) {
        MeshVertex v{};
        const std::uint32_t p = built.vertices[i];
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
        out.levels.push_back(lv);

        if (level >= options.maxLevels) break;
        if (lv.triangles < options.minTriangles) break;
        if (g.groupCount < options.minGroups) break;

        // ---- 每组硬目标 ratio；不传任何历史层锁点 ----
        SimplifyResult sr;
        if (!simplifyGroups(current, g, sr, outError, options.ratio)) return false;
        if (sr.total.outputTriangles >= lv.triangles) break;   // 无法再下降 = 自然收敛

        NMeshData next;
        if (!rebuildMeshlets(sr, next, outError)) return false;
        next.name = (input.name.empty() ? std::string("lod") : input.name) +
                    ".lod" + std::to_string(level + 1);
        next.lodMeta.present = true;
        next.lodMeta.level = std::uint16_t(level + 1);
        // UE ClusterDAG.cpp:1441 / bevy from_mesh.rs:146-151：父层误差不小于子层。
        next.lodMeta.maxError = std::max(current.lodMeta.maxError, sr.total.maxError);

        // 本层对更粗层的接缝承诺 = 本次简化按当前分组锁住的边界点。
        // 顶层（没有生成更粗层）不写 lockedPositions，语义自洽。
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
