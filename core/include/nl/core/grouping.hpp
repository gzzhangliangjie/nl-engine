#pragma once

// M2.0：簇聚组（S2 离线管线第一步）。相邻簇（共享几何顶点，按位置 weld、
// 忽略 UV 缝——与 M1.1 切簇邻接同一口径）通过 METIS 图分割聚成组。
// 组是 S2 组内 QEM 简化的单元（组边界顶点锁定 = 无裂缝的唯一保证），
// 也是 S3 DAG 合并的原料。
// 依赖：METIS（vendored thirdparty/metis）。core 里唯一碰 METIS 符号的文件；
// METIS 本身零图形 API，core 纪律不破。

#include "nl/core/meshlet_io.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nl::core {

// 组大小目标（簇数/组）：bevy 0.15 实证值——4→8 后组内简化质量显著提升
// （Nanite 用 8~32）。S2-PLAN 旧稿「~50000 tri/组」是 UE 的 page 口径而非
// group，已修订。S3 若发现合并需求再调，只许改这一个数。
inline constexpr std::uint32_t kTargetMeshletsPerGroup = 8;

// 聚组结果。clusterGroup 与 mesh.meshlets 一一对应，组号 0..groupCount-1 连续。
struct GroupingResult {
    std::vector<std::uint32_t> clusterGroup;
    std::uint32_t groupCount = 0;
    // 统计（simplify 打印 / selftest 断言用）
    std::uint32_t maxGroupSize = 0;  // 最大组含簇数
    std::uint32_t splitFixups = 0;   // METIS 输出不连通被拆分的组次数
};

// 聚组。mesh 为 .nmesh 读回数据（loadNMesh 产物）。位置完全相等（位级比较，
// float 精确相等）的顶点视为同一几何点。确定性：METIS 固定 seed，同输入同输出。
// 失败（空簇 / 图异常 / METIS 内部错误）返回 false 且 outError 带可读原因。
bool groupClusters(const NMeshData& mesh, GroupingResult& out, std::string& outError);

} // namespace nl::core
