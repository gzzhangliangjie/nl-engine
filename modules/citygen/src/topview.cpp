// citygen 俯视 PNG 实现(M-CG0.2)。口径见 topview.hpp 头注。
#include "citygen/topview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace citygen {

namespace {

void putPixel(std::vector<std::uint8_t>& img, int w, int h,
              int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    std::uint8_t* p = img.data() + (static_cast<std::size_t>(y) * w + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}

// 方形笔刷粗点(边界裁剪);线由沿线步进的点列组成,确定性。
void brushRect(std::vector<std::uint8_t>& img, int w, int h,
               float cx, float cy, float hw,
               std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int x0 = static_cast<int>(std::floor(cx - hw));
    const int x1 = static_cast<int>(std::ceil(cx + hw));
    const int y0 = static_cast<int>(std::floor(cy - hw));
    const int y1 = static_cast<int>(std::ceil(cy + hw));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            putPixel(img, w, h, x, y, r, g, b);
}

// 厚折线:相邻 shape 点线性步进(步长 ≈ 1px),逐步方刷。
void drawPolyline(std::vector<std::uint8_t>& img, int w, int h,
                  const std::vector<std::pair<float, float>>& ptsPx,
                  float halfWpx, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (ptsPx.size() < 2 || halfWpx < 0.5f) return;
    if (halfWpx < 1.0f) halfWpx = 1.0f;
    for (std::size_t i = 0; i + 1 < ptsPx.size(); ++i) {
        const float ax = ptsPx[i].first, ay = ptsPx[i].second;
        const float bx = ptsPx[i + 1].first, by = ptsPx[i + 1].second;
        const float dx = bx - ax, dy = by - ay;
        const float len = std::sqrt(dx * dx + dy * dy);
        const int steps = std::max(1, static_cast<int>(len) + 1);
        for (int s = 0; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            brushRect(img, w, h, ax + dx * t, ay + dy * t, halfWpx, r, g, b);
        }
    }
}

// scanline 奇偶填充(逐行交点配对,确定性;浮点交点四舍五入到像素)。
void fillPolygon(std::vector<std::uint8_t>& img, int w, int h,
                 const std::vector<std::pair<float, float>>& polyPx,
                 std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (polyPx.size() < 3) return;
    float minY = 1e30f, maxY = -1e30f;
    for (const auto& p : polyPx) {
        minY = std::min(minY, p.second);
        maxY = std::max(maxY, p.second);
    }
    int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    int y1 = std::min(h - 1, static_cast<int>(std::ceil(maxY)));
    const std::size_t n = polyPx.size();
    for (int y = y0; y <= y1; ++y) {
        const float cy = static_cast<float>(y) + 0.5f;
        std::vector<float> xs;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& a = polyPx[i];
            const auto& b = polyPx[(i + 1) % n];
            if ((a.second <= cy && b.second > cy) || (b.second <= cy && a.second > cy)) {
                const float t = (cy - a.second) / (b.second - a.second);
                xs.push_back(a.first + t * (b.first - a.first));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            int x0 = std::max(0, static_cast<int>(std::ceil(xs[i] - 0.5f)));
            int x1 = std::min(w - 1, static_cast<int>(std::floor(xs[i + 1] - 0.5f)));
            for (int x = x0; x <= x1; ++x)
                putPixel(img, w, h, x, y, r, g, b);
        }
    }
}

void stbCallback(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(ctx);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// 路网类别 → 画线颜色(亮=主干,中=次干,暗=支路/服务)。
void roadColor(const std::string& cls, std::uint8_t& r, std::uint8_t& g,
               std::uint8_t& b) {
    static const char* kBright[] = {"motorway", "motorway_link", "trunk",
                                    "trunk_link", "primary", "primary_link"};
    static const char* kMid[] = {"secondary", "secondary_link", "tertiary",
                                 "tertiary_link"};
    for (const char* c : kBright)
        if (cls == c) { r = 0xd8; g = 0xdc; b = 0xe4; return; }
    for (const char* c : kMid)
        if (cls == c) { r = 0x9a; g = 0xa0; b = 0xac; return; }
    r = 0x70; g = 0x76; b = 0x82;
}

}  // namespace

bool renderTopView(const Roadnet& net, const std::vector<Building>& builds,
                   const BBox& box, const TopViewConfig& cfg,
                   std::vector<std::uint8_t>& outPng, int& outW, int& outH,
                   TopViewStats* stats, const std::vector<Landmark>* landmarks) {
    TopViewStats st{};
    // 世界范围(bbox 四角局部米)+ 留白 → 画布。
    double minX = 1e300, maxX = -1e300, minZ = 1e300, maxZ = -1e300;
    const double corners[4][2] = {{box.south, box.west}, {box.south, box.east},
                                  {box.north, box.west}, {box.north, box.east}};
    for (const auto& c : corners) {
        auto [x, z] = toLocalMeters(box, c[0], c[1]);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
    }
    // M-CG1:地标锚点可能在 bbox 外(广州塔在南界外约 155m),画布自动扩到全覆盖。
    if (landmarks) {
        for (const Landmark& lm : *landmarks) {
            auto [x, z] = toLocalMeters(box, lm.lat, lm.lon);
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
        }
    }
    const double worldW = (maxX - minX) + 2.0 * cfg.marginM;
    const double worldH = (maxZ - minZ) + 2.0 * cfg.marginM;
    if (worldW <= 0 || worldH <= 0 || cfg.targetWidth <= 0) return false;
    const double ppm = static_cast<double>(cfg.targetWidth) / worldW;
    const int w = cfg.targetWidth;
    const int h = static_cast<int>(worldH * ppm + 0.5);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;

    // 世界 → 像素(x 东向右,z 北向上 → 像素 y = 北在上)。
    auto toPx = [&](double xM, double zM) -> std::pair<float, float> {
        const float px = static_cast<float>((xM - minX + cfg.marginM) * ppm);
        const float py = static_cast<float>((maxZ + cfg.marginM - zM) * ppm);
        return {px, py};
    };

    std::vector<std::uint8_t> img(static_cast<std::size_t>(w) * h * 3, 0);
    // 底色 #20242c。
    for (std::size_t i = 0; i < img.size(); i += 3) {
        img[i] = 0x20; img[i + 1] = 0x24; img[i + 2] = 0x2c;
    }

    // ① 建筑 footprint(高度灰度 0x58→0xe8,9m 以下取 0x58)。
    //   输入 = WGS-84 (lat,lon) → 先转局部米再进像素坐标。
    for (const Building& b : builds) {
        std::vector<std::pair<float, float>> poly;
        poly.reserve(b.poly.size());
        for (const auto& p : b.poly) {
            auto [xM, zM] = toLocalMeters(box, p.first, p.second);
            poly.push_back(toPx(xM, zM));
        }
        double t = (b.heightM - 9.0) / 111.0;   // 9..120m → 0..1
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        const std::uint8_t g = static_cast<std::uint8_t>(0x58 + t * (0xe8 - 0x58));
        fillPolygon(img, w, h, poly, g, g, g);
    }

    // ② 路网(后画在上层;宽度 = edge.width 米 → px,半宽下限 1px)。
    for (const RoadnetEdge& e : net.edges) {
        if (e.shape.size() < 2) continue;
        std::vector<std::pair<float, float>> pts;
        pts.reserve(e.shape.size());
        for (const auto& p : e.shape) pts.push_back(toPx(p.first, p.second));
        float hw = static_cast<float>(e.width * 0.5 * ppm);
        if (hw < 1.0f) hw = 1.0f;
        std::uint8_t r, g2, b2;
        roadColor(e.cls, r, g2, b2);
        drawPolyline(img, w, h, pts, hw, r, g2, b2);
    }

    // ③ M-CG1 地标金色标记(顶层;固定 7px 半宽方块,与灰度明显区分)。
    if (landmarks) {
        for (const Landmark& lm : *landmarks) {
            auto [xM, zM] = toLocalMeters(box, lm.lat, lm.lon);
            auto [px, py] = toPx(xM, zM);
            brushRect(img, w, h, px, py, 7.0f, 0xff, 0xd7, 0x00);
        }
    }

    // ④ 内容计数(终扫一次;建筑灰度带 / 金色地标 / 其余非底色 = 路网)。
    for (std::size_t i = 0; i < img.size(); i += 3) {
        const std::uint8_t r = img[i], g = img[i + 1], b = img[i + 2];
        if (r == 0x20 && g == 0x24 && b == 0x2c) continue;   // 底色
        if (r == 0xff && g == 0xd7 && b == 0x00) { ++st.landmarkPixels; continue; }
        if (r == g && g == b && r >= 0x58 && r <= 0xe8) ++st.buildingPixels;
        else ++st.roadPixels;
    }

    // ⑤ PNG 编码(stb zlib 确定性)。
    outPng.clear();
    const int ok = stbi_write_png_to_func(stbCallback, &outPng, w, h, 3,
                                          img.data(), w * 3);
    if (ok == 0 || outPng.empty()) return false;
    outW = w;
    outH = h;
    if (stats) *stats = st;
    return true;
}

}  // namespace citygen
