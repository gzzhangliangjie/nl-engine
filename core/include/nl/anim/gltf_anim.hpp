#pragma once

// M-A4b：rigged glTF/GLB -> Skeleton + AnimClip 资产入口（纯逻辑层的唯一
// 文件 IO 边界，与 nl::core::mesh_io 同款 tinygltf 接法）。
//
// 口径与留痕：
//   - 骨架 = glTF scene 全部节点按 DFS 拓扑序收表（名字缺省自动补 "bone_N"）；
//     skin.joints 过滤是蒙皮层（2.3）的事，本入口先整树。
//   - 静态 TRS 显式填成单关键帧轨道（times=[0]）：**空轨道 = 零/identity 常量**
//     是 M-A1 采样语义，而 glTF 无动画通道的节点应保持静态 TRS——两者不同，
//     必须在入口转换，不许靠空轨道兜底（踩坑留痕）。
//   - 动画通道：translation / rotation，LINEAR / STEP；**SCALE 通道与
//     CUBICSPLINE 采样器按错误拒绝**（FK 与 BoneTrack 均无 scale 口径，
//     静默忽略 = 假数据，宁可显式失败）。
//   - clip duration = 通道时间轴最大值；骨骼顺序按 Skeleton 下标输出。
//
// 返回 false 时 error 带英文描述（ASCII）；成功 error 保持空串。

#include "nl/anim/anim.hpp"
#include "nl/anim/skeleton.hpp"

#include <string>
#include <vector>

namespace nl::anim {

bool loadGltfAnim(const std::string& path, Skeleton& skeleton,
                  std::vector<AnimClip>& clips, std::string& error);

} // namespace nl::anim
