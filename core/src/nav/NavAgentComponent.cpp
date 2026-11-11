// P8 M-N6：NavAgentComponent 序列化（M-E2 场景 roundtrip 同款纪律：
// float -> json double -> float 无损；缺键由 World::deserialise 统一 catch
// 转显式报错，不静默造出半个组件）。

#include "nl/scene/NavAgentComponent.hpp"

namespace nl::scene {
namespace {

nlohmann::json vec3(const float v[3]) {
    return nlohmann::json::array({v[0], v[1], v[2]});
}

void readVec3(const nlohmann::json& j, const char* key, float out[3]) {
    const auto a = j.at(key);
    for (int k = 0; k < 3; ++k) out[k] = a.at(k).get<float>();
}

} // namespace

nlohmann::json NavAgentComponent::serialise() const {
    nlohmann::json j;
    j["spawn"]  = vec3(spawn);
    j["target"] = vec3(target);
    j["pos"]    = vec3(pos);
    j["radius"]                = radius;
    j["height"]                = height;
    j["maxAcceleration"]       = maxAcceleration;
    j["maxSpeed"]              = maxSpeed;
    j["collisionQueryRange"]   = collisionQueryRange;
    j["pathOptimizationRange"] = pathOptimizationRange;
    j["separationWeight"]      = separationWeight;
    j["updateFlags"]           = updateFlags;
    j["obstacleAvoidanceType"] = obstacleAvoidanceType;
    j["queryFilterType"]       = queryFilterType;
    j["crowdIndex"]            = crowdIndex;
    j["enabled"]               = enabled;
    return j;
}

void NavAgentComponent::deserialise(const nlohmann::json& j) {
    readVec3(j, "spawn", spawn);
    readVec3(j, "target", target);
    readVec3(j, "pos", pos);
    radius                = j.at("radius").get<float>();
    height                = j.at("height").get<float>();
    maxAcceleration       = j.at("maxAcceleration").get<float>();
    maxSpeed              = j.at("maxSpeed").get<float>();
    collisionQueryRange   = j.at("collisionQueryRange").get<float>();
    pathOptimizationRange = j.at("pathOptimizationRange").get<float>();
    separationWeight      = j.at("separationWeight").get<float>();
    updateFlags           = j.at("updateFlags").get<unsigned>();
    obstacleAvoidanceType = j.at("obstacleAvoidanceType").get<unsigned char>();
    queryFilterType       = j.at("queryFilterType").get<unsigned char>();
    crowdIndex            = j.at("crowdIndex").get<int>();
    enabled               = j.at("enabled").get<bool>();
}

} // namespace nl::scene
