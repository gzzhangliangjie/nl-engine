// M-A1：anim-tools —— 动作系统（nl/anim）自测工具，A 类并行轨道。
// 只依赖 core（nl/anim），零图形 API 符号；接线仿 material-graph-tools 先例。
//
// 退出码约定（与 nanite-tools / 展示台 expectExit 0/1/2 对齐）：
//   0 = 用例全过（打印 "<case> selftest: PASS"）
//   1 = 断言失败（打印 "<case> selftest: FAIL <detail>"）
//   2 = 用法错误（未知子命令 / 未知 selftest 用例）
//
// 用例（docs/action-system/ACTION-PLAN.md §3 判据）：
//   curve        M-A1 关键帧采样：lerp/slerp 锚点、domain clamp、Step、
//                常量轨道、wrapTime 循环、整片采样、validate 负向、确定性
//   blend        M-A2 线性/加法混合
//   statemachine M-A3 状态机 + crossfade（确定性状态序列）
//   blendspace   M-A4 1D blend space

#include "nl/anim/anim.hpp"
#include "nl/anim/blend_space.hpp"
#include "nl/anim/state_machine.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expectNear(bool ok, const char* what, float got, float want, float eps) {
    if (ok && std::fabs(got - want) <= eps) return;
    std::printf("  FAIL %s: got %.8f want %.8f (eps %.1e)\n", what, got, want, eps);
    ++g_failures;
}

void expectTrue(bool ok, const char* what) {
    if (ok) return;
    std::printf("  FAIL %s\n", what);
    ++g_failures;
}

