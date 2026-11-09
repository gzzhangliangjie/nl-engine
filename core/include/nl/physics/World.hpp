#pragma once

// nl/physics — P8 M-P0: minimal Jolt-backed physics world (A-class parallel track).
//
// Scope (docs/p8/P8-PHYS-PLAN.md M-P0): vendored Jolt 5.3.1 + one static floor +
// one dynamic sphere, fixed-step headless simulation, trajectory dump.
// Integration layer only: no solver code, no graphics symbols, no kStage.
//
// Header is Jolt-free (PIMPL): consumers of nl_core never include Jolt headers.
// Determinism contract: JobSystemSingleThreaded + fixed 1/60 step, so identical
// DropConfig => bit-identical samples (see thirdparty/jolt/VENDOR.md).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nl/math/math.hpp"

namespace nl::physics {

constexpr const char* kJoltVersion = "5.3.1";

struct DropConfig {
    int           frames       = 240;         // number of fixed steps
    double        dt           = 1.0 / 60.0; // fixed step (Carrot 1/60 cadence)
    float         gravity      = 9.8f;
    float         startHeight  = 5.0f;       // sphere center height above floor top (y=0)
    float         radius       = 0.5f;
    float         restitution  = 0.6f;       // 0 = inelastic, 1 = fully elastic
    std::uint32_t seed         = 0;          // recorded in dump; M-P0 has no RNG source
};

struct Sample {
    int           frame = 0;
    double        t     = 0.0;
    nl::math::Vec3 position{};
    nl::math::Vec3 velocity{};
};

struct SimResult {
    std::vector<Sample> samples;
    std::string         error;   // empty == ok
    bool ok() const { return error.empty(); }
};

// Shared validation for phys-tools CLI and selftest. err receives an ASCII reason.
bool validateConfig(const DropConfig& cfg, std::string* err);

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Process-wide Jolt bootstrap (default allocator / trace / assert / factory /
    // registered types). Idempotent; must run before any other Jolt call.
    static void bootstrap();

    // Body count of the last simulated world (2 = floor + sphere).
    std::uint32_t bodyCount() const;

    // Build a fresh local world, step it, and return per-frame trajectory.
    // A fresh world per call is what makes repeated runs bit-identical.
    SimResult simulateDrop(const DropConfig& cfg);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::physics
