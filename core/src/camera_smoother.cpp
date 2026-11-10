// P8 M-CAM4：CameraSmoother / CameraTrauma 实现（camera_smoother.hpp 口径）。
// 纯 CPU 零图形 API；确定性：固定 dt 下逐位可复现（cam-tools F/G/H 锚定）。

#include "nl/core/camera_smoother.hpp"

#include <cmath>

namespace nl::core {
namespace {

// Unity SmoothDamp（critically damped）：纯 dt 函数，逐位确定。
float smoothDamp(float current, float target, float& vel, float smoothTime, float dt) {
    smoothTime = smoothTime < 1e-4f ? 1e-4f : smoothTime;
    const float omega = 2.0f / smoothTime;
    const float x = omega * dt;
    const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = current - target;
    const float temp = (vel + omega * change) * dt;
    vel = (vel - omega * temp) * exp;
    return target + (change + temp) * exp;
}

} // namespace

void CameraSmoother::reset(const float e[3], const float l[3]) {
    for (int i = 0; i < 3; ++i) {
        eye[i] = e[i];       lookAt[i] = l[i];
        targetEye_[i] = e[i]; targetLook_[i] = l[i];
        velEye[i] = 0.0f;    velLook[i] = 0.0f;
    }
}

void CameraSmoother::setTarget(const float e[3], const float l[3]) {
    for (int i = 0; i < 3; ++i) {
        targetEye_[i] = e[i];
        targetLook_[i] = l[i];
    }
}

void CameraSmoother::step(float dt) {
    if (!enabled) {
        for (int i = 0; i < 3; ++i) { eye[i] = targetEye_[i]; lookAt[i] = targetLook_[i]; }
        return;
    }
    for (int i = 0; i < 3; ++i)
        eye[i]    = smoothDamp(eye[i],    targetEye_[i],    velEye[i],  smoothTime, dt);
    for (int i = 0; i < 3; ++i)
        lookAt[i] = smoothDamp(lookAt[i], targetLook_[i], velLook[i], smoothTime, dt);
}

void CameraSmoother::smoothedEye(float out[3]) const {
    out[0] = eye[0]; out[1] = eye[1]; out[2] = eye[2];
}
void CameraSmoother::smoothedLookAt(float out[3]) const {
    out[0] = lookAt[0]; out[1] = lookAt[1]; out[2] = lookAt[2];
}

void CameraTrauma::addTrauma(float v) {
    trauma = v > trauma ? v : trauma;          // 取最大注入，不叠加爆表
    if (trauma > 1.0f) trauma = 1.0f;
}
void CameraTrauma::step(float dt) {
    stepCount += 1;
    phase += frequency * dt;                    // 确定性：固定 dt → 固定相位推进
    if (trauma > 0.0f) {
        trauma -= decay * dt;
        if (trauma < 0.0f) trauma = 0.0f;
    }
}
float CameraTrauma::trauma01() const {
    return trauma < 0.0f ? 0.0f : (trauma > 1.0f ? 1.0f : trauma);
}
void CameraTrauma::offsets(float& yawRad, float& pitchRad, float& rollRad) const {
    const float s = trauma01();
    const float k = s * s;                     // trauma²（Eiserloh）
    const float sa = static_cast<float>(seed & 0xffu)        * (1.0f / 255.0f);
    const float sb = static_cast<float>((seed >> 8)  & 0xffu) * (1.0f / 255.0f);
    const float sc = static_cast<float>((seed >> 16) & 0xffu) * (1.0f / 255.0f);
    yawRad   = k * amplitude * std::sin(phase * 1.0f + sa * 6.2831853f);
    pitchRad = k * amplitude * std::sin(phase * 1.3f + sb * 6.2831853f + 2.0f);
    rollRad  = k * amplitude * std::sin(phase * 1.7f + sc * 6.2831853f + 4.0f);
}

} // namespace nl::core