void expectStr(const std::string& got, const char* want, const char* what) {
    if (got == want) return;
    std::printf("  FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want);
    ++g_failures;
}

// ---------------------------------------------------------------------------
// M-A1 curve
// ---------------------------------------------------------------------------

void testVec3LerpAndClamp() {
    nl::anim::Vec3Track track;
    track.times = { 0.0f, 1.0f, 3.0f };
    track.values = { { 0, 0, 0 }, { 2, 4, 6 }, { 2, 4, 6 } };

    nl::anim::Vec3 mid = nl::anim::sampleVec3Track(track, 0.5f);
    expectNear(true, "lerp mid x", mid.x, 1.0f, 1e-6f);
    expectNear(true, "lerp mid y", mid.y, 2.0f, 1e-6f);
    expectNear(true, "lerp mid z", mid.z, 3.0f, 1e-6f);

    // 二段 [1,3] 区间中点 t=2 -> (2,4,6)（第二段是常量段）
    nl::anim::Vec3 seg2 = nl::anim::sampleVec3Track(track, 2.0f);
    expectNear(true, "seg2 x", seg2.x, 2.0f, 1e-6f);

    // domain clamp（bevy sample_clamped 语义）
    nl::anim::Vec3 lo = nl::anim::sampleVec3Track(track, -1.0f);
    nl::anim::Vec3 hi = nl::anim::sampleVec3Track(track, 9.0f);
    expectNear(true, "clamp lo", lo.x, 0.0f, 1e-6f);
    expectNear(true, "clamp hi", hi.x, 2.0f, 1e-6f);

    // 端点精确
    nl::anim::Vec3 at0 = nl::anim::sampleVec3Track(track, 0.0f);
    expectNear(true, "endpoint t0", at0.y, 0.0f, 0.0f);
}

void testQuatSlerp() {
    // 绕 Y 轴 0° -> 90°：中点 = 45°
    const float s45 = 0.70710678118f, c45 = 0.70710678118f;
    const float s225 = 0.38268343236f, c225 = 0.92387953251f;
    nl::anim::QuatTrack track;
    track.times = { 0.0f, 1.0f };
    track.values = { nl::anim::quatIdentity(), { 0, s45, 0, c45 } };

    nl::anim::Quat mid = nl::anim::sampleQuatTrack(track, 0.5f);
    expectNear(true, "slerp mid y", mid.y, s225, 1e-6f);
    expectNear(true, "slerp mid w", mid.w, c225, 1e-6f);
    expectNear(true, "slerp mid norm", std::sqrt(nl::anim::dotQuat(mid, mid)), 1.0f, 1e-6f);

    // 端点精确
    nl::anim::Quat at0 = nl::anim::sampleQuatTrack(track, 0.0f);
    nl::anim::Quat at1 = nl::anim::sampleQuatTrack(track, 1.0f);
    expectNear(true, "slerp t0 w", at0.w, 1.0f, 0.0f);
    expectNear(true, "slerp t1 y", at1.y, s45, 1e-7f);

    // 最短路径：b 取反（同一旋转的另一半）结果不变
    nl::anim::QuatTrack flipped = track;
    flipped.values[1] = { -0, -s45, -0, -c45 };
    nl::anim::Quat midFlipped = nl::anim::sampleQuatTrack(flipped, 0.5f);
    expectNear(true, "slerp shortest path y", midFlipped.y, s225, 1e-6f);

    // 近平行退化（nlerp 分支）不爆炸且归一
    nl::anim::QuatTrack tiny = track;
    tiny.values = { nl::anim::quatIdentity(), { 0, 1e-6f, 0, 1.0f } };
    nl::anim::Quat nearParallel = nl::anim::sampleQuatTrack(tiny, 0.5f);
    expectNear(true, "slerp near-parallel norm",
               std::sqrt(nl::anim::dotQuat(nearParallel, nearParallel)), 1.0f, 1e-5f);
}

void testStepAndConstant() {
    nl::anim::QuatTrack step;
    step.interp = nl::anim::Interp::Step;
    step.times = { 0.0f, 1.0f, 2.0f };
    step.values = { { 0, 0, 0, 1 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    // STEP：保持前 key；t=1.5 -> key@1
    nl::anim::Quat mid = nl::anim::sampleQuatTrack(step, 1.5f);
    expectNear(true, "step holds previous y", mid.y, 1.0f, 0.0f);
    expectNear(true, "step holds previous w", mid.w, 0.0f, 0.0f);

    // 常量轨道（单 key）：任意 t 返回该 key
    nl::anim::Vec3Track constant;
    constant.times = { 0.0f };
    constant.values = { { 5, 6, 7 } };
    nl::anim::Vec3 c = nl::anim::sampleVec3Track(constant, 123.0f);
    expectNear(true, "constant track x", c.x, 5.0f, 0.0f);

    // 空轨道 = 常量语义（零位移 / identity）
    nl::anim::Vec3Track emptyV;
    nl::anim::Vec3 ev = nl::anim::sampleVec3Track(emptyV, 1.0f);
    nl::anim::QuatTrack emptyQ;
    nl::anim::Quat eq = nl::anim::sampleQuatTrack(emptyQ, 1.0f);
    expectNear(true, "empty track zero", ev.x + ev.y + ev.z, 0.0f, 0.0f);
    expectNear(true, "empty track identity w", eq.w, 1.0f, 0.0f);
}

void testWrapTime() {
    expectNear(true, "wrap 5/2", nl::anim::wrapTime(5.0f, 2.0f), 1.0f, 1e-6f);
    expectNear(true, "wrap -0.5/2", nl::anim::wrapTime(-0.5f, 2.0f), 1.5f, 1e-6f);
    expectNear(true, "wrap 4/2", nl::anim::wrapTime(4.0f, 2.0f), 0.0f, 1e-6f);
    expectNear(true, "wrap exact duration", nl::anim::wrapTime(2.0f, 2.0f), 0.0f, 1e-6f);
}

void testClipSampleAndValidate() {
    nl::anim::AnimClip clip;
    clip.duration = 2.0f;
    nl::anim::BoneTrack hips;
    hips.bone = "hips";
    hips.translation.times = { 0.0f, 2.0f };
    hips.translation.values = { { 0, 1, 0 }, { 0, 1, 4 } };
    hips.rotation.times = { 0.0f, 1.0f, 2.0f };
    hips.rotation.values = { nl::anim::quatIdentity(),
                             { 0, 0.70710678118f, 0, 0.70710678118f },
                             nl::anim::quatIdentity() };
    nl::anim::BoneTrack head;
    head.bone = "head";   // 常量轨道（gltf 常见：无动画通道的骨骼）
    head.rotation.times = { 0.0f };
    head.rotation.values = { nl::anim::quatIdentity() };
    clip.tracks = { hips, head };

    expectStr(nl::anim::validateClip(clip), "", "valid clip passes");

    std::vector<nl::anim::BonePose> poses;
    expectTrue(nl::anim::sampleClip(clip, 0.5f, poses), "sampleClip ok");
    expectTrue(poses.size() == 2, "pose count == track count");
    expectNear(true, "hips t=0.5 y", poses[0].translation.y, 1.0f, 1e-6f);
    expectNear(true, "hips rot 0.5 y", poses[0].rotation.y, 0.38268343236f, 1e-6f);
    // 循环归一：t=2.5 -> wrap 0.5
    expectTrue(nl::anim::sampleClip(clip, 2.5f, poses), "sampleClip looped ok");
    expectNear(true, "looped rot y", poses[0].rotation.y, 0.38268343236f, 1e-6f);
    // 负 t 回绕
    expectTrue(nl::anim::sampleClip(clip, -1.5f, poses), "sampleClip negative ok");
    expectNear(true, "negative wrap rot y", poses[0].rotation.y, 0.38268343236f, 1e-6f);
    // 按骨骼名索引
    expectTrue(nl::anim::findTrackIndex(clip, "head") == 1, "findTrackIndex hit");
    expectTrue(nl::anim::findTrackIndex(clip, "nope") == static_cast<std::size_t>(-1),
               "findTrackIndex miss");

    // 负向：validate 逐条拒绝
    nl::anim::AnimClip bad = clip;
    bad.duration = 0.0f;
    expectTrue(!nl::anim::validateClip(bad).empty(), "reject zero duration");
    bad = clip;
    bad.tracks[0].bone = "head";
    expectTrue(!nl::anim::validateClip(bad).empty(), "reject duplicate bone");
    bad = clip;
    bad.tracks[0].translation.times = { 0.0f, 1.0f, 2.0f };   // 与 values 不等长
    expectTrue(!nl::anim::validateClip(bad).empty(), "reject size mismatch");
    bad = clip;
    bad.tracks[0].rotation.times = { 0.0f, 1.0f, 1.0f };      // 非严格递增
    expectTrue(!nl::anim::validateClip(bad).empty(), "reject non-increasing times");
    bad = clip;
    bad.tracks[0].translation.values[0] = { 0, std::nanf(""), 0 };
    expectTrue(!nl::anim::validateClip(bad).empty(), "reject NaN");
    bad.tracks[0].translation.values[0] = { 0, 1, 0 };
    bad.tracks[0].rotation.values[1] = { 0, 0, 0, 0 };        // 零长度四元数
    expectTrue(!nl::anim::validateClip(bad).empty(), "reject zero quat");
    // 未过 validate 的 clip 拒绝采样
    expectTrue(!nl::anim::sampleClip(bad, 0.0f, poses), "sampleClip rejects invalid");
}

void testDeterminism() {
    nl::anim::AnimClip clip;
    clip.duration = 1.0f;
    nl::anim::BoneTrack t;
    t.bone = "b";
    t.translation.times = { 0.0f, 0.25f, 0.5f, 1.0f };
    t.translation.values = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 2, 0 }, { 3, 2, 1 } };
    t.rotation.times = { 0.0f, 1.0f };
    t.rotation.values = { nl::anim::quatIdentity(), { 0, 0.70710678118f, 0, 0.70710678118f } };
    clip.tracks = { t };
    const float times[] = { 0.0f, 0.1f, 0.37f, 0.5f, 0.999f };
    std::vector<unsigned char> first, second;
    for (float time : times) {
        std::vector<nl::anim::BonePose> poses;
        nl::anim::sampleClip(clip, time, poses);
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(poses.data());
        first.insert(first.end(), bytes, bytes + poses.size() * sizeof(nl::anim::BonePose));
    }
    for (float time : times) {   // 重跑必须逐位一致（确定性硬判据）
        std::vector<nl::anim::BonePose> poses;
        nl::anim::sampleClip(clip, time, poses);
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(poses.data());
        second.insert(second.end(), bytes, bytes + poses.size() * sizeof(nl::anim::BonePose));
    }
    expectTrue(first == second && !first.empty(), "two runs bit-identical");
}

int runCurveSelftest() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "vec3 lerp/clamp", testVec3LerpAndClamp },
        { "quat slerp", testQuatSlerp },
        { "step/constant", testStepAndConstant },
        { "wrapTime", testWrapTime },
        { "clip sample+validate", testClipSampleAndValidate },
        { "determinism", testDeterminism },
    };
    for (const Case& c : cases) {
        std::printf("  [%s]\n", c.name);
        std::fflush(stdout);
        c.fn();
    }
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// M-A2 blend
// ---------------------------------------------------------------------------

