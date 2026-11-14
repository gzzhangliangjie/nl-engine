// P8 M-T3：TrafficCarComponent 序列化（M-E2 场景 roundtrip 同款纪律：
// float -> json double -> float 无损；缺键由 World::deserialise 统一 catch
// 转显式报错，不静默造出半个组件）。NavAgentComponent.cpp 同款骨架。

#include "nl/scene/TrafficCarComponent.hpp"

namespace nl::scene {

nlohmann::json TrafficCarComponent::serialise() const {
    nlohmann::json j;
    j["route"]   = route;    // int32 数组直通（nlohmann 原生支持）
    j["s"]       = s;
    j["speed"]   = speed;
    j["variant"] = variant;
    j["enabled"] = enabled;
    return j;
}

void TrafficCarComponent::deserialise(const nlohmann::json& j) {
    route   = j.at("route").get<std::vector<std::int32_t>>();
    s       = j.at("s").get<float>();
    speed   = j.at("speed").get<float>();
    variant = j.at("variant").get<std::uint32_t>();
    enabled = j.at("enabled").get<bool>();
}

} // namespace nl::scene
