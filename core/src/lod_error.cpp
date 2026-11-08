#include "nl/core/lod_error.hpp"

#include <cmath>

namespace nl::core {

float screenErrorPixels(float worldError,
                        const float worldSphereCenter[3], float worldSphereRadius,
                        const float eye[3],
                        float projYScale,
                        float viewportHeight, float nearZ) {
    if (!std::isfinite(worldError) || !std::isfinite(projYScale) ||
        !std::isfinite(viewportHeight) || !std::isfinite(nearZ) ||
        viewportHeight <= 0.0f) {
        return 0.0f;
    }
    const float dx = worldSphereCenter[0] - eye[0];
    const float dy = worldSphereCenter[1] - eye[1];
    const float dz = worldSphereCenter[2] - eye[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - worldSphereRadius;
    const float dd = d > nearZ ? d : nearZ;   // max(d, near)：球内不爆
    return worldError / dd * projYScale * 0.5f * viewportHeight;
}

bool lodErrorSelfTest() {
    // 45° fov：projYScale = 1/tan(22.5°) = 2.41421356237...
    const float projY = 2.41421356f;
    const float nearZ = 0.1f;
    const float vh = 720.0f;
    const float eye[3] = {0.0f, 0.0f, 0.0f};

    // 例 1（基线）：err=0.1，d=4.0
    //   px = 0.1/4 * 2.41421356 * 0.5 * 720 = 21.727922...
    {
        const float c[3] = {4.0f, 0.0f, 0.0f};
        const float px = screenErrorPixels(0.1f, c, 0.0f, eye, projY, vh, nearZ);
        if (std::fabs(px - 21.727922f) > 1e-3f) return false;
    }
    // 例 2（半径扣减）：球心距 5.0、半径 1.0 -> d=4.0，与例 1 同值。
    {
        const float c[3] = {5.0f, 0.0f, 0.0f};
        const float px = screenErrorPixels(0.1f, c, 1.0f, eye, projY, vh, nearZ);
        if (std::fabs(px - 21.727922f) > 1e-3f) return false;
    }
    // 例 3（near 截断）：d = 0.5-1.0 = -0.5（相机在球内）-> max(d,near)=0.1
    //   px = 1.0/0.1 * 2.41421356 * 0.5 * 720 = 8691.1688...
    {
        const float c[3] = {0.5f, 0.0f, 0.0f};
        const float px = screenErrorPixels(1.0f, c, 1.0f, eye, projY, vh, nearZ);
        if (std::fabs(px - 8691.1688f) > 1e-1f) return false;
    }
    // 例 4（单调性 + 线性）：d=2 -> px 加倍（0.1/2 -> 43.455844）。
    {
        const float c[3] = {2.0f, 0.0f, 0.0f};
        const float px = screenErrorPixels(0.1f, c, 0.0f, eye, projY, vh, nearZ);
        if (std::fabs(px - 43.455844f) > 1e-3f) return false;
    }
    // 例 5（偏轴不爆）：球心在斜方向，距离与例 1 相同 -> 同值（欧氏距离口径）。
    {
        const float c[3] = {2.3094011f, 2.3094011f, 2.3094011f};  // |c| = 4.0
        const float px = screenErrorPixels(0.1f, c, 0.0f, eye, projY, vh, nearZ);
        if (std::fabs(px - 21.727922f) > 1e-3f) return false;
    }
    return true;
}

} // namespace nl::core