// 两个双骨骼 clip：idle（原位）与 run（前移 4m + 绕 Y 90°）。
void makeBlendClips(nl::anim::AnimClip& idle, nl::anim::AnimClip& run) {
    idle = {};
    idle.duration = 2.0f;
    nl::anim::BoneTrack it;
    it.bone = "hips";
    it.translation.times = { 0.0f };
    it.translation.values = { { 0, 1, 0 } };
    it.rotation.times = { 0.0f };
    it.rotation.values = { nl::anim::quatIdentity() };
    idle.tracks = { it };

    run = {};
    run.duration = 1.0f;
    nl::anim::BoneTrack rt;
    rt.bone = "hips";
    rt.translation.times = { 0.0f, 1.0f };
    rt.translation.values = { { 0, 1, 0 }, { 0, 1, 4 } };
    rt.rotation.times = { 0.0f, 1.0f };
    rt.rotation.values = { nl::anim::quatIdentity(),
                           { 0, 0.70710678118f, 0, 0.70710678118f } };
    run.tracks = { rt };
}

void testLerpPoseAnchors() {
    nl::anim::BonePose a, b;
    a.translation = { 0, 1, 0 };
    b.translation = { 2, 1, 4 };
    b.rotation = { 0, 0.70710678118f, 0, 0.70710678118f };

    nl::anim::BonePose at0 = nl::anim::lerpPose(a, b, 0.0f);
    nl::anim::BonePose at1 = nl::anim::lerpPose(a, b, 1.0f);
    expectNear(true, "lerp w=0 -> a", at0.translation.x, 0.0f, 0.0f);
    expectNear(true, "lerp w=0 rot identity", at0.rotation.w, 1.0f, 0.0f);
    expectNear(true, "lerp w=1 -> b tx", at1.translation.x, 2.0f, 0.0f);
    expectNear(true, "lerp w=1 rot y", at1.rotation.y, 0.70710678118f, 1e-7f);

    nl::anim::BonePose mid = nl::anim::lerpPose(a, b, 0.5f);
    expectNear(true, "lerp mid tx", mid.translation.x, 1.0f, 1e-6f);
    expectNear(true, "lerp mid tz", mid.translation.z, 2.0f, 1e-6f);
    expectNear(true, "lerp mid rot y", mid.rotation.y, 0.38268343236f, 1e-6f);

    // 域外 clamp
    nl::anim::BonePose clamped = nl::anim::lerpPose(a, b, 1.5f);
    expectNear(true, "lerp w>1 clamp", clamped.translation.x, 2.0f, 0.0f);
}

