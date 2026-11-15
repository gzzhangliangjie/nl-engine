#pragma once

// nl/physics — M-PQ0 general physics query API (wave 1 / A-class track).
//
// Jolt-free public surface: consumers of nl_core never include Jolt headers and
// never see JPH:: types. All Jolt symbols live in core/src/physics/Query.cpp.
//
// Design (docs/p8/PHYQUERY-PLAN.md, user decision 2026-11-15 option A minimal
// surface): three POD query families + one filter + one hit record. No UE-style
// channel/profile matrix, no std::function callbacks (Carrot's thick wrapper
// shape is explicitly rejected), no async queries.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nl/math/math.hpp"

namespace nl::physics {

// bit i == objects on layers::* value i may be hit. Default: all 4 layers.
struct QueryFilter {
    std::uint32_t       layerMask = 0x0000000Fu;
    std::vector<std::uint64_t> ignoreBodyIds; // JPH::BodyID raw values
};

struct RayQuery {
    nl::math::Vec3 origin{};
    nl::math::Vec3 direction{};   // must be unit length; scaled by maxT internally
    float          maxT = 1000.0f;
    QueryFilter    filter{};
};

struct ShapeDesc {
    enum Kind : std::uint32_t { Sphere = 0, Box = 1, Capsule = 2 };
    Kind  kind = Box;
    // Sphere  : dims[0] = radius
    // Box     : dims[0..2] = half extents
    // Capsule : dims[0] = half height of cylinder, dims[1] = radius
    float dims[3] = { 0.5f, 0.5f, 0.5f };
};

struct SweepQuery {
    ShapeDesc      shape{};
    nl::math::Vec3 from{};
    nl::math::Vec3 to{};
    nl::math::Quat rotation{};
    QueryFilter    filter{};
};

struct OverlapQuery {
    ShapeDesc      shape{};
    nl::math::Vec3 position{};
    nl::math::Quat rotation{};
    QueryFilter    filter{};
};

struct QueryHit {
    bool           hit        = false;
    float          fraction   = 0.0f;  // 0..1 along the query
    float          distance   = 0.0f;  // world units
    nl::math::Vec3 point{};
    nl::math::Vec3 normal{};
    std::uint32_t  bodyId     = 0;     // BodyID::GetIndexAndSequenceNumber()
    std::uint32_t  subShapeId = 0;
    std::uint32_t  layer      = 0;
    std::uint64_t  owner      = 0;     // engine-side id stored in Jolt userData
};

// Fresh deterministic query world. NOT the engine's live DebugScene world:
// sharing that instance is a separate milestone (M-PQ1).
//
// Fixture layout (all static, built by build()):
//   slot 0 floor   Box(100,1,100) @ (0,-1,0)      layer kFloor   owner 'FLOOR'
//   slot 1 boxA    Box(1,1,1)     @ (0, 1, 5)      layer kDefault owner 'BOXA'
//   slot 2 boxB    Box(1,1,1)     @ (0, 1,10)      layer kDebris  owner 'BOXB'
//   slot 3 boxC    Box(1,1,1)     @ (0, 1,15)      layer kDefault owner 'BOXC'
//   slot 4 sensor  Sphere(0.5)    @ (0, 1,20)      layer kGhost   owner 'SENS'
class QueryScene {
public:
    QueryScene();
    ~QueryScene();
    QueryScene(const QueryScene&) = delete;
    QueryScene& operator=(const QueryScene&) = delete;

    bool build(std::string* err);
    bool ready() const;
    std::uint32_t bodyCount() const;
    std::uint32_t bodyIdBySlot(int slot) const; // 0 on invalid slot
    std::uint64_t ownerBySlot(int slot) const;

    QueryHit         raycast(const RayQuery& q) const;
    QueryHit         sweep  (const SweepQuery& q) const;
    std::vector<QueryHit> overlap(const OverlapQuery& q, std::size_t maxHits) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool validateRayQuery   (const RayQuery&    q, std::string* err);
bool validateSweepQuery (const SweepQuery&  q, std::string* err);
bool validateOverlapQuery(const OverlapQuery& q, std::string* err);

} // namespace nl::physics
