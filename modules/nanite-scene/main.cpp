// nanite-scene —— ECS-lite 场景（nl/scene）自测工具，A 类并行轨道。
// 只依赖 core（nl/scene），零图形 API 符号；接线仿 anim-tools 先例。
//
// 退出码约定（与 nanite-tools / 展示台 expectExit 0/1/2 对齐）：
//   0 = 用例全过（打印 "<suite> selftest: PASS"）
//   1 = 断言失败（打印 "<suite> selftest: FAIL <detail>"）
//   2 = 用法错误（未知子命令 / 缺 --suite / 未知 suite 名）
//
// 用例（docs/p8/P8-ECS-PLAN.md M-E0/M-E1/M-E2 判据）：
//   ecs-core      World/Entity 核心：递增 ID 创建 / 增删 flush 前后存在性
//                 （Carrot 口径：新建即时可见、删除 flush 后不可见）/
//                 名字读写 / Flags(Hidden) 过滤 / 延迟语义锚点
//                 （删实体下一 tick 生效；M-E2 有 system 后补 tick 内删除锚点）
//   ecs-hier      M-E1 组件+层级：三层父子链复合矩阵数值锚（手算）/ reparent
//                 保世界变换 / 删父递归删子 / duplicate 深拷 / 组件库工厂
//   ecs-system    M-E2 System：签名匹配收编（注册收编/flush 收编/组件增删自动
//                 进出列表）/ firstTick 恰一次 / tick 计数与 dt 传递 / duplicate
//   ecs-serial    M-E2 场景 roundtrip：serialise->deserialise->serialise 逐字节
//                 一致 + 内容抽查 + 负向（未知组件名/未知父/未知系统名显式拒绝）
//   ecs-parallel  M-E1b.5 并行 tick：读写集求解（边数/dot 确定性）/ 真并行重叠
//                 证明 / 并行==串行逐位对拍 / 裸类型保守写 / 结构变更守卫
//                 / firstTick 恰一次（World::tickParallel 契约 1~3 全覆盖）
// 另有子命令 deserialise-check（负向判据显式退出码 2，见 main）。

#include "nl/scene/Component.hpp"
#include "nl/scene/System.hpp"
#include "nl/scene/World.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
bool g_verbose = false; // --verbose：断言成功也逐条打印（证据图数据源；默认只报 FAIL + 总结）

inline void verboseOk(const char* what) {
    if (g_verbose) std::printf("  ok %s\n", what);
}

void expectTrue(bool ok, const char* what) {
    if (ok) { verboseOk(what); return; }
    std::printf("  FAIL %s\n", what);
    ++g_failures;
}

void expectSize(size_t got, size_t want, const char* what) {
    if (got == want) { verboseOk(what); return; }
    std::printf("  FAIL %s: got %zu want %zu\n", what, got, want);
    ++g_failures;
}

void expectId(nl::scene::EntityID got, nl::scene::EntityID want, const char* what) {
    if (got == want) { verboseOk(what); return; }
    std::printf("  FAIL %s: got %llu want %llu\n", what,
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
    ++g_failures;
}

void expectStr(const std::string& got, const char* want, const char* what) {
    if (got == want) { verboseOk(what); return; }
    std::printf("  FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want);
    ++g_failures;
}

void expectNear(float got, float want, float eps, const char* what) {
    if (std::fabs(got - want) <= eps) { verboseOk(what); return; }
    std::printf("  FAIL %s: got %.6f want %.6f (eps %g)\n", what, got, want, eps);
    ++g_failures;
}

void expectVec3(nl::math::Vec3 got, nl::math::Vec3 want, float eps, const char* what) {
    if (std::fabs(got.x - want.x) <= eps && std::fabs(got.y - want.y) <= eps &&
        std::fabs(got.z - want.z) <= eps) { verboseOk(what); return; }
    std::printf("  FAIL %s: got (%.6f, %.6f, %.6f) want (%.6f, %.6f, %.6f)\n",
                what, got.x, got.y, got.z, want.x, want.y, want.z);
    ++g_failures;
}

void expectQuat(nl::math::Quat got, nl::math::Quat want, float eps, const char* what) {
    // 双覆盖对齐：dot < 0 时取反再比（同一旋转的另一半）；单条断言单行输出。
    if (nl::math::dotQuat(got, want) < 0.0f) {
        got = { -got.x, -got.y, -got.z, -got.w };
    }
    if (std::fabs(got.x - want.x) <= eps && std::fabs(got.y - want.y) <= eps &&
        std::fabs(got.z - want.z) <= eps && std::fabs(got.w - want.w) <= eps) {
        verboseOk(what); return;
    }
    std::printf("  FAIL %s: got (%.6f, %.6f, %.6f, %.6f) want (%.6f, %.6f, %.6f, %.6f)\n",
                what, got.x, got.y, got.z, got.w, want.x, want.y, want.z, want.w);
    ++g_failures;
}

void expectMat4(const nl::math::Mat4& got, const nl::math::Mat4& want, float eps,
                const char* what) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(got.m[i] - want.m[i]) > eps) {
            std::printf("  FAIL %s: element[%d] got %.6f want %.6f\n", what, i,
                        got.m[i], want.m[i]);
            ++g_failures;
            return;
        }
    }
    verboseOk(what);
}

using nl::scene::Entity;
using nl::scene::EntityFlags;
using nl::scene::World;

// ---------------------------------------------------------------------------
// 创建：ID 递增（拍板口径：u64 计数器从 1 起，0 = 无效）
// ---------------------------------------------------------------------------

void testCreateIncrementIds() {
    World w;
    Entity a = w.newEntity("a");
    Entity b = w.newEntity("b");
    expectId(a.getID(), 1, "first id == 1");
    expectId(b.getID(), 2, "second id == 2 (increment)");
    expectTrue(a.exists(), "new entity exists immediately (name table instant)");
    expectTrue(w.exists(a.getID()) && w.exists(b.getID()), "world.exists by id");
    expectTrue(!w.exists(0), "kInvalidEntity(0) does not exist");
    expectTrue(!w.exists(99), "unknown id does not exist");
}

// ---------------------------------------------------------------------------
// 延迟增删 flush 前后存在性（Carrot 口径的非对称：新建即时 / 删除延迟）
// ---------------------------------------------------------------------------

void testDeferredAddRemoveFlush() {
    World w;
    Entity a = w.newEntity("a");
    Entity b = w.newEntity("b");

    // flush 前：全局实体表还没有（Carrot：entitiesToAdd 未落盘）
    expectSize(w.getAllEntities().size(), 0, "getAllEntities empty before flush");

    w.tick(1.0 / 60.0); // tick 开头 flush
    expectSize(w.getAllEntities().size(), 2, "getAllEntities == 2 after flush");
    expectTrue(a.exists() && b.exists(), "both exist after flush");

    // 延迟删除锚点：removeEntity 只排队，flush（下一次 tick）才生效
    a.remove();
    expectTrue(a.exists(), "exists still true right after remove (deferred)");
    expectSize(w.getAllEntities().size(), 2, "list unchanged before flush");

    w.tick(1.0 / 60.0);
    expectTrue(!a.exists(), "exists false after next tick (deferred anchor)");
    expectTrue(b.exists(), "survivor still exists");
    expectSize(w.getAllEntities().size(), 1, "getAllEntities == 1 after removal flush");
    expectStr(w.getName(a.getID()), "", "name of removed entity is empty");

    // 重复 removeEntity 幂等（Carrot：重复排队无害）
    w.removeEntity(b.getID());
    w.removeEntity(b.getID());
    w.tick(0.0);
    expectSize(w.getAllEntities().size(), 0, "double remove is idempotent");
}

// ---------------------------------------------------------------------------
// 名字读写 + findEntityByName
// ---------------------------------------------------------------------------

