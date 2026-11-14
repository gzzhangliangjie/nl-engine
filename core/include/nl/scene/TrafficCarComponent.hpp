#pragma once

// P8 M-T3（Q-T12=A）：ECS 交通车组件 —— NavAgentComponent 同款口径
// （docs/worldgen/TRAFFIC-PLAN.md §11）。
//
// 组件只持纯 POD 数据（route/s/speed + 演示参数），零第三方符号、零图形符号、
// 零 nl/traffic 头依赖（scene -> traffic 反向依赖禁令与 NavAgent 的 scene->nav
// 同款）：TrafficSim 运行时状态全在 TrafficCarSystem 内。nl/scene 既有的
// Component.hpp / System.hpp / World.hpp / 帧循环语义一行不改，只是新增一个
// 组件类型。
//
// 依赖方向（与 nav 先例一致）：
//   nl/scene/TrafficCarComponent.hpp  —— 纯 POD 壳（本文件）
//   nl/traffic/TrafficCarSystem.hpp   —— sim 引擎（include 本头 + traffic 头）
//
// route 由系统 init 时以 fnv1a64 纯函数确定性生成（零 RNG，TRAFFIC-PLAN §11.2），
// 组件里的 route 字段是 init 产物的落表副本（serialise roundtrip 消费）。

#include <cstdint>
#include <vector>

#include "nl/scene/Component.hpp"

namespace nl::scene {

class TrafficCarComponent : public ComponentOf<TrafficCarComponent> {
public:
    explicit TrafficCarComponent(Entity owner)
        : ComponentOf<TrafficCarComponent>(owner) {}

    // ComponentOf<T>::duplicate 消费的换 owner 深拷（vector 逐元素拷）。
    TrafficCarComponent(const TrafficCarComponent& o, Entity newOwner)
        : ComponentOf<TrafficCarComponent>(newOwner),
          route(o.route), s(o.s), speed(o.speed), variant(o.variant),
          enabled(o.enabled) {}

    static constexpr const char* kName = "TrafficCar";

    std::vector<std::int32_t> route;  // lane 序列（系统 init 回填；A* 产物副本）
    float s = 0.0f;                   // lane 内弧长（系统每步写回，展示/存档用）
    float speed = 0.0f;               // 当前速度 m/s（同上）
    std::uint32_t variant = 0;        // 预留：车型池索引（当前渲染走 Model.mesh）
    bool enabled = true;              // false = 不注册进 sim（负向/冻结用例）

    nlohmann::json serialise() const override;
    void deserialise(const nlohmann::json& j) override;
};

} // namespace nl::scene
