// nl/traffic/LanePath.hpp — P8 M-T0.1：车道图 A* 寻路（traffic-m0，A 类轨道 feat/worldgen）
//
// 拍板依据 docs/worldgen/TRAFFIC-PLAN.md §3 M-T0.1（Q-T2=B1 车道级图上的寻路；
// 自研 = Q-A 已拍「无成熟方案自研例外」，CARLA TrafficManager 看思路不抄代码）。
//
// 口径：
//   - 代价 = 车道折线弧长累加（整车道语义：驶完当前 lane 再过连接）；
//   - 启发 = 当前 lane 末折线点到目标 lane 末折线点的欧氏距离（可采纳，不高估）；
//   - 确定性：open 表线性扫最小（禁 priority_queue 的堆序不确定），同 f 取先插入者
//     （= lane 下标序），同输入逐位同路径；
//   - 负向语义显式：Unreachable / BadLane（越界）各自可判，err 带人话原因；
//   - 掉头链路 M-T0.0 已结构性不建，寻路无需再查。

#pragma once

#include "nl/traffic/LaneGraph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nl::traffic {

enum class PathStatus {
    Ok = 0,          // *outLanePath 填 lane 序号序列（含起点/终点，至少 1 项）
    Unreachable = 1, // 起点可达集不含终点（图连通性，非参数错误）
    BadLane = 2,     // startLane/goalLane 越界
};

PathStatus findPathOnLanes(const LaneGraph& graph, int32_t startLane, int32_t goalLane,
                           std::vector<int32_t>* outLanePath, std::string* err);

// 路径弧长合计（整车道口径；M-T0.2 跟车做 lane 内 s 参数化时另算）
float pathLength(const LaneGraph& graph, const std::vector<int32_t>& lanePath);

} // namespace nl::traffic
