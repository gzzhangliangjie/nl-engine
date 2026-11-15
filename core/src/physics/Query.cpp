// nl/physics — M-PQ0 general physics query API, Jolt 5.3.1 mapping.
//
// This is the ONLY translation unit where JPH:: symbols appear for the query
// surface. Public header nl/physics/query.hpp stays Jolt-free (PIMPL).
//
// Three-source evidence (2026-11-15, AGENTS.md decision-first rule):
//  - Carrot engine/engine/physics/PhysicsSystem.cpp:142-248 -- raycast only,
//    public header leaks JPH types and std::function. Rejected as thick wrapper.
//  - bevy: no official physics crate, no query surface to mirror.
//  - UE 5.8.4 Engine/Classes/Engine/World.h (read only) -- LineTrace / Sweep /
//    Overlap x Single / Multi is the industrial minimal complete set.
// We take UE's three-family split but collapse UE's channel/profile into one
// QueryFilter (layerMask + ignoreBodyIds), because no current game consumes
// channels and a hollow abstraction would be worse than none.

#include <Jolt/Jolt.h> // must be first Jolt header

#include "nl/physics/query.hpp"
#include "nl/physics/World.hpp"  // bootstrap(), layers::*, LayerMatrix

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nl::physics {
namespace {

// ---- PhysicsSystem assembly: 1:1 object->broadphase layer mapping, all pairs
// allowed. The QUERY filter (QueryFilter::layerMask) is applied separately at
// NarrowPhaseQuery call sites; it must NOT be confused with the simulation-side
// LayerMatrix used by World::simulate*(). ----
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return kLayerCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(inLayer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        static const char* kNames[kLayerCount] = { "FLOOR", "DEFAULT", "DEBRIS", "GHOST" };
        return kNames[inLayer.GetValue()];
    }
#endif
};

class AllPairsVsBPLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};

class AllPairsObjectFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
};

// Explicit NarrowPhaseQuery filters. Jolt's default `{}` filters are ALL-PASS;
// without these a query would hit layers disabled by layerMask (e.g. kGhost).
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryLayerFilter(std::uint32_t mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer inLayer) const override {
        const std::uint32_t layer = static_cast<std::uint32_t>(inLayer);
        return layer < 32u && ((mask_ >> layer) & 1u) != 0u;
    }
private:
    std::uint32_t mask_;
};

class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const std::vector<std::uint64_t>& ignored) : ignored_(ignored) {}
    bool ShouldCollide(const JPH::BodyID& inBodyID) const override {
        const std::uint64_t raw = static_cast<std::uint64_t>(
            inBodyID.GetIndexAndSequenceNumber());
        return std::find(ignored_.begin(), ignored_.end(), raw) == ignored_.end();
    }
private:
    const std::vector<std::uint64_t>& ignored_;
};

JPH::Quat toJolt(const nl::math::Quat& q) {
    return JPH::Quat(q.x, q.y, q.z, q.w);
}
JPH::RVec3 toJolt(const nl::math::Vec3& v) {
    return JPH::RVec3(JPH::Real(v.x), JPH::Real(v.y), JPH::Real(v.z));
}
JPH::Vec3 toJoltV3(const nl::math::Vec3& v) {
    return JPH::Vec3(v.x, v.y, v.z);
}
// One template works for both Vec3 and DVec3 (RVec3 == Vec3 when
// JPH_DOUBLE_PRECISION is off; a pair of overloads would be a redefinition).
template <class VecT>
nl::math::Vec3 fromJolt(const VecT& v) {
    return { static_cast<float>(v.GetX()), static_cast<float>(v.GetY()),
             static_cast<float>(v.GetZ()) };
}

// convexRadius = 0 keeps tiny half extents legal (BoxShape asserts
// halfExtent >= convexRadius otherwise).
JPH::ShapeRefC makeShape(const ShapeDesc& d) {
    switch (d.kind) {
    case ShapeDesc::Sphere:
        return new JPH::SphereShape(d.dims[0]);
    case ShapeDesc::Capsule:
        return new JPH::CapsuleShape(d.dims[0], d.dims[1]);
    case ShapeDesc::Box:
    default:
        return new JPH::BoxShape(JPH::Vec3(d.dims[0], d.dims[1], d.dims[2]), 0.0f);
    }
}

