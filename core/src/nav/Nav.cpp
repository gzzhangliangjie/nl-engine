// P8 M-N0：nl/nav 集成层实现。vendored Recast/Detour 符号只出现在本 TU
//（nl/nav PIMPL 纪律，对齐 core/src/physics/World.cpp 把 Jolt 头挡在 cpp 的先例）。
// 烘焙流水线照上游 RecastDemo Sample_SoloMesh.cpp handleBuild 的步骤与换算口径
//（只抄用法不抄代码，P8-NAV-PLAN 三处口径表），逐步检查 rc/dt 返回状态。
// 确定性：全程单线程（上游 rcBuild* 即单线程），同几何同参数产物逐字节一致。

#include "nl/nav/Nav.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"
// M-N3：DetourTileCache（同 v1.6.0 锁定 commit，VENDOR.md 溯源）。
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"
#include "DetourAlloc.h"
#include "DetourCommon.h"
// M-N2：五阶段可视化（上游 duDebugDraw 图元流；收集器实现在本 TU 匿名命名空间）。
// 注意：DetourDebugDraw.h 的实现 TU 被剔除入库（1.6.0 头依赖 DetourTileCacheBuilder.h，
// M-N0 CMake 留痕），NavMesh 阶段自绘（本文件 debugDrawStage NavMesh 分支）。
#include "DebugDraw.h"
#include "RecastDebugDraw.h"

#include <algorithm>
#include <utility>

namespace nl::nav {

const char* recastVersion() {
    // VENDOR.md 锁定 v1.6.0；上游未导出版本宏（RecastVersion 仅在 CMake 侧），
    // 这里硬编码并靠 selftest 校验关键符号存在性兜底。
    return "1.6.0";
}

namespace {

// 静默 context：上游 rcContext 默认关日志，错误信息由我们按返回值自己生成。
class SilentContext final : public rcContext {
public:
    SilentContext() = default;
};

float vmin3(float a, float b) { return a < b ? a : b; }
float vmax3(float a, float b) { return a > b ? a : b; }

// XZ 平面 shoelace 面积（绝对值），用于可走面积统计
float polyAreaXZ(const float* verts, const unsigned short* idx, int n) {
    float area = 0.f;
    for (int j = 0; j < n; ++j) {
        const float* a = &verts[idx[j] * 3];
        const float* b = &verts[(idx[(j + 1) % n]) * 3];
        area += a[0] * b[2] - b[0] * a[2];
    }
    return std::fabs(area) * 0.5f;
}

// ---- M-N3：dtTileCache 三个注入接口 ----
// 口径照上游 RecastDemo/Sample_TempObstacles.cpp（LinearAllocator /
// FastLZCompressor / MeshProcess），但只抄接口语义不抄实现：
//   - LinearAllocator：dtTileCache::buildNavMeshTile 开头会 m_talloc->reset()，
//     所以单块 arena + bump 即够；free() 空操作由 reset 统一回收。
//     容量 1 MiB（上游 32 KiB 对应 48³ tile，留足余量），16 字节对齐。
//   - 直通 compressor：4 字节长度头 + 原样字节（上游用 FastLZ，我们不入库
//     该第三方，压缩率不是本里程碑判据；零新依赖优先）。
//   - MeshProcess：把可走 area 标 polyFlags=1；障碍挖空区 RC_NULL_AREA(0)
//     保持 flag=0 → 默认 dtQueryFilter 排除 = 「洞」，查询绕行/不可达。
class TileLinearAllocator final : public dtTileCacheAlloc {
public:
    explicit TileLinearAllocator(std::size_t cap)
        : m_buf(static_cast<unsigned char*>(dtAlloc(cap, DT_ALLOC_PERM))),
          m_cap(cap) {}
    ~TileLinearAllocator() override { if (m_buf) dtFree(m_buf); }
    void reset() override { m_top = 0; }
    void* alloc(const std::size_t size) override {
        const std::size_t aligned = (size + 15u) & ~static_cast<std::size_t>(15u);
        if (!m_buf || aligned > m_cap - m_top) return nullptr;
        void* p = m_buf + m_top;
        m_top += aligned;
        return p;
    }
    void free(void*) override {}
private:
    unsigned char* m_buf = nullptr;
    std::size_t    m_cap = 0;
    std::size_t    m_top = 0;
};

class TilePassthroughCompressor final : public dtTileCacheCompressor {
public:
    int maxCompressedSize(const int bufferSize) override {
        return bufferSize + 4;
    }
    dtStatus compress(const unsigned char* buffer, const int bufferSize,
                      unsigned char* compressed, const int maxSize,
                      int* compressedSize) override {
        if (bufferSize < 0 || maxSize < bufferSize + 4) return DT_FAILURE;
        const std::uint32_t n = static_cast<std::uint32_t>(bufferSize);
        std::memcpy(compressed, &n, 4);
        if (n) std::memcpy(compressed + 4, buffer, n);
        *compressedSize = bufferSize + 4;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* compressed, const int compressedSize,
                        unsigned char* buffer, const int maxBufferSize,
                        int* bufferSize) override {
        if (!compressed || compressedSize < 4) return DT_FAILURE;
        std::uint32_t n = 0;
        std::memcpy(&n, compressed, 4);
        if (static_cast<int>(n) > maxBufferSize ||
            compressedSize - 4 < static_cast<int>(n)) return DT_FAILURE;
        if (n) std::memcpy(buffer, compressed + 4, n);
        *bufferSize = static_cast<int>(n);
        return DT_SUCCESS;
    }
};

class TileWalkableMeshProcess final : public dtTileCacheMeshProcess {
public:
    void process(dtNavMeshCreateParams* params, unsigned char* polyAreas,
                 unsigned short* polyFlags) override {
        for (int i = 0; i < params->polyCount; ++i)
            polyFlags[i] = (polyAreas[i] == RC_NULL_AREA) ? 0 : 1;
    }
};

// tiled / v2 load 共用：从 dtNavMesh tiles 重算统计（bbox 由调用方给几何盒）。
BuildStats statsFromMesh(const dtNavMesh* mesh,
                         const float gmin[3], const float gmax[3]) {
    BuildStats s;
    for (int k = 0; k < 3; ++k) { s.bmin[k] = gmin[k]; s.bmax[k] = gmax[k]; }
    for (int i = 0; i < mesh->getMaxTiles(); ++i) {
        const dtMeshTile* t = mesh->getTile(i);
        if (!t || !t->header) continue;
        s.polyCount      += t->header->polyCount;
        s.vertCount      += t->header->vertCount;
        s.detailTriCount += t->header->detailTriCount;
        for (int p = 0; p < t->header->polyCount; ++p)
            s.walkableArea +=
                polyAreaXZ(t->verts, t->polys[p].verts, t->polys[p].vertCount);
    }
    return s;
}

// v2 扩展段逐字段读写（禁整块 memcpy struct：padding 字节会破坏确定性）
template <class T> bool wrScalar(std::FILE* f, const T& v) {
    return std::fwrite(&v, sizeof(T), 1, f) == 1;
}
template <class T> bool rdScalar(std::FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
}

// v2 扩展段内容（save 写 / load 读）
struct TiledExt {
    float gmin[3] = {0.f, 0.f, 0.f};
    float gmax[3] = {0.f, 0.f, 0.f};
    dtTileCacheParams tcp{};
    TileBuildParams  tp;
    struct Tile {
        int tx = 0, ty = 0, tlayer = 0;
        std::vector<unsigned char> data;
    };
    std::vector<Tile> tiles;
};

} // namespace

struct NavMesh::Impl {
    dtNavMesh* mesh = nullptr;
    // 1.6.0 API 事实：getTile(int) const 是公开版，非 const 重载是 private ——
    // 读侧一律走 constMesh() 命中公开重载（写侧只有 addTile/init，已封装）。
    const dtNavMesh* constMesh() const { return mesh; }
    // load 路径的 tile 字节由 Impl 持有（addTile 不传 DT_TILE_FREE_DATA）。
    // 铁律：detour 的 dtFree 走 malloc 语义，vector 的 new 内存不能移交，
    // 否则析构时分配器不匹配 + UAF（M-N0 实测：selftest roundtrip 字节差 +
    // resave 确定性 0xC0000409 同此根因）。bake 路径不受影响：dtCreateNavMeshData
    // 的产物本来就是 dtAlloc 的，init(..., DT_TILE_FREE_DATA) 移交合法。
    std::vector<std::vector<unsigned char>> ownedTiles;
    // M-N1：查询对象缓存（init 的 maxNodes 是节点预算，变了才重建）。
    dtNavMeshQuery* query = nullptr;
    int queryMaxNodes = 0;
    // M-N2：bake(keepDebug=true) 保留的五阶段中间产物（上游 rc 对象，rcFree
    // 归还；bake false 路径与 M-N0 逐字节一致——对象照旧当场释放）。
    rcCompactHeightfield* dbgChf   = nullptr;  // Voxels/Regions/Distance
    rcContourSet*         dbgCset  = nullptr;  // Contours/RawContours
    rcPolyMesh*           dbgPmesh = nullptr;  // PolyMesh
    rcPolyMeshDetail*     dbgDmesh = nullptr;  // （保留待 M-N3 detail 视图）
    // ---- M-N3：tiled 状态 ----
    // 三注入对象存活期覆盖 tileCache（dtTileCache 只持裸指针不接管所有权）。
    // releaseTiled 顺序 = cache 先死（内部 dtFree compressed tile data）→ 注入对象。
    dtTileCache*               tileCache = nullptr;
    TileLinearAllocator*       talloc    = nullptr;
    TilePassthroughCompressor* tcomp     = nullptr;
    TileWalkableMeshProcess*   tmproc    = nullptr;
    bool                       isTiled   = false;
    TileBuildParams            tileParams;      // v2 扩展段落盘（重建参数）
    dtTileCacheParams          tcParams{};      // 上游参数原样（v2 落盘）
    dtNavMeshParams            meshParams{};    // tiled dtNavMesh 参数（v2 落盘）
    void releaseTiled() {
        if (tileCache) { dtFreeTileCache(tileCache); tileCache = nullptr; }
        delete talloc; talloc = nullptr;
        delete tcomp;  tcomp  = nullptr;
        delete tmproc; tmproc = nullptr;
        isTiled = false;
    }
    ~Impl() {
        releaseTiled();
        if (query) dtFreeNavMeshQuery(query);
        if (dbgChf)   rcFreeCompactHeightfield(dbgChf);
        if (dbgCset)  rcFreeContourSet(dbgCset);
        if (dbgPmesh) rcFreePolyMesh(dbgPmesh);
        if (dbgDmesh) rcFreePolyMeshDetail(dbgDmesh);
    }
};

// M-N4：Crowd.cpp 的 opaque 桥接点。公开面只有 void*，dtNavMesh 类型仍只在本 TU
// 与 Crowd.cpp 内出现；句柄生命周期严格跟随 NavMesh（A-1 不落盘、不跨重建保存）。
const void* NavMesh::nativeHandle() const {
    return m_impl ? static_cast<const void*>(m_impl->mesh) : nullptr;
}

NavMesh::NavMesh() : m_impl(new Impl()) {}
NavMesh::~NavMesh() {
    if (m_impl) {
        if (m_impl->mesh) dtFreeNavMesh(m_impl->mesh);
        delete m_impl;
    }
}
NavMesh::NavMesh(NavMesh&& o) noexcept
    : m_impl(o.m_impl), m_stats(o.m_stats) {
    o.m_impl = nullptr;
}
NavMesh& NavMesh::operator=(NavMesh&& o) noexcept {
    if (this != &o) {
        if (m_impl && m_impl->mesh) dtFreeNavMesh(m_impl->mesh);
        delete m_impl;
        m_impl   = o.m_impl;
        m_stats  = o.m_stats;
        o.m_impl = nullptr;
    }
    return *this;
}

bool NavMesh::bake(const Geometry& geo, const BuildParams& bp,
                   std::string& outError, bool keepDebug) {
    outError.clear();
    m_stats = BuildStats{};

    const int nverts = static_cast<int>(geo.verts.size() / 3);
    const int ntris  = static_cast<int>(geo.tris.size() / 3);
    if (nverts <= 0 || ntris <= 0 || geo.verts.size() % 3 != 0 ||
        geo.tris.size() % 3 != 0) {
        outError = "geometry empty or not multiple of 3 (verts="
                 + std::to_string(geo.verts.size())
                 + " floats, tris=" + std::to_string(geo.tris.size())
                 + " indices)";
        return false;
    }

    // 几何包围盒（统计口径 = 输入几何，与 NavMesh tile 盒分开）
    float gmin[3] = {geo.verts[0], geo.verts[1], geo.verts[2]};
    float gmax[3] = {geo.verts[0], geo.verts[1], geo.verts[2]};
    for (int i = 1; i < nverts; ++i) {
        for (int k = 0; k < 3; ++k) {
            const float v = geo.verts[i * 3 + k];
            gmin[k] = vmin3(gmin[k], v);
            gmax[k] = vmax3(gmax[k], v);
        }
    }
    for (int k = 0; k < 3; ++k) {
        m_stats.bmin[k] = gmin[k];
        m_stats.bmax[k] = gmax[k];
    }

    SilentContext ctx;

    // ---- rcConfig：换算公式照 Sample_SoloMesh.cpp handleBuild（393-412 行口径）----
    rcConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cs                   = bp.cellSize;
    cfg.ch                   = bp.cellHeight;
    cfg.walkableSlopeAngle   = bp.walkableSlopeAngle;
    cfg.walkableHeight       = static_cast<int>(std::ceilf(bp.agentHeight / cfg.ch));
    cfg.walkableClimb        = static_cast<int>(std::floorf(bp.agentMaxClimb / cfg.ch));
    cfg.walkableRadius       = static_cast<int>(std::ceilf(bp.agentRadius / cfg.cs));
    cfg.maxEdgeLen           = static_cast<int>(bp.edgeMaxLen / bp.cellSize);
    cfg.maxSimplificationError = bp.edgeMaxError;
    cfg.minRegionArea        = static_cast<int>(rcSqr(bp.regionMinSize));
    cfg.mergeRegionArea      = static_cast<int>(rcSqr(bp.regionMergeSize));
    cfg.maxVertsPerPoly      = bp.vertsPerPoly;
    cfg.detailSampleDist     = bp.detailSampleDist < 0.9f ? 0.f
                                  : bp.cellSize * bp.detailSampleDist;
    cfg.detailSampleMaxError = bp.cellHeight * bp.detailSampleMaxError;
    rcVcopy(cfg.bmin, gmin);
    rcVcopy(cfg.bmax, gmax);
    // Recast 网格 XZ 轴向与 glTF 轴向差一个轴序：上游用法是交换 bmin/bmax 的
    // y/z 与宽高（Sample_SoloMesh 同样交由几何自身坐标；此处几何已是渲染坐标，
    // Recast 内部按 bmin/bmax 的 x/z 平铺网格，y 只作高度轴。glTF 顶点 y 向上，
    // 与 Recast 约定一致，无需换轴。）
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    m_stats.gridWidth  = cfg.width;
    m_stats.gridHeight = cfg.height;

    if (cfg.width <= 0 || cfg.height <= 0) {
        outError = "grid size degenerate (width=" + std::to_string(cfg.width)
                 + " height=" + std::to_string(cfg.height) + ")";
        return false;
    }
    // Solo mesh 体素量护栏（M-N3 才引入 tiling）：4096^2 已是极端值
    if (static_cast<long long>(cfg.width) * cfg.height > 4096LL * 4096LL) {
        outError = "grid too large for solo mesh (" + std::to_string(cfg.width)
                 + "x" + std::to_string(cfg.height)
                 + "); increase cellSize or wait for tiled bake (M-N3)";
        return false;
    }

    // ---- 高度场 + 栅格化 ----
    rcHeightfield* solid = rcAllocHeightfield();
    if (!solid) { outError = "rcAllocHeightfield failed"; return false; }
    if (!rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin,
                             cfg.bmax, cfg.cs, cfg.ch)) {
        outError = "rcCreateHeightfield failed";
        rcFreeHeightField(solid);
        return false;
    }

