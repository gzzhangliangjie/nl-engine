// M-E0+M-E1：World/Entity 核心 + 组件/层级实现。
// 口径逐条对标 Carrot engine/ecs/World.cpp（2026-11-09 精读留痕，见 World.hpp 文件头）。

#include "nl/scene/World.hpp"

#include "nl/scene/Component.hpp"

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
// Entity：层级 / 组件（全部委托 World）
// ---------------------------------------------------------------------------

Entity& Entity::setParent(std::optional<EntityID> parent) {
    assert(world && "Entity::setParent on null world");
    world->setParent(id, parent);
    return *this;
}

Entity& Entity::reparent(std::optional<EntityID> parent) {
    assert(world && "Entity::reparent on null world");
    world->reparent(id, parent);
    return *this;
}

std::optional<EntityID> Entity::getParentID() const {
    assert(world && "Entity::getParentID on null world");
    return world->getParentID(id);
}

std::optional<Entity> Entity::getParent() const {
    auto pid = getParentID();
    if (!pid) return std::nullopt;
    return world->wrap(*pid);
}

std::vector<Entity> Entity::getChildren() const {
    assert(world && "Entity::getChildren on null world");
    return world->getChildren(id);
}

Entity Entity::duplicate() const {
    assert(world && "Entity::duplicate on null world");
    return world->duplicate(id);
}

Component& Entity::addComponent(std::unique_ptr<Component> comp) {
    assert(world && "Entity::addComponent on null world");
    return world->addComponent(id, std::move(comp));
}

void Entity::removeComponent(ComponentTypeID typeID) {
    assert(world && "Entity::removeComponent on null world");
    world->removeComponent(id, typeID);
}

Component* Entity::getComponent(ComponentTypeID typeID) const {
    assert(world && "Entity::getComponent on null world");
    return world->getComponent(id, typeID);
}

bool Entity::hasComponent(ComponentTypeID typeID) const {
    assert(world && "Entity::hasComponent on null world");
    return world->hasComponent(id, typeID);
}

std::vector<Component*> Entity::getAllComponents() const {
    assert(world && "Entity::getAllComponents on null world");
    return world->getAllComponents(id);
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
    // M-E1 父删递归删子：立即把直接子全部递归压进待删队列（Carrot removeEntity
    // 口径——childrenCopy 后逐个 removeEntity）。flush 时统一落盘。
    auto it = children_.find(id);
    if (it != children_.end()) {
        for (EntityID child : it->second) {
            removeEntity(child);
        }
    }
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
            // M-E1：组件条目清理
            components_.erase(id);
            // 层级双结构双侧清理：从父的孩子列表摘除自己 + 摘掉父记录
            auto pit = parents_.find(id);
            if (pit != parents_.end()) {
                auto cit = children_.find(pit->second);
                if (cit != children_.end()) {
                    auto& siblings = cit->second;
                    siblings.erase(std::remove(siblings.begin(), siblings.end(), id),
                                   siblings.end());
                    if (siblings.empty()) children_.erase(cit);
                }
                parents_.erase(pit);
            }
            // 自己的孩子列表（正常情况下父删递归时子也在队列里，条目一并清；
            // 单独防重复排队/悬空条目）
            children_.erase(id);
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

// ---------------------------------------------------------------------------
// World：层级（M-E1，Carrot World::setParent/reparent/duplicate 口径）
// ---------------------------------------------------------------------------

void World::setParent(EntityID id, std::optional<EntityID> parent) {
    assert(id != kInvalidEntity && "setParent on invalid entity");
    if (parent) {
        assert(*parent != kInvalidEntity && "setParent to invalid parent");
        assert(*parent != id && "entity cannot parent itself");
        // 环防护：沿新父向上走，碰到自己 = 成环（Carrot 不防，引擎路径必须防）。
        EntityID cursor = *parent;
        while (cursor != kInvalidEntity) {
            assert(cursor != id && "setParent would create a cycle");
            auto pit = parents_.find(cursor);
            cursor = (pit != parents_.end()) ? pit->second : kInvalidEntity;
        }
    }

    // 摘旧：从旧父的孩子列表移除（Carrot setParent 前半段口径）
    auto oldIt = parents_.find(id);
    if (oldIt != parents_.end()) {
        auto cit = children_.find(oldIt->second);
        if (cit != children_.end()) {
            auto& siblings = cit->second;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), id),
                           siblings.end());
            if (siblings.empty()) children_.erase(cit);
        }
        parents_.erase(oldIt);
    }

    // 挂新
    if (parent) {
        parents_[id] = *parent;
        children_[*parent].push_back(id);
    }
}