void testBlendPosesAndClips() {
    nl::anim::AnimClip idle, run;
    makeBlendClips(idle, run);

    std::vector<nl::anim::BonePose> out;
    // w=0.5、t 双 0：平移中点 x=0? idle tx=0、run t0 tx=0 -> 0；rot 都是 identity
    expectTrue(nl::anim::blendClips(idle, 0.0f, run, 0.0f, 0.5f, out), "blendClips ok");
    expectTrue(out.size() == 1, "blendClips pose count");
    expectNear(true, "blend t0 y", out[0].translation.y, 1.0f, 1e-6f);

    // t=0.5、w=1：完全取 run -> x 位移一半 2m，rot 45°
    expectTrue(nl::anim::blendClips(idle, 0.0f, run, 0.5f, 1.0f, out), "blendClips w=1 ok");
    expectNear(true, "blend w=1 x", out[0].translation.x, 0.0f, 1e-6f);
    expectNear(true, "blend w=1 z", out[0].translation.z, 2.0f, 1e-6f);
    expectNear(true, "blend w=1 rot y", out[0].rotation.y, 0.38268343236f, 1e-6f);

    // w=0.5、t=0.5 run 侧：x 位移 2m*0.5=1m
    expectTrue(nl::anim::blendClips(idle, 0.0f, run, 0.5f, 0.5f, out), "blendClips mid ok");
    expectNear(true, "blend mid z", out[0].translation.z, 1.0f, 1e-6f);
    expectNear(true, "blend mid rot y", out[0].rotation.y, 0.19509032207f, 1e-6f);  // 旋转 22.5° -> sin(11.25°)

    // 骨架尺寸不一致拒绝
    nl::anim::AnimClip runBig = run;
    runBig.tracks.push_back(run.tracks[0]);
    runBig.tracks[1].bone = "head";
    std::vector<nl::anim::BonePose> keep;
    expectTrue(!nl::anim::blendClips(idle, 0.0f, runBig, 0.0f, 0.5f, keep),
               "blendClips rejects rig mismatch");
}

void testAdditivePose() {
    // delta = 绕 Y 90° + 前移 2m；base = identity + 原点
    nl::anim::BonePose base;
    base.translation = { 1, 0, 0 };
    nl::anim::BonePose delta;
    delta.translation = { 0, 0, 2 };
    delta.rotation = { 0, 0.70710678118f, 0, 0.70710678118f };

    // w=0：原样 base
    nl::anim::BonePose at0 = nl::anim::addPose(base, delta, 0.0f);
    expectNear(true, "additive w=0 tx", at0.translation.x, 1.0f, 0.0f);
    expectNear(true, "additive w=0 rot w", at0.rotation.w, 1.0f, 1e-7f);

    // w=1：全量叠加 —— 平移 (1,0,2)，旋转 90°
    nl::anim::BonePose at1 = nl::anim::addPose(base, delta, 1.0f);
    expectNear(true, "additive w=1 tx", at1.translation.x, 1.0f, 1e-6f);
    expectNear(true, "additive w=1 tz", at1.translation.z, 2.0f, 1e-6f);
    expectNear(true, "additive w=1 rot y", at1.rotation.y, 0.70710678118f, 1e-6f);

    // w=0.5：平移一半 (1,0,1)，旋转 45°
    nl::anim::BonePose half = nl::anim::addPose(base, delta, 0.5f);
    expectNear(true, "additive half tz", half.translation.z, 1.0f, 1e-6f);
    expectNear(true, "additive half rot y", half.rotation.y, 0.38268343236f, 1e-6f);

    // 相对叠加：base 先转 90°，delta 再 90° -> 合成 180°（绕 Y）
    nl::anim::BonePose turned = base;
    turned.rotation = { 0, 0.70710678118f, 0, 0.70710678118f };
    nl::anim::BonePose composed = nl::anim::addPose(turned, delta, 1.0f);
    expectNear(true, "additive compose y", composed.rotation.y, 1.0f, 1e-6f);   // sin90=1
    expectNear(true, "additive compose w", composed.rotation.w, 0.0f, 1e-6f);   // cos90=0
}

