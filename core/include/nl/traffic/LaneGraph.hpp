// nl/traffic/LaneGraph.hpp — P8 M-T0.0：车道级展开图（traffic-m0，A 类轨道 feat/worldgen）
//
// 拍板依据 docs/worldgen/TRAFFIC-PLAN.md §0/§2：
//   Q-T2=B1 车道级展开图（UE ZoneGraph 口径，只读对照 Engine/Plugins/Runtime/ZoneGraph
//   ZoneGraphTypes.h FZoneLaneData/FZoneLaneLinkData，抄思路不抄代码）；
//   Q-T1=A1 逻辑落 core、零图形依赖（nl/nav PIMPL 先例）。
//
// 输入 = roadnet.json（W0-PLAN §1 Q-W2 接口铁律，city-tools 产物）：
//   有向 edges 双向成对、lanes_per_dir、speed_limit(m/s)、width(车行道总宽)、
//   phases 预留空数组（M-T0.2 起消费）、nodes.signal 布尔位。
//
// 几何口径（TRAFFIC-PLAN §2 固化）：
//   Y-up 右手系、右行交通：行进方向 f=(fx,fz)（XZ 面），右侧 right=(fz,-fx)（up×f，
//   f=+Z 时 right=+X 实证）。车道 i（0=最内贴中线）横向偏移
//   off_i = halfW - (i+0.5)*laneW，halfW=width/2，laneW=halfW/lanes_per_dir，
//   沿 right 施加；折线点序 = 行进方向（起点->终点）。
// 连接口径：lane(edge e: u->v) 的 outgoing = 所有 e'.from==v && e'.to!=u 的边展开
//   车道（edges 文件序 x laneIndex 升序 = 确定性遍历序），incoming 对称；禁掉头。
// 确定性：构建零 RNG，同输入逐字节同输出；dumpLaneGraphJson 作字节级对拍锚。

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace nl::traffic {

// 车道折线点（世界坐标；y 由 roadnet 节点 elevation 插值，M-RD-LAYER 前恒 0）
struct LanePoint {
    float x = 0.0f;
    float y = 0.0f;   // 高程：node.y（= layer * 7.0m）沿弧长线性插值
    float z = 0.0f;
};

// lane->lane 显式连接（UE FZoneLaneLinkData.DestLaneIndex 口径；M-T0.0 只记
// destLane，Incoming/Outgoing 分类与转向 Flags 留 M-T0.1 寻路时按需扩展）
struct LaneLink {
    int32_t destLane = -1;
};

// 展开后的单条车道 = 图上的节点（UE FZoneLaneData 口径：折线预计算 + 连接表）
struct Lane {
    int32_t id = -1;          // lane 序号 = 在 LaneGraph::lanes 里的下标
    int32_t edgeId = -1;      // 溯源 roadnet.json edge.id（CityCar.edgeId 预埋通道）
    int32_t fromNode = -1;    // roadnet node id（edge.from）
    int32_t toNode = -1;      // roadnet node id（edge.to）
    int32_t laneIndex = -1;   // 0 = 最内（贴中线）
    float speedLimit = 0.0f;  // m/s（继承 edge）
    float offset = 0.0f;      // 带符号横向偏移（m，right 方向为正）
    float length = 0.0f;      // 折线弧长
    std::vector<LanePoint> points;   // 起点->终点（行进方向）
    std::vector<LaneLink> outgoing;  // 连接闭合（M-T0.1 寻路消费）
    std::vector<LaneLink> incoming;
};

struct LaneGraph {
    std::vector<Lane> lanes;
    int32_t laneCount() const { return static_cast<int32_t>(lanes.size()); }
};

// roadnet.json -> LaneGraph。确定性：nodes/edges 文件序，零 RNG。
// 失败返回 false 且 *err 填显式原因（坏 JSON / version != 1 / from-to 引用不闭合 /
// lanes_per_dir<1 / width<=0 / speed_limit<=0 / 节点 id 重复）；成功 *err 清空。
bool buildLaneGraphFromRoadnetJson(const char* jsonBytes, size_t len,
                                   LaneGraph* out, std::string* err);

// 确定性 dump（对拍锚）：字段序 / 遍历序固定，同图逐字节一致。
// 非诊断用途，不做 hand-edited 友好化（键序即契约）。
std::string dumpLaneGraphJson(const LaneGraph& graph);

// 最近车道吸附（M-CITY-ROAD，寻路起终点用）：返回离 (x,y,z) 最近的车道下标，
// -1 = maxDist 内没有车道。距离按车道折线最近点算（不是端点），三维距离——
// M-RD-LAYER 起同 x/z 的上下层车道靠 y 区分（缺省 y=0 即贴地面层，车寻路默认）。
// 同距离取 lane 下标最小者（确定性）。复杂度 O(lanes * 点数)，一次查询毫秒级。
int32_t findNearestLane(const LaneGraph& graph, float x, float z, float maxDist,
                        float y = 0.0f);

// 车道折线最近点到 (x,y,z) 的距离（诊断/吸附残差用）；lane 越界返回 -1。
float laneDistanceTo(const Lane& lane, float x, float z, float y = 0.0f);

} // namespace nl::traffic
