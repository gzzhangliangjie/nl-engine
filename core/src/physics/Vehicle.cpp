// P8 M-D2: Jolt VehicleConstraint integration. Integration layer ONLY --
// no solver code, no graphics symbols, no kStage. Jolt types stay in this TU.
//
// Assembly order is dictated by vendored headers (see Vehicle.hpp evidence):
//   AddBody(floor/chassis) -> new VehicleConstraint(body, settings)
//   -> SetVehicleCollisionTester(new VehicleCollisionTesterRay(kFloor))
//   -> AddConstraint -> AddStepListener -> Update(dt, 1, ...)
// VehicleConstraint.h:44 explicitly requires the StepListener; OnStep is the
// only driver (PhysicsSystem.cpp:649) and VehicleConstraint.cpp:219/232
// dereferences the collision tester with no null check.
//
// Wheel contact events are synthesized from HasContact() edges because Jolt's
// wheels are raycast queries, not bodies. The resulting ContactEvent PODs feed
// M-D1's ContactEventStream inside the same logic frame (update -> publish ->
// drain), which is the first real gameplay consumption of that contract.
// M-D0's ContactCollector / simulateContactProbe are untouched.
//
// M-D3 (Q-D3-1 A): chassis/wheel/constraint assembly is now detail::buildVehicle()
// in this TU and shared with DebugScene scenario 2 (VehicleInternal.hpp).
// simulateDrive() keeps its exact floor creation, OptimizeBroadPhase and frame
// loop order so the M-D2 drive.tsv baseline stays byte-identical.

#include <Jolt/Jolt.h> // must be the first Jolt header

#include "nl/physics/Vehicle.hpp"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleDifferential.h>
#include <Jolt/Physics/Vehicle/Wheel.h>

#include "nl/physics/World.hpp"                 // bootstrap(), layers, kLayerCount
#include "nl/physics/ContactEventStream.hpp"    // M-D1 consumer contract
#include "VehicleInternal.hpp"                  // M-D3: shared assembly contract

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

// Same 4-layer / 1:1 broadphase shape as World.cpp (referenced, not owned by
// PhysicsSystem, so these outlive it). Every pair collides here: M-P0 disables
// floor/floor for its byte baseline, but VehicleCollisionTesterRay casts with
// the FLOOR object layer against the floor body -- disabling floor/floor would
// make every wheel ray miss and the chassis would simply sit on its box.
// Kept local to this TU so World.cpp stays byte-identical.
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return nl::physics::kLayerCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(inLayer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "LAYER"; }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
};

constexpr float kPi = 3.14159265358979323846f;

float wrapDelta(float from, float to) {
    float d = to - from;
    while (d > kPi) d -= 2.0f * kPi;
    while (d < -kPi) d += 2.0f * kPi;
    return d;
}

} // namespace

namespace nl::physics {

namespace detail {

bool buildVehicle(JPH::PhysicsSystem& ps,
                  const VehicleTuning& t,
                  const JPH::RVec3& chassisStart,
                  VehicleAssembly* out,
                  std::string* err) {
    auto fail = [err](const char* reason) -> bool {
        if (err) *err = reason;
        return false;
    };
    JPH::BodyInterface& bodyInterface = ps.GetBodyInterface();

    // Chassis box. Jolt derives mass from density * volume; set the density so
    // the configured mass is respected without overriding mass properties.
    const float volume = 8.0f * t.halfWidth * t.halfHeight * t.halfLength;
    JPH::BoxShapeSettings chassisShapeSettings(
        JPH::Vec3(t.halfWidth, t.halfHeight, t.halfLength));
    chassisShapeSettings.SetEmbedded();
    chassisShapeSettings.mDensity = t.mass / volume;
    auto chassisShapeResult = chassisShapeSettings.Create();
    if (chassisShapeResult.HasError()) return fail("chassis shape creation failed");
    JPH::BodyCreationSettings chassisBCS(
        chassisShapeResult.Get(),
        chassisStart,
        JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic,
        static_cast<JPH::ObjectLayer>(layers::kDefault));
    chassisBCS.mUserData = 1;
    chassisBCS.mLinearDamping = 0.0f;
    chassisBCS.mAngularDamping = 0.0f;
    JPH::Body* chassis = bodyInterface.CreateBody(chassisBCS);
    if (!chassis) return fail("chassis body creation failed");
    bodyInterface.AddBody(chassis->GetID(), JPH::EActivation::Activate);

    // Vehicle settings: up/forward are normalized (VehicleConstraint.cpp:87-88
    // asserts), wheels must be WheelSettingsWV (:160 ConstructWheel asserts).
    JPH::VehicleConstraintSettings vehicleSettings;
    vehicleSettings.mUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
    vehicleSettings.mForward = JPH::Vec3(0.0f, 0.0f, 1.0f);
    vehicleSettings.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);

