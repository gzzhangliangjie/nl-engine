#include "nl/core/camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nl::core {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 out{};   // out = a * b，列主序
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            out.m[col * 4 + row] = sum;
        }
    }
    return out;
}

// 右手系、look down -Z、深度范围 [0,1]（D3D12 与 Vulkan 都是 0..1，不用两套矩阵）。
Matrix4 perspectiveZO(float fovY, float aspect, float nearZ, float farZ) {
    Matrix4 p{};
    const float f = 1.0f / std::tan(fovY * 0.5f);
    p.m[0]  = f / aspect;
    p.m[5]  = f;
    p.m[10] = farZ / (nearZ - farZ);
    p.m[11] = -1.0f;
    p.m[14] = (nearZ * farZ) / (nearZ - farZ);
    return p;
}

Matrix4 lookAt(const float eye[3], const float center[3], const float up[3]) {
    float zx = eye[0] - center[0], zy = eye[1] - center[1], zz = eye[2] - center[2];
    float len = std::sqrt(zx * zx + zy * zy + zz * zz);
    zx /= len; zy /= len; zz /= len;                 // camera backward

    float xx = up[1] * zz - up[2] * zy;
    float xy = up[2] * zx - up[0] * zz;
    float xz = up[0] * zy - up[1] * zx;
    len = std::sqrt(xx * xx + xy * xy + xz * xz);
    xx /= len; xy /= len; xz /= len;                 // camera right

    const float yx = zy * xz - zz * xy;
    const float yy = zz * xx - zx * xz;
    const float yz = zx * xy - zy * xx;             // camera up

    Matrix4 v{};
    v.m[0] = xx; v.m[1] = yx; v.m[2] = zx; v.m[3] = 0.0f;
    v.m[4] = xy; v.m[5] = yy; v.m[6] = zy; v.m[7] = 0.0f;
    v.m[8] = xz; v.m[9] = yz; v.m[10] = zz; v.m[11] = 0.0f;
    v.m[12] = -(xx * eye[0] + xy * eye[1] + xz * eye[2]);
    v.m[13] = -(yx * eye[0] + yy * eye[1] + yz * eye[2]);
    v.m[14] = -(zx * eye[0] + zy * eye[1] + zz * eye[2]);
    v.m[15] = 1.0f;
    return v;
}

bool finite(const Matrix4& m) {
    for (float v : m.m) if (!std::isfinite(v)) return false;
    return true;
}

bool differs(const Matrix4& a, const Matrix4& b) {
    for (int i = 0; i < 16; ++i) if (std::fabs(a.m[i] - b.m[i]) > 1e-5f) return true;
    return false;
}

} // namespace

void OrbitCamera::reset() {
    yaw_ = 0.21f;
    pitch_ = 0.16f;
    distance_ = 4.2f;
}

void OrbitCamera::setAspect(float aspect) {
    if (aspect > 0.0f && std::isfinite(aspect)) aspect_ = aspect;
}

void OrbitCamera::setOrbit(float yawDegrees, float pitchDegrees, float distance) {
    yaw_ = yawDegrees * kPi / 180.0f;
    pitch_ = clamp(pitchDegrees * kPi / 180.0f, kMinPitch, kMaxPitch);
    distance_ = clamp(distance, kMinDist, kMaxDist);
}

void OrbitCamera::orbit(float deltaX, float deltaY) {
    yaw_ += deltaX * kOrbitGain;
    pitch_ = clamp(pitch_ + deltaY * kOrbitGain, kMinPitch, kMaxPitch);
}

void OrbitCamera::zoom(float scrollDelta) {
    distance_ = clamp(distance_ * std::exp(-scrollDelta * kZoomGain), kMinDist, kMaxDist);
}

void OrbitCamera::setDistance(float distance) {
    distance_ = clamp(distance, kMinDist, kMaxDist);
}

void OrbitCamera::setFovYDegrees(float fovYDegrees) {
    constexpr float kMinFovDeg = 20.0f, kMaxFovDeg = 120.0f;
    fovY_ = clamp(fovYDegrees * kPi / 180.0f, kMinFovDeg * kPi / 180.0f, kMaxFovDeg * kPi / 180.0f);
}

