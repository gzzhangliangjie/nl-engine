// P8 M-N6：NavAgentSystem 实现。Crowd 是 PIMPL 增量链路，本 TU 只做
// 「ECS 实体 -> Crowd agent -> 逐帧驱动 -> 位置写回组件」的编排，零图形符号。

#include "nl/nav/NavAgentSystem.hpp"

#include <algorithm>
#include <cmath>

namespace nl::nav {

NavAgentSystem::NavAgentSystem(nl::scene::World& world)
    : NavAgentSystemBase(world) {}

NavAgentSystem::~NavAgentSystem() = default;

std::unique_ptr<nl::scene::System> NavAgentSystem::duplicate(
    nl::scene::World& newOwner) const {
    // 编辑器拷贝只带结构不带运行时状态：Crowd 是进程内 GPU/CPU 运行时对象，
    // 不进场景 JSON（与 M-N4 Q2=A-1「.nnav 不落 agent 状态」同口径）。
    // 新系统需由消费方重新 init(nav, cfg)。
    return std::make_unique<NavAgentSystem>(newOwner);
}

bool NavAgentSystem::init(const NavMesh& nav, const CrowdConfig& cfg,
                          std::string& err) {
    err.clear();
    ready_ = false;
    crowd_.reset();
    ids_.clear();
    crowdIdx_.clear();
    snapshot_.clear();
    targets_.clear();
    trail_.clear();
    trailCounts_.clear();
    stepCount_ = 0;
    accumulator_ = 0.0;
    minPair_ = 0.f;
    havePair_ = false;
    error_.clear();

    // World 推送来的匹配实体；显式按 ID 升序，保证注册顺序与创建顺序一致
    // （确定性输入的一部分，与 M-N4「固定 agent 初始顺序」同口径）。
    for (const auto& e : getEntities()) ids_.push_back(e.getID());
    std::sort(ids_.begin(), ids_.end());
    if (ids_.empty()) { err = "no NavAgent+Transform entities"; return false; }
    if (static_cast<int>(ids_.size()) > cfg.maxAgents) {
        err = "NavAgent count exceeds cfg.maxAgents";
        return false;
    }

    crowd_ = std::make_unique<Crowd>();
    if (!crowd_->begin(nav, cfg, err)) return false;

    std::vector<nl::scene::EntityID> active;
    for (nl::scene::EntityID id : ids_) {
        auto* a = world_.getComponent<nl::scene::NavAgentComponent>(id);
        if (!a) { err = "NavAgent component vanished"; return false; }
        // disabled 组件不注册进 Crowd（负向 / 冻结用例），也不进写回列表。
        if (!a->enabled) {
            a->crowdIndex = -1;
            continue;
        }
        active.push_back(id);
        CrowdAgentParams p;
        p.radius                = a->radius;
        p.height                = a->height;
        p.maxAcceleration       = a->maxAcceleration;
        p.maxSpeed              = a->maxSpeed;
        p.collisionQueryRange   = a->collisionQueryRange;
        p.pathOptimizationRange = a->pathOptimizationRange;
        p.separationWeight      = a->separationWeight;
        p.updateFlags           = a->updateFlags;
        p.obstacleAvoidanceType = a->obstacleAvoidanceType;
        p.queryFilterType       = a->queryFilterType;

        const int local = crowd_->addAgent(a->spawn, a->target, p, err);
        if (local < 0) return false;
        a->crowdIndex = local;
        crowdIdx_.push_back(local);
        // 抵达判定用原始请求终点（arena 上 findNearestPoly 吸附不改变 XZ；
        // 与 M-N4 用吸附值的差异留痕，不影响抵达计数）。
        for (int k = 0; k < 3; ++k) targets_.push_back(a->target[k]);
    }
    if (crowdIdx_.empty()) { err = "no enabled NavAgent"; return false; }
    // ids_ 与 crowdIdx_ 逐位对齐（capturePositions 直接按 i 索引两表）。
    ids_ = std::move(active);

    const int n = agentCount();
    snapshot_.assign(static_cast<std::size_t>(n) * 3, 0.f);
    trail_.assign(static_cast<std::size_t>(n) * kMaxTrail * 2, 0.f);
    trailCounts_.assign(static_cast<std::size_t>(n), 0);
    fixedDt_ = cfg.dt;
    arrivalRadius_ = cfg.arrivalRadius;

    // 初始位置写回组件（第 0 帧），并采首个轨迹点。
    capturePositions();
    sampleTrail();
    updatePairDistance();
    ready_ = true;
    return true;
}

void NavAgentSystem::capturePositions() {
    const int n = agentCount();
    for (int i = 0; i < n; ++i) {
        float p[3] = {0.f, 0.f, 0.f};
        if (!crowd_->agentPosition(crowdIdx_[i], p)) continue;
        for (int k = 0; k < 3; ++k) snapshot_[static_cast<std::size_t>(i) * 3 + k] = p[k];
        if (i < static_cast<int>(ids_.size())) {
            auto* a = world_.getComponent<nl::scene::NavAgentComponent>(ids_[i]);
            auto* t = world_.getComponent<nl::scene::TransformComponent>(ids_[i]);
            if (a) for (int k = 0; k < 3; ++k) a->pos[k] = p[k];
            // Q1=B 的实质：Crowd 位置回到 E1 Transform（渲染/脚本消费同一份数据）。
            if (t) t->local.position = {p[0], p[1], p[2]};
        }
    }
}

void NavAgentSystem::updatePairDistance() {
    const int n = agentCount();
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const float* a = &snapshot_[static_cast<std::size_t>(i) * 3];
            const float* b = &snapshot_[static_cast<std::size_t>(j) * 3];
            const float dx = a[0] - b[0], dz = a[2] - b[2];
            const float d = std::sqrt(dx * dx + dz * dz);
            if (!havePair_ || d < minPair_) { minPair_ = d; havePair_ = true; }
        }
    }
}