    std::vector<unsigned char> triareas(static_cast<size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, geo.verts.data(),
                            nverts, reinterpret_cast<const int*>(geo.tris.data()),
                            ntris, triareas.data());
    for (int i = 0; i < ntris; ++i)
        if (triareas[i] == RC_WALKABLE_AREA) ++m_stats.walkableTris;
    if (!rcRasterizeTriangles(&ctx, geo.verts.data(), nverts,
                              reinterpret_cast<const int*>(geo.tris.data()),
                              triareas.data(), ntris, *solid, cfg.walkableClimb)) {
        outError = "rcRasterizeTriangles failed";
        rcFreeHeightField(solid);
        return false;
    }

    // 诊断计数：滤波前（rasterSpans）/滤波后（filteredSpans）
    for (int y = 0; y < solid->height; ++y)
        for (int x = 0; x < solid->width; ++x)
            for (const rcSpan* s = solid->spans[x + y * solid->width]; s;
                 s = s->next)
                if (s->area == RC_WALKABLE_AREA) ++m_stats.rasterSpans;

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);
    for (int y = 0; y < solid->height; ++y)
        for (int x = 0; x < solid->width; ++x)
            for (const rcSpan* s = solid->spans[x + y * solid->width]; s;
                 s = s->next)
                if (s->area == RC_WALKABLE_AREA) ++m_stats.filteredSpans;

    // ---- 紧凑高度场 -> 腐蚀 -> 距离场 -> 区域 ----
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!chf) { outError = "rcAllocCompactHeightfield failed"; rcFreeHeightField(solid); return false; }
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb,
                                   *solid, *chf)) {
        outError = "rcBuildCompactHeightfield failed";
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        return false;
    }
    rcFreeHeightField(solid);
    solid = nullptr;

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) {
        outError = "rcErodeWalkableArea failed";
        rcFreeCompactHeightfield(chf);
        return false;
    }
    if (!rcBuildDistanceField(&ctx, *chf)) {
        outError = "rcBuildDistanceField failed";
        rcFreeCompactHeightfield(chf);
        return false;
    }
    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
        outError = "rcBuildRegions failed";
        rcFreeCompactHeightfield(chf);
        return false;
    }
    for (int i = 0; i < chf->spanCount; ++i) {
        if (chf->areas[i] != RC_NULL_AREA) ++m_stats.compactSpans;
        if (chf->spans[i].reg > 0 && chf->spans[i].reg != RC_BORDER_REG)
            ++m_stats.regionCount;
    }
    // regionCount 口径 = 带 reg 的可走 span 数（同一区域的多个 span 各计一次；
    // 足以区分「有区域 / 零区域」，精确区域数走 contours/poly 数即可）

    // ---- 轮廓 -> 多边形网格 -> 细节网格 ----
    rcContourSet* cset = rcAllocContourSet();
    if (!cset) { outError = "rcAllocContourSet failed"; rcFreeCompactHeightfield(chf); return false; }
    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen,
                         *cset)) {
        outError = "rcBuildContours failed";
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        return false;
    }
    if (cset->nconts == 0) {
        outError = "no walkable contours (geometry yields no navmesh; check "
                   "slope/height/radius params)";
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        return false;
    }

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!pmesh) { outError = "rcAllocPolyMesh failed"; rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); return false; }
    if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) {
        outError = "rcBuildPolyMesh failed";
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        return false;
    }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!dmesh) { outError = "rcAllocPolyMeshDetail failed"; rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh); return false; }
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist,
                               cfg.detailSampleMaxError, *dmesh)) {
        outError = "rcBuildPolyMeshDetail failed";
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return false;
    }
    if (keepDebug) {
        // M-N2：中间产物移交 Impl 常驻（五阶段 debugDrawStage 消费）。
        // 旧残留先释放（重复 bake 时不泄漏）。
        if (m_impl->dbgChf)   { rcFreeCompactHeightfield(m_impl->dbgChf);   m_impl->dbgChf   = nullptr; }
        if (m_impl->dbgCset)  { rcFreeContourSet(m_impl->dbgCset);          m_impl->dbgCset  = nullptr; }
        if (m_impl->dbgPmesh) { rcFreePolyMesh(m_impl->dbgPmesh);           m_impl->dbgPmesh = nullptr; }
        if (m_impl->dbgDmesh) { rcFreePolyMeshDetail(m_impl->dbgDmesh);     m_impl->dbgDmesh = nullptr; }
        m_impl->dbgChf   = chf;
        m_impl->dbgCset  = cset;
        m_impl->dbgPmesh = pmesh;
        m_impl->dbgDmesh = dmesh;
    } else {
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
    }
    chf  = nullptr;
    cset = nullptr;
    // ---- dtCreateNavMeshData（参数填充照 Sample_SoloMesh.cpp 679-707 行）----
    if (pmesh->nverts > 0xFFFF) {
        outError = "polymesh vertex count exceeds Detour tile limit ("
                 + std::to_string(pmesh->nverts) + " > 65535)";
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return false;
    }
    std::vector<unsigned short> polyFlags(static_cast<size_t>(pmesh->npolys), 1);

    dtNavMeshCreateParams dparams;
    memset(&dparams, 0, sizeof(dparams));
    dparams.verts         = pmesh->verts;
    dparams.vertCount     = pmesh->nverts;
    dparams.polys         = pmesh->polys;
    dparams.polyAreas     = pmesh->areas;
    dparams.polyFlags     = polyFlags.data();
    dparams.polyCount     = pmesh->npolys;
    dparams.nvp           = pmesh->nvp;
    dparams.detailMeshes  = dmesh->meshes;
    dparams.detailVerts   = dmesh->verts;
    dparams.detailVertsCount = dmesh->nverts;
    dparams.detailTris    = dmesh->tris;
    dparams.detailTriCount = dmesh->ntris;
    dparams.walkableHeight = bp.agentHeight;
    dparams.walkableRadius = bp.agentRadius;
    dparams.walkableClimb  = bp.agentMaxClimb;
    rcVcopy(dparams.bmin, pmesh->bmin);
    rcVcopy(dparams.bmax, pmesh->bmax);
    dparams.cs         = cfg.cs;
    dparams.ch         = cfg.ch;
    dparams.buildBvTree = true;

    unsigned char* navData  = nullptr;
    int            navDataSize = 0;
    if (!dtCreateNavMeshData(&dparams, &navData, &navDataSize)) {
        outError = "dtCreateNavMeshData failed";
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return false;
    }
    if (keepDebug) {
        m_impl->dbgPmesh = pmesh;   // 上方 dbgChf/dbgCset 移交块同轮生效
        m_impl->dbgDmesh = dmesh;
    } else {
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    }
    pmesh = nullptr;
    dmesh = nullptr;

    // ---- dtNavMesh 装载（单 tile solo 语义，上游样板口径）----
    if (m_impl->mesh) {
        dtFreeNavMesh(m_impl->mesh);
        m_impl->mesh = nullptr;
    }
    m_impl->mesh = dtAllocNavMesh();
    if (!m_impl->mesh) {
        dtFree(navData);
        outError = "dtAllocNavMesh failed";
        return false;
    }
    const dtStatus status =
        m_impl->mesh->init(navData, navDataSize, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status)) {
        dtFree(navData);
        m_impl->mesh = nullptr;
        outError = "dtNavMesh::init failed (status=0x"
                 + std::to_string(static_cast<unsigned>(status)) + ")";
        return false;
    }

    // ---- 统计（面积 = 多边形 XZ shoelace 和）----
    m_stats.polyCount      = dparams.polyCount;
    m_stats.vertCount      = dparams.vertCount;
    m_stats.detailTriCount = dparams.detailTriCount;
    float area = 0.f;
    const dtMeshTile* tile = nullptr;
    // 单 tile：遍历取第一个非空 tile
    for (int i = 0; i < m_impl->mesh->getMaxTiles(); ++i) {
        tile = m_impl->constMesh()->getTile(i);
        if (tile && tile->header) break;
        tile = nullptr;
    }
    if (tile && tile->header) {
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& poly = tile->polys[i];
            area += polyAreaXZ(tile->verts, poly.verts, poly.vertCount);
        }
    }
    m_stats.walkableArea = area;
    return true;
}