float OrbitCamera::fovYDegrees() const { return fovY_ * 180.0f / kPi; }

Matrix4 OrbitCamera::viewProjection() const {
    const float cp = std::cos(pitch_);
    const float eye[3] = {
        distance_ * cp * std::sin(yaw_),
        distance_ * std::sin(pitch_),
        distance_ * cp * std::cos(yaw_)
    };
    const float center[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    // M-A5.4：远平面随距离自适应（kFar=100 只够 dist≤33 的场景）。dist>33 时
    // 模型会被远平面整体裁掉；dist ≤ 33 时取值与旧 kFar 完全一致，历史基线不破。
    // M-C2 merge：fov 用 fovY_ 实例字段（console cam.fov 可写，缺省 45° 不变）。
    const float farZ = std::max(kFar, distance_ * 3.0f);
    return multiply(perspectiveZO(fovY_, aspect_, kNear, farZ), lookAt(eye, center, up));
}

Matrix4 OrbitCamera::view() const {
    // M6.1：与 viewProjection() 的 lookAt 完全同源（同一组 eye/center/up），
    // hiz 遍历的 Mara&Morgan 球投影在 view space 进行（dag.slang g_dagView）。
    const float cp = std::cos(pitch_);
    const float eye[3] = {
        distance_ * cp * std::sin(yaw_),
        distance_ * std::sin(pitch_),
        distance_ * cp * std::cos(yaw_)
    };
    const float center[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    return lookAt(eye, center, up);
}

Matrix4 OrbitCamera::model() const {
    Matrix4 m{};
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

float OrbitCamera::yawDegrees() const   { return yaw_ * 180.0f / kPi; }
float OrbitCamera::pitchDegrees() const { return pitch_ * 180.0f / kPi; }
float OrbitCamera::distance() const     { return distance_; }

// M5.0：与 viewProjection() 的 lookAt 完全同一组 eye 数值（公式同源，不另造一套）。
void OrbitCamera::eyePosition(float out[3]) const {
    const float cp = std::cos(pitch_);
    out[0] = distance_ * cp * std::sin(yaw_);
    out[1] = distance_ * std::sin(pitch_);
    out[2] = distance_ * cp * std::cos(yaw_);
}

float OrbitCamera::projectionYScale() const {
    return 1.0f / std::tan(fovY_ * 0.5f);
}

bool cameraSelfTest() {
    OrbitCamera cam;
    cam.setAspect(640.0f / 360.0f);
    const Matrix4 base = cam.viewProjection();
    if (!finite(base) || !finite(cam.model())) return false;
    if (!std::isfinite(cam.distance()) || cam.distance() <= 0.0f) return false;

    cam.orbit(40.0f, 10.0f);
    if (!finite(cam.viewProjection()) || !differs(base, cam.viewProjection())) return false;
    if (cam.pitchDegrees() > 90.0f || cam.pitchDegrees() < -90.0f) return false;

    const float before = cam.distance();
    cam.zoom(2.0f);
    if (!(cam.distance() < before)) return false;
    for (int i = 0; i < 200; ++i) cam.zoom(10.0f);
    if (!(cam.distance() >= 0.44f)) return false;      // 下限钳制，可推进模型内部看内壁
    for (int i = 0; i < 400; ++i) cam.zoom(-10.0f);
    if (!(cam.distance() <= 500.01f)) return false;    // 上限钳制（M-A5.4：20→500，Fox 级大资产取景）
    if (!finite(cam.viewProjection())) return false;   // 大距离远平面自适应下投影仍有限

    cam.reset();
    if (differs(base, cam.viewProjection())) return false;   // reset 恢复默认（aspect 保持 640/360）
    cam.setAspect(1.0f);
    if (!differs(base, cam.viewProjection())) return false;  // aspect 真的进了投影矩阵
    cam.setAspect(-1.0f);
    if (!(cam.aspect() == 1.0f)) return false;               // 非法 aspect 被忽略
    return true;
}

} // namespace nl::core