int runBlendSelftest() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "lerp pose anchors", testLerpPoseAnchors },
        { "blendPoses/blendClips", testBlendPosesAndClips },
        { "additive pose", testAdditivePose },
    };
    for (const Case& c : cases) {
        std::printf("  [%s]\n", c.name);
        std::fflush(stdout);
        c.fn();
    }
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// M-A3 statemachine
// ---------------------------------------------------------------------------

nl::anim::AnimClip makeLocoClip(const char* bone, float zEnd) {
    nl::anim::AnimClip clip;
    clip.duration = 1.0f;
    nl::anim::BoneTrack track;
    track.bone = bone;
    track.translation.times = { 0.0f, 1.0f };
    track.translation.values = { { 0, 1, 0 }, { 0, 1, zEnd } };
    clip.tracks = { track };
    return clip;
}

nl::anim::StateMachineDesc makeLocoDesc() {
    nl::anim::StateMachineDesc desc;
    desc.states = {
        { "idle", makeLocoClip("hips", 0.0f) },
        { "walk", makeLocoClip("hips", 1.0f) },
        { "run",  makeLocoClip("hips", 4.0f) },
    };
    desc.initialState = 0;   // idle
    // 声明顺序 = 优先级（UE FindValidTransition 取首个满足者）
    desc.rules = {
        { 0, 1, [](const nl::anim::ParamValues& p) { return p[0] > 0.2f; }, 0.2f },   // idle->walk
        { 1, 2, [](const nl::anim::ParamValues& p) { return p[0] > 1.0f; }, 0.3f },   // walk->run
        { 2, 1, [](const nl::anim::ParamValues& p) { return p[0] < 0.8f; }, 0.3f },   // run->walk
        { 1, 0, [](const nl::anim::ParamValues& p) { return p[0] < 0.05f; }, 0.2f },  // walk->idle
    };
    return desc;
}

// 跑一段确定性场景，把 (状态名, alpha, 髋部 z) 序列写进 out（供断言与确定性比对）。
// 时序：每步先推进时间与 alpha，再按优先级查当前态转移规则（转移当帧 alpha 从 0 起）。
void runLocoScenario(nl::anim::AnimationStateMachine& sm, std::string* names,
                     float* alphas, float* durations, float* zs) {
    struct Step { float dt; float speed; };
    const Step steps[] = {
        { 0.1f, 0.0f },   // 0 idle 安稳（z=0）
        { 0.1f, 0.5f },   // 1 idle->walk 起转移（当帧 alpha=0，z=0）
        { 0.1f, 0.5f },   // 2 alpha=0.5（z=lerp(0, walk@0.1 z0.1, 0.5)=0.05）
        { 0.1f, 0.5f },   // 3 alpha=1 完成（z=walk@0.2=0.2）
        { 0.1f, 2.0f },   // 4 walk->run 起转移（z=lerp(walk@0.3, run@0, 0)=0.3）
        { 0.1f, 2.0f },   // 5 alpha=1/3（z=lerp(walk@0.4 z0.4, run@0.1 z0.4, 1/3)=0.4）
        { 0.1f, 0.0f },   // 6 alpha=2/3 后 run->walk 回退：alpha 取反 1/3、缩时 0.2（z 连续 0.7）
        { 0.1f, 0.0f },   // 7 alpha=5/6 后 walk->idle 起转移（旧 run 贡献丢弃=单活跃转移声明）
        { 0.1f, 0.0f },   // 8 alpha=0.5（z=lerp(walk@0.7 z0.7, idle 0, 0.5)=0.35）
        { 0.1f, 0.0f },   // 9 alpha=1 完成（z=idle 0）
    };
    for (int i = 0; i < 10; ++i) {
        sm.update(steps[i].dt, { steps[i].speed });
        std::vector<nl::anim::BonePose> pose;
        if (!sm.evaluate(pose)) {
            ++g_failures;
            std::printf("  FAIL scenario step %d: evaluate false\n", i);
            return;
        }
        names[i] = sm.currentStateName();
        alphas[i] = sm.transitionAlpha();
        durations[i] = sm.transitionDuration();
        zs[i] = pose[0].translation.z;
    }
}

