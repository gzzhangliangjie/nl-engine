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

// 几何汇集：.glb -> loadGltfScene 全 primitive 展平（世界变换已烘进顶点）。
// viewer 与 nav-tools 共用（M-N2 抽出；.nmesh fallback 分支留在 nav-tools，
// viewer 的 --nav-panel 面板烘焙只走 glb）。零第三方符号入头文件。
bool loadGeometryFromGltf(const char* path, Geometry& out, std::string& outError);

// M-N3：E1 场景几何汇集 —— nl::scene World JSON（.scene）→ Geometry。
// 吃 ModelComponent.meshFile + TransformComponent 世界矩阵（含父子层级复合），
// meshDir 是 glb 资产目录（如 ${NL_ROOT}/assets/models）；meshFile 白名单同
// viewer --scene（basename + .glb，拒路径穿越）。按 mesh 缓存避免重复加载。
int  geometryEntityCount(const char* scenePath, std::string& outError);
bool loadGeometryFromScene(const char* scenePath, const char* meshDir,
                           Geometry& out, std::string& outError);

// ---- M-N2：五阶段调试可视化（上游 DebugUtils duDebugDraw 口径）----
// 落地形态（P8-NAV-PLAN 拍板语义内）：nl/nav 只产出 POD 图元流，消费端两处——
// nav-tools debug-stage 软光栅 PNG（证据图取工具真实输出）+ viewer NavPanel
// ImDrawList 画布。本层零图形符号纪律不破。
struct DebugPrims {
    // 图元类型（duDebugDrawPrimitives 值域镜像，头文件不引入第三方符号）
    enum Prim { POINTS = 0, LINES = 1, TRIS = 2, QUADS = 3 };
    struct Group {
        int  prim      = 0;   // Prim
        float size     = 1.f; // 点径/线宽（俯视光栅消费）
        int  vertBegin = 0;   // [vertBegin, vertEnd) 顶点区间
        int  vertEnd   = 0;
    };
    struct Vtx {
        float        pos[3];      // 世界空间
        std::uint32_t col;        // duRGBA 布局：r | g<<8 | b<<16 | a<<24
    };
    std::vector<Group> groups;
    std::vector<Vtx>   verts;
};

// 五阶段枚举（对应上游绘制函数；RecastDemo NavMeshSample 调参常看的五个 +
// 距离场 / 原始轮廓两个诊断阶段）。名字即 cvar/CLI 值。
enum class DebugStage {
    Voxels      = 0,  // 高度场体素（滤波后紧凑高度场 solid 视图）
    Regions     = 1,  // 区域划分（紧凑高度场 region 着色）
    Distance    = 2,  // 距离场（到边界距离热图）
    Contours    = 3,  // 简化轮廓（rcBuildContours 产物）
    RawContours = 4,  // 原始轮廓（简化前）
    PolyMesh    = 5,  // 多边形网格（contour -> convex polys）
    NavMesh     = 6,  // 最终 dtNavMesh（Detour 多边形着色）
};
const char* debugStageName(DebugStage s);       // cvar/CLI 字符串
bool debugStageFromName(const char* name, DebugStage& out);  // 解析（小写）

// 序列化容器格式（nl-engine 自管，非上游格式）：
//   "NLNV" magic | u16 version | u16 reserved0
//   dtNavMeshParams 原样落盘：f32 orig[3], f32 tileWidth/tileHeight, i32 maxTiles/maxPolys
//   u32 tileCount；每 tile：u32 dataBytes + 原始 tile 字节（dtCreateNavMeshData 产物）
// 全部小端、无对齐 padding（逐字段 fwrite），同参数产物逐字节可复现（MD5 硬判据）。
// v1 = Solo Mesh（M-N0）；**v2 = tiled（M-N3 拍板 A-1）**：基础段布局完全同 v1
// （version 字段=2），尾部追加 tiled 扩展段：tcparams/buildParams 参数 +
// dtTileCache compressed tile 数据（每个含 tx/ty/tlayer）。loader 按 version
// 分流：v1 走原路径（老基线字节零回归）；v2 走 tiled 重建（动态障碍能力保留）。
inline constexpr char kNavMagic[4] = {'N', 'L', 'N', 'V'};
inline constexpr std::uint16_t kNavVersion = 1;       // Solo Mesh（M-N0 起）
inline constexpr std::uint16_t kNavVersionTiled = 2;  // TileCache tiled（M-N3 起）
// **v3 = Solo Mesh + off-mesh connection 描述段（M-N5 起）**：基础段布局与 v1
// 完全一致（version 字段=3），尾部追加连接描述；v1/v2 分支一行不改。
inline constexpr std::uint16_t kNavVersionOffMesh = 3;
// offmesh poly 独占 flag 位（对齐 UE ARecastNavMesh::NavLinkFlag 的最高位口径），
// 供 QueryParams::excludeOffMesh 用 dtQueryFilter::setExcludeFlags 排除。
inline constexpr std::uint16_t kOffMeshExcludeFlag = 0x8000;

