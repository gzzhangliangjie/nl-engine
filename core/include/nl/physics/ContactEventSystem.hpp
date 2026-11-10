#pragma once
// P8 M-D1: demo scene::System consumer for ContactEventStream.
//
// Evidence (M-D1 open-day real reads):
//  - Carrot CSharpBindings.cpp:1033-1075: the scripting contact consumer
//    registers with mainThreadOnly=true and filters BOTH sides through
//    entity.exists() before invoking user code. We keep that filter, but as a
//    whole-stream consumer instead of per-body callbacks (Q1: Carrot is
//    corroboration of the timing, not the granularity).
//  - nl/scene/System.hpp contract 2: a System that declares NO reads/writes is
//    "self-contained" and may run in parallel with every other system. This
//    consumer only calls World::exists() (a const name-table lookup), so it
//    derives from System directly (not SignedSystem<>) and stays self-contained.
//  - UE has no ECS event bus; P8-DRIVE-PLAN.md §2.3 explicitly rejects UE's
//    immediate delegate dispatch for this track.
//
// owner -> EntityID: ContactEvent::ownerA/B are the engine-side u64 ids from
// BodyInfo::owner (M-P1), so they are used directly as scene::EntityID values;
// no extra mapping table is introduced (P8-DRIVE-PLAN.md §1). Owners without a
// live entity (pure physics bodies) are counted as unmapped and passed through
// in lastEvents() -- no event is silently dropped.
//
// Jolt-free, zero graphics symbols. This class belongs to nl/physics because it
// is the physics -> ECS bridge; it depends on nl/scene/System.hpp, not vice versa.

#include <cstdint>
#include <memory>
#include <vector>

#include "nl/physics/ContactEventStream.hpp"
#include "nl/physics/Events.hpp"
#include "nl/scene/System.hpp"

namespace nl::physics {

struct ContactConsumeStats {
    std::uint64_t ticks          = 0;
    std::uint64_t events         = 0;
    std::uint64_t mappedA        = 0;
    std::uint64_t mappedB        = 0;
    std::uint64_t unmappedA      = 0;
    std::uint64_t unmappedB      = 0;
    std::uint64_t lastFrameEvents = 0;
};

class ContactEventSystem : public nl::scene::System {
public:
    static constexpr const char* kName = "ContactEventSystem";

    ContactEventSystem(nl::scene::World& world, ContactEventStream& stream)
        : nl::scene::System(world), stream_(stream) {}

    const char* getName() const override { return kName; }
    void tick(double dt) override;
    // Editor copy: a fresh statistics counter bound to the same stream. The demo
    // consumer owns no entity state, so sharing the stream is well-defined.
    std::unique_ptr<nl::scene::System> duplicate(nl::scene::World& newOwner) const override {
        return std::make_unique<ContactEventSystem>(newOwner, stream_);
    }

    const ContactConsumeStats& stats() const { return stats_; }
    // Events consumed by the most recent tick (diagnostics / TSV wire). Events
    // with unmapped owners are present here -- filtering is a count, not a drop.
    const std::vector<ContactEvent>& lastEvents() const { return lastEvents_; }

private:
    ContactEventStream& stream_;
    ContactConsumeStats stats_;
    std::vector<ContactEvent> lastEvents_;
};

} // namespace nl::physics
