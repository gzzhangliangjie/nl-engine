// M-E0+M-E1+M-E2：World/Entity 核心 + 组件/层级 + 系统/序列化实现。
// 口径逐条对标 Carrot engine/ecs/World.cpp（2026-11-09 精读留痕，见 World.hpp 文件头）。

#include "nl/scene/World.hpp"

#include "nl/core/jobgraph.hpp"
#include "nl/scene/Component.hpp"
#include "nl/scene/System.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

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
    guardNoStructuralMutation("newEntity");
    assert(id != kInvalidEntity && "entity id must be non-zero");
    assert(names_.find(id) == names_.end() && "duplicate entity id");
    names_.emplace(id, std::string(name));
    entitiesToAdd_.push_back(id);
    return Entity{id, this};
}

void World::removeEntity(EntityID id) {
    guardNoStructuralMutation("removeEntity");
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
    std::vector<EntityID> added;
    std::vector<EntityID> removed;
    if (!entitiesToAdd_.empty()) {
        entities_.insert(entities_.end(), entitiesToAdd_.begin(), entitiesToAdd_.end());
        added.swap(entitiesToAdd_);
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
        removed.swap(entitiesToRemove_);
        entitiesToRemove_.clear();
    }
    // M-E2：落盘后向系统推送（Carrot World.cpp:268/277 口径——flush 内统一推，
    // 不在增删单点上推）。firstTick 等待中的系统也收（Carrot addSystem 先收编）。
    if (!added.empty()) {
        for (auto& s : systems_) s->onEntitiesAdded(added);
    }
    if (!removed.empty()) {
        for (auto& s : systems_) s->onEntitiesRemoved(removed);
    }
}

void World::tick(double dt) {
    flushEntityCreationAndRemoval();
    // M-E2 系统循环（Carrot World::tick:313-321 口径裁剪版——Logic/Render 双
    // 类目合并单列表）：firstTick 注册后首个 tick 恰一次，然后逐系统 tick。
    for (System* s : waitingForFirstTick_) {
        s->firstTick();
    }
    waitingForFirstTick_.clear();
    for (auto& s : systems_) {
        s->tick(dt);
    }
}

// ---------------------------------------------------------------------------
// World：并行 tick（M-E1b.5，读写集求解 + jobgraph 静态 DAG）
// ---------------------------------------------------------------------------

void World::guardNoStructuralMutation(const char* api) const {
    if (!inParallelTick_) return;
    // 先记账再抛：jobgraph 在封装层捕获异常，原始消息带不到 err——
    // tickParallel 失败时从 violationMsg_ 取回具体违约 API（加锁防并行写撕）。
    {
        std::lock_guard<std::mutex> lock(violationMutex_);
        violationMsg_ = std::string("structural change forbidden during tickParallel: ")
                        + api;
    }
    throw ParallelTickViolation(violationMsg_);
}