void testNames() {
    World w;
    Entity a = w.newEntity("root");
    Entity b = w.newEntity(); // 默认名
    expectStr(a.getName(), "root", "getName initial");
    expectStr(b.getName(), "<unnamed>", "default name (Carrot convention)");

    // 名字表即时登记，但 findEntityByName 扫全局实体表（flush 后才有）——Carrot 口径
    expectTrue(!w.findEntityByName("root").has_value(),
               "findEntityByName before flush misses (entity table deferred)");
    w.tick(0.0); // flush

    a.updateName("renamed");
    expectStr(a.getName(), "renamed", "updateName visible via Entity");
    expectStr(w.getName(a.getID()), "renamed", "updateName visible via World");

    auto found = w.findEntityByName("renamed");
    expectTrue(found.has_value() && found->getID() == a.getID(), "findEntityByName hit");
    expectTrue(!w.findEntityByName("nope").has_value(), "findEntityByName miss -> nullopt");

    w.updateName(b.getID(), "b");
    found = w.findEntityByName("b");
    expectTrue(found.has_value() && found->getID() == b.getID(), "find after rename");
}

// ---------------------------------------------------------------------------
// Flags(Hidden)：位标志不是删除；过滤 = 显式查询（bevy entity_disabling 同向）
// ---------------------------------------------------------------------------

void testFlagsHidden() {
    World w;
    Entity a = w.newEntity("a");
    Entity b = w.newEntity("b");
    Entity c = w.newEntity("c");
    w.tick(0.0);

    // 默认可见
    expectTrue(a.isVisible() && b.isVisible() && c.isVisible(), "default visible");
    expectId(static_cast<nl::scene::EntityID>(a.getFlags()),
             static_cast<nl::scene::EntityID>(EntityFlags::None), "default flags None");

    // 隐藏 a：still exists（隐藏 != 删除），过滤查询只含未隐藏者
    a.setFlags(EntityFlags::Hidden);
    expectTrue(a.exists(), "hidden entity still exists (hide is not delete)");
    expectTrue(!a.isVisible(), "isVisible false after setFlags(Hidden)");
    expectSize(w.getEntitiesWithFlags(EntityFlags::Hidden).size(), 1,
               "getEntitiesWithFlags(Hidden) == 1");
    expectSize(w.getAllEntities().size(), 3, "getAllEntities ignores flags (== 3)");

    // 混合：b 也隐藏 -> 2 个隐藏
    b.setFlags(EntityFlags::Hidden);
    expectSize(w.getEntitiesWithFlags(EntityFlags::Hidden).size(), 2,
               "getEntitiesWithFlags(Hidden) == 2");

    // 摘除：removeFlags 恢复可见
    a.removeFlags(EntityFlags::Hidden);
    expectTrue(a.isVisible(), "isVisible true after removeFlags");
    expectSize(w.getEntitiesWithFlags(EntityFlags::Hidden).size(), 1,
               "getEntitiesWithFlags(Hidden) == 1 after unhide");
}

} // namespace

// ---------------------------------------------------------------------------
// M-E1：组件 + 层级（数值锚全部手算对照）
// ---------------------------------------------------------------------------

