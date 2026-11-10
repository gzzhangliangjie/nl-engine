// cam-tools — P8 M-CAM0 headless camera acceptance tool (A-class track camera).
// Only links nl_core (ChaseCamera / FreeCamera / CameraRig, pure CPU); zero
// graphics API, no GPU, no kStage, no rendering regression.
//
// Exit code contract (aligned with input-tools / phys-tools, showcase expectExit):
//   0 = PASS            1 = assertion / runtime failure            2 = usage error
//
// Subcommands:
//   selftest [section]   mechanical anchors; section omitted = run all
//                        (init / chase-a / chase-b / chase-c / chase-d / free-e);
//                        unknown section = usage error (exit 2)
//   cam-trace            dump the 150-frame chase scenario (straight -> turn ->
//                        mode toggle -> look kick) as per-frame pose TSV
//   cam-check <tsv>      re-verify a dumped cam-trace with the same anchors
//   free-trace           dump the 60-frame free-flight script as TSV
//   free-check <tsv>     re-verify a dumped free-trace with the same anchors
//
// Evidence rule: no GPU screenshots — tool-card evidence images are generated
// from trace TSV by a real-output script (no hand-drawn diagrams).
//
// Determinism contract: camera update consumes only dt + input pose; fixed-dt
// replays are bit-identical (逐位锚), cross-dt comparisons are convergence
// anchors (阈值锚) — the two tiers are asserted explicitly in separate checks.

#include "nl/core/chase_camera.hpp"
#include "nl/core/free_camera.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int  g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float dist3(const float a[3], const float b[3]) {
    const float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

// Chase 场景参数（trace 与 selftest 共用同一脚本定义，单一定义源）。
struct ChaseScript {
    // 直线段：target 沿 +X 以 6 u/s 匀速，朝向 yaw=90°（车头 +X）；
    // 转弯段：frame >= kTurnFrame 起沿 +Z 以 6 u/s 匀速，朝向 yaw=180°（车头 -Z）。
    // look kick：f10 look(+30,0)（0.21 rad 偏航踢一脚，验证自动回正）。
    // mode toggle：f120 切 Orbit（无 look 输入段 offset 保持），f140 切回 Chase。
    static constexpr int   kFrames     = 150;
    static constexpr int   kTurnFrame  = 90;
    static constexpr float kSpeed      = 6.0f;
    static constexpr float kDt         = 1.0f / 60.0f;

    static void targetAt(int frame, float outPos[3], float& outYawDeg, float outVel[3]) {
        if (frame < kTurnFrame) {
            outPos[0] = kSpeed * kDt * frame;
            outPos[1] = 0.0f;
            outPos[2] = 0.0f;
            outYawDeg = 90.0f;
            outVel[0] = kSpeed; outVel[1] = 0.0f; outVel[2] = 0.0f;
        } else {
            outPos[0] = kSpeed * kDt * kTurnFrame;
            outPos[1] = 0.0f;
            outPos[2] = kSpeed * kDt * (frame - kTurnFrame);
            outYawDeg = 180.0f;
            outVel[0] = 0.0f; outVel[1] = 0.0f; outVel[2] = kSpeed;
        }
    }
    static void feedInputs(nl::core::ChaseCamera& cam, int frame) {
        if (frame == 10)  cam.look(30.0f, 0.0f);
        if (frame == 120) cam.setMode(nl::core::ChaseCamera::Mode::Orbit);
        if (frame == 140) cam.setMode(nl::core::ChaseCamera::Mode::Chase);
    }
};

// 期望 eye（与 chase_camera.cpp 的 update 公式镜像；测试独立重算，非读内部状态）。
void desiredEye(const nl::core::ChaseCamera& cam, const float targetPos[3],
                float targetYawDeg, bool flip, float out[3]) {
    const float boomYaw = targetYawDeg * kDeg2Rad + kPi
                        + cam.boomYawOffsetDegrees() * kDeg2Rad + (flip ? kPi : 0.0f);
    const float pitch = cam.pitchDegrees() * kDeg2Rad;
    const float cp = std::cos(pitch);
    out[0] = targetPos[0] + std::sin(boomYaw) * cp * cam.distance();
    out[1] = targetPos[1] + cam.heightOffset() + std::sin(pitch) * cam.distance();
    out[2] = targetPos[2] + std::cos(boomYaw) * cp * cam.distance();
}