    JPH::WheeledVehicleControllerSettings* controllerSettings =
        new JPH::WheeledVehicleControllerSettings();
    JPH::VehicleDifferentialSettings diff;
    diff.mLeftWheel = 2;              // defaults are -1 (VehicleDifferential.h:31-32)
    diff.mRightWheel = 3;
    diff.mEngineTorqueRatio = 1.0f;   // sum over differentials must be 1
    controllerSettings->mDifferentials.push_back(diff);
    vehicleSettings.mController = controllerSettings;

    const float wx[4] = {-t.wheelOffsetX, t.wheelOffsetX, -t.wheelOffsetX, t.wheelOffsetX};
    const float wz[4] = { t.wheelOffsetZ, t.wheelOffsetZ, -t.wheelOffsetZ, -t.wheelOffsetZ};
    for (int i = 0; i < 4; ++i) {
        JPH::WheelSettingsWV* w = new JPH::WheelSettingsWV();
        w->mPosition = JPH::Vec3(wx[i], t.wheelAttachY, wz[i]);
        w->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
        w->mSteeringAxis = JPH::Vec3(0.0f, 1.0f, 0.0f);
        w->mWheelUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
        w->mWheelForward = JPH::Vec3(0.0f, 0.0f, 1.0f);
        w->mSuspensionMinLength = t.suspMin;
        w->mSuspensionMaxLength = t.suspMax;
        w->mSuspensionSpring = JPH::SpringSettings(
            JPH::ESpringMode::FrequencyAndDamping, t.suspFrequency, t.suspDamping);
        w->mRadius = t.wheelRadius;
        w->mWidth = t.wheelWidth;
        w->mMaxSteerAngle = (i < 2) ? JPH::DegreesToRadians(t.maxSteerDeg) : 0.0f;
        vehicleSettings.mWheels.push_back(w);
    }

    JPH::Ref<JPH::VehicleConstraint> constraint =
        new JPH::VehicleConstraint(*chassis, vehicleSettings);
    constraint->SetVehicleCollisionTester(
        new JPH::VehicleCollisionTesterRay(static_cast<JPH::ObjectLayer>(layers::kFloor)));
    ps.AddConstraint(constraint.GetPtr());
    ps.AddStepListener(constraint.GetPtr());

    // Publish the borrowed handles. AddConstraint() already AddRef'd the
    // constraint, so the caller's Ref outlives this local Ref.
    out->constraint = constraint;
    out->controller = static_cast<JPH::WheeledVehicleController*>(constraint->GetController());
    out->chassis = chassis;
    out->chassisId = chassis->GetID();
    return true;
}

} // namespace detail

bool validateDriveConfig(const DriveConfig& cfg, std::string* err) {
    auto bad = [err](const char* reason) -> bool {
        if (err) *err = reason;
        return false;
    };
    if (cfg.frames < 1 || cfg.frames > 100000) return bad("frames out of range [1,100000]");
    if (!(cfg.dt >= 1.0 / 1000.0 && cfg.dt <= 1.0 / 30.0)) return bad("dt out of range [0.001,0.033333]");
    if (!(cfg.gravity > 0.0f && cfg.gravity <= 1000.0f)) return bad("gravity out of range (0,1000]");
    if (!(cfg.accelEnd > 0 && cfg.accelEnd < cfg.turnEnd && cfg.turnEnd <= cfg.frames))
        return bad("phase bounds must satisfy 0 < accelEnd < turnEnd <= frames");
    auto in01 = [](float v) { return v >= 0.0f && v <= 1.0f; };
    if (!in01(cfg.accelForward) || !in01(cfg.turnForward) ||
        !in01(cfg.turnRight) || !in01(cfg.handBrake))
        return bad("driver inputs out of range [0,1]");
    const VehicleTuning& t = cfg.tuning;
    if (!(t.halfWidth > 0.0f && t.halfHeight > 0.0f && t.halfLength > 0.0f))
        return bad("chassis half extents must be positive");
    if (!(t.mass > 0.0f && t.mass < 100000.0f)) return bad("mass out of range (0,100000)");
    if (!(t.wheelRadius > 0.0f && t.wheelWidth > 0.0f)) return bad("wheel size must be positive");
    if (!(t.suspMin > 0.0f && t.suspMax > t.suspMin)) return bad("suspension lengths invalid");
    if (!(t.suspFrequency > 0.0f && t.suspDamping > 0.0f)) return bad("suspension spring invalid");
    if (!(t.maxSteerDeg > 0.0f && t.maxSteerDeg <= 80.0f)) return bad("maxSteerDeg out of range (0,80]");
    if (!(t.wheelOffsetX > 0.0f && t.wheelOffsetZ > 0.0f)) return bad("wheel offsets must be positive");
    if (!(t.startY > 0.0f)) return bad("startY must be positive");
    return true;
}