// ---- M-N3：tiled 烘焙（上游 Sample_TempObstacles::buildTiledNavigation 口径）----
// 与 Solo Mesh bake 完全独立：逐 tile 栅格化 + rcBuildHeightfieldLayers →
// dtBuildTileCacheLayer 压成 compressed tile → dtTileCache::addTile →
// buildNavMeshTilesAt 生成 dtNavMesh tile。动态障碍只重建受影响 tile。
bool NavMesh::bakeTiled(const Geometry& geo, const TileBuildParams& tp,
                        std::string& outError) {
    outError.clear();
    m_stats = BuildStats{};
    const int nverts = static_cast<int>(geo.verts.size() / 3);
    const int ntris  = static_cast<int>(geo.tris.size() / 3);
    if (nverts <= 0 || ntris <= 0 || geo.verts.size() % 3 != 0 ||
        geo.tris.size() % 3 != 0 || tp.tileSize < 8 || tp.tileSize > 256 ||
        tp.maxObstacles <= 0 || tp.base.cellSize <= 0.f ||
        tp.base.cellHeight <= 0.f) {
        outError = "tiled bake: geometry empty or invalid tile params";
        return false;
    }
    // 几何包围盒（统计口径 = 输入几何，与 Solo bake 一致）
    float gmin[3] = {geo.verts[0], geo.verts[1], geo.verts[2]};
    float gmax[3] = {geo.verts[0], geo.verts[1], geo.verts[2]};
    for (int i = 1; i < nverts; ++i)
        for (int k = 0; k < 3; ++k) {
            const float v = geo.verts[i * 3 + k];
            gmin[k] = vmin3(gmin[k], v);
            gmax[k] = vmax3(gmax[k], v);
        }

    const BuildParams& bp = tp.base;
    SilentContext ctx;
    rcConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cs                     = bp.cellSize;
    cfg.ch                     = bp.cellHeight;
    cfg.walkableSlopeAngle     = bp.walkableSlopeAngle;
    cfg.walkableHeight         = static_cast<int>(std::ceilf(bp.agentHeight / cfg.ch));
    cfg.walkableClimb          = static_cast<int>(std::floorf(bp.agentMaxClimb / cfg.ch));
    cfg.walkableRadius         = static_cast<int>(std::ceilf(bp.agentRadius / cfg.cs));
    cfg.maxEdgeLen             = static_cast<int>(bp.edgeMaxLen / bp.cellSize);
    cfg.maxSimplificationError = bp.edgeMaxError;
    cfg.minRegionArea          = static_cast<int>(rcSqr(bp.regionMinSize));
    cfg.mergeRegionArea        = static_cast<int>(rcSqr(bp.regionMergeSize));
    cfg.maxVertsPerPoly        = bp.vertsPerPoly;
    cfg.detailSampleDist       = bp.detailSampleDist < 0.9f ? 0.f
                                    : bp.cellSize * bp.detailSampleDist;
    cfg.detailSampleMaxError   = bp.cellHeight * bp.detailSampleMaxError;
    rcVcopy(cfg.bmin, gmin);
    rcVcopy(cfg.bmax, gmax);
    cfg.tileSize   = tp.tileSize;
    cfg.borderSize = cfg.walkableRadius + 3;   // 上游口径：留出 agent 半径余量
    cfg.width      = cfg.tileSize + cfg.borderSize * 2;
    cfg.height     = cfg.width;

    int gw = 0, gh = 0;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &gw, &gh);
    const int ts = tp.tileSize;
    const int tw = (gw + ts - 1) / ts;
    const int th = (gh + ts - 1) / ts;
    if (gw <= 0 || gh <= 0 || tw <= 0 || th <= 0 ||
        static_cast<long long>(tw) * th > 4096LL) {
        outError = "tiled bake: grid/tile count degenerate";
        return false;
    }
    const float tcs = static_cast<float>(ts) * cfg.cs;

    // tile/poly 位分配（上游 22 位 polyref，tileBits 上限 14）
    const unsigned int wanted = static_cast<unsigned int>(tw * th * 4);
    int tileBits = static_cast<int>(dtIlog2(dtNextPow2(wanted)));
    if (tileBits > 14) tileBits = 14;
    const int polyBits = 22 - tileBits;

    dtTileCacheParams tcp;
    memset(&tcp, 0, sizeof(tcp));
    rcVcopy(tcp.orig, gmin);
    tcp.cs                      = cfg.cs;
    tcp.ch                      = cfg.ch;
    tcp.width                   = ts;
    tcp.height                  = ts;
    tcp.walkableHeight          = bp.agentHeight;
    tcp.walkableRadius          = bp.agentRadius;
    tcp.walkableClimb           = bp.agentMaxClimb;
    tcp.maxSimplificationError  = bp.edgeMaxError;
    tcp.maxTiles                = tw * th * 4;   // EXPECTED_LAYERS_PER_TILE
    tcp.maxObstacles            = tp.maxObstacles;

    dtNavMeshParams nmp;
    memset(&nmp, 0, sizeof(nmp));
    rcVcopy(nmp.orig, gmin);
    nmp.tileWidth  = tcs;
    nmp.tileHeight = tcs;
    nmp.maxTiles   = 1 << tileBits;
    nmp.maxPolys   = 1 << polyBits;

    // 失败路径统一回收（顺序：navmesh → tileCache → 注入对象）
    auto fail = [&](const char* what) {
        outError = std::string("tiled bake: ") + what;
        if (m_impl->mesh) { dtFreeNavMesh(m_impl->mesh); m_impl->mesh = nullptr; }
        m_impl->releaseTiled();
        return false;
    };

    if (m_impl->mesh) { dtFreeNavMesh(m_impl->mesh); m_impl->mesh = nullptr; }
    m_impl->ownedTiles.clear();
    m_impl->releaseTiled();
    if (m_impl->query) {
        dtFreeNavMeshQuery(m_impl->query);
        m_impl->query = nullptr;
        m_impl->queryMaxNodes = 0;
    }

    m_impl->talloc    = new TileLinearAllocator(1u << 20);
    m_impl->tcomp     = new TilePassthroughCompressor();
    m_impl->tmproc    = new TileWalkableMeshProcess();
    m_impl->tileCache = dtAllocTileCache();
    if (!m_impl->tileCache ||
        dtStatusFailed(m_impl->tileCache->init(&tcp, m_impl->talloc,
                                               m_impl->tcomp, m_impl->tmproc)))
        return fail("dtTileCache::init failed");
    m_impl->mesh = dtAllocNavMesh();
    if (!m_impl->mesh || dtStatusFailed(m_impl->mesh->init(&nmp)))
        return fail("dtNavMesh::init(tiled params) failed");

    // 索引转 int（上游 rc* 收 const int*；几何规模 < 2^31）
    std::vector<int> itris(geo.tris.size());
    for (std::size_t i = 0; i < geo.tris.size(); ++i)
        itris[i] = static_cast<int>(geo.tris[i]);
    std::vector<unsigned char> triareas(static_cast<std::size_t>(ntris), 0);
    int layerTotal = 0;

    // 单 tile 的 rc 对象 RAII（错误路径不泄漏，且逐 tile 复位）
    struct RcObjs {
        rcHeightfield*          solid = nullptr;
        rcCompactHeightfield*   chf   = nullptr;
        rcHeightfieldLayerSet*  lset  = nullptr;
        ~RcObjs() {
            if (lset)  rcFreeHeightfieldLayerSet(lset);
            if (chf)   rcFreeCompactHeightfield(chf);
            if (solid) rcFreeHeightField(solid);
        }
    };

    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            RcObjs rc;
            rc.solid = rcAllocHeightfield();
            if (!rc.solid) return fail("rcAllocHeightfield failed");

            // 逐 tile 配置：tile 盒 + border 外扩（上游 rasterizeTileLayers 口径）
            rcConfig tcfg = cfg;
            tcfg.bmin[0] = cfg.bmin[0] + x * tcs - cfg.borderSize * cfg.cs;
            tcfg.bmin[2] = cfg.bmin[2] + y * tcs - cfg.borderSize * cfg.cs;
            tcfg.bmax[0] = cfg.bmin[0] + (x + 1) * tcs + cfg.borderSize * cfg.cs;
            tcfg.bmax[2] = cfg.bmin[2] + (y + 1) * tcs + cfg.borderSize * cfg.cs;
            // y 轴不变（高度）
            if (!rcCreateHeightfield(&ctx, *rc.solid, tcfg.width, tcfg.height,
                                     tcfg.bmin, tcfg.bmax, tcfg.cs, tcfg.ch))
                return fail("rcCreateHeightfield(tile) failed");
            // rcMarkWalkableTriangles 上游返回 void（M-N0 bake 同款直接调用）。
            rcMarkWalkableTriangles(&ctx, tcfg.walkableSlopeAngle,
                                    geo.verts.data(), nverts, itris.data(),
                                    ntris, triareas.data());
            if (!rcRasterizeTriangles(&ctx, geo.verts.data(), nverts,
                                      itris.data(), triareas.data(), ntris,
                                      *rc.solid, tcfg.walkableClimb))
                return fail("rcRasterizeTriangles(tile) failed");
            // 上游 rcFilter* 返回 void（M-N0 bake 同款直接调用，无状态返回）。
            rcFilterLowHangingWalkableObstacles(&ctx, tcfg.walkableClimb,
                                                *rc.solid);
            rcFilterLedgeSpans(&ctx, tcfg.walkableHeight,
                               tcfg.walkableClimb, *rc.solid);
            rcFilterWalkableLowHeightSpans(&ctx, tcfg.walkableHeight,
                                           *rc.solid);
            rc.chf = rcAllocCompactHeightfield();
            if (!rc.chf) return fail("rcAllocCompactHeightfield failed");
            if (!rcBuildCompactHeightfield(&ctx, tcfg.walkableHeight,
                                           tcfg.walkableClimb, *rc.solid, *rc.chf))
                return fail("rcBuildCompactHeightfield failed");
            if (!rcErodeWalkableArea(&ctx, tcfg.walkableRadius, *rc.chf))
                return fail("rcErodeWalkableArea failed");
            rc.lset = rcAllocHeightfieldLayerSet();
            if (!rc.lset) return fail("rcAllocHeightfieldLayerSet failed");
            if (!rcBuildHeightfieldLayers(&ctx, *rc.chf, tcfg.borderSize,
                                          tcfg.walkableHeight, *rc.lset))
                return fail("rcBuildHeightfieldLayers failed");

            for (int i = 0; i < rc.lset->nlayers; ++i) {
                const rcHeightfieldLayer& layer = rc.lset->layers[i];
                dtTileCacheLayerHeader header;
                memset(&header, 0, sizeof(header));
                header.magic   = DT_TILECACHE_MAGIC;
                header.version = DT_TILECACHE_VERSION;
                header.tx = x; header.ty = y; header.tlayer = i;
                for (int k = 0; k < 3; ++k) {
                    header.bmin[k] = layer.bmin[k];
                    header.bmax[k] = layer.bmax[k];
                }
                header.width  = static_cast<unsigned char>(layer.width);
                header.height = static_cast<unsigned char>(layer.height);
                header.minx   = static_cast<unsigned char>(layer.minx);
                header.maxx   = static_cast<unsigned char>(layer.maxx);
                header.miny   = static_cast<unsigned char>(layer.miny);
                header.maxy   = static_cast<unsigned char>(layer.maxy);
                header.hmin   = static_cast<unsigned short>(layer.hmin);
                header.hmax   = static_cast<unsigned short>(layer.hmax);
                unsigned char* data = nullptr;
                int dataSize = 0;
                if (dtStatusFailed(dtBuildTileCacheLayer(
                        m_impl->tcomp, &header, layer.heights, layer.areas,
                        layer.cons, &data, &dataSize)))
                    return fail("dtBuildTileCacheLayer failed");
                dtCompressedTileRef ref = 0;
                const dtStatus st = m_impl->tileCache->addTile(
                    data, dataSize, DT_COMPRESSEDTILE_FREE_DATA, &ref);
                if (dtStatusFailed(st)) {
                    dtFree(data);   // 未移交所有权，按上游分配器归还
                    return fail("dtTileCache::addTile failed");
                }
                ++layerTotal;
            }
        }
    }

    // 初始构建全部 tile（每 tile 一次 dtCreateNavMeshData）
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x)
            if (dtStatusFailed(m_impl->tileCache->buildNavMeshTilesAt(
                    x, y, m_impl->mesh)))
                return fail("buildNavMeshTilesAt failed");

    m_impl->isTiled    = true;
    m_impl->tileParams = tp;
    m_impl->tcParams   = tcp;
    m_impl->meshParams = nmp;
    m_stats = statsFromMesh(m_impl->mesh, gmin, gmax);
    m_stats.gridWidth    = gw;
    m_stats.gridHeight   = gh;
    m_stats.walkableTris = ntris;
    m_stats.regionCount  = layerTotal;   // tiled 口径：compressed layer 总数
    return true;
}