// 期望 look-ahead 观察点（镜像公式）。
void desiredLook(const nl::core::ChaseCamera& cam, const float targetPos[3],
                 const float targetVel[3], float out[3]) {
    const float lt = cam.leadTime();
    float v[3] = { targetVel[0]*lt, targetVel[1]*lt, targetVel[2]*lt };
    const float m = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (m > nl::core::ChaseCamera::kMaxLead) {
        const float s = nl::core::ChaseCamera::kMaxLead / m;
        v[0]*=s; v[1]*=s; v[2]*=s;
    }
    out[0] = targetPos[0] + v[0];
    out[1] = targetPos[1] + v[1];
    out[2] = targetPos[2] + v[2];
}

// ---- s1：init / clamp 机械断言（照 cameraSelfTest 口径 + 基座扩展）----
void selftestInit() {
    std::printf("[init]\n");
    // 基座：透视缺省 + 正交分派 + 矩阵有限。
    nl::core::CameraRig rig;
    check(rig.projection == nl::core::CameraRig::Projection::Perspective,
          "rig: perspective default");
    const nl::core::Matrix4 persp = rig.viewProjection();
    bool finite = true;
    for (float v : persp.m) if (!std::isfinite(v)) finite = false;
    check(finite, "rig: perspective matrix finite");
    check(persp.m[11] == -1.0f && persp.m[15] == 0.0f, "rig: perspective ZO signature");

    rig.projection = nl::core::CameraRig::Projection::Orthographic;
    const nl::core::Matrix4 ortho = rig.viewProjection();
    finite = true;
    for (float v : ortho.m) if (!std::isfinite(v)) finite = false;
    check(finite, "rig: ortho matrix finite");
    check(ortho.m[11] == -1.0f && ortho.m[15] == 1.0f && ortho.m[10] < 0.0f,
          "rig: ortho ZO signature (m[15]=1, m[10]<0)");
    bool differs = false;
    for (int i = 0; i < 16; ++i) if (persp.m[i] != ortho.m[i]) differs = true;
    check(differs, "rig: ortho/perspective matrices differ");
    rig.orthoHeight = -5.0f;   // 非法 orthoHeight：相机 setter 拦，rig 字段由调用方负责
    check(rig.orthoHeight == -5.0f, "rig: orthoHeight raw field passthrough");

    // Chase：clamp 族。
    nl::core::ChaseCamera chase;
    chase.reset();
    chase.setAspect(640.0f / 360.0f);
    chase.setDistance(999.0f);
    check(chase.distance() == 50.0f, "chase: distance clamp hi (50)");
    chase.setDistance(0.0f);
    check(chase.distance() == 1.0f, "chase: distance clamp lo (1)");
    chase.setHeightOffset(-3.0f);
    check(chase.heightOffset() == 0.0f, "chase: height clamp lo (0)");
    chase.setStiffness(0.0f);
    check(chase.stiffness() == 0.1f, "chase: stiffness clamp lo (0.1)");
    chase.setLeadTime(9.0f);
    check(chase.leadTime() == 1.0f, "chase: leadTime clamp hi (1)");
    chase.setPitchDegrees(180.0f);
    check(chase.pitchDegrees() <= 83.08f && chase.pitchDegrees() >= 83.0f,
          "chase: pitch clamp (~83 deg)");
    chase.setFovYDegrees(999.0f);
    chase.look(100000.0f, 0.0f);   // boomYawOffset clamp ±180°
    check(chase.boomYawOffsetDegrees() <= 180.01f,
          "chase: boomYawOffset clamp (+180 deg)");
    for (int i = 0; i < 10; ++i) chase.look(-100000.0f, 0.0f);
    check(chase.boomYawOffsetDegrees() >= -180.01f,
          "chase: boomYawOffset clamp (-180 deg)");

    // Free：clamp 族 + boost 常量。
    nl::core::FreeCamera free_;
    free_.reset();
    free_.setSpeed(9999.0f);
    check(free_.speed() == 500.0f, "free: speed clamp hi (500)");
    free_.setSpeed(0.0f);
    check(free_.speed() == 0.1f, "free: speed clamp lo (0.1)");
    check(nl::core::FreeCamera::kBoost == 4.0f, "free: boost constant 4x");

    // 双相机缺省透视（正交是显式 opt-in）。
    check(chase.projection() == nl::core::CameraRig::Projection::Perspective,
          "chase: perspective default");
    check(free_.projection() == nl::core::CameraRig::Projection::Perspective,
          "free: perspective default");

    // 输出矩阵有限（透视与正交各一条）。
    chase.reset();
    finite = true;
    const nl::core::Matrix4 m1 = chase.viewProjection();
    for (float v : m1.m) if (!std::isfinite(v)) finite = false;
    check(finite, "chase: viewProjection finite");
    chase.setProjection(nl::core::CameraRig::Projection::Orthographic);
    finite = true;
    const nl::core::Matrix4 m2 = chase.viewProjection();
    for (float v : m2.m) if (!std::isfinite(v)) finite = false;
    check(finite, "chase: ortho viewProjection finite");
    free_.reset();
    finite = true;
    const nl::core::Matrix4 m3 = free_.viewProjection();
    for (float v : m3.m) if (!std::isfinite(v)) finite = false;
    check(finite, "free: viewProjection finite");
}

