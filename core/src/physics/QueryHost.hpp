#pragma once

// M-PQ1 internal bridge between DebugScene::Impl (World.cpp) and
// QueryScene::Impl (Query.cpp). NOT a public header -- lives in core/src/physics
// where every Jolt symbol is allowed (PIMPL discipline: query.hpp / World.hpp
// stay Jolt-free; QueryHost erases the concrete system type behind void*).
//
// Ownership: DebugScene::Impl implements QueryHost and OUTLIVES the pointer it
// hands out (vtable is stable; the JPH::PhysicsSystem member is owned by Impl
// itself). QueryScene in attach mode stores the host pointer but never owns
// the scene -- callers must guarantee the attached QueryScene dies before the
// DebugScene (S15 UAF discipline, same as audioSetWorld/querySetScene wiring).

namespace nl::physics {

struct QueryHost {
    virtual ~QueryHost() = default;
    // Returns the live JPH::PhysicsSystem* (erased as void*; Query.cpp casts it
    // back inside the only Jolt-aware TU). nullptr before build().
    virtual void* system() = 0;
};

} // namespace nl::physics
