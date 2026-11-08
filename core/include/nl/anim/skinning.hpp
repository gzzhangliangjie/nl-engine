#pragma once

// M-A5：CPU 蒙皮层（nl/anim，零图形 API）。
// 口径（2026-11-08 定）：
// - 调色板 palette[i] = model[i] * inverseBind[i]（glTF/UE/bevy 同式）。
//   model[i] 来自 Skeleton::localToModel 的 (T,R) FK；inverseBind 来自资产
//   （glTF skin.inverseBindMatrices）或程序化构造（rest 世界矩阵的逆）。
// - LBS：out = Σ_k w_k · palette[j_k] · v（j_k/ w_k 为每顶点 ≤4 关节绑定）。
//   平移用点变换（w=1），法线用方向变换（w=0，去平移）后归一化。
// - 本层只做数学，不碰 GPU；GPU 展示（viewer --mode skin）每帧 CPU 蒙皮后
//   走 writeBuffer 动态顶点缓冲（hud.cpp 同款 CopyDest+keepInitialState 模式）。
// - Mat4 列主序（glTF/OpenGL 约定），m[col*4+row]。

#include "anim.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace nl::anim {

// 4x4 列主序矩阵（够用面：mul / 点变换 / 方向变换 / identity）。
struct Mat4 {
    float m[16];   // m[col * 4 + row]
};

Mat4 mat4Identity();
Mat4 mat4Mul(const Mat4& a, const Mat4& b);                 // a * b
Vec3 mat4TransformPoint(const Mat4& m, const Vec3& p);      // w = 1
Vec3 mat4TransformDir(const Mat4& m, const Vec3& d);        // w = 0（不平移）
// 从 (T,R) 组装世界矩阵（localToModel 的 Mat4 版，供程序化构造 IBM/调色板用）。
Mat4 mat4FromTR(const Vec3& t, const Quat& r);

// 蒙皮绑定：每顶点 ≤4 关节 + 权重（和≈1），逆绑定矩阵按骨架下标存。
// joints 填 -1 的槽位权重必须为 0（skinPositions 跳过）。
struct SkinBinding {
    std::vector<std::array<int, 4>>     joints;
    std::vector<std::array<float, 4>>   weights;
    std::vector<Mat4>                   inverseBind;   // [boneIndex]
};

// 校验：顶点数一致、joints 在 [-1, boneCount)、权重非负且有效槽位和 ∈ [0.999, 1.001]、
// IBM 数量 = boneCount 且元素有限。空串 = 合法。
std::string validateSkinBinding(const SkinBinding& binding, std::size_t vertexCount,
                                std::size_t boneCount);

// palette[i] = model[i] * inverseBind[i]；outPalette 与 model 等长。
void computePalette(std::vector<Mat4>& outPalette,
                    const std::vector<BonePose>& modelPose,
                    const std::vector<Mat4>& inverseBind);

// LBS 位置：out[v] = Σ_k w_k · palette[j_k] · base[v]。out 允许与 base 同容器。
void skinPositions(std::vector<Vec3>& out, const std::vector<Vec3>& base,
                   const std::vector<Mat4>& palette, const SkinBinding& binding);

// LBS 法线：方向变换（去平移）后归一化；零向量输入输出零向量（不 NaN）。
void skinNormals(std::vector<Vec3>& out, const std::vector<Vec3>& baseN,
                 const std::vector<Mat4>& palette, const SkinBinding& binding);

} // namespace nl::anim