bool World::tickParallel(double dt, unsigned threadCount, std::string& err,
                         std::string* dotOut) {
    err.clear();
    violationMsg_.clear();
    flushEntityCreationAndRemoval();
    // firstTick 串行（Carrot firstTick 无并行语义；首个 tick 建内部状态）。
    for (System* s : waitingForFirstTick_) {
        s->firstTick();
    }
    waitingForFirstTick_.clear();

    // 求解（UE Mass FMassProcessorDependencySolver 口径裁剪版）：
    // 冲突 = 两系统的写集与对方读写集相交（写读 / 写写；读读不冲突——
    // bevy access.is_compatible 同判据）。边方向 = 注册序（i<j 时 i 先于 j），
    // 构造上无环，jobgraph 的 Kahn 拒环只是兜底闸。
    nl::core::jobgraph::Graph graph;
    for (auto& s : systems_) {
        // 捕获裸指针：systems_ 持 unique_ptr 且并行段结构变更已封死，不悬垂。
        System* raw = s.get();
        graph.add(s->getName(), [raw, dt]() { raw->tick(dt); });
    }
    for (std::size_t i = 0; i < systems_.size(); ++i) {
        for (std::size_t j = i + 1; j < systems_.size(); ++j) {
            const Signature& wi = systems_[i]->getWrites();
            const Signature& ri = systems_[i]->getReads();
            const Signature& wj = systems_[j]->getWrites();
            const Signature& rj = systems_[j]->getReads();
            const bool conflict = (wi & rj).any() || (wi & wj).any() ||
                                  (wj & ri).any();
            if (conflict) graph.precede(i, j);
        }
    }
    if (dotOut) *dotOut = graph.dumpDot();

    inParallelTick_ = true;
    const bool ok = graph.run(threadCount, &err);
    inParallelTick_ = false;
    if (!ok) {
        // 违约消息比 jobgraph 的「exception in task i」更可定位，优先带出。
        std::lock_guard<std::mutex> lock(violationMutex_);
        if (!violationMsg_.empty()) err = violationMsg_;
        err = "parallel tick failed: " + err;
    }
    return ok;
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
    guardNoStructuralMutation("setParent");
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
    guardNoStructuralMutation("addComponent");
    assert(comp && "addComponent with null component");
    assert(exists(id) && "addComponent on non-existent entity");
    auto& slot = components_[id];
    auto [it, inserted] = slot.emplace(comp->getComponentTypeID(), std::move(comp));
    assert(inserted && "duplicate component type on one entity (Carrot 同款一类型一实例)");
    // M-E2：签名变化即时推送（Carrot addComponent -> onEntitiesUpdated 口径；
    // Carrot 走 entitiesUpdated 队列 flush 推送，组件增删本身即时，我们即时推，
    // 语义一致且省一队列——留痕）。
    for (auto& s : systems_) s->onEntitiesUpdated({id});
    return *it->second;
}

