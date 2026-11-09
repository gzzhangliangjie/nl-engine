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
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>

using namespace JPH::literals;

namespace {

// ---- Layers: two object layers / two broadphase trees (Jolt HelloWorld cadence) ----
namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING     = 1;
constexpr JPH::uint        NUM_LAYERS = 2;
}

namespace BroadPhaseLayers {
const JPH::BroadPhaseLayer NON_MOVING(0);
const JPH::BroadPhaseLayer MOVING(1);
constexpr JPH::uint        NUM_LAYERS = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return inLayer == Layers::NON_MOVING ? BroadPhaseLayers::NON_MOVING
                                            : BroadPhaseLayers::MOVING;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        return inLayer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        // Static floor never collides with another static body.
        return inLayer1 == Layers::NON_MOVING ? inLayer2 == BroadPhaseLayers::MOVING : true;
    }
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        return !(inObject1 == Layers::NON_MOVING && inObject2 == Layers::NON_MOVING);
    }
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
    JPH::TempAllocatorImpl tempAllocator(10 * 1024 * 1024);
    JPH::JobSystemSingleThreaded jobSystem(JPH::cMaxPhysicsJobs);
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilterImpl objectLayerPairFilter;
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
        JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING);
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
        JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::MOVING);
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

} // namespace nl::physics
