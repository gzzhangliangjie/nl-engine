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

    // M-C2：控制台运行时调参（cam.fov / cam.dist 的落点）。
    // setDistance 与 zoom 同一个 clamp；setFovYDegrees clamp 20°..120°（过窄过广都失真）。
    // 缺省值与历史 kFovY 一致 —— 老基线/回归零波及。
    void setDistance(float distance);
    void setFovYDegrees(float fovYDegrees);
    float fovYDegrees() const;

    Matrix4 viewProjection() const;
    Matrix4 view() const;                  // M6.1：world→view（hiz 遍历 Mara&Morgan
                                           //  球投影消费；与 viewProjection 的 lookAt 同源）
    Matrix4 model() const;                 // M0.4 只画一个静态模型，返回单位阵

    float yawDegrees() const;
    float pitchDegrees() const;
    float distance() const;
    float aspect() const { return aspect_; }

    // M5.0：屏幕误差投影需要的相机侧数据（只读，不改任何相机状态）。
    // eyePosition = viewProjection() 里 lookAt 用的同一组 eye（不重复算两套）。
    void  eyePosition(float out[3]) const;
    float projectionYScale() const;   // 投影 [1][1] = 1/tan(fovY/2)
    // M-DEPTH0：near 随取景距离自适应（远景门控），viewProjection() 每次调用刷新
    // nearZ_。门控内恒等 kNear，历史基线（dist ≤ kNearGate 的全部用例）零漂。
    float nearPlane() const { return nearZ_; }

private:
    float yaw_      = 0.21f;   // rad（约 12°），默认能看到 Suzanne 正脸
    float pitch_    = 0.16f;   // rad
    float distance_ = 4.2f;
    float aspect_   = 16.0f / 9.0f;
    float fovY_     = kFovY;          // 缺省 = 历史 kFovY（M-C2 起可运行时改，setFovYDegrees clamp 20°..120°）
    mutable float nearZ_ = kNear;     // M-DEPTH0：viewProjection() 刷新（门控内恒 kNear）；const 视图函数内更新缓存

    static constexpr float kFovY      = 0.785398163f;  // 45°（fovY_ 缺省值；M-C2 起仅作初值）
    static constexpr float kNear      = 0.1f;
    static constexpr float kFar       = 100.0f;
    // M-DEPTH0：远景深度精度。标准 Z + near=0.1 时，z 处世界深度分辨率
    // ≈ ε_ndc·z²/near（float32 ε_ndc≈6e-8）→ dist=2600 航拍处 ≈4 m，0.12 m 厚
    // 的路面 slab 全场 z-fight（航拍碎白线根因，M-CITY-ROAD 挂账）。修复 =
    // 远景门控放大近裁面：dist > kNearGate 时 near = dist·kNearRatio
    // （2600 → 40.6，z=2600 处分辨率 ≈ 0.01 m）。门控内（dist ≤ 200，现有
    // 148 张远景卡里最大 dist=48）near 恒 = kNear，历史基线逐位不变。
    // ratio=1/64 依据：Δz = ε·z²/n ≈ ε·z²/(dist/64) = ε·64·z；z=2600 → 1 cm。
    static constexpr float kNearGate  = 200.0f;
    static constexpr float kNearRatio = 1.0f / 64.0f;
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