bool NavMesh::save(const char* path, std::string& outError) const {
    outError.clear();
    if (!m_impl || !m_impl->mesh) {
        outError = "no navmesh (bake or load first)";
        return false;
    }
    dtNavMeshParams nmp;
    nmp = *m_impl->constMesh()->getParams();

    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        outError = std::string("cannot open for write: ") + path;
        return false;
    }
    auto bail = [&](const char* what) {
        outError = std::string("save failed: ") + what;
        std::fclose(f);
        return false;
    };
    if (std::fwrite(kNavMagic, 1, 4, f) != 4) return bail("magic");
    // M-N3 A-1：同一容器语义升级，version=2 表示 tiled（尾部追加扩展段）。
    const bool tiled = m_impl->isTiled && m_impl->tileCache != nullptr;
    const std::uint16_t version = tiled ? kNavVersionTiled : kNavVersion, zero = 0;
    if (std::fwrite(&version, 2, 1, f) != 1) return bail("version");
    if (std::fwrite(&zero, 2, 1, f) != 1) return bail("reserved");
    // dtNavMeshParams 逐字段（f32 orig[3] + f32 tileW/H + i32 maxTiles/maxPolys）
    if (std::fwrite(nmp.orig, 4, 3, f) != 3) return bail("orig");
    if (std::fwrite(&nmp.tileWidth, 4, 1, f) != 1) return bail("tileWidth");
    if (std::fwrite(&nmp.tileHeight, 4, 1, f) != 1) return bail("tileHeight");
    if (std::fwrite(&nmp.maxTiles, 4, 1, f) != 1) return bail("maxTiles");
    if (std::fwrite(&nmp.maxPolys, 4, 1, f) != 1) return bail("maxPolys");
    // tile 清点
    std::uint32_t tileCount = 0;
    for (int i = 0; i < m_impl->mesh->getMaxTiles(); ++i) {
        const dtMeshTile* t = m_impl->constMesh()->getTile(i);
        if (t && t->header) ++tileCount;
    }
    if (std::fwrite(&tileCount, 4, 1, f) != 1) return bail("tileCount");
    for (int i = 0; i < m_impl->mesh->getMaxTiles(); ++i) {
        const dtMeshTile* t = m_impl->constMesh()->getTile(i);
        if (!t || !t->header) continue;
        const std::uint32_t bytes = static_cast<std::uint32_t>(t->dataSize);
        if (std::fwrite(&bytes, 4, 1, f) != 1) return bail("tileBytes");
        if (bytes && std::fwrite(t->data, 1, bytes, f) != bytes)
            return bail("tileData");
    }
    // ---- v2 tiled 扩展段（逐字段，小端、无 padding）----
    if (tiled) {
        const dtTileCacheParams& tcp = m_impl->tcParams;
        const TileBuildParams&  tp  = m_impl->tileParams;
        const BuildParams&      bp  = tp.base;
        for (int k = 0; k < 3; ++k) {
            if (!wrScalar(f, m_stats.bmin[k]) || !wrScalar(f, m_stats.bmax[k]))
                return bail("tiled bbox");
            if (!wrScalar(f, tcp.orig[k])) return bail("tiled tcParams.orig");
        }
        if (!wrScalar(f, tcp.cs) || !wrScalar(f, tcp.ch) ||
            !wrScalar(f, tcp.walkableHeight) || !wrScalar(f, tcp.walkableRadius) ||
            !wrScalar(f, tcp.walkableClimb) ||
            !wrScalar(f, tcp.maxSimplificationError) ||
            !wrScalar(f, tcp.width) || !wrScalar(f, tcp.height) ||
            !wrScalar(f, tcp.maxTiles) || !wrScalar(f, tcp.maxObstacles))
            return bail("tiled tcParams");
        if (!wrScalar(f, bp.cellSize) || !wrScalar(f, bp.cellHeight) ||
            !wrScalar(f, bp.walkableSlopeAngle) || !wrScalar(f, bp.agentHeight) ||
            !wrScalar(f, bp.agentRadius) || !wrScalar(f, bp.agentMaxClimb) ||
            !wrScalar(f, bp.regionMinSize) || !wrScalar(f, bp.regionMergeSize) ||
            !wrScalar(f, bp.edgeMaxLen) || !wrScalar(f, bp.edgeMaxError) ||
            !wrScalar(f, bp.vertsPerPoly) ||
            !wrScalar(f, bp.detailSampleDist) ||
            !wrScalar(f, bp.detailSampleMaxError) ||
            !wrScalar(f, tp.tileSize) || !wrScalar(f, tp.maxObstacles))
            return bail("tiled buildParams");
        std::uint32_t compCount = 0;
        for (int i = 0; i < m_impl->tileCache->getTileCount(); ++i) {
            const dtCompressedTile* t = m_impl->tileCache->getTile(i);
            if (t && t->header && t->data && t->dataSize > 0) ++compCount;
        }
        if (!wrScalar(f, compCount)) return bail("tiled tileCount");
        for (int i = 0; i < m_impl->tileCache->getTileCount(); ++i) {
            const dtCompressedTile* t = m_impl->tileCache->getTile(i);
            if (!t || !t->header || !t->data || t->dataSize <= 0) continue;
            const std::uint32_t bytes = static_cast<std::uint32_t>(t->dataSize);
            if (!wrScalar(f, bytes) ||
                !wrScalar(f, t->header->tx) || !wrScalar(f, t->header->ty) ||
                !wrScalar(f, t->header->tlayer)) return bail("tiled tileHeader");
            if (std::fwrite(t->data, 1, bytes, f) != bytes)
                return bail("tiled tileData");
        }
    }
    if (std::fclose(f) != 0) { outError = "save failed: fclose"; return false; }
    return true;
}