namespace {

using nl::math::Mat4;
using nl::math::Vec3;
using nl::scene::ComponentLibrary;
using nl::scene::TransformComponent;

constexpr float kEps = 1e-4f;

// 手算锚定用三层链（均匀 scale 父 => 复合无剪切，reparent 分解严格成立）：
//   A: pos(1,0,0) scale(2) rot=I
//   B: pos(0,1,0) scale(3) rot=Rz(90)
//   C: pos(1,1,0) scale(1) rot=I
// W_C = A@ * B@ * C@（列主序，手算）：
//   col0=(0,6,0) col1=(-6,0,0) col2=(0,0,6) col3=(-5,8,0)
// C 世界位置 = W_C * (1,1,0,1) = (-11,14,0)
void buildChain(World& w, Entity& a, Entity& b, Entity& c) {
    a = w.newEntity("A");
    b = w.newEntity("B");
    c = w.newEntity("C");
    auto* ta = static_cast<TransformComponent*>(&a.addComponent(std::make_unique<TransformComponent>(a)));
    ta->local.position = { 1.0f, 0.0f, 0.0f };
    ta->local.scale = { 2.0f, 2.0f, 2.0f };
    auto* tb = static_cast<TransformComponent*>(&b.addComponent(std::make_unique<TransformComponent>(b)));
    tb->local.position = { 0.0f, 1.0f, 0.0f };
    tb->local.scale = { 3.0f, 3.0f, 3.0f };
    tb->local.rotation = nl::math::quatFromAxisAngle({ 0.0f, 0.0f, 1.0f }, 3.14159265f * 0.5f);
    auto* tc = static_cast<TransformComponent*>(&c.addComponent(std::make_unique<TransformComponent>(c)));
    tc->local.position = { 1.0f, 1.0f, 0.0f };
    b.setParent(a.getID());
    c.setParent(b.getID());
    w.tick(0.0); // flush，让全局表收齐（层级本身不依赖 flush）
}

Mat4 expectedWorldC() {
    // 手算结果直接逐元素写出（独立于被测代码路径的参照物）。
    Mat4 w;
    w.m[0] = 0.0f;  w.m[4] = -6.0f; w.m[8]  = 0.0f; w.m[12] = -5.0f;
    w.m[1] = 6.0f;  w.m[5] = 0.0f;  w.m[9]  = 0.0f; w.m[13] = 8.0f;
    w.m[2] = 0.0f;  w.m[6] = 0.0f;  w.m[10] = 6.0f; w.m[14] = 0.0f;
    return w;
}

// ---------------------------------------------------------------------------
// 三层父子链复合矩阵数值锚（手算对照）+ setParent 纯关系不动变换
// ---------------------------------------------------------------------------

void testHierarchyWorldMatrix() {
    World w;
    Entity a, b, c;
    buildChain(w, a, b, c);

    // 层级关系正确
    expectTrue(a.getParentID().has_value() == false, "A has no parent");
    expectTrue(b.getParentID() == a.getID(), "B.parent == A");
    expectTrue(c.getParentID() == b.getID(), "C.parent == B");
    expectSize(w.getChildIDs(a.getID()).size(), 1, "A has 1 child");
    expectSize(w.getChildIDs(b.getID()).size(), 1, "B has 1 child");

    // setParent 纯关系变更不动局部变换（Carrot setParent/reparent 分工口径）
    auto* tc = w.getComponent<TransformComponent>(c.getID());
    expectVec3(tc->local.position, { 1.0f, 1.0f, 0.0f }, kEps, "C local unchanged by setParent");

    // 复合矩阵数值锚（手算逐元素）
    expectMat4(tc->getWorldMatrix(), expectedWorldC(), kEps, "C world matrix == hand-computed anchor");

    // 世界位置锚（Carrot computeFinalPosition 口径 = 局部原点的世界位置 = W 平移列）
    expectVec3(tc->getWorldPosition(), { -5.0f, 8.0f, 0.0f }, kEps, "C world position anchor (local origin)");
    // 局部点变换锚：W * (1,1,0,1) = (-11,14,0)（手算独立验证矩阵路径）
    expectVec3(nl::math::mat4TransformPoint(tc->getWorldMatrix(), { 1.0f, 1.0f, 0.0f }),
               { -11.0f, 14.0f, 0.0f }, kEps, "C world position of local point (1,1,0)");

    // 中间层：B 世界矩阵 = A@ * B@（线性 2*3 列，平移 2*(0,1,0)+(1,0,0)=(1,2,0)）
    auto* tb = w.getComponent<TransformComponent>(b.getID());
    Mat4 wb = tb->getWorldMatrix();
    expectVec3({ wb.m[0], wb.m[1], wb.m[2] }, { 0.0f, 6.0f, 0.0f }, kEps, "B world col0");
    expectVec3({ wb.m[4], wb.m[5], wb.m[6] }, { -6.0f, 0.0f, 0.0f }, kEps, "B world col1");
    expectVec3({ wb.m[12], wb.m[13], wb.m[14] }, { 1.0f, 2.0f, 0.0f }, kEps, "B world translation");

    // 根节点世界 = 局部
    expectMat4(w.getComponent<TransformComponent>(a.getID())->getWorldMatrix(),
               w.getComponent<TransformComponent>(a.getID())->local.toMatrix(),
               kEps, "root world == local");
}

// ---------------------------------------------------------------------------
// reparent 保世界变换（硬判据：世界矩阵逐元素不变 + 局部值手算锚）
// ---------------------------------------------------------------------------

void testReparentKeepsWorldTransform() {
    World w;
    Entity a, b, c;
    buildChain(w, a, b, c);
    auto* tc = w.getComponent<TransformComponent>(c.getID());

    const Mat4 worldBefore = tc->getWorldMatrix();

    // reparent C: B -> A（保世界变换）
    c.reparent(a.getID());
    expectTrue(c.getParentID() == a.getID(), "C.parent == A after reparent");
    expectSize(w.getChildIDs(b.getID()).size(), 0, "B has no children after reparent");

    // 硬判据：世界矩阵逐元素不变
    expectMat4(tc->getWorldMatrix(), worldBefore, kEps, "world matrix unchanged by reparent");
    expectVec3(tc->getWorldPosition(), { -5.0f, 8.0f, 0.0f }, kEps, "world origin position unchanged");
    expectVec3(nl::math::mat4TransformPoint(tc->getWorldMatrix(), { 1.0f, 1.0f, 0.0f }),
               { -11.0f, 14.0f, 0.0f }, kEps, "world position of local point unchanged");

    // 新局部值手算锚：local = A@^-1 * W = pos(-3,4,0) scale(3) rot=Rz(90)
    expectVec3(tc->local.position, { -3.0f, 4.0f, 0.0f }, kEps, "new local position anchor");
    expectVec3(tc->local.scale, { 3.0f, 3.0f, 3.0f }, kEps, "new local scale anchor");
    expectQuat(tc->local.rotation,
               nl::math::quatFromAxisAngle({ 0.0f, 0.0f, 1.0f }, 3.14159265f * 0.5f),
               kEps, "new local rotation anchor (Rz 90)");

    // reparent 到无 Transform 的父（挂到没有变换语义的父：世界矩阵 = 局部直通）
    World w2;
    Entity p = w2.newEntity("P");          // 无 Transform 组件
    Entity q = w2.newEntity("Q");
    auto* tq = static_cast<TransformComponent*>(&q.addComponent(std::make_unique<TransformComponent>(q)));
    tq->local.position = { 5.0f, 0.0f, 0.0f };
    q.reparent(p.getID());
    expectTrue(q.getParentID() == p.getID(), "reparent under transform-less parent works");
    expectVec3(tq->getWorldPosition(), { 5.0f, 0.0f, 0.0f }, kEps,
               "transform-less parent passes local through (Carrot)");
}

// ---------------------------------------------------------------------------
// 删父递归删子（延迟语义：flush 后子孙全不存在）
// ---------------------------------------------------------------------------

void testRemoveParentRecursesChildren() {
    World w;
    Entity a, b, c;
    buildChain(w, a, b, c);

    b.remove(); // 删 B（C 是 B 的子）
    expectTrue(b.exists() && c.exists(), "still exists right after remove (deferred)");

    w.tick(0.0); // flush
    expectTrue(!b.exists(), "B removed after flush");
    expectTrue(!c.exists(), "C recursively removed with parent (anchor)");
    expectTrue(a.exists(), "A survives");
    expectSize(w.getAllEntities().size(), 1, "only A left");

    // 层级双结构清理：A 的孩子列表应空（flush 摘除）
    expectSize(w.getChildIDs(a.getID()).size(), 0, "A children list cleaned after flush");
}

// ---------------------------------------------------------------------------
// duplicate 深拷（含子孙 + 组件 + 名字 " (Copy)"）
// ---------------------------------------------------------------------------

void testDuplicateDeepCopy() {
    World w;
    Entity a, b, c;
    buildChain(w, a, b, c);

    Entity dup = b.duplicate();
    expectStr(dup.getName(), "B (Copy)", "duplicate name suffix (Carrot)");
    expectTrue(dup.getParentID() == a.getID(), "copy inherits source parent");

    // 子孙递归：副本应有 1 个直接子 "C (Copy)"
    auto kids = dup.getChildren();
    expectSize(kids.size(), 1, "copy has 1 child (deep copy)");
    if (!kids.empty()) {
        expectStr(kids[0].getName(), "C (Copy)", "grandchild copy name");
        // 副本孙的世界矩阵 = 原孙世界矩阵（同父同局部）
        auto* origTc = w.getComponent<TransformComponent>(c.getID());
        auto* dupTc = w.getComponent<TransformComponent>(kids[0].getID());
        expectTrue(dupTc != nullptr, "copy child has Transform component");
        if (origTc && dupTc) {
            expectMat4(dupTc->getWorldMatrix(), origTc->getWorldMatrix(), kEps,
                       "copy child world matrix == original");
        }
    }

    // 组件深拷：副本有 Transform 且与原件局部一致
    auto* origTb = w.getComponent<TransformComponent>(b.getID());
    auto* dupTb = w.getComponent<TransformComponent>(dup.getID());
    expectTrue(dupTb != nullptr && origTb != nullptr, "copy has Transform");
    expectVec3(dupTb->local.position, origTb->local.position, kEps, "copy local == source local");

    // 深拷独立性：改副本不影响原件
    dupTb->local.position = { 9.0f, 9.0f, 9.0f };
    expectVec3(origTb->local.position, { 0.0f, 1.0f, 0.0f }, kEps,
               "source unaffected by copy mutation (independent)");
}

// ---------------------------------------------------------------------------
// 组件库：显式注册工厂 + 按名创建（M-E2 反序列化的铺垫）
// ---------------------------------------------------------------------------

void testComponentLibrary() {
    ComponentLibrary lib;
    lib.registerComponent<TransformComponent>();
    expectTrue(lib.has("Transform"), "library has registered Transform");
    expectTrue(!lib.has("Nope"), "unknown name misses");

    World w;
    Entity e = w.newEntity("e");
    auto comp = lib.create("Transform", e);
    expectTrue(comp != nullptr, "create registered component");
    if (comp) {
        expectTrue(comp->getComponentTypeID() == TransformComponent::typeID(),
                   "created component type id matches");
        expectTrue(comp->getName() == std::string("Transform"), "created component name");
        expectTrue(lib.create("Nope", e) == nullptr, "create unknown -> nullptr (M-E2 negative groundwork)");
    }

    // 类型 ID 唯一且稳定（Signature 位对应的前提）
    World w2;
    Entity e2 = w2.newEntity("e2");
    TransformComponent direct(e2);
    expectTrue(direct.getComponentTypeID() == TransformComponent::typeID(),
               "type id stable across instances");
}

} // namespace

// ---------------------------------------------------------------------------
// M-E2：System + 场景 roundtrip
// ---------------------------------------------------------------------------

namespace {

using nl::scene::ComponentLibrary;
using nl::scene::Entity;
using nl::scene::LightComponent;
using nl::scene::ModelComponent;
using nl::scene::System;
using nl::scene::TransformComponent;
using nl::scene::World;

// 测试系统：要求 Transform（SignedSystem<Required...> 口径）；计数生命周期。
class CounterSystem : public nl::scene::SignedSystem<TransformComponent> {
public:
    using SignedSystem::SignedSystem;