void testStateMachineScenario() {
    nl::anim::AnimationStateMachine sm;
    expectTrue(sm.reset(makeLocoDesc()), "reset ok");

    std::string names[10];
    float alphas[10], durations[10], zs[10];
    runLocoScenario(sm, names, alphas, durations, zs);

    expectTrue(names[0] == "idle", "step0 idle");
    expectNear(true, "step0 z", zs[0], 0.0f, 1e-6f);

    expectTrue(names[1] == "walk", "step1 target walk");
    expectNear(true, "step1 alpha", alphas[1], 0.0f, 0.0f);
    expectNear(true, "step1 duration", durations[1], 0.2f, 1e-6f);
    expectNear(true, "step1 z", zs[1], 0.0f, 1e-6f);

    expectNear(true, "step2 alpha", alphas[2], 0.5f, 1e-6f);
    expectNear(true, "step2 z", zs[2], 0.05f, 1e-6f);

    expectNear(true, "step3 alpha", alphas[3], 1.0f, 1e-6f);
    expectNear(true, "step3 z", zs[3], 0.2f, 1e-6f);

    expectTrue(names[4] == "run", "step4 target run");
    expectNear(true, "step4 z", zs[4], 0.3f, 1e-6f);

    expectNear(true, "step5 alpha", alphas[5], 1.0f / 3.0f, 1e-6f);
    expectNear(true, "step5 z", zs[5], 0.4f, 1e-6f);

    // 回退转移：alpha=2/3 时切回 walk（目标态既有权重 1/3）-> alpha 取反 1/3、
    // UE 缩时 effective = 0.3 * (1 - 1/3) = 0.2；z 连续（回退前后同值 0.7）
    expectTrue(names[6] == "walk", "step6 revert to walk");
    expectNear(true, "step6 alpha", alphas[6], 1.0f / 3.0f, 1e-6f);
    expectNear(true, "step6 duration", durations[6], 0.2f, 1e-6f);
    expectNear(true, "step6 z", zs[6], 0.7f, 1e-5f);

    expectTrue(names[7] == "idle", "step7 walk->idle");
    expectNear(true, "step7 z", zs[7], 0.6f, 1e-6f);   // alpha=0 -> 纯 walk@0.6

    expectNear(true, "step8 alpha", alphas[8], 0.5f, 1e-6f);
    expectNear(true, "step8 z", zs[8], 0.35f, 1e-6f);

    expectTrue(names[9] == "idle", "step9 idle");
    expectNear(true, "step9 alpha", alphas[9], 1.0f, 1e-6f);
    expectNear(true, "step9 z", zs[9], 0.0f, 1e-6f);
}

void testStateMachineRevertBlend() {
    nl::anim::AnimationStateMachine sm;
    expectTrue(sm.reset(makeLocoDesc()), "reset ok");

    // idle->walk 半途（alpha=0.5）速度归零 -> walk->idle 命中回退路径
    sm.update(0.1f, { 0.0f });              // idle 安稳
    sm.update(0.1f, { 0.5f });              // idle->walk 起转移（alpha=0）
    sm.update(0.1f, { 0.0f });              // alpha=0.5 后 walk->idle 命中回退
    std::vector<nl::anim::BonePose> pose;
    expectTrue(sm.currentStateName() == "idle", "revert target idle");
    expectNear(true, "revert alpha", sm.transitionAlpha(), 0.5f, 1e-6f);
    // UE 防跳变缩时：effective = 0.2 * (1 - 0.5) = 0.1
    expectNear(true, "revert effective duration", sm.transitionDuration(), 0.1f, 1e-6f);
    // 姿态连续：回退当帧 z 与回退前相同（lerp 对称性）
    expectTrue(sm.evaluate(pose), "eval after revert");
    expectNear(true, "continuity z", pose[0].translation.z, 0.05f, 1e-6f);

    // 0.1s 后混合完成（缩时时长生效）
    sm.update(0.1f, { 0.0f });
    expectNear(true, "revert done alpha", sm.transitionAlpha(), 1.0f, 1e-6f);
    expectTrue(sm.evaluate(pose), "eval done");
    expectNear(true, "revert done z", pose[0].translation.z, 0.0f, 1e-6f);
}

