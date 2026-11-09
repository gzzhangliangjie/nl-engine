// M-E1：TransformComponent 实现层。
// 口径见 Component.hpp 文件头（Carrot TransformComponent.cpp / Math Transform.cpp
// 2026-11-09 精读留痕）。

#include "nl/scene/Component.hpp"

#include <cassert>

// 数学函数注入（anim 内部 cpp 同款：文件级 using namespace，头不污染）。
using namespace nl::math;

namespace nl::scene {

Mat4 TransformComponent::getWorldMatrix() const {
    const Entity self = getEntity();
    assert(self.world && "TransformComponent owner must be bound to a World");
    const std::optional<EntityID> parent = self.world->getParentID(self.getID());
    if (parent) {
        if (const TransformComponent* parentComp =
                self.world->getComponent<TransformComponent>(*parent)) {
            return mulMat4(parentComp->getWorldMatrix(), local.toMatrix());
        }
    }
    return local.toMatrix();
}

Vec3 TransformComponent::getWorldPosition() const {
    return mat4TransformPoint(getWorldMatrix(), Vec3{0.0f, 0.0f, 0.0f});
}

void TransformComponent::setWorldTransform(const Transform& world) {
    setWorldFromMatrix(world.toMatrix());
}

void TransformComponent::setWorldFromMatrix(const Mat4& worldMat) {
    const Entity self = getEntity();
    assert(self.world && "TransformComponent owner must be bound to a World");
    const std::optional<EntityID> parent = self.world->getParentID(self.getID());
    if (parent) {
        if (const TransformComponent* parentComp =
                self.world->getComponent<TransformComponent>(*parent)) {
            // localMat = inverse(parentWorld) * worldMat，然后 TRS 分解。
            // 严格成立条件：复合无剪切（锚点用例用均匀 scale 父）。
            const Mat4 inv = inverseMat4(parentComp->getWorldMatrix());
            const Mat4 localMat = mulMat4(inv, worldMat);

            local.position = { localMat.m[12], localMat.m[13], localMat.m[14] };

            // 各列长度 = scale（非负）；列归一化 = 旋转（无剪切时列正交）。
            const float sx = std::sqrt(localMat.m[0] * localMat.m[0] + localMat.m[1] * localMat.m[1] + localMat.m[2] * localMat.m[2]);
            const float sy = std::sqrt(localMat.m[4] * localMat.m[4] + localMat.m[5] * localMat.m[5] + localMat.m[6] * localMat.m[6]);
            const float sz = std::sqrt(localMat.m[8] * localMat.m[8] + localMat.m[9] * localMat.m[9] + localMat.m[10] * localMat.m[10]);
            local.scale = { sx, sy, sz };

            if (sx > 1e-12f && sy > 1e-12f && sz > 1e-12f) {
                // 归一化后的旋转矩阵列主序，重组成四元数（Shepperd 法）。
                const float r00 = localMat.m[0] / sx, r10 = localMat.m[1] / sx, r20 = localMat.m[2] / sx;
                const float r01 = localMat.m[4] / sy, r11 = localMat.m[5] / sy, r21 = localMat.m[6] / sy;
                const float r02 = localMat.m[8] / sz, r12 = localMat.m[9] / sz, r22 = localMat.m[10] / sz;
                const float trace = r00 + r11 + r22;
                if (trace > 0.0f) {
                    const float s = std::sqrt(trace + 1.0f) * 2.0f; // 4w
                    local.rotation = { (r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s };
                } else if (r00 > r11 && r00 > r22) {
                    const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f; // 4x
                    local.rotation = { 0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s };
                } else if (r11 > r22) {
                    const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f; // 4y
                    local.rotation = { (r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s };
                } else {
                    const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f; // 4z
                    local.rotation = { (r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s };
                }
                local.rotation = normalizeQuat(local.rotation);
            } else {
                local.rotation = quatIdentity();  // 退化 scale：旋转无意义
            }
            return;
        }
    }
    // 无父 / 父无 Transform：世界即局部（Carrot setGlobalTransform 尾段口径）。
    // 从 worldMat 直接分解 TRS。
    local.position = { worldMat.m[12], worldMat.m[13], worldMat.m[14] };
    const float sx = std::sqrt(worldMat.m[0] * worldMat.m[0] + worldMat.m[1] * worldMat.m[1] + worldMat.m[2] * worldMat.m[2]);
    const float sy = std::sqrt(worldMat.m[4] * worldMat.m[4] + worldMat.m[5] * worldMat.m[5] + worldMat.m[6] * worldMat.m[6]);
    const float sz = std::sqrt(worldMat.m[8] * worldMat.m[8] + worldMat.m[9] * worldMat.m[9] + worldMat.m[10] * worldMat.m[10]);
    local.scale = { sx, sy, sz };
    if (sx > 1e-12f && sy > 1e-12f && sz > 1e-12f) {
        const float r00 = worldMat.m[0] / sx, r10 = worldMat.m[1] / sx, r20 = worldMat.m[2] / sx;
        const float r01 = worldMat.m[4] / sy, r11 = worldMat.m[5] / sy, r21 = worldMat.m[6] / sy;
        const float r02 = worldMat.m[8] / sz, r12 = worldMat.m[9] / sz, r22 = worldMat.m[10] / sz;
        const float trace = r00 + r11 + r22;
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            local.rotation = { (r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s };
        } else if (r00 > r11 && r00 > r22) {
            const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
            local.rotation = { 0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s };
        } else if (r11 > r22) {
            const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
            local.rotation = { (r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s };
        } else {
            const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
            local.rotation = { (r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s };
        }
        local.rotation = normalizeQuat(local.rotation);
    } else {
        local.rotation = quatIdentity();
    }
}

// ---------------------------------------------------------------------------
// TransformComponent：场景 roundtrip（M-E2）
// 数组序固定 [x,y,z] / [x,y,z,w]；float -> json double -> float 无损
// （float 值在 double 中精确表示）。缺键 = nlohmann 异常（World::deserialise
// 统一 catch 转显式报错）。
// ---------------------------------------------------------------------------

nlohmann::json TransformComponent::serialise() const {
    nlohmann::json j;
    j["position"] = {local.position.x, local.position.y, local.position.z};
    j["scale"]    = {local.scale.x, local.scale.y, local.scale.z};
    j["rotation"] = {local.rotation.x, local.rotation.y, local.rotation.z, local.rotation.w};
    return j;
}

void TransformComponent::deserialise(const nlohmann::json& j) {
    local.position = {j.at("position")[0], j.at("position")[1], j.at("position")[2]};
    local.scale    = {j.at("scale")[0], j.at("scale")[1], j.at("scale")[2]};
    local.rotation = {j.at("rotation")[0], j.at("rotation")[1], j.at("rotation")[2], j.at("rotation")[3]};
}

// ---------------------------------------------------------------------------
// ModelComponent / LightComponent：场景 roundtrip（M-E1b.0）
// 数组序固定；float -> json double -> float 无损（TransformComponent 同款）。
// ---------------------------------------------------------------------------

nlohmann::json ModelComponent::serialise() const {
    nlohmann::json j;
    j["mesh"] = meshFile;
    j["tint"] = {tint.x, tint.y, tint.z};
    return j;
}

void ModelComponent::deserialise(const nlohmann::json& j) {
    meshFile = j.at("mesh").get<std::string>();
    tint = {j.at("tint")[0], j.at("tint")[1], j.at("tint")[2]};
    // M-E1b.3：逐实例 PBR 材质缺键按默认（roughness=1 / metallic=0），
    // 与 LightComponent type=0 不写新键同套向后兼容铁律；序列化不写这两键，
    // 保住 M-E2 roundtrip byte-identical 硬判据（见 ecs-e1a/run.bat）。
    roughness = j.value("roughness", 1.0f);
    metallic  = j.value("metallic", 0.0f);
}

nlohmann::json LightComponent::serialise() const {
    nlohmann::json j;
    j["direction"] = {direction.x, direction.y, direction.z};
    j["color"]     = {color.x, color.y, color.z};
    j["intensity"] = intensity;
    // M-E1b.2：方向灯（type=0）不写新键 —— 既有 M-E1b.0/1 场景 JSON 与
    // ecs-serial roundtrip 逐字节判据零变化；点光才带 type/position/range。
    if (type != 0) {
        j["type"]     = type;
        j["position"] = {position.x, position.y, position.z};
        j["range"]    = range;
    }
    return j;
}

void LightComponent::deserialise(const nlohmann::json& j) {
    direction = {j.at("direction")[0], j.at("direction")[1], j.at("direction")[2]};
    color     = {j.at("color")[0], j.at("color")[1], j.at("color")[2]};
    intensity = j.at("intensity").get<float>();
    // 缺键 = 方向灯（向后兼容）。type 合法性与 range>0 由 viewer 收集段显式拒绝，
    // 不在组件层 assert（五路负向口径：不静默、不崩、返回退出码 2）。
    if (j.find("type") != j.end()) {
        type = j.at("type").get<int>();
        if (j.find("position") != j.end())
            position = {j["position"][0], j["position"][1], j["position"][2]};
        if (j.find("range") != j.end())
            range = j["range"].get<float>();
    }
}

} // namespace nl::scene
