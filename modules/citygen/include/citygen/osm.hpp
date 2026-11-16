// citygen —— nl-engine 自研城市生成库(游戏③广州地图,feat/citygen 轨道 M-CG0)
//
// 定位(2026-11-15 citygen 库化拍板):可插拔输入源(① OSM 导入【M-CG0,本文件】
//   ② 程序化规格网【city-tools 既有,M-CG1+ 迁入】)→ 共用内容生成 → 统一输出
//   city.scene.engine + roadnet.json + 俯视 PNG + CLI 壳。
// 纯 CPU 离线;运行时零联网;引擎侧零改动(不 bump kStage,A 类工具轨)。
//
// 依据 docs/citygen/CG0-PLAN.md(2026-11-16 用户三项拍板):
//   Q-GZ1 区域试水(珠江新城一带 2~3km) / Q-GZ2 Overpass+nlohmann / Q-GZ3 新轨 citygen。
// 输出契约对齐 docs/worldgen/W0-PLAN.md §1(Q-W2 接口铁律,只加字段不减字段)。
// 数据许可:OSM © OpenStreetMap contributors,ODbL——产物属衍生数据库,分发须署名;
//   引擎代码不受传染(见 S-PLAN §3)。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace citygen {

// WGS-84 经纬框(south/west/north/east,度)。
struct BBox {
    double south = 0.0, west = 0.0, north = 0.0, east = 0.0;

    bool contains(double lat, double lon) const {
        return lat >= south && lat <= north && lon >= west && lon <= east;
    }
    bool valid() const {
        return south < north && west < east && south >= -90.0 && north <= 90.0
            && west >= -180.0 && east <= 180.0;
    }
    // 中心锚(CLI 元信息用,保证局部坐标可复现)。
    double centerLat() const { return (south + north) * 0.5; }
    double centerLon() const { return (west + east) * 0.5; }
};

// WGS-84 → 局部米(BusViewer route-planner.js 同公式;等距圆柱,GZ 纬度带)。
// 返回 {dx(m, 东向), dy(m, 北向)},锚 = bbox 中心。
std::pair<double, double> toLocalMeters(const BBox& box, double lat, double lon);

// Overpass `[out:json]`(out geom 形态)的 way 元素:自带 geometry 顺序点 + tags。
struct OsmWay {
    std::int64_t id = 0;
    std::vector<std::pair<double, double>> pts;  // {lat, lon} 顺序形状点
    std::map<std::string, std::string> tags;
};

struct OsmData {
    std::vector<OsmWay> ways;
    // 不静默丢弃计数:无 geometry 的 way / 结构性非法元素(CG0-PLAN §6 风险条)。
    std::size_t droppedNoGeometry = 0;
    std::size_t droppedNonWay = 0;
};

// 解析 Overpass JSON 响应文本。结构性损坏(非 JSON / 无 elements)抛 std::runtime_error。
// node 元素计入 droppedNonWay(M-CG0 只消费 way;后续里程碑如需 node 再扩)。
OsmData parseOverpassJson(const std::string& text);

// bbox 过滤:先按任一点命中保留(way 可能跨 bbox 边界,整条保留,M-CG0.1 裁剪归一)。
OsmData filterBBox(const OsmData& in, const BBox& box);

}  // namespace citygen
