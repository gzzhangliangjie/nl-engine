// M-SC1：console 绑定面实现。口径/ABI/内存所有权见 nl/script/Bindings.hpp 文件头。
//
// 落点：nl_core（core/console 与 core/script 同库，绑定胶水不引新 lib）。
// extern "C" 函数只被 C# 经函数指针调用，不在任何 C++ 头暴露（ApiTable 是唯一入口）。

#include "nl/script/Bindings.hpp"

#include "nl/core/console.hpp"
#include "nl/scene/Component.hpp"
#include "nl/scene/World.hpp"

#include <objbase.h>
#include <string>
#include <vector>

#include <windows.h>

namespace nl::script {

namespace {

// ---- C++ → C# 返回值静态缓冲（下一次调用前有效；console 单线程约定下无竞态）----

std::string& ownedBuffer() {
    static std::string s;
    return s;
}

// ---- 托管命令回调 ABI（C# 侧实现）----
// int fn(int argc, const char** argv, const char** outText) -> 0=ok / 1=失败
using ManagedCommandFn = int(__cdecl*)(int, const char**, const char**);

// SEH guard：托管异常在互操作边界捕获（__try 函数体禁 C++ 对象，独立纯 POD 函数）。
int guardedCommandCall(ManagedCommandFn fn, int argc, const char** argv,
                       const char** outText, std::uint32_t* exceptionCode) {
    __try {
        *outText = nullptr;   // 先清：托管侧若中途炸，C++ 侧不读到野指针
        *exceptionCode = 0;
        return fn(argc, argv, outText);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionCode = static_cast<std::uint32_t>(GetExceptionCode());
        return -1;   // 哨兵：与托管侧 0/1 区分
    }
}

} // namespace

// ---- extern "C" 绑定函数（只经 ApiTable 函数指针被 C# 调用）----

namespace {

using nl::core::console::Console;
using nl::core::console::CVar;

extern "C" int nl_cvarGet(const char* name, const char** outValue) {
    if (!name || !outValue) return 0;
    *outValue = nullptr;
    CVar* cv = Console::get().findCVar(name);
    if (!cv) return 0;
    ownedBuffer() = cv->value().toString();
    *outValue = ownedBuffer().c_str();
    // 类型码与 CVarType 枚举对齐（Bool=1 起，0 留给"未注册"）
    return static_cast<int>(cv->type()) + 1;
}

extern "C" int nl_cvarSet(const char* name, const char* value) {
    if (!name || !value) return 2;
    CVar* cv = Console::get().findCVar(name);
    if (!cv) return 1;
    return cv->setFromString(value) ? 0 : 2;
}

extern "C" const char* nl_cvarList() {
    // 与 installBuiltins 的 cvar.list 同格式（allEntries 全量视图）
    std::string out;
    for (const auto& e : Console::get().allEntries()) {
        if (!e.isCVar) continue;
        const CVar* cv = Console::get().findCVar(e.name);
        out += "\n  " + e.name + " = " + (cv ? cv->value().toString() : std::string("?")) +
               "  (" + (cv ? nl::core::console::cvarTypeName(cv->type()) : "?") + ")  " + e.help;
    }
    ownedBuffer() = std::move(out);
    return ownedBuffer().c_str();
}

extern "C" int nl_consoleExec(const char* line, const char** outOutput) {
    if (!line || !outOutput) return 2;
    const auto r = Console::get().exec(line);
    *outOutput = nullptr;
    ownedBuffer() = r.output;
    // 返回值在缓冲填充后取：exec 内部也可能动 ownedBuffer 吗？不会——本文件是唯一
    // 使用者；但 exec 的输出经 finish 进 console 环形日志，ownedBuffer 只在这里写。
    *outOutput = ownedBuffer().c_str();
    return r.exitCode();
}

extern "C" int nl_consoleRegister(const char* name, const char* help, void* fn) {
    if (!name || !help || !fn) return 1;
    if (Console::get().findCVar(name)) return 1;   // 不许遮蔽 CVar（统一注册表先查 CVar）
    ManagedCommandFn managed = reinterpret_cast<ManagedCommandFn>(fn);
    std::string nameCopy = name;
    Console::get().registerCommand(name, help,
        [managed, nameCopy](const std::vector<std::string>& args) -> nl::core::console::CommandResult {
            // argv 生命周期：C++ 字符串在调用期间存活（lambda 参数持有）
            std::vector<const char*> argv;
            argv.reserve(args.size());
            for (const auto& a : args) argv.push_back(a.c_str());

            const char* text = nullptr;
            std::uint32_t excCode = 0;
            const int rc = guardedCommandCall(managed, static_cast<int>(argv.size()),
                                              argv.data(), &text, &excCode);

            // 托管侧 outText = Marshal.AllocHGlobal（CoTaskMem）→ C++ 拷完 CoTaskMemFree
            std::string textCopy;
            if (text) {
                textCopy = text;
                ::CoTaskMemFree(const_cast<char*>(text));
            }

            if (rc == -1) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%08x", excCode);
                return {false, std::string("managed exception at command boundary: SEH code=0x") + buf};
            }
            return {rc == 0, std::move(textCopy)};
        });
    return 0;
}

extern "C" int nl_consoleUnregister(const char* name) {
    if (!name) return 1;
    return Console::get().unregisterCommand(name) ? 0 : 1;
}

// ---- v2 ECS 导出（M-SC2；操作目标 = ecsSetWorld 绑定的 World）----
// 失效安全拒绝统一前置：g_ecsWorld 未绑 = kEcsNoWorld，exists() 假 = kEcsNoEntity。

scene::World* g_ecsWorld = nullptr;

extern "C" int nl_ecsSpawn(const char* name, std::uint64_t* outID) {
    if (!outID) return kEcsBadArg;
    *outID = 0;
    if (!g_ecsWorld) return kEcsNoWorld;
    const auto e = g_ecsWorld->newEntity(name ? name : "<unnamed>");
    *outID = e.getID();
    return kEcsOk;
}

extern "C" int nl_ecsRemove(std::uint64_t id) {
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    g_ecsWorld->removeEntity(id);   // 延迟语义：flush/tick 生效（Carrot 口径）
    return kEcsOk;
}

extern "C" int nl_ecsFlush() {
    if (!g_ecsWorld) return kEcsNoWorld;
    g_ecsWorld->flushEntityCreationAndRemoval();
    return kEcsOk;
}

extern "C" int nl_ecsExists(std::uint64_t id, int* outExists) {
    if (!outExists) return kEcsBadArg;
    *outExists = 0;
    if (!g_ecsWorld) return kEcsNoWorld;
    *outExists = g_ecsWorld->exists(id) ? 1 : 0;
    return kEcsOk;
}

extern "C" int nl_ecsSetParent(std::uint64_t id, std::uint64_t parentID) {
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    if (parentID != 0 && !g_ecsWorld->exists(parentID)) return kEcsNoEntity;
    g_ecsWorld->setParent(id, parentID == 0 ? std::nullopt : std::optional<scene::EntityID>(parentID));
    return kEcsOk;
}

extern "C" int nl_ecsGetParent(std::uint64_t id, std::uint64_t* outParent) {
    if (!outParent) return kEcsBadArg;
    *outParent = 0;
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    if (const auto p = g_ecsWorld->getParentID(id)) *outParent = *p;
    return kEcsOk;
}

extern "C" int nl_ecsAddTransform(std::uint64_t id) {
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    if (g_ecsWorld->getComponent(id, scene::componentTypeID<scene::TransformComponent>())) {
        return kEcsComponent;   // 重复挂 = 3
    }
    g_ecsWorld->addComponent(id, std::make_unique<scene::TransformComponent>(g_ecsWorld->wrap(id)));
    return kEcsOk;
}

// 局部 TRS 读/写；rot = quat (x,y,z,w)。缺 Transform 组件 = kEcsComponent。
extern "C" int nl_ecsTransformGet(std::uint64_t id, float* outPos3, float* outRot4, float* outScale3) {
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    const auto* tc = g_ecsWorld->getComponent(id, scene::componentTypeID<scene::TransformComponent>());
    if (!tc) return kEcsComponent;
    const scene::Transform& t = static_cast<const scene::TransformComponent*>(tc)->local;
    if (outPos3) {
        outPos3[0] = t.position.x; outPos3[1] = t.position.y; outPos3[2] = t.position.z;
    }
    if (outRot4) {
        outRot4[0] = t.rotation.x; outRot4[1] = t.rotation.y;
        outRot4[2] = t.rotation.z; outRot4[3] = t.rotation.w;
    }
    if (outScale3) {
        outScale3[0] = t.scale.x; outScale3[1] = t.scale.y; outScale3[2] = t.scale.z;
    }
    return kEcsOk;
}

extern "C" int nl_ecsTransformSet(std::uint64_t id, const float* pos3, const float* rot4, const float* scale3) {
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    auto* tc = g_ecsWorld->getComponent(id, scene::componentTypeID<scene::TransformComponent>());
    if (!tc) return kEcsComponent;
    scene::Transform& t = static_cast<scene::TransformComponent*>(tc)->local;
    if (pos3) {
        t.position = {pos3[0], pos3[1], pos3[2]};
    }
    if (rot4) {
        t.rotation = {rot4[0], rot4[1], rot4[2], rot4[3]};
    }
    if (scale3) {
        t.scale = {scale3[0], scale3[1], scale3[2]};
    }
    return kEcsOk;
}

extern "C" int nl_ecsFindByName(const char* name, std::uint64_t* outID) {
    if (!name || !outID) return kEcsBadArg;
    *outID = 0;
    if (!g_ecsWorld) return kEcsNoWorld;
    if (const auto e = g_ecsWorld->findEntityByName(name)) *outID = e->getID();
    return kEcsOk;
}

} // namespace

