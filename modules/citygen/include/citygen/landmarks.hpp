// citygen 地标白名单(M-CG1):OSM id → glb 单体,混合架构的程序化/真模型分界。
//
// 契约(CG0-PLAN §7,2026-11-16 用户拍板):
//   · bbox 内 building 命中 osmId → 从 builds 移除(不产 box),改产真模型实体;
//   · osmId=0 或无匹配 building 的条目(如广州塔,bbox 南界外) → 直接追加;
//   · mesh = 扁平 ASCII .glb 名(W1 资产契约,消费端白名单 validGlbAssetName);
//   · lat/lon = 模型原点锚定的真实经纬度(塔底中心);glb 自身零 transform、
//     Y-up 米制、原点在底座中心,故 Transform.scale 恒 1、rotation 恒单位。
// nlohmann 不进公开头(只 landmarks.cpp include)。
#pragma once

#include "citygen/buildings.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace citygen {

struct Landmark {
    std::uint64_t osmId = 0;   // 0 = 合成地标(无 bbox 内 OSM way)
    std::string name;          // 实体级 label(OSM 名或 whitelist 指定名)
    double lat = 0.0;          // 锚点 WGS-84(模型原点)
    double lon = 0.0;
    std::string mesh;          // 扁平 ASCII .glb 文件名
    double scale = 1.0;        // 模型米制基准=1(UE 艺术缩比版需显式填)
    double heightM = 0.0;      // 仅统计/俯视标记,0 = 从被抑制 building 继承
};

struct LandmarkStats {
    std::size_t suppressed = 0;    // bbox 内命中 → box 实体被真模型替换
    std::size_t addedOutside = 0;  // 无匹配 building → bbox 外直接加
};

// 解析 landmark_whitelist.json;非法 mesh 名/scale 显式抛异常(不静默)。
std::vector<Landmark> loadLandmarkWhitelist(const std::string& jsonText);

// 就地从 builds 删除被抑制的 building;返回按 whitelist 顺序排列的落地地标
// (锚点缺失时从被抑制 footprint 中心继承)。
std::vector<Landmark> applyLandmarks(std::vector<Building>& builds,
                                     const std::vector<Landmark>& whitelist,
                                     LandmarkStats* stats = nullptr);

}  // namespace citygen