    static constexpr const char* kName = "Counter";

    const char* getName() const override { return kName; }
    void firstTick() override { ++firstTicks; }
    void tick(double dt) override { ++ticks; dtSum += dt; }

    // duplicate = 数据成员拷贝（实体列表由 registerSystem 重新收编，不拷）。
    std::unique_ptr<System> duplicate(World& newOwner) const override {
        auto copy = std::make_unique<CounterSystem>(newOwner);
        copy->ticks = ticks;
        copy->firstTicks = firstTicks;
        copy->dtSum = dtSum;
        return copy;
    }

    nlohmann::json serialise() const override { return {{"ticks", ticks}}; }
    void deserialise(const nlohmann::json& j) override { ticks = j.at("ticks").get<int>(); }

    int firstTicks = 0;
    int ticks = 0;
    double dtSum = 0.0;
};

// 统一注册（库注册顺序不影响 roundtrip——序列化走名字不走 ID）。
// M-E1b.0：ModelComponent/LightComponent 也入注册表（ecs-serial 场景含 Model+Light
// 实体，roundtrip 需要两个组件都能按名字还原）。
void registerLibraries(World& w) {
    w.getComponentLibrary().registerComponent<TransformComponent>();
    w.getComponentLibrary().registerComponent<ModelComponent>();
    w.getComponentLibrary().registerComponent<LightComponent>();
    w.getSystemLibrary().registerSystem<CounterSystem>();
}

// ---------------------------------------------------------------------------
// System：签名匹配收编 + 生命周期
// ---------------------------------------------------------------------------

void testSystemSignatureMatching() {
    World w;
    registerLibraries(w);
    Entity a = w.newEntity("a");   // + Transform
    Entity b = w.newEntity("b");   // 无组件（签名不匹配）
    Entity c = w.newEntity("c");   // + Transform
    a.addComponent(std::make_unique<TransformComponent>(a));
    c.addComponent(std::make_unique<TransformComponent>(c));
    w.tick(0.0); // flush（组件即时已挂，实体此刻落表）

    // 路径 A：注册即收编现存匹配实体（Carrot addSystem -> onEntitiesAdded(entities)）
    auto sys = std::make_unique<CounterSystem>(w);
    CounterSystem* sysPtr = sys.get();
    w.registerSystem(std::move(sys));
    expectSize(sysPtr->getEntities().size(), 2, "register collects signature-matching entities");
    expectTrue(std::none_of(sysPtr->getEntities().begin(), sysPtr->getEntities().end(),
                            [&](const Entity& e) { return e.getID() == b.getID(); }),
               "non-matching entity not collected");

    // 路径 B：组件即时推送收编（Carrot 口径：组件增删即时 onEntitiesUpdated，
    // 实体表才延迟——d 有 Transform 即匹配进列表，不等 flush；非对称留痕）
    Entity d = w.newEntity("d");
    d.addComponent(std::make_unique<TransformComponent>(d));
    expectSize(sysPtr->getEntities().size(), 3, "component add collects immediately (instant push)");
    w.tick(0.016); // flush：d 落表 + firstTick + 第一次 tick
    expectSize(sysPtr->getEntities().size(), 3, "unchanged after flush (already listed)");

    // 组件增删自动进出列表（即时推送 onEntitiesUpdated）
    b.addComponent(std::make_unique<TransformComponent>(b));
    expectSize(sysPtr->getEntities().size(), 4, "component add -> auto join list");
    b.removeComponent(TransformComponent::typeID());
    expectSize(sysPtr->getEntities().size(), 3, "component remove -> auto leave list");

    // 删除实体：flush 推送 removed 出列表（延迟语义锚点）
    d.remove();
    expectSize(sysPtr->getEntities().size(), 3, "still in list right after remove");
    w.tick(0.032);
    expectSize(sysPtr->getEntities().size(), 2, "removed entity leaves list after flush");

    // 生命周期：firstTick 恰一次（两个 tick），dt 传递累计
    expectId(static_cast<nl::scene::EntityID>(sysPtr->firstTicks), 1, "firstTick exactly once");
    expectId(static_cast<nl::scene::EntityID>(sysPtr->ticks), 2, "tick count == 2");
    expectNear(static_cast<float>(sysPtr->dtSum), 0.016f + 0.032f, 1e-6f, "dt passed through");

    // Signature 对标：系统签名 = Transform 位
    expectTrue(sysPtr->getSignature()[TransformComponent::typeID()], "system signature has Transform bit");

    // duplicate：数据拷贝 + 换 World（实体列表由新 World registerSystem 收编）
    World w2;
    registerLibraries(w2);
    // 所有权先落 unique_ptr 再取裸指针（临时对象 .get() 会悬空——崩溃教训见台账）
    std::unique_ptr<nl::scene::System> copyOwned = sysPtr->duplicate(w2);
    auto* copy = static_cast<CounterSystem*>(copyOwned.get());
    expectId(static_cast<nl::scene::EntityID>(copy->ticks), 2, "duplicate copies tick count");
    w2.registerSystem(std::move(copyOwned));
    expectSize(w2.findSystem("Counter")->getEntities().size(), 0,
               "duplicate starts with empty list (new world has no entities)");
}

// ---------------------------------------------------------------------------
// 场景 roundtrip：serialise -> deserialise -> serialise 逐字节一致
// ---------------------------------------------------------------------------

// 构建标准测试场景：三层链 + flags + 系统（数据来自 ecs-hier 数值锚场景）。
void buildSerialiseScene(World& w, CounterSystem** outSys) {
    Entity a = w.newEntity("A");
    Entity b = w.newEntity("B");
    Entity c = w.newEntity("C");
    auto* ta = static_cast<TransformComponent*>(&a.addComponent(std::make_unique<TransformComponent>(a)));
    ta->local.position = { 1.0f, 0.0f, 0.0f };
    ta->local.scale = { 2.0f, 2.0f, 2.0f };
    auto* tb = static_cast<TransformComponent*>(&b.addComponent(std::make_unique<TransformComponent>(b)));
    tb->local.position = { 0.0f, 1.0f, 0.0f };
    tb->local.scale = { 3.0f, 3.0f, 3.0f };
    tb->local.rotation = nl::math::quatFromAxisAngle({ 0.0f, 0.0f, 1.0f }, 3.14159265f * 0.5f);
    auto* tc = static_cast<TransformComponent*>(&c.addComponent(std::make_unique<TransformComponent>(c)));
    tc->local.position = { 1.0f, 1.0f, 0.0f };
    b.setParent(a.getID());
    c.setParent(b.getID());
    w.setFlags(b.getID(), nl::scene::EntityFlags::Hidden);

    // M-E1b.0：实体 D 挂 Model+Light（拍板口径字段值），验证两个渲染组件
    // 走同一条 roundtrip 字节一致路径（JSON 序列化按组件名）。
    Entity d = w.newEntity("D");
    auto* dm = static_cast<ModelComponent*>(&d.addComponent(std::make_unique<ModelComponent>(d)));
    dm->meshFile = "Suzanne.glb";
    dm->tint = { 0.8f, 0.4f, 0.2f };
    auto* dl = static_cast<LightComponent*>(&d.addComponent(std::make_unique<LightComponent>(d)));
    dl->direction = { 0.3f, -0.8f, 0.52f };
    dl->color = { 1.0f, 0.95f, 0.9f };
    dl->intensity = 2.5f;

    // M-E1b.2：实体 E 挂 Point Light（type=1），验证新字段 roundtrip 与
    // 方向灯（D，type=0 不写新键）的向后兼容。
    Entity e = w.newEntity("E");
    auto* el = static_cast<LightComponent*>(&e.addComponent(std::make_unique<LightComponent>(e)));
    el->type = 1;
    el->color = { 0.3f, 0.8f, 1.0f };
    el->intensity = 8.0f;
    el->position = { -2.5f, 1.5f, 3.0f };
    el->range = 9.0f;

    w.tick(0.0); // flush（serialise 序列化 flush 后世界）
    auto sys = std::make_unique<CounterSystem>(w);
    *outSys = sys.get();
    w.registerSystem(std::move(sys));
    w.tick(0.016);
    w.tick(0.032); // 系统计数 firstTick=1 ticks=2（进 serialise data）
}

void testRoundtripByteIdentical() {
    std::printf("[step] enter roundtrip\n"); // 临时插桩
    World w;
    registerLibraries(w);
    CounterSystem* sys = nullptr;
    buildSerialiseScene(w, &sys);
    std::printf("[step] build ok\n"); // 临时插桩

    const std::string dump1 = w.serialise().dump();
    std::printf("[step] dump1 ok\n"); // 临时插桩
    World w2;
    registerLibraries(w2);
    std::string err;
    const bool ok = w2.deserialise(nlohmann::json::parse(dump1), err);
    std::printf("[step] deserialise ok=%d err=%s\n", ok ? 1 : 0, err.c_str()); // 临时插桩
    expectTrue(ok, "deserialise succeeds");
    if (!ok) {
        std::printf("  (err: %s)\n", err.c_str());
        return;
    }
    const std::string dump2 = w2.serialise().dump();
    std::printf("[step] dump2 ok\n"); // 临时插桩
    expectTrue(dump1 == dump2, "roundtrip byte-identical (M-E2 hard criterion)");
    if (dump1 != dump2) {
        std::printf("  dump1: %s\n  dump2: %s\n", dump1.c_str(), dump2.c_str());
    }

    // 内容抽查：ID/名字/层级/flags/组件数值/系统状态逐一还原
    expectTrue(w2.exists(1) && w2.exists(2) && w2.exists(3), "ids preserved");
    expectStr(w2.getName(1), "A", "name preserved");
    expectTrue(w2.getParentID(1) == std::nullopt, "A root preserved");
    expectTrue(w2.getParentID(2) == 1, "B.parent == A preserved");
    expectTrue(w2.getParentID(3) == 2, "C.parent == B preserved");
    expectTrue((w2.getFlags(2) & nl::scene::EntityFlags::Hidden) != nl::scene::EntityFlags::None,
               "Hidden flags preserved");
    auto* tc2 = w2.getComponent<TransformComponent>(3);
    expectVec3(tc2->local.position, { 1.0f, 1.0f, 0.0f }, kEps, "C local position preserved");
    expectVec3(tc2->local.scale, { 1.0f, 1.0f, 1.0f }, kEps, "C local scale preserved");
    expectQuat(tc2->local.rotation, nl::math::quatIdentity(), kEps, "C local rotation preserved");
    // 浮点无损锚：非平凡 quat（Rz90 的 float 值 double 往返必须逐位一致）
    auto* tb2 = w2.getComponent<TransformComponent>(2);
    expectQuat(tb2->local.rotation,
               nl::math::quatFromAxisAngle({ 0.0f, 0.0f, 1.0f }, 3.14159265f * 0.5f),
               kEps, "B rotation preserved (float->double->float lossless)");
    expectMat4(tc2->getWorldMatrix(), expectedWorldC(), kEps, "world matrix rebuilt identically");
    auto* sys2 = static_cast<CounterSystem*>(w2.findSystem("Counter"));
    expectTrue(sys2 != nullptr, "system restored");
    if (sys2) {
        expectId(static_cast<nl::scene::EntityID>(sys2->ticks), 2, "system serialise data restored");
        expectSize(sys2->getEntities().size(), 3, "restored system collects 3 transform entities");
    }
    // M-E1b.0：Model/Light 组件逐字段还原断言（D 占 id 4）
    expectTrue(w2.exists(4), "D id preserved");
    expectStr(w2.getName(4), "D", "D name preserved");
    auto* dm2 = w2.getComponent<ModelComponent>(4);
    expectTrue(dm2 != nullptr, "D ModelComponent restored");
    if (dm2) {
        expectTrue(dm2->meshFile == "Suzanne.glb", "meshFile preserved");
        expectVec3(dm2->tint, { 0.8f, 0.4f, 0.2f }, kEps, "tint preserved");
    }
    auto* dl2 = w2.getComponent<LightComponent>(4);
    expectTrue(dl2 != nullptr, "D LightComponent restored");
    if (dl2) {
        expectVec3(dl2->direction, { 0.3f, -0.8f, 0.52f }, kEps, "light direction preserved");
        expectVec3(dl2->color, { 1.0f, 0.95f, 0.9f }, kEps, "light color preserved");
        expectNear(dl2->intensity, 2.5f, kEps, "light intensity preserved");
    }
    // M-E1b.2：Point Light 新字段逐字段还原（E 占 id 5）；D 仍为方向灯且不带新键
    auto* el2 = w2.getComponent<LightComponent>(5);
    expectTrue(el2 != nullptr, "E LightComponent restored");
    if (el2) {
        expectTrue(el2->type == 1, "point light type preserved");
        expectVec3(el2->position, { -2.5f, 1.5f, 3.0f }, kEps, "point light position preserved");
        expectNear(el2->range, 9.0f, kEps, "point light range preserved");
        expectVec3(el2->color, { 0.3f, 0.8f, 1.0f }, kEps, "point light color preserved");
        expectNear(el2->intensity, 8.0f, kEps, "point light intensity preserved");
    }
    if (dl2) expectTrue(dl2->type == 0, "D stays directional (backward compat)");
    // nextID 保留：新实体 ID 接续（6 = A/B/C/D/E 五实体后的 nextID）
    Entity e4 = w2.newEntity("E4");
    expectId(e4.getID(), 6, "nextID preserved (no id reuse)");
}

void testDeserialiseExplicitReject() {
    // 未知组件名
    {
        World w;
        registerLibraries(w);
        std::string err;
        nlohmann::json doc = nlohmann::json::parse(
            "{\"version\":1,\"nextID\":2,\"entities\":[{\"id\":1,\"name\":\"e\","
            "\"parent\":0,\"components\":{\"Bogus\":{}}}],\"systems\":[]}");
        expectTrue(!w.deserialise(doc, err), "unknown component rejected");
        expectTrue(err.find("unknown component") != std::string::npos, "err names unknown component");
    }
    // 未知父引用
    {
        World w;
        registerLibraries(w);
        std::string err;
        nlohmann::json doc = nlohmann::json::parse(
            "{\"version\":1,\"nextID\":2,\"entities\":[{\"id\":1,\"name\":\"e\","
            "\"parent\":99,\"components\":{}}],\"systems\":[]}");
        expectTrue(!w.deserialise(doc, err), "unknown parent rejected");
        expectTrue(err.find("unknown parent") != std::string::npos, "err names unknown parent");
    }
    // 未知系统名
    {
        World w;
        registerLibraries(w);
        std::string err;
        nlohmann::json doc = nlohmann::json::parse(
            "{\"version\":1,\"nextID\":2,\"entities\":[{\"id\":1,\"name\":\"e\","
            "\"parent\":0,\"components\":{}}],\"systems\":[{\"name\":\"Bogus\",\"data\":{}}]}");
        expectTrue(!w.deserialise(doc, err), "unknown system rejected");
        expectTrue(err.find("unknown system") != std::string::npos, "err names unknown system");
    }
    // 错误版本号 / 非法结构
    {
        World w;
        registerLibraries(w);
        std::string err;
        nlohmann::json doc = nlohmann::json::parse("{\"version\":99,\"nextID\":1,\"entities\":[]}");
        expectTrue(!w.deserialise(doc, err), "bad version rejected");
        expectTrue(err.find("version") != std::string::npos, "err names version problem");
    }
    // 重复实体 ID（显式报错而非 assert——deserialise 数据不可信场景）
    {
        World w;
        registerLibraries(w);
        std::string err;
        nlohmann::json doc = nlohmann::json::parse(
            "{\"version\":1,\"nextID\":3,\"entities\":[{\"id\":1,\"name\":\"a\","
            "\"parent\":0,\"components\":{}},{\"id\":1,\"name\":\"b\","
            "\"parent\":0,\"components\":{}}],\"systems\":[]}");
        expectTrue(!w.deserialise(doc, err), "duplicate id rejected");
        expectTrue(err.find("duplicate") != std::string::npos, "err names duplicate id");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// M-E1b.5：并行 tick（读写集求解 + jobgraph 静态 DAG + 结构变更守卫）
// 口径：UE Mass 显式 AddRequirement(ReadOnly/ReadWrite) + 依赖求解器；
// bevy access.is_compatible 冲突判据（显式声明代替反射）；Carrot 串行序作
// 冲突边方向锚（注册序）。构造上无环；确定性 = 同配置逐位一致。
// ---------------------------------------------------------------------------

namespace {

using nl::scene::ComponentTypeID;
using nl::scene::ParallelTickViolation;
using nl::scene::Read;
using nl::scene::SignedSystem;
using nl::scene::Write;

// 写 Transform：position.x = float(base + ticks)（整数值浮点，位精确）。
class ParWriterT : public SignedSystem<Write<TransformComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParWriterT";
    const char* getName() const override { return kName; }
    void tick(double) override {
        ++ticks;
        forEachEntity([&](Entity&, TransformComponent& tc) {
            tc.local.position.x = static_cast<float>(base + ticks);
        });
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        auto c = std::make_unique<ParWriterT>(o);
        c->ticks = ticks;
        return c;
    }
    nlohmann::json serialise() const override { return {{"ticks", ticks}}; }
    void deserialise(const nlohmann::json& j) override {
        ticks = j.at("ticks").get<int>();
    }
    int base = 0;
    int ticks = 0;
};

// 只读 Transform：把每个实体的 x 观测值记进自家列表（自足状态）。
class ParReaderT : public SignedSystem<Read<TransformComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParReaderT";
    const char* getName() const override { return kName; }
    void tick(double) override {
        ++ticks;
        forEachEntity([&](Entity&, TransformComponent& tc) {
            observed.push_back(static_cast<int>(tc.local.position.x));
        });
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        auto c = std::make_unique<ParReaderT>(o);
        c->ticks = ticks;
        c->observed = observed;
        return c;
    }
    nlohmann::json serialise() const override {
        return {{"ticks", ticks}, {"n", observed.size()}};
    }
    void deserialise(const nlohmann::json& j) override {
        ticks = j.at("ticks").get<int>();
        observed.resize(j.at("n").get<size_t>());
    }
    int ticks = 0;
    std::vector<int> observed;
};

// 写 Model（M-E2 裸类型风格 = 保守写）：tint = {x,x,x}（读 Transform 的链式写）。
class ParChainB : public SignedSystem<Read<TransformComponent>, Write<ModelComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParChainB";
    const char* getName() const override { return kName; }
    void tick(double) override {
        ++ticks;
        forEachEntity([&](Entity&, TransformComponent& tc, ModelComponent& mc) {
            const float x = tc.local.position.x;
            mc.tint = { x, x, x };
        });
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        auto c = std::make_unique<ParChainB>(o);
        c->ticks = ticks;
        return c;
    }
    nlohmann::json serialise() const override { return {{"ticks", ticks}}; }
    void deserialise(const nlohmann::json& j) override {
        ticks = j.at("ticks").get<int>();
    }
    int ticks = 0;
};

// 写 Light（与 T/M 链独立——并行收益锚的第三方）。
class ParLightC : public SignedSystem<Write<LightComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParLightC";
    const char* getName() const override { return kName; }
    void tick(double) override {
        ++ticks;
        forEachEntity([&](Entity&, LightComponent& lc) {
            lc.intensity = static_cast<float>(100 + ticks);
        });
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        auto c = std::make_unique<ParLightC>(o);
        c->ticks = ticks;
        return c;
    }
    nlohmann::json serialise() const override { return {{"ticks", ticks}}; }
    void deserialise(const nlohmann::json& j) override {
        ticks = j.at("ticks").get<int>();
    }
    int ticks = 0;
};