void World::removeComponent(EntityID id, ComponentTypeID typeID) {
    guardNoStructuralMutation("removeComponent");
    auto it = components_.find(id);
    if (it != components_.end()) {
        it->second.erase(typeID);
        if (it->second.empty()) components_.erase(it);
        for (auto& s : systems_) s->onEntitiesUpdated({id});
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

World::World() {
    componentLibrary_ = std::make_unique<ComponentLibrary>();
    systemLibrary_ = std::make_unique<SystemLibrary>();
}

World::~World() = default;

// ---------------------------------------------------------------------------
// World：系统 / 签名（M-E2，Carrot addLogicSystem/getLogicSystem/getSignature 口径）
// ---------------------------------------------------------------------------

Signature World::getSignature(EntityID id) const {
    Signature sig;
    auto it = components_.find(id);
    if (it != components_.end()) {
        for (auto& [tid, comp] : it->second) {
            sig.set(tid);
        }
    }
    return sig;
}

void World::registerSystem(std::unique_ptr<System> system) {
    guardNoStructuralMutation("registerSystem");
    assert(system && "registerSystem with null system");
    // 注册即收编：对现存实体表全量推送（Carrot addLogicSystem:838 口径），
    // 并排进 firstTick 等待列表（首个 tick 恰一次）。
    std::vector<EntityID> all = entities_;
    system->onEntitiesAdded(all);
    systems_.push_back(std::move(system));
    waitingForFirstTick_.push_back(systems_.back().get());
}

System* World::findSystem(std::string_view name) {
    for (auto& s : systems_) {
        if (name == s->getName()) return s.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// World：场景 JSON 序列化（M-E2；格式口径见 World.hpp serialise 注释）
// ---------------------------------------------------------------------------

nlohmann::json World::serialise() const {
    nlohmann::json doc;
    doc["version"] = 1;
    doc["nextID"] = nextID_;

    // 实体数组按 id 升序（确定性；序列化的是 flush 后世界，待加/待删队列不进）。
    std::vector<EntityID> sorted = entities_;
    std::sort(sorted.begin(), sorted.end());
    nlohmann::json ents = nlohmann::json::array();
    for (EntityID id : sorted) {
        nlohmann::json e;
        e["id"] = id;
        e["name"] = getName(id);
        e["flags"] = static_cast<std::uint64_t>(getFlags(id));
        auto pid = getParentID(id);
        e["parent"] = pid ? *pid : EntityID{0};
        nlohmann::json comps = nlohmann::json::object();
        for (Component* c : getAllComponents(id)) {
            comps[c->getName()] = c->serialise();   // 键序由 nlohmann(std::map) 决定
        }
        e["components"] = comps;
        ents.push_back(e);
    }
    doc["entities"] = ents;

    nlohmann::json syss = nlohmann::json::array();
    for (auto& s : systems_) {
        nlohmann::json sj;
        sj["name"] = s->getName();
        sj["data"] = s->serialise();
        syss.push_back(sj);
    }
    doc["systems"] = syss;
    return doc;
}

bool World::deserialise(const nlohmann::json& doc, std::string& err) {
    auto fail = [&err](const std::string& msg) { err = msg; return false; };

    // ---- 结构校验 + 清空重建（工厂库保留；实体/组件/层级/系统全清） ----
    if (!doc.is_object() || !doc.contains("version") || doc["version"] != 1) {
        return fail("scene json: unsupported or missing \"version\" (expect 1)");
    }
    if (!doc.contains("nextID") || !doc["nextID"].is_number_unsigned()) {
        return fail("scene json: missing \"nextID\"");
    }
    entities_.clear();
    entitiesToAdd_.clear();
    entitiesToRemove_.clear();
    names_.clear();
    flags_.clear();
    parents_.clear();
    children_.clear();
    components_.clear();
    systems_.clear();
    waitingForFirstTick_.clear();

    try {
        const auto& ents = doc.at("entities");
        if (!ents.is_array()) return fail("scene json: \"entities\" must be an array");

        // 第一轮：实体壳（名字/flags/ID 校验显式报错，不 assert——负向判据）
        for (const auto& e : ents) {
            EntityID id = e.at("id").get<EntityID>();
            if (id == kInvalidEntity) return fail("scene json: entity id 0 is invalid");
            if (names_.find(id) != names_.end()) {
                std::ostringstream oss;
                oss << "scene json: duplicate entity id " << id;
                return fail(oss.str());
            }
            newEntityWithID(id, e.value("name", "<unnamed>"));
            if (e.contains("flags")) {
                flags_[id] = static_cast<EntityFlags>(e["flags"].get<std::uint64_t>());
            }
        }
        nextID_ = doc["nextID"].get<EntityID>();
        for (const auto& e : ents) {
            EntityID id = e.at("id").get<EntityID>();
            if (id >= nextID_) nextID_ = id + 1;
        }

        // 第二轮：挂父 + 组件（父引用/组件名显式报错）
        for (const auto& e : ents) {
            EntityID id = e.at("id").get<EntityID>();
            EntityID parent = e.value("parent", EntityID{0});
            if (parent != kInvalidEntity) {
                if (names_.find(parent) == names_.end()) {
                    std::ostringstream oss;
                    oss << "scene json: entity " << id << " references unknown parent " << parent;
                    return fail(oss.str());
                }
                setParent(id, parent);
            }
            if (e.contains("components")) {
                for (auto& [name, cj] : e.at("components").items()) {
                    auto comp = componentLibrary_->create(name, wrap(id));
                    if (!comp) {
                        std::ostringstream oss;
                        oss << "scene json: unknown component name \"" << name << "\"";
                        return fail(oss.str());
                    }
                    comp->deserialise(cj);
                    addComponent(id, std::move(comp));
                }
            }
        }

        // 壳落表（系统收编需要 flush 后实体表；注册系统随后全量收编）。
        flushEntityCreationAndRemoval();

        // 第三轮：系统（名字显式报错；注册序 = 数组序）
        if (doc.contains("systems")) {
            for (const auto& sj : doc.at("systems")) {
                auto sys = systemLibrary_->create(sj.at("name").get<std::string>(), *this);
                if (!sys) {
                    std::ostringstream oss;
                    oss << "scene json: unknown system name \""
                        << sj.at("name").get<std::string>() << "\"";
                    return fail(oss.str());
                }
                sys->deserialise(sj.at("data"));
                registerSystem(std::move(sys));
            }
        }
    } catch (const nlohmann::json::exception& ex) {
        // 非法结构（缺键/类型错）显式报错（M-E2 负向判据：不静默）。
        return fail(std::string("scene json: malformed document: ") + ex.what());
    }
    err.clear();
    return true;
}

} // namespace nl::scene
