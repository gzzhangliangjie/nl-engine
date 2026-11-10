// nl/physics — P8 M-P0 minimal world. Integration layer ONLY; Jolt supplies the solver.
//
// Three-source evidence (2026-11-09 re-read, AGENTS.md decision-first rule):
//  - Carrot engine/physics/PhysicsSystem.cpp: singleton world, RegisterDefaultAllocator
//    -> Trace -> Factory::sInstance -> RegisterTypes -> TempAllocatorImpl(10MB)
//    -> JobSystemThreadPool -> PhysicsSystem::Init(...) -> SetGravity -> tick 1/60
//    with accumulator. We take the lifecycle + fixed-step cadence, NOT the singleton
//    (headless tool needs fresh worlds), and NOT the thread pool (determinism contract).
//  - Jolt upstream HelloWorld/HelloWorld.cpp (read only): layer/broadphase filter
//    skeleton, BoxShapeSettings floor, CreateAndAddBody sphere, OptimizeBroadPhase,
//    Update(dt, 1, allocator, jobs). Copy ideas, not code.
//  - UE 5.8.4 Chaos (read only): industrial solver boundary = Scene/Actor + BodyInstance;
//    we deliberately stay on the integration side (UE-DIFFS D10).
//
// M-P0 scope: 2 object layers (static/dynamic), 2 broadphase layers, one static
// floor, one dynamic sphere, per-frame center-of-mass trajectory. M-P1 adds the
// engine<->body association and configurable layer matrix; M-P3 adds debug rendering.

#include <Jolt/Jolt.h> // must be first Jolt header

#include "nl/physics/World.hpp"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Renderer/DebugRenderer.h>

#include "nl/core/jobs.hpp"  // M-J5c: ThreadPool（PoolJobSystem 的执行底座）

#include <algorithm>
#include <atomic>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>

using namespace JPH::literals;

namespace {

// ---- M-P1: kLayerCount object layers mapped 1:1 onto broadphase layers, so the
// caller-supplied LayerMatrix is authoritative at BOTH filter stages.
// M-P0 keeps its exact original cadence by passing a matrix with (floor,floor)
// disabled -- see simulateDrop() below (keeps the M-P0 baseline bit-identical). ----
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return nl::physics::kLayerCount; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(inLayer));
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        static const char* kNames[nl::physics::kLayerCount] = {"FLOOR", "DEFAULT", "DEBRIS", "GHOST"};
        return kNames[inLayer.GetValue()];
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    explicit ObjectVsBroadPhaseLayerFilterImpl(const nl::physics::LayerMatrix& m) : matrix_(m) {}
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        return matrix_.allows(inLayer1, inLayer2.GetValue());
    }
private:
    const nl::physics::LayerMatrix& matrix_;
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectLayerPairFilterImpl(const nl::physics::LayerMatrix& m) : matrix_(m) {}
    bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        return matrix_.allows(inObject1, inObject2);
    }
private:
    const nl::physics::LayerMatrix& matrix_;
};

// ---- Process-wide Jolt plumbing (idempotent) ----
void traceImpl(const char* inFMT, ...) {
    va_list args;
    va_start(args, inFMT);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), inFMT, args);
    va_end(args);
    std::fprintf(stderr, "[Jolt] %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
// Returning true makes JPH_ASSERT fire JPH_BREAKPOINT; a headless tool must not
// int3 on an informational assert, so log and continue instead.
bool assertFailedImpl(const char* expr, const char* msg, const char* file, JPH::uint line) {
    std::fprintf(stderr, "[Jolt ASSERT] %s:%u (%s) %s\n",
                 file, line, expr, msg ? msg : "");
    return false;
}
#endif

std::once_flag g_bootstrapOnce;

} // namespace

