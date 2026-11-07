#pragma once

// M2.2：逐层 LOD 循环（严格对齐 Nanite 系标准做法，不自造机制）。
//
//   L0（输入 .nmesh）→ 重新聚组 → 按当前分组重算边界锁 → 每组硬目标 50%
//   简化 → buildMeshlets 重切成新簇 → L1 → 再重新聚组 → …
//
// 源码对照（2026-11-07 实读本机三处）：
//   bevy from_mesh.rs:87-195   每轮 group_meshlets 重新分组；
//     :114-120 lock_group_borders 每轮开头重算边界锁，:520-523 对 vertex_locks
//     全量覆写（含置 false）→ 上一轮锁不继承；
//     :556 每组硬目标 group_indices.len()/2；:154 重切 compute_meshlets。
//   UE ClusterDAG.cpp:1067+   逐层 ReduceMesh；:1470 TargetNumTris =
//     NumParents * TargetClusterSize 为硬目标。
//   UE Cluster.cpp:1023-1039  LockPosition 只作用于当前 group 的 ExternalEdges
//     端点；:1080-1091 简化后按位置对重建外部边标记。
//   meshoptimizer simplifier.cpp:2492  while (result_count > target_index_count)
//     反复折叠到达标；:526-529 SimplifyLockBorder 把 border 改判 Kind_Locked。
//
// 因此本实现不做「跨层累积锁定」：每层 boundary 只由该层 groupClusters 的
// 组归属重算（在 simplifyGroups 内完成），历史层锁点一律不带过来。跨层无裂缝
// 由 DAG cut 以 group 为原子单位保证（S3），不靠把接缝点一直背下去。
// 误差按 UE ClusterDAG.cpp:1441 / bevy :146-151 口径单调化：
//   parentError(k+1) = max(parentError(k), 本次简化误差)，S3 cut 才成立。
//
// 每层 NMeshData 自带 lodMeta（level / maxError / lockedPositions），落盘即
// .nmesh version 2；lockedPositions = 本层为更粗层保留的组间边界点（顶层为空）。
// 纪律：core 零图形 API、零文件 IO（写盘在 nanite-tools 的 lod 子命令）。

#include "nl/core/meshlet_io.hpp"
#include "nl/core/simplify.hpp"    // kSimplifyDefaultRatio

#include <cstdint>
#include <string>
#include <vector>

namespace nl::core {

struct LodOptions {
    float ratio = kSimplifyDefaultRatio;   // 每组硬目标三角数比例（bevy :556 的 50%）
    std::uint32_t maxLevels = 8;           // 最多生成多少个简化层（不含 L0）
    std::uint32_t minTriangles = 128;      // 当前层三角数低于此值即收敛
    std::uint32_t minGroups = 1;           // 聚组数低于此值即收敛
    // 终止条件只用「三角数/组数/不再下降/maxLevels」——bevy :94 的
    // while !simplification_queue.is_empty() 同义。不设「简化率不足就提前停」
    // 的自造门槛：某层简化不足时，下一次重新分组会换一组边界，仍能继续。
};

struct LodLevel {
    std::uint32_t level = 0;               // 0 = 最细原始层
    NMeshData mesh;                        // 自带 lodMeta（version 2）
    std::uint32_t triangles = 0;
    std::uint32_t clusters = 0;
    std::uint32_t groups = 0;              // 该层重新聚组后的组数
    std::uint32_t lockedPoints = 0;        // 本层按当前分组重算的组间边界点数
    std::uint32_t collapses = 0;           // 生成下一层时的折叠数（顶层 0）
    float simplifyError = 0.f;             // 本次简化引入的最大误差（世界空间）
    float maxError = 0.f;                  // parent error，沿层级单调不减
};

struct LodHierarchy {
    std::vector<LodLevel> levels;          // levels[0] = 输入网格副本
};

// 失败（参数非法 / 聚组或简化失败 / 重切失败）返回 false，outError 带可读原因。
// 确定性：METIS 固定 seed + QEM 有序集队列，同输入同输出。
bool buildLodHierarchy(const NMeshData& input, LodHierarchy& out,
                       std::string& outError,
                       const LodOptions& options = LodOptions{});

} // namespace nl::core
