#pragma once

// M-E2：System 基类 + SignedSystem + SystemLibrary
// （P8 E1a 拆分段，docs/p8/P8-ECS-PLAN.md M-E2 行；2026-11-09 精读留痕）。
//
// 口径来源（三处对照）：
//   - Carrot engine/ecs/systems/System.{h,cpp,.ipp}（节奏参照，逐条对标）：
//     实体列表由 World 推送维护（onEntitiesAdded/Removed/Updated），系统不自扫
//     ——World 在 flush 增删落盘后、组件增删后推送（World.cpp:182/268/277/888）；
//     签名匹配 = (entitySig & systemSig) == systemSig（子集判定，System.cpp:515）；
//     生命周期 firstTick（注册后首个 tick 恰一次，waitingForFirstTick 清空）->
//     tick(dt)（World::tick:313-321 口径：flush 之后、逐系统顺序调用）；
//     duplicate/serialise/getName 虚（场景保存与编辑器拷贝消费）。
//   - bevy：关系型 ECS 无 System 基类（schedule + system 参数自动注入）；
//     OOP 路线按拍板跟 Carrot，Signature 位对应组件类型 ID 是两代 ECS 共同的
//     经典口径（bevy 早期 bevy_ecs 同款 bitset）。
//   - UE：无公开 ECS 基类对标（UWorld/Actor 是 OOP 直系），不涉。
//
// 有意裁剪（P8-ECS-PLAN「砍掉的范围」+ 50 物体规模锚，升级路径注明）：
//   - Logic/Render 双类目 + frozenLogic：单列表统一 tick（渲染系统 E1b 后挂时
//     若需分列再加，届时 duplicate/serialise 同步扩展）；
//   - prePhysics/postPhysics/swapBuffers/broadcastStart/Stop 事件：无物理/双缓冲
//     消费方，进了也没人调；
//   - parallelSubmit/TaskScheduler：job-system 轨道已收官（master feat/job-system），
//     需要并行 tick 时按 M-J 口径挂接；
//   - entitiesWithComponents 缓存列表 + recreateEntityWithComponentsList：
//     50 物体规模 forEachEntity 现查组件指针足够，不缓存。

#include <bitset>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.hpp"
#include "nl/scene/World.hpp"

namespace nl::scene {

// 签名 = 组件类型 ID 位集（ComponentTypeID 0..63 <-> bitset 位，World.hpp 口径）。
using Signature = std::bitset<kMaxComponentTypes>;

// ---------------------------------------------------------------------------
// 读写集访问标记（M-E1b.5，UE Mass EMassFragmentAccess 口径裁剪版）：
//   Read<T>  = 只读组件 T（多个只读者可并行）
//   Write<T> = 读写组件 T（与任何触及 T 的系统互斥）
// 裸组件类型 T = Write<T>（M-E2 SignedSystem<Required...> 向后兼容：Required
// 保守视为读写，既有系统零改动、行为不变；要用并行就显式标 Read<>）。
// 口径来源（三处对照，2026-11-09 精读留痕）：
//   - UE Mass：FMassFragmentRequirements::AddRequirement(FragmentType,
//     EMassFragmentAccess::ReadOnly/ReadWrite)（MassRequirements.h:196-243）
//     + FMassProcessorDependencySolver 从读写需求求解依赖图（:95-236）。
//   - bevy：MultiThreadedExecutor 用 system.access.is_compatible 冲突判定自动
//     建边（multi_threaded.rs:188）——access 由 Query 参数反射得出，OOP ECS
//     无反射，抄思路（读写集冲突判定）不抄实现（显式声明代替反射）。
//   - Carrot：World::tick 严格串行（World.cpp:321-332），System::parallelSubmit
//     只做系统内数据并行——系统间调度本项目按 UE 显式声明路线走（用户拍板 A）。
// ---------------------------------------------------------------------------
template <class T> struct Read  { using Type = T; };
template <class T> struct Write { using Type = T; };

// 访问标记解包：AccessTypeOf<Read<T>>::Type = AccessTypeOf<Write<T>>::Type =
// AccessTypeOf<T>::Type = T（裸类型直通）。
template <class A> struct AccessTypeOf { using Type = A; };
template <class T> struct AccessTypeOf<Read<T>>  { using Type = T; };
template <class T> struct AccessTypeOf<Write<T>> { using Type = T; };

// 是否写访问：Write<T> 与裸类型 = true（保守），Read<T> = false。
template <class A> struct IsWriteAccess : std::true_type {};
template <class T> struct IsWriteAccess<Read<T>>  : std::false_type {};
template <class T> struct IsWriteAccess<Write<T>> : std::true_type {};

// ---------------------------------------------------------------------------
// System 基类：Carrot System.h 口径裁剪版。
// 约定：具体系统提供 static constexpr const char* kName（SystemLibrary 注册
// / 场景序列化 key，与 Component::kName 同款纪律）。
// ---------------------------------------------------------------------------
class System {
public:
    explicit System(World& world) : world_(world) {}
    virtual ~System() = default;

