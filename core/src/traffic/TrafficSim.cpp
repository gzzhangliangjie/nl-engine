// nl/traffic/TrafficSim.cpp — P8 M-T0.2：直线跟车仿真（C1 口径，头注见hpp）
#include "nl/traffic/TrafficSim.hpp"

#include <cmath>

namespace nl::traffic {

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
    // 固定车序逐车更新（下标序 = 确定性锚；同帧后车看到的是前车上一帧末状态）
    for (TrafficCar& car : cars) {
        TrafficCarState& st = car.state;
        const Lane& lane = graph->lanes[static_cast<size_t>(st.lane)];

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

        // route 末端停车曲线：remain = 末 lane 剩余弧长，
        // target = min(target, sqrt(2*brake*remain))——以 brake 减速度恰好停在末端
        //（瞬钳停车 = 无限 jerk，跟车者物理上刹不住；曲线停车让前车行为可实现）
        if (car.routeIdx + 1 == car.route.size()) {
            const float remain = lane.length - st.s;
            const float stopV = std::sqrt(2.0f * params.brake * (remain > 0.0f ? remain : 0.0f));
            if (stopV < target) target = stopV;
        }

        // bang-bang 限幅 -> 位置积分 -> 车道推进（运算序固定）
        const float dSpeed = target - st.speed;
        const float maxD = params.accel * params.dt;
        const float maxB = params.brake * params.dt;
        st.speed += (dSpeed > maxD) ? maxD : (dSpeed < -maxB ? -maxB : dSpeed);
        if (st.speed < 0.0f) st.speed = 0.0f;
        st.s += st.speed * params.dt;
        st.odometer += st.speed * params.dt;

        // lane 推进：route 内前进；route 走完 = 停车钳位（循环旗标独立后续项）
        while (st.s > lane.length && car.routeIdx + 1 < car.route.size()) {
            st.s -= lane.length;
            ++car.routeIdx;
            st.lane = car.route[static_cast<size_t>(car.routeIdx)];
        }
        const Lane& cur = graph->lanes[static_cast<size_t>(st.lane)];
        if (st.s > cur.length) {
            st.s = cur.length; // route 末端钳位
            st.speed = 0.0f;
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
            out->z = a.z + dz * t;
            return true;
        }
        remain -= seg;
    }
    *out = lane.points.back();
    return true;
}

} // namespace nl::traffic