// ---- s2：A 直线跟随（稳态误差锚 + look kick 自动回正）----
void selftestChaseA() {
    std::printf("[chase-a]\n");
    nl::core::ChaseCamera cam;
    cam.reset();
    cam.setAspect(640.0f / 360.0f);
    float pose[3];   // eye
    float yawOffDeg0 = 0.0f;
    for (int f = 0; f < ChaseScript::kFrames; ++f) {
        float p[3], v[3], yawDeg;
        ChaseScript::targetAt(f, p, yawDeg, v);
        ChaseScript::feedInputs(cam, f);
        cam.update(ChaseScript::kDt, p, yawDeg, v);
        cam.eyePosition(pose);
        if (f == 9)  yawOffDeg0 = cam.boomYawOffsetDegrees();   // kick 前一帧无偏移
        if (f == 10) check(cam.boomYawOffsetDegrees() > 10.0f,
                           "A: f10 look kick registered (30px -> 12 deg)");
        (void)yawOffDeg0;
    }
    // 稳态残差：|eye - desired| ≈ v*dt/alpha（离散阻尼滞后，理论 ≈ 1.25 @ k=5,v=6,60Hz）。
    float p[3], v[3], yawDeg, want[3], look[3], eye[3];
    ChaseScript::targetAt(ChaseScript::kFrames - 1, p, yawDeg, v);
    desiredEye(cam, p, yawDeg, false, want);
    desiredLook(cam, p, v, look);
    cam.eyePosition(eye);
    const float residual = dist3(eye, want);
    check(residual > 0.9f && residual < 1.6f,
          "A: steady-state position lag in [0.9, 1.6] (damping model anchor)");
    check(std::fabs(eye[1] - want[1]) < 1e-3f,
          "A: steady-state height error < 1e-3 (no vertical motion)");
    check(std::fabs(cam.boomYawOffsetDegrees()) < 1.5f,
          "A: look kick auto-returned (< 1.5 deg residual after ~2.3 s)");
    // 观察点 look-ahead（直行段帧重算）：vel*lead = 6*0.3 = 1.8 沿速度方向，无 clamp。
    float p50[3], v50[3], yaw50, look50[3];
    ChaseScript::targetAt(50, p50, yaw50, v50);
    desiredLook(cam, p50, v50, look50);
    check(std::fabs(look50[0] - (p50[0] + 1.8f)) < 1e-4f && std::fabs(look50[2]) < 1e-4f,
          "A: look-ahead = pos + vel*leadTime (no clamp at 6 u/s)");
    // 期望 eye 与车尾方位：车朝 +X，boom 在 -X 侧。
    check(eye[0] < p[0] && eye[1] > p[1], "A: eye behind (+X-facing) and above target");
}