DriveResult simulateDrive(const DriveConfig& cfg) {
    DriveResult r;
    if (!validateDriveConfig(cfg, &r.error)) return r;
    World::bootstrap();

    const VehicleTuning& t = cfg.tuning;
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

    // Static floor: box centered at y=-1 so its top face is y=0.
    JPH::BoxShapeSettings floorShapeSettings(JPH::Vec3(100.0f, 1.0f, 100.0f));
    floorShapeSettings.SetEmbedded();
    auto floorShapeResult = floorShapeSettings.Create();
    if (floorShapeResult.HasError()) {
        r.error = "floor shape creation failed";
        return r;
    }
    JPH::BodyCreationSettings floorBCS(
        floorShapeResult.Get(),
        JPH::RVec3(JPH::Real(0), JPH::Real(-1), JPH::Real(0)),
        JPH::Quat::sIdentity(), JPH::EMotionType::Static,
        static_cast<JPH::ObjectLayer>(layers::kFloor));
    floorBCS.mUserData = 0;
    JPH::BodyID floorId = bodyInterface.CreateAndAddBody(floorBCS, JPH::EActivation::DontActivate);
    if (floorId.IsInvalid()) {
        r.error = "floor body creation failed";
        return r;
    }

    // M-D3 (Q-D3-1 A): the very same assembly helper is used by
    // DebugScene::build(2). Floor creation and OptimizeBroadPhase stay here so
    // M-D2's call order / drive.tsv stay byte-identical.
    detail::VehicleAssembly assembly;
    if (!detail::buildVehicle(physicsSystem, t,
                             JPH::RVec3(JPH::Real(0), JPH::Real(t.startY), JPH::Real(0)),
                             &assembly, &r.error)) {
        return r;
    }
    assembly.floorId = floorId;
    physicsSystem.OptimizeBroadPhase();

    auto* controller = assembly.controller;
    auto& joltWheels = assembly.constraint->GetWheels();
    JPH::Body* const chassis = assembly.chassis;


    ContactEventStream stream;
    bool prevContact[4] = {false, false, false, false};
    std::uint64_t prevDropped = 0;
    r.frames.reserve(static_cast<std::size_t>(cfg.frames) + 1);
    r.wheels.reserve((static_cast<std::size_t>(cfg.frames) + 1) * 4);

    for (int frame = 0; frame <= cfg.frames; ++frame) {
        stream.update(); // Physics group, frame start (M-D1 contract)
        const int phase = frame < cfg.accelEnd ? 0 : (frame < cfg.turnEnd ? 1 : 2);
        float forward = 0.0f, right = 0.0f, brake = 0.0f, handBrake = 0.0f;
        if (phase == 0) {
            forward = cfg.accelForward;
        } else if (phase == 1) {
            forward = cfg.turnForward;
            right = cfg.turnRight;
        } else {
            handBrake = cfg.handBrake;
        }
        controller->SetDriverInput(forward, right, brake, handBrake);
        if (frame > 0) {
            physicsSystem.Update(static_cast<float>(cfg.dt), 1, &tempAllocator, &jobSystem);
        }

        const JPH::RVec3 p = chassis->GetPosition();
        const JPH::Vec3 v = chassis->GetLinearVelocity();
        const JPH::Vec3 fwd = chassis->GetRotation() * JPH::Vec3(0.0f, 0.0f, 1.0f);

        DriveFrame fr;
        fr.frame = frame;
        fr.phase = phase;
        fr.t = frame * cfg.dt;
        fr.forward = forward;
        fr.right = right;
        fr.brake = brake;
        fr.handBrake = handBrake;
        fr.position = {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                       static_cast<float>(p.GetZ())};
        fr.velocity = {v.GetX(), v.GetY(), v.GetZ()};
        fr.yaw = std::atan2(fwd.GetX(), fwd.GetZ());
        fr.speed = std::sqrt(v.GetX() * v.GetX() + v.GetZ() * v.GetZ());

        std::vector<ContactEvent> produced;
        for (int i = 0; i < 4; ++i) {
            const JPH::Wheel* wheel = joltWheels[static_cast<std::size_t>(i)];
            const bool now = wheel->HasContact();
            DriveWheelSample ws;
            ws.frame = frame;
            ws.wheel = i;
            ws.contact = now ? 1 : 0;
            ws.suspensionLength = wheel->GetSuspensionLength();
            ws.rotationAngle = wheel->GetRotationAngle();
            ws.steerAngle = wheel->GetSteerAngle();
            r.wheels.push_back(ws);
            if (now) ++fr.grounded;

            ContactKind kind;
            if (!prevContact[i] && now) kind = ContactKind::Added;
            else if (prevContact[i] && now) kind = ContactKind::Persisted;
            else if (prevContact[i] && !now) kind = ContactKind::Removed;
            else { prevContact[i] = now; continue; }

            ContactEvent e;
            e.frame = frame;
            e.kind = kind;
            e.ownerA = kDriveWheelOwnerBase + static_cast<std::uint64_t>(i);
            e.ownerB = kDriveFloorOwner;
            e.bodyIdA = kDriveWheelSlotBase + static_cast<std::uint32_t>(i);
            e.bodyIdB = floorId.GetIndexAndSequenceNumber();
            e.normalY = 1.0f;  // floor normal; raycast wheels have no BodyID
            produced.push_back(e);
            prevContact[i] = now;
        }

        stream.publish(produced);
        const std::vector<ContactEvent> consumed = stream.drain(); // PostPhysics
        fr.published = static_cast<std::uint32_t>(produced.size());
        fr.consumed = static_cast<std::uint32_t>(consumed.size());
        fr.dropped = static_cast<std::uint32_t>(stream.droppedTotal() - prevDropped);
        prevDropped = stream.droppedTotal();
        r.frames.push_back(fr);
        r.events.insert(r.events.end(), consumed.begin(), consumed.end());
    }

    r.publishedTotal = stream.publishedTotal();
    r.consumedTotal = stream.drainedTotal();
    r.droppedTotal = stream.droppedTotal();

    // Diagnostic anchors (drive-check recomputes them from the F rows only).
    constexpr float kReachSpeed = 1.0f;
    constexpr float kStopSpeed = 0.2f;
    for (const DriveFrame& f : r.frames) {
        if (f.speed > r.maxSpeed) r.maxSpeed = f.speed;
        if (f.phase == 0 && r.reachSpeedFrame < 0 && f.speed >= kReachSpeed)
            r.reachSpeedFrame = f.frame;
        if (f.phase == 2 && r.stopFrame < 0 && f.speed <= kStopSpeed)
            r.stopFrame = f.frame;
    }
    auto yawAt = [&](int f) {
        return r.frames[static_cast<std::size_t>(std::min(f, cfg.frames))].yaw;
    };
    const float y0 = yawAt(cfg.accelEnd);
    const float y1 = yawAt(cfg.turnEnd);
    const float y2 = yawAt(cfg.frames);
    r.finalSpeed = r.frames[static_cast<std::size_t>(std::min(cfg.accelEnd, cfg.frames))].speed;
    r.brakeStartSpeed = r.frames[static_cast<std::size_t>(std::min(cfg.turnEnd, cfg.frames))].speed;
    r.brakeEndSpeed = r.frames[static_cast<std::size_t>(cfg.frames)].speed;
    r.turnYawDelta = wrapDelta(y0, y1);
    r.brakeYawDelta = wrapDelta(y1, y2);
    const auto& p0 = r.frames[static_cast<std::size_t>(std::min(cfg.accelEnd, cfg.frames))].position;
    const auto& p1 = r.frames[static_cast<std::size_t>(std::min(cfg.turnEnd, cfg.frames))].position;
    const float dx = p1.x - p0.x, dz = p1.z - p0.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    const float ayaw = std::fabs(r.turnYawDelta);
    r.turnRadius = ayaw > 1.0e-3f ? dist / ayaw : 0.0f;

    // Detach before teardown: the step listener is a raw pointer in Jolt.
    physicsSystem.RemoveStepListener(assembly.constraint.GetPtr());
    physicsSystem.RemoveConstraint(assembly.constraint.GetPtr());
    return r;
}

} // namespace nl::physics
