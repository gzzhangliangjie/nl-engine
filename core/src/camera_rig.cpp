// P8 M-CAM0：CameraRig 基座实现（track camera）。
// 矩阵口径与 core/src/camera.cpp（OrbitCamera）同源：perspectiveZO + lookAt、
// 列主序、深度 [0,1]。OrbitCamera 的匿名命名空间实现一字不动（零回归），
// 本文件是兄弟类共享口径的独立副本——改口径必须两处一起核（§ SHADER-CONVENTIONS）。

#include "nl/core/camera_rig.hpp"

#include <cmath>

namespace nl::core {
namespace {

constexpr float kPi = 3.14159265358979323846f;

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 out{};   // out = a * b，列主序
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            out.m[col * 4 + row] = sum;
        }
    }
    return out;
}

// 右手系、look down -Z、深度范围 [0,1]（同 camera.cpp：D3D12 与 Vulkan 共用一套）。
Matrix4 perspectiveZO(float fovY, float aspect, float nearZ, float farZ) {
    Matrix4 p{};
    const float f = 1.0f / std::tan(fovY * 0.5f);
    p.m[0]  = f / aspect;
    p.m[5]  = f;
    p.m[10] = farZ / (nearZ - farZ);
    p.m[11] = -1.0f;
    p.m[14] = (nearZ * farZ) / (nearZ - farZ);
    return p;
}

// 正交 ZO（深度 [0,1]）：D3D XMMatrixOrthographicOffCenterZO 同构，中心对称。
// orthoHeight = 竖直取景高（世界单位），水平 = orthoHeight * aspect。
Matrix4 orthographicZO(float orthoHeight, float aspect, float nearZ, float farZ) {
    Matrix4 p{};
    const float h = 2.0f / orthoHeight;
    p.m[0]  = h / aspect;
    p.m[5]  = h;
    p.m[10] = 1.0f / (nearZ - farZ);
    p.m[14] = nearZ / (nearZ - farZ);
    p.m[11] = -1.0f;
    p.m[15] = 1.0f;
    return p;
}

Matrix4 makeLookAt(const float eye[3], const float center[3], const float up[3]) {
    float zx = eye[0] - center[0], zy = eye[1] - center[1], zz = eye[2] - center[2];
    float len = std::sqrt(zx * zx + zy * zy + zz * zz);
    zx /= len; zy /= len; zz /= len;                 // camera backward

    float xx = up[1] * zz - up[2] * zy;
    float xy = up[2] * zx - up[0] * zz;
    float xz = up[0] * zy - up[1] * zx;
    len = std::sqrt(xx * xx + xy * xy + xz * xz);
    xx /= len; xy /= len; xz /= len;                 // camera right

    const float yx = zy * xz - zz * xy;
    const float yy = zz * xx - zx * xz;
    const float yz = zx * xy - zy * xx;             // camera up

    Matrix4 v{};
    v.m[0] = xx; v.m[1] = yx; v.m[2] = zx; v.m[3] = 0.0f;
    v.m[4] = xy; v.m[5] = yy; v.m[6] = zy; v.m[7] = 0.0f;
    v.m[8] = xz; v.m[9] = yz; v.m[10] = zz; v.m[11] = 0.0f;
    v.m[12] = -(xx * eye[0] + xy * eye[1] + xz * eye[2]);
    v.m[13] = -(yx * eye[0] + yy * eye[1] + yz * eye[2]);
    v.m[14] = -(zx * eye[0] + zy * eye[1] + zz * eye[2]);
    v.m[15] = 1.0f;
    return v;
}

} // namespace

Matrix4 CameraRig::view() const {
    const float up[3] = {0.0f, 1.0f, 0.0f};
    return makeLookAt(eye, lookAt, up);
}

Matrix4 CameraRig::viewProjection() const {
    const Matrix4 v = view();
    if (projection == Projection::Orthographic) {
        return multiply(orthographicZO(orthoHeight, aspect, nearZ, farZ), v);
    }
    return multiply(perspectiveZO(fovYDegrees * kPi / 180.0f, aspect, nearZ, farZ), v);
}

void CameraRig::eyePosition(float out[3]) const {
    out[0] = eye[0];
    out[1] = eye[1];
    out[2] = eye[2];
}

} // namespace nl::core
