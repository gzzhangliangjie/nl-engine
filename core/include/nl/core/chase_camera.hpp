#pragma once

// P8 M-CAM0：ChaseCamera 跟车相机（纯 CPU，track camera，A 类并行轨道）。
// 兄弟类口径：CameraRig 出矩阵（列主序 / perspectiveZO / lookAt 同 OrbitCamera）；
// 本类只写 update/look/setMode 等状态面。OrbitCamera（core/camera.hpp）零改动零回归。
//
// 设计锚（P8-CAMERA-PLAN §3，UE SpringArm 实读口径）：
//   - 位置 / 旋转 lag 分离：eye 用 posK_ 指数阻尼（factor = exp(-k*dt)，帧率无关），
//     boomYawOffset 用 returnK_ 衰减（UE CameraLagSpeed / CameraRotationLagSpeed
//     双系数口径）；
//   - rig：期望 eye = targetPos + heightOffset + boomDir(yaw + 180° + boomYawOffset,
//     pitch) * distance——boom yaw 在目标朝向基础上 +180°，机位落在车尾后方；
//   - look-ahead：观察点 = targetPos + clampMag(targetVel * leadTime, kMaxLead)；
//   - 模式 Mode::Chase / Mode::Orbit：切换只改「无鼠标输入时 boomYawOffset 是否
//     自动回正」的语义，不动任何位姿状态 → 切换帧位姿逐位连续（锚 C）；
//   - 驾驶变体：reverseView（倒车自动后视）+ peekBack（回头键按住瞬切），实现 =
//     视角 yaw +180°，两者叠加取 XOR（都开 = 看回前方）；
//   - 确定性契约：update 只吃 dt 与输入位姿，无 wall-clock 无随机；固定 dt 下
//     位姿逐位可复现；跨 dt 是收敛性锚（阈值），与逐位锚显式分档。
//
// 角度约定：yaw/pitch 内部存 rad，对外接口用度；forward(yaw) = (sin, 0, cos)
// （yaw=0 朝 +Z，yaw=90° 朝 +X），与 FreeCamera / viewer HUD 同一口径。

#include "nl/core/camera_rig.hpp"

namespace nl::core {

class ChaseCamera {
public:
    enum class Mode { Chase, Orbit };

    ChaseCamera() = default;

    void reset();
    void setAspect(float aspect);
    void setFovYDegrees(float fovYDegrees);   // clamp 20..120（OrbitCamera 同款）
    void setFarZ(float farZ);                 // viewer 大场景取景用（缺省 100）

    // 正交/透视（M-CAM0 基座扩展；透视缺省，正交用途 = 俯视/工程调试视图）
    void setProjection(CameraRig::Projection p)  { rig_.projection = p; }
    void setOrthoHeight(float height)            { if (height > 0.0f) rig_.orthoHeight = height; }
    CameraRig::Projection projection() const     { return rig_.projection; }

    // ---- 可调参数（M-CAM2 console cvar 的落点；全带 clamp）----
    void setDistance(float distance);         // 目标到机位臂长 clamp [1, 50]
    void setHeightOffset(float height);       // 机位抬升 clamp [0, 10]
    void setStiffness(float k);               // 位置阻尼系数 clamp [0.1, 50]
    void setBoomReturnRate(float rate);       // boomYawOffset 自动回正速率 clamp [0.1, 20]
    void setLeadTime(float seconds);          // look-ahead 前视时长 clamp [0, 1]
    void setPitchDegrees(float pitchDegrees); // 俯仰 clamp ±kMaxPitch

    float distance() const        { return distance_; }
    float heightOffset() const    { return height_; }
    float stiffness() const       { return posK_; }
    float leadTime() const        { return leadTime_; }
    float pitchDegrees() const;
    float boomYawOffsetDegrees() const;       // 锚点 / HUD 用只读

    // ---- 交互 ----
    // 鼠标 look（像素增量）：按 kLookGain 累计进 boomYawOffset / pitch（带 clamp），
    // 并置 lookActive_（本帧不自动回正；update 消费后清零）。
    void look(float deltaX, float deltaY);
    void setMode(Mode mode);                  // C 边沿切换；只改语义，位姿逐位不动
    Mode mode() const { return mode_; }

    // 驾驶变体（机制 M-CAM0 落；viewer 接线 M-CAM2）
    void setReverse(bool on);                 // 倒车自动后视
    void setPeekBack(bool on);                // 回头键按住（松开由调用方置 false）

    // ---- 帧更新（确定性核心）----
    // targetPos：目标世界坐标；targetYawDegrees：目标朝向；targetVel：目标速度
    // （look-ahead 与倒车判定由调用方负责，本类只消费数值）。
    void update(float dt, const float targetPos[3], float targetYawDegrees,
                const float targetVel[3]);

    // ---- 只读输出口径（rig 透传，同 OrbitCamera）----
    Matrix4 viewProjection() const { return rig_.viewProjection(); }
    Matrix4 view() const           { return rig_.view(); }
    void eyePosition(float out[3]) const { rig_.eyePosition(out); }

    static constexpr float kMaxPitch = 1.45f;      // ±83°（OrbitCamera 同款口径）
    static constexpr float kMaxBoomYaw = 3.14159f; // boomYawOffset clamp ±180°
    static constexpr float kMaxLead = 4.0f;        // look-ahead 距离上限（世界单位）

private:
    CameraRig rig_;
    Mode mode_ = Mode::Chase;

    float distance_ = 6.0f;
    float height_   = 2.0f;
    float posK_     = 5.0f;           // 位置阻尼系数
    float returnK_  = 3.0f;           // boomYawOffset 自动回正速率
    float leadTime_ = 0.3f;
    float pitch_    = 0.35f;          // rad（约 20° 俯视）
    float boomYawOffset_ = 0.0f;      // rad，鼠标 look 累计
    bool  lookActive_    = false;     // 本帧有 look 输入（look() 置位，update 清零）
    bool  reverse_       = false;
    bool  peekBack_      = false;
};

} // namespace nl::core