bool validDims(const ShapeDesc& d, std::string* err) {
    const std::size_t n = (d.kind == ShapeDesc::Sphere) ? 1u
                        : (d.kind == ShapeDesc::Capsule) ? 2u : 3u;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(d.dims[i] > 0.0f) || !std::isfinite(d.dims[i])) {
            if (err) *err = "shape dims must be finite and > 0";
            return false;
        }
    }
    return true;
}

float lengthV3(const nl::math::Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

struct FixtureBody {
    ShapeDesc      shape{};
    nl::math::Vec3 position{};
    std::uint32_t  layer = 0;
    std::uint64_t  owner = 0;
};

constexpr std::uint64_t kOwnerFloor  = 0x464c4f4f52000000ull; // "FLOOR"
constexpr std::uint64_t kOwnerBoxA   = 0x424f584100000001ull; // "BOXA"
constexpr std::uint64_t kOwnerBoxB   = 0x424f584200000002ull; // "BOXB"
constexpr std::uint64_t kOwnerBoxC   = 0x424f584300000003ull; // "BOXC"
constexpr std::uint64_t kOwnerSensor = 0x53454e5300000004ull; // "SENS"

FixtureBody fixture(int slot) {
    FixtureBody b;
    ShapeDesc box;
    box.kind = ShapeDesc::Box;
    box.dims[0] = box.dims[1] = box.dims[2] = 1.0f;
    switch (slot) {
    case 0:
        b.shape = box;
        b.shape.dims[0] = 100.0f; b.shape.dims[1] = 1.0f; b.shape.dims[2] = 100.0f;
        b.position = { 0.0f, -1.0f, 0.0f };   // top face y = 0
        b.layer = layers::kFloor;
        b.owner = kOwnerFloor;
        break;
    case 1:
        b.shape = box; b.position = { 0.0f, 1.0f, 5.0f };   // near face z = 4
        b.layer = layers::kDefault; b.owner = kOwnerBoxA;
        break;
    case 2:
        b.shape = box; b.position = { 0.0f, 1.0f, 10.0f };  // near face z = 9
        b.layer = layers::kDebris; b.owner = kOwnerBoxB;
        break;
    case 3:
        b.shape = box; b.position = { 0.0f, 1.0f, 15.0f };
        b.layer = layers::kDefault; b.owner = kOwnerBoxC;
        break;
    default:
        b.shape.kind = ShapeDesc::Sphere; b.shape.dims[0] = 0.5f;
        b.position = { 0.0f, 1.0f, 20.0f };
        b.layer = layers::kGhost; b.owner = kOwnerSensor;
        break;
    }
    return b;
}

} // namespace

// ---------------------------------------------------------------------------
// QueryScene::Impl
// ---------------------------------------------------------------------------
struct QueryScene::Impl {
    // Declaration order = reverse destruction order: filters hold no refs here
    // but physicsSystem holds POINTERS to them, so they must outlive it.
    AllPairsVsBPLayerFilter  objectVsBroadPhaseLayerFilter;
    AllPairsObjectFilter     objectLayerPairFilter;
    BPLayerInterfaceImpl     bpLayerInterface;
    JPH::PhysicsSystem       physicsSystem;
    std::vector<std::uint32_t> bodyIds;
    std::vector<std::uint64_t> owners;
    bool built = false;

    std::uint32_t bodyIdBySlot(int slot) const {
        return (slot >= 0 && static_cast<std::size_t>(slot) < bodyIds.size())
             ? bodyIds[static_cast<std::size_t>(slot)] : 0u;
    }
    std::uint64_t ownerBySlot(int slot) const {
        return (slot >= 0 && static_cast<std::size_t>(slot) < owners.size())
             ? owners[static_cast<std::size_t>(slot)] : 0ull;
    }

