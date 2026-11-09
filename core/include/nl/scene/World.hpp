#pragma once

// M-E0+M-E1：World/Entity 核心 + 组件/层级（P8 E1a 拆分段，docs/p8/P8-ECS-PLAN.md）。
// Carrot 式 OOP 组件场景内核：实体注册表 + 延迟增删 + 名字 + Flags + 组件存储
// + 层级双结构（M-E1）。System 生命周期（M-E2）、JSON 序列化 roundtrip（M-E2）坐本层之上。
//
// 口径来源（三处对照，P8-ECS-PLAN.md「三处参考口径」+ 2026-11-09 精读留痕）：
//   - Carrot engine/ecs/World.{h,cpp}（节奏参照，逐条对标）：
//     延迟增删 entitiesToAdd/Remove + flushEntityCreationAndRemoval；
//     tick 开头先 flush（「tick 中删实体下一 tick 生效」延迟语义锚点）；
//     exists() = entityNames.contains —— 新建即时可见、删除 flush 后才不可见
//     （非对称，Carrot 口径照抄：新建立即可挂组件，删除必须等 flush 统一落盘）；
//     Hidden = 位标志不是删除（hide/show 递归版本是显式 opt-in）。
//     M-E1 层级：关系存 World 端双结构 entityParents/entityChildren；
//     setParent 摘旧挂新；removeEntity 立即递归压子孙进待删队列；
//     reparent = 取世界变换 -> setParent -> 写回局部（TransformComponent 特判）；
//     duplicateWithRemap = 名字 " (Copy)" -> 组件逐份 duplicate -> 子孙递归 -> 挂父。
//     M-E1 组件：增删即时（不延迟，Carrot addComponent/removeComponent 口径），
//     flush 删实体时清组件条目。
//   - bevy entity_disabling.rs（借语义不借结构）：Disabled 是标记组件不是删除，
//     实体留在 World、成员资格不动；只禁用实体自身不牵连子级。反向印证别把
//     「隐藏」做成「删除」，也别为百万实体做 archetype（50 物体规模锚）。
//   - UE Actor.h 挂接树口径 = Carrot 层级同构（attach/detach + 相对变换），
//     M-E1 以 Carrot 为锚（OOP 路线拍板）。
//   - EntityID = u64 递增（2026-11-09 拍板，非 Carrot UUID：无网络/多编辑器场景，
//     50 物体规模 u64 最简直接，序列化存数值）。
// 有意简化（P8-ECS-PLAN.md「砍掉的范围」留痕）：查询缓存/invalidateQueries
// （50 物体 O(N) 遍历足够）、ComponentReflection、Prefab、repairLinks（M-E2 后
// 按需）、systems 增删回调（M-E2 随 System 一起进）。
//
// 纪律：core 零图形 API（NVRHI/ImGui 零 include）；确定性 = selftest 数值锚。

#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace nl::scene {

using EntityID = std::uint64_t;

// 0 = 无效实体（World 的 ID 计数器从 1 起）。
inline constexpr EntityID kInvalidEntity = 0;

// 实体标志位（Carrot EntityFlags 口径：u64 位域，不是组件、不是删除）。
enum class EntityFlags : std::uint64_t {
    None   = 0,
    Hidden = 1u << 0,
};

inline EntityFlags operator&(EntityFlags a, EntityFlags b) {
    return static_cast<EntityFlags>(static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b));
}
inline EntityFlags operator|(EntityFlags a, EntityFlags b) {
    return static_cast<EntityFlags>(static_cast<std::uint64_t>(a) | static_cast<std::uint64_t>(b));
}
inline EntityFlags& operator|=(EntityFlags& self, EntityFlags b) {
    self = self | b;
    return self;
}
inline EntityFlags& operator&=(EntityFlags& self, EntityFlags b) {
    self = static_cast<EntityFlags>(static_cast<std::uint64_t>(self) & static_cast<std::uint64_t>(b));
    return self;
}
inline EntityFlags operator~(EntityFlags a) {
    return static_cast<EntityFlags>(~static_cast<std::uint64_t>(a));
}

class World;

// 组件类型 ID：0..63（Signature 位对应；静态递增分配，跨 TU 唯一）。
// 定义在 World.hpp（基础类型层），Component.hpp/系统层共用。
using ComponentTypeID = std::uint32_t;
inline constexpr std::uint32_t kMaxComponentTypes = 64;

// 签名 = 组件类型 ID 位集（基础类型层定义；System.hpp 用同款 using 重声明）。
using Signature = std::bitset<kMaxComponentTypes>;

class Component;
template <class T> class ComponentOf;
class System;
class ComponentLibrary;
class SystemLibrary;

// 组件类型 ID 的静态获取（World::getComponent<T> 内部用）。
template <class T>
ComponentTypeID componentTypeID() { return ComponentOf<T>::typeID(); }

// 轻量引用壳：{ID, World*}，不持组件不持存储（Carrot Entity 口径）。
// 组件存取接口 M-E1 随组件存储一起进。
struct Entity {
    EntityID id = kInvalidEntity;
    World* world = nullptr;

