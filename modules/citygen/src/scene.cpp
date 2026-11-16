// citygen city.scene 实现(M-CG0.2)。形态见 scene.hpp 头注。
#include "citygen/scene.hpp"

#include <algorithm>
#include <cstdio>

namespace citygen {

bool buildingBlockBox(const Building& b, const BBox& box, BlockBox& out) {
    if (b.poly.size() < 3) return false;
    double minLat = 1e300, maxLat = -1e300, minLon = 1e300, maxLon = -1e300;
    for (const auto& p : b.poly) {
        minLat = std::min(minLat, p.first);
        maxLat = std::max(maxLat, p.first);
        minLon = std::min(minLon, p.second);
        maxLon = std::max(maxLon, p.second);
    }
    auto [x0, z0] = toLocalMeters(box, minLat, minLon);
    auto [x1, z1] = toLocalMeters(box, maxLat, maxLon);
    out.cx = (x0 + x1) * 0.5;
    out.cz = (z0 + z1) * 0.5;
    out.wx = x1 - x0;
    out.wz = z1 - z0;
    return out.wx > 0.0 && out.wz > 0.0;
}

// JSON 字符串最小转义(UTF-8 原样直出,只转义控制字符与引号)。
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// M-CG0 冻结形态:无地标,逐字节保持 cg0-m0 tag 产物。
std::string serializeCityScene(const std::vector<Building>& builds,
                               const BBox& box, const std::string& meshPath) {
    static const std::vector<Landmark> kNone;
    return serializeCityScene(builds, box, meshPath, kNone);
}

// M-CG1:先 box 实体(顺序同 CG0),再 whitelist 顺序的地标真模型实体。
// name 用 ASCII 安全形态(osm id / lm_序号);真名走实体级 "label"(A 档拍板,
// 2026-11-16;只加不减,当前树内无引擎加载器,消费端接线时同契约实现)。
std::string serializeCityScene(const std::vector<Building>& builds,
                               const BBox& box, const std::string& meshPath,
                               const std::vector<Landmark>& landmarks) {
    std::string s;
    char buf[640];
    std::uint32_t emitted = 0;
    for (const Building& b : builds) {
        BlockBox bb;
        if (buildingBlockBox(b, box, bb)) ++emitted;
    }
    const std::uint32_t n = emitted + static_cast<std::uint32_t>(landmarks.size());
    s += "{\n";
    s += "  \"version\": 1,\n";
    std::snprintf(buf, sizeof buf, "  \"nextID\": %u,\n", n + 1u);
    s += buf;
    s += "  \"entities\": [\n";
    std::uint32_t id = 1;
    for (const Building& b : builds) {
        BlockBox bb;
        if (!buildingBlockBox(b, box, bb)) continue;
        const double cy = b.heightM * 0.5;
        char head[160];
        std::snprintf(head, sizeof head,
                      "    { \"id\": %u, \"name\": \"bld_%llu\", \"flags\": 0, "
                      "\"parent\": 0,\n",
                      id, static_cast<unsigned long long>(b.osmId));
        std::string ent = head;
        if (!b.name.empty())
            ent += "      \"label\": \"" + jsonEscape(b.name) + "\",\n";
        std::snprintf(buf, sizeof buf,
                      "      \"components\": {\n"
                      "      \"Model\": { \"mesh\": \"%s\", "
                      "\"tint\": [1.0, 1.0, 1.0] },\n"
                      "      \"Transform\": { \"position\": [%.2f, %.2f, %.2f], "
                      "\"scale\": [%.2f, %.2f, %.2f], "
                      "\"rotation\": [0.0, 0.0, 0.0, 1.0] }\n"
                      "    } }%s\n",
                      meshPath.c_str(), bb.cx, cy, bb.cz,
                      bb.wx, b.heightM, bb.wz,
                      id < n ? "," : "");
        s += ent;
        s += buf;
        ++id;
    }
    // 地标实体:glb 零 transform、Y-up 米制、原点在底座中心 → position.y=0、
    // scale=lm.scale、rotation 单位。lat/lon → citygen 局部米(同 box 锚)。
    for (std::size_t i = 0; i < landmarks.size(); ++i) {
        const Landmark& lm = landmarks[i];
        const auto [x, z] = toLocalMeters(box, lm.lat, lm.lon);
        char head[160];
        std::snprintf(head, sizeof head,
                      "    { \"id\": %u, \"name\": \"lm_%zu\", \"flags\": 0, "
                      "\"parent\": 0,\n",
                      id, i);
        std::string ent = head;
        if (!lm.name.empty())
            ent += "      \"label\": \"" + jsonEscape(lm.name) + "\",\n";
        std::snprintf(buf, sizeof buf,
                      "      \"components\": {\n"
                      "      \"Model\": { \"mesh\": \"%s\", "
                      "\"tint\": [1.0, 1.0, 1.0] },\n"
                      "      \"Transform\": { \"position\": [%.2f, 0.00, %.2f], "
                      "\"scale\": [%.2f, %.2f, %.2f], "
                      "\"rotation\": [0.0, 0.0, 0.0, 1.0] }\n"
                      "    } }%s\n",
                      lm.mesh.c_str(), x, z,
                      lm.scale, lm.scale, lm.scale,
                      id < n ? "," : "");
        s += ent;
        s += buf;
        ++id;
    }
    s += "  ]\n";
    s += "}\n";
    return s;
}

std::string unitBoxObj() {
    // 1×1×1 box,原点盒心;面序与 city-tools OBJ 中间件同口径(v/vn/f 三元组)。
    static const char* kObj =
        "# citygen unit box (M-CG0.2 buildings middleware)\n"
        "v -0.5 -0.5 -0.5\n"
        "v 0.5 -0.5 -0.5\n"
        "v 0.5 0.5 -0.5\n"
        "v -0.5 0.5 -0.5\n"
        "v -0.5 -0.5 0.5\n"
        "v 0.5 -0.5 0.5\n"
        "v 0.5 0.5 0.5\n"
        "v -0.5 0.5 0.5\n"
        "vn 0 0 -1\n"
        "vn 1 0 0\n"
        "vn 0 0 1\n"
        "vn -1 0 0\n"
        "vn 0 1 0\n"
        "vn 0 -1 0\n"
        "f 1//1 2//1 3//1\n"
        "f 1//1 3//1 4//1\n"
        "f 2//2 6//2 7//2\n"
        "f 2//2 7//2 3//2\n"
        "f 6//3 5//3 8//3\n"
        "f 6//3 8//3 7//3\n"
        "f 5//4 1//4 4//4\n"
        "f 5//4 4//4 8//4\n"
        "f 4//5 3//5 7//5\n"
        "f 4//5 7//5 8//5\n"
        "f 5//6 6//6 2//6\n"
        "f 5//6 2//6 1//6\n";
    return kObj;
}

}  // namespace citygen
