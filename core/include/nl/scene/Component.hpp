#pragma once

// M-E1：Component 基类 + ComponentLibrary + TransformComponent
// （P8 E1a 拆分段，docs/p8/P8-ECS-PLAN.md M-E1 行；2026-11-09 精读留痕）。
//
// 口径来源（三处对照）：
//   - Carrot engine/ecs/components/Component.h（节奏参照，逐条对标）：
//     基类五虚 = getName / duplicate / serialise / deserialise / getComponentTypeID；
//     duplicate(newOwner) 返回 unique_ptr 深拷（组件数据拷贝、owner 换新）；
//     serialise 默认空实现 = 无状态组件不用覆写；deserialise 同（Carrot 同款）。
//     Carrot 的 reloadComponent/unloadComponent/repairLinks 属 C# 热载/Prefab
//     修复（M-E3 后按需），本轮不进。
//   - Carrot components/TransformComponent.{h,cpp} + math/Transform.cpp：
//     TRS 构造顺序 translate*rotate*scale；层级复合 = parentGlobal * local
//     （toTransformMatrix 递归）；世界位置 = worldMat * (0,0,0,1) 归宿在
//     computeFinalPosition。世界 scale/orientation 用逐级复合
//     （computeFinalScale = parentScale * localScale）——非均匀 scale 下是近似，
//     Carrot 同病，锚点用例避开剪切情形（见 ecs-hier selftest 注释）。
//   - Carrot ecs/World.cpp World::setParent/reparent/duplicateWithRemap：
//     层级关系存 World 端双结构（entityParents 子->父 + entityChildren 父->子），
//     与组件解耦；reparent = 先取世界变换 -> setParent -> 写回局部。
//   - bevy：无对应物（bevy 是关系型 ECS，层级走 ChildOf 关系组件）；
//     OOP 组件路线按拍板跟 Carrot。
//
// 有意裁剪（50 物体规模锚 + E1a 边界，P8-ECS-PLAN「砍掉的范围」）：
//   - Carrot 反射 machinery（ComponentReflection/BaseComponentPropertyReflection/
//     FIELD 宏）——我们显式注册 + 手写 serialise（M-E2 只 3 个组件，反射过度设计）。
//   - IdentifiableComponent 的 per-instance 组件 UUID——组件不需要跨网标识，
//     类型 ID + 实体 ID 足够定位。
//
// 类型 ID 口径（P8-ECS-PLAN 拍板）：组件类型 ID = 0..63 递增分配（静态计数），
// M-E2 System 的 Signature = bitset<64> 位对应；类型数 <64 硬上限。
//
// 序列化：接口签名用 vendored nlohmann json（thirdparty/tinygltf/json.hpp，
// 零新依赖，P8-ECS-PLAN 拍板）；M-E1 只立接口，场景文件 roundtrip 是 M-E2 判据。

#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>

#include "json.hpp"
#include "nl/math/math.hpp"
#include "nl/scene/World.hpp"

namespace nl::scene {

// 数学类型注入（定向 using 声明，不开 using namespace——anim.hpp 同款纪律）。
using nl::math::Mat4;
using nl::math::Quat;
using nl::math::Vec3;
// 函数注入：头内 Transform::toMatrix/rotation 默认值用；实现在 Component.cpp
// 复用同一组（nl::math::Vec3/Quat 及函数 = API 契约，消费方侧 nl::math:: 限定）。
using nl::math::mat4TRS;
using nl::math::quatIdentity;

// 组件类型 ID：0..63（Signature 位对应；静态递增分配，跨 TU 唯一）。
// 定义在 World.hpp（基础类型层，World 存储与组件层共用）。

class Component {
public:
    explicit Component(Entity owner) : owner_(owner) {}
    virtual ~Component() = default;

    Entity& getEntity() { return owner_; }
    const Entity& getEntity() const { return owner_; }

    // 组件名（序列化 key + 工厂查找 key；Carrot getName 口径）。
    virtual const char* getName() const = 0;

    virtual ComponentTypeID getComponentTypeID() const = 0;

    // 深拷：数据成员逐份拷贝，owner 换成新实体（duplicate 实体入口）。
    virtual std::unique_ptr<Component> duplicate(Entity newOwner) const = 0;