    // Fill the common part of a hit from a BodyID.
    void decorate(QueryHit* out, const JPH::BodyID& id,
                  std::uint32_t subShapeId, const nl::math::Vec3& fallbackNormal) const {
        out->bodyId     = id.GetIndexAndSequenceNumber();
        out->subShapeId = subShapeId;
        out->owner      = physicsSystem.GetBodyInterface().GetUserData(id);
        JPH::BodyLockRead lock(physicsSystem.GetBodyLockInterface(), id);
        if (lock.Succeeded()) {
            out->layer = static_cast<std::uint32_t>(lock.GetBody().GetObjectLayer());
        }
        (void)fallbackNormal;
    }
};

QueryScene::QueryScene() : impl_(std::make_unique<Impl>()) {}
QueryScene::~QueryScene() = default;

bool QueryScene::build(std::string* err) {
    if (impl_->built) return true;
    World::bootstrap();

    impl_->physicsSystem.Init(64, 0, 256, 64,
                              impl_->bpLayerInterface,
                              impl_->objectVsBroadPhaseLayerFilter,
                              impl_->objectLayerPairFilter);
    JPH::BodyInterface& bi = impl_->physicsSystem.GetBodyInterface();

    for (int slot = 0; slot < 5; ++slot) {
        const FixtureBody f = fixture(slot);
        JPH::ShapeRefC shape = makeShape(f.shape);
        JPH::BodyCreationSettings settings(
            shape, toJolt(f.position), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, static_cast<JPH::ObjectLayer>(f.layer));
        JPH::Body* body = bi.CreateBody(settings);
        if (!body) {
            if (err) *err = "CreateBody failed for fixture slot " + std::to_string(slot);
            return false;
        }
        const JPH::BodyID id = body->GetID();
        bi.AddBody(id, JPH::EActivation::DontActivate);
        bi.SetUserData(id, f.owner);
        impl_->bodyIds.push_back(id.GetIndexAndSequenceNumber());
        impl_->owners.push_back(f.owner);
    }
    impl_->physicsSystem.OptimizeBroadPhase();
    impl_->built = true;
    return true;
}

bool QueryScene::ready() const { return impl_->built; }
std::uint32_t QueryScene::bodyCount() const {
    return static_cast<std::uint32_t>(impl_->bodyIds.size());
}
std::uint32_t QueryScene::bodyIdBySlot(int slot) const { return impl_->bodyIdBySlot(slot); }
std::uint64_t QueryScene::ownerBySlot(int slot) const { return impl_->ownerBySlot(slot); }

// ---------------------------------------------------------------------------
// raycast
// ---------------------------------------------------------------------------
QueryHit QueryScene::raycast(const RayQuery& q) const {
    QueryHit out;
    if (!impl_->built || !validateRayQuery(q, nullptr)) return out;

    const JPH::RRayCast ray(toJolt(q.origin), toJoltV3(q.direction) * q.maxT);
    QueryLayerFilter layerFilter(q.filter.layerMask);
    QueryBodyFilter  bodyFilter(q.filter.ignoreBodyIds);
    JPH::RayCastResult hit;

    const bool found = impl_->physicsSystem.GetNarrowPhaseQuery().CastRay(
        ray, hit, JPH::BroadPhaseLayerFilter(), layerFilter, bodyFilter);
    if (!found) return out;

    out.hit      = true;
    out.fraction = hit.mFraction;
    out.distance = hit.mFraction * q.maxT;
    const JPH::RVec3 hp = ray.GetPointOnRay(hit.mFraction);
    out.point = fromJolt(hp);

    JPH::BodyLockRead lock(impl_->physicsSystem.GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
            hit.mSubShapeID2, hp);
        out.normal = fromJolt(n);
    }
    impl_->decorate(&out, hit.mBodyID, hit.mSubShapeID2.GetValue(), out.normal);
    return out;
}

