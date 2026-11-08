#pragma once

// M0.4：纯 CPU 的轨道相机。core 层不接触任何图形 API / NVRHI 类型。
// 矩阵是列主序（column-major）float[16]，直接喂给 HLSL/Slang cbuffer 里的 float4x4，
// DXIL 与 SPIR-V 两个目标用同一份数值，保证两后端画面一致。

namespace nl::core {

struct Matrix4 {
    float m[16] = {};
};

class OrbitCamera {
public:
    OrbitCamera() = default;

    void reset();
    void setAspect(float aspect);
    // 自动化验收用：固定相机角度出图，两个后端逐像素对照。
    void setOrbit(float yawDegrees, float pitchDegrees, float distance);

    // 鼠标左键拖动（像素增量）：yaw/pitch 有上下限，避免翻过头看不出方向。
    void orbit(float deltaX, float deltaY);
    // 滚轮：正 = 拉近。距离有上下限，避免穿进模型或远到看不见。
    void zoom(float scrollDelta);

    Matrix4 viewProjection() const;
    Matrix4 model() const;                 // M0.4 只画一个静态模型，返回单位阵

    float yawDegrees() const;
    float pitchDegrees() const;
    float distance() const;
    float aspect() const { return aspect_; }

private:
    float yaw_      = 0.21f;   // rad（约 12°），默认能看到 Suzanne 正脸
    float pitch_    = 0.16f;   // rad
    float distance_ = 4.2f;
    float aspect_   = 16.0f / 9.0f;

    static constexpr float kFovY      = 0.785398163f;  // 45°
    static constexpr float kNear      = 0.1f;
    static constexpr float kFar       = 100.0f;
    static constexpr float kMinPitch  = -1.45f;
    static constexpr float kMaxPitch  =  1.45f;
    // 允许滚轮推进模型内部：cull=back 时全被剔除只剩背景，
    // cull=none 时能看到带贴图的内壁 —— M0.4 的可见对照用例。
    static constexpr float kMinDist   = 0.45f;
    // M-A5.4：20 → 500。Fox.glb 等资产坐标 ~80 单位，20 的上限会把相机钳在
    // 模型内部（skin 模式自动取景 dist≈radius*2.4 一样被钳）。历史用例
    // dist ≤ 20 不受影响；远景裁剪由 viewProjection 的自适应远平面兜住。
    static constexpr float kMaxDist   = 500.0f;
    static constexpr float kOrbitGain = 0.007f;
    static constexpr float kZoomGain  = 0.16f;
};

// 不创建窗口、不碰 GPU 的机械自检（--selftest 用）。
bool cameraSelfTest();

} // namespace nl::core