void ecsSetWorld(scene::World* world) {
    g_ecsWorld = world;
}

const ApiTable* consoleApiTable() {
    static const ApiTable table{
        reinterpret_cast<void*>(&nl_cvarGet),
        reinterpret_cast<void*>(&nl_cvarSet),
        reinterpret_cast<void*>(&nl_cvarList),
        reinterpret_cast<void*>(&nl_consoleExec),
        reinterpret_cast<void*>(&nl_consoleRegister),
        reinterpret_cast<void*>(&nl_consoleUnregister),
        // v2 ECS（M-SC2，尾部追加）
        reinterpret_cast<void*>(&nl_ecsSpawn),
        reinterpret_cast<void*>(&nl_ecsRemove),
        reinterpret_cast<void*>(&nl_ecsFlush),
        reinterpret_cast<void*>(&nl_ecsExists),
        reinterpret_cast<void*>(&nl_ecsSetParent),
        reinterpret_cast<void*>(&nl_ecsGetParent),
        reinterpret_cast<void*>(&nl_ecsAddTransform),
        reinterpret_cast<void*>(&nl_ecsTransformGet),
        reinterpret_cast<void*>(&nl_ecsTransformSet),
        reinterpret_cast<void*>(&nl_ecsFindByName),
    };
    return &table;
}

} // namespace nl::script
