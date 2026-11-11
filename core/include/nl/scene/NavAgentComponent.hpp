#pragma once

// P8 M-N6（Q1=B）：E1 ECS 导航 Agent 组件。
// 口径（P8-NAV-PLAN Q6-1）：组件只持纯 POD 数据（crowd 参数 + spawn/target +
// 当前位置），零第三方符号、零图形符号、零 nl/nav 头依赖；Crowd 运行时状态
// 全在 NavAgentSystem 内。这样 nl/scene 既有的 Component.hpp / System.hpp /
// World.hpp / 帧循环语义一行不改，只是新增一个组件类型。
//
// 与 Carrot NavMeshComponent 的边界证据一致：Carrot 的组件只是壳，寻路在
// 系统侧；UE Mass 亦把移动状态放 fragment、由 processor 驱动。
//
// 组件类型 ID 走 ComponentOf<T>::typeID() 的全局递增计数器（0..63），
// 注册顺序即签名位；NavAgentSystem 用 <NavAgent, Transform> 双组件签名。

#include "nl/scene/Component.hpp"

namespace nl::scene {

class NavAgentComponent : public ComponentOf<NavAgentComponent> {
public:
    explicit NavAgentComponent(Entity owner)
        : ComponentOf<NavAgentComponent>(owner) {}

    // ComponentOf<T>::duplicate 消费的换 owner 深拷（POD 数组逐元素拷，
    // 不能用默认数组成员赋值）。
    NavAgentComponent(const NavAgentComponent& o, Entity newOwner)
        : ComponentOf<NavAgentComponent>(newOwner),
          radius(o.radius), height(o.height),
          maxAcceleration(o.maxAcceleration), maxSpeed(o.maxSpeed),
          collisionQueryRange(o.collisionQueryRange),
          pathOptimizationRange(o.pathOptimizationRange),
          separationWeight(o.separationWeight), updateFlags(o.updateFlags),
          obstacleAvoidanceType(o.obstacleAvoidanceType),
          queryFilterType(o.queryFilterType), crowdIndex(o.crowdIndex),
          enabled(o.enabled) {
        for (int k = 0; k < 3; ++k) {
            spawn[k]  = o.spawn[k];
            target[k] = o.target[k];
            pos[k]    = o.pos[k];
        }
    }

    static constexpr const char* kName = "NavAgent";

    float spawn[3]  = {0.f, 0.f, 0.f};   // 注册起点（系统 init 时消费）
    float target[3] = {0.f, 0.f, 0.f};   // 请求终点
    float pos[3]    = {0.f, 0.f, 0.f};   // 系统每步写回的当前位置

    // dtCrowdAgentParams 的本层投影（与 nl::nav::CrowdAgentParams 同数值；
    // 这里不 include nav 头，避免 scene -> nav 反向依赖）。
    float radius                 = 0.30f;
    float height                 = 2.00f;
    float maxAcceleration        = 8.00f;
    float maxSpeed               = 3.50f;
    float collisionQueryRange    = 3.60f;
    float pathOptimizationRange  = 9.00f;
    float separationWeight       = 2.00f;
    unsigned updateFlags         = 0x1fu;  // CrowdFlags::kDefault
    unsigned char obstacleAvoidanceType = 3;
    unsigned char queryFilterType       = 0;

    int  crowdIndex = -1;   // 系统 init 回填的本层顺序号
    bool enabled    = true; // false = 不注册进 Crowd（负向/冻结用例）

    nlohmann::json serialise() const override;
    void deserialise(const nlohmann::json& j) override;
};

} // namespace nl::scene
