// nl/traffic/TrafficSim.hpp — P8 M-T0.2：直线跟车仿真（traffic-m0，A 类轨道 feat/worldgen）
//
// 拍板依据 docs/worldgen/TRAFFIC-PLAN.md §0 Q-T3=C1：恒速巡航 + 前车安全间距减速
// （简单间距模型，IDM 参数面大留后续）；GAME-DEV-ORDER 第 8 步「直线跟车」。
//
// 确定性口径（phys/audio/nav 先例）：
//   - 固定 dt = 1/60、固定车序（下标序逐车更新）、零 RNG；
//   - 同输入同帧数 → 全部车辆状态逐位可复现（memcmp 锚）；
//   - 浮点运算序固定：期望速度 -> clamp 加减速 -> 位置积分 -> 车道推进。
//
// 行为口径（C1 最小集）：
//   - 期望速度 = min(当前 lane speedLimit, 同 lane 前车间距 < minGap 时的前车速度)；
//     前车 = 同 lane 上 s 更大的最近车（跨 lane 感知归第 9 步调度，不在本步）；
//   - speed += clamp(target - speed, -brake*dt, +accel*dt)（bang-bang 限幅）；
//   - s += speed*dt；s 超过 lane.length 时推进到 route 下一条 lane（s 扣除弧长），
//     route 走完 = 停车（s 钳在末 lane 末端、speed 归零；循环 demo 旗标是独立后续项）；
//   - route 末 lane 走停车减速曲线 target=sqrt(2*brake*remain)（瞬钳 = 无限 jerk，
//     跟车者物理刹不住；曲线让「前车停车、后车跟随刹停」可实现）。

#pragma once

#include "nl/traffic/LaneGraph.hpp"
#include "nl/traffic/LanePath.hpp"
#include "nl/traffic/TrafficControl.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nl::traffic {

struct TrafficSimParams {
    float dt = 1.0f / 60.0f;  // 固定逻辑步长（GameClock 60Hz 同源）
    float accel = 2.0f;       // 巡航加速 m/s^2
    float brake = 4.0f;       // 减速限幅 m/s^2
    float minGap = 2.0f;      // 同 lane 最小车距 m（中心距近似）
    bool loop = false;        // M-T2.2：route 走完且环闭合（末 lane toNode ==
                              // 首 lane fromNode）时回绕续行；false = 停靠（M-T0 行为）
};

struct TrafficCarState {
    int32_t lane = -1;   // 当前 lane 下标
    float s = 0.0f;      // lane 内弧长位置 m
    float speed = 0.0f;  // 当前速度 m/s
    float odometer = 0.0f; // 累计行驶距离 m（统计锚）
};

struct TrafficCar {
    std::vector<int32_t> route; // lane 序列（findPathOnLanes 产物，车主权）
    int32_t routeIdx = 0;       // 当前在 route 的下标
    TrafficCarState state;
    int32_t haltFrames = 0;     // 停滞帧计数（速度 0 且未完成 route；C1 拥堵统计）
};

struct TrafficSim {
    const LaneGraph* graph = nullptr;
    TrafficSimParams params;
    std::vector<TrafficCar> cars;
    // M-T1.1：路口控制面（信号相位 + 占用锁）可选挂接；nullptr = M-T0 原行为
    //（锚不重拍：既有 ⑧ 全部无 control 跑）。sim 持可变引用（advanceFrame/
    // acquire/releaseCleared 是控制面运行时状态机），dt 由 control 自带。
    TrafficControl* control = nullptr;

    // route = 车主权路径（至少 1 条 lane）；startS = 首 lane 内初始弧长（跟车锚用）；
    // 追加到车序尾部（下标序 = 更新序）
    bool addCar(const std::vector<int32_t>& route, float startS, std::string* err);

    // 推进一帧（固定车序，逐位可复现）
    void step();

    // 车辆世界坐标（lane 折线按 s 线性插值；XZ 面）
    bool carPosition(size_t carIdx, LanePoint* out) const;
};

} // namespace nl::traffic