// ---- M-N3：tiled 烘焙参数（方案 A / A-1 拍板）----
// 复用 BuildParams 的换算口径（cellSize/agent* 等），新增上游 tileSize（体素/边）
// 与障碍池上限。tileSize=48 是上游 RecastDemo Sample_TempObstacles 默认值。
struct TileBuildParams {
    BuildParams base;
    int  tileSize     = 48;   // 每 tile 边长（体素数）
    int  maxObstacles = 128;  // 动态障碍池上限（dtTileCacheParams）
};

// ---- M-N5：off-mesh connection（烘焙期注入，上游 1.6.0 无运行时 add API）----
// 语义照上游 dtNavMeshCreateParams.offMeshCon*（DetourNavMeshBuilder.h:54-76）：
// start=A / end=B，0 = 单向 A→B，bidirectional = 双向；radius 决定端点与
// NavMesh poly 建立链接的搜索半径；flags 是 poly 通行标记，注入时自动 OR 上
// kOffMeshExcludeFlag 以支持查询过滤；area/userId 原样交给上游。
// 端点必须落在 tile bbox 内：上游 builder 的 classifyOffMeshPoint 对 bbox 外的
// 连接直接丢弃（0xff），封装层在 bake 前显式校验并报错，不静默吞。
struct OffMeshConnection {
    float start[3] = {0.f, 0.f, 0.f};
    float end[3]   = {0.f, 0.f, 0.f};
    float radius   = 0.5f;
    bool  bidirectional = true;
    std::uint16_t flags = 1;       // 默认可走（bake 侧普通 poly 亦为 1）
    std::uint8_t  area  = 1;       // RC_WALKABLE_AREA
    std::uint32_t userId = 0;
};

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
    // keepDebug（M-N2）：保留五阶段中间产物（rc 高度场/区域/轮廓/多边形网格），
    // 供 debugDrawStage 消费；内存开销 = 中间对象常驻（小场景可接受）。默认 false
    // 与 M-N0 行为逐字节一致。
    bool bake(const Geometry& geo, const BuildParams& params,
              std::string& outError, bool keepDebug = false);
    // M-N5：带 off-mesh 连接表的烘焙重载。传空表 = 与上方 bake 逐字节一致
    // （dparams.offMeshCon* 全 0，version 仍写 v1）。只支持 Solo Mesh 路径；
    // tiled 连接注入属另一条链，本轮不做（见 PLAN Q5-3）。
    bool bake(const Geometry& geo, const BuildParams& params,
              const std::vector<OffMeshConnection>& links,
              std::string& outError, bool keepDebug = false);
    bool save(const char* path, std::string& outError) const;
    bool load(const char* path, std::string& outError);

    // M-N5：连接描述（bake 传入 / v3 load 读回）；offMeshPolyCount() 返回
    // dtMeshHeader::offMeshConCount 之和（上游 builder 实际保留下来的条数，
    // 可与上面描述条数不同——端点落 bbox 外会被丢弃）。
    const std::vector<OffMeshConnection>& offMeshConnections() const;
    int  offMeshPolyCount() const;

    const BuildStats& stats() const { return m_stats; }

    // 多边形边界边（世界空间线段端点对，a*b* 各 xyz）——M-N0 线框 PNG 证据源。
    struct Edge { float a[3]; float b[3]; };
    std::vector<Edge> polyEdges() const;

    // ---- M-N4：opaque 原生 dtNavMesh 句柄 ----
    // 仅供 core/src/nav/Crowd.cpp 内部桥接（A-1：Crowd 只吃已加载 NavMesh，
    // .nnav 不升 v3）。返回 const void* 是为了让公开面保持零第三方符号：
    // 业务侧不得解引用、不得跨 NavMesh 重建/析构保存；未装载时返回 nullptr。
    const void* nativeHandle() const;

    // ---- M-N1：Detour 查询封装 ----
    // 链路：findNearestPoly（端点吸附）-> findPath（多边形走廊）-> findStraightPath
    //（string-pulling 折线，即本层语义上的「路径平滑」；上游 RecastDemo/DetourCrowd
    // 亦只到此为止，不引入 Catmull-Rom 类额外平滑）。
    struct QueryParams {
        float halfExtents[3] = {2.0f, 1.0f, 2.0f}; // 端点吸附搜索盒半宽（m）
        int   maxNodes       = 4096;  // 节点预算（<=65535；硬闸下限 4——上游
                                      //   dtNextPow2(maxNodes/4) 在 <4 时为 0，
                                      //   hash 池 0 字节分配 → getNode 越界，M-N1 实测）
        int   maxPathPolys   = 256;   // 多边形走廊缓冲
        int   maxWaypoints   = 256;   // 折线点缓冲
        // M-N5：排除 off-mesh 连接 poly（对齐 UE FilterOutNavLinksImpl 的独占
        // flag 位）。默认 false = 放行，M-N1~M-N4 行为逐点不变。
        bool  excludeOffMesh = false;
    };

    // 明确错误码（计划负向语义：不可达 / 超出节点预算各占一态，不静默折算）。
    // 上游 1.6.0 实测口径（DetourNavMeshQuery.cpp findPath 973-1165 行）：
    //   不可达（open list 耗尽）= DT_SUCCESS|DT_PARTIAL_RESULT（非 FAILURE）
    //   节点池分配失败          = 追加 DT_OUT_OF_NODES detail（可与 partial 并存）
    //   缓冲不足                = DT_SUCCESS|DT_BUFFER_TOO_SMALL（截断填充）
    enum class QueryStatus {
        OK               = 0, // 直达终点
        UNREACHABLE      = 1, // 端点不在 NavMesh 上，或搜索耗尽不可达
        OUT_OF_NODES     = 2, // 节点预算耗尽（partial，best-effort 折线仍输出）
        BUFFER_TOO_SMALL = 3, // 输出缓冲不足（路径截断填充）
        NOT_INITIALIZED  = 4, // NavMesh 未 bake/load
        INVALID_PARAM    = 5, // 参数非法（maxNodes 越界 / 非有限坐标等）
    };
    static const char* queryStatusName(QueryStatus s);

    struct QueryResult {
        QueryStatus status = QueryStatus::NOT_INITIALIZED;
        bool reachedLimit = false; // 路径完整但邻居展开受节点池限制（OUT_OF_NODES 无 partial）
        std::vector<float> waypoints; // xyz 连续；UNREACHABLE/OUT_OF_NODES 时为
                                      // best-effort 前缀（到最近可达点）
        // M-N5：与 waypoints 一一对应的 dtStraightPathFlags（上游 0x04 =
        // DT_STRAIGHTPATH_OFFMESH_CONNECTION，该点是跳跃段起点）。无连接时
        // 全 0 向量保持尺寸 = waypoints/3，老路径语义不变。
        std::vector<std::uint8_t> waypointFlags;
    };

    // 完整查询链。结果状态与 out.status 一致；确定性：同 mesh 同参数同端点，
    // waypoints 逐点一致（上游 A* 即确定性单线程）。
    QueryStatus findPath(const float start[3], const float end[3],
                         const QueryParams& params, QueryResult& out) const;

    // ---- M-N2：五阶段图元导出 ----
    // 从 bake(keepDebug=true) 保留的中间产物收集图元；Voxels/Regions/Distance/
    // Contours/RawContours/PolyMesh 消费 rc 中间对象，NavMesh 消费 dtNavMesh
    //（load 路径无中间产物，只有 NavMesh 阶段可用）。输出为 POD 图元流，
    // 消费端自行投影/光栅/绘制。非本层语义失败（未保留中间产物 / 阶段不可用）
    // 返回 false 且 outError 带可读原因。
    bool debugDrawStage(DebugStage stage, DebugPrims& out,
                        std::string& outError) const;

    // ---- M-N3：tiled 增量重烘 + 动态障碍（方案 A / A-1）----
    // bakeTiled：几何 -> 逐 tile rasterizeTileLayers -> dtTileCache compressed tile
    // -> dtNavMesh tiled tiles（上游 Sample_TempObstacles 主流程口径）。与 Solo
    // Mesh bake 是两条独立路径；save 写 v2，load 按 version 分流。
    bool bakeTiled(const Geometry& geo, const TileBuildParams& params,
                   std::string& outError);
    bool isTiled() const;
    int  compressedTileCount() const;  // dtTileCache 内有效 compressed tile 数
    int  obstacleCount() const;        // 当前已处理的动态障碍数
    // 圆柱障碍：addObstacle 立即触发 update 到 upToDate（只重建受影响 tile）；
    // removeObstacle 同样。ref 供移除用。查询直接消费更新后的 dtNavMesh。
    bool addObstacle(const float pos[3], float radius, float height,
                     std::uint32_t& outRef, std::string& outError);
    bool removeObstacle(std::uint32_t ref, std::string& outError);
    bool updateObstacles(std::string& outError);

private:
    struct Impl;
    Impl*      m_impl = nullptr;
    BuildStats m_stats;
};

} // namespace nl::nav