bool NavMesh::load(const char* path, std::string& outError) {
    outError.clear();
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        outError = std::string("cannot open: ") + path;
        return false;
    }
    auto rd = [&](void* dst, size_t sz, size_t n) {
        return std::fread(dst, sz, n, f) == n;
    };
    char magic[4] = {0};
    std::uint16_t version = 0, zero = 0;
    if (!rd(magic, 1, 4) || std::memcmp(magic, kNavMagic, 4) != 0) {
        std::fclose(f);
        outError = "bad magic (not an NLNV navmesh file)";
        return false;
    }
    if (!rd(&version, 2, 1) || !rd(&zero, 2, 1)) {
        std::fclose(f);
        outError = "truncated header";
        return false;
    }
    if (version != kNavVersion && version != kNavVersionTiled) {
        std::fclose(f);
        outError = "unsupported version " + std::to_string(version);
        return false;
    }
    dtNavMeshParams nmp;
    memset(&nmp, 0, sizeof(nmp));
    if (!rd(nmp.orig, 4, 3) || !rd(&nmp.tileWidth, 4, 1) ||
        !rd(&nmp.tileHeight, 4, 1) || !rd(&nmp.maxTiles, 4, 1) ||
        !rd(&nmp.maxPolys, 4, 1)) {
        std::fclose(f);
        outError = "truncated navmesh params";
        return false;
    }
    std::uint32_t tileCount = 0;
    if (!rd(&tileCount, 4, 1) || tileCount == 0 || tileCount > 4096) {
        std::fclose(f);
        outError = "bad tile count";
        return false;
    }
    std::vector<std::vector<unsigned char>> tiles(tileCount);
    for (std::uint32_t i = 0; i < tileCount; ++i) {
        std::uint32_t bytes = 0;
        if (!rd(&bytes, 4, 1) || bytes == 0 || bytes > (64u << 20)) {
            std::fclose(f);
            outError = "bad tile size at index " + std::to_string(i);
            return false;
        }
        tiles[i].resize(bytes);
        if (!rd(tiles[i].data(), 1, bytes)) {
            std::fclose(f);
            outError = "truncated tile data at index " + std::to_string(i);
            return false;
        }
    }
    // ---- v2 扩展段（读完后重建 tiled，基础段 tile 数据只作兼容占位）----
    TiledExt ext;
    if (version == kNavVersionTiled) {
        auto extBail = [&](const char* what) {
            std::fclose(f);
            outError = std::string("v2 extension: ") + what;
            return false;
        };
        dtTileCacheParams& tcp = ext.tcp;
        TileBuildParams&   tp  = ext.tp;
        BuildParams&       bp  = tp.base;
        for (int k = 0; k < 3; ++k)
            if (!rdScalar(f, ext.gmin[k]) || !rdScalar(f, ext.gmax[k]) ||
                !rdScalar(f, tcp.orig[k])) return extBail("truncated bbox/orig");
        if (!rdScalar(f, tcp.cs) || !rdScalar(f, tcp.ch) ||
            !rdScalar(f, tcp.walkableHeight) || !rdScalar(f, tcp.walkableRadius) ||
            !rdScalar(f, tcp.walkableClimb) ||
            !rdScalar(f, tcp.maxSimplificationError) ||
            !rdScalar(f, tcp.width) || !rdScalar(f, tcp.height) ||
            !rdScalar(f, tcp.maxTiles) || !rdScalar(f, tcp.maxObstacles))
            return extBail("truncated tcParams");
        if (!rdScalar(f, bp.cellSize) || !rdScalar(f, bp.cellHeight) ||
            !rdScalar(f, bp.walkableSlopeAngle) || !rdScalar(f, bp.agentHeight) ||
            !rdScalar(f, bp.agentRadius) || !rdScalar(f, bp.agentMaxClimb) ||
            !rdScalar(f, bp.regionMinSize) || !rdScalar(f, bp.regionMergeSize) ||
            !rdScalar(f, bp.edgeMaxLen) || !rdScalar(f, bp.edgeMaxError) ||
            !rdScalar(f, bp.vertsPerPoly) ||
            !rdScalar(f, bp.detailSampleDist) ||
            !rdScalar(f, bp.detailSampleMaxError) ||
            !rdScalar(f, tp.tileSize) || !rdScalar(f, tp.maxObstacles))
            return extBail("truncated buildParams");
        std::uint32_t compCount = 0;
        if (!rdScalar(f, compCount) || compCount == 0 || compCount > 4096)
            return extBail("bad compressed tile count");
        ext.tiles.reserve(compCount);
        for (std::uint32_t i = 0; i < compCount; ++i) {
            TiledExt::Tile t;
            std::uint32_t bytes = 0;
            if (!rdScalar(f, bytes) || bytes == 0 || bytes > (64u << 20) ||
                !rdScalar(f, t.tx) || !rdScalar(f, t.ty) ||
                !rdScalar(f, t.tlayer))
                return extBail("bad compressed tile header");
            t.data.resize(bytes);
            if (!rd(t.data.data(), 1, bytes))
                return extBail("truncated compressed tile data");
            ext.tiles.push_back(std::move(t));
        }
    }
    std::fclose(f);

    if (m_impl->mesh) {
        dtFreeNavMesh(m_impl->mesh);
        m_impl->mesh = nullptr;
    }
    m_impl->ownedTiles.clear();
    m_impl->releaseTiled();
    if (m_impl->query) {
        dtFreeNavMeshQuery(m_impl->query);
        m_impl->query = nullptr;
        m_impl->queryMaxNodes = 0;
    }
    // ---- v2：按扩展段重建 dtTileCache + tiled dtNavMesh（动态障碍能力保留）----
    if (version == kNavVersionTiled) {
        auto failV2 = [&](const char* what) {
            outError = std::string("v2 rebuild: ") + what;
            if (m_impl->mesh) { dtFreeNavMesh(m_impl->mesh); m_impl->mesh = nullptr; }
            m_impl->releaseTiled();
            return false;
        };
        m_impl->talloc    = new TileLinearAllocator(1u << 20);
        m_impl->tcomp     = new TilePassthroughCompressor();
        m_impl->tmproc    = new TileWalkableMeshProcess();
        m_impl->tileCache = dtAllocTileCache();
        if (!m_impl->tileCache ||
            dtStatusFailed(m_impl->tileCache->init(&ext.tcp, m_impl->talloc,
                                                   m_impl->tcomp, m_impl->tmproc)))
            return failV2("dtTileCache::init failed");
        m_impl->mesh = dtAllocNavMesh();
        if (!m_impl->mesh || dtStatusFailed(m_impl->mesh->init(&nmp)))
            return failV2("dtNavMesh::init failed");
        for (const TiledExt::Tile& t : ext.tiles) {
            // 铁律（M-N0 同款）：addTile(FREE_DATA) 的数据必须由 dtAlloc 分配，
            // vector 的 new 内存不能移交，否则析构分配器不匹配 + UAF。
            unsigned char* data = static_cast<unsigned char*>(
                dtAlloc(t.data.size(), DT_ALLOC_PERM));
            if (!data) return failV2("dtAlloc compressed tile failed");
            std::memcpy(data, t.data.data(), t.data.size());
            dtCompressedTileRef ref = 0;
            const dtStatus st = m_impl->tileCache->addTile(
                data, static_cast<int>(t.data.size()),
                DT_COMPRESSEDTILE_FREE_DATA, &ref);
            if (dtStatusFailed(st)) {
                dtFree(data);
                return failV2("addTile failed");
            }
        }
        // 每个 (tx,ty) 只重建一次（同位置多层由 getTilesAt 一次全收）
        std::vector<std::pair<int, int>> built;
        for (const TiledExt::Tile& t : ext.tiles) {
            const std::pair<int, int> key{t.tx, t.ty};
            if (std::find(built.begin(), built.end(), key) != built.end()) continue;
            if (dtStatusFailed(m_impl->tileCache->buildNavMeshTilesAt(
                    t.tx, t.ty, m_impl->mesh)))
                return failV2("buildNavMeshTilesAt failed");
            built.push_back(key);
        }
        m_impl->isTiled    = true;
        m_impl->tileParams = ext.tp;
        m_impl->tcParams   = ext.tcp;
        m_impl->meshParams = nmp;
        m_stats = statsFromMesh(m_impl->mesh, ext.gmin, ext.gmax);
        m_stats.regionCount = static_cast<int>(ext.tiles.size());
        return true;
    }
    m_impl->ownedTiles = std::move(tiles);
    m_impl->mesh = dtAllocNavMesh();
    if (!m_impl->mesh || dtStatusFailed(m_impl->mesh->init(&nmp))) {
        outError = "dtNavMesh::init(params) failed";
        return false;
    }
    for (std::uint32_t i = 0; i < tileCount; ++i) {
        unsigned char* data = m_impl->ownedTiles[i].data();
        const int n = static_cast<int>(m_impl->ownedTiles[i].size());
        // flags=0：不移交所有权（见 Impl 注释）；生存期由 Impl 管。
        if (dtStatusFailed(m_impl->mesh->addTile(
                data, n, 0, 0, nullptr))) {
            outError = "addTile failed at index " + std::to_string(i);
            return false;
        }
    }

    // 统计重算（与 bake 同口径；bbox 取各 tile 头的并集）
    BuildStats s;
    bool first = true;
    for (int i = 0; i < m_impl->mesh->getMaxTiles(); ++i) {
        const dtMeshTile* t = m_impl->constMesh()->getTile(i);
        if (!t || !t->header) continue;
        s.polyCount += t->header->polyCount;
        s.vertCount += t->header->vertCount;
        s.detailTriCount += t->header->detailTriCount;
        for (int p = 0; p < t->header->polyCount; ++p)
            s.walkableArea +=
                polyAreaXZ(t->verts, t->polys[p].verts, t->polys[p].vertCount);
        for (int k = 0; k < 3; ++k) {
            s.bmin[k] = first ? t->header->bmin[k]
                              : vmin3(s.bmin[k], t->header->bmin[k]);
            s.bmax[k] = first ? t->header->bmax[k]
                              : vmax3(s.bmax[k], t->header->bmax[k]);
        }
        first = false;
    }
    m_stats = s;
    return true;
}

