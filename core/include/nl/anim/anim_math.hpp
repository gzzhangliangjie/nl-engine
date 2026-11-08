#pragma once

// M-A1：动作系统最小数学（nl/anim 专用，零第三方依赖）。
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

namespace nl::anim {

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

} // namespace nl::anim
