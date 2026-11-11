// P8 M-N6：合成 arena。逐顶点/逐三角形与 nav-tools/main.cpp 的
// syntheticScene() 相同（该函数保留在工具 TU 内不动，锁 M-N0~M-N5 基线）。
// 顶点顺序、法线朝向、障碍高度必须一致：Recast 按三角形法线坡角标记可走，
// 俯视 CCW（法线 +Y）才可走；竖直侧面不可走 = 障碍。

#include "nl/nav/Arena.hpp"

#include <cstdint>

namespace nl::nav {

Geometry makeArenaGeometry() {
    Geometry g;
    auto quad = [&](float x0, float z0, float x1, float z1, float y) {
        const std::uint32_t base = static_cast<std::uint32_t>(g.verts.size() / 3);
        const float v[4][3] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
        for (int i = 0; i < 4; ++i) g.verts.insert(g.verts.end(), v[i], v[i] + 3);
        g.tris.push_back(base + 0);
        g.tris.push_back(base + 2);
        g.tris.push_back(base + 1);
        g.tris.push_back(base + 0);
        g.tris.push_back(base + 3);
        g.tris.push_back(base + 2);
    };
    auto box = [&](float x0, float z0, float x1, float z1, float y0, float y1) {
        quad(x0, z0, x1, z1, y0);
        quad(x0, z0, x1, z1, y1);
        const float side[4][2][3] = {
            {{x0, y0, z0}, {x1, y0, z0}}, {{x0, y0, z1}, {x1, y0, z1}},
            {{x0, y0, z0}, {x0, y0, z1}}, {{x1, y0, z0}, {x1, y0, z1}},
        };
        for (auto& e : side) {
            const std::uint32_t base =
                static_cast<std::uint32_t>(g.verts.size() / 3);
            const float q[4][3] = {{e[0][0], y0, e[0][2]},
                                   {e[1][0], y0, e[1][2]},
                                   {e[1][0], y1, e[1][2]},
                                   {e[0][0], y1, e[0][2]}};
            for (int i = 0; i < 4; ++i)
                g.verts.insert(g.verts.end(), q[i], q[i] + 3);
            g.tris.push_back(base + 0);
            g.tris.push_back(base + 1);
            g.tris.push_back(base + 2);
            g.tris.push_back(base + 0);
            g.tris.push_back(base + 2);
            g.tris.push_back(base + 3);
        }
    };
    quad(-10, -10, 10, 10, 0.f);          // 地板
    box(-1, -1, 1, 1, 0.f, 2.f);          // 中央障碍
    quad(12, -2, 16, 2, 0.f);             // 孤立岛（与主地板隔 2m 沟）
    return g;
}

} // namespace nl::nav