    virtual const char* getName() const = 0;

    const Signature& getSignature() const { return signature_; }
    // 读写集（M-E1b.5 tickParallel 求解输入；基类默认空 = 未声明访问，
    // 语义 = 自足系统不触碰任何共享组件——契约见 World::tickParallel 注释）。
    const Signature& getReads() const { return reads_; }
    const Signature& getWrites() const { return writes_; }
    // World 推送维护的实体列表（签名匹配者；含注册时已存在的全部匹配实体）。
    const std::vector<Entity>& getEntities() const { return entities_; }
    World& getWorld() const { return world_; }

    // 生命周期：firstTick = 注册后首个 tick 恰一次（World::tick 清 waiting 列表）。
    virtual void firstTick() {}
    virtual void tick(double dt) { (void)dt; }

    // 编辑器拷贝：数据成员拷贝、挂到新 World（Component::duplicate 同款语义）。
    virtual std::unique_ptr<System> duplicate(World& newOwner) const = 0;

    // 场景序列化（World::serialise 的 systems 数组消费；默认空 = 无状态系统）。
    virtual nlohmann::json serialise() const { return {}; }
    virtual void deserialise(const nlohmann::json& j) { (void)j; }

    // ---- 实体列表维护（World 推送调用，friend World；系统不自扫——Carrot 口径）----
    // 签名匹配者进列表；签名判定现算 getSignature(id)（50 物体规模，无缓存）。
    void onEntitiesAdded(const std::vector<EntityID>& added);
    void onEntitiesRemoved(const std::vector<EntityID>& removed);
    // 组件增删后推送：按最新签名重新匹配（出列表或补进列表，Carrot 口径）。
    void onEntitiesUpdated(const std::vector<EntityID>& updated);

    // 签名匹配判定（onEntities* 共用）：实体签名是系统签名的超集。
    bool matches(EntityID id) const;

protected:
    friend class World;
    World& world_;
    Signature signature_;
    Signature reads_;    // M-E1b.5：只读组件集（SignedSystem 填充；基类空）
    Signature writes_;   // M-E1b.5：写组件集（SignedSystem 填充；基类空）
    std::vector<Entity> entities_;
};

// ---------------------------------------------------------------------------
// SignedSystem<Required...>：构造时把所需组件类型位加进签名（Carrot System.ipp
// 口径：signature.addComponents<Required...>()）。forEachEntity 转发组件指针。
//
// M-E1b.5：模板参数同时展开为读写集——Read<T> 记只读、Write<T>/裸 T 记读写
// （裸类型保守 = 写，保 M-E2 语义向后兼容：既有系统与触及同组件的系统保持
// 串行，零行为变化；显式标 Read<> 才解锁并行）。
// ---------------------------------------------------------------------------
template <class... Required>
class SignedSystem : public System {
public:
    explicit SignedSystem(World& world) : System(world) {
        (setAccess<Required>(), ...);
    }

    // 逐实体回调（组件指针现查，无缓存——文件头裁剪留痕）。
    // Required 可能是 Read<T>/Write<T> 包装——取组件前经 AccessTypeOf 解包。
    template <class F>
    void forEachEntity(F&& f) {
        for (Entity e : entities_) {
            f(e, *(world_.getComponent<typename AccessTypeOf<Required>::Type>(
                      e.getID()))...);
        }
    }

private:
    // 单参数展开：签名位 + 读位恒置；写位仅 Write<T>/裸 T 置。
    // 注意 forEachEntity 的 getComponent<Required> 用的仍是原模板参数，
    // Read<>/Write<> 不能直接当组件类型——解包见 AccessTypeOf。
    template <class A>
    void setAccess() {
        const ComponentTypeID id =
            componentTypeID<typename AccessTypeOf<A>::Type>();
        signature_.set(id);
        reads_.set(id);
        if (IsWriteAccess<A>::value) writes_.set(id);
    }
};

// ---------------------------------------------------------------------------
// SystemLibrary：名字 -> 工厂注册表（ComponentLibrary 同款裁剪版）。
// create 用于场景反序列化；duplicate 用系统自身 duplicate 虚函数。
// ---------------------------------------------------------------------------
class SystemLibrary {
public:
    template <class T>
    void registerSystem() {
        static_assert(std::is_base_of_v<System, T>, "T must derive System");
        factories_[T::kName] = [](World& w) -> std::unique_ptr<System> {
            return std::make_unique<T>(w);
        };
    }

    bool has(const std::string& name) const { return factories_.find(name) != factories_.end(); }

    // 未注册名 = 空指针（M-E2 负向判据：未知系统名显式报错）。
    std::unique_ptr<System> create(const std::string& name, World& world) const {
        auto it = factories_.find(name);
        return it != factories_.end() ? it->second(world) : nullptr;
    }

private:
    using Factory = std::unique_ptr<System> (*)(World&);
    std::unordered_map<std::string, Factory> factories_;
};

} // namespace nl::scene