// ---- s3：B 急转衰减（残差单调衰减序列锚）----
void selftestChaseB() {
    std::printf("[chase-b]\n");
    nl::core::ChaseCamera cam;
    cam.reset();
    cam.setAspect(640.0f / 360.0f);
    std::vector<float> residual(ChaseScript::kFrames);
    float eye[3];
    for (int f = 0; f < ChaseScript::kFrames; ++f) {
        float p[3], v[3], yawDeg;
        ChaseScript::targetAt(f, p, yawDeg, v);
        ChaseScript::feedInputs(cam, f);
        cam.update(ChaseScript::kDt, p, yawDeg, v);
        float want[3];
        desiredEye(cam, p, yawDeg, false, want);
        cam.eyePosition(eye);
        residual[f] = dist3(eye, want);
    }
    // 转弯帧残差跳升（desired 突变），随后单调衰减。
    check(residual[ChaseScript::kTurnFrame] > residual[ChaseScript::kTurnFrame - 1] * 2.0f,
          "B: turn frame residual spikes (> 2x previous)");
    bool monotonic = true;
    for (int f = ChaseScript::kTurnFrame + 2; f < 140; ++f)
        if (residual[f] > residual[f - 1] + 1e-6f) monotonic = false;
    check(monotonic, "B: residual monotonically decays after turn (f92..f139)");
    check(residual[149] < residual[91] * 0.4f, "B: residual decayed to < 40% within 1 s");
}

// ---- s4：C 模式切换（位姿逐位连续 + orbit 不回正 + clamp）----
void selftestChaseC() {
    std::printf("[chase-c]\n");
    nl::core::ChaseCamera cam;
    cam.reset();
    cam.setAspect(640.0f / 360.0f);
    // 先跑 50 帧建立阻尼状态。
    for (int f = 0; f < 50; ++f) {
        float p[3], v[3], yawDeg;
        ChaseScript::targetAt(f, p, yawDeg, v);
        cam.update(ChaseScript::kDt, p, yawDeg, v);
    }
    float eyeBefore[3], eyeAfter[3];
    cam.eyePosition(eyeBefore);
    cam.setMode(nl::core::ChaseCamera::Mode::Orbit);   // 切换本身不动位姿
    cam.eyePosition(eyeAfter);
    check(std::memcmp(eyeBefore, eyeAfter, sizeof(eyeBefore)) == 0,
          "C: mode switch pose bit-identical (eye)");
    check(cam.mode() == nl::core::ChaseCamera::Mode::Orbit, "C: mode is Orbit");

    // orbit 模式：look 后无输入 60 帧 → offset 保持（不自动回正）。
    cam.look(20.0f, 0.0f);
    const float orbitKick = cam.boomYawOffsetDegrees();
    for (int f = 50; f < 110; ++f) {
        float p[3], v[3], yawDeg;
        ChaseScript::targetAt(f, p, yawDeg, v);
        cam.update(ChaseScript::kDt, p, yawDeg, v);
    }
    check(std::fabs(cam.boomYawOffsetDegrees() - orbitKick) < 1e-4f,
          "C: orbit mode keeps boomYawOffset (no auto-return)");

    // 切回 Chase：回正恢复（衰减可见）。
    cam.setMode(nl::core::ChaseCamera::Mode::Chase);
    for (int f = 110; f < 140; ++f) {
        float p[3], v[3], yawDeg;
        ChaseScript::targetAt(f, p, yawDeg, v);
        cam.update(ChaseScript::kDt, p, yawDeg, v);
    }
    check(std::fabs(cam.boomYawOffsetDegrees()) < std::fabs(orbitKick) * 0.9f,
          "C: chase mode resumes auto-return (offset decaying)");

    // 倒车/回头 XOR（各跑 120 帧到稳态再比对）：都开 = 看回前方。
    auto settle = [](bool reverse, bool peek, float outEye[3]) {
        nl::core::ChaseCamera c;
        c.reset();
        c.setReverse(reverse);
        c.setPeekBack(peek);
        float p[3] = {0, 0, 0}, v[3] = {0, 0, 0};
        const float yaw90 = 90.0f;
        for (int i = 0; i < 120; ++i) c.update(ChaseScript::kDt, p, yaw90, v);
        c.eyePosition(outEye);
    };
    float eyeNormal[3], eyeReverse[3], eyeBoth[3];
    settle(false, false, eyeNormal);
    settle(true,  false, eyeReverse);
    settle(true,  true,  eyeBoth);
    check(std::fabs(eyeReverse[0] - eyeNormal[0]) > 1.5f * cam.distance(),
          "C: reverse flips eye to opposite side (steady state)");
    check(dist3(eyeBoth, eyeNormal) < 1e-4f,
          "C: reverse+peek XOR -> back to normal side (steady state)");
}

