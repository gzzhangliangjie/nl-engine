// citygen OSM 输入源实现(Overpass [out:json] 解析)。
// nlohmann json 只在本 .cpp 内部 include,类型不进公开头(选型闸第 5 条隔离)。
#include "citygen/osm.hpp"

#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>

namespace citygen {

std::pair<double, double> toLocalMeters(const BBox& box, double lat, double lon) {
    // 等距圆柱投影(GZ 纬度带精度足够;BusViewer route-planner.js 同公式留档)。
    static constexpr double kAnchorLat = 23.1;  // 广州纬度带 cos 锚(与公式留档一致)
    const double cosLat = std::cos(kAnchorLat * 3.14159265358979323846 / 180.0);
    const double dx = (lon - box.centerLon()) * cosLat * 111320.0;
    const double dy = (lat - box.centerLat()) * 110540.0;
    return {dx, dy};
}

OsmData parseOverpassJson(const std::string& text) {
    nlohmann::json j = nlohmann::json::parse(text);  // 解析失败抛异常 → runtime_error
    if (!j.is_object() || !j.contains("elements") || !j["elements"].is_array()) {
        throw std::runtime_error("overpass json: missing elements array");
    }
    OsmData out;
    for (const auto& el : j["elements"]) {
        if (!el.is_object()) { ++out.droppedNonWay; continue; }
        const auto type = el.value("type", std::string{});
        if (type != "way") { ++out.droppedNonWay; continue; }  // node/relation:M-CG0 不消费
        OsmWay w;
        w.id = el.value("id", std::int64_t{0});
        if (el.contains("geometry") && el["geometry"].is_array()) {
            for (const auto& p : el["geometry"]) {
                if (p.is_object() && p.contains("lat") && p.contains("lon")) {
                    w.pts.emplace_back(p["lat"].get<double>(), p["lon"].get<double>());
                }
            }
        }
        if (w.pts.size() < 2) { ++out.droppedNoGeometry; continue; }
        if (el.contains("tags") && el["tags"].is_object()) {
            for (auto it = el["tags"].begin(); it != el["tags"].end(); ++it) {
                if (it.value().is_string()) {
                    w.tags.emplace(it.key(), it.value().get<std::string>());
                }
            }
        }
        out.ways.push_back(std::move(w));
    }
    return out;
}

OsmData filterBBox(const OsmData& in, const BBox& box) {
    OsmData out;
    out.droppedNoGeometry = in.droppedNoGeometry;
    out.droppedNonWay = in.droppedNonWay;
    for (const auto& w : in.ways) {
        bool hit = false;
        for (const auto& p : w.pts) {
            if (box.contains(p.first, p.second)) { hit = true; break; }
        }
        if (hit) out.ways.push_back(w);  // 整条保留,裁剪归一属 M-CG0.1
    }
    return out;
}

}  // namespace citygen