    // Carrot 口径：true iff world 当前拥有该实体。
    explicit operator bool() const;
    bool exists() const;

    EntityID getID() const { return id; }
    World& getWorld() const { return *world; }

    // 名字读写（缺失实体返回空串引用，不创建条目）。
    const std::string& getName() const;
    Entity& updateName(std::string_view name);

    // Flags（Hidden 语义见文件头；默认 None）。
    EntityFlags getFlags() const;
    Entity& setFlags(EntityFlags flags);
    Entity& removeFlags(EntityFlags flags);
    bool isVisible() const;   // (flags & Hidden) == 0

    // 延迟删除：排队 entitiesToRemove，下一次 tick（flush）生效。
    // M-E1：父删递归删子（Carrot removeEntity 立即递归压队列口径）。
    void remove();

    // ---- 层级（M-E1；关系存 World 端双结构，Carrot 口径） ----
    // 挂父（纯关系变更，不动变换）；空 = 解除父子关系。
    // 环防护：assert 新父不是自己的后代（Carrot 编辑器场景不防，引擎路径必须防）。
    Entity& setParent(std::optional<EntityID> parent);
    // 保世界变换挂父：有 Transform 组件时先取世界矩阵 -> setParent -> 写回局部。
    Entity& reparent(std::optional<EntityID> parent);
    std::optional<EntityID> getParentID() const;            // 无父 = nullopt
    std::optional<Entity> getParent() const;
    // 子列表（直接子；递归遍历子孙是显式 opt-in，Carrot ShouldRecurse 口径）。
    std::vector<Entity> getChildren() const;
    // 递归深拷：名字 + " (Copy)"，组件逐份 duplicate，子孙递归，父关系继承。
    Entity duplicate() const;

    // ---- 组件（M-E1；增删即时，Carrot 口径——组件不延迟） ----
    Component& addComponent(std::unique_ptr<Component> comp);   // 同类型重复 = assert
    void removeComponent(ComponentTypeID typeID);
    template <class T>
    T* getComponent() const {
        return static_cast<T*>(getComponent(componentTypeID<T>()));
    }
    Component* getComponent(ComponentTypeID typeID) const;
    bool hasComponent(ComponentTypeID typeID) const;
    std::vector<Component*> getAllComponents() const;           // 顺序不保证（无序表）

    bool operator==(const Entity& o) const { return id == o.id && world == o.world; }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

class World {
public:
    World();   // 工厂库 unique_ptr 构造（完整类型在 .cpp，头里只前向声明）
    ~World();   // unique_ptr<Component> 完整类型要求，实现在 World.cpp
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---- 创建 / 删除（延迟语义） ----
    // 新实体：ID 递增（nextID_ 从 1 起），名字表即时登记（exists 即时真），
    // 全局实体表等 flush 才收——Carrot newEntity/newEntityWithID 口径。
    Entity newEntity(std::string_view name = "<unnamed>");
    // 指定 ID 创建（M-E2 反序列化入口；ID 冲突 = 断言失败）。
    Entity newEntityWithID(EntityID id, std::string_view name = "<unnamed>");
    // 延迟删除：排队，下一次 tick/flush 生效（含名字/flags 条目清理）。
    // M-E1：父删递归删子——子孙立即全部压入待删队列（Carrot removeEntity 口径）。
    void removeEntity(EntityID id);
    // 统一落盘：先加后删（Carrot flushEntityCreationAndRemoval 顺序）；
    // 删时清名字/flags/组件/层级双结构（parents_/children_ 双侧都摘）。
    // M-E2：落盘后向系统推送 onEntitiesAdded/Removed（Carrot flush 推送点口径）。
    void flushEntityCreationAndRemoval();
    // tick：开头先 flush -> firstTick（注册后恰一次）-> 逐系统 tick(dt)
    // （Carrot World::tick:313-321 口径）。
    void tick(double dt);

    // ---- 层级（M-E1；双结构：parents_ 子->父 + children_ 父->子，Carrot 口径） ----
    void setParent(EntityID id, std::optional<EntityID> parent);  // 纯关系，不动变换
    void reparent(EntityID id, std::optional<EntityID> parent);   // 保世界变换
    std::optional<EntityID> getParentID(EntityID id) const;
    // 直接子列表（顺序 = 挂接序；Carrot entityChildren vector 同款）。
    std::vector<EntityID> getChildIDs(EntityID id) const;
    std::vector<Entity> getChildren(EntityID id) const;
    // 递归深拷（含子孙 + 组件 + 父关系；名字 " (Copy)"，Carrot 口径）。
    Entity duplicate(EntityID src);

    // ---- 组件（M-E1；增删即时） ----
    Component& addComponent(EntityID id, std::unique_ptr<Component> comp);
    void removeComponent(EntityID id, ComponentTypeID typeID);
    Component* getComponent(EntityID id, ComponentTypeID typeID) const;
    template <class T>
    T* getComponent(EntityID id) const {
        return static_cast<T*>(getComponent(id, componentTypeID<T>()));
    }
    bool hasComponent(EntityID id, ComponentTypeID typeID) const;
    std::vector<Component*> getAllComponents(EntityID id) const;

