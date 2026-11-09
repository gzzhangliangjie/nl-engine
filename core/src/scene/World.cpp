// M-E0：World/Entity 核心实现（P8 E1a 拆分段，docs/p8/P8-ECS-PLAN.md）。
// 口径逐条对标 Carrot engine/ecs/World.cpp（2026-11-09 精读留痕，见 World.hpp 文件头）。

#include "nl/scene/World.hpp"

#include <algorithm>
#include <cassert>

namespace nl::scene {

// ---------------------------------------------------------------------------
// Entity（轻量引用壳，全部委托 World）
// ---------------------------------------------------------------------------

bool Entity::exists() const {
    return world && world->exists(id);
}

Entity::operator bool() const {
    return exists();
}

const std::string& Entity::getName() const {
    assert(world && "Entity::getName on null world");
    return world->getName(id);
}

Entity& Entity::updateName(std::string_view name) {
    assert(world && "Entity::updateName on null world");
    world->updateName(id, name);
    return *this;
}

EntityFlags Entity::getFlags() const {
    assert(world && "Entity::getFlags on null world");
    return world->getFlags(id);
}

Entity& Entity::setFlags(EntityFlags flags) {
    assert(world && "Entity::setFlags on null world");
    world->setFlags(id, flags);
    return *this;
}

Entity& Entity::removeFlags(EntityFlags flags) {
    assert(world && "Entity::removeFlags on null world");
    world->removeFlags(id, flags);
    return *this;
}

bool Entity::isVisible() const {
    return (getFlags() & EntityFlags::Hidden) == EntityFlags::None;
}

void Entity::remove() {
    assert(world && "Entity::remove on null world");
    world->removeEntity(id);
}

// ---------------------------------------------------------------------------
// World：创建 / 删除（延迟）
// ---------------------------------------------------------------------------

Entity World::newEntity(std::string_view name) {
    return newEntityWithID(nextID_++, name);
}

Entity World::newEntityWithID(EntityID id, std::string_view name) {
    assert(id != kInvalidEntity && "entity id must be non-zero");
    assert(names_.find(id) == names_.end() && "duplicate entity id");
    names_.emplace(id, std::string(name));
    entitiesToAdd_.push_back(id);
    return Entity{id, this};
}

void World::removeEntity(EntityID id) {
    entitiesToRemove_.push_back(id);
    // M-E0 无层级；M-E1 落地后此处递归排队子实体（Carrot removeEntity 口径）。
}

void World::flushEntityCreationAndRemoval() {
    // 先加（Carrot flush 顺序：add 落表 -> remove 清理）
    if (!entitiesToAdd_.empty()) {
        entities_.insert(entities_.end(), entitiesToAdd_.begin(), entitiesToAdd_.end());
        entitiesToAdd_.clear();
    }
    if (!entitiesToRemove_.empty()) {
        for (EntityID id : entitiesToRemove_) {
            // 重复排队无害：remove-erase 全清，map erase 幂等（Carrot 同口径）
            entities_.erase(std::remove(entities_.begin(), entities_.end(), id),
                            entities_.end());
            names_.erase(id);
            flags_.erase(id);
            // M-E1：组件表/层级表清理挂这里。
        }
        entitiesToRemove_.clear();
    }
}

void World::tick(double dt) {
    (void)dt; // M-E2：dt 传给 system tick。
    flushEntityCreationAndRemoval();
    // M-E2：system firstTick / tick 插在这里（flush 之后、收尾之前）。
}

// ---------------------------------------------------------------------------
// World：查询（O(N)，无缓存）
// ---------------------------------------------------------------------------

bool World::exists(EntityID id) const {
    return names_.find(id) != names_.end();
}

std::vector<Entity> World::getAllEntities() const {
    std::vector<Entity> list;
    list.reserve(entities_.size());
    for (EntityID id : entities_) {
        list.push_back(Entity{id, const_cast<World*>(this)});
    }
    return list;
}

std::vector<Entity> World::getEntitiesWithFlags(EntityFlags flags) const {
    std::vector<Entity> list;
    list.reserve(entities_.size());
    for (EntityID id : entities_) {
        auto it = flags_.find(id);
        EntityFlags f = (it != flags_.end()) ? it->second : EntityFlags::None;
        if ((f & flags) == flags) {
            list.push_back(Entity{id, const_cast<World*>(this)});
        }
    }
    return list;
}

// ---------------------------------------------------------------------------
// World：名字
// ---------------------------------------------------------------------------

const std::string& World::getName(EntityID id) const {
    static const std::string kMissing;
    auto it = names_.find(id);
    return it != names_.end() ? it->second : kMissing;
}

void World::updateName(EntityID id, std::string_view name) {
    auto it = names_.find(id);
    assert(it != names_.end() && "updateName on non-existent entity");
    if (it != names_.end()) {
        it->second.assign(name);
    }
}

std::optional<Entity> World::findEntityByName(std::string_view name) const {
    for (EntityID id : entities_) {
        auto it = names_.find(id);
        if (it != names_.end() && it->second == name) {
            return Entity{id, const_cast<World*>(this)};
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// World：Flags
// ---------------------------------------------------------------------------

EntityFlags World::getFlags(EntityID id) const {
    auto it = flags_.find(id);
    return it != flags_.end() ? it->second : EntityFlags::None;
}

void World::setFlags(EntityID id, EntityFlags flags) {
    flags_[id] |= flags;
}

void World::removeFlags(EntityID id, EntityFlags flags) {
    flags_[id] &= ~flags;
}

bool World::isVisible(EntityID id) const {
    return (getFlags(id) & EntityFlags::Hidden) == EntityFlags::None;
}

} // namespace nl::scene
