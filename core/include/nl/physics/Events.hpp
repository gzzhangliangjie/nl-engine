#pragma once

// nl/physics — P8 M-D0: Jolt-free contact event POD + headless probe config.
//
// Source evidence (2026-11-10 M-D0 open-day real reads, docs/p8/P8-DRIVE-PLAN.md §2):
//  - vendored Jolt ContactListener.h:106/119/136 = three callbacks; :69-71 callbacks
//    run while all bodies are locked (read only, NoLock interfaces); :123-128
//    OnContactRemoved MUST NOT touch bodies (they may already be destroyed) --
//    cache what you need in Added/Persisted; :100-101 sleeping bodies receive
//    Removed; :114-118/:133-134 SubShapeID may be reused after shape changes.
//  - Carrot PhysicsSystem.cpp:124/345 = one global listener + per-body fan-out,
//    PhysicsBody.cpp:68/87-94 = in-step queue -> post-physics main-thread dispatch.
//    We keep the "Physics group produces / frame-end dispatch" rhythm, NOT the
//    per-body callback granularity (Carrot is corroboration only, see Q1).
//  - bevy bevy_ecs/src/event/mod.rs:573/716 = double-buffered Events<T>, two-frame
//    survival. M-D0 only PRODUCES the POD stream; the buffered consumer is M-D1.
//  - UE PrimitiveComponent.h:274 = OnComponentHit(FHitResult) useful subset.
//    UE dispatches delegates immediately; we do NOT borrow that timing.
//
// Header stays Jolt-free: no JPH types, no Jolt includes. The listener lives in
// core/src/physics/World.cpp (the only Jolt symbol surface inside nl_core).
//
// Q2 (adopted default at M-D0 kickoff, no separate user sign-off; P8-DRIVE-PLAN.md §7): owner pairs are cached in
// Added/Persisted; the cache entry is erased AFTER its Removed event is emitted.

#include <cstdint>
#include <string>
#include <vector>

#include "nl/math/math.hpp"

namespace nl::physics {

// Raw JPH::BodyID value meaning "no body". Single source of truth shared by
// BodyInfo (World.hpp) and ContactEvent.
constexpr std::uint32_t kInvalidBodyId = 0xffffffffu;

// Jolt callback ordinal; frozen as the TSV wire format.
enum class ContactKind : std::uint8_t {
    Added     = 0,
    Persisted = 1,
    Removed   = 2,
};

// Plain-old-data event: safe to copy out of the physics step, no Jolt lifetime.
// Jolt guarantees body1 ID < body2 ID in all three callbacks
// (ContactListener.h:98/112/131), so (ownerA, bodyIdA) is the low-id side.
// worldPos / normalY come from ContactManifold at Added/Persisted time; for a
// Removed event they are replayed from the cache, never re-read from Jolt.
struct ContactEvent {
    int            frame   = 0;
    ContactKind    kind    = ContactKind::Added;
    std::uint64_t  ownerA  = 0;
    std::uint64_t  ownerB  = 0;
    std::uint32_t  bodyIdA = kInvalidBodyId;
    std::uint32_t  bodyIdB = kInvalidBodyId;
    nl::math::Vec3 worldPos{};
    float          normalY = 0.0f;
};

// One trajectory row dumped alongside the event stream (M-P0 TSV subset).
struct ContactProbeFrame {
    int    frame = 0;
    double t     = 0.0;
    float  y     = 0.0f;
    float  vy    = 0.0f;
};

// Three-body course (M-P1 simulateLayerProbe precedent): static floor, static
// spherical blocker, dynamic probe dropped onto them.
struct ContactProbeConfig {
    int           frames       = 240;
    double        dt           = 1.0 / 60.0;
    float         gravity      = 9.8f;
    float         radius       = 0.5f;
    float         startHeight  = 6.0f;
    float         blockerY     = 2.5f;
    float         restitution  = 0.6f;
    std::uint64_t probeOwner   = 0x4e4c443000000001ull; // "NLD0" + slot 1
    std::uint64_t blockerOwner = 0x4e4c443000000002ull;
    std::uint64_t floorOwner   = 0x4e4c443000000003ull;
};

struct ContactProbeResult {
    std::vector<ContactEvent>      events;             // frame-ordered, frame-end stable sorted
    std::vector<ContactProbeFrame> trajectory;         // frames + 1 rows
    std::uint32_t bodyCount           = 0;
    std::uint32_t removedCacheMisses  = 0;             // must stay 0 (Q2 cache self-check)
    std::string   error;
    bool ok() const { return error.empty(); }
};

bool validateContactProbe(const ContactProbeConfig& cfg, std::string* err);

} // namespace nl::physics
