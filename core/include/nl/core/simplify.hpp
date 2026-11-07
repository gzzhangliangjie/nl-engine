#pragma once

// M2.1：组内 QEM 简化 + 边界锁定（S2 灵魂里程碑）。
//
// 灵魂机制（没有第二个魔法）：组间边界顶点锁死——永不移动、永不作为折叠
// 端点消失，且双锁定端点的边（跨组接缝边）不会被第三方折叠摧毁。这是
// 「同一层内相邻组无裂缝」的唯一保证。S2-PLAN 的坑位提醒：边界判定看**组归属**
// （一个 canonical 点被 ≥2 个组的簇引用 = 边界），组内两簇之间的共享点
// 不是边界，锁了会过度锁定、简化率上不去。
//
// 标准口径对齐（2026-11-07 核过源码，不自造机制）：
//   meshoptimizer src/simplifier.cpp:526-529  meshopt_SimplifyLockBorder 把
//     border/fringe 整体改判 Kind_Locked —— 锁点不能作折叠源、只能作目标，
//     双锁定边不折叠；与本实现一致。
//   bevy from_mesh.rs:114-120,520-523  lock_group_borders 在每一轮循环开头
//     对 vertex_locks 全量覆写 —— 边界锁按**当前轮分组**重算，不跨层继承。
//   UE Cluster.cpp:1023-1039          LockPosition 只对当前 group 的
//     ExternalEdges 端点生效，简化后按位置对重建标记（1080-1091）。
// 跨层无裂缝由 DAG cut 以 group 为原子单位保证，不靠把历史锁点一直背下去。
//
// QEM（Garland & Heckbert）：每点累积邻域三角形的面积加权平面二次型，
// 边折叠队列按折叠后误差升序；折叠位置取二次型最优点（退化时回退端点/
// 中点）。误差单位 = 世界空间长度（sqrt 二次型值），与 M1.1 叶子误差同口径。
// 退化保护：零面积三角形不贡献平面；共面/细长由面积权重自然 clamp。
//
// 纪律：core/ 零图形 API 符号；本文件不碰 METIS（聚组在 grouping.cpp）、
// 不碰 meshoptimizer（对照 oracle 在 nanite-tools/main.cpp）。meshoptimizer
// 的 simplify 只当**对照 oracle**（同参数 meshopt_SimplifyLockBorder），
// 我们的 QEM 才是正式实现。

#include "nl/core/grouping.hpp"    // NMeshData / GroupingResult / weldPositions

#include <cstdint>
#include <string>
#include <vector>

namespace nl::core {

// 目标三角数比例（每组）：S2-PLAN 约定减半。
inline constexpr float kSimplifyDefaultRatio = 0.5f;

// 单组简化统计。
struct GroupSimplifyStats {
    std::uint32_t inputTriangles = 0;
    std::uint32_t outputTriangles = 0;
    std::uint32_t collapses = 0;
    float maxError = 0.f;   // 折叠引入的最大几何误差（世界空间长度）
};

struct SimplifyResult {
    // 简化后三角形 soup：canonical 点编号（weldPositions 口径），所有组
    // 首尾相接；组 g 的三角形从 indices[groupFirstTri[g]*3] 开始。
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> groupFirstTri;   // 每组三角形起始序号
    std::vector<std::uint32_t> groupTriCount;   // 每组三角形数
    // canonical 点位置（简化后；边界点 = 原位置位级不变；3*pointCount）。
    // 未被任何三角形引用的点保留原位置。
    std::vector<float> pointPositions;
    std::uint32_t pointCount = 0;
    // 组间边界 canonical 点集合（排序去重）。S3 DAG 合并还要用——
    // 该集合必须持久化（随 M2.2 的分层 .nmesh 落盘），不许只活在内存里。
    std::vector<std::uint32_t> lockedPoints;
    // 与 lockedPoints 一一对应的世界空间位置（3 float/点，顺序一致）。
    // 存位置而非点号：重切后 canonical 编号全变，S3 按位置认接缝。
    std::vector<float> lockedPositions;
    std::vector<GroupSimplifyStats> perGroup;   // 与组号一一对应
    GroupSimplifyStats total;
};

// 聚组后逐组 QEM 简化。grouping 必须是对同一 mesh 的 groupClusters 产物。
// targetRatio ∈ (0,1]：每组折叠到 round(inputTris * ratio) 即停（拓扑约束
// 下可能提前停住，输出三角数 >= 目标）。确定性：同输入同输出（折叠队列
// 有序集，代价平局按点编号决胜）。
// 边界锁只由本次传入的 grouping 决定（同层组归属），不接收任何历史层锁点：
// 逐层循环每层重新聚组、重新调用本函数即可（bevy lock_group_borders 口径）。
// 失败（参数非法 / 聚组结果与网格不符）返回 false 且 outError 带可读原因。
bool simplifyGroups(const NMeshData& mesh, const GroupingResult& grouping,
                    SimplifyResult& out, std::string& outError,
                    float targetRatio = kSimplifyDefaultRatio);

// 把 .nmesh 的簇展开成 canonical 点三角形 soup（weld + 每簇三角按序展开，
// 重复点的退化三角剔除）。返回 canonical 点数；outIndices 每三角 3 个
// canonical 点编号，簇序展开。simplify 内部、selftest、meshopt oracle 共用。
std::uint32_t buildCanonicalTriangles(const NMeshData& mesh,
                                      std::vector<std::uint32_t>& outIndices);

} // namespace nl::core
