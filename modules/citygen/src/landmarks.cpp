// citygen 地标白名单实现(M-CG1)。口径见 landmarks.hpp 头注。
#include "citygen/landmarks.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace citygen {

namespace {

// W1 资产契约:basename + .glb(消费端 nl::scene::validGlbAssetName 同口径子集)。
bool isFlatGlbName(const std::string& s) {
    if (s.size() < 6 || s.compare(s.size() - 4, 4, ".glb") != 0) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::vector<Landmark> loadLandmarkWhitelist(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text);
    std::vector<Landmark> out;
    if (!j.contains("landmarks")) return out;
    const auto& arr = j.at("landmarks");
    if (!arr.is_array()) throw std::runtime_error("landmarks must be an array");
    for (const auto& e : arr) {
        Landmark lm;
        lm.osmId = e.value("osmId", std::uint64_t{0});
        lm.name = e.value("name", std::string{});
        lm.lat = e.value("lat", 0.0);
        lm.lon = e.value("lon", 0.0);
        lm.mesh = e.value("mesh", std::string{});
        lm.scale = e.value("scale", 1.0);
        lm.heightM = e.value("heightM", 0.0);
        if (!isFlatGlbName(lm.mesh))
            throw std::runtime_error("landmark mesh must be flat ASCII .glb name: " + lm.mesh);
        if (!(lm.scale > 0.0))
            throw std::runtime_error("landmark scale must be positive: " + lm.mesh);
        out.push_back(std::move(lm));
    }
    return out;
}

std::vector<Landmark> applyLandmarks(std::vector<Building>& builds,
                                     const std::vector<Landmark>& whitelist,
                                     LandmarkStats* stats) {
    LandmarkStats st{};
    std::vector<Landmark> out;
    out.reserve(whitelist.size());
    for (const Landmark& src : whitelist) {
        Landmark lm = src;
        bool matched = false;
        if (lm.osmId != 0) {
            auto it = std::find_if(builds.begin(), builds.end(),
                                   [&](const Building& b) { return b.osmId == lm.osmId; });
            if (it != builds.end()) {
                // footprint 顶点均值 = 真中心(比外接盒中心更贴实体锚)。
                double lat = 0.0, lon = 0.0;
                if (!it->poly.empty()) {
                    for (const auto& p : it->poly) { lat += p.first; lon += p.second; }
                    lat /= static_cast<double>(it->poly.size());
                    lon /= static_cast<double>(it->poly.size());
                }
                if (lm.lat == 0.0 && lm.lon == 0.0) { lm.lat = lat; lm.lon = lon; }
                if (lm.name.empty()) lm.name = it->name;
                if (lm.heightM == 0.0) lm.heightM = it->heightM;
                builds.erase(it);
                ++st.suppressed;
                matched = true;
            }
        }
        if (!matched) ++st.addedOutside;
        out.push_back(std::move(lm));
    }
    if (stats) *stats = st;
    return out;
}

}  // namespace citygen
