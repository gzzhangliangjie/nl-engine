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
#include "nl/traffic/TrafficControl.hpp"
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

    // P8 M-T4（Q-T15=A）：信号调度挂接。必须在 init 之后调用（graph_ 已就绪）。
    // roadnet.json 原字节 -> TrafficControl::buildFromRoadnetJson；autoSignal
    // = 全部受控节点置 signal=true（缺省两相 8/2/1s + gap_out 0 固定周期；
    // roadnet signals[] 显式条目已在 build 时优先消费，autoSignal 只补默认）。
    // 成功后 sim_.params.control 指上 —— M-T1/T2 全部调度行为（红灯停车/
    // 占用锁/gap-out）从下一 step 起生效。失败返回 false 且 err 显式。
    bool initControl(const char* roadnetJson, size_t len, bool autoSignal,
                     std::string& err);

    // P8 M-T4（Q-T17=A）：灯实体显式注册（viewer 创建并挂 SignalLampComponent
    // + flush 之后逐灯调用；系统不自扫）。nodeId 须是受控节点（findNode>=0），
    // 否则返回 false 且 err 显式。nodeIdx 注册时缓存（writeBack 零查找）。
    bool registerLamp(nl::scene::EntityID id, std::int32_t nodeId,
                      std::int32_t edgeId, std::string& err);

    void tick(double dt) override;

    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }
    int  carCount() const { return static_cast<int>(sim_.cars.size()); }
    std::uint64_t stepCount() const { return stepCount_; }
    double totalOdometer() const;   // 全车 odometer 之和（m）
    int  arrivedCount() const;      // route 走完且停下的车数（loop=true 恒 0）
    const TrafficSim& sim() const { return sim_; }
    const TrafficControl& control() const { return control_; }   // M-T4 只读
    bool controlAttached() const { return controlAttached_; }
    int  lampCount() const { return static_cast<int>(lamps_.size()); }

private:
    void writeBack();   // sim -> 组件 s/speed + Transform position/rotation；
                        // M-T4：灯实体 stateForEdge -> ModelComponent.tint

    struct LampRef {
        nl::scene::EntityID id{};
        std::int32_t nodeId = -1;
        std::int32_t edgeId = -1;
        size_t nodeIdx = 0;   // registerLamp 时 findNode 缓存
    };

    LaneGraph graph_;    // init 拷入（值语义，系统自持）
    TrafficSim sim_;
    TrafficControl control_;   // M-T4：值持有（graph 指针指向自身 graph_）
    bool controlAttached_ = false;
    double fixedDt_ = 1.0 / 60.0;
    double accumulator_ = 0.0;
    std::uint64_t stepCount_ = 0;
    bool ready_ = false;
    std::string error_;
    std::vector<nl::scene::EntityID> ids_;   // EntityID 升序（确定性车序）
    std::vector<LampRef> lamps_;             // registerLamp 序（M-T4）
};

} // namespace nl::traffic