    // 序列化接口（M-E2 场景 roundtrip 消费；默认空 = 无状态组件不覆写）。
    virtual nlohmann::json serialise() const { return {}; }
    virtual void deserialise(const nlohmann::json& j) { (void)j; }

private:
    Entity owner_;
};

// CRTP 中间层：类型 ID / 名字 / duplicate 三件套自动落地。
// 约定：T 提供 static constexpr const char* kName；T 必须有
// T(const T& other, Entity newOwner) 拷构造（duplicate 语义 = 换 owner 的深拷）。
// 组件类型 ID 的全局递增计数器（M-E1b.0 修正）。
// 必须放在非模板 inline 函数里：inline 函数的函数级 static 在整个程序只有一份实例，
// 各 TU 共享；若像原实现那样放在 ComponentOf<T> 的成员函数里，模板每个 T 实例化
// 都各自持有一份 next，所有组件类型都会拿到 id=0 —— getComponent<U>() 会把
// 槽 0 上别的组件 static_cast 成 U* 读内存（虚假灯数 / 段错误，UB）。
inline ComponentTypeID allocateComponentTypeID() {
    static ComponentTypeID next = 0;
    assert(next < kMaxComponentTypes && "component type count exceeds 64 (Signature bitset)");
    return next++;
}

template <class T>
class ComponentOf : public Component {
public:
    using Component::Component;

    const char* getName() const override { return T::kName; }
    static ComponentTypeID typeID() {
        static const ComponentTypeID id = allocateComponentTypeID();
        return id;
    }
    ComponentTypeID getComponentTypeID() const override { return typeID(); }
    std::unique_ptr<Component> duplicate(Entity newOwner) const override {
        return std::make_unique<T>(*static_cast<const T*>(this), newOwner);
    }
};

// ---------------------------------------------------------------------------
// ComponentLibrary：名字 -> 工厂注册表（Carrot ComponentLibrary 口径裁剪版）。
// create 用于反序列化（M-E2）/编辑器新建；duplicate 用组件自身 duplicate 虚函数。
// ---------------------------------------------------------------------------
class ComponentLibrary {
public:
    template <class T>
    void registerComponent() {
        static_assert(std::is_base_of_v<Component, T>, "T must derive Component");
        factories_[T::kName] = [](Entity e) -> std::unique_ptr<Component> {
            return std::make_unique<T>(e);
        };
    }

    bool has(const std::string& name) const { return factories_.find(name) != factories_.end(); }

    // 未注册名 = 空指针（M-E2 负向判据：未知组件名显式退出码 2）。
    std::unique_ptr<Component> create(const std::string& name, Entity owner) const {
        auto it = factories_.find(name);
        return it != factories_.end() ? it->second(owner) : nullptr;
    }

private:
    using Factory = std::unique_ptr<Component> (*)(Entity);
    std::unordered_map<std::string, Factory> factories_;
};

// ---------------------------------------------------------------------------
// TransformComponent：层级变换组件（M-E1 判据主载体）。
// 层级关系本身在 World 端（setParent/...），本组件只持局部 TRS + 世界矩阵复合。
// ---------------------------------------------------------------------------
struct Transform {
    Vec3 position;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    Quat rotation = quatIdentity();

    // TRS = translate * rotate * scale（Carrot Math/Transform.cpp:14-21 对标）。
    Mat4 toMatrix() const { return mat4TRS(position, rotation, scale); }
};

class TransformComponent : public ComponentOf<TransformComponent> {
public:
    explicit TransformComponent(Entity owner) : ComponentOf<TransformComponent>(owner) {}
    // duplicate 用换 owner 拷构造（ComponentOf<T>::duplicate 消费）。
    TransformComponent(const TransformComponent& other, Entity newOwner)
        : ComponentOf<TransformComponent>(newOwner), local(other.local) {}

    static constexpr const char* kName = "Transform";

    Transform local;   // 局部 TRS（相对父；无父 = 世界）。

    // 世界矩阵：parentGlobal * local 递归复合；无父 / 父无 Transform = local
    // （Carrot TransformComponent::toTransformMatrix 口径：层级挂在有 Transform
    // 组件的实体链上，断链即到此为止）。
    Mat4 getWorldMatrix() const;

