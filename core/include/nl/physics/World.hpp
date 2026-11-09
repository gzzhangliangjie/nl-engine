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
    // M-J5c: 0 = JobSystemSingleThreaded (M-P0 byte baseline, default);
    // >=1 = PhysicsSystem::Update runs on a dedicated ThreadPool via the
    // JobSystem adapter (PoolJobSystem). Determinism anchors per mode are
    // check-jobs' job (N=1 must be bit-identical to the baseline path).
    int           jobsThreads  = 0;
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
// the A-class track keeps a bool mask (docs/s5/UE-DIFFS.md D13).
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

// ---- M-P2: stacking stability ----------------------------------------------
// A tower of `boxes` unit cubes on a static floor, stepped for a long run.
// Three cases (baseline / no_sleep / no_friction) make the stability sources
// observable: contact solver + friction hold the tower, sleeping only removes
// work. UE counterpart is Chaos IslandManager (velocity threshold + counter
// sleeps a whole island) -- see docs/s5/UE-DIFFS.md D13.
struct StackConfig {
    int           frames        = 600;        // long run: 10 s at 1/60
    double        dt            = 1.0 / 60.0;
    float         gravity       = 9.8f;
    std::uint32_t boxes         = 6;
    float         halfExtent    = 0.5f;       // cube half edge => 1.0 m cubes
    float         gap           = 0.005f;     // initial clearance between cubes
    float         friction      = 0.6f;
    float         restitution   = 0.0f;
    float         lateralSpeed  = 0.0f;       // +x velocity given to the top cube
    bool          allowSleeping = true;
    float         damping       = 0.0f;       // 0 = no artificial damping
    int           jobsThreads   = 0;          // M-J5c: 0 = SingleThreaded (baseline), >=1 = pool adapter
};

struct BodySample {
    nl::math::Vec3 position{};
    nl::math::Vec3 velocity{};
    bool           active = true;
};

struct StackFrame {
    int    frame = 0;
    double t     = 0.0;
    std::vector<BodySample> bodies;
    std::uint32_t activeBodies = 0;
};

struct StackResult {
    std::vector<StackFrame> frames;
    std::string   error;
    std::uint32_t finalActive   = 0;
    float  maxTailSpread = 0.0f;  // max |y(t)-y(end)| over the last 60 frames, all cubes
    float  maxTailSpeed  = 0.0f;  // max |velocity| over the last 60 frames, all cubes
    float  maxDrift      = 0.0f;  // max horizontal displacement of any cube at t=end
    float  topDrift      = 0.0f;  // horizontal displacement of the top cube at t=end
    float  topY          = 0.0f;  // top cube center height at t=end
    bool   collapsed     = false; // any cube ended more than half a cube below its slot
    bool ok() const { return error.empty(); }
};

bool validateStack(const StackConfig& cfg, std::string* err);

// ---- M-P4：角色胶囊（CharacterMovement 式：扫掠 + 地面检测 + 斜坡/台阶） ----
// Jolt CharacterVirtual（虚体走查，与 UE CharacterMovementComponent 同型：
// MoveAlongFloor/FindFloor/MaxSlopeAngle <-> Update/StickToFloor/WalkStairs/
// GetGroundState）。重力与输入由应用侧每帧合成（Jolt 约定，见
// CharacterVirtual.h Update 注释）。Carrot 的 Character（JPH::Character 真实体
// 式）是另一路，M-P4 计划口径为 CharacterMovement 式，故取 CharacterVirtual。
// 头文件零 Jolt 类型；确定性契约与 M-P0 相同（fresh world + 固定步 + 单线程
// JobSystem + CharacterVirtual mInnerBodyIDOverride 固定 BodyID）。
struct CharacterConfig {
    int           frames       = 1200;       // 20 s at 1/60：走完 24 m 到 60° 坡脚再静止
    double        dt           = 1.0 / 60.0;
    float         gravity      = 9.8f;
    float         radius       = 0.5f;
    float         halfCylinder = 0.6f;       // capsule 圆柱半高（总高 2.2 m）
    float         walkSpeed    = 1.5f;       // 沿 +x 匀速（动作系统输入接口的演示驱动）
    float         maxSlopeDeg  = 50.0f;      // Jolt 默认 50°：30° 可爬、60° 被挡
    std::uint32_t seed         = 0;
};

// groundState 数值 = JPH::CharacterBase::EGroundState 枚举序（固化，TSV 直接可读）：
// 0=OnGround 1=OnSteepGround 2=NotSupported 3=InAir
struct CharacterSample {
    int            frame = 0;
    double         t     = 0.0;
    nl::math::Vec3 position{};             // 脚底（CharacterVirtual mPosition = 胶囊底部中心）
    nl::math::Vec3 velocity{};
    int            groundState = -1;
    float          groundNormalY = 0.0f;
};

struct CharacterResult {
    std::vector<CharacterSample> samples;
    float         finalX = 0.0f;           // 末帧脚底 x（锚：被 60° 斜坡挡住）
    float         finalY = 0.0f;
    std::string   error;
    bool ok() const { return error.empty(); }
};

bool validateCharacter(const CharacterConfig& cfg, std::string* err);

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

    // M-P2: floor + N-cube tower, stepped for a long run. Returns per-cube
    // per-frame position/velocity/active flag plus the stability metrics.
    StackResult simulateStack(const StackConfig& cfg);

    // M-P4: character walk-through over ramp/stairs/steep-slope course
    // (floor -> 30deg ramp -> platform -> 3 steps -> platform -> 60deg slope).
    // One fresh world per call (determinism contract); CharacterVirtual
    // ExtendedUpdate cadence, gravity/input composed app-side per Jolt docs.
    CharacterResult simulateCharacter(const CharacterConfig& cfg);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- M-P3：物理调试场景（碰撞形状线框叠加） ----
// 头文件零 Jolt 类型：线框以 POD 视图（指针 + 顶点数）暴露，Jolt 头只在 World.cpp 内。
// 用法：build() 建世界并预步进到稳态；每帧 step() 推进、extractLines() 取线流；
// extractLines 返回的 data 指向 DebugScene 内部缓冲，仅在下次 extractLines 前有效
// （同一帧内喂给 FrameParams 并 renderFrame 后即失效，符合 viewer 单帧消费模型）。
struct DebugLines {
    const float* data = nullptr;          // px,py,pz,r,g,b 交替，长度 = vertexCount*6
    std::uint32_t vertexCount = 0;        // LINE_LIST：每 2 顶点为一段线
};

class DebugScene {
public:
    DebugScene();
    ~DebugScene();
    DebugScene(const DebugScene&) = delete;
    DebugScene& operator=(const DebugScene&) = delete;

    // scenario: 0 = 静态地板 + 落体盒/球/胶囊（落地稳态）。
    //           1 = M-P4 角色走查场地（地板 -> 30° 斜坡 -> 平台 -> 3 级台阶 ->
    //               平台 -> 60° 斜坡；CharacterVirtual 沿 +x 匀速走查，
    //               线框含亮黄角色胶囊——CharacterVirtual 无真实体，
    //               DebugRenderer 不会自动画它，由本类手动补画）。
    // gravity 默认 9.8；seed 当前未驱动 RNG（确定性来自固定步 + 单线程 JobSystem）。
    bool build(int scenario, std::uint32_t seed, std::string* err);
    void step(double dt);                 // 推进一个固定步（dt 建议 1/60）
    DebugLines extractLines();            // 提取所有刚体碰撞形状的线框（Jolt DebugRenderer 子类）
    const char* lastError() const;
    std::uint32_t bodyCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::physics
