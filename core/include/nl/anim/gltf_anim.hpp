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
#include "nl/anim/skinning.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nl::anim {

bool loadGltfAnim(const std::string& path, Skeleton& skeleton,
                  std::vector<AnimClip>& clips, std::string& error);

// ---- M-A5：rigged 网格蒙皮数据入口（glTF skin + mesh primitives[0]）----
//
// 口径（2026-11-08 三处参考核实后定）：
//   - 调色板公式在 skinning.hpp（bevy skin.rs / UE RefToLocal 同式，本层只搬数）。
//   - 权重加载即归一化（bevy try_normalize_joint_weights 语义：负→0、零和→w0=1、
//     否则 ×1/sum）；WEIGHTS_0 非 float vec4 拒绝（glTF 罕见 normalized 整型，
//     不静默猜）。
//   - skin.inverseBindMatrices 缺失 → identity 兜底（bevy loader 同款回退）；
//     joints 数 > 256（bevy MAX_JOINTS）拒绝。
//   - glTF 规范：蒙皮网格节点的自身变换被忽略——顶点按 bind 空间原样使用，
//     不烘 mesh node TRS（与 loadGltfScene 的静态烘焙语义相反，注意区分）。
//   - 只取第一个含 mesh 的节点 + primitives[0]（TRIANGLES）；要求 POSITION +
//     NORMAL + JOINTS_0 + WEIGHTS_0 + 索引齐全，缺一显式报错。
struct SkinnedMeshData {
    std::vector<Vec3>          positions;   // bind 空间
    std::vector<Vec3>          normals;     // bind 空间
    std::vector<std::uint32_t> indices;
};

bool loadGltfSkinnedModel(const std::string& path, Skeleton& skeleton,
                          std::vector<AnimClip>& clips, SkinnedMeshData& mesh,
                          SkinBinding& skin, std::string& error);

} // namespace nl::anim