namespace nl::physics {

bool validateConfig(const DropConfig& cfg, std::string* err) {
    auto bad = [err](const char* reason) -> bool {
        if (err) *err = reason;
        return false;
    };
    if (cfg.frames < 1 || cfg.frames > 100000) return bad("frames out of range [1,100000]");
    if (!(cfg.dt >= 1.0 / 1000.0 && cfg.dt <= 1.0 / 30.0)) return bad("dt out of range [0.001,0.033333]");
    if (!(cfg.gravity > 0.0f && cfg.gravity <= 1000.0f)) return bad("gravity out of range (0,1000]");
    if (!(cfg.radius > 0.0f && cfg.radius <= 10.0f)) return bad("radius out of range (0,10]");
    if (!(cfg.startHeight > cfg.radius && cfg.startHeight <= 1000.0f))
        return bad("startHeight must exceed radius and be <= 1000");
    if (!(cfg.restitution >= 0.0f && cfg.restitution <= 1.0f))
        return bad("restitution out of range [0,1]");
    if (cfg.jobsThreads < 0 || cfg.jobsThreads > 64) return bad("jobsThreads out of range [0,64]");
    return true;
}

void World::bootstrap() {
    std::call_once(g_bootstrapOnce, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = traceImpl;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = assertFailedImpl;
#endif
        // Jolt's own HelloWorld does the same: factory is process-wide and intentionally
        // outlives every world (destroying it would invalidate registered types).
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

struct World::Impl {
    std::uint32_t lastBodyCount = 0;
};

World::World() : impl_(std::make_unique<Impl>()) {}
World::~World() = default;

std::uint32_t World::bodyCount() const { return impl_->lastBodyCount; }

// ===== M-J5c: Jolt JobSystem 适配器 → nl/core 池底座（决策前置读证 2026-11-09）=====
// 三处证据：Jolt JobSystem.h 头注释三支柱合约（依赖计数在 JPH::Job 内部 /
// Barrier 等待期可 AddJob / WaitForJobs 必须帮跑）；JobSystemWithBarrier.cpp
// Wait() 自带帮跑（扫描执行 barrier 内可执行 job）= 合约 c 由 Jolt 基类兜底；
// JobSystemThreadPool.cpp QueueJobInternal = 入队必须 AddRef、执行完 Release。
// jobsync 原语（M-J5c-prep）与本类同构，但依赖计数留在 JPH::Job 内部完成，
// 适配器只需把 QueueJob 映射到池 post —— 因此执行底座用 ThreadPool::post。
// 边界：pool 生命周期必须覆盖整个 simulate 调用（unique_ptr 声明在
// PhysicsSystem 之前 = 析构在其之后，满足 Jolt 对 JobSystem 的存活要求）。
class PoolJobSystem final : public JPH::JobSystemWithBarrier {
public:
    explicit PoolJobSystem(nl::core::jobs::ThreadPool& pool)
        : JPH::JobSystemWithBarrier(8)  // PhysicsSystem::Update 用 1 个 barrier，8 兜余量
        , pool_(pool) {}

    int GetMaxConcurrency() const override {
        return static_cast<int>(pool_.threadCount());
    }
    // 注：Job / JobHandle 是 JobSystem 嵌套类型（派生类内可裸用）；
    // ColorArg / uint32 / uint 是 JPH 命名空间类型（须限定）。
    JobHandle CreateJob(const char* inName, JPH::ColorArg inColor,
                        const JobFunction& inFunction, JPH::uint32 inNumDependencies) override {
        // Job 为引用计数对象：构造即持 1 引（由 JobHandle 承接）；deps==0 直接入队
        // （JobSystemThreadPool::CreateJob 同款次序）。
        Job* job = new Job(inName, inColor, this, inFunction, inNumDependencies);
        JobHandle handle(job);
        if (inNumDependencies == 0) QueueJob(job);
        return handle;
    }
    void QueueJob(Job* inJob) override {
        // Jolt 合约：QueueJob 调用期内 job 保活，但异步闭包必须自持引用
        //（AddRef 在投递前，闭包内 Execute + Release 成对）。
        inJob->AddRef();
        pool_.post([inJob] {
            inJob->Execute();
            inJob->Release();
        });
    }
    void QueueJobs(Job** inJobs, JPH::uint inNumJobs) override {
        for (JPH::uint i = 0; i < inNumJobs; ++i) QueueJob(inJobs[i]);
    }
    void FreeJob(Job* inJob) override { delete inJob; }

private:
    nl::core::jobs::ThreadPool& pool_;
};

// jobsThreads 语义：0 = 单线程字节基线路径（M-P0~P4 全部基线）；>=1 = 适配器 +
// 独立 ThreadPool（不走 Scheduler 单例——configure 冻结使一进程多配置不可行，
// 验收需 N=1/N=4 同进程对拍；Scheduler 常驻底座仍是推荐生产入口，见 §11 文档）。
struct JobRuntime {
    // 【2026-11-10 崩溃修复】成员声明序 = 析构序的逆序，必须 pool 在 adapter
    // 之后声明（析构时 pool 先死）：Jolt Job::Execute() 末尾就通知 barrier
    // （JobSystem.h "Notify the barrier..."），而排队引用的 Release() 在池
    // worker 闭包里 Execute 之后才跑 —— PhysicsSystem::Update 返回时可能仍有
    // worker 卡在 Release() 前后；若 adapter（JobSystem 本体）先析构，最后
    // 一条 Release() 减到 0 会对已析构对象虚调 FreeJob → 纯虚调用 → ucrtbase
    // fail-fast 0xC0000409（本机实测 N=1 崩率 7% / N=4 27%，纳秒级窗口，
    // 任何时序扰动都压得住的 Heisenbug）。pool 先析构 = join 全部 worker
    // = 闭包全部跑完（含 FreeJob），adapter 才轮到析构，两种 last-ref 线程
    // 次序都安全。
    std::unique_ptr<JPH::JobSystemSingleThreaded>    st;
    std::unique_ptr<PoolJobSystem>                   adapter;
    std::unique_ptr<nl::core::jobs::ThreadPool>      pool;
    JPH::JobSystem*                                  sys = nullptr;
};

JobRuntime makeJobRuntime(int jobsThreads) {
    JobRuntime rt;
    if (jobsThreads > 0) {
        rt.pool = std::make_unique<nl::core::jobs::ThreadPool>(
            static_cast<unsigned>(jobsThreads));
        rt.adapter = std::make_unique<PoolJobSystem>(*rt.pool);
        rt.sys = rt.adapter.get();
    } else {
        rt.st = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        rt.sys = rt.st.get();
    }
    return rt;
}

SimResult World::simulateDrop(const DropConfig& cfg) {
    SimResult result;
    if (!validateConfig(cfg, &result.error)) return result;
    bootstrap();

    // Filter interfaces are referenced (not owned) by PhysicsSystem: declare them
    // before it so they outlive it under reverse destruction order.
    // M-P0 cadence preserved exactly: floor/floor disabled, all other pairs on.
    LayerMatrix matrix;
    matrix.set(layers::kFloor, layers::kFloor, false);
    JPH::TempAllocatorImpl tempAllocator(10 * 1024 * 1024);
    // M-J5c：job system 按配置分流；声明在 physicsSystem 之前 = 析构在其之后。
    JobRuntime jobRuntime = makeJobRuntime(cfg.jobsThreads);
    JPH::JobSystem* jobSystem = jobRuntime.sys;
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter(matrix);
    ObjectLayerPairFilterImpl objectLayerPairFilter(matrix);
    JPH::PhysicsSystem physicsSystem;

    physicsSystem.Init(1024, 0, 1024, 1024,
                       bpLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter);
    physicsSystem.SetGravity(JPH::Vec3(0.0f, -cfg.gravity, 0.0f));

    JPH::BodyInterface& bodyInterface = physicsSystem.GetBodyInterface();

    // Static floor: box half-extents (100, 1, 100) centered at y=-1 => top face y=0.
    JPH::BoxShapeSettings floorShapeSettings(JPH::Vec3(100.0f, 1.0f, 100.0f));
    floorShapeSettings.SetEmbedded();
    auto floorShapeResult = floorShapeSettings.Create();
    if (floorShapeResult.HasError()) {
        result.error = "floor shape creation failed";
        return result;
    }
    JPH::ShapeRefC floorShape = floorShapeResult.Get();

    JPH::BodyCreationSettings floorSettings(
        floorShape, JPH::RVec3(0.0_r, -1.0_r, 0.0_r),
        JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kFloor);
    floorSettings.mRestitution = cfg.restitution;
    JPH::Body* floor = bodyInterface.CreateBody(floorSettings);
    if (!floor) {
        result.error = "floor body creation failed (out of bodies)";
        return result;
    }
    bodyInterface.AddBody(floor->GetID(), JPH::EActivation::DontActivate);

    JPH::BodyCreationSettings sphereSettings(
        new JPH::SphereShape(cfg.radius),
        JPH::RVec3(0.0_r, JPH::Real(cfg.startHeight), 0.0_r),
        JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, layers::kDefault);
    sphereSettings.mRestitution = cfg.restitution;
    sphereSettings.mLinearDamping = 0.0f;
    JPH::BodyID sphereId = bodyInterface.CreateAndAddBody(sphereSettings, JPH::EActivation::Activate);
    if (sphereId.IsInvalid()) {
        result.error = "sphere body creation failed (out of bodies)";
        return result;
    }

    physicsSystem.OptimizeBroadPhase();
    impl_->lastBodyCount = physicsSystem.GetNumBodies();

    auto pushSample = [&](int frame) {
        const JPH::RVec3 p = bodyInterface.GetCenterOfMassPosition(sphereId);
        const JPH::Vec3  v = bodyInterface.GetLinearVelocity(sphereId);
        Sample s;
        s.frame = frame;
        s.t = frame * cfg.dt;
        s.position = { static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                       static_cast<float>(p.GetZ()) };
        s.velocity = { v.GetX(), v.GetY(), v.GetZ() };
        result.samples.push_back(s);
    };

    result.samples.reserve(static_cast<std::size_t>(cfg.frames) + 1);
    pushSample(0);
    for (int frame = 1; frame <= cfg.frames; ++frame) {
        physicsSystem.Update(static_cast<float>(cfg.dt), 1, &tempAllocator, jobSystem);
        pushSample(frame);
    }

    bodyInterface.RemoveBody(sphereId);
    bodyInterface.DestroyBody(sphereId);
    bodyInterface.RemoveBody(floor->GetID());
    bodyInterface.DestroyBody(floor->GetID());
    return result;
}

// ---- M-P1: layer matrix, engine<->body association, probe harness ----
LayerMatrix::LayerMatrix() {
    const std::uint32_t all = (1u << kLayerCount) - 1u;
    for (std::uint32_t i = 0; i < kLayerCount; ++i) rows[i] = all;
}

bool LayerMatrix::allows(std::uint32_t a, std::uint32_t b) const {
    if (a >= kLayerCount || b >= kLayerCount) return false;
    return (rows[a] & (1u << b)) != 0u;
}

void LayerMatrix::set(std::uint32_t a, std::uint32_t b, bool allow) {
    if (a >= kLayerCount || b >= kLayerCount) return;
    if (allow) {
        rows[a] |= (1u << b);
        rows[b] |= (1u << a);
    } else {
        rows[a] &= ~(1u << b);
        rows[b] &= ~(1u << a);
    }
}

std::string LayerMatrix::hex() const {
    char buf[kLayerCount * 2 + 1];
    for (std::uint32_t i = 0; i < kLayerCount; ++i)
        std::snprintf(buf + i * 2, 3, "%02x", rows[i] & 0xffu);
    buf[kLayerCount * 2] = '\0';
    return std::string(buf);
}

bool validateLayerProbe(const LayerProbeConfig& cfg, const LayerMatrix& matrix,
                        std::string* err) {
    auto bad = [err](const char* reason) -> bool {
        if (err) *err = reason;
        return false;
    };
    if (cfg.frames < 1 || cfg.frames > 100000) return bad("frames out of range [1,100000]");
    if (!(cfg.dt >= 1.0 / 1000.0 && cfg.dt <= 1.0 / 30.0)) return bad("dt out of range [0.001,0.033333]");
    if (!(cfg.gravity > 0.0f && cfg.gravity <= 1000.0f)) return bad("gravity out of range (0,1000]");
    if (!(cfg.radius > 0.0f && cfg.radius <= 10.0f)) return bad("radius out of range (0,10]");
    if (cfg.probeLayer >= kLayerCount || cfg.blockerLayer >= kLayerCount ||
        cfg.floorLayer >= kLayerCount)
        return bad("layer out of range [0,3]");
    if (!(cfg.startHeight > cfg.blockerY + 2.0f * cfg.radius))
        return bad("startHeight must clear the blocker");
    if (!(cfg.blockerY > cfg.radius)) return bad("blockerY must clear the floor");
    if (!(cfg.restitution >= 0.0f && cfg.restitution <= 1.0f))
        return bad("restitution out of range [0,1]");
    for (std::uint32_t a = 0; a < kLayerCount; ++a)
        for (std::uint32_t b = 0; b < kLayerCount; ++b)
            if (matrix.allows(a, b) != matrix.allows(b, a))
                return bad("layer matrix is not symmetric");
    return true;
}

LayerProbeResult World::simulateLayerProbe(const LayerMatrix& matrix,
                                           const LayerProbeConfig& cfg) {
    LayerProbeResult r;
    if (!validateLayerProbe(cfg, matrix, &r.error)) return r;
    bootstrap();

    // Declared first: filters hold references and PhysicsSystem holds pointers,
    // so the matrix must outlive both under reverse destruction order.
    LayerMatrix local = matrix;
    JPH::TempAllocatorImpl tempAllocator(10 * 1024 * 1024);
    JPH::JobSystemSingleThreaded jobSystem(JPH::cMaxPhysicsJobs);
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter(local);
    ObjectLayerPairFilterImpl objectLayerPairFilter(local);
    JPH::PhysicsSystem physicsSystem;

    physicsSystem.Init(1024, 0, 1024, 1024,
                       bpLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter);
    physicsSystem.SetGravity(JPH::Vec3(0.0f, -cfg.gravity, 0.0f));
    JPH::BodyInterface& bi = physicsSystem.GetBodyInterface();

    JPH::BoxShapeSettings floorShapeSettings(JPH::Vec3(100.0f, 1.0f, 100.0f));
    floorShapeSettings.SetEmbedded();
    auto floorShapeResult = floorShapeSettings.Create();
    if (floorShapeResult.HasError()) { r.error = "floor shape creation failed"; return r; }
    JPH::ShapeRefC floorShape = floorShapeResult.Get();
    JPH::ShapeRefC sphereShape = new JPH::SphereShape(cfg.radius);

    std::unordered_map<std::uint64_t, JPH::BodyID> ownerToId;
    std::vector<JPH::BodyID> ids;

    // Engine object <-> Jolt body association. Jolt's userData holds the INDEX
    // into r.bodies (never a raw pointer, so a stale handle cannot dangle).
    auto add = [&](const JPH::ShapeRefC& shape, JPH::RVec3 pos, JPH::EMotionType motion,
                   std::uint32_t layer, std::uint64_t owner) -> bool {
        JPH::BodyCreationSettings s(shape, pos, JPH::Quat::sIdentity(), motion,
                                    static_cast<JPH::ObjectLayer>(layer));
        s.mRestitution   = cfg.restitution;
        s.mLinearDamping = 0.0f;
        JPH::Body* body = bi.CreateBody(s);
        if (!body) return false;
        const std::uint32_t idx = static_cast<std::uint32_t>(r.bodies.size());
        body->SetUserData(static_cast<std::uint64_t>(idx));
        const JPH::BodyID id = body->GetID();

        BodyInfo info;
        info.id       = id.GetIndexAndSequenceNumber();
        info.owner    = owner;
        info.layer    = layer;
        info.isStatic = (motion == JPH::EMotionType::Static);
        info.position = { static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()),
                          static_cast<float>(pos.GetZ()) };
        r.bodies.push_back(info);
        ownerToId[owner] = id;
        ids.push_back(id);
        bi.AddBody(id, motion == JPH::EMotionType::Static
                           ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        return true;
    };

    if (!add(floorShape, JPH::RVec3(0.0_r, -1.0_r, 0.0_r),
             JPH::EMotionType::Static, cfg.floorLayer, cfg.floorOwner) ||
        !add(sphereShape, JPH::RVec3(0.0_r, JPH::Real(cfg.blockerY), 0.0_r),
             JPH::EMotionType::Static, cfg.blockerLayer, cfg.blockerOwner) ||
        !add(sphereShape, JPH::RVec3(0.0_r, JPH::Real(cfg.startHeight), 0.0_r),
             JPH::EMotionType::Dynamic, cfg.probeLayer, cfg.probeOwner)) {
        r.error = "body creation failed";
        return r;
    }

    physicsSystem.OptimizeBroadPhase();
    impl_->lastBodyCount = physicsSystem.GetNumBodies();
    const JPH::BodyID probeId = ids.back();
    r.probeId = probeId.GetIndexAndSequenceNumber();

    auto pushSample = [&](int frame) {
        const JPH::RVec3 p = bi.GetCenterOfMassPosition(probeId);
        const JPH::Vec3  v = bi.GetLinearVelocity(probeId);
        Sample s;
        s.frame = frame;
        s.t = frame * cfg.dt;
        s.position = { static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                       static_cast<float>(p.GetZ()) };
        s.velocity = { v.GetX(), v.GetY(), v.GetZ() };
        r.samples.push_back(s);
    };

    r.samples.reserve(static_cast<std::size_t>(cfg.frames) + 1);
    pushSample(0);
    for (int frame = 1; frame <= cfg.frames; ++frame) {
        physicsSystem.Update(static_cast<float>(cfg.dt), 1, &tempAllocator, &jobSystem);
        pushSample(frame);
    }
    r.finalY  = r.samples.back().position.y;
    r.blocked = r.finalY > cfg.blockerY + cfg.radius; // ~3.5 on blocker vs ~0.5 on floor

    // Two-way self-check: BodyID -> userData(index) -> owner, and owner -> BodyID.
    const std::uint64_t userData = bi.GetUserData(probeId);
    if (userData < r.bodies.size())
        r.ownerFromId = r.bodies[static_cast<std::size_t>(userData)].owner;
    const auto it = ownerToId.find(cfg.probeOwner);
    if (it != ownerToId.end()) r.idFromOwner = it->second.GetIndexAndSequenceNumber();

    for (auto rit = ids.rbegin(); rit != ids.rend(); ++rit) {
        bi.RemoveBody(*rit);
        bi.DestroyBody(*rit);
    }
    return r;
}


// ===== P8 M-D0: contact event bridge (Jolt ContactListener -> POD stream) =====
// Three-source evidence and the removed-body hard constraint are documented in
// Events.hpp. This is the ONLY place inside nl_core where JPH contact types appear.
//
// Determinism (M-P0 contract): the probe runs on JobSystemSingleThreaded, so
// callback order equals frame order. We STILL stable-sort each frame's slice by
// (minBodyId, maxBodyId, kind) so a future pool-backed JobSystem cannot alter the
// byte stream (M-J5c: callbacks may arrive from arbitrary threads). The mutex is
// defensive: M-D0 never enables the pool path, but the collector must remain
// correct if a later milestone switches the course to parallel jobs.
namespace {

struct CachedContact {
    std::uint64_t  ownerA  = 0;
    std::uint64_t  ownerB  = 0;
    std::uint32_t  bodyIdA = nl::physics::kInvalidBodyId;
    std::uint32_t  bodyIdB = nl::physics::kInvalidBodyId;
    nl::math::Vec3 worldPos{};
    float          normalY = 0.0f;
};

// Key uses ONLY the body-id part of SubShapeIDPair. ContactListener.h:133-134
// warns sub-shape IDs come from the previous step and may be invalid after a
// shape change; body ids are stable for the pair's lifetime. Both ids are read
// from the callback argument itself -- never via a Jolt body lookup.
std::uint64_t contactPairKey(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t lo = a < b ? a : b;
    const std::uint32_t hi = a < b ? b : a;
    return (static_cast<std::uint64_t>(lo) << 32) | hi;
}

class ContactCollector final : public JPH::ContactListener {
public:
    explicit ContactCollector(const std::vector<std::uint64_t>& owners)
        : owners_(owners) {}

    void beginFrame(int frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_.store(frame, std::memory_order_relaxed);
        frameStart_ = events_.size();
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        record(nl::physics::ContactKind::Added, b1, b2, manifold);
    }

    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override {
        record(nl::physics::ContactKind::Persisted, b1, b2, manifold);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        // ContactListener.h:123-128 -- bodies may be destroyed by now. Do NOT
        // call any body interface; the pair argument and the cache are all we read.
        const std::uint32_t a = pair.GetBody1ID().GetIndexAndSequenceNumber();
        const std::uint32_t b = pair.GetBody2ID().GetIndexAndSequenceNumber();
        nl::physics::ContactEvent e;
        e.frame   = frame_.load(std::memory_order_relaxed);
        e.kind    = nl::physics::ContactKind::Removed;
        e.bodyIdA = a;
        e.bodyIdB = b;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(contactPairKey(a, b));
        if (it != cache_.end()) {
            e.ownerA   = it->second.ownerA;
            e.ownerB   = it->second.ownerB;
            e.worldPos = it->second.worldPos;
            e.normalY  = it->second.normalY;
            cache_.erase(it);          // Q2: clear only after Removed is emitted
        } else {
            ++cacheMisses_;
        }
        events_.push_back(e);
    }

    // Frame-end stable sort: single-threaded baseline keeps callback order,
    // pool-backed callbacks need this to stay bit-identical.
    void finishFrame() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cmpKey = [](const nl::physics::ContactEvent& e) {
            const std::uint32_t lo = e.bodyIdA < e.bodyIdB ? e.bodyIdA : e.bodyIdB;
            const std::uint32_t hi = e.bodyIdA < e.bodyIdB ? e.bodyIdB : e.bodyIdA;
            return std::make_pair(std::make_pair(lo, hi), static_cast<int>(e.kind));
        };
        std::stable_sort(events_.begin() + static_cast<std::ptrdiff_t>(frameStart_),
                         events_.end(),
                         [&](const nl::physics::ContactEvent& x,
                             const nl::physics::ContactEvent& y) {
                             return cmpKey(x) < cmpKey(y);
                         });
    }

    std::vector<nl::physics::ContactEvent> takeEvents() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::move(events_);
    }
    std::uint32_t cacheMisses() const { return cacheMisses_.load(std::memory_order_relaxed); }

private:
    void record(nl::physics::ContactKind kind, const JPH::Body& b1, const JPH::Body& b2,
                const JPH::ContactManifold& manifold) {
        const std::uint32_t id1 = b1.GetID().GetIndexAndSequenceNumber();
        const std::uint32_t id2 = b2.GetID().GetIndexAndSequenceNumber();
        const std::uint64_t u1  = b1.GetUserData();
        const std::uint64_t u2  = b2.GetUserData();

        nl::physics::ContactEvent e;
        e.frame   = frame_.load(std::memory_order_relaxed);
        e.kind    = kind;
        e.bodyIdA = id1;
        e.bodyIdB = id2;
        e.ownerA  = (u1 < owners_.size()) ? owners_[static_cast<std::size_t>(u1)] : 0;
        e.ownerB  = (u2 < owners_.size()) ? owners_[static_cast<std::size_t>(u2)] : 0;
        if (manifold.mRelativeContactPointsOn1.size() > 0) {
            const JPH::RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
            e.worldPos = { static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                           static_cast<float>(p.GetZ()) };
        } else {
            const JPH::RVec3 p = manifold.mBaseOffset;
            e.worldPos = { static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                           static_cast<float>(p.GetZ()) };
        }
        // mWorldSpaceNormal points along the direction that pushes body2 out of
        // collision (ContactListener.h:30). Kept as-is, no sign flip.
        e.normalY = manifold.mWorldSpaceNormal.GetY();

        CachedContact c;
        c.ownerA = e.ownerA;   c.ownerB = e.ownerB;
        c.bodyIdA = id1;       c.bodyIdB = id2;
        c.worldPos = e.worldPos; c.normalY = e.normalY;

        std::lock_guard<std::mutex> lock(mutex_);
        cache_[contactPairKey(id1, id2)] = c;
        events_.push_back(e);
    }

    const std::vector<std::uint64_t>& owners_;   // index == Body::GetUserData
    std::mutex mutex_;
    std::atomic<int> frame_{0};
    std::size_t frameStart_ = 0;
    std::atomic<std::uint32_t> cacheMisses_{0};
    std::unordered_map<std::uint64_t, CachedContact> cache_;
    std::vector<nl::physics::ContactEvent> events_;
};

} // namespace

bool validateContactProbe(const ContactProbeConfig& cfg, std::string* err) {
    auto bad = [err](const char* reason) -> bool {
        if (err) *err = reason;
        return false;
    };
    if (cfg.frames < 1 || cfg.frames > 100000) return bad("frames out of range [1,100000]");
    if (!(cfg.dt >= 1.0 / 1000.0 && cfg.dt <= 1.0 / 30.0)) return bad("dt out of range [0.001,0.033333]");
    if (!(cfg.gravity > 0.0f && cfg.gravity <= 1000.0f)) return bad("gravity out of range (0,1000]");
    if (!(cfg.radius > 0.0f && cfg.radius <= 10.0f)) return bad("radius out of range (0,10]");
    if (!(cfg.startHeight > cfg.blockerY + 2.0f * cfg.radius))
        return bad("startHeight must clear the blocker");
    if (!(cfg.blockerY > cfg.radius)) return bad("blockerY must clear the floor");
    if (!(cfg.restitution >= 0.0f && cfg.restitution <= 1.0f))
        return bad("restitution out of range [0,1]");
    return true;
}

ContactProbeResult World::simulateContactProbe(const ContactProbeConfig& cfg) {
    ContactProbeResult r;
    if (!validateContactProbe(cfg, &r.error)) return r;
    bootstrap();

    // Declared first: filters hold references and PhysicsSystem holds pointers,
    // so the matrix / owners / collector must outlive the system.
    LayerMatrix local;
    JPH::TempAllocatorImpl tempAllocator(10 * 1024 * 1024);
    JPH::JobSystemSingleThreaded jobSystem(JPH::cMaxPhysicsJobs);
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter(local);
    ObjectLayerPairFilterImpl objectLayerPairFilter(local);
    std::vector<std::uint64_t> owners;         // vector index == Body::GetUserData
    ContactCollector collector(owners);
    JPH::PhysicsSystem physicsSystem;

    physicsSystem.Init(1024, 0, 1024, 1024,
                       bpLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter);
    physicsSystem.SetGravity(JPH::Vec3(0.0f, -cfg.gravity, 0.0f));
    physicsSystem.SetContactListener(&collector);
    JPH::BodyInterface& bi = physicsSystem.GetBodyInterface();

    JPH::BoxShapeSettings floorShapeSettings(JPH::Vec3(100.0f, 1.0f, 100.0f));
    floorShapeSettings.SetEmbedded();
    auto floorShapeResult = floorShapeSettings.Create();
    if (floorShapeResult.HasError()) { r.error = "floor shape creation failed"; return r; }
    JPH::ShapeRefC floorShape = floorShapeResult.Get();
    JPH::ShapeRefC sphereShape = new JPH::SphereShape(cfg.radius);

    std::vector<JPH::BodyID> ids;
    // Engine object <-> Jolt body association, same index-in-vector trick as M-P1
    // (userData is an INDEX, never a raw pointer, so a stale handle cannot dangle).
    auto add = [&](const JPH::ShapeRefC& shape, JPH::RVec3 pos, JPH::EMotionType motion,
                   std::uint32_t layer, std::uint64_t owner) -> bool {
        JPH::BodyCreationSettings s(shape, pos, JPH::Quat::sIdentity(), motion,
                                    static_cast<JPH::ObjectLayer>(layer));
        s.mRestitution   = cfg.restitution;
        s.mLinearDamping = 0.0f;
        JPH::Body* body = bi.CreateBody(s);
        if (!body) return false;
        const std::uint32_t idx = static_cast<std::uint32_t>(owners.size());
        body->SetUserData(static_cast<std::uint64_t>(idx));
        owners.push_back(owner);
        const JPH::BodyID id = body->GetID();
        ids.push_back(id);
        bi.AddBody(id, motion == JPH::EMotionType::Static
                           ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        return true;
    };

    if (!add(floorShape, JPH::RVec3(0.0_r, -1.0_r, 0.0_r),
             JPH::EMotionType::Static, layers::kFloor, cfg.floorOwner) ||
        !add(sphereShape, JPH::RVec3(0.0_r, JPH::Real(cfg.blockerY), 0.0_r),
             JPH::EMotionType::Static, layers::kDefault, cfg.blockerOwner) ||
        !add(sphereShape, JPH::RVec3(0.0_r, JPH::Real(cfg.startHeight), 0.0_r),
             JPH::EMotionType::Dynamic, layers::kDebris, cfg.probeOwner)) {
        r.error = "body creation failed";
        return r;
    }

    physicsSystem.OptimizeBroadPhase();
    r.bodyCount = physicsSystem.GetNumBodies();
    impl_->lastBodyCount = r.bodyCount;
    const JPH::BodyID probeId = ids.back();

    auto pushFrame = [&](int frame) {
        const JPH::RVec3 p = bi.GetCenterOfMassPosition(probeId);
        const JPH::Vec3  v = bi.GetLinearVelocity(probeId);
        ContactProbeFrame s;
        s.frame = frame;
        s.t     = frame * cfg.dt;
        s.y     = static_cast<float>(p.GetY());
        s.vy    = v.GetY();
        r.trajectory.push_back(s);
    };

    r.trajectory.reserve(static_cast<std::size_t>(cfg.frames) + 1);
    pushFrame(0);
    for (int frame = 1; frame <= cfg.frames; ++frame) {
        collector.beginFrame(frame);
        physicsSystem.Update(static_cast<float>(cfg.dt), 1, &tempAllocator, &jobSystem);
        collector.finishFrame();          // stable sort this frame's slice
        pushFrame(frame);
    }
    // Detach BEFORE teardown: Jolt may emit Removed on the NEXT Update after a
    // body is removed (ContactListener.h:126) -- those are outside the probe window.
    physicsSystem.SetContactListener(nullptr);
    r.events = collector.takeEvents();
    r.removedCacheMisses = collector.cacheMisses();

    for (auto rit = ids.rbegin(); rit != ids.rend(); ++rit) {
        bi.RemoveBody(*rit);
        bi.DestroyBody(*rit);
    }
    return r;
}

// ---- M-P2: stacking stability ----------------------------------------------
// UE Chaos counterpart (read only): IslandManager sleeps a whole island when
// SleepLinearThresholdSq / SleepAngularThresholdSq stay under for
// SleepCounterThreshold frames. Jolt exposes the same idea as
// PhysicsSettings::mAllowSleeping + mTimeBeforeSleep +
// mPointVelocitySleepThreshold. We keep the solver untouched and only READ the
// active-body count, so the TSV shows whether the tower really went to sleep.
bool validateStack(const StackConfig& cfg, std::string* err) {
    auto bad = [err](const char* reason) -> bool {
        if (err) *err = reason;
        return false;
    };
    if (cfg.frames < 60 || cfg.frames > 100000) return bad("frames out of range [60,100000]");
    if (!(cfg.dt >= 1.0 / 1000.0 && cfg.dt <= 1.0 / 30.0)) return bad("dt out of range [0.001,0.033333]");
    if (!(cfg.gravity > 0.0f && cfg.gravity <= 1000.0f)) return bad("gravity out of range (0,1000]");
    if (cfg.boxes < 2 || cfg.boxes > 16) return bad("boxes out of range [2,16]");
    if (!(cfg.halfExtent > 0.0f && cfg.halfExtent <= 10.0f)) return bad("halfExtent out of range (0,10]");
    if (!(cfg.gap >= 0.0f && cfg.gap <= cfg.halfExtent)) return bad("gap out of range [0,halfExtent]");
    if (!(cfg.friction >= 0.0f && cfg.friction <= 1.0f)) return bad("friction out of range [0,1]");
    if (!(cfg.restitution >= 0.0f && cfg.restitution <= 1.0f)) return bad("restitution out of range [0,1]");
    if (!(cfg.lateralSpeed >= 0.0f && cfg.lateralSpeed <= 10.0f)) return bad("lateralSpeed out of range [0,10]");
    if (!(cfg.damping >= 0.0f && cfg.damping <= 1.0f)) return bad("damping out of range [0,1]");
    if (cfg.jobsThreads < 0 || cfg.jobsThreads > 64) return bad("jobsThreads out of range [0,64]");
    return true;
}

StackResult World::simulateStack(const StackConfig& cfg) {
    StackResult r;
    if (!validateStack(cfg, &r.error)) return r;
    bootstrap();

    // Same filter cadence as M-P0/M-P1: all pairs on except (floor,floor).
    LayerMatrix matrix;
    matrix.set(layers::kFloor, layers::kFloor, false);
    JPH::TempAllocatorImpl tempAllocator(10 * 1024 * 1024);
    // M-J5c：job system 按配置分流；声明在 physicsSystem 之前 = 析构在其之后。
    JobRuntime jobRuntime = makeJobRuntime(cfg.jobsThreads);
    JPH::JobSystem* jobSystem = jobRuntime.sys;
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter(matrix);
    ObjectLayerPairFilterImpl objectLayerPairFilter(matrix);
    JPH::PhysicsSystem physicsSystem;

    physicsSystem.Init(1024, 0, 1024, 1024,
                       bpLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter);
    JPH::PhysicsSettings settings;
    settings.mAllowSleeping = cfg.allowSleeping;
    physicsSystem.SetPhysicsSettings(settings);
    physicsSystem.SetGravity(JPH::Vec3(0.0f, -cfg.gravity, 0.0f));
    JPH::BodyInterface& bi = physicsSystem.GetBodyInterface();

    JPH::BoxShapeSettings floorShapeSettings(JPH::Vec3(100.0f, 1.0f, 100.0f));
    floorShapeSettings.SetEmbedded();
    auto floorShapeResult = floorShapeSettings.Create();
    if (floorShapeResult.HasError()) { r.error = "floor shape creation failed"; return r; }
    JPH::ShapeRefC floorShape = floorShapeResult.Get();

    const JPH::Vec3 he(cfg.halfExtent, cfg.halfExtent, cfg.halfExtent);
    JPH::BoxShapeSettings boxShapeSettings(he);
    boxShapeSettings.SetEmbedded();
    auto boxShapeResult = boxShapeSettings.Create();
    if (boxShapeResult.HasError()) { r.error = "box shape creation failed"; return r; }
    JPH::ShapeRefC boxShape = boxShapeResult.Get();

    std::vector<JPH::BodyID> ids;
    auto add = [&](const JPH::ShapeRefC& shape, JPH::RVec3 pos, JPH::EMotionType motion,
                   std::uint32_t layer) -> bool {
        JPH::BodyCreationSettings s(shape, pos, JPH::Quat::sIdentity(), motion,
                                    static_cast<JPH::ObjectLayer>(layer));
        s.mFriction        = cfg.friction;
        s.mRestitution     = cfg.restitution;
        s.mAllowSleeping   = cfg.allowSleeping;
        s.mLinearDamping   = cfg.damping;
        s.mAngularDamping  = cfg.damping;
        JPH::Body* body = bi.CreateBody(s);
        if (!body) return false;
        const JPH::BodyID id = body->GetID();
        bi.AddBody(id, motion == JPH::EMotionType::Static
                           ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        ids.push_back(id);
        return true;
    };

    if (!add(floorShape, JPH::RVec3(0.0_r, -1.0_r, 0.0_r),
             JPH::EMotionType::Static, layers::kFloor)) {
        r.error = "floor body creation failed";
        return r;
    }
    const float pitch = 2.0f * cfg.halfExtent + cfg.gap;
    for (std::uint32_t i = 0; i < cfg.boxes; ++i) {
        const float y = cfg.halfExtent + static_cast<float>(i) * pitch;
        if (!add(boxShape, JPH::RVec3(0.0_r, JPH::Real(y), 0.0_r),
                 JPH::EMotionType::Dynamic, layers::kDefault)) {
            r.error = "box body creation failed";
            return r;
        }
        if (i + 1 == cfg.boxes && cfg.lateralSpeed > 0.0f)
            bi.SetLinearVelocity(ids.back(), JPH::Vec3(cfg.lateralSpeed, 0.0f, 0.0f));
    }

    physicsSystem.OptimizeBroadPhase();
    impl_->lastBodyCount = physicsSystem.GetNumBodies();

    r.frames.reserve(static_cast<std::size_t>(cfg.frames) + 1);
    auto pushFrame = [&](int frame) {
        StackFrame f;
        f.frame = frame;
        f.t = frame * cfg.dt;
        f.activeBodies = physicsSystem.GetNumActiveBodies(JPH::EBodyType::RigidBody);
        f.bodies.resize(cfg.boxes);
        for (std::uint32_t i = 0; i < cfg.boxes; ++i) {
            const JPH::BodyID id = ids[static_cast<std::size_t>(i) + 1];
            const JPH::RVec3 p = bi.GetCenterOfMassPosition(id);
            const JPH::Vec3  v = bi.GetLinearVelocity(id);
            BodySample& bs = f.bodies[i];
            bs.position = { static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                            static_cast<float>(p.GetZ()) };
            bs.velocity = { v.GetX(), v.GetY(), v.GetZ() };
            bs.active = bi.IsActive(id);
        }
        r.frames.push_back(std::move(f));
    };

    pushFrame(0);
    for (int frame = 1; frame <= cfg.frames; ++frame) {
        physicsSystem.Update(static_cast<float>(cfg.dt), 1, &tempAllocator, jobSystem);
        pushFrame(frame);
    }

    const StackFrame& last = r.frames.back();
    r.finalActive = last.activeBodies;
    const int tail = std::min(60, static_cast<int>(r.frames.size()));
    const int from = static_cast<int>(r.frames.size()) - tail;
    for (int k = from; k < static_cast<int>(r.frames.size()); ++k) {
        for (std::uint32_t i = 0; i < cfg.boxes; ++i) {
            const BodySample& b = r.frames[static_cast<std::size_t>(k)].bodies[i];
            const float dy = std::abs(b.position.y - last.bodies[i].position.y);
            r.maxTailSpread = std::max(r.maxTailSpread, dy);
            const nl::math::Vec3& v = b.velocity;
            const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            r.maxTailSpeed = std::max(r.maxTailSpeed, speed);
        }
    }
    for (std::uint32_t i = 0; i < cfg.boxes; ++i) {
        const BodySample& b = last.bodies[i];
        const float drift = std::sqrt(b.position.x * b.position.x + b.position.z * b.position.z);
        r.maxDrift = std::max(r.maxDrift, drift);
        const float expectedY = cfg.halfExtent + static_cast<float>(i) * 2.0f * cfg.halfExtent;
        if (b.position.y < expectedY - cfg.halfExtent) r.collapsed = true;
    }
    const BodySample& top = last.bodies[cfg.boxes - 1];
    r.topDrift = std::sqrt(top.position.x * top.position.x + top.position.z * top.position.z);
    r.topY = top.position.y;

    for (auto rit = ids.rbegin(); rit != ids.rend(); ++rit) {
        bi.RemoveBody(*rit);
        bi.DestroyBody(*rit);
    }
    return r;
}

// ---- M-P4: character walk-through (UE CharacterMovement-style) --------------
// UE counterpart (read only): CharacterMovementComponent::MoveAlongFloor /
// FindFloor / MaxSlopeAngle (Engine/Classes/GameFramework/
// CharacterMovementComponent.h). Jolt CharacterVirtual maps 1:1 onto that model
// (Update sweep + StickToFloor + WalkStairs + GetGroundState). Carrot uses the
// other flavor (JPH::Character kinematic rigid body, engine/physics/Character.cpp)
// with no CharacterVirtual reference; the plan says "CharacterMovement-style
// sweeping", so CharacterVirtual is the right tool.
// CharacterVirtual owns NO rigid body -- it is not stepped by
// PhysicsSystem::Update. Gravity/input are composed app-side each frame (Jolt
// docs on Update: "it's your own responsibility to apply gravity").
//
// Position semantics (pinned from CharacterVirtual.cpp/h, not guessed):
//   GetCenterOfMassTransform = sRotationTranslation(rot, mPosition)
//     .PreTranslated(shapeOffset + COM).PostTranslated(padding * up)
//   => for a capsule (COM=0, offset=0) the shape origin (cylinder center)
//   sits at mPosition + padding*up, i.e. mPosition is the CAPSULE CENTER and
//   feet y = mPosition.y - (halfCylinder + radius - padding).
//
// Course (static, y-up), walked along +x, centered near the origin so the
// viewer orbit camera (pivot = origin, dist clamp <= 20) can frame it all:
//   floor top y=0
//   character starts at x=-13, walks +x
//   30 deg ramp x in [-7,-3]  -> top y=kCharRampTop (=4*tan30, walkable)
//   platform1 x in [-3,5]     -> top y=kCharRampTop
//   3 steps (0.3 up / 0.6 deep) x in [5,6.8] -> top y=kCharStepTop (WalkStairs)
//   platform2 x in [6.8,11]   -> top y=kCharStepTop
//   60 deg slope x in [11,13] -> BLOCKED (maxSlope 50 deg), character parks
//   safety wall x in [16.5,17.5]
constexpr float kCharRampTop   = 2.3094f;  // 4 * tan(30 deg)
constexpr float kCharStepTop   = 3.2094f;  // kCharRampTop + 3 * 0.3
constexpr float kCharPadding   = 0.02f;    // Jolt default mCharacterPadding
constexpr float kCharFeetToPos = 1.08f;    // halfCyl 0.6 + radius 0.5 - padding

// Shared harness: World::simulateCharacter (headless full run) and
// DebugScene scenario=1 (viewer per-frame) step the exact same course + cadence,
// so viewer screenshots and tool TSVs are two views of one simulation.
struct CharacterHarness {
    // Declaration order = construction order; destruction is reverse, so
    // character (declared after physicsSystem) dies first and filters outlive
    // the system -- same discipline as DebugScene::Impl (M-P3).
    LayerMatrix matrix;
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilterImpl objectLayerPairFilter;
    JPH::TempAllocatorImpl tempAllocator{10 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobSystem{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem physicsSystem;
    JPH::BodyInterface* bodyInterface = nullptr;
    JPH::Ref<JPH::CharacterVirtual> character;
    std::vector<JPH::BodyID> courseIds;  // ramp/steps/platforms/steep slope (green)
    JPH::BodyID floorId;
    float gravity   = 9.8f;
    float walkSpeed = 1.5f;
    std::string error;

    CharacterHarness()
        : objectVsBroadPhaseLayerFilter(matrix)
        , objectLayerPairFilter(matrix)
    {}

    bool build(const CharacterConfig& cfg) {
        matrix.set(layers::kFloor, layers::kFloor, false);
        physicsSystem.Init(1024, 0, 1024, 1024, bpLayerInterface,
                           objectVsBroadPhaseLayerFilter, objectLayerPairFilter);
        physicsSystem.SetGravity(JPH::Vec3(0.0f, -cfg.gravity, 0.0f));
        bodyInterface = &physicsSystem.GetBodyInterface();
        JPH::BodyInterface& bi = *bodyInterface;
        gravity = cfg.gravity;
        walkSpeed = cfg.walkSpeed;

        auto addStatic = [&](const JPH::Vec3& half, JPH::RVec3Arg pos,
                             const JPH::Quat& rot) -> JPH::BodyID {
            JPH::BoxShapeSettings s(half);
            s.SetEmbedded();
            auto r = s.Create();
            if (r.HasError()) { error = "static box shape failed"; return JPH::BodyID(); }
            JPH::BodyCreationSettings bcs(r.Get(), pos, rot, JPH::EMotionType::Static,
                                          static_cast<JPH::ObjectLayer>(layers::kFloor));
            JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
            if (id.IsInvalid()) error = "static body failed";
            return id;
        };

        const JPH::Quat rotZ30 = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(),
                                                      JPH::DegreesToRadians(30.0f));
        const JPH::Quat rotZ60 = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(),
                                                      JPH::DegreesToRadians(60.0f));
        // floor: half-height 0.5 box, top y=0, spans x in [-30,60]
        floorId = addStatic(JPH::Vec3(45.0f, 0.5f, 10.0f), JPH::RVec3(2.0_r, -0.5_r, 0.0_r),
                            JPH::Quat::sIdentity());
        if (floorId.IsInvalid()) return false;
        // 30 deg ramp: rotated slab; axial half-length 2.30945 -> horizontal 2.0, rise 1.1547
        {
            JPH::BodyID id = addStatic(JPH::Vec3(2.30945f, 0.25f, 5.0f),
                                       JPH::RVec3(-5.0_r, JPH::Real(kCharRampTop * 0.5f), 0.0_r),
                                       rotZ30);
            if (id.IsInvalid()) return false;
            courseIds.push_back(id);
        }
        // platform1: top y=kCharRampTop, x in [10,18]
        {
            JPH::BodyID id = addStatic(JPH::Vec3(4.0f, 0.25f, 5.0f),
                                       JPH::RVec3(1.0_r, JPH::Real(kCharRampTop - 0.25f), 0.0_r),
                                       JPH::Quat::sIdentity());
            if (id.IsInvalid()) return false;
            courseIds.push_back(id);
        }
        // 3 steps: full-height pillars from the ground, tops 2.6094/2.9094/3.2094
        for (int i = 0; i < 3; ++i) {
            const float top = kCharRampTop + 0.3f * static_cast<float>(i + 1);
            const float cx  = 5.3f + 0.6f * static_cast<float>(i);
            JPH::BodyID id = addStatic(JPH::Vec3(0.3f, top * 0.5f, 5.0f),
                                       JPH::RVec3(JPH::Real(cx), JPH::Real(top * 0.5f), 0.0_r),
                                       JPH::Quat::sIdentity());
            if (id.IsInvalid()) return false;
            courseIds.push_back(id);
        }
        // platform2: top y=kCharStepTop, x in [19.8,24]
        {
            JPH::BodyID id = addStatic(JPH::Vec3(2.1f, 0.25f, 5.0f),
                                       JPH::RVec3(8.9_r, JPH::Real(kCharStepTop - 0.25f), 0.0_r),
                                       JPH::Quat::sIdentity());
            if (id.IsInvalid()) return false;
            courseIds.push_back(id);
        }
        // 60 deg slope: axial half-length 2.0 -> horizontal 1.0, rise 1.732; blocks the walk
        {
            JPH::BodyID id = addStatic(JPH::Vec3(2.0f, 0.25f, 5.0f),
                                       JPH::RVec3(12.0_r, JPH::Real(kCharStepTop + 1.73205f), 0.0_r),
                                       rotZ60);
            if (id.IsInvalid()) return false;
            courseIds.push_back(id);
        }
        // safety wall (anchor backstop; the 60 deg slope should stop the walk first)
        {
            JPH::BodyID id = addStatic(JPH::Vec3(0.5f, 3.0f, 5.0f),
                                       JPH::RVec3(17.0_r, 3.0_r, 0.0_r),
                                       JPH::Quat::sIdentity());
            if (id.IsInvalid()) return false;
            courseIds.push_back(id);
        }

        // Character: capsule r=0.5 halfCyl=0.6 (total 2.2 m), feet start 0.02 above floor.
        JPH::CapsuleShapeSettings capSettings(cfg.halfCylinder, cfg.radius);
        capSettings.SetEmbedded();
        auto capRes = capSettings.Create();
        if (capRes.HasError()) { error = "capsule shape failed"; return false; }

        JPH::CharacterVirtualSettings cs;
        cs.mShape = capRes.Get();
        cs.mMaxSlopeAngle = JPH::DegreesToRadians(cfg.maxSlopeDeg);
        cs.mUp = JPH::Vec3::sAxisY();
        // No mInnerBodyShape: pure virtual body (CharacterMovement-style, simplest form).
        // No inner body => no BodyID in the world => DebugRenderer will not draw it;
        // DebugScene::extractLines draws the capsule manually for scenario=1.
        character = new JPH::CharacterVirtual(&cs, JPH::RVec3(-13.0_r, JPH::Real(1.10f), 0.0_r),
                                              JPH::Quat::sIdentity(), &physicsSystem);
        if (!character) { error = "character creation failed"; return false; }
        physicsSystem.OptimizeBroadPhase();
        return true;
    }

    // One walk step. Jolt cadence (CharacterVirtual.h Update doc):
    //  - OnGround: velocity = ground velocity (0 here, static course) + input
    //    + dt*gravity (presses onto the ground/ramp so the sweep keeps contact)
    //  - else:     velocity = current vertical + input + dt*gravity
    void advance(float dt) {
        const JPH::CharacterBase::EGroundState gs = character->GetGroundState();
        JPH::Vec3 v;
        if (gs == JPH::CharacterBase::EGroundState::OnGround)
            v = JPH::Vec3(walkSpeed, -gravity * dt, 0.0f);
        else
            v = JPH::Vec3(walkSpeed,
                          character->GetLinearVelocity().GetY() - gravity * dt, 0.0f);
        character->SetLinearVelocity(v);
        // Jolt defaults: stickDown {0,-0.5,0} / stepUp {0,0.4,0} (explicit for provenance).
        JPH::CharacterVirtual::ExtendedUpdateSettings ext;
        character->ExtendedUpdate(dt, JPH::Vec3(0.0f, -gravity, 0.0f), ext,
                                  JPH::DefaultBroadPhaseLayerFilter(objectVsBroadPhaseLayerFilter,
                                      static_cast<JPH::ObjectLayer>(layers::kDefault)),
                                  JPH::DefaultObjectLayerFilter(objectLayerPairFilter,
                                      static_cast<JPH::ObjectLayer>(layers::kDefault)),
                                  JPH::BodyFilter(), JPH::ShapeFilter(), tempAllocator);
    }

    CharacterSample sample(int frame, double t) const {
        CharacterSample s;
        s.frame = frame;
        s.t = t;
        const JPH::RVec3 p = character->GetPosition();
        s.position = { static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                       static_cast<float>(p.GetZ()) };
        const JPH::Vec3 v = character->GetLinearVelocity();
        s.velocity = { v.GetX(), v.GetY(), v.GetZ() };
        s.groundState = static_cast<int>(character->GetGroundState());
        s.groundNormalY = character->GetGroundNormal().GetY();
        return s;
    }
};

bool validateCharacter(const CharacterConfig& cfg, std::string* err) {
    auto bad = [err](const char* reason) -> bool {
        if (err) *err = reason;
        return false;
    };
    if (cfg.frames < 60 || cfg.frames > 100000) return bad("frames out of range [60,100000]");
    if (!(cfg.dt >= 1.0 / 1000.0 && cfg.dt <= 1.0 / 30.0)) return bad("dt out of range [0.001,0.033333]");
    if (!(cfg.gravity > 0.0f && cfg.gravity <= 1000.0f)) return bad("gravity out of range (0,1000]");
    if (!(cfg.radius > 0.0f && cfg.radius <= 5.0f)) return bad("radius out of range (0,5]");
    if (!(cfg.halfCylinder >= 0.0f && cfg.halfCylinder <= 5.0f)) return bad("halfCylinder out of range [0,5]");
    if (!(cfg.walkSpeed >= 0.0f && cfg.walkSpeed <= 20.0f)) return bad("walkSpeed out of range [0,20]");
    if (!(cfg.maxSlopeDeg > 0.0f && cfg.maxSlopeDeg <= 85.0f)) return bad("maxSlopeDeg out of range (0,85]");
    return true;
}

CharacterResult World::simulateCharacter(const CharacterConfig& cfg) {
    CharacterResult r;
    if (!validateCharacter(cfg, &r.error)) return r;
    bootstrap();

    CharacterHarness h;   // fresh world per call: the determinism contract
    if (!h.build(cfg)) { r.error = h.error; return r; }

    double t = 0.0;
    r.samples.reserve(static_cast<std::size_t>(cfg.frames));
    for (int frame = 0; frame < cfg.frames; ++frame) {
        h.advance(static_cast<float>(cfg.dt));
        t += cfg.dt;
        r.samples.push_back(h.sample(frame, t));
    }
    if (!r.samples.empty()) {
        const CharacterSample& last = r.samples.back();
        r.finalX = last.position.x;
        r.finalY = last.position.y;
    }
    return r;
}

// ---- M-P3：物理调试场景（碰撞形状线框） ----
// Jolt 头只进本 cpp。线框经 POD 视图（const float* + 顶点数）暴露，World.hpp 零 Jolt 类型。
// 套路与 Carrot / Jolt 双实证一致：物理引擎只「喊」图元（DrawBox/DrawSphere/... ->
// DrawGeometry），本类收顶点，不自己画几何。
namespace {
using namespace JPH;   // 本 anon namespace 内的 JoltLineRenderer 用到 RMat44Arg/AABox/ECullMode 等 JPH 类型

// Jolt DebugRenderer 子类：把所有图元收进一个浮点线顶点流（LINE_LIST 语义）。
class JoltLineRenderer final : public JPH::DebugRenderer {
public:
    JoltLineRenderer() { Initialize(); }

    // 每帧 extractLines 前 clear 后重填。
    std::vector<float> lines;

    virtual void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override {
        push(inFrom, inColor);
        push(inTo, inColor);
    }
    // 只取线框，忽略实心三角与 3D 文本。
    virtual void DrawTriangle(JPH::RVec3Arg, JPH::RVec3Arg, JPH::RVec3Arg,
                             JPH::ColorArg, ECastShadow) override {}
    virtual void DrawText3D(JPH::RVec3Arg, const std::string_view&, JPH::ColorArg, float) override {}

    // 三角形批：存顶点 + 索引，DrawGeometry 据此走边。
    struct BatchData final : public JPH::RefTargetVirtual {
        std::vector<JPH::DebugRenderer::Vertex> verts;
        std::vector<std::uint32_t> indices;
        mutable std::atomic<int> refCount{0};
        virtual void AddRef() override { refCount.fetch_add(1, std::memory_order_relaxed); }
        virtual void Release() override {
            if (refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
        }
    };

    virtual Batch CreateTriangleBatch(const Triangle* inTriangles, int inTriangleCount) override {
        auto* b = new BatchData();
        b->verts.reserve(static_cast<std::size_t>(inTriangleCount) * 3u);
        b->indices.reserve(static_cast<std::size_t>(inTriangleCount) * 3u);
        for (int i = 0; i < inTriangleCount; ++i) {
            const Triangle& t = inTriangles[i];
            const std::uint32_t base = static_cast<std::uint32_t>(b->verts.size());
            b->verts.push_back(t.mV[0]); b->verts.push_back(t.mV[1]); b->verts.push_back(t.mV[2]);
            b->indices.push_back(base); b->indices.push_back(base + 1u); b->indices.push_back(base + 2u);
        }
        return Batch(b);
    }
    virtual Batch CreateTriangleBatch(const Vertex* inVertices, int inVertexCount,
                                     const std::uint32_t* inIndices, int inIndexCount) override {
        auto* b = new BatchData();
        b->verts.assign(inVertices, inVertices + inVertexCount);
        b->indices.assign(inIndices, inIndices + inIndexCount);
        return Batch(b);
    }
    virtual void DrawGeometry(RMat44Arg inModelMatrix, const AABox&, float,
                              JPH::ColorArg inModelColor, const GeometryRef& inGeometry,
                              ECullMode, ECastShadow, EDrawMode) override {
        if (!inGeometry) return;
        // 取最细 LOD：线框叠加层要清晰可读，细分多几段（盒/胶囊各档边结构一致，
        // 球/胶囊细分越多越像「球/胶囊」而非星状）。代价是顶点略多，debug 场景可忽略。
        const LOD& lod = inGeometry->mLODs.front();
        BatchData* batch = static_cast<BatchData*>(lod.mTriangleBatch.GetPtr());
        if (!batch) return;
        const std::size_t n = batch->indices.size();
        for (std::size_t i = 0; i + 2u < n; i += 3u) {
            const Vertex& a = batch->verts[batch->indices[i]];
            const Vertex& b = batch->verts[batch->indices[i + 1u]];
            const Vertex& c = batch->verts[batch->indices[i + 2u]];
            const JPH::RVec3 pa = inModelMatrix * JPH::Vec3(a.mPosition.x, a.mPosition.y, a.mPosition.z);
            const JPH::RVec3 pb = inModelMatrix * JPH::Vec3(b.mPosition.x, b.mPosition.y, b.mPosition.z);
            const JPH::RVec3 pc = inModelMatrix * JPH::Vec3(c.mPosition.x, c.mPosition.y, c.mPosition.z);
            DrawLine(pa, pb, inModelColor);   // virtual -> 收集顶点
            DrawLine(pb, pc, inModelColor);
            DrawLine(pc, pa, inModelColor);
        }
    }

private:
    static void push(std::vector<float>& out, JPH::RVec3Arg p, JPH::ColorArg c) {
        out.push_back(static_cast<float>(p.GetX()));
        out.push_back(static_cast<float>(p.GetY()));
        out.push_back(static_cast<float>(p.GetZ()));
        out.push_back(c.r / 255.0f);
        out.push_back(c.g / 255.0f);
        out.push_back(c.b / 255.0f);
    }
    void push(JPH::RVec3Arg p, JPH::ColorArg c) { push(lines, p, c); }
};

} // namespace

struct DebugScene::Impl {
    std::uint32_t totalBodies = 0;
    std::string error;
    int scenario = 0;
    bool valid = false;

    // Jolt 生命周期对象（声明顺序 = 构造顺序；析构逆序，保证 filters 与 matrix
    // 长于 physicsSystem，与 simulateDrop 同纪律）。
    JPH::TempAllocatorImpl tempAllocator{10 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobSystem{JPH::cMaxPhysicsJobs};
    LayerMatrix matrix;
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilterImpl objectLayerPairFilter;
    JPH::PhysicsSystem physicsSystem;
    JPH::BodyInterface* bodyInterface = nullptr;
    std::vector<JPH::BodyID> dynamicIds;
    JPH::BodyID floorId;
    std::unique_ptr<JoltLineRenderer> renderer;
    std::vector<float> lineBuffer;   // extractLines 持久输出，供本帧 renderFrame 消费
    // M-P4：scenario=1 时全接管（自带 PhysicsSystem + CharacterVirtual 走查场），
    // Impl 的 physicsSystem 不 Init、不 Update；scenario=0 时为空。
    std::unique_ptr<CharacterHarness> charH;

    Impl()
        : objectVsBroadPhaseLayerFilter(matrix)
        , objectLayerPairFilter(matrix)
    {}
};

DebugScene::DebugScene() = default;
DebugScene::~DebugScene() = default;

bool DebugScene::build(int scenario, std::uint32_t seed, std::string* err) {
    World::bootstrap();
    impl_ = std::make_unique<Impl>();
    auto& I = *impl_;
    I.scenario = scenario;
    (void)seed;

    // M-P4：scenario=1 = 角色走查场（地板 -> 30° 斜坡 -> 平台 -> 3 级台阶 ->
    // 平台 -> 60° 斜坡），CharacterHarness 全接管，与 phys-tools character
    // 命令同一套场地与步进节奏（确定性契约同源）。
    if (scenario == 1) {
        I.charH = std::make_unique<CharacterHarness>();
        CharacterConfig cc;   // 默认口径：r0.5/halfCyl0.6、walk 1.5、maxSlope 50°
        if (!I.charH->build(cc)) {
            if (err) *err = I.charH->error.c_str();
            I.error = I.charH->error;
            return false;
        }
        I.renderer = std::make_unique<JoltLineRenderer>();
        I.valid = true;
        return true;
    }

    // floor/floor 禁用；其余 pair 全开（M-P0 同口径）。
    I.matrix.set(layers::kFloor, layers::kFloor, false);

    I.physicsSystem.Init(1024, 0, 1024, 1024,
                         I.bpLayerInterface, I.objectVsBroadPhaseLayerFilter, I.objectLayerPairFilter);
    I.physicsSystem.SetGravity(JPH::Vec3(0.0f, -9.8f, 0.0f));
    I.bodyInterface = &I.physicsSystem.GetBodyInterface();
    JPH::BodyInterface& bi = *I.bodyInterface;

    // 静态地板：薄盒（半高 0.5）顶面 y=0。
    {
        JPH::BoxShapeSettings floorShapeSettings(JPH::Vec3(5.0f, 0.5f, 5.0f));
        floorShapeSettings.SetEmbedded();
        auto fr = floorShapeSettings.Create();
        if (fr.HasError()) { if (err) *err = "floor shape failed"; I.error = "floor shape failed"; return false; }
        JPH::BodyCreationSettings s(fr.Get(), JPH::RVec3(0.0_r, -0.5_r, 0.0_r),
                                    JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kFloor);
        JPH::Body* b = bi.CreateBody(s);
        if (!b) { if (err) *err = "floor body failed"; I.error = "floor body failed"; return false; }
        I.floorId = b->GetID();
        bi.AddBody(I.floorId, JPH::EActivation::DontActivate);
    }

    // 三个落体：盒 / 球 / 胶囊，x 方向错开避免初始重叠。
    auto addDynamic = [&](const JPH::ShapeRefC& shape, JPH::RVec3 pos) {
        JPH::BodyCreationSettings s(shape, pos, JPH::Quat::sIdentity(),
                                    JPH::EMotionType::Dynamic, layers::kDefault);
        s.mRestitution = 0.4f;
        s.mFriction = 0.6f;
        s.mLinearDamping = 0.0f;
        JPH::BodyID id = bi.CreateAndAddBody(s, JPH::EActivation::Activate);
        if (!id.IsInvalid()) I.dynamicIds.push_back(id);
    };
    addDynamic(new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f)), JPH::RVec3(-2.0_r, 4.0_r, 0.0_r));
    addDynamic(new JPH::SphereShape(0.5f), JPH::RVec3(0.0_r, 4.0_r, 0.0_r));
    addDynamic(new JPH::CapsuleShape(0.5f, 0.3f), JPH::RVec3(2.0_r, 4.0_r, 0.0_r));

    I.physicsSystem.OptimizeBroadPhase();
    I.totalBodies = I.physicsSystem.GetNumBodies();
    I.renderer = std::make_unique<JoltLineRenderer>();
    I.valid = true;
    return true;
}

void DebugScene::step(double dt) {
    if (!impl_ || !impl_->valid) return;
    auto& I = *impl_;
    if (I.charH) {
        // M-P4：CharacterVirtual 不占 PhysicsSystem::Update（无真实体），
        // 走查步进独立于此处；场景全是静态体，物理 Update 本身无事可做。
        I.charH->advance(static_cast<float>(dt));
        return;
    }
    I.physicsSystem.Update(static_cast<float>(dt), 1, &I.tempAllocator, &I.jobSystem);
}

DebugLines DebugScene::extractLines() {
    DebugLines out;
    if (!impl_ || !impl_->valid) return out;
    // scenario=1 时 Impl 的 bodyInterface 为空（charH 接管），不能在此拦截。
    if (!impl_->bodyInterface && !impl_->charH) return out;
    auto& I = *impl_;
    I.lineBuffer.clear();
    I.renderer->lines.clear();

    // M-P4：走查场线框——地板蓝、场地几何绿、角色胶囊亮黄（手动补画：
    // CharacterVirtual 无真实体，DebugRenderer 不会自动画它）。
    if (I.charH) {
        CharacterHarness& h = *I.charH;
        JPH::BodyInterface& bi = *h.bodyInterface;
        auto drawH = [&](JPH::BodyID id, const JPH::Color& color) {
            JPH::RefConst<JPH::Shape> shape = bi.GetShape(id);
            if (!shape) return;
            const JPH::RMat44 xf = bi.GetCenterOfMassTransform(id);
            shape->Draw(I.renderer.get(), xf, JPH::Vec3::sOne(), color, false, true);
        };
        drawH(h.floorId, JPH::Color(60, 120, 255));
        for (JPH::BodyID id : h.courseIds) drawH(id, JPH::Color(80, 220, 80));
        // 胶囊 shape 原点（圆柱中心）= mPosition + padding*up（COM=0、offset=0，
        // 见 M-P4 段位置语义注释）；halfHeightOfCylinder = cfg halfCylinder 0.6。
        const JPH::RVec3 pos = h.character->GetPosition();
        const JPH::RMat44 capXf = JPH::RMat44::sTranslation(
            pos + JPH::RVec3(0.0_r, JPH::Real(kCharPadding), 0.0_r));
        I.renderer->DrawCapsule(capXf, 0.6f, 0.5f, JPH::Color(255, 230, 60));
        I.lineBuffer.swap(I.renderer->lines);
        out.data = I.lineBuffer.data();
        out.vertexCount = static_cast<std::uint32_t>(I.lineBuffer.size() / 6u);
        return out;
    }

    JPH::BodyInterface& bi = *I.bodyInterface;
    auto drawBody = [&](JPH::BodyID id, const JPH::Color& color) {
        JPH::RefConst<JPH::Shape> shape = bi.GetShape(id);
        if (!shape) return;
        const JPH::RMat44 xf = bi.GetCenterOfMassTransform(id);
        // 物理引擎只「喊」图元；JoltLineRenderer 收顶点（线框走边）。
        shape->Draw(I.renderer.get(), xf, JPH::Vec3::sOne(), color, false, true);
    };

    // 地板蓝；落体按形状分色（盒绿 / 球橙 / 胶囊品红 / 其它青）。
    drawBody(I.floorId, JPH::Color(60, 120, 255));
    for (JPH::BodyID id : I.dynamicIds) {
        JPH::RefConst<JPH::Shape> shape = bi.GetShape(id);
        if (!shape) continue;
        // 盒/球/胶囊的 EShapeType 都是 Convex，区分三者要看 EShapeSubType。
        const JPH::EShapeSubType t = shape->GetSubType();
        JPH::Color c;
        if (t == JPH::EShapeSubType::Box) c = JPH::Color(80, 220, 80);
        else if (t == JPH::EShapeSubType::Sphere) c = JPH::Color(255, 160, 40);
        else if (t == JPH::EShapeSubType::Capsule) c = JPH::Color(230, 80, 230);
        else c = JPH::Color(80, 220, 220);
        drawBody(id, c);
    }

    // 收集到的线流搬进持久缓冲（供本帧 renderFrame 消费）。
    I.lineBuffer.swap(I.renderer->lines);
    out.data = I.lineBuffer.data();
    out.vertexCount = static_cast<std::uint32_t>(I.lineBuffer.size() / 6u);
    return out;
}

const char* DebugScene::lastError() const { return impl_ ? impl_->error.c_str() : "not built"; }
std::uint32_t DebugScene::bodyCount() const {
    if (!impl_) return 0;
    if (impl_->charH) return impl_->charH->physicsSystem.GetNumBodies();
    return impl_->totalBodies;
}


} // namespace nl::physics
