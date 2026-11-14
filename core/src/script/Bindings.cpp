// M-SC1：console 绑定面实现。口径/ABI/内存所有权见 nl/script/Bindings.hpp 文件头。
//
// 落点：nl_core（core/console 与 core/script 同库，绑定胶水不引新 lib）。
// extern "C" 函数只被 C# 经函数指针调用，不在任何 C++ 头暴露（ApiTable 是唯一入口）。

#include "nl/script/Bindings.hpp"

#include "nl/core/console.hpp"
#include "nl/core/game_clock.hpp"
#include "nl/core/input.hpp"
#include "nl/core/action_map.hpp"
#include "nl/core/log.hpp"
#include "nl/physics/World.hpp"     // M-SC8：DebugScene 载具训练场（PIMPL，Jolt 挡在 World.cpp）
#include "nl/scene/AssetName.hpp"   // M-SC7：meshFile 白名单单源
#include "nl/scene/Component.hpp"
#include "nl/scene/World.hpp"

#include <objbase.h>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

namespace nl::script {

namespace {

// M-SC3 脚本命令登记表（Bindings.hpp 口径；set 有序去重，输出稳定）。
// 定义放匿名 namespace 顶部：extern "C" 注册/注销路径与文件尾部的公开查询
// 接口共用同一份存储（一次定义，整 TU 可见）。
std::set<std::string>& scriptCommandTable() {
    static std::set<std::string> names;
    return names;
}

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
    // M-SC3 登记：脚本注册的命令进台账（回调指向 collectible ALC thunk，
    // 热重载 unload 前必须 scriptUnregisterAll 清空——thunk 卸载后悬空）。
    scriptCommandTable().erase(nameCopy);
    scriptCommandTable().insert(nameCopy);
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
    // M-SC3 对称擦除：C# 主动注销的命令同步出登记表。
    scriptCommandTable().erase(name);
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

// ---- v5 ECS 模型挂载（M-SC7；拍板方案 A：binding 只做组件增删 + 白名单校验，
// viewer 帧边界比对 Model 集合签名重建 scene batch——本层不感知渲染）。----
// 防崩语义（拍板）：无 Transform = kEcsNoTransform 拒绝（不隐式建表，E1b 口径）；
// 已有 Model 再挂 = kEcsComponent 拒绝（World::addComponent 同类型重复是 assert，
// binding 必须先挡——直接调 = 崩进程）。meshFile 白名单拒绝 = kEcsBadAsset
//（坏名不落 World，防路径穿越——nl::scene::validGlbAssetName 单源）。

extern "C" int nl_ecsAttachModel(std::uint64_t id, const char* meshFile) {
    if (!meshFile || !*meshFile) return kEcsBadArg;
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    if (!scene::validGlbAssetName(meshFile)) return kEcsBadAsset;
    if (!g_ecsWorld->getComponent(id, scene::componentTypeID<scene::TransformComponent>()))
        return kEcsNoTransform;
    if (g_ecsWorld->getComponent(id, scene::componentTypeID<scene::ModelComponent>()))
        return kEcsComponent;
    auto mc = std::make_unique<scene::ModelComponent>(g_ecsWorld->wrap(id));
    mc->meshFile = meshFile;
    g_ecsWorld->addComponent(id, std::move(mc));
    return kEcsOk;
}

extern "C" int nl_ecsDetachModel(std::uint64_t id) {
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    if (!g_ecsWorld->getComponent(id, scene::componentTypeID<scene::ModelComponent>()))
        return kEcsComponent;
    g_ecsWorld->removeComponent(id, scene::componentTypeID<scene::ModelComponent>());
    return kEcsOk;
}

// ModelComponent 字段读回。meshFile = 静态缓冲账本（out str 语义：「下次同线程
// 调用前有效」，C# 立即拷贝——ownedBuffer 复用 M-SC5 logSetFilter 落点）。
// tint/roughness/metallic 指针可 NULL 跳过；meshFile 指针必填（kEcsBadArg）。
extern "C" int nl_ecsModelInfo(std::uint64_t id, const char** outMeshFile,
                               float* outTint3, float* outRoughness, float* outMetallic) {
    if (!outMeshFile) return kEcsBadArg;
    *outMeshFile = nullptr;
    if (!g_ecsWorld) return kEcsNoWorld;
    if (!g_ecsWorld->exists(id)) return kEcsNoEntity;
    const auto* mc = static_cast<const scene::ModelComponent*>(
        g_ecsWorld->getComponent(id, scene::componentTypeID<scene::ModelComponent>()));
    if (!mc) return kEcsComponent;
    *outMeshFile = (ownedBuffer() = mc->meshFile).c_str();
    if (outTint3) {
        outTint3[0] = mc->tint.x; outTint3[1] = mc->tint.y; outTint3[2] = mc->tint.z;
    }
    if (outRoughness) *outRoughness = mc->roughness;
    if (outMetallic)  *outMetallic  = mc->metallic;
    return kEcsOk;
}

// ---- v3 Time 导出（M-SC5；操作目标 = timeSetClock 绑定的 GameClock）----

core::GameClock* g_clock = nullptr;

extern "C" int nl_timeGetState(double* outSimTime, double* outFrameDt,
                               std::uint64_t* outTickCount, int* outPaused) {
    if (!outSimTime || !outFrameDt || !outTickCount || !outPaused) return kTimeBadArg;
    *outSimTime = 0.0;
    *outFrameDt = 0.0;
    *outTickCount = 0;
    *outPaused = 0;
    if (!g_clock) return kTimeNoClock;
    *outSimTime = g_clock->simTime();
    *outFrameDt = g_clock->frameDt();
    *outTickCount = g_clock->tickCount();
    *outPaused = g_clock->paused() ? 1 : 0;
    return kTimeOk;
}

extern "C" int nl_timeSetState(int paused, double timeScale) {
    if (!g_clock) return kTimeNoClock;
    // paused：-1=不变（约定哨兵）；0/1 显式设；其它越界值宽容当作不变（防呆）
    if (paused == 0) g_clock->setPaused(false);
    else if (paused == 1) g_clock->setPaused(true);
    // timeScale < 0 = 不变；NaN 恒 false 天然跳过（setTimeScale 内部还有钳制兜底）
    if (timeScale >= 0.0) g_clock->setTimeScale(timeScale);
    return kTimeOk;
}

// ---- v3 Input 导出（M-SC5；操作目标 = inputSetActionMap / inputSetState 绑定的实例）----

core::InputState* g_inputState = nullptr;
core::ActionMap* g_actionMap = nullptr;

// 动作 id 有效性 + 类型校验（name() 非法 id = nullptr 是 ActionMap 文档口径）。
bool actionValid(core::ActionMap* map, core::ActionMap::ActionId id, core::ActionMap::Type want) {
    if (id == core::ActionMap::kInvalidAction) return false;
    if (map->name(id) == nullptr) return false;
    return map->type(id) == want;
}

extern "C" int nl_inputActionFind(const char* name, std::uint32_t* outId) {
    if (!name || !outId) return kInputBadArg;
    *outId = core::ActionMap::kInvalidAction;
    if (!g_actionMap) return kInputNoSource;
    const auto id = g_actionMap->find(name);
    if (id == core::ActionMap::kInvalidAction) return kInputNoAction;
    *outId = id;
    return kInputOk;
}

extern "C" int nl_inputActionBool(std::uint32_t id, int* outValue, int* outPressed, int* outReleased) {
    if (!outValue || !outPressed || !outReleased) return kInputBadArg;
    *outValue = 0; *outPressed = 0; *outReleased = 0;
    if (!g_actionMap) return kInputNoSource;
    if (!actionValid(g_actionMap, id, core::ActionMap::Type::Bool)) return kInputNoAction;
    *outValue = g_actionMap->boolValue(id) ? 1 : 0;
    *outPressed = g_actionMap->boolJustPressed(id) ? 1 : 0;
    *outReleased = g_actionMap->boolJustReleased(id) ? 1 : 0;
    return kInputOk;
}

extern "C" int nl_inputActionFloat(std::uint32_t id, float* outValue, float* outDelta) {
    if (!outValue || !outDelta) return kInputBadArg;
    *outValue = 0.0f; *outDelta = 0.0f;
    if (!g_actionMap) return kInputNoSource;
    if (!actionValid(g_actionMap, id, core::ActionMap::Type::Float)) return kInputNoAction;
    *outValue = g_actionMap->floatValue(id);
    *outDelta = g_actionMap->floatDelta(id);
    return kInputOk;
}

extern "C" int nl_inputActionVec2(std::uint32_t id, float* outValue2, float* outDelta2) {
    if (!outValue2 || !outDelta2) return kInputBadArg;
    outValue2[0] = outValue2[1] = 0.0f;
    outDelta2[0] = outDelta2[1] = 0.0f;
    if (!g_actionMap) return kInputNoSource;
    if (!actionValid(g_actionMap, id, core::ActionMap::Type::Vec2)) return kInputNoAction;
    const auto v = g_actionMap->vec2Value(id);
    const auto d = g_actionMap->vec2Delta(id);
    outValue2[0] = v.x; outValue2[1] = v.y;
    outDelta2[0] = d.x; outDelta2[1] = d.y;
    return kInputOk;
}

extern "C" int nl_inputKeyIsDown(int key, int* outDown) {
    if (!outDown) return kInputBadArg;
    *outDown = 0;
    if (!g_inputState) return kInputNoSource;
    // 越界键码：InputState::isDown 防御性返回 false（文档口径），此处透传
    *outDown = g_inputState->isDown(key) ? 1 : 0;
    return kInputOk;
}

// ---- v3 Log 导出（M-SC5；nl::core::log 全局面，通道 "script"）----

extern "C" int nl_logWrite(int level, const char* message) {
    if (!message) return kLogBadArg;
    if (level < 0 || level > 5) return kLogBadArg;   // 0..5 = Trace..Fatal
    using core::log::Level;
    const Level lv = static_cast<Level>(level);
    core::log::writeCh(lv, "script", message);
    return kLogOk;
}

extern "C" int nl_logSetFilter(const char* instruction, const char** outError) {
    if (!instruction || !outError) return kLogBadArg;
    *outError = nullptr;
    std::string err;
    if (core::log::setFilter(instruction, &err)) return kLogOk;
    ownedBuffer() = std::move(err);   // 静态缓冲账本：下次调用前有效，C# 立即拷
    *outError = ownedBuffer().c_str();
    return kLogBadFilter;
}

// ---- v4 Camera 导出（M-SC6；操作目标 = cameraSetSource 绑定的适配源）----
// core 不反向依赖 viewer：适配源由注册方（viewer / selftest 假源）实现 4 个函数
// 指针；导出体只做指针校验 + 未绑兜底 + 透传，模式感知语义在适配源内（Bindings.hpp）。

const CameraSource* g_camSource = nullptr;

extern "C" int nl_cameraGetPose(float* outEye, float* outYawDeg, float* outPitchDeg,
                                float* outFovY, int* outMode) {
    if (!g_camSource) return kCamNoSource;
    // 任一 out 可 NULL 跳过该字段：适配源契约 = 逐字段判空（值字段经栈暂存透传）
    float eyeTmp[3] = {0.0f, 0.0f, 0.0f};
    float yawTmp = 0.0f, pitchTmp = 0.0f, fovTmp = 0.0f;
    int modeTmp = 0;
    g_camSource->getPose(g_camSource->ctx, outEye ? outEye : eyeTmp,
                         outYawDeg ? outYawDeg : &yawTmp,
                         outPitchDeg ? outPitchDeg : &pitchTmp,
                         outFovY ? outFovY : &fovTmp, outMode ? outMode : &modeTmp);
    return kCamOk;
}

extern "C" int nl_cameraSetPose(const float* eye, float yawDeg, float pitchDeg) {
    if (!eye) return kCamBadArg;
    if (!g_camSource) return kCamNoSource;
    return g_camSource->setPose(g_camSource->ctx, eye, yawDeg, pitchDeg);
}

extern "C" float nl_cameraGetFov() {
    // f32 返回无 rc 通道：未绑 = NaN（C# 侧 float.IsNaN 判未绑——exports.api 写死）
    if (!g_camSource) return std::numeric_limits<float>::quiet_NaN();
    return g_camSource->getFov(g_camSource->ctx);
}

extern "C" int nl_cameraSetFov(float fovY) {
    if (!g_camSource) return kCamNoSource;
    g_camSource->setFov(g_camSource->ctx, fovY);
    return kCamOk;
}

// ---- v6 Physics 载具导出（M-SC8；操作目标 = driveSetScene 绑定的训练场）----
// 直持 nl::physics::DebugScene*（拍板 Q-SC8-1 A'）：M-NET2 多载具面逐字透传，
// 绑定面零 Jolt。线程口径 = OnTick 逻辑帧约定（Q-SC8-2 A：输入在下一次注册方
// step 消费——DebugScene::step 由 viewer 循环调，脚本只在 tick 边界写输入）。
// 槽位有效性探针 = vehicleChassisState(slot).valid（used && chassis 同条件，
// tombstone 后即 false——M-NET2 内部静默忽略，绑定面显式回 kPhyNoSlot）。
physics::DebugScene* g_driveScene = nullptr;

extern "C" int nl_physicsAddVehicle(float spawnX, float spawnZ, int* outSlot) {
    if (!outSlot) return kPhyBadArg;
    *outSlot = -1;
    if (!g_driveScene) return kPhyNoScene;
    const int slot = g_driveScene->addVehicle(spawnX, spawnZ);
    if (slot < 0) return kPhyAddFailed;   // lastError 已置因（DebugScene 契约）
    *outSlot = slot;
    return kPhyOk;
}

extern "C" int nl_physicsRemoveVehicle(int slot) {
    if (!g_driveScene) return kPhyNoScene;
    return g_driveScene->removeVehicle(slot) ? kPhyOk : kPhyNoSlot;
}

extern "C" int nl_physicsSetVehicleInput(int slot, float forward, float right,
                                         float brake, float handBrake) {
    if (!g_driveScene) return kPhyNoScene;
    if (!g_driveScene->vehicleChassisState(slot).valid) return kPhyNoSlot;
    g_driveScene->setVehicleInput(slot, {forward, right, brake, handBrake});
    return kPhyOk;
}

extern "C" int nl_vehicleFullPose(int slot, float* outPos, float* outQuat,
                                  float* outVel) {
    if (!g_driveScene) return kPhyNoScene;
    // 任一 out 可 NULL 跳过该字段：逐字段判空，值字段经栈暂存透传（Camera 同款）
    float pTmp[3] = {0.0f, 0.0f, 0.0f}, qTmp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float vTmp[3] = {0.0f, 0.0f, 0.0f};
    const bool ok = g_driveScene->vehicleFullPose(
        slot, outPos ? outPos : pTmp, outQuat ? outQuat : qTmp,
        outVel ? outVel : vTmp);
    return ok ? kPhyOk : kPhyNoSlot;
}

extern "C" int nl_teleportVehicle(int slot, const float* pos, const float* quat) {
    if (!pos || !quat) return kPhyBadArg;
    if (!g_driveScene) return kPhyNoScene;
    if (!g_driveScene->vehicleChassisState(slot).valid) return kPhyNoSlot;
    g_driveScene->teleportVehicle(slot, pos, quat);
    return kPhyOk;
}

} // namespace

void ecsSetWorld(scene::World* world) {
    g_ecsWorld = world;
}

void driveSetScene(physics::DebugScene* scene) {
    g_driveScene = scene;
}

// ---- M-SC10 属性面板域（描述表 POD；生命周期硬规则①：C++ 只存 POD，
//      托管反射缓存留脚本程序集随 ALC 回收——Bindings.hpp 口径）----

namespace {

std::vector<PropDesc> g_props;
std::uint32_t g_propsGen = 0;

} // namespace

const std::vector<PropDesc>& scriptProps() {
    return g_props;
}

std::uint32_t scriptPropsGen() {
    return g_propsGen;
}

extern "C" int nl_propsPushBegin() {
    g_props.clear();
    ++g_propsGen;   // 重建信号（面板/宿主按代数比对换表）
    return kPropsOk;
}

extern "C" int nl_propsDescAdd(int kind, const char* name, float min, float max, float def) {
    if (kind < kPropKindFloat || kind > kPropKindString || name == nullptr || name[0] == '\0')
        return kPropsBadArg;
    if (g_props.size() >= kMaxProps)
        return kPropsBadArg;
    PropDesc d{};
    d.kind = kind;
    std::snprintf(d.name, sizeof d.name, "%.*s", kPropNameCap, name);
    d.min = min;
    d.max = max;
    d.def = def;
    g_props.push_back(d);
    return kPropsOk;
}

// ---- M-SC5 宿主侧活实例绑定（Bindings.hpp 口径；nullptr = 解绑）----

void timeSetClock(core::GameClock* clock) {
    g_clock = clock;
}

void inputSetState(core::InputState* state) {
    g_inputState = state;
}

void inputSetActionMap(core::ActionMap* map) {
    g_actionMap = map;
}

// ---- M-SC6 宿主侧相机适配源绑定（nullptr = 解绑，导出回 kCamNoSource）----

void cameraSetSource(const CameraSource* source) {
    g_camSource = source;
}

// ---- M-SC3 脚本命令登记表公开接口（存储 = 匿名 namespace 顶部的 scriptCommandTable）----

std::vector<std::string> scriptRegisteredCommands() {
    return {scriptCommandTable().begin(), scriptCommandTable().end()};
}

std::vector<std::string> scriptUnregisterAll() {
    std::vector<std::string> names(scriptCommandTable().begin(), scriptCommandTable().end());
    for (const auto& n : names) Console::get().unregisterCommand(n);   // unknown 容错跳过
    scriptCommandTable().clear();
    return names;
}

const ApiTable* consoleApiTable() {
    // M-SC4：注册行 = 生成物（单源 exports.api；改绑定面 = 改声明 + 重跑 gen.py，
    // 编译期 asserts.inc 防漂移、CMake nl_scriptapi_freshness 防过期）。
    static const ApiTable table{
#include "ApiTable.g.inc"
    };
    return &table;
}

// ---- M-SC4 编译期签名防漂移（生成物；声明 exports.api 与上面 extern "C" 实现
//      签名不一致 = 编译失败。须在实现之后、namespace nl::script 内 include——
//      M-SC10 教训：props 段实现落在了 include 之后会被 decltype 判未声明）----
#include "ApiTable.g.asserts.inc"

} // namespace nl::script
