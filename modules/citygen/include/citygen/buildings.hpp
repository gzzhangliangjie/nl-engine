// citygen 建筑提取(M-CG0.2):OSM building way → footprint 多边形 + 高度。
//
// 高度口径(CG0-PLAN §3,确定性三级):
//   ① height 标签(纯数字,可带 " m" 尾缀)优先 → 米;
//   ② building:levels × 3.0 m(中国住宅/商业层高折算缺省);
//   ③ 都没有 → 9.0 m 缺省(3 层)。
// footprint = way geometry(out geom);< 3 点退化丢弃计数,不静默。
// bbox 过滤复用 osm.hpp filterBBox 的 way 级语义(任一点在框内即保留)。
#pragma once

#include "citygen/osm.hpp"

#include <string>
#include <utility>
#include <vector>

namespace citygen {

struct Building {
    std::uint64_t osmId = 0;
    std::vector<std::pair<double, double>> poly;  // WGS-84 (lat,lon) footprint
    double heightM = 9.0;
    std::string name;                             // 可为空
    int heightSrc = 0;                            // 1=height 标签 2=levels 3=缺省
};

struct BuildingStats {
    std::size_t droppedNoPoly = 0;     // 无 geometry / 点数 < 3
    std::size_t heightFromTag = 0;
    std::size_t heightFromLevels = 0;
    std::size_t heightDefault = 0;
};

// building ways → Building 列表(输入顺序 = 输出顺序,确定性)。
std::vector<Building> extractBuildings(const OsmData& data,
                                       BuildingStats* stats = nullptr);

}  // namespace citygen
