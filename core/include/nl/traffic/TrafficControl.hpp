// nl/traffic/TrafficControl.hpp — P8 M-T1.0：红绿灯相位 + 路口占用锁（traffic-m1，
// A 类轨道 feat/worldgen）
//
// 拍板依据 docs/worldgen/TRAFFIC-PLAN.md §6：
//   Q-T6=A2 固定周期两相含黄灯（绿→黄→全红清空→对向绿…，黄灯不放行新进入、
//   持锁在穿的继续穿完）；Q-T7=B1 路口占用锁（reservation，FIFO，全路口互斥
//   一车一锁——保守但确定性易锚，无信号路口同机制 lock-only）；
//   Q-T8=C1 验收走 sim 数值锚 + PNG（GameClock/ECS 承载是后置）。
//
// 契约扩展（W0-PLAN §1 只加不减）：nodes[].signal=true 消费顶层数组可选条目
//   {"node":<id>,"green_s":..,"yellow_s":..,"clear_s":..}；缺省自动两相（进入边按
//   行进主轴分组 x 轴向组0 / z 轴向组1，edge id 升序）+ 默认时序 8/2/1 s。
//
// 确定性口径：相位时钟 = 整数帧计数 × dt（无浮点累加漂移）；锁队列 FIFO 按申请序；
//   同帧多车申请按车更新序裁定（TrafficSim 固定车序先到先得）。

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
    float clearEff = 3.0f;           // 自适应释放半径 m = min(3, 0.3*最小邻接边长)
                                     //（M-T1 根修：W0 城 4m lane 上固定 3m 释放圈
                                     // 与 2.5m 停车线几何不相容 -> 排队车永远压在
                                     // 上游节点圈内持锁不放 = 系统性 gridlock）
    std::vector<int32_t> groups[2];  // 进入边 edgeId 集合（升序，确定性）
};

struct TrafficControl {
    const LaneGraph* graph = nullptr;
    std::vector<NodeControl> nodes;  // 受控节点 = 进入边 >= 2 的节点（节点 id 升序）

    // 运行参数（TRAFFIC-PLAN §7 固化口径）
    float dt = 1.0f / 60.0f;         // 相位时钟步长（与 TrafficSim 同源）
    float entryStop = 2.5f;          // 停车线距节点距离 m
    float clearRadius = 3.0f;        // 路口区半径 m（持锁车越过即释放）

    // 运行时状态（build 后 reset 清零）
    std::vector<int32_t> holder;     // 节点下标 -> 持锁车下标 / -1
    std::vector<std::vector<int32_t>> waitQueue;  // 节点下标 -> FIFO 车下标
    int64_t frame = 0;               // 相位时钟整数帧

    // roadnet.json + LaneGraph -> 控制面。失败返回 false 且 *err 显式
    //（坏 JSON / node id 重复 / phases 引用未知节点）；成功 *err 清空。
    bool buildFromRoadnetJson(const char* jsonBytes, size_t len,
                              const LaneGraph* g, std::string* err);

    // 节点 id -> nodes 下标（不存在 = -1）
    int32_t findNode(int32_t nodeId) const;

    // 某进入边当前信号态（纯函数 of frame；lock-only 节点恒 Green）
    // edgeId 不属于任何组 = Red（防御；分组覆盖全部进入边时不触发）
    SignalState stateForEdge(size_t nodeIdx, int32_t edgeId) const;

    // FIFO 申请路口锁：已持锁 = true；空闲且（队空或队首是自己）= 授予并出队；
    // 否则入队（重复申请不重复入队）返回 false
    bool acquire(size_t nodeIdx, int32_t carIdx);

    // 持锁车离开路口区（中心距 > clearRadius）则释放；每次车位置更新后调用
    void releaseCleared(int32_t carIdx, float carX, float carZ);

    bool isHolder(size_t nodeIdx, int32_t carIdx) const {
        return holder[nodeIdx] == carIdx;
    }

    // 推进相位时钟一帧（TrafficSim::step 开头调用）
    void advanceFrame() { ++frame; }
};

} // namespace nl::traffic