// ---- M-N3：动态障碍（dtTileCache 增量重建，只触碰受影响 tile）----
bool NavMesh::isTiled() const {
    return m_impl && m_impl->isTiled && m_impl->tileCache != nullptr;
}

int NavMesh::compressedTileCount() const {
    if (!isTiled()) return 0;
    int n = 0;
    for (int i = 0; i < m_impl->tileCache->getTileCount(); ++i) {
        const dtCompressedTile* t = m_impl->tileCache->getTile(i);
        if (t && t->header && t->data && t->dataSize > 0) ++n;
    }
    return n;
}

int NavMesh::obstacleCount() const {
    if (!isTiled()) return 0;
    int n = 0;
    for (int i = 0; i < m_impl->tileCache->getObstacleCount(); ++i) {
        const dtTileCacheObstacle* o = m_impl->tileCache->getObstacle(i);
        if (o && o->state == DT_OBSTACLE_PROCESSED) ++n;
    }
    return n;
}

bool NavMesh::updateObstacles(std::string& outError) {
    outError.clear();
    if (!isTiled()) { outError = "not a tiled navmesh"; return false; }
    bool upToDate = false;
    for (int i = 0; i < 128 && !upToDate; ++i) {
        if (dtStatusFailed(m_impl->tileCache->update(0.f, m_impl->mesh,
                                                     &upToDate))) {
            outError = "dtTileCache::update failed";
            return false;
        }
    }
    if (!upToDate) { outError = "tile cache update did not converge"; return false; }
    // tile refs 变化 → 查询对象缓存失效（下次 findPath 重建）；统计重算。
    if (m_impl->query) {
        dtFreeNavMeshQuery(m_impl->query);
        m_impl->query = nullptr;
        m_impl->queryMaxNodes = 0;
    }
    const float gmin[3] = {m_stats.bmin[0], m_stats.bmin[1], m_stats.bmin[2]};
    const float gmax[3] = {m_stats.bmax[0], m_stats.bmax[1], m_stats.bmax[2]};
    BuildStats s = statsFromMesh(m_impl->mesh, gmin, gmax);
    s.gridWidth     = m_stats.gridWidth;
    s.gridHeight    = m_stats.gridHeight;
    s.walkableTris  = m_stats.walkableTris;
    s.rasterSpans   = m_stats.rasterSpans;
    s.filteredSpans = m_stats.filteredSpans;
    s.compactSpans  = m_stats.compactSpans;
    s.regionCount   = compressedTileCount();
    m_stats = s;
    return true;
}

