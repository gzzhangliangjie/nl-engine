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

// ---- M-P1: engine<->body association + configurable collision layer matrix ----
// Header stays Jolt-free: BodyInfo::id is the raw JPH::BodyID value, not a Jolt type.
constexpr std::uint32_t kLayerCount    = 4;
constexpr std::uint32_t kInvalidBodyId = 0xffffffffu;

namespace layers {
constexpr std::uint32_t kFloor   = 0;  // static geometry
constexpr std::uint32_t kDefault = 1;  // ordinary dynamic bodies / static blocker
constexpr std::uint32_t kDebris  = 2;  // probe layer, pair with kDefault can be disabled
constexpr std::uint32_t kGhost   = 3;  // collides with nothing
}

// Symmetric bool matrix: row a bit b == "a and b may collide".
// UE counterpart is FCollisionResponseContainer's Ignore/Overlap/Block tri-state;
// the A-class track keeps a bool mask (docs/s5/UE-DIFFS.md D12).
struct LayerMatrix {
    std::uint32_t rows[kLayerCount]{};

    LayerMatrix();                                        // every pair collides
    bool allows(std::uint32_t a, std::uint32_t b) const;
    void set(std::uint32_t a, std::uint32_t b, bool allow); // symmetric by construction
    std::string hex() const;                              // ASCII provenance, e.g. "0f0f0f0f"
};

struct BodyInfo {
    std::uint32_t id       = kInvalidBodyId; // JPH::BodyID raw value
    std::uint64_t owner    = 0;              // engine-side entity id
    std::uint32_t layer    = 0;
    bool          isStatic = false;
    nl::math::Vec3 position{};
};

// Probe harness: a static blocker sphere sits between the falling probe and the
// floor. If the layer pair is enabled the probe rests on the blocker
// (y ~= blockerY + 2r); if disabled it passes through and rests on the floor
// (y ~= r). That makes the collision matrix directly observable in a TSV.
struct LayerProbeConfig {
    std::uint32_t probeLayer    = layers::kDebris;
    std::uint32_t blockerLayer  = layers::kDefault;
    std::uint32_t floorLayer    = layers::kFloor;
    int           frames        = 180;
    double        dt            = 1.0 / 60.0;
    float         gravity       = 9.8f;
    float         radius        = 0.5f;
    float         startHeight   = 6.0f;
    float         blockerY      = 2.5f;
    float         restitution   = 0.6f;
    std::uint64_t probeOwner    = 0x4e4c503100000001ull; // "NLP1" + slot
    std::uint64_t blockerOwner  = 0x4e4c503100000002ull;
    std::uint64_t floorOwner    = 0x4e4c503100000003ull;
};

struct LayerProbeResult {
    std::vector<Sample>   samples;               // probe per-frame trajectory
    std::vector<BodyInfo> bodies;                // all 3 bodies; vector index == Jolt userData
    float        finalY       = 0.0f;
    bool         blocked       = false;          // came to rest on the blocker
    std::uint32_t probeId     = kInvalidBodyId;
    std::uint64_t ownerFromId = 0;              // BodyID -> userData -> owner
    std::uint32_t idFromOwner = kInvalidBodyId; // owner -> BodyID
    std::string  error;
    bool ok() const { return error.empty(); }
};

bool validateLayerProbe(const LayerProbeConfig& cfg, const LayerMatrix& matrix,
                        std::string* err);

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

    // M-P1: build a 3-body world (floor / static blocker / dynamic probe) with
    // a caller-supplied layer matrix, step it, and return the trajectory plus
    // the two-way engine<->Jolt association self-check.
    LayerProbeResult simulateLayerProbe(const LayerMatrix& matrix,
                                        const LayerProbeConfig& cfg);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::physics
