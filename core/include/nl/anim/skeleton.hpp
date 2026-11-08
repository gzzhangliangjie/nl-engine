#pragma once

// M-A4b：骨架层级 + 前向运动学（FK, local -> model）。
//
// 口径来源（三处对照，见 docs/action-system/ACTION-PLAN.md §2）：
//   - 骨架 = 扁平表（骨骼名 + 父索引），ozz Skeleton 同构（Range<parent>）；
//     bevy 把骨骼摊成 Entity 层级是 ECS 形态，纯逻辑层取扁平表最贴现有
//     per-bone pose 数组（下标即骨骼号，混合/采样全按序号走）。
//   - FK 只做平移 + 旋转（model[i] = model[parent] ⊗ local[i]），scale 后置
//     （与 M-A1 的 BoneTrack 省略 scale 同口径；glTF 节点常量 scale 在
//     资产入口按错误拒绝，留痕见 gltf_anim.hpp）。
//   - 全局 pose 复用 BonePose（TR 已够表达；蒙皮要 mat4/inverseBind 时在
//     2.3 蒙皮层再扩，本层不背包袱）。
// 纪律：纯函数 FK、父索引严格先于子（拓扑序）、无文件 IO = 确定性单测。

#include "nl/anim/anim.hpp"

#include <string>
#include <vector>

namespace nl::anim {

// 骨骼顺序 = 拓扑序：parentIndices[i] < i（root = -1）。采样/混合产出的
// BonePose 数组下标与本表一一对应。
struct Skeleton {
    std::vector<std::string> boneNames;      // 不重复、非空
    std::vector<int> parentIndices;          // 与 boneNames 等长
    std::vector<BonePose> restPose;          // 静态 TRS（glTF node 静态平移/旋转）

    std::size_t boneCount() const { return boneNames.size(); }
};

// 按骨骼名查下标；无则返回 SIZE_MAX。
std::size_t findBoneIndex(const Skeleton& skeleton, const std::string& bone);

// 校验：空串 = 合法；否则第一处错误的英文描述（ASCII）。
// 规则：三表等长且非空；名非空且不重复；parent == -1 或 [0, i)（拓扑序）。
std::string validateSkeleton(const Skeleton& skeleton);

// 前向运动学：局部 pose -> model（全局）pose。
// out[i].rotation = out[parent].rotation * local[i].rotation（root 直接抄）；
// out[i].translation = out[parent].translation + rotVec(out[parent].rotation,
//                                                      local[i].translation)。
// out 允许与 local 同容器（内部拷贝隔离，无混叠）；local 尺寸 != 骨骼数返回
// false（out 保持不变）。
bool localToModel(const std::vector<BonePose>& local, const Skeleton& skeleton,
                  std::vector<BonePose>& out);

} // namespace nl::anim
