// nl/traffic/TrafficControl.hpp — P8 M-T1.0 红绿灯相位 + 路口占用锁；
// M-T2.0（traffic-m2）锁模型升级为「组共享 + 转弯独占」+ gap-out 感应切相。
//
// 拍板依据 docs/worldgen/TRAFFIC-PLAN.md §6（M-T1：A2 两相黄灯 / B1 FIFO 占用锁 /
// C1 数值锚）与 §10（M-T2：Q-T9 组共享并行 / Q-T10 gap-out / Q-T11 循环路线）。
//
// 契约扩展（W0-PLAN §1 只加不减）：nodes[].signal=true 消费顶层数组可选条目
//   {"node":<id>,"green_s":..,"yellow_s":..,"clear_s":..,"gap_out_s":..}；
//   缺省自动两相（进入边按行进主轴分组 x 轴向组0 / z 轴向组1，edge id 升序）
//   + 默认时序 8/2/1 s + gap_out 0（关 = M-T1 固定周期逐位行为）。
//
// 锁资源模型（M-T2.0 Q-T9）：
//   直行车  -> 申请所属组共享通行权（同组多车并行：对向直行右行车道分离不冲突）
//   转弯车  -> 申请节点独占（1 车道路网左转穿对向直行，必须全清）
//   FIFO    -> 队首裁决；队首 group 可授予时连续授予后续同/异组条目，
//              队列中存在 turn 等待者则组授予暂停（防转弯饿死）
//   确定性  -> 队列序 = 申请序；同帧多车按车更新序（TrafficSim 固定车序）
//
// gap-out（M-T2.0 Q-T10）：绿相活跃组本帧零等待车持续 gap_out_s -> 提前切黄
//   （frame 跳到黄灯起点，整数帧确定性）；最短绿时防抖 min(greenS, 2s)。
//
// 确定性口径：相位时钟 = 整数帧计数 × dt（无浮点累加漂移）。

#pragma once

#include "nl/traffic/LaneGraph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nl::traffic {

enum class SignalState : int32_t { Green = 0, Yellow = 1, Red = 2 };

// 单个受控节点的静态配置 + 进入边分组（组 0 = x 轴向，组 1 = z 轴向）
struct NodeControl {
    int32_t nodeId = -1;
    float x = 0.0f, z = 0.0f;        // 节点坐标（锁释放清空判定的圆心）
    bool signal = false;             // false = lock-only（无信号，仅占用锁）
    float greenS = 8.0f;             // 绿灯时长 s（信号节点）
    float yellowS = 2.0f;            // 黄灯时长 s
    float clearS = 1.0f;             // 全红清空间隔 s
    float gapOutS = 0.0f;            // M-T2.1 gap-out 阈值 s（0 = 固定周期）
    float clearEff = 3.0f;           // 自适应释放半径 m = min(3, 0.3*最小邻接边长)
                                     //（M-T1 根修：W0 城 4m lane 上固定 3m 释放圈
                                     // 与 2.5m 停车线几何不相容 -> 排队车永远压在
                                     // 上游节点圈内持锁不放 = 系统性 gridlock）
    bool axisPure = true;            // M-T2.0：进入边全部严格沿轴（dx/dz 有精确 0 分量）
                                     // 时才允许组内共享——斜边/T 型拓扑「同组直行」
                                     // 轨迹可能交叉（城市实测 min gap 0.557m），
                                     // 非纯轴节点组锁退化为单持有者（保守正确）
    std::vector<int32_t> groups[2];  // 进入边 edgeId 集合（升序，确定性）
};

// FIFO 等待条目：组共享申请（turn=false，group=0/1）或转弯独占申请（turn=true）
struct LockWaitEntry {
    int32_t car = -1;
    int8_t group = 0;
    bool turn = false;
};

struct TrafficControl {
    const LaneGraph* graph = nullptr;
    std::vector<NodeControl> nodes;  // 受控节点 = 进入边 >= 2 的节点（节点 id 升序）

    // 运行参数（TRAFFIC-PLAN §7 固化口径）
    float dt = 1.0f / 60.0f;         // 相位时钟步长（与 TrafficSim 同源）
    float entryStop = 2.5f;          // 停车线距节点距离 m
    float clearRadius = 3.0f;        // 路口区半径 m（持锁车越过即释放）

    // 运行时状态（build 后 reset 清零）
    std::vector<std::vector<int32_t>> groupHolders[2]; // [组][节点下标] -> 共享持有车集
    std::vector<int32_t> turnHolder;              // 节点下标 -> 转弯独占车下标 / -1
    std::vector<std::vector<LockWaitEntry>> waitQueue;  // 节点下标 -> FIFO 队列
    std::vector<int32_t> demandCount[2];          // 节点下标 -> 本帧各组在线等待车数
    std::vector<int32_t> greenIdle;               // 节点下标 -> 绿相空闲连续帧
    std::vector<int32_t> lastGreenSlot;           // 节点下标 -> 上帧绿相组（换相清零）
    int64_t frame = 0;               // 相位时钟整数帧

    // roadnet.json + LaneGraph -> 控制面。失败返回 false 且 *err 显式
    //（坏 JSON / node id 重复 / phases 引用未知节点 / 时长非法）；成功 *err 清空。
    bool buildFromRoadnetJson(const char* jsonBytes, size_t len,
                              const LaneGraph* g, std::string* err);

    // 节点 id -> nodes 下标（不存在 = -1）
    int32_t findNode(int32_t nodeId) const;

    // 某进入边所属组（不属于任何组 = -1）
    int groupOfEdge(size_t nodeIdx, int32_t edgeId) const;

    // 某进入边当前信号态（纯函数 of frame；lock-only 节点恒 Green）
    // edgeId 不属于任何组 = Red（防御；分组覆盖全部进入边时不触发）
    SignalState stateForEdge(size_t nodeIdx, int32_t edgeId) const;

    // 申请路口通行权（M-T2.0 统一入口）：turn=false 走 group 共享、true 走独占。
    // 已持有任一可通行该申请的资源 = true；授予规则见头注；否则入队返回 false。
    bool acquire(size_t nodeIdx, int32_t carIdx, int group, bool turn);

    // 兼容口径：任一资源（组共享集 / 转弯独占）含 carIdx 即 true
    bool hasAccess(size_t nodeIdx, int32_t carIdx) const;

    // 持有车离开路口区（中心距 > clearEff）则释放其全部资源；每帧车位置更新后调用
    void releaseCleared(int32_t carIdx, float carX, float carZ);

    // sim 每帧对在线等待（atLine 且未获准）的车上报需求（gap-out 数据源）
    void reportWaiting(size_t nodeIdx, int32_t edgeId);

    // 推进相位时钟一帧（TrafficSim::step 开头调用）：先按上帧 demand 做
    // gap-out 检查（可能跳帧进黄），再 ++frame、清 demand 计数
    void advanceFrame();

    // 诊断：终态持锁判定（两共享集 + turn 全空 = 无泄漏）
    int heldNodeCount() const;
};

} // namespace nl::traffic