// ---------------------------------------------------------------------------
// sweep
// ---------------------------------------------------------------------------
QueryHit QueryScene::sweep(const SweepQuery& q) const {
    QueryHit out;
    if (!impl_->built || !validateSweepQuery(q, nullptr)) return out;

    JPH::ShapeRefC shape = makeShape(q.shape);
    const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(
        toJolt(q.rotation), toJolt(q.from));
    const JPH::Vec3 delta = toJoltV3(q.to) - toJoltV3(q.from);
    const float length = delta.Length();
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        shape.GetPtr(), JPH::Vec3::sReplicate(1.0f), start, delta);

    JPH::ShapeCastSettings settings;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    QueryLayerFilter layerFilter(q.filter.layerMask);
    QueryBodyFilter  bodyFilter(q.filter.ignoreBodyIds);
    impl_->physicsSystem.GetNarrowPhaseQuery().CastShape(
        cast, settings, JPH::RVec3::sZero(), collector,
        JPH::BroadPhaseLayerFilter(), layerFilter, bodyFilter);
    if (!collector.HadHit()) return out;

    const JPH::ShapeCastResult& r = collector.mHit;
    out.hit      = true;
    out.fraction = r.mFraction;
    out.distance = r.mFraction * length;
    out.point    = fromJolt(r.mContactPointOn2);
    out.normal   = fromJolt(-r.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero()));
    impl_->decorate(&out, r.mBodyID2, r.mSubShapeID2.GetValue(), out.normal);
    return out;
}

// ---------------------------------------------------------------------------
// overlap
// ---------------------------------------------------------------------------
std::vector<QueryHit> QueryScene::overlap(const OverlapQuery& q,
                                          std::size_t maxHits) const {
    std::vector<QueryHit> out;
    if (!impl_->built || maxHits == 0 || !validateOverlapQuery(q, nullptr)) return out;

    JPH::ShapeRefC shape = makeShape(q.shape);
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
        toJolt(q.rotation), toJolt(q.position));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryLayerFilter layerFilter(q.filter.layerMask);
    QueryBodyFilter  bodyFilter(q.filter.ignoreBodyIds);
    impl_->physicsSystem.GetNarrowPhaseQuery().CollideShape(
        shape.GetPtr(), JPH::Vec3::sReplicate(1.0f), transform, settings,
        JPH::RVec3::sZero(), collector,
        JPH::BroadPhaseLayerFilter(), layerFilter, bodyFilter);

    for (const JPH::CollideShapeResult& r : collector.mHits) {
        QueryHit h;
        h.hit      = true;
        h.fraction = 0.0f;
        h.distance = 0.0f;
        h.point    = fromJolt(r.mContactPointOn2);
        h.normal   = fromJolt(-r.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero()));
        impl_->decorate(&h, r.mBodyID2, r.mSubShapeID2.GetValue(), h.normal);
        out.push_back(h);
    }
    // Jolt's AllHitCollisionCollector::Sort() orders by -penetrationDepth, which
    // is not a stable total order for equal-depth contacts. BodyID ascending is
    // deterministic and is what the acceptance TSV asserts.
    std::stable_sort(out.begin(), out.end(),
                     [](const QueryHit& a, const QueryHit& b) {
                         return a.bodyId < b.bodyId;
                     });
    if (out.size() > maxHits) out.resize(maxHits);
    return out;
}

// ---------------------------------------------------------------------------
// validation
// ---------------------------------------------------------------------------
bool validateRayQuery(const RayQuery& q, std::string* err) {
    if (!(q.maxT > 0.0f) || !std::isfinite(q.maxT)) {
        if (err) *err = "maxT must be finite and > 0";
        return false;
    }
    const float len = lengthV3(q.direction);
    if (!std::isfinite(len) || std::fabs(len - 1.0f) > 1.0e-3f) {
        if (err) *err = "direction must be a finite unit vector";
        return false;
    }
    return true;
}

bool validateSweepQuery(const SweepQuery& q, std::string* err) {
    if (!validDims(q.shape, err)) return false;
    const nl::math::Vec3 d = { q.to.x - q.from.x, q.to.y - q.from.y,
                               q.to.z - q.from.z };
    const float len = lengthV3(d);
    if (!std::isfinite(len) || len <= 0.0f) {
        if (err) *err = "sweep from/to must differ";
        return false;
    }
    return true;
}

bool validateOverlapQuery(const OverlapQuery& q, std::string* err) {
    return validDims(q.shape, err);
}

} // namespace nl::physics
