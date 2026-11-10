#pragma once

// P8 M-CAM4：相机平滑 / 过渡 / 防抖（纯 CPU，零图形 API，确定性）。
// 二阶 critically damped spring（Unity SmoothDamp 同款模型）→ 位置 / 朝向双通道独立。
// 默认关（Godot Camera3D「平滑是属性不是相机」口径）：开时叠加在 chase/free 阻尼之上，
// 既有 A~E 锚逐位不动；轨迹相机自由控制。
// trauma shake 用 GDC Eiserloh 的 trauma² 模型，固定 seed + 帧序相位（无 wall-clock），
// 同 seed 重放逐位一致。
//
// 用法（viewer 侧，见 viewer/camera_mode.hpp）：每帧把原始 (eye, lookAt) 喂 setTarget，
// step(dt) 推进平滑；需要抖动用 trauma.addTrauma 注入、step 衰减、offsets 取偏移。

#include <cstdint>

namespace nl::core {

// 二阶 critically damped spring：位置 (eye) 与朝向 (lookAt) 独立平滑。
struct CameraSmoother {
    bool  enabled    = false;        // 默认关（既有锚逐位不动）
    float smoothTime = 0.15f;        // 收敛时间常数（秒）；越小越跟手
    float eye[3]     = {0.0f, 0.0f, 0.0f};     // 平滑后 eye
    float lookAt[3]  = {0.0f, 0.0f, -1.0f};    // 平滑后 lookAt
    float velEye[3]  = {0.0f, 0.0f, 0.0f};     // spring 速度态（内部）
    float velLook[3] = {0.0f, 0.0f, 0.0f};

    void reset(const float eyeIn[3], const float lookAtIn[3]);   // 跳变不插值（切模式/初始化）
    void setTarget(const float eyeIn[3], const float lookAtIn[3]); // 每帧喂原始目标
    void step(float dt);                 // 推进平滑（固定 dt 逐位确定）
    void smoothedEye(float out[3]) const;
    void smoothedLookAt(float out[3]) const;

private:
    float targetEye_[3]  = {0.0f, 0.0f, 0.0f};
    float targetLook_[3] = {0.0f, 0.0f, -1.0f};
};

// 确定性 trauma shake（GDC Eiserloh）：trauma 0..1 线性衰减，偏移 = trauma² ×
// 固定 seed 正弦噪声；相位按 step 帧序推进（确定性，非 wall-clock）。
struct CameraTrauma {
    float    trauma    = 0.0f;   // 当前创伤 [0,1]（事件注入）
    float    decay     = 1.0f;   // 每秒线性衰减
    float    amplitude = 0.25f;  // 最大偏移（弧度，yaw/pitch/roll 同幅）
    float    frequency = 18.0f;  // 每秒相位推进（Hz 风格，确定性非 wall-clock）
    uint32_t seed      = 12345u; // 固定 seed → 同 seed 重放逐位一致
    uint32_t stepCount = 0;      // 内部帧序（确定性）
    float    phase     = 0.0f;   // 累积相位

    void addTrauma(float v);                 // 注入并 clamp [0,1]
    void step(float dt);                     // 衰减 + 相位推进
    void offsets(float& yawRad, float& pitchRad, float& rollRad) const; // trauma² × 噪声
    float trauma01() const;
};

} // namespace nl::core
