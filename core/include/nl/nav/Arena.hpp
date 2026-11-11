#pragma once

// P8 M-N6：viewer --nav-crowd 的合成 arena 几何（与 nav-tools::syntheticScene
// 同源同参数：20x20 地板 + 中央 2x2x2 障碍 + 12..16 孤立岛，俯视 CCW）。
// 工具侧保留其 TU 内的原实现不改动，以锁 M-N0~M-N5 的全部字节基线；
// 本函数只服务 viewer 侧，产物几何逐顶点相同（selftest 对拍锚点）。
// 零第三方符号、零图形符号。

#include "nl/nav/Nav.hpp"

namespace nl::nav {
Geometry makeArenaGeometry();
} // namespace nl::nav