bool NavMesh::addObstacle(const float pos[3], float radius, float height,
                          std::uint32_t& outRef, std::string& outError) {
    outError.clear();
    outRef = 0;
    if (!isTiled()) { outError = "not a tiled navmesh"; return false; }
    if (!pos || !(radius > 0.f) || !(height > 0.f) ||
        !std::isfinite(pos[0]) || !std::isfinite(pos[1]) ||
        !std::isfinite(pos[2]) || !std::isfinite(radius) ||
        !std::isfinite(height)) {
        outError = "invalid cylinder obstacle";
        return false;
    }
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_impl->tileCache->addObstacle(pos, radius, height,
                                                      &ref))) {
        outError = "dtTileCache::addObstacle failed (obstacle pool full?)";
        return false;
    }
    outRef = static_cast<std::uint32_t>(ref);
    return updateObstacles(outError);
}

bool NavMesh::removeObstacle(std::uint32_t ref, std::string& outError) {
    outError.clear();
    if (!isTiled()) { outError = "not a tiled navmesh"; return false; }
    if (ref == 0 ||
        dtStatusFailed(m_impl->tileCache->removeObstacle(
            static_cast<dtObstacleRef>(ref)))) {
        outError = "dtTileCache::removeObstacle failed (bad ref?)";
        return false;
    }
    return updateObstacles(outError);
}

const char* NavMesh::queryStatusName(QueryStatus s) {
    switch (s) {
    case QueryStatus::OK:               return "OK";
    case QueryStatus::UNREACHABLE:      return "UNREACHABLE";
    case QueryStatus::OUT_OF_NODES:     return "OUT_OF_NODES";
    case QueryStatus::BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case QueryStatus::NOT_INITIALIZED:  return "NOT_INITIALIZED";
    case QueryStatus::INVALID_PARAM:    return "INVALID_PARAM";
    }
    return "?";
}

NavMesh::QueryStatus NavMesh::findPath(const float start[3], const float end[3],
                                       const QueryParams& qp,
                                       QueryResult& out) const {
    out = QueryResult{};
    if (!m_impl || !m_impl->mesh) {
        out.status = QueryStatus::NOT_INITIALIZED;
        return out.status;
    }
    // 参数闸（上游 findPath 对非法参数只回 FAILURE|INVALID_PARAM，这里给出
    // 可区分的明确错误码；maxNodes 上限 = dtNodePool 的 65535 硬界）。
    const bool finiteStart = std::isfinite(start[0]) && std::isfinite(start[1]) &&
                             std::isfinite(start[2]);
    const bool finiteEnd   = std::isfinite(end[0]) && std::isfinite(end[1]) &&
                             std::isfinite(end[2]);
    const bool finiteHe    = std::isfinite(qp.halfExtents[0]) &&
                             std::isfinite(qp.halfExtents[1]) &&
                             std::isfinite(qp.halfExtents[2]);
    if (!finiteStart || !finiteEnd || !finiteHe ||
        qp.maxNodes < 4 || qp.maxNodes > 65535 ||
        qp.maxPathPolys <= 0 || qp.maxWaypoints <= 0 ||
        qp.halfExtents[0] <= 0.f || qp.halfExtents[1] <= 0.f ||
        qp.halfExtents[2] <= 0.f) {
        out.status = QueryStatus::INVALID_PARAM;
        return out.status;
    }
    // ⚠️ 上游真 bug（M-N1 实测，maxNodes<4 = 段错误）：dtNavMeshQuery::init 用
    // dtNextPow2(maxNodes/4) 作 hash 桶数，maxNodes<4 时该值为 0（dtNextPow2 的
    // v-- 对 0 下溢）→ dtNodePool 的 m_first 0 字节分配 → getNode 哈希索引越界。
    // RecastDemo 默认 4096 从不触发。故封装层硬闸下限 4（hashSize>=1）。

    // 查询对象：maxNodes 变化才重建（dtNavMeshQuery::init 分配节点池）。
    if (!m_impl->query || m_impl->queryMaxNodes != qp.maxNodes) {
        if (m_impl->query) { dtFreeNavMeshQuery(m_impl->query); m_impl->query = nullptr; }
        m_impl->query = dtAllocNavMeshQuery();
        if (!m_impl->query ||
            dtStatusFailed(m_impl->query->init(m_impl->constMesh(),
                                               qp.maxNodes))) {
            if (m_impl->query) { dtFreeNavMeshQuery(m_impl->query); m_impl->query = nullptr; }
            out.status = QueryStatus::INVALID_PARAM;
            return out.status;
        }
        m_impl->queryMaxNodes = qp.maxNodes;
    }

    // 全 poly flag=1（bake 侧），默认 filter（include 0xffff）全放行。
    dtQueryFilter filter;

    // ---- 端点吸附：findNearestPoly ----
    dtPolyRef sref = 0, eref = 0;
    float spos[3], epos[3];
    const dtStatus ss = m_impl->query->findNearestPoly(
        start, qp.halfExtents, &filter, &sref, spos);
    const dtStatus se = m_impl->query->findNearestPoly(
        end, qp.halfExtents, &filter, &eref, epos);
    if (dtStatusFailed(ss) || dtStatusFailed(se) || sref == 0 || eref == 0) {
        out.status = QueryStatus::UNREACHABLE;
        return out.status;
    }

    // ---- 走廊：findPath ----
    // 上游语义（DetourNavMeshQuery.cpp:973-1165）：
    //   不可达（open list 耗尽）= DT_SUCCESS|DT_PARTIAL_RESULT，折线为 best-effort
    //   节点池分配失败          = detail DT_OUT_OF_NODES（可与 partial 并存）
    //   走廊缓冲不足            = DT_SUCCESS|DT_BUFFER_TOO_SMALL（截断填充）
    std::vector<dtPolyRef> corridor(static_cast<size_t>(qp.maxPathPolys));
    int nPath = 0;
    const dtStatus sp = m_impl->query->findPath(
        sref, eref, spos, epos, &filter, corridor.data(), &nPath,
        qp.maxPathPolys);
    if (dtStatusFailed(sp)) {
        out.status = QueryStatus::INVALID_PARAM; // 参数已闸过，防御性兜底
        return out.status;
    }
    const bool partial    = dtStatusDetail(sp, DT_PARTIAL_RESULT) != 0;
    const bool outOfNodes = dtStatusDetail(sp, DT_OUT_OF_NODES) != 0;
    const bool bufSmallP  = dtStatusDetail(sp, DT_BUFFER_TOO_SMALL) != 0;
    if (partial && outOfNodes)      out.status = QueryStatus::OUT_OF_NODES;
    else if (partial)               out.status = QueryStatus::UNREACHABLE;
    else if (outOfNodes)            { out.status = QueryStatus::OK; out.reachedLimit = true; }
    else if (bufSmallP)             out.status = QueryStatus::BUFFER_TOO_SMALL;
    else                            out.status = QueryStatus::OK;

    // ---- 折线：findStraightPath（string-pulling）----
    if (nPath > 0) {
        std::vector<float> wp(static_cast<size_t>(qp.maxWaypoints) * 3);
        int nWp = 0;
        const dtStatus st = m_impl->query->findStraightPath(
            spos, epos, corridor.data(), nPath, wp.data(), nullptr, nullptr,
            &nWp, qp.maxWaypoints);
        if (dtStatusFailed(st)) {
            out.status = QueryStatus::INVALID_PARAM; // 走廊内故障，防御性兜底
            return out.status;
        }
        // 折线阶段缓冲不足：仅在 OK 态升级为 BUFFER_TOO_SMALL（错误态信息优先）。
        if (dtStatusDetail(st, DT_BUFFER_TOO_SMALL) &&
            out.status == QueryStatus::OK) {
            out.status = QueryStatus::BUFFER_TOO_SMALL;
        }
        out.waypoints.assign(wp.begin(), wp.begin() + static_cast<size_t>(nWp) * 3);
    }
    return out.status;
}

