// M-E0：nanite-scene —— ECS-lite 场景（nl/scene）自测工具，A 类并行轨道。
// 只依赖 core（nl/scene），零图形 API 符号；接线仿 anim-tools 先例。
//
// 退出码约定（与 nanite-tools / 展示台 expectExit 0/1/2 对齐）：
//   0 = 用例全过（打印 "ecs-core selftest: PASS"）
//   1 = 断言失败（打印 "ecs-core selftest: FAIL <detail>"）
//   2 = 用法错误（未知子命令 / 缺 --suite / 未知 suite 名）
//
// 用例（docs/p8/P8-ECS-PLAN.md M-E0 判据）：
//   ecs-core      World/Entity 核心：递增 ID 创建 / 增删 flush 前后存在性
//                 （Carrot 口径：新建即时可见、删除 flush 后不可见）/
//                 名字读写 / Flags(Hidden) 过滤 / 延迟语义锚点
//                 （删实体下一 tick 生效；M-E2 有 system 后补 tick 内删除锚点）

#include "nl/scene/World.hpp"

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

void printUsage() {
    std::printf("usage:\n"
                "  nanite-scene selftest --suite <name>\n"
                "suites:\n"
                "  ecs-core    M-E0 World/Entity core (deferred add/remove, names, Hidden flags)\n");
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
    std::printf("unknown selftest suite: %s\n", suite.c_str());
    printUsage();
    return 2;
}