void testStateMachineValidateNegative() {
    nl::anim::StateMachineDesc desc = makeLocoDesc();
    expectStr(nl::anim::validateStateMachine(desc), "", "valid desc passes");

    nl::anim::StateMachineDesc bad = desc;
    bad.rules[0].to = 9;
    expectTrue(!nl::anim::validateStateMachine(bad).empty(), "reject rule out of range");
    bad = desc;
    bad.states[1].name = "idle";
    expectTrue(!nl::anim::validateStateMachine(bad).empty(), "reject duplicate state");
    bad = desc;
    bad.rules[1].from = 1; bad.rules[1].to = 1;
    expectTrue(!nl::anim::validateStateMachine(bad).empty(), "reject self-loop");
    bad = desc;
    bad.rules[2].canTake = nullptr;
    expectTrue(!nl::anim::validateStateMachine(bad).empty(), "reject missing condition");
    bad = desc;
    bad.initialState = 7;
    expectTrue(!nl::anim::validateStateMachine(bad).empty(), "reject initialState range");

    nl::anim::AnimationStateMachine sm;
    expectTrue(!sm.reset(bad), "reset rejects invalid desc");
    std::vector<nl::anim::BonePose> pose;
    expectTrue(!sm.evaluate(pose), "evaluate rejects un-reset machine");
}

void testStateMachineDeterminism() {
    std::string namesA[10], namesB[10];
    float alphaA[10], alphaB[10], durA[10], durB[10], zA[10], zB[10];
    nl::anim::AnimationStateMachine smA, smB;
    smA.reset(makeLocoDesc());
    smB.reset(makeLocoDesc());
    runLocoScenario(smA, namesA, alphaA, durA, zA);
    runLocoScenario(smB, namesB, alphaB, durB, zB);
    bool same = true;
    for (int i = 0; i < 10; ++i)
        same = same && namesA[i] == namesB[i] && alphaA[i] == alphaB[i] &&
               durA[i] == durB[i] && zA[i] == zB[i];
    expectTrue(same, "two runs identical sequence");
}

int runStateMachineSelftest() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "loco scenario", testStateMachineScenario },
        { "revert blend + continuity", testStateMachineRevertBlend },
        { "validate negatives", testStateMachineValidateNegative },
        { "determinism", testStateMachineDeterminism },
    };
    for (const Case& c : cases) {
        std::printf("  [%s]\n", c.name);
        std::fflush(stdout);
        c.fn();
    }
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// M-A4 blendspace
// ---------------------------------------------------------------------------

// 轴：速度 0/1/2（m/s），clip z 终点 0 / 1 / 4（t=0.5 时 z = 0 / 0.5 / 2）。
nl::anim::BlendSpace1DDesc makeLocoBlendSpace() {
    nl::anim::BlendSpace1DDesc desc;
    desc.samples = {
        { 0.0f, makeLocoClip("hips", 0.0f) },
        { 1.0f, makeLocoClip("hips", 1.0f) },
        { 2.0f, makeLocoClip("hips", 4.0f) },
    };
    return desc;
}

void testBlendSpaceWeights() {
    nl::anim::BlendSpace1D bs;
    expectTrue(bs.reset(makeLocoBlendSpace()), "reset ok");

    std::size_t i0, i1;
    float w = -1.0f;
    bs.segmentWeights(0.5f, i0, i1, w);
    expectTrue(i0 == 0 && i1 == 1, "seg(0.5) indices");
    expectNear(true, "seg(0.5) w", w, 0.5f, 1e-6f);
    bs.segmentWeights(1.5f, i0, i1, w);
    expectTrue(i0 == 1 && i1 == 2, "seg(1.5) indices");
    expectNear(true, "seg(1.5) w", w, 0.5f, 1e-6f);

    // 域外 clamp：i0==i1、w=0
    bs.segmentWeights(-1.0f, i0, i1, w);
    expectTrue(i0 == 0 && i1 == 0 && w == 0.0f, "seg clamp low");
    bs.segmentWeights(9.0f, i0, i1, w);
    expectTrue(i0 == 2 && i1 == 2 && w == 0.0f, "seg clamp high");
}

void testBlendSpaceEvaluate() {
    nl::anim::BlendSpace1D bs;
    expectTrue(bs.reset(makeLocoBlendSpace()), "reset ok");
    std::vector<nl::anim::BonePose> pose;

    // 段中点：param 0.5 -> lerp(idle z0, walk@0.5 z0.5, 0.5) = 0.25
    expectTrue(bs.evaluate(0.5f, 0.5f, pose), "eval 0.5");
    expectNear(true, "eval 0.5 z", pose[0].translation.z, 0.25f, 1e-6f);

    // param 1.5 -> lerp(walk z0.5, run z2, 0.5) = 1.25
    expectTrue(bs.evaluate(1.5f, 0.5f, pose), "eval 1.5");
    expectNear(true, "eval 1.5 z", pose[0].translation.z, 1.25f, 1e-6f);

    // 精确落点：param 1 -> 纯 walk（w=0 混合即 walk 本身）
    expectTrue(bs.evaluate(1.0f, 0.5f, pose), "eval 1.0");
    expectNear(true, "eval 1.0 z", pose[0].translation.z, 0.5f, 1e-6f);

    // 域外 clamp：-1 -> idle z0；9 -> run z2
    expectTrue(bs.evaluate(-1.0f, 0.5f, pose), "eval low");
    expectNear(true, "eval low z", pose[0].translation.z, 0.0f, 1e-6f);
    expectTrue(bs.evaluate(9.0f, 0.5f, pose), "eval high");
    expectNear(true, "eval high z", pose[0].translation.z, 2.0f, 1e-6f);

    // 单 sample 退化 = 常量 clip（任意 param 都返回该 clip；t=0.5 时 z=3.5。
    // 注意 t=1.0 会被 wrapTime 归到 0（循环 clip 语义），别拿端点当测试时点）
    nl::anim::BlendSpace1DDesc single;
    single.samples = { { 3.0f, makeLocoClip("hips", 7.0f) } };
    expectTrue(bs.reset(single), "single reset ok");
    expectTrue(bs.evaluate(0.0f, 0.5f, pose), "single eval");
    expectNear(true, "single eval z", pose[0].translation.z, 3.5f, 1e-6f);
}