// ---- s5：D 帧率无关（同 dt 逐位 / 跨 dt 收敛，分档显式）----
void selftestChaseD() {
    std::printf("[chase-d]\n");
    auto run = [](float dt, int frames) {
        nl::core::ChaseCamera cam;
        cam.reset();
        cam.setAspect(640.0f / 360.0f);
        std::vector<float> pose(frames * 3);
        for (int f = 0; f < frames; ++f) {
            float p[3], v[3], yawDeg;
            ChaseScript::targetAt(f, p, yawDeg, v);
            ChaseScript::feedInputs(cam, f);
            cam.update(dt, p, yawDeg, v);
            cam.eyePosition(&pose[f * 3]);
        }
        return pose;
    };
    const float dt60 = 1.0f / 60.0f;
    const float dt30 = 1.0f / 30.0f;
    const std::vector<float> a = run(dt60, ChaseScript::kFrames);
    const std::vector<float> b = run(dt60, ChaseScript::kFrames);
    check(a.size() == b.size() &&
          std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0,
          "D [逐位档]: same-dt replay bit-identical (150 frames x eye)");
    // 收敛档用纯直行脚本（理论稳态滞后差 ≈ v*k*dt^2/2 量级 ≈ 0.05，含转弯的
    // 瞬态残差是 dt 敏感段，不作跨 dt 断言）。
    auto runStraight = [](float dt, int frames) {
        nl::core::ChaseCamera cam;
        cam.reset();
        cam.setAspect(640.0f / 360.0f);
        std::vector<float> pose(frames * 3);
        for (int f = 0; f < frames; ++f) {
            float p[3] = { ChaseScript::kSpeed * dt * f, 0.0f, 0.0f };
            float v[3] = { ChaseScript::kSpeed, 0.0f, 0.0f };
            cam.update(dt, p, 90.0f, v);
            cam.eyePosition(&pose[f * 3]);
        }
        return pose;
    };
    const std::vector<float> s60 = runStraight(dt60, 150);   // 2.5 s
    const std::vector<float> s30 = runStraight(dt30, 75);    // 2.5 s
    const float endA[3] = {s60[147], s60[148], s60[149]};
    const float endC[3] = {s30[72], s30[73], s30[74]};
    check(dist3(endA, endC) < 0.2f,
          "D [收敛档]: dt=1/60 vs dt=1/30 final eye within 0.2 (2.5 s straight)");
}

