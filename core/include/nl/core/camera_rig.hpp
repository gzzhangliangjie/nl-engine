#pragma once

// P8 M-CAM0：CameraRig 轻量基座（玩法相机族三兄弟共享的数据口径，track camera）。
// 只共享「位姿 + 投影参数 + Matrix4 列主序输出口径」，不共享更新逻辑——
// 各相机（Chase / Free / 未来的 Cinematic）update 各写，基座只管出矩阵。
//
// 口径同 OrbitCamera（core/camera.hpp，本轨零改动零回归）：
//   - 纯 CPU，不接触任何图形 API / NVRHI 类型；
//   - 列主序 float[16]，直接喂 HLSL/Slang cbuffer float4x4；
//   - perspectiveZO（深度 [0,1]，DX12/Vulkan 一套矩阵）+ lookAt（右手系、up=(0,1,0)）；
//   - viewProjection() / view() / eyePosition() 只读，同一组 eye 数值不重复算两套。
//
// M-CAM0 基座扩展（2026-11-10 用户拍板）：正交/透视双模式。
//   - Perspective 缺省：Chase/Free/既有口径零影响；
//   - Orthographic：orthoHeight 定义竖直取景高（世界单位），水平 = orthoHeight*aspect；
//     正交矩阵同款 ZO 深度 [0,1]（D3D XMMatrixOrthographicOffCenterZO 同构，
//     中心对称 ±width/2 ±height/2）。

#include "nl/core/camera.hpp"   // Matrix4

namespace nl::core {

struct CameraRig {
    enum class Projection { Perspective, Orthographic };

    // ---- 共享参数（各相机透传自己的 setter，cvar 接线在 viewer 侧）----
    Projection projection = Projection::Perspective;
    float fovYDegrees = 45.0f;      // 仅 Perspective 用（clamp 20..120 由相机 setter 负责）
    float orthoHeight = 10.0f;      // 仅 Orthographic 用：竖直取景高（世界单位，>0）
    float aspect      = 16.0f / 9.0f;
    float nearZ       = 0.1f;
    float farZ        = 100.0f;

    // ---- 位姿（由各相机 update() 写入；本结构不做任何阻尼/插值）----
    float eye[3]    = {0.0f, 0.0f, 0.0f};
    float lookAt[3] = {0.0f, 0.0f, -1.0f};

    Matrix4 view() const;
    Matrix4 viewProjection() const;   // 按 projection 分派透视/正交
    void eyePosition(float out[3]) const;   // 与 view() 同一组 eye，同源口径
};

} // namespace nl::core
