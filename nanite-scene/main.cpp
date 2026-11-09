// nanite-scene —— ECS-lite 场景（nl/scene）自测工具，A 类并行轨道。
// 只依赖 core（nl/scene），零图形 API 符号；接线仿 anim-tools 先例。
//
// 退出码约定（与 nanite-tools / 展示台 expectExit 0/1/2 对齐）：
//   0 = 用例全过（打印 "<suite> selftest: PASS"）
//   1 = 断言失败（打印 "<suite> selftest: FAIL <detail>"）
//   2 = 用法错误（未知子命令 / 缺 --suite / 未知 suite 名）
//
// 用例（docs/p8/P8-ECS-PLAN.md M-E0/M-E1 判据）：
//   ecs-core      World/Entity 核心：递增 ID 创建 / 增删 flush 前后存在性
//                 （Carrot 口径：新建即时可见、删除 flush 后不可见）/
//                 名字读写 / Flags(Hidden) 过滤 / 延迟语义锚点
//                 （删实体下一 tick 生效；M-E2 有 system 后补 tick 内删除锚点）
//   ecs-hier      M-E1 组件+层级：三层父子链复合矩阵数值锚（手算）/ reparent
//                 保世界变换 / 删父递归删子 / duplicate 深拷 / 组件库工厂

#include "nl/scene/Component.hpp"
#include "nl/scene/World.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool ok, const char* what) {
    if (ok) return;
    std::printf("  FAIL %s\n", what);
    ++g_failures;
}

void expectSize(size_t got, size_t want, const char* what) {
    if (got == want) return;
    std::printf("  FAIL %s: got %zu want %zu\n", what, got, want);
    ++g_failures;
}

void expectId(nl::scene::EntityID got, nl::scene::EntityID want, const char* what) {
    if (got == want) return;
    std::printf("  FAIL %s: got %llu want %llu\n", what,
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
    ++g_failures;
}

void expectStr(const std::string& got, const char* want, const char* what) {
    if (got == want) return;
    std::printf("  FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want);
    ++g_failures;
}

void expectNear(float got, float want, float eps, const char* what) {
    if (std::fabs(got - want) <= eps) return;
    std::printf("  FAIL %s: got %.6f want %.6f (eps %g)\n", what, got, want, eps);
    ++g_failures;
}

void expectVec3(nl::math::Vec3 got, nl::math::Vec3 want, float eps, const char* what) {
    if (std::fabs(got.x - want.x) <= eps && std::fabs(got.y - want.y) <= eps &&
        std::fabs(got.z - want.z) <= eps) return;
    std::printf("  FAIL %s: got (%.6f, %.6f, %.6f) want (%.6f, %.6f, %.6f)\n",
                what, got.x, got.y, got.z, want.x, want.y, want.z);
    ++g_failures;
}

void expectQuat(nl::math::Quat got, nl::math::Quat want, float eps, const char* what) {
    // 双覆盖对齐：dot < 0 时取反再比（同一旋转的另一半）。
    if (nl::math::dotQuat(got, want) < 0.0f) {
        got = { -got.x, -got.y, -got.z, -got.w };
    }
    expectNear(got.x, want.x, eps, what); expectNear(got.y, want.y, eps, what);
    expectNear(got.z, want.z, eps, what); expectNear(got.w, want.w, eps, what);
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
    expectId(b.getID(), 2, "second id == 2 (递增)");
    expectTrue(a.exists(), "new entity exists immediately (名字表即时登记)");
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
    expectTrue(a.exists(), "exists still true right after remove (延迟)");
    expectSize(w.getAllEntities().size(), 2, "list unchanged before flush");

    w.tick(1.0 / 60.0);
    expectTrue(!a.exists(), "exists false after next tick (延迟语义锚点)");
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
    expectStr(b.getName(), "<unnamed>", "default name (Carrot 口径)");

    // 名字表即时登记，但 findEntityByName 扫全局实体表（flush 后才有）——Carrot 口径
    expectTrue(!w.findEntityByName("root").has_value(),
               "findEntityByName before flush misses (实体表延迟)");
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
    expectTrue(a.exists(), "hidden entity still exists (隐藏不是删除)");
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
               "transform-less parent passes local through (Carrot 断链口径)");
}

// ---------------------------------------------------------------------------
// 删父递归删子（延迟语义：flush 后子孙全不存在）
// ---------------------------------------------------------------------------

void testRemoveParentRecursesChildren() {
    World w;
    Entity a, b, c;
    buildChain(w, a, b, c);

    b.remove(); // 删 B（C 是 B 的子）
    expectTrue(b.exists() && c.exists(), "still exists right after remove (延迟)");

    w.tick(0.0); // flush
    expectTrue(!b.exists(), "B removed after flush");
    expectTrue(!c.exists(), "C recursively removed with parent (父删递归删子锚点)");
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
    expectStr(dup.getName(), "B (Copy)", "duplicate name suffix (Carrot 口径)");
    expectTrue(dup.getParentID() == a.getID(), "copy inherits source parent");

    // 子孙递归：副本应有 1 个直接子 "C (Copy)"
    auto kids = dup.getChildren();
    expectSize(kids.size(), 1, "copy has 1 child (递归深拷)");
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
               "source unaffected by copy mutation (深拷独立)");
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
        expectTrue(lib.create("Nope", e) == nullptr, "create unknown -> nullptr (M-E2 负向铺垫)");
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

void printUsage() {
    std::printf("usage:\n"
                "  nanite-scene selftest --suite <name>\n"
                "suites:\n"
                "  ecs-core    M-E0 World/Entity core (deferred add/remove, names, Hidden flags)\n"
                "  ecs-hier    M-E1 components + hierarchy (world-matrix anchors, reparent, duplicate)\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd != "selftest") {
        std::printf("unknown command: %s\n", cmd.c_str());
        printUsage();
        return 2;
    }
    // selftest --suite <name>
    std::string suite;
    bool haveSuite = false;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--suite") {
            suite = argv[i + 1];
            haveSuite = true;
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
    std::printf("unknown selftest suite: %s\n", suite.c_str());
    printUsage();
    return 2;
}
