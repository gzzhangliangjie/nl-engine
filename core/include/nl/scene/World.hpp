#pragma once

// M-E0：World/Entity 核心（P8 E1a 拆分段，docs/p8/P8-ECS-PLAN.md）。
// Carrot 式 OOP 组件场景内核第一段：实体注册表 + 延迟增删 + 名字 + Flags。
// 组件存储/层级（M-E1）、System 生命周期（M-E2）、JSON 序列化（M-E2）坐本层之上。
//
// 口径来源（三处对照，P8-ECS-PLAN.md「三处参考口径」+ 2026-11-09 本轮精读留痕）：
//   - Carrot engine/ecs/World.{h,cpp}（节奏参照，逐条对标）：
//     延迟增删 entitiesToAdd/Remove + flushEntityCreationAndRemoval；
//     tick 开头先 flush（「tick 中删实体下一 tick 生效」延迟语义锚点）；
//     exists() = entityNames.contains —— 新建即时可见、删除 flush 后才不可见
//     （非对称，Carrot 口径照抄：新建立即可挂组件，删除必须等 flush 统一落盘）；
//     Hidden = 位标志不是删除（hide/show 递归版本是显式 opt-in，M-E1 层级落地后补）。
//   - bevy entity_disabling.rs（借语义不借结构）：Disabled 是标记组件不是删除，
//     实体留在 World、成员资格不动；只禁用实体自身不牵连子级。反向印证别把
//     「隐藏」做成「删除」，也别为百万实体做 archetype（50 物体规模锚）。
//   - UE Actor.h 挂接树口径属 M-E1 层级范围，M-E0 不涉。
//   - EntityID = u64 递增（2026-11-09 拍板，非 Carrot UUID：无网络/多编辑器场景，
//     50 物体规模 u64 最简直接，序列化存数值）。
// 有意简化（P8-ECS-PLAN.md「砍掉的范围」留痕）：查询缓存/invalidateQueries
// （50 物体 O(N) 遍历足够）、ComponentReflection、Prefab、repairLinks、
// systems 增删回调（M-E2 随 System 一起进）。
//
// 纪律：core 零图形 API（NVRHI/ImGui 零 include）；确定性 = selftest 数值锚。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
    void remove();

    bool operator==(const Entity& o) const { return id == o.id && world == o.world; }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

class World {
public:
    // ---- 创建 / 删除（延迟语义） ----
    // 新实体：ID 递增（nextID_ 从 1 起），名字表即时登记（exists 即时真），
    // 全局实体表等 flush 才收——Carrot newEntity/newEntityWithID 口径。
    Entity newEntity(std::string_view name = "<unnamed>");
    // 指定 ID 创建（M-E2 反序列化入口；ID 冲突 = 断言失败）。
    Entity newEntityWithID(EntityID id, std::string_view name = "<unnamed>");
    // 延迟删除：排队，下一次 tick/flush 生效（含名字/flags 条目清理）。
    // M-E0 无层级；M-E1 落地后父删递归删子（Carrot removeEntity 递归口径）。
    void removeEntity(EntityID id);
    // 统一落盘：先加后删（Carrot flushEntityCreationAndRemoval 顺序）；
    // M-E0 无组件/无查询缓存，只动实体表 + 名字 + flags 三张表。
    void flushEntityCreationAndRemoval();
    // tick：开头先 flush（延迟语义锚点）。M-E0 无 system，只 flush；
    // M-E2 在 flush 与收尾之间插 system firstTick/tick。
    void tick(double dt);

    // ---- 查询（O(N) 遍历，50 物体规模锚，无缓存） ----
    bool exists(EntityID id) const;             // = names_.contains（Carrot 口径）
    std::vector<Entity> getAllEntities() const; // flush 后的全局表
    std::vector<Entity> getEntitiesWithFlags(EntityFlags flags) const; // 位过滤

    // ---- 名字 ----
    const std::string& getName(EntityID id) const;
    void updateName(EntityID id, std::string_view name);
    std::optional<Entity> findEntityByName(std::string_view name) const;

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
};

} // namespace nl::scene
