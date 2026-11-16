// citygen 建筑提取实现(M-CG0.2)。口径见 buildings.hpp 头注。
#include "citygen/buildings.hpp"

#include <cmath>
#include <cstdlib>

namespace citygen {

namespace {

// height 标签解析:纯数字(可带 " m"/"m" 尾缀)→ 米;其余(如 "12 m; 3 floors")
// 不猜,返回 false 走 levels/缺省。
bool parseHeightMeters(const std::string& s, double& out) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || v <= 0.0 || v > 1000.0) return false;
    while (end && (*end == ' ')) ++end;
    if (end && *end != '\0' && *end != 'm' && *end != 'M') return false;
    if (end && (*end == 'm' || *end == 'M') && *(end + 1) != '\0' &&
        *(end + 1) != ' ' && *(end + 1) != '\0') {
        // "12 meters" 之类:容忍 m 后直接结尾或空格结尾,其它拒。
        const char* q = end + 1;
        while (*q == ' ') ++q;
        if (*q != '\0') return false;
    }
    out = v;
    return true;
}

}  // namespace

std::vector<Building> extractBuildings(const OsmData& data,
                                       BuildingStats* stats) {
    BuildingStats st{};
    std::vector<Building> out;
    for (const OsmWay& w : data.ways) {
        const auto bIt = w.tags.find("building");
        if (bIt == w.tags.end()) continue;                 // 非建筑 way 不计数
        if (w.pts.size() < 3) { ++st.droppedNoPoly; continue; }
        Building b;
        b.osmId = w.id;
        b.poly = w.pts;
        const auto hIt = w.tags.find("height");
        const auto lvIt = w.tags.find("building:levels");
        double h = 0.0;
        if (hIt != w.tags.end() && parseHeightMeters(hIt->second, h)) {
            b.heightM = h;
            b.heightSrc = 1;
            ++st.heightFromTag;
        } else if (lvIt != w.tags.end()) {
            char* end = nullptr;
            const double lv = std::strtod(lvIt->second.c_str(), &end);
            if (end != lvIt->second.c_str() && lv > 0.0 && lv <= 250.0) {
                b.heightM = lv * 3.0;
                b.heightSrc = 2;
                ++st.heightFromLevels;
            } else {
                b.heightM = 9.0;
                b.heightSrc = 3;
                ++st.heightDefault;
            }
        } else {
            b.heightM = 9.0;
            b.heightSrc = 3;
            ++st.heightDefault;
        }
        const auto nIt = w.tags.find("name");
        if (nIt != w.tags.end()) b.name = nIt->second;
        out.push_back(std::move(b));
    }
    if (stats) *stats = st;
    return out;
}

}  // namespace citygen