void World::reparent(EntityID id, std::optional<EntityID> parent) {
    // 有 Transform 组件：先取世界矩阵 -> setParent -> 新父系下写回局部
    // （Carrot reparent 的 TransformComponent 特判口径；统一走矩阵路径，
    // 保 reparent 前后世界矩阵不变——M-E1 硬判据）。
    if (TransformComponent* tc = getComponent<TransformComponent>(id)) {
        Mat4 worldMat = tc->getWorldMatrix();
        setParent(id, parent);
        tc->setWorldFromMatrix(worldMat);
    } else {
        setParent(id, parent);
    }
}

std::optional<EntityID> World::getParentID(EntityID id) const {
    auto it = parents_.find(id);
    if (it != parents_.end()) return it->second;
    return std::nullopt;
}

std::vector<EntityID> World::getChildIDs(EntityID id) const {
    auto it = children_.find(id);
    return it != children_.end() ? it->second : std::vector<EntityID>{};
}

std::vector<Entity> World::getChildren(EntityID id) const {
    std::vector<Entity> list;
    auto it = children_.find(id);
    if (it != children_.end()) {
        list.reserve(it->second.size());
        for (EntityID child : it->second) {
            list.push_back(Entity{child, const_cast<World*>(this)});
        }
    }
    return list;
}

// ---------------------------------------------------------------------------
// World：duplicate（Carrot duplicateWithRemap 口径）
// ---------------------------------------------------------------------------

Entity World::duplicateWithRemap(EntityID src, std::optional<EntityID> newParent,
                                 std::unordered_map<EntityID, EntityID>& remap) {
    // 名字 + " (Copy)"（Carrot 口径）；ID 递增不复用 remap（Carrot 那里是
    // UUID 未赋值 bug，我们直接递增，语义更直白）。
    Entity clone = newEntity(getName(src) + " (Copy)");
    remap[src] = clone.getID();

    // 组件逐份深拷（duplicate 虚函数 = 换 owner 的数据拷贝）
    for (Component* comp : getAllComponents(src)) {
        clone.addComponent(comp->duplicate(clone));
    }

    // 子孙递归（先深拷子树再挂父，父关系最后统一落——Carrot 顺序）
    auto srcChildren = getChildIDs(src);
    for (EntityID child : srcChildren) {
        duplicateWithRemap(child, clone.getID(), remap);
    }

    // 挂父：显式 newParent 优先；否则继承源的父（Carrot 口径）
    setParent(clone.getID(), newParent ? newParent : getParentID(src));
    return clone;
}

Entity World::duplicate(EntityID src) {
    assert(exists(src) && "duplicate on non-existent entity");
    std::unordered_map<EntityID, EntityID> remap;
    return duplicateWithRemap(src, std::nullopt, remap);
}

// ---------------------------------------------------------------------------
// World：组件（M-E1，增删即时——Carrot addComponent/removeComponent 口径）
// ---------------------------------------------------------------------------

Component& World::addComponent(EntityID id, std::unique_ptr<Component> comp) {
    assert(comp && "addComponent with null component");
    assert(exists(id) && "addComponent on non-existent entity");
    auto& slot = components_[id];
    auto [it, inserted] = slot.emplace(comp->getComponentTypeID(), std::move(comp));
    assert(inserted && "duplicate component type on one entity (Carrot 同款一类型一实例)");
    return *it->second;
}

void World::removeComponent(EntityID id, ComponentTypeID typeID) {
    auto it = components_.find(id);
    if (it != components_.end()) {
        it->second.erase(typeID);
        if (it->second.empty()) components_.erase(it);
    }
}

Component* World::getComponent(EntityID id, ComponentTypeID typeID) const {
    auto it = components_.find(id);
    if (it == components_.end()) return nullptr;
    auto cit = it->second.find(typeID);
    return cit != it->second.end() ? cit->second.get() : nullptr;
}

bool World::hasComponent(EntityID id, ComponentTypeID typeID) const {
    return getComponent(id, typeID) != nullptr;
}

std::vector<Component*> World::getAllComponents(EntityID id) const {
    std::vector<Component*> list;
    auto it = components_.find(id);
    if (it != components_.end()) {
        list.reserve(it->second.size());
        for (auto& [tid, comp] : it->second) {
            list.push_back(comp.get());
        }
    }
    return list;
}

World::~World() = default;

} // namespace nl::scene
