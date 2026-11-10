#pragma once
// P8 M-D2: headless wheeled-vehicle integration (A-class parallel track).
//
// Q-D2-1 (user decision 2026-11-10) = plan A "probe-style fresh world":
//   simulateDrive() builds a brand-new JPH::PhysicsSystem on every call, runs a
//   frame-indexed input script to completion, and returns POD views only.
//   World.hpp:226-228 keeps the "fresh world per call => bit-identical" contract;
//   M-D3 will reuse the internal buildVehicle(PhysicsSystem&) helper to attach a
//   car to DebugScene scenario 2. Ownership of a long-lived world is NOT decided
//   here. See docs/p8/D2-VEHICLE-OWNERSHIP-OPTIONS.md and P8-DRIVE-PLAN.md §3.
//
// Three-source evidence (M-D2 open-day re-read, NOT M-D0/D1 evidence):
//  - Carrot engine/engine/physics/ (16 files): zero Vehicle / StepListener /
//    Wheeled hits; only a generic raycast and a SixDOFConstraint. No vehicle
//    assembly reference exists there.
//  - bevy crates/ (65 official crates): no VehicleController, no official
//    physics; the only "vehicle" hit is a teaching Component example in
//    bevy_ecs/src/component/mod.rs:52. Honestly recorded as "no counterpart".
//  - UE 5.8.4 ChaosVehicles (read only, not copied): four separate input APIs
//    (ChaosVehicleMovementComponent.h:887/899/903/919) and a per-frame cached
//    FWheelStatus[] read back by index (:716/:955). Only the "separate inputs +
//    state readback" idea is borrowed; Jolt has no equivalent normalized fields.
//  - vendored Jolt Physics/Vehicle/ (24 files): VehicleConstraint.h:44 requires
//    AddStepListener, :69/:79/:127/:130/:136/:142 are the assembly API;
//    WheeledVehicleController.h:19/:105/:160 requires WheelSettingsWV and a
//    downcast from the base VehicleController*; VehicleDifferential.h:31-32
//    defaults both wheel indices to -1 (must be filled explicitly);
//    VehicleCollisionTesterRay is the ray tester (VehicleCollisionTester.h:84).
//    No Jolt Samples / VehicleTest are vendored: assembly is derived from headers.
//
// Header is Jolt-free: no JPH types, no Jolt includes. Only ContactEvent (M-D0
// POD) is reused so M-D1's ContactEventStream can consume real gameplay data.
//
// IMPORTANT implementation note (vendored evidence, not an architecture change):
// Jolt's wheels are raycast queries (VehicleCollisionTesterRay::Collide), NOT
// rigid bodies, so they emit no ContactListener callbacks. The M-D2 "wheel
// contact event sequence" is therefore synthesized from per-wheel HasContact()
// edges (Added / Persisted / Removed) and converted to ContactEvent PODs; the
// chassis box stays above the floor and never touches it. bodyIdA is a stable
// synthetic wheel-slot id, worldPos is left zero, normalY is the ground normal.
// M-D0's ContactCollector / simulateContactProbe are untouched.

#include <cstdint>
#include <string>
#include <vector>

#include "nl/math/math.hpp"
#include "nl/physics/Events.hpp"

