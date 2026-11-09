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
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

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
    JPH::JobSystemSingleThreaded jobSystem(JPH::cMaxPhysicsJobs);
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
        physicsSystem.Update(static_cast<float>(cfg.dt), 1, &tempAllocator, &jobSystem);
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
    JPH::JobSystemSingleThreaded jobSystem(JPH::cMaxPhysicsJobs);
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
        physicsSystem.Update(static_cast<float>(cfg.dt), 1, &tempAllocator, &jobSystem);
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


} // namespace nl::physics
