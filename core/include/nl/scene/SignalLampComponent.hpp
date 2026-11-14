#pragma once

// P8 M-T4（Q-T17=A）：红绿灯摆件组件 —— TrafficCarComponent 同款口径
// （docs/worldgen/TRAFFIC-PLAN.md §12）。
//
// 灯实体由 viewer --traffic-city 分支创建（newEntity + Transform + Model +
// 本组件），flush 后逐灯 registerLamp 显式注册进 TrafficCarSystem（系统不自扫
// 纪律）；系统 writeBack 逐灯 stateForEdge 写 ModelComponent.tint（渲染零新
// 特性——每帧 objects 原地刷新含 tint）。
//
// 组件只持纯 POD 数据（nodeId/edgeId 溯源），零第三方符号、零图形符号、
// 零 nl/traffic 头依赖（scene -> traffic 反向依赖禁令与 NavAgent/TrafficCar
// 同款）。灯态查询全在 TrafficCarSystem 内。

#include <cstdint>

#include "nl/scene/Component.hpp"

namespace nl::scene {

class SignalLampComponent : public ComponentOf<SignalLampComponent> {
public:
    explicit SignalLampComponent(Entity owner)
        : ComponentOf<SignalLampComponent>(owner) {}

    SignalLampComponent(const SignalLampComponent& o, Entity newOwner)
        : ComponentOf<SignalLampComponent>(newOwner),
          nodeId(o.nodeId), edgeId(o.edgeId) {}

    static constexpr const char* kName = "SignalLamp";

    std::int32_t nodeId = -1;   // 受控 roadnet node id（TrafficControl::findNode）
    std::int32_t edgeId = -1;   // 进入边 edge id（stateForEdge 的信号查询键）

    nlohmann::json serialise() const override;
    void deserialise(const nlohmann::json& j) override;
};

} // namespace nl::scene
