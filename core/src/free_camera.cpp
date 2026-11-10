// P8 M-CAM0：FreeCamera 实现（track camera）。
// 无阻尼直出：eye 与朝向由输入逐帧直接驱动，跟手优先（计划 §5 口径）。
// 全部状态演化只吃 dt 与输入——固定 dt 重放逐位一致（确定性契约）。

#include "nl/core/free_camera.hpp"

#include <cmath>

namespace nl::core {
namespace {

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

void FreeCamera::reset() {
    yaw_   = 0.0f;
    pitch_ = 0.0f;
    speed_ = 10.0f;
    rig_.projection  = CameraRig::Projection::Perspective;
    rig_.fovYDegrees = 45.0f;
    rig_.aspect      = 16.0f / 9.0f;
    rig_.nearZ       = 0.1f;
    rig_.farZ        = 100.0f;
    rig_.eye[0] = 0.0f; rig_.eye[1] = 0.0f; rig_.eye[2] = 0.0f;
    rig_.lookAt[0] = 0.0f; rig_.lookAt[1] = 0.0f; rig_.lookAt[2] = 1.0f;  // eye + forward
}

void FreeCamera::setAspect(float aspect) {
    if (aspect > 0.0f && std::isfinite(aspect)) rig_.aspect = aspect;
}

void FreeCamera::setFovYDegrees(float fovYDegrees) {
    rig_.fovYDegrees = clampf(fovYDegrees, 20.0f, 120.0f);
}

void FreeCamera::setFarZ(float farZ) {
    if (farZ > rig_.nearZ && std::isfinite(farZ)) rig_.farZ = farZ;
}

void FreeCamera::setSpeed(float unitsPerSecond) {
    speed_ = clampf(unitsPerSecond, 0.1f, 500.0f);
}

float FreeCamera::yawDegrees() const   { return yaw_ / kDeg2Rad; }
float FreeCamera::pitchDegrees() const { return pitch_ / kDeg2Rad; }

void FreeCamera::setPose(const float eye[3], float yawDegrees, float pitchDegrees) {
    yaw_   = yawDegrees * kDeg2Rad;
    pitch_ = clampf(pitchDegrees * kDeg2Rad, -kMaxPitch, kMaxPitch);
    rig_.eye[0] = eye[0]; rig_.eye[1] = eye[1]; rig_.eye[2] = eye[2];
    // lookAt 同 update 前向公式（同源口径，下一帧 update 会重算一致）。
    const float cp = std::cos(pitch_);
    rig_.lookAt[0] = rig_.eye[0] + std::sin(yaw_) * cp;
    rig_.lookAt[1] = rig_.eye[1] + std::sin(pitch_);
    rig_.lookAt[2] = rig_.eye[2] + std::cos(yaw_) * cp;
}

void FreeCamera::update(float dt, const float moveInput[3], const float lookDelta[2],
                        bool boost) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;   // 非法 dt：本帧冻结（确定性）

    // ---- 朝向：look 增量直加（弧度由映射层给增益），俯仰 clamp ----
    yaw_ += lookDelta[0];
    pitch_ = clampf(pitch_ + lookDelta[1], -kMaxPitch, kMaxPitch);

    // ---- 基底：forward 含俯仰（W/S 沿视线飞），right 水平，升降走世界 Y ----
    const float cp = std::cos(pitch_);
    const float forward[3] = {
        std::sin(yaw_) * cp,
        std::sin(pitch_),
        std::cos(yaw_) * cp
    };
    const float right[3] = { std::cos(yaw_), 0.0f, -std::sin(yaw_) };

    // ---- 平移：基底 × 轴 × speed ×(boost?kBoost:1) × dt ----
    const float scale = speed_ * (boost ? kBoost : 1.0f) * dt;
    rig_.eye[0] += (right[0] * moveInput[0] + forward[0] * moveInput[2]) * scale;
    rig_.eye[1] += (moveInput[1] + forward[1] * moveInput[2]) * scale;
    rig_.eye[2] += (right[2] * moveInput[0] + forward[2] * moveInput[2]) * scale;

    rig_.lookAt[0] = rig_.eye[0] + forward[0];
    rig_.lookAt[1] = rig_.eye[1] + forward[1];
    rig_.lookAt[2] = rig_.eye[2] + forward[2];
}

} // namespace nl::core
