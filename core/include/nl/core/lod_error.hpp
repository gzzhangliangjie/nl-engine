#pragma once

// M5.0（S5.0）：屏幕空间误差投影——S5 运行时组原子 cut 的选层判据。
// 纯 CPU 数学，core 层零图形符号；M5.1 的 compute 遍历在 shader 端用同一公式。

namespace nl::core {

// 世界空间误差 -> 屏幕像素误差。
// 口径（2026-11-08 用户拍板）：bevy cull_shared.wesl:13（溯源 zeux meshoptimizer
// demo/nanite.cpp:115），与 S3 error chain 同一权威源：
//
//   screenPx = worldError / max(d, near) * projYScale * 0.5 * viewportHeight
//   d        = |worldSphereCenter - eye| - worldSphereRadius   （保守：球最近点）
//   projYScale = 投影矩阵 [1][1] = 1/tan(fovY/2)
//
// near 截断防止相机进入球内时误差爆到无穷；半径扣减让包围球侧面（偏轴）簇
// 不被高估细节。不可感知判据（S5 cut 语义）：screenPx < cutThresholdPx。
float screenErrorPixels(float worldError,
                        const float worldSphereCenter[3], float worldSphereRadius,
                        const float eye[3],
                        float projYScale,
                        float viewportHeight, float nearZ);

// 手算对照自检（--selftest）：固定相机 / 已知 worldError 的球，逐例对比
// 手算值（45° fov：projYScale = 1/tan(22.5°) = 2.41421356...）。
bool lodErrorSelfTest();

} // namespace nl::core