    // ---- 查询（O(N) 遍历，50 物体规模锚，无缓存） ----
    bool exists(EntityID id) const;             // = names_.contains（Carrot 口径）
    std::vector<Entity> getAllEntities() const; // flush 后的全局表
    std::vector<Entity> getEntitiesWithFlags(EntityFlags flags) const; // 位过滤
    // 实体签名 = 该实体全部组件类型位（现算，Carrot World::getSignature:460 口径）。
    Signature getSignature(EntityID id) const;

    // ---- 系统（M-E2；Carrot addLogicSystem/getLogicSystem 口径裁剪版） ----
    // 注册即收编：立即对现存实体推送 onEntitiesAdded，并排进 firstTick 等待列表。
    void registerSystem(std::unique_ptr<System> system);
    std::size_t getSystemCount() const { return systems_.size(); }
    // 按名查找（找不到 = nullptr，Carrot getLogicSystem 口径）。
    System* findSystem(std::string_view name);

    // ---- 工厂库（serialise/deserialise 的组件/系统名解析用） ----
    ComponentLibrary& getComponentLibrary() { return *componentLibrary_; }
    const ComponentLibrary& getComponentLibrary() const { return *componentLibrary_; }
    SystemLibrary& getSystemLibrary() { return *systemLibrary_; }
    const SystemLibrary& getSystemLibrary() const { return *systemLibrary_; }

    // ---- 场景 JSON 序列化（M-E2 判据：roundtrip 字节一致） ----
    // 格式（本项目拍板自定，字段口径借 Carrot Scene::serialise：id/name/flags/
    // 逐组件 serialise；层级 Carrot 用目录树表达，我们单文件用 parent 字段）：
    //   {"version":1,"nextID":N,
    //    "entities":[{"id":u64,"name":str,"flags":u64,"parent":u64(0=无父),
    //                 "components":{"<组件名>":{...}}}, ...按 id 升序],
    //    "systems":[{"name":str,"data":{...}}, ...按注册序]}
    // 确定性：实体数组按 id 升序、组件对象键由 nlohmann（std::map）排序、
    // 系统数组按注册序 -> serialise 输出可逐字节复现。
    nlohmann::json serialise() const;
    // 反序列化：非法结构 / 未知组件名 / 未知系统名 / 未知父引用 = 返回 false
    // + err 描述（显式报错不静默；调用方决定退出码语义）。失败时 world 状态清空。
    bool deserialise(const nlohmann::json& doc, std::string& err);

    // ---- 名字 ----
    const std::string& getName(EntityID id) const;
    void updateName(EntityID id, std::string_view name);
    std::optional<Entity> findEntityByName(std::string_view name) const;

    // M-E1b.0：flush 后全局实体表只读快照（serialise 同一数据源）。viewer 的
    // --scene 收集渲染描述走 O(N) 遍历（P8-ECS-PLAN 50 物体规模锚：不做查询缓存）。
    const std::vector<EntityID>& getEntityIDs() const { return entities_; }

    // ---- Flags ----
    EntityFlags getFlags(EntityID id) const;    // 缺失条目 = None
    void setFlags(EntityID id, EntityFlags flags);
    void removeFlags(EntityID id, EntityFlags flags);
    bool isVisible(EntityID id) const;

    Entity wrap(EntityID id) const { return Entity{id, const_cast<World*>(this)}; }

private:
    EntityID nextID_ = 1;
    std::vector<EntityID> entities_;          // flush 后的全局实体表
    std::vector<EntityID> entitiesToAdd_;     // 待落盘
    std::vector<EntityID> entitiesToRemove_;  // 待落盘
    std::unordered_map<EntityID, std::string> names_;
    std::unordered_map<EntityID, EntityFlags> flags_;

    // ---- M-E1：层级双结构（Carrot entityParents/entityChildren 口径） ----
    std::unordered_map<EntityID, EntityID> parents_;
    std::unordered_map<EntityID, std::vector<EntityID>> children_;

    // ---- M-E1：组件存储（类型 ID -> 组件；增删即时，flush 只做删除清理） ----
    std::unordered_map<EntityID, std::unordered_map<ComponentTypeID, std::unique_ptr<Component>>> components_;

    // ---- M-E2：系统列表（注册序）+ firstTick 等待列表（Carrot logicSystems /
    // logicSystemsWaitingForFirstTick 口径；Logic/Render 双类目裁剪成单列表） ----
    std::vector<std::unique_ptr<System>> systems_;
    std::vector<System*> waitingForFirstTick_;

    // ---- M-E2：工厂库（unique_ptr 持有避免 World.hpp include Component.hpp
    // 造成环——Component.hpp/System.hpp 都 include 本头） ----
    std::unique_ptr<ComponentLibrary> componentLibrary_;   // World.cpp 构造
    std::unique_ptr<SystemLibrary> systemLibrary_;

    // duplicate 的递归内核（remap 表 M-E2 修链接时复用）。
    Entity duplicateWithRemap(EntityID src, std::optional<EntityID> newParent,
                              std::unordered_map<EntityID, EntityID>& remap);
};

} // namespace nl::scene
