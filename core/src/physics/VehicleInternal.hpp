#pragma once
// P8 M-D3: INTERNAL-ONLY vehicle assembly contract (core/src side).
//
// Q-D3-1 A (user decision 2026-11-10): the car lives in DebugScene scenario 2's
// long-lived JPH::PhysicsSystem. M-D2's promise of a reusable
// buildVehicle(PhysicsSystem&) was NOT implemented at M-D2 close -- assembly was
// inlined in simulateDrive(). M-D3 really extracts it here so BOTH callers share
// one assembly sequence:
//   * nl::physics::simulateDrive()      -> fresh world, frame-indexed script
//   * nl::physics::DebugScene::build(2) -> long-lived world, real-time input
//
// This header MAY use Jolt types: it lives under core/src and is never exposed
// through core/include. nl/physics/World.hpp stays Jolt-free (POD views only).
//
// Layer-filter contract: VehicleCollisionTesterRay casts with the FLOOR object
// layer against the floor body, so the caller's PhysicsSystem must allow
// floor/floor in BOTH filter stages. M-D2 uses all-true filters; DebugScene
// scenario 2 must NOT apply the scenario-0 I.matrix.set(kFloor,kFloor,false).

#include <Jolt/Jolt.h> // must be the first Jolt header

#include "nl/physics/Vehicle.hpp"

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <string>

namespace nl::physics::detail {

struct VehicleAssembly {
    JPH::Ref<JPH::VehicleConstraint> constraint;  // caller's world owns a ref via AddConstraint
    JPH::WheeledVehicleController* controller = nullptr; // borrowed from constraint
    JPH::Body* chassis = nullptr;                // borrowed from physicsSystem
    JPH::BodyID chassisId;
    JPH::BodyID floorId;                        // filled in by the caller
};

// Creates the chassis box, four WheelSettingsWV wheels, the
// WheeledVehicleControllerSettings (explicit differential left=2/right=3), the
// VehicleConstraint and its VehicleCollisionTesterRay, then registers the
// constraint + step listener on `ps`. Does NOT create the floor and does NOT
// call OptimizeBroadPhase(): both are the caller's job (M-D2 keeps its exact
// original call order so drive.tsv stays byte-identical; DebugScene adds the
// training-ground wall before optimizing).
bool buildVehicle(JPH::PhysicsSystem& ps,
                  const VehicleTuning& tuning,
                  const JPH::RVec3& chassisStart,
                  VehicleAssembly* out,
                  std::string* err);

} // namespace nl::physics::detail
