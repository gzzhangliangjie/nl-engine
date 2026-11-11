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
        const float* b = &verts[idx[(j + 1) % n] * 3];
        area += a[0] * b[2] - b[0] * a[2];
    }
    return std::fabs(area) * 0.5f;
}

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
    ~Impl() {
        if (query) dtFreeNavMeshQuery(query);
    }
};

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
                   std::string& outError) {
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
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);
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
    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);
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
    const std::uint16_t version = kNavVersion, zero = 0;
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
    if (version != kNavVersion) {
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
    std::fclose(f);

    if (m_impl->mesh) {
        dtFreeNavMesh(m_impl->mesh);
        m_impl->mesh = nullptr;
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

} // namespace nl::nav
