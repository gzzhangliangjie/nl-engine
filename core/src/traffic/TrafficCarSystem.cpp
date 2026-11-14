// P8 M-T3：TrafficCarSystem 实现（TRAFFIC-PLAN §11；NavAgentSystem 同构纪律）。

#include "nl/traffic/TrafficCarSystem.hpp"

#include <algorithm>
#include <cmath>

namespace nl::traffic {
namespace {

// fnv1a64 —— 与 city-tools assetVariantHash 同常数同字节序口径
// （seed 8B ‖ tag 8B ‖ idx 4B，小端；零 RNG 纪律，citygen ↔ 交通接口同源）。
std::uint64_t fnv1a64(std::uint64_t seed, std::uint64_t tag, std::uint32_t idx) {
    std::uint8_t buf[20];
    for (int i = 0; i < 8; ++i)
        buf[i] = static_cast<std::uint8_t>(seed >> (8 * i));
    for (int i = 0; i < 8; ++i)
        buf[8 + i] = static_cast<std::uint8_t>(tag >> (8 * i));
    for (int i = 0; i < 4; ++i)
        buf[16 + i] = static_cast<std::uint8_t>(idx >> (8 * i));
    std::uint64_t h = 14695981039346656037ULL;
    for (std::uint8_t v : buf) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace

TrafficCarSystem::TrafficCarSystem(nl::scene::World& world)
    : TrafficCarSystemBase(world) {}

TrafficCarSystem::~TrafficCarSystem() = default;

std::unique_ptr<nl::scene::System> TrafficCarSystem::duplicate(
    nl::scene::World& newOwner) const {
    auto copy = std::make_unique<TrafficCarSystem>(newOwner);
    copy->graph_ = graph_;
    copy->sim_ = sim_;
    copy->fixedDt_ = fixedDt_;
    copy->accumulator_ = accumulator_;
    copy->stepCount_ = stepCount_;
    copy->ready_ = ready_;
    copy->error_ = error_;
    copy->ids_ = ids_;
    // sim_.graph 指针指向自身 graph_ 的地址会随拷贝失效，重建：
    if (copy->ready_) copy->sim_.graph = &copy->graph_;
    return copy;
}

bool TrafficCarSystem::init(const LaneGraph& graph, const TrafficSimParams& params,
                            std::uint64_t routeSeed, std::string& err) {
    ready_ = false;
    error_.clear();
    err.clear();

    // 1) 收集 enabled 实体（EntityID 升序 = 确定性车序；系统实体列表由 World
    //    推送维护，此处不重扫 World）。
    ids_.clear();
    for (nl::scene::Entity e : entities_) {
        const auto* comp =
            world_.getComponent<nl::scene::TrafficCarComponent>(e.getID());
        if (comp && comp->enabled) ids_.push_back(e.getID());
    }
    if (ids_.empty()) {
        err = "traffic-car system: no enabled TrafficCar entities";
        error_ = err;
        return false;
    }
    if (graph.lanes.empty()) {
        err = "traffic-car system: empty lane graph";
        error_ = err;
        return false;
    }

    // 2) 图深拷入 + sim 就绪。
    graph_ = graph;
    sim_ = TrafficSim{};
    sim_.graph = &graph_;
    sim_.params = params;
    sim_.params.loop = true;   // route 均为闭合回路（见下），永续行驶；
                               // 非闭合回落路径由 step 的逐车闭合判定兜底。

    // 3) 确定性 route 生成：起点 lane + 闭合回路终点 lane（toNode == 起点
    //    fromNode 且 fromNode != 起点 toNode，禁直接掉头回环），A* 连路。
    const int32_t laneN = graph_.laneCount();
    for (size_t ci = 0; ci < ids_.size(); ++ci) {
        const int32_t startLane =
            static_cast<int32_t>(fnv1a64(routeSeed, 0x726f7574ULL /*"rout"*/,
                                         static_cast<std::uint32_t>(ci)) %
                                 static_cast<std::uint64_t>(laneN));
        const Lane& L0 = graph_.lanes[static_cast<size_t>(startLane)];

        // 闭合候选：终点 lane 末节点 == L0 起点节点，且起点节点 != L0 末节点
        //（fromNode != L0.toNode 排除「直接掉头一条边回环」）。
        std::vector<int32_t> closed;
        for (int32_t li = 0; li < laneN; ++li) {
            const Lane& Lg = graph_.lanes[static_cast<size_t>(li)];
            if (Lg.toNode == L0.fromNode && Lg.fromNode != L0.toNode)
                closed.push_back(li);
        }

        std::vector<int32_t> route;
        bool loopRoute = false;
        if (!closed.empty()) {
            const int32_t goal =
                closed[fnv1a64(routeSeed, 0x676f616cULL /*"goal"*/,
                               static_cast<std::uint32_t>(ci)) %
                       closed.size()];
            std::string perr;
            if (findPathOnLanes(graph_, startLane, goal, &route, &perr) ==
                PathStatus::Ok) {
                loopRoute = true;   // goal.toNode == L0.fromNode = 环闭合
            }
        }
        if (!loopRoute) {
            // 回落：非闭合单程（终点 = 起点 lane 出发可直达的任意 lane）。
            const int32_t goal =
                static_cast<int32_t>(fnv1a64(routeSeed, 0x66616c6cULL /*"fall"*/,
                                             static_cast<std::uint32_t>(ci)) %
                                     static_cast<std::uint64_t>(laneN));
            std::string perr;
            const PathStatus st =
                findPathOnLanes(graph_, startLane, goal, &route, &perr);
            if (st != PathStatus::Ok) {
                err = "traffic-car system: car " + std::to_string(ci) +
                      " route failed: " + perr;
                error_ = err;
                return false;
            }
        }

        // 起步弧长散布：[0.1, 0.5]·首 lane 长（hash 决定，避免 42 辆车同点叠置）。
        const float len0 = graph_.lanes[static_cast<size_t>(route.front())].length;
        const float frac = 0.1f + static_cast<float>(
            fnv1a64(routeSeed, 0x73746172ULL /*"star"*/,
                    static_cast<std::uint32_t>(ci)) % 100u) * 0.004f;
        const float startS = len0 * frac;

        auto* comp =
            world_.getComponent<nl::scene::TrafficCarComponent>(ids_[ci]);
        std::string aerr;
        if (!sim_.addCar(route, startS, &aerr)) {
            err = "traffic-car system: car " + std::to_string(ci) +
                  " addCar failed: " + aerr;
            error_ = err;
            return false;
        }
        // route/s/speed 落表（serialise roundtrip 与诊断消费；speed 起步 0）。
        comp->route = route;
        comp->s = startS;
        comp->speed = 0.0f;
    }

    ready_ = true;
    return true;
}

void TrafficCarSystem::tick(double dt) {
    if (!ready_) return;
    // NavAgent 同款累加器：每步积分恒 fixedDt_，卡顿帧 maxSteps 防爆。
    accumulator_ += dt;
    int steps = 0;
    while (accumulator_ >= fixedDt_) {
        sim_.step();
        accumulator_ -= fixedDt_;
        ++stepCount_;
        if (++steps >= kMaxStepsPerTick) {
            accumulator_ = 0.0;   // 丢弃欠账（慢机器不追赶，保步长恒定）
            break;
        }
    }
    writeBack();
}

void TrafficCarSystem::writeBack() {
    for (size_t ci = 0; ci < ids_.size(); ++ci) {
        LanePoint p;
        float yaw = 0.0f;
        if (!sim_.carPose(ci, &p, &yaw)) continue;
        const TrafficCarState& st = sim_.cars[ci].state;

        auto* comp =
            world_.getComponent<nl::scene::TrafficCarComponent>(ids_[ci]);
        if (comp) {
            comp->s = st.s;
            comp->speed = st.speed;
        }
        auto* t = world_.getComponent<nl::scene::TransformComponent>(ids_[ci]);
        if (!t) continue;
        t->local.position = {p.x, 0.0f, p.z};
        const float hy = yaw * 0.5f;
        t->local.rotation = {0.0f, std::sin(hy), 0.0f, std::cos(hy)};
    }
}

double TrafficCarSystem::totalOdometer() const {
    double sum = 0.0;
    for (const TrafficCar& c : sim_.cars) sum += c.state.odometer;
    return sum;
}

int TrafficCarSystem::arrivedCount() const {
    int n = 0;
    for (const TrafficCar& c : sim_.cars) {
        if (c.routeIdx + 1 >= static_cast<int32_t>(c.route.size()) &&
            c.state.speed == 0.0f)
            ++n;
    }
    return n;
}

} // namespace nl::traffic
