#pragma once

// P8 M-N6（Q1=B）：E1 ECS 系统驱动 DetourCrowd 多 Agent。
// 口径（P8-NAV-PLAN Q6-1）：
//   - SignedSystem<NavAgentComponent, TransformComponent>：实体由 World 按签名
//     推送，系统不自扫（Carrot System 口径）；World::tick(dt) 每帧调用本系统。
//   - dt 累加器固定步长：每步积分恒为 cfg.dt（1/60），累加器只决定「这一帧
//     走几步」→ 与 M-N4 Q3=A 的固定 dt + 固定帧数确定性口径完全一致。
//   - 每步把 Crowd 的 npos 写回 NavAgentComponent::pos 与
//     TransformComponent::local.position（引擎集成版的实质：位置回到组件）。
//   - 零图形符号；轨迹 / 位置快照以 POD vector<float> 暴露给消费方
//    （nav-tools 出证据图、viewer 填 ImDrawList 画布），本层不画任何东西。
//   - 不改 nl/scene 既有组件 / World / 帧循环源码；本系统只是被注册的一个新系统。

#include <memory>
#include <string>
#include <vector>

#include "nl/nav/Crowd.hpp"
#include "nl/nav/Nav.hpp"
#include "nl/scene/Component.hpp"
#include "nl/scene/NavAgentComponent.hpp"
#include "nl/scene/System.hpp"
#include "nl/scene/World.hpp"

namespace nl::nav {

using NavAgentSystemBase =
    nl::scene::SignedSystem<nl::scene::NavAgentComponent,
                            nl::scene::TransformComponent>;

class NavAgentSystem : public NavAgentSystemBase {
public:
    static constexpr const char* kName = "NavAgentSystem";
    static constexpr int kMaxTrail = 32;       // 每 agent 轨迹采样点上限
    static constexpr int kSampleEvery = 16;    // 每 N 个固定步采一次

    explicit NavAgentSystem(nl::scene::World& world);
    ~NavAgentSystem() override;

    const char* getName() const override { return kName; }
    std::unique_ptr<nl::scene::System> duplicate(
        nl::scene::World& newOwner) const override;

    // 必须在「实体已创建 + 组件已挂 + World 已 flush + 系统已 registerSystem」
    // 之后调用：按 EntityID 升序注册（确定性输入顺序），任何上游失败都返回
    // false 且 err 带原因，不静默吞掉 findNearestPoly / addAgent 的失败。
    bool init(const NavMesh& nav, const CrowdConfig& cfg, std::string& err);

    void tick(double dt) override;

    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }
    int  stepCount() const { return stepCount_; }
    int  agentCount() const { return static_cast<int>(crowdIdx_.size()); }
    const std::vector<float>& snapshot() const { return snapshot_; }  // agentCount*3
    const std::vector<float>& targets() const { return targets_; }   // agentCount*3
    const std::vector<float>& trail() const { return trail_; }       // [agent][sample][xy]
    const std::vector<int>& trailCounts() const { return trailCounts_; }
    int  arrivedCount() const;      // 末帧 XZ 距 target <= arrivalRadius 的数量
    float minPairDistance() const;  // 全程任意两 agent 最小 XZ 距离（m）

private:
    void capturePositions();   // Crowd -> 组件 pos / Transform / snapshot_
    void updatePairDistance();
    void sampleTrail();

    std::unique_ptr<Crowd> crowd_;
    double fixedDt_ = 1.0 / 60.0;
    double accumulator_ = 0.0;
    int   maxStepsPerTick_ = 8;     // 防止卡顿帧一次补太多步
    int   stepCount_ = 0;
    bool  ready_ = false;
    bool  havePair_ = false;
    float minPair_ = 0.f;
    float arrivalRadius_ = 0.50f;
    std::string error_;
    std::vector<nl::scene::EntityID> ids_;
    std::vector<int> crowdIdx_;
    std::vector<float> snapshot_;       // agentCount*3
    std::vector<float> targets_;        // agentCount*3
    std::vector<float> trail_;          // agentCount*kMaxTrail*2
    std::vector<int>   trailCounts_;
};

} // namespace nl::nav