// ---- s6：E 自由飞行（脚本逐位重放 + clamp + boost 倍率 + 朝向轴）----
void selftestFreeE() {
    std::printf("[free-e]\n");
    auto run = []() {
        nl::core::FreeCamera cam;
        cam.reset();
        cam.setAspect(640.0f / 360.0f);
        std::vector<float> pose(60 * 5);   // eye xyz + yaw/pitch deg
        for (int f = 0; f < 60; ++f) {
            float mv[3] = {0, 0, 0}, lk[2] = {0, 0};
            if (f >= 10) mv[2] = 1.0f;                                        // 前飞
            if (f >= 30 && f < 50) lk[0] = (90.0f / 20.0f) * kDeg2Rad;        // 20 帧 yaw 共 +90°
            if (f >= 30 && f < 50) mv[2] = 0.0f;
            if (f >= 30 && f < 40) mv[0] = 1.0f;                              // 右移
            if (f >= 50) lk[1] = -0.02f;                                      // 俯冲
            const bool boost = (f >= 10 && f < 30);
            cam.update(1.0f / 60.0f, mv, lk, boost);
            cam.eyePosition(&pose[f * 5]);
            pose[f * 5 + 3] = cam.yawDegrees();
            pose[f * 5 + 4] = cam.pitchDegrees();
        }
        return pose;
    };
    const std::vector<float> a = run();
    const std::vector<float> b = run();
    check(a.size() == b.size() &&
          std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0,
          "E: free-flight script replays bit-identical (60 frames)");

    // 朝向轴：yaw=0 时前飞沿 +Z（eye.z 增大）。
    check(a[11 * 5 + 2] > a[10 * 5 + 2], "E: forward flight moves along +Z at yaw=0");
    // boost 倍率直验：单帧同输入 boost=4x。
    {
        nl::core::FreeCamera cam;
        cam.reset();
        const float mv[3] = {0, 0, 1}, lk[2] = {0, 0};
        float e0[3], e1[3];
        cam.update(1.0f / 60.0f, mv, lk, false);
        cam.eyePosition(e0);
        cam.reset();
        cam.update(1.0f / 60.0f, mv, lk, true);
        cam.eyePosition(e1);
        const float d0 = std::fabs(e0[2]);
        const float d1 = std::fabs(e1[2]);
        check(std::fabs(d1 - 4.0f * d0) < 1e-5f, "E: boost displacement exactly 4x per frame");
    }
    // 俯仰 clamp：疯狂俯冲 → -83° 封顶。
    {
        nl::core::FreeCamera cam;
        cam.reset();
        const float mv[3] = {0, 0, 0}, lk[2] = {0, -0.5f};
        for (int f = 0; f < 400; ++f) cam.update(1.0f / 60.0f, mv, lk, false);
        check(cam.pitchDegrees() > -83.1f && cam.pitchDegrees() < -83.0f,
              "E: pitch clamped at -83 deg (no flip-through)");
    }
    // yaw 90° 后前飞沿 +X。
    {
        nl::core::FreeCamera cam;
        cam.reset();
        const float lkTurn[2] = {90.0f * kDeg2Rad, 0};
        const float lkNone[2] = {0, 0};
        const float mv0[3] = {0, 0, 0};
        cam.update(1.0f / 60.0f, mv0, lkTurn, false);   // 一帧转 90°
        const float mv[3] = {0, 0, 1};
        float e0[3], e1[3];
        cam.eyePosition(e0);
        cam.update(1.0f / 60.0f, mv, lkNone, false);    // 增量归零后再前飞
        cam.eyePosition(e1);
        check(e1[0] - e0[0] > 0.0f && std::fabs(e1[2] - e0[2]) < 1e-6f,
              "E: after yaw 90 deg forward = +X");
    }
    // 非法 dt 冻结（确定性防御）。
    {
        nl::core::FreeCamera cam;
        cam.reset();
        const float mv[3] = {0, 0, 1}, lk[2] = {0, 0};
        float e0[3], e1[3];
        cam.eyePosition(e0);
        cam.update(-1.0f, mv, lk, false);
        cam.update(0.0f, mv, lk, false);
        cam.eyePosition(e1);
        check(std::memcmp(e0, e1, sizeof(e0)) == 0, "E: dt<=0 freezes update");
    }
}

struct Section { const char* name; void (*fn)(); };

// ---- cam-trace：150 帧 chase 场景 → TSV（evidence 管线数据源，真实工具输出）----
int runCamTrace(int argc, char** argv) {
    (void)argc; (void)argv;
    nl::core::ChaseCamera cam;
    cam.reset();
    cam.setAspect(640.0f / 360.0f);
    std::printf("frame\teye_x\teye_y\teye_z\tboom_off_deg\tpitch_deg\tmode\n");
    for (int f = 0; f < ChaseScript::kFrames; ++f) {
        float p[3], v[3], yawDeg;
        ChaseScript::targetAt(f, p, yawDeg, v);
        ChaseScript::feedInputs(cam, f);
        cam.update(ChaseScript::kDt, p, yawDeg, v);
        float eye[3];
        cam.eyePosition(eye);
        std::printf("%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%d\n",
                    f, eye[0] + 0.0f, eye[1] + 0.0f, eye[2] + 0.0f,
                    cam.boomYawOffsetDegrees() + 0.0f, cam.pitchDegrees() + 0.0f,
                    cam.mode() == nl::core::ChaseCamera::Mode::Orbit ? 1 : 0);
    }
    return 0;
}

// ---- free-trace：60 帧自由飞行 → TSV ----
int runFreeTrace(int argc, char** argv) {
    (void)argc; (void)argv;
    nl::core::FreeCamera cam;
    cam.reset();
    cam.setAspect(640.0f / 360.0f);
    std::printf("frame\teye_x\teye_y\teye_z\tyaw_deg\tpitch_deg\n");
    for (int f = 0; f < 60; ++f) {
        float mv[3] = {0, 0, 0}, lk[2] = {0, 0};
        if (f >= 10) mv[2] = 1.0f;
        if (f >= 30 && f < 50) lk[0] = (90.0f / 20.0f) * kDeg2Rad;   // 4.5 deg/帧 x20 = 90°
        if (f >= 30 && f < 50) mv[2] = 0.0f;
        if (f >= 30 && f < 40) mv[0] = 1.0f;
        if (f >= 50) lk[1] = -0.02f;
        const bool boost = (f >= 10 && f < 30);
        cam.update(1.0f / 60.0f, mv, lk, boost);
        float eye[3];
        cam.eyePosition(eye);
        std::printf("%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n",
                    f, eye[0] + 0.0f, eye[1] + 0.0f, eye[2] + 0.0f,
                    cam.yawDegrees() + 0.0f, cam.pitchDegrees() + 0.0f);
    }
    return 0;
}


