// nl/traffic/TrafficSim.cpp — P8 M-T0.2：直线跟车仿真（C1 口径，头注见hpp）
#include "nl/traffic/TrafficSim.hpp"

#include <cmath>

namespace nl::traffic {

namespace {

// M-T2.2：loop 有效 = 开关开 + route 成环（末 lane toNode == 首 lane fromNode）
bool loopActive(const TrafficSim& sim, const TrafficCar& car) {
    if (!sim.params.loop || car.route.size() < 2) return false;
    const LaneGraph& g = *sim.graph;
    return g.lanes[static_cast<size_t>(car.route.back())].toNode ==
           g.lanes[static_cast<size_t>(car.route.front())].fromNode;
}

} // namespace

bool TrafficSim::addCar(const std::vector<int32_t>& route, float startS, std::string* err) {
    if (err) err->clear();
    if (!graph || route.empty()) {
        if (err) *err = "sim not bound or empty route";
        return false;
    }
    for (const int32_t id : route) {
        if (id < 0 || id >= graph->laneCount()) {
            if (err) *err = "route lane index out of range";
            return false;
        }
    }
    const float firstLen = graph->lanes[static_cast<size_t>(route.front())].length;
    if (!(startS >= 0.0f) || startS > firstLen) {
        if (err) *err = "startS out of first lane range";
        return false;
    }
    TrafficCar car;
    car.route = route;
    car.routeIdx = 0;
    car.state.lane = route.front();
    car.state.s = startS;
    car.state.speed = 0.0f;
    car.state.odometer = 0.0f;
    cars.push_back(car);
    return true;
}

void TrafficSim::step() {
    // 相位时钟先推进（第 1 次车辆更新看到 frame=1，即 t=dt；确定性锚按此口径）
    if (control) control->advanceFrame();

    // 固定车序逐车更新（下标序 = 确定性锚；同帧后车看到的是前车上一帧末状态；
    // 同帧冲突申请按车序先到先得 = control.acquire 的 FIFO tie-break）
    const int32_t carCount = static_cast<int32_t>(cars.size());
    for (int32_t ci = 0; ci < carCount; ++ci) {
        TrafficCar& car = cars[static_cast<size_t>(ci)];
        TrafficCarState& st = car.state;
        const Lane& lane = graph->lanes[static_cast<size_t>(st.lane)];

        // route 末停靠帽：终点 lane 的 toNode 受控时停在停车线（不进路口区停靠，
        // 否则终点车泊在节点圆心与穿行车穿模）。停车线自适应：length -
        // min(entryStop, 0.6*length)——密集路网（W0 城 4m lane）上固定 2.5m
        // 停车线吃满 lane，排队车永远压在上游节点释放圈内持锁不放（M-T1 根修）。
        // M-T2.2：loop 环上无「route 末」，永不停靠。
        const bool looping = loopActive(*this, car);
        float parkCap = lane.length;
        const bool atRouteEnd =
            car.routeIdx + 1 == car.route.size() && !looping;
        if (control && atRouteEnd) {
            const int32_t nIdx = control->findNode(lane.toNode);
            if (nIdx >= 0)
                parkCap = lane.length -
                          std::fmin(control->entryStop, 0.6f * lane.length);
        }

        // 期望速度：巡航 = lane 限速；跟车律 = sqrt(brake * max(0, gap - minGap))
        //（以 brake 减速度恰好停在 minGap 跟随点；比「匹配前车速度」式稳定——
        // 后者在前车停车曲线 + 后车自由段再加速下形成 limit cycle 间距棘轮下滑）。
        // 离散余量：s 每帧滞后 <= v*dt，minGap 下方穿越实测有界。
        float target = lane.speedLimit;
        float leaderS = -1.0f;
        for (const TrafficCar& other : cars) {
            if (&other == &car) continue;
            const TrafficCarState& ost = other.state;
            if (ost.lane != st.lane) continue;
            if (ost.s > st.s && (leaderS < 0.0f || ost.s < leaderS)) {
                leaderS = ost.s;
            }
        }
        if (leaderS >= 0.0f) {
            const float gap = leaderS - st.s;
            const float followV =
                std::sqrt(params.brake * (gap > params.minGap ? gap - params.minGap : 0.0f));
            if (followV < target) target = followV;
        }

        // route 末端停车曲线：remain = 停靠帽剩余弧长，
        // target = min(target, sqrt(2*brake*remain))——以 brake 减速度恰好停在帽位
        //（瞬钳停车 = 无限 jerk，跟车者物理上刹不住；曲线停车让前车行为可实现）
        if (atRouteEnd) {
            const float remain = parkCap - st.s;
            const float stopV = std::sqrt(2.0f * params.brake * (remain > 0.0f ? remain : 0.0f));
            if (stopV < target) target = stopV;
        }

        // M-T1.1 路口门控：未到 route 末、前方节点受控时，穿行须持锁；
        // 信号节点黄/红不受理新申请（持锁在穿的穿完），lock-only 节点只看锁。
        // 申请前瞻 = 刹车距离 + minGap（早申请保绿波直行不添停顿；被拒才压停车线）。
        float stopS = lane.length; // 停车线（未受控 = lane 末端，硬钳退化为无操作）
        bool mayCross = true;
        if (control && !atRouteEnd) {
            const int32_t nIdx = control->findNode(lane.toNode);
            if (nIdx >= 0) {
                stopS = lane.length -
                        std::fmin(control->entryStop, 0.6f * lane.length);
                if (!control->hasAccess(static_cast<size_t>(nIdx), ci)) {
                    mayCross = false;
                    const float remain = stopS - st.s;
                    const float brakeDist =
                        st.speed * st.speed / (2.0f * params.brake) + params.minGap;
                    const bool atLine = st.s >= stopS - 1e-3f;
                    // 死锁闸 1：同 lane 前方有车时不允许前瞻申请——锁只能被
                    // 「当前能流动的车」持有（前瞻拿锁后被前车堵住 = 循环等待）。
                    // 有前车的车到线再申请；前车停车在帽位时它物理到不了线，
                    // 永不入队不持锁（等而不堵路口）。
                    const bool earlyOk = leaderS < 0.0f && remain <= brakeDist;
                    // M-T2.2：next lane（loop 环上回绕取模）
                    const size_t nextPos = static_cast<size_t>(car.routeIdx + 1) %
                                           car.route.size();
                    const int32_t nextLaneId = car.route[nextPos];
                    const float keepClear =
                        params.minGap + control->nodes[static_cast<size_t>(nIdx)].clearEff;
                    bool exitBlocked = false;
                    for (const TrafficCar& other : cars) {
                        if (&other == &car) continue;
                        if (other.state.lane == nextLaneId &&
                            other.state.s < keepClear) {
                            exitBlocked = true;
                            break;
                        }
                    }
                    // M-T2.0 转弯判定：当前 lane vs next lane 行进方向夹角
                    // cos < 0.7071（偏转 >= 45°）= 转弯 -> 节点独占
                    //（1 车道路网左转穿对向直行；掉头被 M-T0.0 构造禁掉）
                    const Lane& nextLane =
                        graph->lanes[static_cast<size_t>(nextLaneId)];
                    const float fdx = lane.points.back().x - lane.points.front().x;
                    const float fdz = lane.points.back().z - lane.points.front().z;
                    const float ndx = nextLane.points.back().x - nextLane.points.front().x;
                    const float ndz = nextLane.points.back().z - nextLane.points.front().z;
                    const float flen = std::sqrt(fdx * fdx + fdz * fdz);
                    const float nlen = std::sqrt(ndx * ndx + ndz * ndz);
                    const bool isTurn =
                        flen > 0.0f && nlen > 0.0f &&
                        (fdx * ndx + fdz * ndz) / (flen * nlen) < 0.7071f;
                    const int group = control->groupOfEdge(
                        static_cast<size_t>(nIdx), lane.edgeId);
                    if ((atLine || earlyOk) && !exitBlocked &&
                        control->stateForEdge(static_cast<size_t>(nIdx), lane.edgeId) ==
                            SignalState::Green) {
                        mayCross = control->acquire(static_cast<size_t>(nIdx), ci,
                                                    group, isTurn);
                    }
                    if (!mayCross) {
                        if (atLine) // M-T2.1：在线等待 = gap-out 需求数据源
                            control->reportWaiting(static_cast<size_t>(nIdx),
                                                   lane.edgeId);
                        // 未获准：向停车线收敛（刹车曲线）+ 硬钳 backstop
                        const float lineV = std::sqrt(
                            2.0f * params.brake * (remain > 0.0f ? remain : 0.0f));
                        if (lineV < target) target = lineV;
                    }
                }
            }
        }

        // bang-bang 限幅 -> 位置积分 -> 硬钳 -> 车道推进（运算序固定）
        const float dSpeed = target - st.speed;
        const float maxD = params.accel * params.dt;
        const float maxB = params.brake * params.dt;
        st.speed += (dSpeed > maxD) ? maxD : (dSpeed < -maxB ? -maxB : dSpeed);
        if (st.speed < 0.0f) st.speed = 0.0f;
        st.s += st.speed * params.dt;
        st.odometer += st.speed * params.dt;
        if (!mayCross && st.s > stopS) {
            st.s = stopS; // 禁入硬钳（离散超调 backstop；确定性钳位不依赖刹车精度）
            st.speed = 0.0f;
        }
        if (atRouteEnd && st.s > parkCap) {
            st.s = parkCap; // route 末停靠帽（同上）
            st.speed = 0.0f;
        }

        // lane 推进：route 内前进；route 走完 = 停车钳位。
        // M-T2.2：loop 环闭合时 routeIdx 回绕续行（车流永不落停）。
        while (st.s > lane.length &&
               (car.routeIdx + 1 < car.route.size() || looping)) {
            st.s -= lane.length;
            car.routeIdx = static_cast<int32_t>(
                (static_cast<size_t>(car.routeIdx) + 1) % car.route.size());
            st.lane = car.route[static_cast<size_t>(car.routeIdx)];
        }
        const Lane& cur = graph->lanes[static_cast<size_t>(st.lane)];
        if (st.s > cur.length) {
            st.s = cur.length; // route 末端钳位
            st.speed = 0.0f;
        }

        // 停滞统计（C1 拥堵锚数据源）：速度 0 且未到停靠帽
        if (st.speed == 0.0f &&
            !(atRouteEnd && st.s >= parkCap - 1e-3f)) {
            ++car.haltFrames;
        }

        // 持锁车离开路口区（中心距 > clearRadius）则释放（供同帧后车申请）
        if (control) {
            LanePoint p;
            if (carPosition(static_cast<size_t>(ci), &p))
                control->releaseCleared(ci, p.x, p.z);
        }
    }
}

bool TrafficSim::carPosition(size_t carIdx, LanePoint* out) const {
    if (carIdx >= cars.size() || !out) return false;
    const TrafficCarState& st = cars[carIdx].state;
    const Lane& lane = graph->lanes[static_cast<size_t>(st.lane)];
    if (lane.points.size() < 2) return false;
    // 折线逐段弧长插值（当前实现 lane 折线恒 2 点；多点折线此处已按段通写）
    float remain = st.s;
    for (size_t i = 0; i + 1 < lane.points.size(); ++i) {
        const LanePoint& a = lane.points[i];
        const LanePoint& b = lane.points[i + 1];
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float seg = std::sqrt(dx * dx + dz * dz);
        if (seg <= 0.0f) continue;
        if (remain <= seg || i + 2 == lane.points.size()) {
            const float t = (remain > seg ? 1.0f : remain / seg);
            out->x = a.x + dx * t;
            out->y = a.y + (b.y - a.y) * t;   // M-RD-LAYER：桥面/匝道高程
            out->z = a.z + dz * t;
            return true;
        }
        remain -= seg;
    }
    *out = lane.points.back();
    return true;
}

// M-T3 纯新增：carPosition 的插值逻辑 + 所在段行进方向。与 carPosition 逐位
// 同源（同一段循环），只在命中段多算 yaw；失败语义一致。
bool TrafficSim::carPose(size_t carIdx, LanePoint* outPos, float* outYaw) const {
    if (carIdx >= cars.size()) return false;
    const TrafficCarState& st = cars[carIdx].state;
    const Lane& lane = graph->lanes[static_cast<size_t>(st.lane)];
    if (lane.points.size() < 2) return false;
    float remain = st.s;
    for (size_t i = 0; i + 1 < lane.points.size(); ++i) {
        const LanePoint& a = lane.points[i];
        const LanePoint& b = lane.points[i + 1];
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float seg = std::sqrt(dx * dx + dz * dz);
        if (seg <= 0.0f) continue;
        if (remain <= seg || i + 2 == lane.points.size()) {
            const float t = (remain > seg ? 1.0f : remain / seg);
            if (outPos) {
                outPos->x = a.x + dx * t;
                outPos->z = a.z + dz * t;
            }
            if (outYaw) *outYaw = std::atan2(dx, dz);
            return true;
        }
        remain -= seg;
    }
    if (outPos) *outPos = lane.points.back();
    if (outYaw) {
        const LanePoint& a = lane.points[lane.points.size() - 2];
        const LanePoint& b = lane.points.back();
        *outYaw = std::atan2(b.x - a.x, b.z - a.z);
    }
    return true;
}

} // namespace nl::traffic
