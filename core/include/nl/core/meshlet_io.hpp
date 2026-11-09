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

// M2.2：LOD 层元数据（可选）。.nmesh version 2 才在尾部写这一段；
// version 1 文件逐字节不变，老 viewer / 回归基线不动。
// lockedPositions 存**世界空间位置**而不是 canonical 点编号——逐层重切后
// 顶点编号全变，位置才是稳定身份（S3 DAG 合并按位置认接缝）。
// 语义对齐 bevy lock_group_borders / UE LockPosition：这里存的是**本层按当前
// 分组重算**的组间边界点，供更粗层拼接用；不累积历史层锁点，顶层为空。
struct NMeshLodMeta {
    bool present = false;
    std::uint16_t level = 0;      // 0 = 最细原始层
    // parent error：沿层级单调不减（UE ClusterDAG.cpp:1441 / bevy :146-151）。
    float maxError = 0.f;
    std::vector<float> lockedPositions;  // 3 float/点，排序去重由生产者保证

    // ---- M3.0：DAG 血缘（meta version 2，只加不改）----
    // 三家口径对齐（2026-11-07 实读 UE ClusterDAG.cpp / bevy from_mesh.rs /
    // Carrot fertilizer + zeux clusterlod.h）：
    //   组 = DAG 父节点（UE FClusterGroup.Children / bevy TempMeshletGroup）；
    //   **按组重切，父簇绝不跨组**（UE ReduceGroup 组内 Merged+SplitCluster、
    //   bevy 每组独立 compute_meshlets）→ 每个父簇恰有一个生成组，S5 的
    //   组原子 cut（弹堆时同误差兄弟一起出堆）才成立；
    //   组 error = max(组简化误差, 全部成员簇误差)（UE :1441+:1587），
    //   且父簇 bounds.error 统一覆写为组 error（UE :1593 "They are all
    //   dependent"）；顶层组 genCount=0（zeux clusterlod 的 terminal group）。
    bool dagPresent = false;
    // 本层的组（DAG 父节点）。genFirst/genCount = 本组简化重切出的父簇区间，
    // 指**下一层文件**（.lodN+1.nmesh）的簇号；顶层组 genCount=0。
    struct DagGroup {
        float error = 0.f;         // ParentLODError = max(simplifyError, 成员簇误差)
        float simplifyError = 0.f; // 本次组内 QEM 简化误差（顶层组为 0）
        float sphere[4];           // 成员簇包围球并（LODBounds 基础，运行时 + error 膨胀）
        std::uint32_t genFirst = 0;
        std::uint32_t genCount = 0;
    };
    std::vector<DagGroup> dagGroups;
    // 本层每簇的组号（成员方向；组记录索引，与 dagGroups 一一对应）。
    // 生成组方向（下一层簇 → 本层组号）由 genFirst/genCount 区间反转，不冗余存。
    std::vector<std::uint32_t> clusterGroup;
};

// .nmesh 自包含数据。vertices 的顺序就是 meshlet.vertexOffset/vertexCount
// 窗口所指的数组——即 BuiltMeshlets::vertices 经 remap 后的实际顶点内容。
struct NMeshData {
    std::string name;                     // 子网格名（ASCII，UTF-8 安全但约定 ASCII）
    std::vector<Meshlet> meshlets;
    std::vector<MeshletBounds> bounds;    // 与 meshlets 一一对应
    std::vector<MeshVertex> vertices;     // 自包含（不引用外部网格）
    std::vector<std::uint8_t> triangles;  // 每三角 3 个局部顶点索引（0..63）
    NMeshLodMeta lodMeta;                 // M2.2：有 present 时落盘为 version 2

    // ---- M3.1：fallback mesh（可选，文件 version 3 尾段）----
    // 详见 fallback.hpp。落点约定 = <prefix>.lod0.nmesh（lod 命令在链写完
    // 后拼最顶层写回）；S8 BLAS / 非 Nanite 兜底渲染直接吃，不追 DAG。
    // 落盘独立于 lodMeta：v1 base 文件也可以只加 fallback 尾段（version 3）。
    bool fallbackPresent = false;
    std::uint32_t fallbackSourceLevel = 0; // 生成 fallback 的层号
    float fallbackMaxError = 0.f;          // 来源层 parent error（世界空间）
    std::vector<MeshVertex> fallbackVertices;   // weld 后无跨簇重复
    std::vector<std::uint32_t> fallbackIndices; // 3 × 三角数

