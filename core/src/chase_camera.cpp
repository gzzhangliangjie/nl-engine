// P8 M-CAM0：ChaseCamera 实现（track camera）。
// 阻尼口径：帧率无关指数阻尼 eye += (desired - eye) * (1 - exp(-k*dt))；
// boomYawOffset 自动回正 boomYawOffset *= exp(-rate*dt)（Chase 模式且本帧无 look 输入）。
// 全部状态演化只吃 dt 与输入位姿——固定 dt 重放逐位一致（确定性契约）。

#include "nl/core/chase_camera.hpp"

#include <cmath>

namespace nl::core {
namespace {

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kLookGain = 0.007f;   // 像素 -> rad（OrbitCamera kOrbitGain 同款口径）

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

void ChaseCamera::reset() {
    mode_          = Mode::Chase;
    distance_      = 6.0f;
    height_        = 2.0f;
    posK_          = 5.0f;
    returnK_       = 3.0f;
    leadTime_      = 0.3f;
    pitch_         = 0.35f;
    boomYawOffset_ = 0.0f;
    lookActive_    = false;
    reverse_       = false;
    peekBack_      = false;
    rig_.projection = CameraRig::Projection::Perspective;
    rig_.fovYDegrees = 45.0f;
    rig_.aspect      = 16.0f / 9.0f;
    rig_.nearZ       = 0.1f;
    rig_.farZ        = 100.0f;
    rig_.eye[0] = 0.0f; rig_.eye[1] = height_; rig_.eye[2] = distance_;
    rig_.lookAt[0] = 0.0f; rig_.lookAt[1] = 0.0f; rig_.lookAt[2] = 0.0f;
}

void ChaseCamera::setAspect(float aspect) {
    if (aspect > 0.0f && std::isfinite(aspect)) rig_.aspect = aspect;
}

void ChaseCamera::setFovYDegrees(float fovYDegrees) {
    rig_.fovYDegrees = clampf(fovYDegrees, 20.0f, 120.0f);
}

void ChaseCamera::setFarZ(float farZ) {
    if (farZ > rig_.nearZ && std::isfinite(farZ)) rig_.farZ = farZ;
}

void ChaseCamera::setDistance(float distance) {
    distance_ = clampf(distance, 1.0f, 50.0f);
}

void ChaseCamera::setHeightOffset(float height) {
    height_ = clampf(height, 0.0f, 10.0f);
}

void ChaseCamera::setStiffness(float k) {
    posK_ = clampf(k, 0.1f, 50.0f);
}

void ChaseCamera::setBoomReturnRate(float rate) {
    returnK_ = clampf(rate, 0.1f, 20.0f);
}

void ChaseCamera::setLeadTime(float seconds) {
    leadTime_ = clampf(seconds, 0.0f, 1.0f);
}

void ChaseCamera::setPitchDegrees(float pitchDegrees) {
    pitch_ = clampf(pitchDegrees * kDeg2Rad, -kMaxPitch, kMaxPitch);
}

float ChaseCamera::pitchDegrees() const         { return pitch_ / kDeg2Rad; }
float ChaseCamera::boomYawOffsetDegrees() const { return boomYawOffset_ / kDeg2Rad; }

void ChaseCamera::look(float deltaX, float deltaY) {
    boomYawOffset_ = clampf(boomYawOffset_ + deltaX * kLookGain, -kMaxBoomYaw, kMaxBoomYaw);
    pitch_         = clampf(pitch_ + deltaY * kLookGain, -kMaxPitch, kMaxPitch);
    lookActive_    = true;
}

void ChaseCamera::setMode(Mode mode) {
    mode_ = mode;   // 只改语义：位姿状态一个字节不动 → 切换帧位姿逐位连续（锚 C）
}

void ChaseCamera::setReverse(bool on)  { reverse_ = on; }
void ChaseCamera::setPeekBack(bool on) { peekBack_ = on; }

void ChaseCamera::update(float dt, const float targetPos[3], float targetYawDegrees,
                         const float targetVel[3]) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;   // 非法 dt：本帧冻结（确定性）

    // ---- boom yaw：目标朝向 + 180°（车尾后方）+ 鼠标 look 累计 + 倒车/回头翻转（XOR）----
    const bool  flip    = (reverse_ != peekBack_);
    const float boomYaw = targetYawDegrees * kDeg2Rad + kPi + boomYawOffset_
                        + (flip ? kPi : 0.0f);
    const float cp = std::cos(pitch_);
    const float boomDir[3] = {
        std::sin(boomYaw) * cp,
        std::sin(pitch_),
        std::cos(boomYaw) * cp
    };

    // ---- look-ahead：观察点 = targetPos + clampMag(targetVel * leadTime, kMaxLead) ----
    float velLead[3] = {
        targetVel[0] * leadTime_,
        targetVel[1] * leadTime_,
        targetVel[2] * leadTime_
    };
    const float leadMag = std::sqrt(velLead[0] * velLead[0] + velLead[1] * velLead[1] +
                                    velLead[2] * velLead[2]);
    if (leadMag > kMaxLead) {
        const float s = kMaxLead / leadMag;
        velLead[0] *= s; velLead[1] *= s; velLead[2] *= s;
    }
    rig_.lookAt[0] = targetPos[0] + velLead[0];
    rig_.lookAt[1] = targetPos[1] + velLead[1];
    rig_.lookAt[2] = targetPos[2] + velLead[2];

    // ---- 期望 eye = targetPos + heightOffset + boomDir * distance ----
    const float desired[3] = {
        targetPos[0] + boomDir[0] * distance_,
        targetPos[1] + height_ + boomDir[1] * distance_,
        targetPos[2] + boomDir[2] * distance_
    };

    // ---- 位置指数阻尼（帧率无关）：eye += (desired - eye) * (1 - exp(-k*dt)) ----
    const float alpha = 1.0f - std::exp(-posK_ * dt);
    rig_.eye[0] += (desired[0] - rig_.eye[0]) * alpha;
    rig_.eye[1] += (desired[1] - rig_.eye[1]) * alpha;
    rig_.eye[2] += (desired[2] - rig_.eye[2]) * alpha;

    // ---- boomYawOffset 自动回正（仅 Chase 模式、本帧无 look 输入；orbit 模式不衰减）----
    if (mode_ == Mode::Chase && !lookActive_) {
        boomYawOffset_ *= std::exp(-returnK_ * dt);
    }
    lookActive_ = false;
}

} // namespace nl::core