void testBlendSpaceValidateNegative() {
    nl::anim::BlendSpace1DDesc desc = makeLocoBlendSpace();
    expectStr(nl::anim::validateBlendSpace1D(desc), "", "valid desc passes");

    nl::anim::BlendSpace1DDesc bad = desc;
    bad.samples[1].param = 0.0f;   // 递增被破坏（0,0,2）
    expectTrue(!nl::anim::validateBlendSpace1D(bad).empty(), "reject non-increasing");
    bad = desc;
    bad.samples.pop_back();        // 单 sample 合法，换成空
    bad.samples.clear();
    expectTrue(!nl::anim::validateBlendSpace1D(bad).empty(), "reject empty");
    bad = desc;
    bad.samples[2].param = std::nanf("");
    expectTrue(!nl::anim::validateBlendSpace1D(bad).empty(), "reject NaN param");
    bad = desc;
    bad.samples[2].clip.duration = 0.0f;
    expectTrue(!nl::anim::validateBlendSpace1D(bad).empty(), "reject invalid clip");
    bad = desc;
    bad.samples[2].clip.tracks.push_back(desc.samples[0].clip.tracks[0]);
    bad.samples[2].clip.tracks[1].bone = "head";
    expectTrue(!nl::anim::validateBlendSpace1D(bad).empty(), "reject rig mismatch");

    nl::anim::BlendSpace1D bs;
    expectTrue(!bs.reset(bad), "reset rejects invalid");
    std::vector<nl::anim::BonePose> pose;
    expectTrue(!bs.evaluate(0.0f, 0.0f, pose), "evaluate rejects un-reset");
}

void testBlendSpaceDeterminism() {
    nl::anim::BlendSpace1D bs;
    expectTrue(bs.reset(makeLocoBlendSpace()), "reset ok");
    std::vector<unsigned char> first, second;
    for (int run = 0; run < 2; ++run) {
        std::vector<unsigned char>& sink = run == 0 ? first : second;
        for (int i = 0; i <= 8; ++i) {
            std::vector<nl::anim::BonePose> pose;
            if (!bs.evaluate(0.25f * i, 0.5f, pose)) {
                ++g_failures;
                std::printf("  FAIL determinism sweep: eval %d\n", i);
                return;
            }
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(pose.data());
            sink.insert(sink.end(), bytes, bytes + pose.size() * sizeof(nl::anim::BonePose));
        }
    }
    expectTrue(first == second && !first.empty(), "two sweeps bit-identical");
}

int runBlendSpaceSelftest() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "segment weights", testBlendSpaceWeights },
        { "evaluate", testBlendSpaceEvaluate },
        { "validate negatives", testBlendSpaceValidateNegative },
        { "determinism", testBlendSpaceDeterminism },
    };
    for (const Case& c : cases) {
        std::printf("  [%s]\n", c.name);
        std::fflush(stdout);
        c.fn();
    }
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "selftest") == 0) {
        const std::string testCase = argv[2];
        if (testCase == "curve") {
            const int rc = runCurveSelftest();
            std::printf("anim curve selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        if (testCase == "blend") {
            const int rc = runBlendSelftest();
            std::printf("anim blend selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        if (testCase == "statemachine") {
            const int rc = runStateMachineSelftest();
            std::printf("anim statemachine selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        if (testCase == "blendspace") {
            const int rc = runBlendSpaceSelftest();
            std::printf("anim blendspace selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        std::fprintf(stderr, "anim-tools: unknown selftest case '%s'\n", testCase.c_str());
        return 2;
    }
    std::printf(
        "anim-tools - nl-engine action/animation controller (M-A track)\n"
        "usage:\n"
        "  anim-tools selftest curve|blend|statemachine|blendspace  deterministic unit tests\n");
    return argc == 1 ? 0 : 2;
}