// ---- check 共用：TSV 行解析 + 逐帧取行 ----
struct CamRow { int f; double ex, ey, ez, off, pitch; int mode; };
struct FreeRow { int f; double ex, ey, ez, yaw, pitch; };

bool near2(double v, double expect, double tol) {
    return v > expect - tol && v < expect + tol;
}

int runCamCheck(int argc, char** argv) {
    if (argc < 1) { std::printf("ERROR: cam-check needs a <tsv> path\n"); return 2; }
    std::FILE* fp = std::fopen(argv[0], "r");
    if (!fp) { std::printf("ERROR: cannot open '%s'\n", argv[0]); return 1; }
    char line[512];
    std::vector<CamRow> rows;
    while (std::fgets(line, sizeof(line), fp)) {
        if (std::strstr(line, "frame\t") == line) continue;   // header
        CamRow r{};
        if (std::sscanf(line, "%d\t%lf\t%lf\t%lf\t%lf\t%lf\t%d",
                        &r.f, &r.ex, &r.ey, &r.ez, &r.off, &r.pitch, &r.mode) == 7)
            rows.push_back(r);
    }
    std::fclose(fp);
    auto at = [&](int f) -> const CamRow* {
        for (const CamRow& r : rows) if (r.f == f) return &r;
        return nullptr;
    };
    check(static_cast<int>(rows.size()) == ChaseScript::kFrames, "cam-check: 150 rows");
    // f0 初始阻尼位姿（trace 实测锚：reset (0,2,6) -> 阻尼一步）。
    if (const CamRow* r = at(0)) {
        check(near2(r->ex, -0.450649, 1e-4) && near2(r->ey, 2.164500, 1e-4) &&
              near2(r->ez, 5.520267, 1e-4) && near2(r->pitch, 20.053522, 1e-4),
              "cam-check: f0 damped pose from reset (trace-derived)");
        check(r->off == 0.0, "cam-check: f0 boom offset 0");
    } else { check(false, "cam-check: f0 present"); }
    // look kick + 回正。
    if (const CamRow* r10 = at(10)) check(near2(r10->off, 12.032114, 1e-3),
                                          "cam-check: f10 look kick (12.03 deg)");
    if (const CamRow* r11 = at(11)) check(r11->off < at(10)->off,
                                          "cam-check: f11 decay resumed");
    // 直行段 offset 单调衰减（f11..f89）。
    bool mono = true;
    for (int f = 12; f <= 89; ++f)
        if (at(f)->off > at(f - 1)->off + 1e-6) mono = false;
    check(mono, "cam-check: offset monotonically decays f12..f89");
    // 转弯帧残差跳升（eye_z 直行 0.058 -> 转弯 0.504）。
    check(at(90)->ez - at(89)->ez > 0.4, "cam-check: f90 turn residual spike (eye_z jump)");
    check(near2(at(89)->ey, 4.0562, 0.01),
          "cam-check: steady-state height 2 + 6*sin(20.05 deg) (trace-derived)");
    // orbit 段 offset 冻结（f120 切入，f139 仍原值），切回后恢复衰减。
    check(at(120)->mode == 1 && at(120)->off == at(119)->off,
          "cam-check: f120 orbit switch freezes offset");
    check(near2(at(139)->off, at(120)->off, 1e-9),
          "cam-check: f139 orbit keeps offset (no auto-return)");
    check(at(140)->off < at(139)->off && at(149)->off < at(140)->off,
          "cam-check: f140 chase resumes decay");
    if (g_failures != 0) { std::printf("cam-check FAIL\n"); return 1; }
    std::printf("cam-check PASS (150 rows, 10 anchors)\n");
    return 0;
}

