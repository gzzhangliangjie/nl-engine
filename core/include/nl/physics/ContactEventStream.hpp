#pragma once
// P8 M-D1: Jolt-free double-buffered contact event stream.
//
// Three-source evidence (2026-11-10 M-D1 open-day real reads; NOT reusing the
// M-D0 contact-listener evidence):
//  - bevy crates/bevy_ecs/src/event/mod.rs: update() swaps pages; :573
//    "reader_missed missed events unread after two update() calls"; :704-719
//    "events are only empty after the second call to update due to double
//    buffering"; :680-701 update_drain() hands out the oldest page.
//    => UNREAD events survive two update() calls; they are NOT cleared on the
//    next frame. Generic Events<T> is out of scope (P8-DRIVE-PLAN.md §4).
//  - Carrot engine/engine/physics/PhysicsBody.cpp:78-94: in-step enqueue ->
//    dispatchEventsPostPhysicsMainThread() at frame end on the main thread,
//    then clear(). Scripting consumer CSharpBindings.cpp:1033-1075 filters
//    with entity.exists() before touching the ECS. => Physics group produces /
//    PostPhysics group consumes; owner lookup is a filtered read.
//  - UE EngineBaseTypes.h:83-110: TG_PostPhysics = "needs rigid body and cloth
//    simulation to be complete before being executed"; the enum ordinal IS the
//    execution order. UE dispatches delegates immediately -- that timing is
//    explicitly NOT borrowed (P8-DRIVE-PLAN.md §2.3, Q1).
//
// Contract (P8-DRIVE-PLAN.md §3 M-D1 / §5 acceptance, M-GL5 group order):
//   update()   frame start, inside Physics group BEFORE publishing this frame
//   publish()  Physics group, after the Jolt step (real ContactEvent PODs)
//   drain()    PostPhysics group, from the scene::System consumer
//   * same-frame visibility: frame N events are readable in frame N's
//     PostPhysics drain (both pages are readable, previous page first).
//   * already-drained events are gone in frame N+1 (§5 "next frame clear"),
//     because drain() advances per-page cursors.
//   * UNREAD events survive into frame N+1 and are dropped by the THIRD
//     update() (bevy two-frame survival, §2.1). Both statements hold at once;
//     §5 is a shorthand about consumed events, not a contradiction.
//
// Jolt-free: only the ContactEvent POD. No JPH types, no Jolt includes.
// Single producer / single consumer on the logic thread; no locks -- the
// M-GL5 group ordinal is the synchronization contract.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nl/physics/Events.hpp"

namespace nl::physics {

class ContactEventStream {
public:
    // Frame-start page swap: the oldest page is dropped (its unread events are
    // counted for the two-frame-survival proof) and becomes the new write page;
    // the page written last frame becomes the readable older page.
    // Returns the number of unread events discarded by this swap.
    std::size_t update();

    // Physics group: append one event or a whole frame slice to the write page.
    void publish(const ContactEvent& e);
    void publish(const std::vector<ContactEvent>& events);

    // PostPhysics group: hand out all readable-but-not-yet-drained events in
    // chronological order (older page first, current page second), then advance
    // the read cursors so a second drain() in the same frame returns nothing.
    std::vector<ContactEvent> drain();

    // ---- read-only diagnostics (selftest / TSV provenance, no hidden state) ----
    std::size_t readableCount() const;    // unread events across both pages
    std::size_t pendingCount() const;     // events in the current write page
    std::size_t droppedTotal() const { return droppedTotal_; }
    std::uint64_t updateCount() const { return updateCount_; }
    std::uint64_t publishedTotal() const { return publishedTotal_; }
    std::uint64_t drainedTotal() const { return drainedTotal_; }

    void clear();

private:
    void appendUnread(int page, std::vector<ContactEvent>* out);
    std::size_t unreadIn(int page) const;

    std::vector<ContactEvent> pages_[2];
    std::size_t cursors_[2] = {0, 0};    // drained prefix of each page
    int write_ = 0;                      // index of the current write page
    std::size_t droppedTotal_ = 0;
    std::uint64_t updateCount_ = 0;
    std::uint64_t publishedTotal_ = 0;
    std::uint64_t drainedTotal_ = 0;
};

} // namespace nl::physics