namespace nl::physics {

// Stable synthetic ids for wheel contact events (raycast wheels have no BodyID).
constexpr std::uint32_t kDriveWheelSlotBase = 0xD200u;
constexpr std::uint64_t kDriveWheelOwnerBase = 0x4E4C563200000001ull; // "NLV2" + slot
constexpr std::uint64_t kDriveFloorOwner = 0x4E4C563200000005ull;

struct VehicleTuning {
    float halfWidth      = 0.9f;   // chassis box half extents (m)
    float halfHeight     = 0.4f;
    float halfLength     = 2.0f;
    float mass           = 1500.0f;
    float wheelRadius    = 0.3f;
    float wheelWidth     = 0.1f;
    float suspMin        = 0.3f;
    float suspMax        = 0.5f;
    float suspFrequency  = 1.5f;
    float suspDamping    = 0.5f;
    float maxSteerDeg    = 30.0f;  // front axle only; Jolt default is 70 deg
    float wheelOffsetX   = 0.9f;
    float wheelOffsetZ   = 1.4f;   // +Z is forward (mForward = (0,0,1))
    float wheelAttachY   = -0.2f;  // suspension attachment point, body-local
    float startY         = 1.0f;   // chassis center height (suspMax + radius - attachY)
};

// Frame-indexed input script with three phases (M-I2 --input-demo precedent):
//   [0, accelEnd)   phase 0 forward=accelForward
//   [accelEnd, turnEnd) phase 1 forward=turnForward, right=turnRight
//   [turnEnd, frames]   phase 2 handBrake=handBrake
// Extra frames beyond turnEnd ONLY extend the handbrake observation window;
// accelEnd/turnEnd stay fixed. Measured M-D2 pitfall: scaling all three phases
// with --frames made top speed grow faster than the brake window, so stop_frame
// stayed -1 for N=360/480/600/720. A longer run must never mean a faster car.
struct DriveConfig {
    int    frames        = 480;
    double dt            = 1.0 / 60.0;
    float  gravity       = 9.8f;
    int    accelEnd      = 150;
    int    turnEnd       = 270;
    float  accelForward  = 1.0f;
    float  turnForward   = 0.6f;
    float  turnRight     = 1.0f;
    float  handBrake     = 1.0f;   // Jolt takes a float 0..1, NOT UE's bool
    VehicleTuning tuning{};
};

struct DriveFrame {
    int    frame        = 0;
    int    phase        = 0;       // 0 accel / 1 turn / 2 handbrake
    double t            = 0.0;
    float  forward      = 0.0f;
    float  right        = 0.0f;
    float  brake        = 0.0f;
    float  handBrake    = 0.0f;
    nl::math::Vec3 position{};
    nl::math::Vec3 velocity{};
    float  yaw          = 0.0f;    // atan2(forward.x, forward.z), radians
    float  speed        = 0.0f;    // horizontal speed (m/s)
    int    grounded     = 0;       // wheels with HasContact()
    std::uint32_t published = 0;   // events pushed into ContactEventStream this frame
    std::uint32_t consumed   = 0;  // events drained by the PostPhysics consumer
    std::uint32_t dropped    = 0;  // unread events discarded by update()
};

struct DriveWheelSample {
    int   frame            = 0;
    int   wheel            = 0;
    int   contact          = 0;
    float suspensionLength = 0.0f;
    float rotationAngle    = 0.0f;
    float steerAngle       = 0.0f;
};

struct DriveResult {
    std::vector<DriveFrame>       frames;   // frames + 1 rows (frame 0..frames)
    std::vector<DriveWheelSample> wheels;   // (frames + 1) * 4 rows
    std::vector<ContactEvent>     events;   // consumed by M-D1 ContactEventStream

    // Diagnostic anchors, recomputed independently by drive-check from F rows.
    int   reachSpeedFrame = -1;
    float maxSpeed        = 0.0f;
    float finalSpeed      = 0.0f;   // speed at accelEnd
    float turnYawDelta    = 0.0f;
    float turnRadius      = 0.0f;
    int   stopFrame       = -1;
    float brakeStartSpeed = 0.0f;   // speed at turnEnd (handbrake entry)
    float brakeEndSpeed   = 0.0f;   // speed at the final frame
    float brakeYawDelta   = 0.0f;

    std::uint64_t publishedTotal = 0;
    std::uint64_t consumedTotal  = 0;
    std::uint64_t droppedTotal   = 0;
    std::string   error;
    bool ok() const { return error.empty(); }
};

// Shared validation for phys-tools CLI and selftest. err receives an ASCII reason.
bool validateDriveConfig(const DriveConfig& cfg, std::string* err);

// Build a fresh headless world with a static floor + one 4-wheel vehicle, step
// it through the frame-indexed script, and return per-frame / per-wheel / event
// views. Determinism: fresh world + fixed dt + JobSystemSingleThreaded.
DriveResult simulateDrive(const DriveConfig& cfg);

} // namespace nl::physics
