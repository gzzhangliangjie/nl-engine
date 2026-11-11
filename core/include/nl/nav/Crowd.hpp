#pragma once

// P8 M-N4：DetourCrowd 群体避障薄封装（vendored Recast/Detour 1.6.0 之上）。
// 纪律（P8-NAV-PLAN 2026-11-11 M-N4 拍板 A / A-1 / A）：
//   - 本头文件零第三方符号：dtCrowd / dtNavMesh / dtPolyRef 全挡在
//     core/src/nav/Crowd.cpp 内（PIMPL，与 Nav.hpp 同款）。
//   - A-1 容器口径：Crowd 只消费已加载的 NavMesh（.nnav v1 solo / v2 tiled 均可），
//     NavMesh 通过 opaque const void* 句柄桥接；.nnav 不升 v3、不落 agent 状态，
//     M-N0/M-N3 全部字节基线一字不动。Crowd 状态只在进程内 + 独立 .ntraj 产物。
//   - A 确定性口径：固定 dt（默认 1/60）+ 固定帧数 + 固定 agent 初始顺序 +
//     全显式参数；上游 DetourCrowd.cpp 无 RNG，轨迹为每帧每 agent 的 npos[3]
//     扁平 float 数组，同输入两次运行逐字节一致（fc /b 硬判据）。
//   - 零图形符号（NVRHI/ImGui 零 include）；A 类工具版不碰 viewer / 渲染基线。

#include <string>
#include <vector>

namespace nl::nav {

class NavMesh;

// 上游 UpdateFlags（DetourCrowd.h:185-192）在本层的位标志投影，
// Crowd.cpp 负责把位模式映射到 dtCrowdAgentParams::updateFlags。
namespace CrowdFlags {
inline constexpr unsigned kAnticipateTurns   = 1u << 0;
inline constexpr unsigned kObstacleAvoidance = 1u << 1;
inline constexpr unsigned kSeparation        = 1u << 2;
inline constexpr unsigned kOptimizeVis       = 1u << 3;
inline constexpr unsigned kOptimizeTopo      = 1u << 4;
inline constexpr unsigned kAll               = 0x1fu;
inline constexpr unsigned kDefault =
    kAnticipateTurns | kObstacleAvoidance | kSeparation |
    kOptimizeVis | kOptimizeTopo;
} // namespace CrowdFlags

// 官方样板 RecastDemo/Source/CrowdTool.cpp:644-670 的默认口径（本项目显式化，
// 不依赖任何隐式全局状态）。radius 必须 <= CrowdConfig::maxAgentRadius。
struct CrowdAgentParams {
    float radius                 = 0.30f;
    float height                 = 2.00f;
    float maxAcceleration        = 8.00f;
    float maxSpeed               = 3.50f;
    float collisionQueryRange    = 3.60f;  // radius * 12（上游样板）
    float pathOptimizationRange  = 9.00f;  // radius * 30（上游样板）
    float separationWeight       = 2.00f;
    unsigned updateFlags         = CrowdFlags::kDefault;
    unsigned char obstacleAvoidanceType = 3;   // [0, DT_CROWD_MAX_OBSTAVOIDANCE_PARAMS)
    unsigned char queryFilterType       = 0;   // [0, DT_CROWD_MAX_QUERY_FILTER_TYPE)
};

// 一个 agent 的起点 / 终点。顺序即 agent 注册顺序（确定性输入的一部分）。
struct CrowdSpawn {
    float pos[3]    = {0.f, 0.f, 0.f};
    float target[3] = {0.f, 0.f, 0.f};
};

struct CrowdConfig {
    int   maxAgents      = 16;          // [1, 256]
    float maxAgentRadius = 0.60f;       // > 0，且 >= agent.radius
    float dt             = 1.f / 60.f;  // 固定步长（Q3=A）；(0, 0.25]
    int   frames         = 600;         // 固定 update 次数 [1, 10000]；默认 10s@1/60，
                                        // 覆盖 20x20 arena 对角穿越（实测 ~600 帧抵达）
    // 终点吸附搜索盒：与 M-N1 QueryParams 同口径 {2,1,2}。y 半宽不能放大——
    // HizScene 箱顶 y=1.25 / 下层 poly y=-0.35 相距 1.6m，y=4 会把终点吸附到
    // 错误高度层，导致 agent 跨层不可达（M-N4 实测踩坑，故收紧到 1）。
    float targetHalfExtents[3] = {2.f, 1.f, 2.f};
    float arrivalRadius  = 0.50f;       // 抵达判定（XZ 距离，m）
    CrowdAgentParams agent;
};

struct CrowdTrace {
    int   agentCount      = 0;
    int   frames          = 0;   // 采样帧数 = cfg.frames + 1（含初始帧）
    float dt              = 0.f;
    std::vector<float> targets;   // agentCount * 3，原始请求终点
    std::vector<float> samples;   // frames * agentCount * 3，[frame][agent][xyz]
    std::vector<float> finalPos;  // agentCount * 3，末帧 npos
    int   arrivedCount    = 0;    // 末帧 XZ 距终点 <= arrivalRadius 的 agent 数
    float minPairDistance = 0.f;  // 全程任意两 agent 最小 XZ 距离（m）；0 个/1 个 agent 时为 0
};

class Crowd {
public:
    Crowd();
    ~Crowd();
    Crowd(const Crowd&) = delete;
    Crowd& operator=(const Crowd&) = delete;

    // 一站式固定步长模拟：init -> 按 spawns 顺序 addAgent -> requestMoveTarget ->
    // 固定帧 update -> 逐帧采样。任何上游状态失败都返回 false 且 outError 带原因
    //（不静默吞掉 findNearestPoly / addAgent / requestMoveTarget 的失败）。
    bool run(const NavMesh& nav, const CrowdConfig& cfg,
             const std::vector<CrowdSpawn>& spawns,
             CrowdTrace& out, std::string& outError);

    int agentCount() const;  // 上一次 run / begin 成功注册的 agent 数

    // ---- M-N6：增量驱动 API（Q1=B，E1 ECS NavAgentSystem 固定步长消费）----
    // 与 run() 共享同一上游 dtCrowd 语义，但由调用方逐帧驱动：
    //   begin(nav,cfg) -> addAgent(pos,target,params)* -> step()* -> agentPosition()
    // run() 本体一行未改（M-N4 的 .ntraj / PNG 字节基线锁定）；增量链路不写
    // 任何 .ntraj / .nnav 容器，轨迹由消费方（nav-tools / viewer）自行采样。
    // addAgent 返回本层顺序号 [0, agentCount)，与注册顺序一致（确定性输入）。
    bool begin(const NavMesh& nav, const CrowdConfig& cfg, std::string& outError);
    int  addAgent(const float pos[3], const float target[3],
                  const CrowdAgentParams& params, std::string& outError);
    bool step(std::string& outError);
    bool agentPosition(int localIndex, float outPos[3]) const;
    bool running() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace nl::nav
