#pragma once

// P8 M-N0：寻路集成层（vendored Recast/Detour 1.6.0 之上的 nl/ 薄封装）。
// 纪律（P8-NAV-PLAN §架构约束）：
//   - 本头文件零第三方符号：rc*/dt* 类型全部挡在 core/src/nav/Nav.cpp 内（PIMPL，
//     对齐 nl/physics 把 Jolt 头挡在 cpp 的先例），业务侧只见 POD 与 nl::nav 接口。
//   - 零图形符号（NVRHI/ImGui 零 include）；可视化挂 viewer/debugDraw 与编辑器层。
//   - 确定性硬判据：单线程上游流水线（不引入并行），同几何同参数产物逐字节一致；
//     save/load 走自管容器格式（"NLNV"），tile 字节即 dtCreateNavMeshData 原始产物。
// 上游版本锁 1.6.0（见 thirdparty/recastnavigation/VENDOR.md）。

#include <cstdint>
#include <string>
#include <vector>

namespace nl::nav {

// 上游版本横幅（实现侧返回编译期锁定值，与 VENDOR.md 一致）
const char* recastVersion();

// ---- 烘焙参数（世界单位 / 角度，集成层换算成 rcConfig 体素数；换算公式照
// 上游 Sample_SoloMesh.cpp handleBuild 的 ceilf/floorf 口径，不私改）----
struct BuildParams {
    float cellSize           = 0.30f;  // XZ 体素边长（m）
    float cellHeight         = 0.20f;  // Y 体素边长（m）
    float walkableSlopeAngle = 45.0f;  // 可走坡面角（度）
    float agentHeight        = 2.00f;  // Agent 身高（m）
    float agentRadius        = 0.50f;  // Agent 半径（m，用于腐蚀可走区）
    float agentMaxClimb      = 0.40f;  // 可攀台阶高（m）
    // 区域面积阈值（换算 rcSqr 后为体素格²；默认口径按本项目 2~3 m 测试资产：
    // RecastDemo 的 8/20 假设几十米大地图，小资产可走区会被 minRegionArea 丢光）
    float regionMinSize      = 2.0f;   // 最小区域（m）
    float regionMergeSize    = 6.0f;   // 区域合并阈值（m）
    float edgeMaxLen         = 12.0f;  // 轮廓简化最大边长（m）
    float edgeMaxError       = 1.3f;   // 轮廓简化最大误差（体素单位）
    int   vertsPerPoly       = 6;      // 每多边形最大顶点（<= DT_VERTS_PER_POLYGON）
    float detailSampleDist   = 6.0f;   // 细节网格采样距（m；<0.9 视为关闭，上游口径）
    float detailSampleMaxError = 1.0f; // 细节网格最大误差（m，乘 cellHeight 前为高度口径）
};

// ---- 几何输入：世界空间三角网格（顶点已烘变换；nav-tools 负责从
// .glb / .nmesh fallback 段汇集，本层只吃扁平数组）----
struct Geometry {
    std::vector<float>        verts;  // xyz 连续排布，3 * 顶点数
    std::vector<std::uint32_t> tris;  // 三角形索引，3 * 三角数（指向 verts）
};

// ---- 烘焙统计（dump 交代物；面积 = 多边形 XZ 投影面积和，m^2）----
struct BuildStats {
    int   polyCount    = 0;  // NavMesh 多边形数
    int   vertCount    = 0;  // 多边形顶点数
    int   detailTriCount = 0; // 细节网格三角数
    int   walkableTris = 0;  // 坡度标记为可走的输入三角形数（诊断用）
    int   gridWidth = 0, gridHeight = 0; // 高度场网格尺寸（诊断用）
    int   rasterSpans  = 0;  // 栅格化后（滤波前）可走 span 数（诊断用）
    int   filteredSpans = 0; // 三连滤波后可走 span 数（诊断用）
    int   compactSpans = 0;  // 紧凑高度场 span 数（诊断用）
    int   regionCount  = 0;  // rcBuildRegions 产生的区域数（诊断用）
    float walkableArea = 0.f; // 可走面积（m^2）
    float bmin[3] = {0.f, 0.f, 0.f};  // 几何包围盒
    float bmax[3] = {0.f, 0.f, 0.f};
};

// 序列化容器格式（nl-engine 自管，非上游格式）：
//   "NLNV" magic | u16 version | u16 reserved0
//   dtNavMeshParams 原样落盘：f32 orig[3], f32 tileWidth/tileHeight, i32 maxTiles/maxPolys
//   u32 tileCount；每 tile：u32 dataBytes + 原始 tile 字节（dtCreateNavMeshData 产物）
// 全部小端、无对齐 padding（逐字段 fwrite），同参数产物逐字节可复现（MD5 硬判据）。
inline constexpr char kNavMagic[4] = {'N', 'L', 'N', 'V'};
inline constexpr std::uint16_t kNavVersion = 1;

// ---- NavMesh：bake 产物持有者（PIMPL）----
class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(NavMesh&&) noexcept;
    NavMesh& operator=(NavMesh&&) noexcept;
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // 几何 -> Recast Solo Mesh -> dtNavMesh。失败返回 false 且 outError 带可读原因
    //（rc/dt 每步状态都检查，不吞错）。
    bool bake(const Geometry& geo, const BuildParams& params,
              std::string& outError);
    bool save(const char* path, std::string& outError) const;
    bool load(const char* path, std::string& outError);

    const BuildStats& stats() const { return m_stats; }

    // 多边形边界边（世界空间线段端点对，a*b* 各 xyz）——M-N0 线框 PNG 证据源。
    struct Edge { float a[3]; float b[3]; };
    std::vector<Edge> polyEdges() const;

private:
    struct Impl;
    Impl*      m_impl = nullptr;
    BuildStats m_stats;
};

} // namespace nl::nav
