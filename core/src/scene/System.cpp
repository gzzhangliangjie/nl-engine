// M-E2：System 实体列表维护实现（Carrot systems/System.cpp:515-575 逐条对标）。

#include "nl/scene/System.hpp"

#include <algorithm>

namespace nl::scene {

bool System::matches(EntityID id) const {
    // (entitySig & systemSig) == systemSig：实体具备系统要求的全部组件类型。
    const Signature entitySig = world_.getSignature(id);
    return (entitySig & signature_) == signature_;
}

void System::onEntitiesAdded(const std::vector<EntityID>& added) {
    for (EntityID id : added) {
        // 已在列表跳过（重复推送无害，Carrot find_if 去重口径）
        auto it = std::find_if(entities_.begin(), entities_.end(),
                               [id](const Entity& e) { return e.getID() == id; });
        if (it != entities_.end()) continue;
        if (matches(id)) {
            entities_.push_back(world_.wrap(id));
        }
    }
}

void System::onEntitiesRemoved(const std::vector<EntityID>& removed) {
    for (EntityID id : removed) {
        entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                       [id](const Entity& e) { return e.getID() == id; }),
                        entities_.end());
    }
}

void System::onEntitiesUpdated(const std::vector<EntityID>& updated) {
    for (EntityID id : updated) {
        auto it = std::find_if(entities_.begin(), entities_.end(),
                               [id](const Entity& e) { return e.getID() == id; });
        const bool inList = it != entities_.end();
        const bool matched = matches(id);
        if (matched && !inList) {
            entities_.push_back(world_.wrap(id));
        } else if (!matched && inList) {
            entities_.erase(it);
        }
    }
}

} // namespace nl::scene