// 裸类型保守写（M-E2 向后兼容锚）：SignedSystem<ModelComponent> 不标 Write
// 也必须被当成写者。
class ParBareWriterM : public SignedSystem<ModelComponent> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParBareWriterM";
    const char* getName() const override { return kName; }
    void tick(double) override {
        ++ticks;
        forEachEntity([&](Entity&, ModelComponent& mc) {
            mc.roughness = static_cast<float>(ticks);
        });
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        auto c = std::make_unique<ParBareWriterM>(o);
        c->ticks = ticks;
        return c;
    }
    nlohmann::json serialise() const override { return {{"ticks", ticks}}; }
    void deserialise(const nlohmann::json& j) override {
        ticks = j.at("ticks").get<int>();
    }
    int ticks = 0;
};

// 只读 Model（与 ParBareWriterM 冲突——验证裸类型保守串行）。
class ParReaderM : public SignedSystem<Read<ModelComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParReaderM";
    const char* getName() const override { return kName; }
    void tick(double) override {
        ++ticks;
        forEachEntity([&](Entity&, ModelComponent& mc) {
            observed.push_back(static_cast<int>(mc.roughness));
        });
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        auto c = std::make_unique<ParReaderM>(o);
        c->ticks = ticks;
        c->observed = observed;
        return c;
    }
    nlohmann::json serialise() const override { return {{"ticks", ticks}}; }
    void deserialise(const nlohmann::json& j) override {
        ticks = j.at("ticks").get<int>();
    }
    int ticks = 0;
    std::vector<int> observed;
};

