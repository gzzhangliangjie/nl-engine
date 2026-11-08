// M-A5：CPU 蒙皮实现（口径见 skinning.hpp 文件头）。

#include "nl/anim/skinning.hpp"

#include <cmath>

namespace nl::anim {

Mat4 mat4Identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

// 列主序：r.m[c*4+r] = Σ_k a.m[k*4+r] * b.m[c*4+k]
Mat4 mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[static_cast<std::size_t>(k) * 4 + row] *
                     b.m[static_cast<std::size_t>(c) * 4 + k];
            r.m[static_cast<std::size_t>(c) * 4 + row] = s;
        }
    return r;
}

Vec3 mat4TransformPoint(const Mat4& m, const Vec3& p) {
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
    };
}

Vec3 mat4TransformDir(const Mat4& m, const Vec3& d) {
    return {
        m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
        m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
        m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z,
    };
}

Mat4 mat4FromTR(const Vec3& t, const Quat& r) {
    // 单位四元数旋转矩阵（右手系，xyzw）。
    const float xx = r.x * r.x, yy = r.y * r.y, zz = r.z * r.z;
    const float xy = r.x * r.y, xz = r.x * r.z, yz = r.y * r.z;
    const float wx = r.w * r.x, wy = r.w * r.y, wz = r.w * r.z;
    Mat4 m = mat4Identity();
    m.m[0] = 1 - 2 * (yy + zz); m.m[4] = 2 * (xy - wz);     m.m[8]  = 2 * (xz + wy);
    m.m[1] = 2 * (xy + wz);     m.m[5] = 1 - 2 * (xx + zz); m.m[9]  = 2 * (yz - wx);
    m.m[2] = 2 * (xz - wy);     m.m[6] = 2 * (yz + wx);     m.m[10] = 1 - 2 * (xx + yy);
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
    return m;
}

std::string validateSkinBinding(const SkinBinding& binding, std::size_t vertexCount,
                                std::size_t boneCount) {
    if (binding.joints.size() != vertexCount || binding.weights.size() != vertexCount)
        return "skin: joints/weights size != vertex count";
    if (binding.inverseBind.size() != boneCount)
        return "skin: inverseBind size != bone count";
    for (std::size_t i = 0; i < vertexCount; ++i) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const int j = binding.joints[i][static_cast<std::size_t>(k)];
            const float w = binding.weights[i][static_cast<std::size_t>(k)];
            if (w < 0.0f || !std::isfinite(w)) return "skin: negative/non-finite weight";
            if (w == 0.0f) continue;
            if (j < 0 || j >= static_cast<int>(boneCount))
                return "skin: joint index out of range";
            sum += w;
        }
        if (sum < 0.999f || sum > 1.001f) return "skin: weights do not sum to 1";
    }
    for (const Mat4& m : binding.inverseBind)
        for (const float v : m.m)
            if (!std::isfinite(v)) return "skin: non-finite inverse bind matrix";
    return {};
}

void computePalette(std::vector<Mat4>& outPalette,
                    const std::vector<BonePose>& modelPose,
                    const std::vector<Mat4>& inverseBind) {
    outPalette.resize(modelPose.size());
    for (std::size_t i = 0; i < modelPose.size(); ++i) {
        const Mat4 model = mat4FromTR(modelPose[i].translation, modelPose[i].rotation);
        outPalette[i] = mat4Mul(model, inverseBind[i]);
    }
}

void skinPositions(std::vector<Vec3>& out, const std::vector<Vec3>& base,
                   const std::vector<Mat4>& palette, const SkinBinding& binding) {
    const bool alias = (&out == &base);
    std::vector<Vec3> tmp;
    if (alias) tmp.resize(base.size());
    std::vector<Vec3>& dst = alias ? tmp : out;
    dst.resize(base.size());
    for (std::size_t v = 0; v < base.size(); ++v) {
        Vec3 acc{};
        for (int k = 0; k < 4; ++k) {
            const float w = binding.weights[v][static_cast<std::size_t>(k)];
            if (w == 0.0f) continue;
            const Mat4& m = palette[static_cast<std::size_t>(binding.joints[v][static_cast<std::size_t>(k)])];
            const Vec3 p = mat4TransformPoint(m, base[v]);
            acc.x += w * p.x; acc.y += w * p.y; acc.z += w * p.z;
        }
        dst[v] = acc;
    }
    if (alias) out = std::move(tmp);
}

void skinNormals(std::vector<Vec3>& out, const std::vector<Vec3>& baseN,
                 const std::vector<Mat4>& palette, const SkinBinding& binding) {
    const bool alias = (&out == &baseN);
    std::vector<Vec3> tmp;
    if (alias) tmp.resize(baseN.size());
    std::vector<Vec3>& dst = alias ? tmp : out;
    dst.resize(baseN.size());
    for (std::size_t v = 0; v < baseN.size(); ++v) {
        Vec3 acc{};
        for (int k = 0; k < 4; ++k) {
            const float w = binding.weights[v][static_cast<std::size_t>(k)];
            if (w == 0.0f) continue;
            const Mat4& m = palette[static_cast<std::size_t>(binding.joints[v][static_cast<std::size_t>(k)])];
            const Vec3 n = mat4TransformDir(m, baseN[v]);
            acc.x += w * n.x; acc.y += w * n.y; acc.z += w * n.z;
        }
        const float len = std::sqrt(acc.x * acc.x + acc.y * acc.y + acc.z * acc.z);
        if (len > 1e-12f) {
            dst[v] = { acc.x / len, acc.y / len, acc.z / len };
        } else {
            dst[v] = { 0.0f, 0.0f, 0.0f };
        }
    }
    if (alias) out = std::move(tmp);
}

} // namespace nl::anim
