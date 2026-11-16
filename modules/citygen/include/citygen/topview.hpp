// citygen 俯视 PNG(M-CG0.2):路网 shape 画线 + 建筑 footprint 高度灰度填充。
//
// 确定性:固定 targetWidth 自适应 pxPerMeter、scanline 奇偶填充、逐边固定序绘制;
// PNG 编码走 vendored stb_image_write.h(thirdparty/stb,city-tools 同源先例),
// stb zlib 同输入逐字节一致。
// 颜色(CG0-PLAN 验收判据 3 肉眼可辨):底 #20242c,建筑按高度 0x58→0xe8 灰度,
// 路网按类别分级亮灰(主干亮/支路暗)。
#pragma once

#include "citygen/buildings.hpp"
#include "citygen/roadnet.hpp"

#include <cstdint>
#include <vector>

namespace citygen {

struct TopViewConfig {
    int targetWidth = 2048;   // 画布宽(px);高按世界宽高比自适应,上限 16384
    double marginM = 30.0;    // 四边留白(m)
};

// 渲染统计(内容级 selftest 锚:建筑/路网像素必须 > 0,防「坐标系错位全裁出
// 画布仍绿」的空图假 PASS)。
struct TopViewStats {
    std::size_t buildingPixels = 0;
    std::size_t roadPixels = 0;
};

bool renderTopView(const Roadnet& net, const std::vector<Building>& builds,
                   const BBox& box, const TopViewConfig& cfg,
                   std::vector<std::uint8_t>& outPng, int& outW, int& outH,
                   TopViewStats* stats = nullptr);

}  // namespace citygen
