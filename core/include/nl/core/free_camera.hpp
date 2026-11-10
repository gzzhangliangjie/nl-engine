#pragma once

// P8 M-CAM0：FreeCamera 纯自由飞行相机（spectator / fly-through，纯 CPU）。
// 无阻尼直出（调试相机跟手优先，无 lag 是特性不是缺陷——计划 §5 口径）。
//
// 设计锚（P8-CAMERA-PLAN §3，UE 编辑器视口相机 PIE spectator 实读口径）：
//   - 状态：eye + yaw/pitch（欧拉，俯仰 clamp）+ fov；无阻尼；
//   - update 只吃归一化轴：moveInput[3] = [右向±1, 升降±1, 前向±1]（键位语义由
//     外部映射层翻译，相机不认键码）；lookDelta[2] = [yaw 增量 rad, pitch 增量 rad]
//     （增益在映射层，相机吃弧度）；boost = 速度档倍率；
//   - 移动 = (forward*m[2] + right*m[0] + worldUp*m[1]) * speed * (boost ? kBoost : 1) * dt；
//     forward 含俯仰分量（W/S 沿视线飞，UE spectator 同款），right 水平，升降走世界 Y；
//   - 确定性契约：update 只吃 dt 与输入，固定 dt 位姿逐位可复现。
//
// 角度约定：yaw/pitch 内部存 rad，对外接口用度；forward(yaw,pitch) =
// (sin(yaw)*cos(pitch), sin(pitch), cos(yaw)*cos(pitch))（yaw=0/pitch=0 朝 +Z，
// yaw=90° 朝 +X），与 ChaseCamera / viewer HUD 同一口径。

#include "nl/core/camera_rig.hpp"

namespace nl::core {

class FreeCamera {
public:
    FreeCamera() = default;

    void reset();
    void setAspect(float aspect);
    void setFovYDegrees(float fovYDegrees);   // clamp 20..120（OrbitCamera 同款）
    void setFarZ(float farZ);                 // viewer 大场景取景用（缺省 100）
    void setSpeed(float unitsPerSecond);      // clamp (0, 500]

    // 正交/透视（M-CAM0 基座扩展；透视缺省）
    void setProjection(CameraRig::Projection p)  { rig_.projection = p; }
    void setOrthoHeight(float height)            { if (height > 0.0f) rig_.orthoHeight = height; }
    CameraRig::Projection projection() const     { return rig_.projection; }

    float speed() const       { return speed_; }
    float yawDegrees() const;
    float pitchDegrees() const;

    // ---- 帧更新（确定性核心）----
    // moveInput[3]：[0]=右向 ±1，[1]=升降 ±1，[2]=前向 ±1（映射层负责归一化）；
    // lookDelta[2]：[0]=yaw 增量 rad，[1]=pitch 增量 rad；boost：速度档。
    void update(float dt, const float moveInput[3], const float lookDelta[2], bool boost);

    // ---- 只读输出口径（rig 透传，同 OrbitCamera）----
    Matrix4 viewProjection() const { return rig_.viewProjection(); }
    Matrix4 view() const           { return rig_.view(); }
    void eyePosition(float out[3]) const { rig_.eyePosition(out); }

    static constexpr float kMaxPitch = 1.45f;   // ±83°（OrbitCamera 同款口径）
    static constexpr float kBoost    = 4.0f;    // 加速档倍率

private:
    CameraRig rig_;
    float yaw_   = 0.0f;              // rad
    float pitch_ = 0.0f;              // rad
    float speed_ = 10.0f;             // 世界单位 / 秒
};

} // namespace nl::core