// 确定性对拍场景：3 实体，T+M+L 全套（链 A(Write T) -> B(R T, W M)，C(W L) 独立）。
void buildParityScene(World& w) {
    for (int i = 0; i < 3; ++i) {
        Entity e = w.newEntity("p" + std::to_string(i));
        auto* tc = static_cast<TransformComponent*>(
            &e.addComponent(std::make_unique<TransformComponent>(e)));
        tc->local.position = { static_cast<float>(i), 0.0f, 0.0f };
        auto* mc = static_cast<ModelComponent*>(
            &e.addComponent(std::make_unique<ModelComponent>(e)));
        (void)mc;
        auto* lc = static_cast<LightComponent*>(
            &e.addComponent(std::make_unique<LightComponent>(e)));
        (void)lc;
    }
    w.tick(0.0); // flush
}

// 并行重叠证明：两个独立系统互相等对方置位（自旋 + 2s 死线）。
// 真并行 -> 双双快速通过；错误串行 -> 先跑者死线超时 -> 断言 FAIL。
std::atomic<bool> g_spinX{false};
std::atomic<bool> g_spinY{false};

class ParSpinX : public SignedSystem<Write<TransformComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParSpinX";
    const char* getName() const override { return kName; }
    void tick(double) override {
        g_spinX.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!g_spinY.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        sawPeer = g_spinY.load();
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        return std::make_unique<ParSpinX>(o);
    }
    bool sawPeer = false;
};

class ParSpinY : public SignedSystem<Write<ModelComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParSpinY";
    const char* getName() const override { return kName; }
    void tick(double) override {
        g_spinY.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!g_spinX.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        sawPeer = g_spinX.load();
    }
    std::unique_ptr<System> duplicate(World& o) const override {
        return std::make_unique<ParSpinY>(o);
    }
    bool sawPeer = false;
};

