#pragma once

// nl/math：通用最小数学（零第三方依赖）。
//
// 2026-11-09 ECS-lite 立项拍板（docs/p8/P8-ECS-PLAN.md「架构约束·数学」）：
//   原 nl/anim/anim_math.hpp 的通用数学（Vec3/Quat 及函数）上移至此，
//   anim_math.hpp 变薄转发头（using 引回 nl::anim），全部消费方零改动；
//   ECS 的 Transform 层级数学（非均匀 scale 复合 / mat4）M-E1 起在本模块新增，
//   与 nl/anim::Mat4（蒙皮专用，列主序）合并时机届时再定，本重构零行为变化。
//
// 选型记录（docs/action-system/ACTION-PLAN.md §4，2026-11-08 用户拍板）：
//   自写最小 vec3/quat 而非 vendored glm——纯逻辑层数学面极窄（lerp/slerp/
//   normalize），零新依赖，确定性单测可控。ozz/Lumix 只作参照不作依赖。
//
// 口径来源（三处对照，见 ACTION-PLAN §2）：
//   - 四元数 slerp 走最短路径并归一化：UE ExtractTransformFromTrack 两 key
//     Blend() 后 NormalizeRotation（AnimationUtils.cpp:1051）；bevy gltf
//     CubicRotationCurve 采样后归一化。注意：最短路径翻转（dot<0 取反）是
//     蒙皮混合的业界默认，关键帧插值同样适用（轨道内不会出现，混合会出现）。
//   - dot > 1-1e-4 退化 nlerp：ozz Lerp 阈值同款做法（防止 slerp 除以极小
//     sin 放大误差）。
// 纪律：全 inline、无静态可变状态、float 顺序固定 = 确定性可单测。

#include <cmath>

namespace nl::math {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

inline float dotVec3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float lengthVec3(const Vec3& a) { return std::sqrt(dotVec3(a, a)); }

// 线性插值：t=0 -> a，t=1 -> b（端点精确，gltf LINEAR / UE Blend 同口径）。
inline Vec3 lerpVec3(const Vec3& a, const Vec3& b, float t) {
    return a + (b - a) * t;
}

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;   // identity = 无旋转
};

inline Quat quatIdentity() { return {}; }

inline Quat normalizeQuat(const Quat& q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1e-12f) return quatIdentity();   // 退化保护：零长度按 identity 处理
    const float inv = 1.0f / len;
    return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
}

inline float dotQuat(const Quat& a, const Quat& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// 球面插值（最短路径）：蒙皮/关键帧旋转插值的标准口径（见文件头）。
// dot 接近 1 时退化 nlerp（数值稳定）；dot<0 先取反 b（同一旋转的另一半）。
inline Quat slerpQuat(const Quat& a, const Quat& b, float t) {
    Quat end = b;
    float d = dotQuat(a, b);
    if (d < 0.0f) {
        d = -d;
        end = { -b.x, -b.y, -b.z, -b.w };
    }
    if (d > 0.99995f) {
        // 近平行：nlerp + 归一化误差与 slerp 同量级，且避开除以极小 sin
        Quat r{ a.x + (end.x - a.x) * t,
                a.y + (end.y - a.y) * t,
                a.z + (end.z - a.z) * t,
                a.w + (end.w - a.w) * t };
        return normalizeQuat(r);
    }
    const float theta0 = std::acos(d);
    const float sin0 = std::sin(theta0);
    const float sa = std::sin((1.0f - t) * theta0) / sin0;
    const float sb = std::sin(t * theta0) / sin0;
    return { a.x * sa + end.x * sb,
             a.y * sa + end.y * sb,
             a.z * sa + end.z * sb,
             a.w * sa + end.w * sb };
}

// 加法混合的旋转语义（M-A2 用，先落数学）：相对基准旋转叠加。
// bevy blendable.rs 口径：slerp(IDENTITY, rot, weight) * base。
inline Quat scaleQuat(const Quat& q, float w) {
    return slerpQuat(quatIdentity(), q, w);
}

inline Quat mulQuat(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// 四元数旋转向量 v' = q v q^-1（优化式 t=2(qv×v); v'=v+w·t+qv×t）。
// M-A4b FK 用：父旋转作用于子骨骼局部平移偏移。
inline Vec3 rotVecByQuat(const Quat& q, const Vec3& v) {
    const Vec3 qv{ q.x, q.y, q.z };
    const Vec3 t{
        2.0f * (qv.y * v.z - qv.z * v.y),
        2.0f * (qv.z * v.x - qv.x * v.z),
        2.0f * (qv.x * v.y - qv.y * v.x),
    };
    return {
        v.x + q.w * t.x + (qv.y * t.z - qv.z * t.y),
        v.y + q.w * t.y + (qv.z * t.x - qv.x * t.z),
        v.z + q.w * t.z + (qv.x * t.y - qv.y * t.x),
    };
}

// 轴角 -> 四元数（资产构造 / 自测锚点用；axis 须为单位向量）。
inline Quat quatFromAxisAngle(const Vec3& axis, float radians) {
    const float s = std::sin(radians * 0.5f);
    return { axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f) };
}

// ---------------------------------------------------------------------------
// Mat4（M-E1 ECS 层级数学，2026-11-09 P8-ECS-PLAN「架构约束·数学」拍板：在本模块
// 新增；与 nl/anim 蒙皮矩阵合并时机届时再定）。列主序 m[col*4 + row]，列向量约定
// v' = M v，与 Carrot glm 一致；TRS 构造顺序 = translate * rotate * scale
// （Carrot Math/Transform.cpp:15-16 逐条对标）。
// 纪律不变：全 inline、float 顺序固定 = 确定性可单测。
// ---------------------------------------------------------------------------

struct Mat4 {
    // m[col][row] 语义的扁平存储：m[0..3]=第 0 列 (x 基向量 + 0)，m[12..14]=平移。
    float m[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
};

inline Mat4 mat4Identity() { return {}; }

// 列主序矩阵乘 result = a * b：result[c][r] = Σ_k a.m[k*4+r] * b.m[c*4+k]。
inline Mat4 mulMat4(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) {
                acc += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = acc;
        }
    }
    return r;
}

