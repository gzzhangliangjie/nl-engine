#pragma once

// M3.1：fallback mesh —— 链最顶层整体拼成一份普通 indexed mesh。
//
// 用途（S3-PLAN 底线：这步定稿，S8 直接读，不许到 S8 再「顺手生成一份」）：
//   - 光追 BLAS（S8）直接吃 vertices + indices，不追 cluster DAG；
//   - 「Nanite 关」时的兜底渲染路径（UE 同款语义）。
//
// UE 口径对照（2026-11-07 实读 NaniteBuilder.cpp BuildFallbackMesh :769+
// 与 BuildCoarseRepresentation :415）：UE 的 fallback 是从 ClusterDAG 独立
// 简化到 FallbackPercentTriangles（默认 ~2% 输入三角数）目标的普通 indexed
// mesh（顶点 + 三角索引 + material section）。本实现取**链最顶层**整体拼接：
//   - 顶层已有 parent error 单调链背书（S3.0），误差可接受口径由分层链给出；
//   - Suzanne 104/3936 = 2.6%，与 UE 默认 ~2% 同量级；
//   - 不引入第二次独立简化——同一条链、同一份确定性，无第二口径可漂移。
//
// 拼接 = 顶层各簇按位置 weld（weldPositions 同口径：float 位级相等）合并成
// 单一 indexed mesh。组间边界点在简化时锁死（M2.1 边界锁），同一几何点在
// 相邻簇里是同一 float 的拷贝，位级相等成立 → 拼接面无接缝无洞。
// 顶点属性（normal/uv）取 canonical 点首次出现的那份——边界点两侧本就是
// 同一源顶点的拷贝，属性一致。
//
// 落盘：.nmesh 文件 version 3 尾段（NMeshFallbackHeader，'NFLB'），见
// meshlet_io.hpp 格式注释。落点约定 = <prefix>.lod0.nmesh（lod 命令写回）。
// 纪律：core 零图形 API 零文件 IO（写盘在 nanite-tools 的 lod 子命令）。

#include "nl/core/meshlet_io.hpp"   // NMeshData / MeshVertex

#include <cstdint>
#include <string>
#include <vector>

namespace nl::core {

struct FallbackMesh {
    std::vector<MeshVertex> vertices;    // weld 后的独立顶点（无跨簇重复）
    std::vector<std::uint32_t> indices;  // 3 × 三角数，指向 vertices
    std::uint32_t sourceLevel = 0;       // 生成它的层号（= 链顶层）
    float maxError = 0.f;                // 来源层 parent error（世界空间）
};

// 从 NMeshData（约定 = 链最顶层）拼接 fallback。
// 失败返回 false 且 outError 带可读原因（退化三角 / 空网格 / 索引越界）。
bool buildFallbackMesh(const NMeshData& top, FallbackMesh& out, std::string& outError);

} // namespace nl::core
