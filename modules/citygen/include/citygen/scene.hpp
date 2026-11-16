// citygen city.scene(M-CG0.2):建筑块 → E1 World JSON(city-tools serializeCityScene
// 同构形态:version/nextID/entities;实体 = Model{mesh,tint} + Transform{position,
// scale,rotation})。OSM 版实体 = 每建筑一个「块」:mesh 共享 unit box.obj
// (1×1×1,程序化落盘),Transform.position = footprint 外接盒中心(半高上移落地),
// scale = [外接盒宽, 高, 深]。roadnet 不进 scene(交通消费走 roadnet.json)。
#pragma once

#include "citygen/buildings.hpp"
#include "citygen/landmarks.hpp"
#include "citygen/osm.hpp"

#include <string>
#include <vector>

namespace citygen {

struct BlockBox {
    double cx = 0.0, cz = 0.0;   // 外接盒中心(局部米)
    double wx = 0.0, wz = 0.0;   // 外接盒宽/深(局部米)
};

// footprint → 外接盒(局部米;输入 WGS-84)。退化(零宽)返回 false。
bool buildingBlockBox(const Building& b, const BBox& box, BlockBox& out);

// city.scene 文本(手写固定序确定性;meshPath = "buildings/box.obj")。
// 三参版 = M-CG0 冻结形态(无地标),四参版 = M-CG1 追加真模型实体(只加不减)。
std::string serializeCityScene(const std::vector<Building>& builds,
                               const BBox& box, const std::string& meshPath);
std::string serializeCityScene(const std::vector<Building>& builds,
                               const BBox& box, const std::string& meshPath,
                               const std::vector<Landmark>& landmarks);

// unit box OBJ 中间件文本(1×1×1,8 顶点 12 三角,原点在盒心)。
std::string unitBoxObj();

}  // namespace citygen