void NavAgentSystem::sampleTrail() {
    const int n = agentCount();
    for (int i = 0; i < n; ++i) {
        int& c = trailCounts_[i];
        if (c >= kMaxTrail) continue;
        const float* p = &snapshot_[static_cast<std::size_t>(i) * 3];
        const std::size_t at =
            (static_cast<std::size_t>(i) * kMaxTrail + c) * 2;
        trail_[at + 0] = p[0];
        trail_[at + 1] = p[2];
        ++c;
    }
}

void NavAgentSystem::tick(double dt) {
    if (!ready_) return;
    if (!(dt > 0.0) || !std::isfinite(dt)) return;
    accumulator_ += dt;
    int budget = maxStepsPerTick_;
    while (budget-- > 0 && accumulator_ + 1e-9 >= fixedDt_) {
        accumulator_ -= fixedDt_;
        std::string err;
        if (!crowd_->step(err)) {
            error_ = err;
            ready_ = false;
            return;
        }
        ++stepCount_;
        capturePositions();
        updatePairDistance();
        if (stepCount_ % kSampleEvery == 0) sampleTrail();
    }
}

int NavAgentSystem::arrivedCount() const {
    const int n = agentCount();
    const float r2 = arrivalRadius_ * arrivalRadius_;
    int arrived = 0;
    for (int i = 0; i < n; ++i) {
        const float dx = snapshot_[static_cast<std::size_t>(i) * 3 + 0] -
                         targets_[static_cast<std::size_t>(i) * 3 + 0];
        const float dz = snapshot_[static_cast<std::size_t>(i) * 3 + 2] -
                         targets_[static_cast<std::size_t>(i) * 3 + 2];
        if (dx * dx + dz * dz <= r2) ++arrived;
    }
    return arrived;
}

float NavAgentSystem::minPairDistance() const { return havePair_ ? minPair_ : 0.f; }

} // namespace nl::nav
