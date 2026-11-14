// nl/scene/SignalLampComponent.cpp — P8 M-T4：序列化（TrafficCarComponent
// 同款：数组序固定、float/int <-> double 无损、缺键 = nlohmann 异常统一 catch）。

#include "nl/scene/SignalLampComponent.hpp"

namespace nl::scene {

nlohmann::json SignalLampComponent::serialise() const {
    return nlohmann::json{
        {"nodeId", nodeId},
        {"edgeId", edgeId},
    };
}

void SignalLampComponent::deserialise(const nlohmann::json& j) {
    nodeId = j.at("nodeId").get<std::int32_t>();
    edgeId = j.at("edgeId").get<std::int32_t>();
}

} // namespace nl::scene