int runFreeCheck(int argc, char** argv) {
    if (argc < 1) { std::printf("ERROR: free-check needs a <tsv> path\n"); return 2; }
    std::FILE* fp = std::fopen(argv[0], "r");
    if (!fp) { std::printf("ERROR: cannot open '%s'\n", argv[0]); return 1; }
    char line[512];
    std::vector<FreeRow> rows;
    while (std::fgets(line, sizeof(line), fp)) {
        if (std::strstr(line, "frame\t") == line) continue;   // header
        FreeRow r{};
        if (std::sscanf(line, "%d\t%lf\t%lf\t%lf\t%lf\t%lf",
                        &r.f, &r.ex, &r.ey, &r.ez, &r.yaw, &r.pitch) == 6)
            rows.push_back(r);
    }
    std::fclose(fp);
    auto at = [&](int f) -> const FreeRow* {
        for (const FreeRow& r : rows) if (r.f == f) return &r;
        return nullptr;
    };
    check(static_cast<int>(rows.size()) == 60, "free-check: 60 rows");
    if (const FreeRow* r = at(0)) {
        check(r->ex == 0.0 && r->ey == 0.0 && r->ez == 0.0 && r->yaw == 0.0 && r->pitch == 0.0,
              "free-check: f0 identity pose");
    } else { check(false, "free-check: f0 present"); }
    // boost 直行段：20 帧 x 10*4/60 = 13.3333。
    check(near2(at(29)->ez, 13.333335, 1e-3),
          "free-check: f29 boost straight total (20f x 0.6667)");
    // boost 倍率：boost 段首帧位移 = 4 x 常速步长（speed*dt = 10/60；f31 起混入
    // yaw/横移轴，不作对照帧）。
    check(near2(at(10)->ez - at(9)->ez, 4.0 * (10.0 / 60.0), 1e-3),
          "free-check: per-frame boost displacement 4x");
    // yaw 20 帧累计 +90°；pitch 10 帧 -0.02 rad/帧 = -11.459°。
    check(near2(at(49)->yaw, 90.0, 1e-3), "free-check: f49 yaw accumulated 90 deg");
    check(near2(at(59)->pitch, -11.459154, 1e-3),
          "free-check: f59 pitch dive (-0.02 rad x 10f)");
    bool yawMono = true;
    for (int f = 31; f <= 49; ++f) if (at(f)->yaw <= at(f - 1)->yaw) yawMono = false;
    check(yawMono, "free-check: yaw strictly increasing f31..f49");
    if (g_failures != 0) { std::printf("free-check FAIL\n"); return 1; }
    std::printf("free-check PASS (60 rows, 6 anchors)\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "cam-tools — P8 M-CAM0 camera acceptance tool (chase/free headless)\n"
            "usage:\n"
            "  cam-tools selftest [section]   run mechanical anchors (default: all)\n"
            "  cam-tools cam-trace            dump the 150-frame chase scenario as TSV\n"
            "  cam-tools cam-check <tsv>      re-verify a dumped cam-trace\n"
            "  cam-tools free-trace           dump the 60-frame free-flight script as TSV\n"
            "  cam-tools free-check <tsv>     re-verify a dumped free-trace\n"
            "sections: init chase-a chase-b chase-c chase-d free-e\n"
            "exit codes: 0=PASS 1=failure 2=usage error\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "selftest") {
        static const Section sections[] = {
            {"init",    selftestInit},
            {"chase-a", selftestChaseA},
            {"chase-b", selftestChaseB},
            {"chase-c", selftestChaseC},
            {"chase-d", selftestChaseD},
            {"free-e",  selftestFreeE},
        };
        const char* only = (argc >= 3) ? argv[2] : nullptr;
        bool ranAny = false;
        for (const Section& s : sections) {
            if (only && std::strcmp(only, s.name) != 0) continue;
            ranAny = true;
            s.fn();
        }
        if (!ranAny) {
            std::printf("ERROR: unknown selftest section '%s'\n", only ? only : "");
            return 2;
        }
        if (g_failures != 0) {
            std::printf("selftest FAIL (%d failure(s))\n", g_failures);
            return 1;
        }
        std::printf("selftest PASS (6 sections)\n");
        return 0;
    }
    if (cmd == "cam-trace")  return runCamTrace(argc - 2, argv + 2);
    if (cmd == "free-trace") return runFreeTrace(argc - 2, argv + 2);
    if (cmd == "cam-check")  return runCamCheck(argc - 2, argv + 2);
    if (cmd == "free-check") return runFreeCheck(argc - 2, argv + 2);
    std::printf("ERROR: unknown command '%s'\n", cmd.c_str());
    return 2;
}