// 违约系统：并行 tick 内做结构变更（必须显式失败，不是静默竞争）。
class ParBadStructural : public SignedSystem<Read<TransformComponent>> {
public:
    using SignedSystem::SignedSystem;
    static constexpr const char* kName = "ParBadStructural";
    const char* getName() const override { return kName; }
    void tick(double) override { getWorld().newEntity("bad"); }
    std::unique_ptr<System> duplicate(World& o) const override {
        return std::make_unique<ParBadStructural>(o);
    }
};

// ---- 用例 ----

// 1. 读写集求解正确性：写者先于读者（冲突边）+ 图形状（边数）确定。
void testParallelDependencyOrder() {
    World w;
    registerLibraries(w);
    buildParityScene(w);
    auto writer = std::make_unique<ParWriterT>(w);
    auto readerA = std::make_unique<ParReaderT>(w);
    auto readerB = std::make_unique<ParReaderT>(w);
    ParWriterT* pW = writer.get();
    ParReaderT* pA = readerA.get();
    ParReaderT* pB = readerB.get();
    w.registerSystem(std::move(writer));
    w.registerSystem(std::move(readerA));
    w.registerSystem(std::move(readerB));

    std::string dot;
    std::string err;
    expectTrue(w.tickParallel(0.016, 4, err, &dot), "tickParallel ok");
    if (!dot.empty()) {
        // 3 节点：写 T + 读 T×2 -> 2 条冲突边（写->读两条；读读无边）。
        size_t edges = 0;
        for (size_t pos = dot.find("->"); pos != std::string::npos;
             pos = dot.find("->", pos + 1))
            ++edges;
        expectSize(edges, 2, "solver builds exactly 2 edges (writer -> 2 readers)");
        expectTrue(dot.find("digraph") != std::string::npos, "dotOut is DOT text");
    }
    expectTrue(pA->observed.size() == 3 && pB->observed.size() == 3,
               "readers observed all 3 entities");
    for (int v : pA->observed)
        expectTrue(v == pW->base + pW->ticks, "readerA saw post-write value (writer ordered first)");
    for (int v : pB->observed)
        expectTrue(v == pW->base + pW->ticks, "readerB saw post-write value");

    // 求解确定性：再跑一次 dot 逐字节一致。
    std::string dot2;
    w.tickParallel(0.016, 4, err, &dot2);
    expectTrue(dot == dot2, "solver deterministic (dot byte-identical across runs)");
}

// 2. 真并行证明：两个无冲突系统互相等对方置位，双双快速通过。
void testParallelTrueOverlap() {
    World w;
    registerLibraries(w);
    buildParityScene(w);
    auto sx = std::make_unique<ParSpinX>(w);
    auto sy = std::make_unique<ParSpinY>(w);
    ParSpinX* pX = sx.get();
    ParSpinY* pY = sy.get();
    w.registerSystem(std::move(sx));
    w.registerSystem(std::move(sy));
    std::string err;
    expectTrue(w.tickParallel(0.016, 4, err), "tickParallel ok");
    expectTrue(pX->sawPeer, "spinX saw spinY (true parallel overlap, not serialized)");
    expectTrue(pY->sawPeer, "spinY saw spinX");
}

// 3. 确定性对拍：并行 tick 与串行 tick 结果逐位一致（JSON dump 相等）。
void testParallelSerialParity() {
    World ws;   // 串行
    registerLibraries(ws);
    buildParityScene(ws);
    ws.registerSystem(std::make_unique<ParWriterT>(ws));
    ws.registerSystem(std::make_unique<ParChainB>(ws));
    ws.registerSystem(std::make_unique<ParLightC>(ws));

    World wp;   // 并行
    registerLibraries(wp);
    buildParityScene(wp);
    wp.registerSystem(std::make_unique<ParWriterT>(wp));
    wp.registerSystem(std::make_unique<ParChainB>(wp));
    wp.registerSystem(std::make_unique<ParLightC>(wp));

    for (int i = 0; i < 3; ++i) {
        ws.tick(0.016);
        std::string err;
        expectTrue(wp.tickParallel(0.016, 4, err), "parallel frame ok");
    }
    expectTrue(ws.serialise().dump() == wp.serialise().dump(),
               "parallel == serial bitwise (3 frames, chained systems)");
}

// 4. 裸类型保守写：SignedSystem<ModelComponent>（不标 Write）与只读者串行。
void testParallelBareTypeConservative() {
    World w;
    registerLibraries(w);
    buildParityScene(w);
    auto bw = std::make_unique<ParBareWriterM>(w);
    auto rm = std::make_unique<ParReaderM>(w);
    ParBareWriterM* pBW = bw.get();
    ParReaderM* pRM = rm.get();
    w.registerSystem(std::move(bw));   // 注册序 = 冲突边方向锚
    w.registerSystem(std::move(rm));
    std::string err;
    expectTrue(w.tickParallel(0.016, 4, err), "tickParallel ok");
    expectSize(pRM->observed.size(), 3, "readerM collected entities");
    for (int v : pRM->observed)
        expectTrue(v == pBW->ticks,
                   "bare-type writer ordered before reader (conservative Write)");
}

// 5. 结构变更守卫：并行 tick 内 newEntity = 显式违约（false + err 可定位），
//    违约后 World 可正常继续用（守卫是逐次检查不是状态污染）。
void testParallelStructuralViolation() {
    World w;
    registerLibraries(w);
    buildParityScene(w);
    w.registerSystem(std::make_unique<ParBadStructural>(w));
    std::string err;
    expectTrue(!w.tickParallel(0.016, 4, err), "violation -> tickParallel false");
    expectTrue(err.find("forbidden") != std::string::npos,
               "err names the contract violation");
    expectTrue(err.find("newEntity") != std::string::npos,
               "err names the offending API");
    // 守卫复位：串行 tick 不受影响（违约系统串行下跑 newEntity 合法；
    // 延迟语义 = 新实体下一次 flush 才进全局表，所以跑两个 tick）。
    w.tick(0.016);
    w.tick(0.016);
    expectTrue(w.getAllEntities().size() == 4, "world usable after violation (serial tick adds 1)");
}

// 6. firstTick 生命周期：tickParallel 里注册后首个 tick 恰一次（串行段跑）。
void testParallelFirstTickOnce() {
    World w;
    registerLibraries(w);
    buildParityScene(w);
    auto cs = std::make_unique<CounterSystem>(w);
    CounterSystem* p = cs.get();
    w.registerSystem(std::move(cs));
    std::string err;
    expectTrue(w.tickParallel(0.016, 4, err), "tickParallel ok");
    expectId(static_cast<nl::scene::EntityID>(p->firstTicks), 1, "firstTick exactly once");
    expectId(static_cast<nl::scene::EntityID>(p->ticks), 1, "tick once per tickParallel");
}

