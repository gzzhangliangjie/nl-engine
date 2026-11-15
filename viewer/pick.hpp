#pragma once

// M-I4：屏幕拾取（--pick / --pick-at，docs/p8/P8-INPUT-PLAN.md §7.2）。
//
// 口径来源（三处对照留痕见计划 §7.1）：
//   - UE PlayerController::DeprojectScreenPositionToWorld（PlayerController.h:735
//     实读）：near/far 反投影口径锚——实现为本仓手写 inverseMat4，不抄 UE 代码。
//   - 本仓 math：Mat4 列主序扁平存储（m[0..3]=第 0 列），inverseMat4 可逆性
//     由调用方保证（相机 VP 恒可逆）。
//   - glTF：节点变换已烘进顶点（mesh_io.hpp 约定）→ 顶点 AABB = 模型空间
//     AABB，乘实体世界矩阵得世界 AABB（slab 法求交，最近 t 胜出）。
//
// 零渲染改动：本文件纯 CPU（core math + scene 组件 + mesh_io），结果只进
// log 锚（nl:info|viewer "pick -> ..."）与返回值；viewer 缺省不开 --pick，
// 开启后不动任何渲染路径（A 类零漂移口径）。
//
// 确定性：--pick-at <px,py> 注入式拾取（M-NL2 旗标教训：真实鼠标事件不进对拍）。

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "nl/core/mesh_io.hpp"     // loadGltfScene / SceneMeshData / MeshVertex
#include "nl/core/paths.hpp"       // paths::assetDir（meshFile → 全路径，scene.cpp 同款拼接）
#include "nl/math/math.hpp"       // Mat4 / Vec3 / inverseMat4（nl::math）
#include "nl/scene/Component.hpp"  // TransformComponent / ModelComponent
#include "nl/scene/World.hpp"      // World / EntityID

namespace nl::viewer {

// 数学类型定向 using（scene 侧同款纪律：不进全局、不开 using namespace）。
using nl::math::Mat4;
using nl::math::Vec3;
using nl::math::inverseMat4;

struct PickHit {
    std::uint64_t entityId = 0;   // 0 = 未命中（kInvalidEntity）
    std::string name;             // 实体名；未命中 = ""
    std::string meshFile;         // 命中的模型文件名（诊断面）
    float t = -1.0f;              // 射线参数（世界单位）；未命中 = -1
};

// 屏幕像素 → 世界射线（近点 + 单位方向）。GLFW 光标原点 = 左上。
// NDC：x = px/w*2-1，y = 1 - py/h*2（翻转 y）。vp 列主序扁平（相机 viewProjection）。
inline bool screenRay(const float vp[16], int fbW, int fbH, float px, float py,
                      Vec3& outOrigin, Vec3& outDir) {
    if (fbW <= 0 || fbH <= 0) return false;
    const float ndcX = (px / static_cast<float>(fbW)) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (py / static_cast<float>(fbH)) * 2.0f;

    Mat4 invMat;
    std::memcpy(invMat.m, vp, sizeof(invMat.m));   // 列主序扁平直拷（存储约定一致）
    const Mat4 inv = inverseMat4(invMat);
    // world = inv * v（列向量）：world[c] = Σ_k inv.m[k*4+c] * v[k]。
    float nearW[4] = {}, farW[4] = {};
    {
        const float vn[4] = {ndcX, ndcY, -1.0f, 1.0f};
        const float vf[4] = {ndcX, ndcY, 1.0f, 1.0f};
        for (int c = 0; c < 4; ++c) {
            nearW[c] = inv.m[0 * 4 + c] * vn[0] + inv.m[1 * 4 + c] * vn[1] +
                       inv.m[2 * 4 + c] * vn[2] + inv.m[3 * 4 + c] * vn[3];
            farW[c] = inv.m[0 * 4 + c] * vf[0] + inv.m[1 * 4 + c] * vf[1] +
                      inv.m[2 * 4 + c] * vf[2] + inv.m[3 * 4 + c] * vf[3];
        }
    }
    if (std::fabs(nearW[3]) < 1e-12f || std::fabs(farW[3]) < 1e-12f) return false;
    outOrigin = {nearW[0] / nearW[3], nearW[1] / nearW[3], nearW[2] / nearW[3]};
    const Vec3 farP = {farW[0] / farW[3], farW[1] / farW[3], farW[2] / farW[3]};
    const Vec3 d = {farP.x - outOrigin.x, farP.y - outOrigin.y, farP.z - outOrigin.z};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1e-12f) return false;
    outDir = {d.x / len, d.y / len, d.z / len};
    return true;
}