    // 世界位置 = worldMat * (0,0,0,1)（Carrot computeFinalPosition 矩阵路径）。
    Vec3 getWorldPosition() const;

    // 写回世界变换（reparent 落点）：local = inverse(parentWorld) * world 后
    // TRS 分解（各列长度 = scale、列归一化 = 旋转、第 4 列 = position）。
    // 非均匀 scale + 旋转的父链会产生剪切，分解为近似（Carrot glm::decompose
    // 同病）；锚点用例用均匀 scale 父保证严格成立。
    void setWorldTransform(const Transform& world);
    // 矩阵版主实现（reparent 直接传旧世界矩阵，保证 reparent 前后世界矩阵
    // 逐元素不变是硬判据；Transform 版 = toMatrix 后转发）。
    // 与 Carrot 的差异留痕：Carrot reparent 走 computeGlobalPhysicsTransform +
    // setGlobalTransform 逐字段路径（physics transform 不带 scale，scale 另算）；
    // 我们统一走矩阵路径——reparent 保世界矩阵严格成立优先于像素级对标。
    void setWorldFromMatrix(const Mat4& worldMat);

    // ---- M-E2 场景 roundtrip（float -> json double -> float 无损：
    // float 值的 double 表示精确，序列化数组 [x,y,z] / [x,y,z,w]）----
    nlohmann::json serialise() const override;
    void deserialise(const nlohmann::json& j) override;
};

// ---------------------------------------------------------------------------
// ModelComponent（M-E1b.0，渲染语义组件首次进 nl/scene）。
// 口径来源：Carrot ecs/components/ModelComponent.h——组件持「文件路径 + 颜色」
// 字符串/纯数据，GPU 资源由渲染侧按路径加载（我们 viewer 侧同步加载，Carrot 的
// AsyncModelResource 异步句柄是升级项）；零图形符号纪律不破（只有 std::string
// 与 Vec3，NVRHI/ImGui 零 include）。
// direction/deserialise 与 TransformComponent 同款：数组序固定、float<->double
// 无损、缺键 = nlohmann 异常（World::deserialise 统一 catch 转显式报错）。
// ---------------------------------------------------------------------------
class ModelComponent : public ComponentOf<ModelComponent> {
public:
    explicit ModelComponent(Entity owner) : ComponentOf<ModelComponent>(owner) {}
    ModelComponent(const ModelComponent& other, Entity newOwner)
        : ComponentOf<ModelComponent>(newOwner), meshFile(other.meshFile), tint(other.tint) {}

    static constexpr const char* kName = "Model";

    std::string meshFile;          // assets/ 下的资产文件名（白名单校验在 viewer 侧）。
    Vec3 tint{1.0f, 1.0f, 1.0f};   // 实体级着色（N 物体画面区分用；Carrot color 字段口径）。

    nlohmann::json serialise() const override;
    void deserialise(const nlohmann::json& j) override;
};

// ---------------------------------------------------------------------------
// LightComponent（M-E1b.0）：方向灯纯数据组件（bevy PointLight/UE ULightComponent
// 的「灯数据存组件」口径；本期只支持方向灯数组，点光源等 pbr-direct merge 后
// 扩类型字段）。方向存组件字段（世界空间光照传播方向），从 Transform 层级推导
// 方向是升级项（UE 口径），留痕 docs/p8/P8-ECS-PLAN.md。
// ---------------------------------------------------------------------------
class LightComponent : public ComponentOf<LightComponent> {
public:
    explicit LightComponent(Entity owner) : ComponentOf<LightComponent>(owner) {}
    LightComponent(const LightComponent& other, Entity newOwner)
        : ComponentOf<LightComponent>(newOwner),
          direction(other.direction), color(other.color), intensity(other.intensity) {}

    static constexpr const char* kName = "Light";

    Vec3 direction{0.0f, -1.0f, 0.0f};  // 光照传播方向（世界空间，viewer 侧归一化）。
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;

    nlohmann::json serialise() const override;
    void deserialise(const nlohmann::json& j) override;
};

} // namespace nl::scene