    // ---- M7.2：多子网格（可选，version 3 机制下的 'NSUB' 尾段）----
    // D-M7.2-1 拍板（2026-11-10，方案 A）：材质归属构建期随簇记录——簇按子
    // 网格序拼接，本段记每子网格的簇区间，材质 id = 子网格序号（renderer 建
    // clusterMeta 时填槽 3）。各子网格独立走完 cluster（M1.1 语义）后拼接；
    // simplify/lod/dag 链不消费多子网格文件（visbuf 光栅不吃 DAG）。
    struct SubmeshRange {
        std::uint32_t clusterFirst;   // 首簇号（全局簇 id）
        std::uint32_t clusterCount;   // 簇数
        bool operator==(const SubmeshRange& o) const {
            return clusterFirst == o.clusterFirst && clusterCount == o.clusterCount;
        }
    };
    bool submeshesPresent = false;
    std::vector<SubmeshRange> submeshes;   // 单子网格文件不写此段（缺省单段）
};

// ---- 二进制格式（little-endian）----
//
//   NMeshHeader        固定 20 B（version 1 / 2 / 3）
//   name               nameLen 字节 ASCII
//   NMeshletRecord[meshletCount]   每个 52 B
//   MeshVertex[vertexCount]        每个 32 B（float x9，直接内存映像）
//   uint8[triangleCount * 3]
//   [version 2+ only] NMeshMetaHeader 24 B + payload
//     meta version 1: float[3*lockedCount]                    （M2.2，仅锁点）
//     meta version 2: float[3*lockedCount]                    （M3.0，锁点 + DAG）
//                     + NMeshDagGroupRecord[groupCount] 每个 32 B
//                     + u32 clusterGroup[meshletCount]
//   [version 3 only] 尾段按 magic 识别，顺序不敏感，至少一段（M3.1）：
//     NMeshMetaHeader（可选）或 NMeshFallbackHeader（可选）或
//     NMeshSubmeshHeader（可选，M7.2 多子网格，簇按子网格序拼接）：
//       NMeshFallbackHeader 24 B
//       MeshVertex[vertexCount] + u32[indexCount]
//       NMeshSubmeshHeader 16 B
//       NMeshSubmeshRecord[submeshCount] 每个 8 B
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

// version 3 = version 2 语义 + 尾部可选 fallback section（只加不改，
// loadNMesh 1/2/3 都读；v3 尾段按 magic 识别，顺序不敏感，至少一段）。
struct NMeshMetaHeader {
    std::uint32_t magic;          // 'N','L','O','D' → 0x444F4C4E (LE)
    std::uint32_t payloadBytes;   // 跟随的 payload 字节数
    std::uint16_t version;        // 1 = 仅锁点 / 2 = 锁点 + DAG 血缘（M3.0）
    std::uint16_t level;          // LOD 层号
    float maxError;               // 世界空间
    std::uint32_t lockedCount;    // payload 前 3*lockedCount 字节 = 锁点 float
    std::uint32_t reserved;       // 置 0
};

// meta version 2 的 DAG 组记录（M3.0）。32 B 紧凑，紧跟锁点 payload 之后：
//   DagGroupRecord[groupCount]
//   u32 clusterGroup[meshletCount]   （组号，引用上面的记录索引）
struct NMeshDagGroupRecord {
    float error;
    float simplifyError;
    float sphere[4];
    std::uint32_t genFirst;
    std::uint32_t genCount;
};

// M3.1 fallback 尾段头（24 B，紧跟文件主体；S8 BLAS 用，不追 DAG）。
// payload = MeshVertex[vertexCount] + u32[indexCount]。
struct NMeshFallbackHeader {
    std::uint32_t magic;          // 'N','F','L','B' → 0x424C464E (LE)
    std::uint32_t payloadBytes;   // vertexCount*32 + indexCount*4
    std::uint32_t vertexCount;
    std::uint32_t indexCount;     // = 三角数 ×3
    std::uint32_t sourceLevel;    // 生成 fallback 的层号（链顶层）
    float maxError;               // 来源层 parent error（世界空间）
};

// M7.2 多子网格尾段头（16 B，version 3 机制；材质归属随簇记录，D-M7.2-1 方案 A）。
// payload = NMeshSubmeshRecord[submeshCount]，簇按子网格序拼接。
struct NMeshSubmeshHeader {
    std::uint32_t magic;          // 'N','S','U','B' → 0x4255534E (LE)
    std::uint32_t payloadBytes;   // = submeshCount * sizeof(NMeshSubmeshRecord)
    std::uint32_t submeshCount;
    std::uint32_t reserved;       // 置 0
};
// 每子网格 8 B。约定：区间从 0 起连续无缝（拼接不变量），loadNMesh 强校验。
struct NMeshSubmeshRecord {
    std::uint32_t clusterFirst;
    std::uint32_t clusterCount;
};