// 拾取上下文：meshFile → 顶点 AABB 缓存（glTF 解析一次）。
class PickContext {
public:
    // 遍历 modelIds：变换 AABB slab 求交，最近 t 胜出。返回命中（未命中 t=-1）。
    PickHit pick(const nl::scene::World& world, const std::vector<std::uint64_t>& modelIds,
                 const Vec3& origin, const Vec3& dir) {
        PickHit best;
        for (const std::uint64_t id : modelIds) {
            const auto* mc = world.getComponent<nl::scene::ModelComponent>(id);
            const auto* tc = world.getComponent<nl::scene::TransformComponent>(id);
            if (!mc || !tc) continue;
            const std::array<float, 6> box = modelAabb(mc->meshFile);
            if (!boxValid(box)) continue;

            // 世界 AABB = 世界矩阵 × 模型 AABB 8 角。
            const Mat4 w = tc->getWorldMatrix();
            float wmin[3] = {1e30f, 1e30f, 1e30f}, wmax[3] = {-1e30f, -1e30f, -1e30f};
            for (int i = 0; i < 8; ++i) {
                const float p[4] = {box[(i & 1) ? 3 : 0], box[(i & 2) ? 4 : 1],
                                    box[(i & 4) ? 5 : 2], 1.0f};
                float q[4] = {};
                for (int c = 0; c < 4; ++c)
                    q[c] = w.m[0 * 4 + c] * p[0] + w.m[1 * 4 + c] * p[1] +
                           w.m[2 * 4 + c] * p[2] + w.m[3 * 4 + c] * p[3];
                if (q[3] <= 1e-12f) { wmin[0] = 2e30f; break; }   // 退化：整体弃权
                for (int a = 0; a < 3; ++a) {
                    const float v = q[a] / q[3];
                    wmin[a] = v < wmin[a] ? v : wmin[a];
                    wmax[a] = v > wmax[a] ? v : wmax[a];
                }
            }
            if (wmin[0] > 1e29f) continue;

            // slab（D. Ayla 教科书版）：任一轴反序且 tNear > tFar → 无交。
            float tNear = 0.0f, tFar = 1e30f;
            const float ro[3] = {origin.x, origin.y, origin.z};
            const float rd[3] = {dir.x, dir.y, dir.z};
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                if (std::fabs(rd[a]) < 1e-12f) {
                    if (ro[a] < wmin[a] || ro[a] > wmax[a]) miss = true;
                    continue;
                }
                const float inv = 1.0f / rd[a];
                float t0 = (wmin[a] - ro[a]) * inv;
                float t1 = (wmax[a] - ro[a]) * inv;
                if (t0 > t1) { const float tmp = t0; t0 = t1; t1 = tmp; }
                tNear = t0 > tNear ? t0 : tNear;
                tFar = t1 < tFar ? t1 : tFar;
                if (tNear > tFar) miss = true;
            }
            if (miss || tFar < 0.0f) continue;
            const float t = tNear >= 0.0f ? tNear : tFar;   // 相机在盒内 = 出射面
            if (best.entityId == 0 || t < best.t) {
                best.entityId = id;
                best.name = world.getName(id);
                best.meshFile = mc->meshFile;
                best.t = t;
            }
        }
        return best;
    }

private:
    static bool boxValid(const std::array<float, 6>& b) {
        return b[0] <= b[3] && b[1] <= b[4] && b[2] <= b[5];
    }

    const std::array<float, 6>& modelAabb(const std::string& meshFile) {
        auto it = cache_.find(meshFile);
        if (it != cache_.end()) return it->second;
        std::array<float, 6> box = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        nl::core::SceneMeshData data;
        std::string err;
        // ModelComponent.meshFile = assets/ 相对文件名（M0.7 白名单口径），
        // scene.cpp 同款全路径拼接（scene renderer 的模型加载路径）。
        const std::string full = nl::core::paths::assetDir() + "/" + meshFile;
        if (nl::core::loadGltfScene(full.c_str(), data, err)) {
            for (const auto& prim : data.meshes)
                for (const auto& v : prim.vertices)
                    for (int a = 0; a < 3; ++a) {
                        box[a] = v.pos[a] < box[a] ? v.pos[a] : box[a];
                        box[3 + a] = v.pos[a] > box[3 + a] ? v.pos[a] : box[3 + a];
                    }
        }
        return cache_.emplace(meshFile, box).first->second;
    }

    std::unordered_map<std::string, std::array<float, 6>> cache_;
};

} // namespace nl::viewer