// parallel-dot：固定演示场景（1 写者 + 2 只读者 + 1 独立写者）跑一次
// tickParallel，stdout 打印求解图 DOT（真实求解器输出，证据图数据源不手绘）。
// 拓扑：ParWriterT -(写 T)-> ParReaderT×2；ParLightC（写 L）独立无边。
int runParallelDot() {
    World w;
    registerLibraries(w);
    buildParityScene(w);
    w.registerSystem(std::make_unique<ParWriterT>(w));
    w.registerSystem(std::make_unique<ParReaderT>(w));
    w.registerSystem(std::make_unique<ParReaderT>(w));
    w.registerSystem(std::make_unique<ParLightC>(w));
    std::string err;
    std::string dot;
    if (!w.tickParallel(0.016, 2, err, &dot)) {
        std::printf("parallel-dot: tickParallel failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s", dot.c_str());
    return 0;
}

int runSuiteEcsParallel() {
    testParallelDependencyOrder();
    testParallelTrueOverlap();
    testParallelSerialParity();
    testParallelBareTypeConservative();
    testParallelStructuralViolation();
    testParallelFirstTickOnce();

    if (g_failures != 0) {
        std::printf("ecs-parallel selftest: FAIL (%d assertion(s))\n", g_failures);
        return 1;
    }
    std::printf("ecs-parallel selftest: PASS\n");
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------

namespace {

int runSuiteEcsCore() {
    testCreateIncrementIds();
    testDeferredAddRemoveFlush();
    testNames();
    testFlagsHidden();

    if (g_failures != 0) {
        std::printf("ecs-core selftest: FAIL (%d assertion(s))\n", g_failures);
        return 1;
    }
    std::printf("ecs-core selftest: PASS\n");
    return 0;
}

int runSuiteEcsHier() {
    testHierarchyWorldMatrix();
    testReparentKeepsWorldTransform();
    testRemoveParentRecursesChildren();
    testDuplicateDeepCopy();
    testComponentLibrary();

    if (g_failures != 0) {
        std::printf("ecs-hier selftest: FAIL (%d assertion(s))\n", g_failures);
        return 1;
    }
    std::printf("ecs-hier selftest: PASS\n");
    return 0;
}

int runSuiteEcsSystem() {
    testSystemSignatureMatching();

    if (g_failures != 0) {
        std::printf("ecs-system selftest: FAIL (%d assertion(s))\n", g_failures);
        return 1;
    }
    std::printf("ecs-system selftest: PASS\n");
    return 0;
}

int runSuiteEcsSerial() {
    testRoundtripByteIdentical();
    testDeserialiseExplicitReject();

    if (g_failures != 0) {
        std::printf("ecs-serial selftest: FAIL (%d assertion(s))\n", g_failures);
        return 1;
    }
    std::printf("ecs-serial selftest: PASS\n");
    return 0;
}

// deserialise-check <good|badjson|unknown>：负向判据显式退出码 2 的工具入口。
//   good    = 内置合法场景 -> deserialise 成功 -> exit 0
//   badjson = 非法 JSON 文本 -> parse 失败 -> stderr 报错 -> exit 2
//   unknown = 合法结构但组件名未注册 -> deserialise 拒绝 -> stderr 报错 -> exit 2
int runDeserialiseCheck(const std::string& mode) {
    const char* kGoodScene =
        "{\"version\":1,\"nextID\":2,\"entities\":[{\"id\":1,\"name\":\"e\","
        "\"parent\":0,\"components\":{\"Transform\":{\"position\":[1,2,3],"
        "\"scale\":[1,1,1],\"rotation\":[0,0,0,1]}}}],\"systems\":[]}";

    World w;
    registerLibraries(w);
    if (mode == "good") {
        std::string err;
        if (!w.deserialise(nlohmann::json::parse(kGoodScene), err)) {
            std::printf("deserialise-check: unexpected failure: %s\n", err.c_str());
            return 1;
        }
        auto* tc = w.getComponent<TransformComponent>(1);
        if (!tc || tc->local.position.x != 1.0f || tc->local.position.z != 3.0f) {
            std::printf("deserialise-check: good scene content mismatch\n");
            return 1;
        }
        std::printf("deserialise-check good: PASS\n");
        return 0;
    }
    if (mode == "badjson") {
        try {
            nlohmann::json::parse("{ this is not json ]");
            std::printf("deserialise-check: bad json unexpectedly parsed\n");
            return 1;
        } catch (const nlohmann::json::exception& ex) {
            std::fprintf(stderr, "deserialise-check badjson: rejected (%s)\n", ex.what());
            return 2; // 显式退出码 2（M-E2 负向判据）
        }
    }
    if (mode == "unknown") {
        nlohmann::json doc = nlohmann::json::parse(
            "{\"version\":1,\"nextID\":2,\"entities\":[{\"id\":1,\"name\":\"e\","
            "\"parent\":0,\"components\":{\"Bogus\":{}}}],\"systems\":[]}");
        std::string err;
        if (w.deserialise(doc, err)) {
            std::printf("deserialise-check: unknown component unexpectedly accepted\n");
            return 1;
        }
        std::fprintf(stderr, "deserialise-check unknown: rejected (%s)\n", err.c_str());
        return 2; // 显式退出码 2（M-E2 负向判据）
    }
    std::printf("unknown deserialise-check mode: %s\n", mode.c_str());
    return 2;
}

void printUsage() {
    std::printf("usage:\n"
                "  nanite-scene selftest --suite <name|all> [--verbose]\n"
                "  nanite-scene deserialise-check <good|badjson|unknown>\n"
                "  nanite-scene parallel-dot              M-E1b.5 solver graph DOT\n"
                "suites:\n"
                "  ecs-core    M-E0 World/Entity core (deferred add/remove, names, Hidden flags)\n"
                "  ecs-hier    M-E1 components + hierarchy (world-matrix anchors, reparent, duplicate)\n"
                "  ecs-system  M-E2 systems (signature matching, auto list, firstTick/tick)\n"
                "  ecs-serial  M-E2 scene roundtrip (byte-identical, explicit rejections)\n"
                "  ecs-parallel M-E1b.5 parallel tick (read/write solver, overlap proof,\n"
                "              serial parity, structural-change guard)\n"
                "  all         run every suite in order\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "parallel-dot") {
        return runParallelDot();
    }
    if (cmd == "deserialise-check") {
        if (argc < 3) {
            std::printf("deserialise-check requires <good|badjson|unknown>\n");
            printUsage();
            return 2;
        }
        return runDeserialiseCheck(argv[2]);
    }
    if (cmd != "selftest") {
        std::printf("unknown command: %s\n", cmd.c_str());
        printUsage();
        return 2;
    }
    // selftest --suite <name> [--verbose]
    std::setvbuf(stdout, nullptr, _IONBF, 0); // 临时插桩：崩溃前输出不丢（定位后移除）
    std::string suite;
    bool haveSuite = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--suite" && i + 1 < argc) {
            suite = argv[i + 1];
            haveSuite = true;
            ++i;
        } else if (std::string(argv[i]) == "--all") {
            // M-E1b.0：裸 --all 等价 --suite all（验收脚本口径）
            suite = "all";
            haveSuite = true;
        } else if (std::string(argv[i]) == "--verbose") {
            g_verbose = true;
        }
    }
    if (!haveSuite) {
        std::printf("selftest requires --suite <name>\n");
        printUsage();
        return 2;
    }
    if (suite == "ecs-core") {
        return runSuiteEcsCore();
    }
    if (suite == "ecs-hier") {
        return runSuiteEcsHier();
    }
    if (suite == "ecs-system") {
        return runSuiteEcsSystem();
    }
    if (suite == "ecs-serial") {
        return runSuiteEcsSerial();
    }
    if (suite == "ecs-parallel") {
        return runSuiteEcsParallel();
    }
    // M-E1b.0：--suite all 顺序跑全部四个 suite（每个 suite 前清零 g_failures，
    // 任一 FAIL 退出码 1——build/验收脚本用 `nanite-scene.exe selftest --all`）。
    if (suite == "all") {
        bool anyFail = false;
        const int rc1 = (g_failures = 0, runSuiteEcsCore());
        if (rc1 != 0) anyFail = true;
        const int rc2 = (g_failures = 0, runSuiteEcsHier());
        if (rc2 != 0) anyFail = true;
        const int rc3 = (g_failures = 0, runSuiteEcsSystem());
        if (rc3 != 0) anyFail = true;
        const int rc4 = (g_failures = 0, runSuiteEcsSerial());
        if (rc4 != 0) anyFail = true;
        const int rc5 = (g_failures = 0, runSuiteEcsParallel());
        if (rc5 != 0) anyFail = true;
        return anyFail ? 1 : 0;
    }
    std::printf("unknown selftest suite: %s\n", suite.c_str());
    printUsage();
    return 2;
}
