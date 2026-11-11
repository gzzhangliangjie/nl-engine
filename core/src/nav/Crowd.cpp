// P8 M-N4：nl::nav::Crowd 实现。DetourCrowd / Detour 符号只出现在本 TU
//（PIMPL 纪律：Crowd.hpp 零第三方符号，与 Nav.cpp 同款）。
// 上游口径：RecastDemo/Source/CrowdTool.cpp:644-675（dtCrowdAgentParams 全套参数 +
// addAgent + requestMoveTarget）、DetourCrowd.h:249/318（init / update）。
// 确定性：上游 DetourCrowd.cpp 全文无 RNG，纯单线程固定步长积分；本项目把
// dt / 帧数 / agent 顺序 / 全部参数显式化，轨迹逐帧采样 npos[3]，同输入逐字节一致。

#include "nl/nav/Crowd.hpp"
#include "nl/nav/Nav.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

namespace nl::nav {

struct Crowd::Impl {
    dtCrowd*          crowd = nullptr;
    dtNavMeshQuery*   query = nullptr;
    std::vector<int>  indices;   // 注册顺序（确定性输入的一部分）
    int               agentCount = 0;

    ~Impl() { release(); }
    void release() {
        // 外部 query 先释放；dtCrowd::purge() 只管自己内部的 navquery。
        if (query) { dtFreeNavMeshQuery(query); query = nullptr; }
        if (crowd) { dtFreeCrowd(crowd);       crowd = nullptr; }
        indices.clear();
        agentCount = 0;
    }
};

Crowd::Crowd() : m_impl(new Impl()) {}
Crowd::~Crowd() { delete m_impl; }

int Crowd::agentCount() const {
    return m_impl ? m_impl->agentCount : 0;
}

namespace {

bool finite(float v) { return std::isfinite(v) != 0; }

bool validConfig(const CrowdConfig& cfg, std::size_t spawnCount,
                 std::string& outError) {
    const CrowdAgentParams& a = cfg.agent;
    auto bad = [&](const char* what) {
        outError = what;
        return false;
    };
    if (cfg.maxAgents < 1 || cfg.maxAgents > 256)
        return bad("maxAgents must be in [1, 256]");
    if (!finite(cfg.maxAgentRadius) || !(cfg.maxAgentRadius > 0.f))
        return bad("maxAgentRadius must be > 0");
    if (!finite(cfg.dt) || !(cfg.dt > 0.f) || cfg.dt > 0.25f)
        return bad("dt must be in (0, 0.25] (fixed step, Q3=A)");
    if (cfg.frames < 1 || cfg.frames > 10000)
        return bad("frames must be in [1, 10000] (fixed count, Q3=A)");
    if (spawnCount < 1 || spawnCount > static_cast<std::size_t>(cfg.maxAgents))
        return bad("spawn count must be in [1, maxAgents] (fixed order)");
    if (!finite(cfg.arrivalRadius) || cfg.arrivalRadius < 0.f)
        return bad("arrivalRadius must be >= 0");
    for (int k = 0; k < 3; ++k)
        if (!finite(cfg.targetHalfExtents[k]) || cfg.targetHalfExtents[k] <= 0.f)
            return bad("targetHalfExtents must all be > 0");
    if (!finite(a.radius) || a.radius < 0.f || a.radius > cfg.maxAgentRadius)
        return bad("agent.radius must be in [0, maxAgentRadius]");
    if (!finite(a.height) || a.height <= 0.f)
        return bad("agent.height must be > 0");
    if (!finite(a.maxAcceleration) || a.maxAcceleration < 0.f ||
        !finite(a.maxSpeed) || a.maxSpeed < 0.f)
        return bad("agent.maxAcceleration/maxSpeed must be >= 0");
    if (!finite(a.collisionQueryRange) || a.collisionQueryRange <= 0.f ||
        !finite(a.pathOptimizationRange) || a.pathOptimizationRange <= 0.f ||
        !finite(a.separationWeight) || a.separationWeight < 0.f)
        return bad("agent ranges/separationWeight invalid");
    if ((a.updateFlags & ~CrowdFlags::kAll) != 0u)
        return bad("agent.updateFlags has unknown bits");
    if (a.obstacleAvoidanceType >= DT_CROWD_MAX_OBSTAVOIDANCE_PARAMS)
        return bad("agent.obstacleAvoidanceType out of range");
    if (a.queryFilterType >= DT_CROWD_MAX_QUERY_FILTER_TYPE)
        return bad("agent.queryFilterType out of range");
    return true;
}

// 本层位标志 -> 上游 UpdateFlags（数值一致，但显式映射避免依赖上游枚举布局）。
unsigned char mapUpdateFlags(unsigned flags) {
    unsigned char v = 0;
    if (flags & CrowdFlags::kAnticipateTurns)   v |= DT_CROWD_ANTICIPATE_TURNS;
    if (flags & CrowdFlags::kObstacleAvoidance) v |= DT_CROWD_OBSTACLE_AVOIDANCE;
    if (flags & CrowdFlags::kSeparation)        v |= DT_CROWD_SEPARATION;
    if (flags & CrowdFlags::kOptimizeVis)       v |= DT_CROWD_OPTIMIZE_VIS;
    if (flags & CrowdFlags::kOptimizeTopo)      v |= DT_CROWD_OPTIMIZE_TOPO;
    return v;
}

} // namespace

bool Crowd::run(const NavMesh& nav, const CrowdConfig& cfg,
                const std::vector<CrowdSpawn>& spawns,
                CrowdTrace& out, std::string& outError) {
    out = CrowdTrace{};
    m_impl->release();
    outError.clear();

    if (!validConfig(cfg, spawns.size(), outError)) return false;

    // A-1：只消费已加载 NavMesh 的 opaque 句柄；.nnav 格式与字节基线一字不动。
    // dtCrowd::init 需要非 const dtNavMesh*（内部只用于查询，不改写网格）。
    dtNavMesh* mesh =
        static_cast<dtNavMesh*>(const_cast<void*>(nav.nativeHandle()));
    if (!mesh) {
        outError = "navmesh not loaded (load/bake a .nnav first)";
        return false;
    }

    dtCrowd* crowd = dtAllocCrowd();
    if (!crowd) { outError = "dtAllocCrowd failed"; return false; }
    if (!crowd->init(cfg.maxAgents, cfg.maxAgentRadius, mesh)) {
        dtFreeCrowd(crowd);
        outError = "dtCrowd::init failed";
        return false;
    }
    m_impl->crowd = crowd;

    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    if (!query || dtStatusFailed(query->init(mesh, 2048))) {
        if (query) dtFreeNavMeshQuery(query);
        outError = "crowd target query init failed";
        return false;
    }
    m_impl->query = query;

    dtQueryFilter filter;  // 上游默认 includeFlags=0xffff / excludeFlags=0
    dtCrowdAgentParams ap;
    std::memset(&ap, 0, sizeof(ap));
    ap.radius                = cfg.agent.radius;
    ap.height                = cfg.agent.height;
    ap.maxAcceleration       = cfg.agent.maxAcceleration;
    ap.maxSpeed              = cfg.agent.maxSpeed;
    ap.collisionQueryRange   = cfg.agent.collisionQueryRange;
    ap.pathOptimizationRange = cfg.agent.pathOptimizationRange;
    ap.separationWeight      = cfg.agent.separationWeight;
    ap.updateFlags           = mapUpdateFlags(cfg.agent.updateFlags);
    ap.obstacleAvoidanceType = cfg.agent.obstacleAvoidanceType;
    ap.queryFilterType       = cfg.agent.queryFilterType;
    ap.userData              = nullptr;

    out.targets.reserve(spawns.size() * 3);
    for (std::size_t k = 0; k < spawns.size(); ++k) {
        dtPolyRef startRef = 0, targetRef = 0;
        float startNearest[3] = {0.f, 0.f, 0.f};
        float targetNearest[3] = {0.f, 0.f, 0.f};
        dtStatus st = query->findNearestPoly(spawns[k].pos, cfg.targetHalfExtents,
                                             &filter, &startRef, startNearest);
        if (dtStatusFailed(st) || startRef == 0) {
            outError = "spawn #" + std::to_string(k) + " not on navmesh";
            return false;
        }
        st = query->findNearestPoly(spawns[k].target, cfg.targetHalfExtents,
                                    &filter, &targetRef, targetNearest);
        if (dtStatusFailed(st) || targetRef == 0) {
            outError = "target #" + std::to_string(k) + " not on navmesh";
            return false;
        }

        const int idx = crowd->addAgent(startNearest, &ap);
        if (idx < 0) {
            outError = "addAgent #" + std::to_string(k) + " failed";
            return false;
        }
        if (!crowd->requestMoveTarget(idx, targetRef, targetNearest)) {
            outError = "requestMoveTarget #" + std::to_string(k) + " failed";
            return false;
        }
        m_impl->indices.push_back(idx);
        // 容器内存吸附后的目标：抵达判定与轨迹自洽（原始请求值留在工具日志里）。
        out.targets.push_back(targetNearest[0]);
        out.targets.push_back(targetNearest[1]);
        out.targets.push_back(targetNearest[2]);
    }

    const int n = static_cast<int>(m_impl->indices.size());
    m_impl->agentCount = n;
    out.agentCount = n;
    out.frames = cfg.frames + 1;   // 初始帧 + 每次 update 一帧
    out.dt = cfg.dt;
    out.samples.reserve(static_cast<std::size_t>(out.frames) * n * 3);

    auto sample = [&]() {
        for (int idx : m_impl->indices) {
            const dtCrowdAgent* ag = crowd->getAgent(idx);
            out.samples.push_back(ag->npos[0]);
            out.samples.push_back(ag->npos[1]);
            out.samples.push_back(ag->npos[2]);
        }
    };
    sample();
    for (int f = 0; f < cfg.frames; ++f) {
        crowd->update(cfg.dt, nullptr);  // 固定 dt；debug 信息不需要
        sample();
    }

    // 末帧位置 + 抵达判定（XZ 平面距离）
    const std::size_t last = static_cast<std::size_t>(out.frames - 1) * n * 3;
    out.finalPos.assign(out.samples.begin() + last,
                        out.samples.begin() + last + static_cast<std::ptrdiff_t>(n * 3));
    const float r2 = cfg.arrivalRadius * cfg.arrivalRadius;
    for (int i = 0; i < n; ++i) {
        const float dx = out.finalPos[i * 3 + 0] - out.targets[i * 3 + 0];
        const float dz = out.finalPos[i * 3 + 2] - out.targets[i * 3 + 2];
        if (dx * dx + dz * dz <= r2) ++out.arrivedCount;
    }

    // 全程两两最小 XZ 距离（群体避障语义：不重叠）
    bool have = false;
    float minD = 0.f;
    for (int f = 0; f < out.frames; ++f) {
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                const float* a = &out.samples[(static_cast<std::size_t>(f) * n + i) * 3];
                const float* b = &out.samples[(static_cast<std::size_t>(f) * n + j) * 3];
                const float dx = a[0] - b[0], dz = a[2] - b[2];
                const float d = std::sqrt(dx * dx + dz * dz);
                if (!have || d < minD) { minD = d; have = true; }
            }
        }
    }
    out.minPairDistance = have ? minD : 0.f;
    return true;
}

} // namespace nl::nav
