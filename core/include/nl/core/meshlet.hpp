#pragma once

// M1.1：meshlet 贪心构建（切簇）。算法骨架来自 meshoptimizer clusterizer.cpp
// 的 meshopt_buildMeshletsFlex（拓扑优先级 + 空间评分贪心吸收），本工程裁剪版：
// 不带 kd-tree（断连组件用线性扫描兜底）、不带 split_factor（S2 再说）。
// 纪律：core/ 零图形 API 符号；误差单位从第一天起就是世界空间长度（S3 DAG
// 传播依赖同一单位，改口径必须同步 S1-PLAN/SHADER-CONVENTIONS）。

#include <cstdint>
#include <vector>

namespace nl::core {

// 切簇上限常量：全工程唯一出处。GPU 侧（S4 起的 cluster 顶点属性上限）、
// .nmesh 资产格式（M1.2）、本算法共用这三个数，不许各抄一份。
inline constexpr std::uint32_t kMeshletMaxTriangles = 128;
inline constexpr std::uint32_t kMeshletMaxVertices = 64;

// 一个 meshlet = 共享数组上的一个窗口（偏移 + 数量）。
struct Meshlet {
    std::uint32_t vertexOffset;    // 进 BuiltMeshlets::vertices
    std::uint32_t vertexCount;     // 去重后的簇内顶点数，<= kMeshletMaxVertices
    std::uint32_t triangleOffset;  // 进 BuiltMeshlets::triangles 的【字节】偏移
                                   // （每三角 3 字节；不是三角号，按三角号 ×3
                                   // 会读错窗口——M3.1 已踩，字节语义全仓唯一口径）
    std::uint32_t triangleCount;   // <= kMeshletMaxTriangles
};

// 包围体 + 叶子误差。全部世界空间（M1.0 起节点变换已烘进顶点）。
struct MeshletBounds {
    float sphere[4];   // 包围球 center xyz + radius（Ritter 近似，两趟扫描）
    float coneAxis[3]; // 法线锥轴（单位向量；全簇退化时为 (0,0,1)）
    float coneCutoff;  // cos(锥半角) = min(dot(三角法线, 轴))；-1 = 法线杂乱/退化
    float error;       // 叶子误差 = 簇内最长边长（世界空间）。S3 起被 DAG 节点误差覆盖
};

// 构建结果。vertices / triangles 是簇共享的紧凑数组（GPU 直接可吃的布局雏形，
// M1.2 原样序列化进 .nmesh）。
struct BuiltMeshlets {
    std::vector<Meshlet> meshlets;
    std::vector<std::uint32_t> vertices;  // 全局顶点索引（簇内去重）
    std::vector<std::uint8_t> triangles;  // 每三角 3 个局部顶点索引（0..63）
    std::vector<MeshletBounds> bounds;    // 与 meshlets 一一对应
};

// 贪心切簇。positions 按 positionStrideFloats 个 float 一个顶点寻址
// （MeshVertex 传 8——pos 在结构体头部；紧凑 float[3] 数组传 3）。
// indexCount 必须是 3 的倍数；索引必须 < vertexCount。maxVertices <= 256。
// 失败（参数非法）返回 false 且 out 不写入。
bool buildMeshlets(const float* positions, std::size_t positionStrideFloats,
                   std::size_t vertexCount,
                   const std::uint32_t* indices, std::size_t indexCount,
                   BuiltMeshlets& out,
                   std::uint32_t maxTriangles = kMeshletMaxTriangles,
                   std::uint32_t maxVertices = kMeshletMaxVertices,
                   float coneWeight = 0.5f);

} // namespace nl::core
