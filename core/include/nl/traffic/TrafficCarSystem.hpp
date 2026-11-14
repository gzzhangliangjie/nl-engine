#pragma once

// P8 M-T3（Q-T12=A）：ECS 交通车系统 —— NavAgentSystem 同构
// （docs/worldgen/TRAFFIC-PLAN.md §11）。
//
// 口径（三处对照，2026-11-14 实读）：
//   - UE ZoneGraph：图本体只有「数据 + 查询」（CalculateLocationAlongLane ≈
//     TrafficSim::carPosition 同构 API），移动由独立消费方驱动——本系统即
//     M-T0 LaneGraph/TrafficSim 的引擎内消费方。
//   - Carrot System.h：实体列表 World 推送（不自扫），tick 生命周期
//     firstTick->tick（World::tick flush 后逐系统调用）。
//   - bevy FixedMain：固定速率逻辑帧 + 命名阶段——累加器口径与 M-N6 一致。
//
// 确定性纪律（TRAFFIC-PLAN §11.2）：
//   - 固定 dt=1/60 累加器（每步积分恒 fixedDt_，卡顿帧 maxStepsPerTick_ 防爆）；
//   - route 生成零 RNG：fnv1a64(routeSeed, carIdx)（与 city-tools
//     assetVariantHash 同常数同字节序口径）选起点 lane 与闭合回路终点 lane，
//     findPathOnLanes 连路；闭合 route + TrafficSimParams.loop=true = 永续行驶；
//   - 车序 = 实体 EntityID 升序（sim addCar 追加序 = 更新序）；
//   - 同输入同 tick 数 -> 全部 Transform 逐位可复现（memcmp 锚）。
//
// 写回口径：位置 {x, 0, z} + yaw 四元数 [0, sin(yaw/2), 0, cos(yaw/2)]
// （city.scene 车实体同款；yaw = atan2(dx,dz)，模型 -z 朝前）。
// 零图形符号；统计（odometer/arrived/stepCount）以标量暴露给消费方。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nl/scene/Component.hpp"
#include "nl/scene/System.hpp"
#include "nl/scene/TrafficCarComponent.hpp"
#include "nl/scene/World.hpp"
#include "nl/traffic/LaneGraph.hpp"
#include "nl/traffic/LanePath.hpp"
#include "nl/traffic/TrafficSim.hpp"

namespace nl::traffic {

using TrafficCarSystemBase =
    nl::scene::SignedSystem<nl::scene::TrafficCarComponent,
                            nl::scene::TransformComponent>;

class TrafficCarSystem : public TrafficCarSystemBase {
public:
    static constexpr const char* kName = "TrafficCarSystem";
    static constexpr int kMaxStepsPerTick = 8;   // 卡顿帧一次最多补几步

    explicit TrafficCarSystem(nl::scene::World& world);
    ~TrafficCarSystem() override;

    const char* getName() const override { return kName; }
    std::unique_ptr<nl::scene::System> duplicate(
        nl::scene::World& newOwner) const override;

    // 必须在「实体已挂 TrafficCarComponent + World flush + 系统已 registerSystem」
    // 之后调用。graph 深拷入（值持有）；routeSeed 驱动确定性 route 生成。
    // 任何上游失败（enabled 实体 0 辆 / route 不可达 / addCar 失败）返回 false
    // 且 err 带原因，不静默吞掉。
    bool init(const LaneGraph& graph, const TrafficSimParams& params,
              std::uint64_t routeSeed, std::string& err);

    void tick(double dt) override;

    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }
    int  carCount() const { return static_cast<int>(sim_.cars.size()); }
    std::uint64_t stepCount() const { return stepCount_; }
    double totalOdometer() const;   // 全车 odometer 之和（m）
    int  arrivedCount() const;      // route 走完且停下的车数（loop=true 恒 0）
    const TrafficSim& sim() const { return sim_; }

private:
    void writeBack();   // sim -> 组件 s/speed + Transform position/rotation

    LaneGraph graph_;    // init 拷入（值语义，系统自持）
    TrafficSim sim_;
    double fixedDt_ = 1.0 / 60.0;
    double accumulator_ = 0.0;
    std::uint64_t stepCount_ = 0;
    bool ready_ = false;
    std::string error_;
    std::vector<nl::scene::EntityID> ids_;   // EntityID 升序（确定性车序）
};

} // namespace nl::traffic