std::vector<NavMesh::Edge> NavMesh::polyEdges() const {
    std::vector<Edge> out;
    if (!m_impl || !m_impl->mesh) return out;
    for (int i = 0; i < m_impl->mesh->getMaxTiles(); ++i) {
        const dtMeshTile* t = m_impl->constMesh()->getTile(i);
        if (!t || !t->header) continue;
        for (int p = 0; p < t->header->polyCount; ++p) {
            const dtPoly& poly = t->polys[p];
            for (int j = 0; j < poly.vertCount; ++j) {
                Edge e;
                const unsigned short ia = poly.verts[j];
                const unsigned short ib =
                    poly.verts[(j + 1) % poly.vertCount];
                e.a[0] = t->verts[ia * 3 + 0];
                e.a[1] = t->verts[ia * 3 + 1];
                e.a[2] = t->verts[ia * 3 + 2];
                e.b[0] = t->verts[ib * 3 + 0];
                e.b[1] = t->verts[ib * 3 + 1];
                e.b[2] = t->verts[ib * 3 + 2];
                out.push_back(e);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// M-N2：五阶段调试可视化

namespace {

// duDebugDraw 收集器：把上游 duDebugDraw* 系列的图元流原样收进 DebugPrims
//（POD），消费端（nav-tools 软光栅 / viewer ImDrawList）自行投影绘制。
// 上游颜色约定 duRGBA：r | g<<8 | b<<16 | a<<24，POD 直存不改序。
class PrimCollector final : public duDebugDraw {
public:
    DebugPrims* out = nullptr;

    void depthMask(bool) override {}
    void texture(bool) override {}
    void begin(duDebugDrawPrimitives prim, float size) override {
        DebugPrims::Group g;
        g.prim      = static_cast<int>(prim);
        g.size      = size;
        g.vertBegin = static_cast<int>(out->verts.size());
        g.vertEnd   = g.vertBegin;
        out->groups.push_back(g);
    }
    // 上游五个 vertex 重载最终全部落到 (x,y,z,color) 形态（带 uv 的在
    // DetourDebugDraw 内部已换算成颜色），只实现这两条即可覆盖。
    void vertex(const float x, const float y, const float z,
                unsigned int color) override {
        DebugPrims::Vtx v;
        v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
        v.col    = color;
        out->verts.push_back(v);
        if (!out->groups.empty()) ++out->groups.back().vertEnd;
    }
    void vertex(const float* pos, unsigned int color) override {
        vertex(pos[0], pos[1], pos[2], color);
    }
    // 带 uv 的重载：本项目消费端不采样纹理，丢弃 uv 直取颜色（防静默丢顶点）。
    void vertex(const float* pos, unsigned int color,
                const float*) override {
        vertex(pos[0], pos[1], pos[2], color);
    }
    void vertex(const float x, const float y, const float z, unsigned int color,
                const float, const float) override {
        vertex(x, y, z, color);
    }
    void end() override {}
};

} // namespace

const char* debugStageName(DebugStage s) {
    switch (s) {
        case DebugStage::Voxels:      return "voxels";
        case DebugStage::Regions:     return "regions";
        case DebugStage::Distance:    return "distance";
        case DebugStage::Contours:    return "contours";
        case DebugStage::RawContours: return "raw-contours";
        case DebugStage::PolyMesh:    return "polymesh";
        case DebugStage::NavMesh:     return "navmesh";
    }
    return "?";
}

bool debugStageFromName(const char* name, DebugStage& out) {
    const std::string n = name ? name : "";
    static const std::pair<const char*, DebugStage> table[] = {
        {"voxels",      DebugStage::Voxels},
        {"regions",     DebugStage::Regions},
        {"distance",    DebugStage::Distance},
        {"contours",    DebugStage::Contours},
        {"raw-contours",DebugStage::RawContours},
        {"polymesh",    DebugStage::PolyMesh},
        {"navmesh",     DebugStage::NavMesh},
    };
    for (const auto& e : table)
        if (n == e.first) { out = e.second; return true; }
    return false;
}

bool NavMesh::debugDrawStage(DebugStage stage, DebugPrims& out,
                             std::string& outError) const {
    out.groups.clear();
    out.verts.clear();
    if (!m_impl) { outError = "navmesh not initialized"; return false; }

    PrimCollector col;
    col.out = &out;

    switch (stage) {
        case DebugStage::Voxels:
        case DebugStage::Regions:
        case DebugStage::Distance:
            if (!m_impl->dbgChf) {
                outError = "stage requires bake(keepDebug=true) intermediates "
                           "(compact heightfield missing)";
                return false;
            }
            if (stage == DebugStage::Voxels)
                duDebugDrawCompactHeightfieldSolid(&col, *m_impl->dbgChf);
            else if (stage == DebugStage::Regions)
                duDebugDrawCompactHeightfieldRegions(&col, *m_impl->dbgChf);
            else
                duDebugDrawCompactHeightfieldDistance(&col, *m_impl->dbgChf);
            break;
        case DebugStage::Contours:
        case DebugStage::RawContours:
            if (!m_impl->dbgCset) {
                outError = "stage requires bake(keepDebug=true) intermediates "
                           "(contour set missing)";
                return false;
            }
            if (stage == DebugStage::Contours)
                duDebugDrawContours(&col, *m_impl->dbgCset);
            else
                duDebugDrawRawContours(&col, *m_impl->dbgCset);
            break;
        case DebugStage::PolyMesh:
            if (!m_impl->dbgPmesh) {
                outError = "stage requires bake(keepDebug=true) intermediates "
                           "(poly mesh missing)";
                return false;
            }
            duDebugDrawPolyMesh(&col, *m_impl->dbgPmesh);
            break;
        case DebugStage::NavMesh: {
            if (!m_impl->mesh) { outError = "navmesh not initialized"; return false; }
            // 自绘（DetourDebugDraw TU 不入库，M-N0 CMake 留痕口径）：遍历
            // dtMeshTile，poly 顶点 fan 成 TRIS + 边界 LINES，按 poly 序号着色
            //（duIntToCol 同上游 DebugDraw.cpp 配色语义）。
            for (int i = 0; i < m_impl->mesh->getMaxTiles(); ++i) {
                const dtMeshTile* t = m_impl->constMesh()->getTile(i);
                if (!t || !t->header) continue;
                for (int p = 0; p < t->header->polyCount; ++p) {
                    const dtPoly& poly = t->polys[p];
                    const unsigned int col = duTransCol(
                        duIntToCol(p, 200), 255);
                    DebugPrims::Group g;
                    g.prim      = DebugPrims::TRIS;
                    g.size      = 1.f;
                    g.vertBegin = static_cast<int>(out.verts.size());
                    for (int j = 2; j < poly.vertCount; ++j) {
                        const unsigned short idx[3] = {
                            poly.verts[0], poly.verts[j - 1],
                            poly.verts[j]};
                        for (const unsigned short k : idx) {
                            DebugPrims::Vtx v;
                            v.pos[0] = t->verts[k * 3 + 0];
                            v.pos[1] = t->verts[k * 3 + 1];
                            v.pos[2] = t->verts[k * 3 + 2];
                            v.col    = col;
                            out.verts.push_back(v);
                        }
                    }
                    g.vertEnd = static_cast<int>(out.verts.size());
                    if (g.vertEnd > g.vertBegin) out.groups.push_back(g);
                    DebugPrims::Group l;
                    l.prim      = DebugPrims::LINES;
                    l.size      = 1.f;
                    l.vertBegin = g.vertEnd;
                    for (int j = 0; j < poly.vertCount; ++j) {
                        const unsigned short ia = poly.verts[j];
                        const unsigned short ib =
                            poly.verts[(j + 1) % poly.vertCount];
                        const unsigned short pair[2] = {ia, ib};
                        for (const unsigned short k : pair) {
                            DebugPrims::Vtx v;
                            v.pos[0] = t->verts[k * 3 + 0];
                            v.pos[1] = t->verts[k * 3 + 1];
                            v.pos[2] = t->verts[k * 3 + 2];
                            v.col    = duRGBA(0, 48, 96, 255);
                            out.verts.push_back(v);
                        }
                    }
                    l.vertEnd = static_cast<int>(out.verts.size());
                    if (l.vertEnd > l.vertBegin) out.groups.push_back(l);
                }
            }
            break;
        }
        default:
            outError = "unknown stage";
            return false;
    }
    return !out.groups.empty();
}

} // namespace nl::nav