inline Mat4 mat4Translate(const Vec3& t) {
    Mat4 r;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

inline Mat4 mat4Scale(const Vec3& s) {
    Mat4 r;
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

// 四元数 -> 旋转矩阵（列主序；glm::toMat4 同款公式）。
inline Mat4 quatToMat4(const Quat& q) {
    Mat4 r;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.m[0]  = 1 - 2 * (yy + zz); r.m[4]  = 2 * (xy - wz);      r.m[8]  = 2 * (xz + wy);
    r.m[1]  = 2 * (xy + wz);     r.m[5]  = 1 - 2 * (xx + zz);  r.m[9]  = 2 * (yz - wx);
    r.m[2]  = 2 * (xz - wy);     r.m[6]  = 2 * (yz + wx);      r.m[10] = 1 - 2 * (xx + yy);
    return r;
}

// TRS 构造：T(pos) * R(rot) * S(scale)（Carrot Transform::toTransformMatrix 顺序）。
inline Mat4 mat4TRS(const Vec3& pos, const Quat& rot, const Vec3& scale) {
    Mat4 m = quatToMat4(rot);
    for (int c = 0; c < 3; ++c) {           // 旋转三列各乘 scale（列向量约定）
        m.m[c * 4 + 0] *= scale.x;
        m.m[c * 4 + 1] *= scale.y;
        m.m[c * 4 + 2] *= scale.z;
    }
    m.m[12] = pos.x; m.m[13] = pos.y; m.m[14] = pos.z;
    return m;
}

// 通用 4x4 求逆（Mesa gluInvertMatrix 经典展开，列主序逐项对应；reparent 的
// inverse(parent) 用）。可逆性由调用方保证（TRS 矩阵 scale 非 0）；奇异矩阵
// 结果无定义（不崩溃，不除零保护——引擎矩阵路径不允许奇异输入）。
inline Mat4 inverseMat4(const Mat4& a) {
    const float* m = a.m;
    Mat4 inv;
    float* o = inv.m;
    o[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    o[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    o[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    o[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    o[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    o[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    o[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    o[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    o[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    o[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    o[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    o[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    o[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    o[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    o[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    o[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    const float det = m[0]*o[0] + m[1]*o[4] + m[2]*o[8] + m[3]*o[12];
    const float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) o[i] *= invDet;
    return inv;
}

// 点变换 v' = M * vec4(v, 1)（层级世界位置提取用）。
inline Vec3 mat4TransformPoint(const Mat4& m, const Vec3& v) {
    return {
        m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z + m.m[12],
        m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z + m.m[13],
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14],
    };
}

} // namespace nl::math