// ---- P9 / V4：分页友好页表尾段（DECIDED 2026-11-08，待 V1 落地）----
// 设计：v4 = v3 + 强制 NPAG 页表尾段。meshlet 记录布局零改动（vertexOffset 等
// 仍绝对指向共享 vertices/triangles 数组）；离线 page-builder 把顶点/索引重排成
// 页连续、顶点闭合的 slab，页表记录每页的 meshlet 区间与顶点/索引 slab 区间。
// 流式（V4）按页 slab 的固定偏移拷入同一块大 GPU buffer，meshlet 偏移永不 patch
// （对标 UE5 FStreamOutMeshDataHeader）。证据见 docs/p9/NMESH-PAGING-DRAFT.md §3。
// 当前仅声明结构 + 锁布局；saveNMesh/loadNMesh 的 v4 读写在 V1 实现（§8）。
// 字段布局与 §4.2 草案一致；页表头原草案误写为 20B，此处按 static_assert(==24) 修正。
struct NMeshPageTableHeader {
    std::uint32_t magic;          // 'N','P','A','G' → 0x4741504E (LE)
    std::uint32_t payloadBytes;   // = pageCount * sizeof(NMeshPageRecord)
    std::uint32_t pageCount;      // 页数（u32，避 16-bit 上限）
    std::uint16_t version;        // 1
    std::uint16_t reserved0;      // 0
    std::uint32_t reserved1;      // 0
    std::uint32_t reserved2;      // 0
};

// 每页 32 B，镜像 NMeshDagGroupRecord 尺寸。所有偏移为绝对偏移（磁盘 = GPU buffer）。
struct NMeshPageRecord {
    std::uint32_t firstMeshlet;    // 本页首 meshlet 号
    std::uint32_t meshletCount;    // 本页 meshlet 数
    std::uint32_t vertexOffset;    // 顶点 slab 基（绝对，= 磁盘 & GPU 同一偏移）
    std::uint32_t vertexCount;     // 顶点 slab 长度
    std::uint32_t triangleOffset;  // 索引 slab 基（BYTE 偏移，绝对）
    std::uint32_t triangleCount;   // 索引 slab 长度（三角数）
    std::uint32_t flags;           // bit0 = 本页含跨页复制的边界顶点
    std::uint32_t reserved;        // 0
};

inline constexpr std::uint16_t kNMeshVersion = 1;
inline constexpr std::uint16_t kNMeshVersionLod = 2;
inline constexpr std::uint16_t kNMeshVersionFallback = 3;  // + fallback 尾段（M3.1）
inline constexpr std::uint16_t kNMeshVersionPage = 4;      // + 强制 NPAG 页表尾段（P9/V4，待落地）
inline constexpr std::uint16_t kNMeshMetaVersion = 1;     // 仅锁点
inline constexpr std::uint16_t kNMeshMetaVersionDag = 2;  // 锁点 + DAG 血缘（M3.0）
inline constexpr std::uint32_t kNMeshMagic = 0x48534D4Eu;     // "NMSH"
inline constexpr std::uint32_t kNMeshLodMagic = 0x444F4C4Eu;  // "NLOD"
inline constexpr std::uint32_t kNMeshFallbackMagic = 0x424C464Eu;  // "NFLB"
inline constexpr std::uint32_t kNMeshSubmeshMagic = 0x4255534Eu;   // "NSUB"（M7.2 多子网格）
inline constexpr std::uint32_t kNMeshPageMagic = 0x4741504Eu;     // "NPAG"（P9/V4，待落地）

static_assert(sizeof(NMeshHeader) == 20, "NMeshHeader layout drift");
static_assert(sizeof(NMeshletRecord) == 52, "NMeshletRecord layout drift");
static_assert(sizeof(NMeshMetaHeader) == 24, "NMeshMetaHeader layout drift");
static_assert(sizeof(NMeshDagGroupRecord) == 32, "NMeshDagGroupRecord layout drift");
static_assert(sizeof(NMeshFallbackHeader) == 24, "NMeshFallbackHeader layout drift");
static_assert(sizeof(NMeshSubmeshHeader) == 16, "NMeshSubmeshHeader layout drift");
static_assert(sizeof(NMeshSubmeshRecord) == 8, "NMeshSubmeshRecord layout drift");
static_assert(sizeof(MeshVertex) == 32, "MeshVertex layout drift");
static_assert(sizeof(NMeshPageTableHeader) == 24, "NMeshPageTableHeader layout drift");
static_assert(sizeof(NMeshPageRecord) == 32, "NMeshPageRecord layout drift");

// 写盘。失败返回 false，outError 带可读原因；成功时不留部分文件（先写临时再改名）。
bool saveNMesh(const char* path, const NMeshData& data, std::string& outError);

// 读盘。version 不认识 / magic 不对 / 截断 / 窗口越界都算失败。
// 读回后校验每簇窗口与三角局部索引的合法性（防手改文件导致越界读）。
bool loadNMesh(const char* path, NMeshData& out, std::string& outError);

} // namespace nl::core
