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

std::string serializeCityScene(const std::vector<Building>& builds,
                               const BBox& box, const std::string& meshPath) {
    std::string s;
    char buf[640];
    const std::uint32_t n = static_cast<std::uint32_t>(builds.size());
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
        // name 用 ASCII 安全形态(osm id);真名走 label 字段(M-CG0.2 A 档拍板
        // 2026-11-16:OSM name 标签 → 实体级 "label",只加不减;当前树内无引擎
        // 加载器,消费端 M-CG1/W2 接线时同契约实现)。无名字建筑不产 label。
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
                      id == n ? "" : ",");
        ent += buf;
        s += ent;
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
