#pragma once

// M1.2：.nmesh 二进制资产读写。切簇结果（M1.1 的 BuiltMeshlets）落盘成
// 自包含资产：簇窗口 + 包围体 + 顶点数据 + 三角局部索引，运行时（M1.3 起
// viewer，S4 起 GPU）直接吃，不再依赖 glTF 原文件。
// 纪律：core/ 零图形 API 符号；格式只加字段不改旧字段（version 递增）。

#include "nl/core/mesh_io.hpp"    // MeshVertex（32B，与 GPU 输入布局一致）
#include "nl/core/meshlet.hpp"    // Meshlet / MeshletBounds

#include <cstdint>
#include <string>
#include <vector>

namespace nl::core {

// .nmesh 自包含数据。vertices 的顺序就是 meshlet.vertexOffset/vertexCount
// 窗口所指的数组——即 BuiltMeshlets::vertices 经 remap 后的实际顶点内容。
struct NMeshData {
    std::string name;                     // 子网格名（ASCII，UTF-8 安全但约定 ASCII）
    std::vector<Meshlet> meshlets;
    std::vector<MeshletBounds> bounds;    // 与 meshlets 一一对应
    std::vector<MeshVertex> vertices;     // 自包含（不引用外部网格）
    std::vector<std::uint8_t> triangles;  // 每三角 3 个局部顶点索引（0..63）
};

// ---- 二进制格式（version 1，little-endian）----
//
//   NMeshHeader        固定 20 B
//   name               nameLen 字节 ASCII
//   NMeshletRecord[meshletCount]   每个 52 B
//   MeshVertex[vertexCount]        每个 32 B（float x9，直接内存映像）
//   uint8[triangleCount * 3]
//
// 全部紧密排列、无填充；字段布局用 static_assert 锁死，漂移编译期就爆。
struct NMeshHeader {
    std::uint32_t magic;          // 'N','M','S','H' → 0x48534D4E (LE)
    std::uint16_t version;        // 当前 1
    std::uint16_t nameLen;        // 子网格名字节数（可 0）
    std::uint32_t meshletCount;
    std::uint32_t vertexCount;
    std::uint32_t triangleCount;  // 三角形个数（字节数 = ×3）
};
struct NMeshletRecord {
    std::uint32_t vertexOffset;
    std::uint32_t vertexCount;
    std::uint32_t triangleOffset;
    std::uint32_t triangleCount;
    float sphere[4];
    float coneAxis[3];
    float coneCutoff;
    float error;
};

inline constexpr std::uint16_t kNMeshVersion = 1;
inline constexpr std::uint32_t kNMeshMagic = 0x48534D4Eu;  // "NMSH"

static_assert(sizeof(NMeshHeader) == 20, "NMeshHeader layout drift");
static_assert(sizeof(NMeshletRecord) == 52, "NMeshletRecord layout drift");
static_assert(sizeof(MeshVertex) == 32, "MeshVertex layout drift");

// 写盘。失败返回 false，outError 带可读原因；成功时不留部分文件（先写临时再改名）。
bool saveNMesh(const char* path, const NMeshData& data, std::string& outError);

// 读盘。version 不认识 / magic 不对 / 截断 / 窗口越界都算失败。
// 读回后校验每簇窗口与三角局部索引的合法性（防手改文件导致越界读）。
bool loadNMesh(const char* path, NMeshData& out, std::string& outError);

} // namespace nl::core
