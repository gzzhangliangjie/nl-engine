// citygen roadnet 转换(M-CG0.1):OSM highway way → roadnet.json 契约结构。
//
// 契约源 = docs/worldgen/W0-PLAN.md §1(Q-W2 接口铁律)+ M-T4 signal 填充规则
// (进入边 >= 2 → signal=true)+ M-T2 消费端(TrafficControl/NodeControl 缺省
// 时序,phases 留空数组)。OSM 版只加字段:edges 加 "osm_name"/"cls",
// nodes/edges 版本仍 1(只加不减)。
//
// 拓扑口径(CG0-PLAN §3):
//   节点 = ① way 端点 ② 被 >=2 条不同 way 经过的共享点(Overpass out geom 下
//   同一 OSM node 的 lat/lon 在各 way 中逐位一致 → 精确坐标哈希聚类可靠)。
//   way 中间形状点不进节点表;way 按节点切成 edge 段。
//   oneway=yes 只产 from→to 单向;oneway=-1 产 to→from;其余产双向两条。
#pragma once

#include "citygen/osm.hpp"

#include <string>
#include <vector>

namespace citygen {

struct RoadnetNode {
    std::uint32_t id = 0;
    double lat = 0.0, lon = 0.0;   // WGS-84 原始(序列化前转局部米)
    double x = 0.0, z = 0.0;       // 局部米(bbox 锚定)
    bool isSignal = false;         // 进入边 >= 2(M-T4 口径)
};

struct RoadnetEdge {
    std::uint32_t id = 0;
    std::uint32_t from = 0, to = 0;
    std::uint32_t lanesPerDir = 1;
    double speedLimit = 8.3;       // m/s,highway 类别缺省表(maxspeed 标签优先)
    double width = 3.0;            // 车行道总宽 m(类别映射)
    std::string osmName;           // OSM 版新增字段(只加不减)
    std::string cls;               // highway 类别原文
    // 内存态形状点(局部米,含 from/to 端点,方向 = from→to);不进 roadnet.json
    // (契约 v1 只加不减,俯视 PNG 绘制消费)。相邻节点间的 way 中间形状点。
    std::vector<std::pair<double, double>> shape;
};

struct Roadnet {
    std::vector<RoadnetNode> nodes;
    std::vector<RoadnetEdge> edges;  // 有向;双向路两条(from→to + to→from)
    double anchorLat = 0.0, anchorLon = 0.0;  // bbox 中心(元信息)
};

// highway 类别 → {speed_limit m/s, width m} 缺省表(CG0-PLAN §3;
// maxspeed 标签 > 类别缺省)。footway/steps/path 等非车行道不进 roadnet。
bool highwayClassParams(const std::string& cls, double& speed, double& width);

// OSM ways → Roadnet(纯 CPU,确定性:节点按首次出现序编号,way 顺序 = 输入序)。
// 非车行道 way(footway/steps/path/…)+ 车行道但单点退化 way 丢弃,计数返回。
struct RoadnetBuildStats {
    std::size_t droppedNonVehicle = 0;
    std::size_t droppedDegenerate = 0;
    std::size_t junctionCandidates = 0;
};
Roadnet buildRoadnetFromOsm(const OsmData& data, const BBox& box,
                            RoadnetBuildStats* stats = nullptr);

// 确定性序列化(与 city-tools serializeRoadnet 同构:2 空格缩进固定字段序;
// OSM 版坐标 %.2f、新增 osm_name/cls 字段;JSON 字符串最小转义)。
std::string serializeRoadnet(const Roadnet& net);

}  // namespace citygen
