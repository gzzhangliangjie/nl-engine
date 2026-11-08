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
#include "nl/anim/gltf_anim.hpp"
#include "nl/anim/skeleton.hpp"
#include "nl/anim/skinning.hpp"
#include "nl/anim/state_machine.hpp"

#include <algorithm>
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

// ---------------------------------------------------------------------------
// M-A4 blendspace
// ---------------------------------------------------------------------------

// sample：确定性演示输出（证据图数据源，纯文本 ASCII，两次运行逐位一致）。
// 段 1 = 1D blend space 参数扫掠（0..2 步长 0.25，t=0.5）；
// 段 2 = 状态机 10 步 loco 场景（speed 输入驱动 idle/walk/run + 回退缩时）。
// M-A4b 标准资产演示：inspect <rigged.glb>
// 打印骨架统计 + clip 列表（名/时长/通道数）+ 抽 5 个时刻的末端骨骼 FK 世界位置。
// 全部输出确定性（同资产两次运行逐位一致），作为 M-A5 之前的 CPU 端演示替身。
int cmdInspect(const std::string& path) {
    nl::anim::Skeleton skeleton;
    std::vector<nl::anim::AnimClip> clips;
    std::string err;
    if (!nl::anim::loadGltfAnim(path, skeleton, clips, err)) {
        std::fprintf(stderr, "inspect: %s\n", err.c_str());
        return 1;
    }
    std::printf("asset %s\n", path.c_str());
    std::printf("bones %zu clips %zu\n", skeleton.boneCount(), clips.size());
    for (std::size_t b = 0; b < skeleton.boneCount(); ++b) {
        const int p = skeleton.parentIndices[b];
        std::printf("bone %zu %s parent %d restT %.4f %.4f %.4f\n", b,
                    skeleton.boneNames[b].c_str(), p, skeleton.restPose[b].translation.x,
                    skeleton.restPose[b].translation.y, skeleton.restPose[b].translation.z);
    }
    for (const nl::anim::AnimClip& clip : clips) {
        std::printf("clip duration %.4f tracks %zu\n", clip.duration, clip.tracks.size());
        // 末端骨骼 = 无子节点的最后一个骨骼（演示用近似：取 boneCount-1）。
        const std::size_t tip = skeleton.boneCount() - 1;
        std::printf("tip %s world (t=0.00,0.25,0.50,0.75,1.00 of duration)\n",
                    skeleton.boneNames[tip].c_str());
        for (int k = 0; k < 5; ++k) {
            const float t = clip.duration * 0.25f * static_cast<float>(k);
            std::vector<nl::anim::BonePose> local, model;
            if (!sampleClip(clip, t, local) || !localToModel(local, skeleton, model)) return 1;
            std::printf("  t=%.4f %.4f %.4f %.4f\n", t, model[tip].translation.x,
                        model[tip].translation.y, model[tip].translation.z);
        }
    }
    return 0;
}

// fksample <glb> <clipIndex> [samples] —— 整周期 FK 轨迹 TSV（确定性，证据图数据源）。
// 输出末端骨骼世界位置 (x,y,z) 随时间的采样表；两次运行逐位一致。
int cmdFkSample(const std::string& path, int clipIndex, int samples) {
    if (samples < 2 || samples > 4096) {
        std::fprintf(stderr, "fksample: samples must be in [2, 4096]\n");
        return 2;
    }
    nl::anim::Skeleton skeleton;
    std::vector<nl::anim::AnimClip> clips;
    std::string err;
    if (!nl::anim::loadGltfAnim(path, skeleton, clips, err)) {
        std::fprintf(stderr, "fksample: %s\n", err.c_str());
        return 1;
    }
    if (clipIndex < 0 || static_cast<std::size_t>(clipIndex) >= clips.size()) {
        std::fprintf(stderr, "fksample: clip index %d out of range [0, %zu)\n",
                     clipIndex, clips.size());
        return 2;
    }
    const nl::anim::AnimClip& clip = clips[static_cast<std::size_t>(clipIndex)];
    const std::size_t tip = skeleton.boneCount() - 1;
    std::printf("# asset %s clip %d duration %.4f bone %zu %s samples %d\n",
                path.c_str(), clipIndex, clip.duration, tip,
                skeleton.boneNames[tip].c_str(), samples);
    std::printf("# t x y z\n");
    for (int i = 0; i < samples; ++i) {
        const float t = clip.duration * static_cast<float>(i) / static_cast<float>(samples - 1);
        std::vector<nl::anim::BonePose> local, model;
        if (!sampleClip(clip, t, local) || !localToModel(local, skeleton, model)) {
            std::fprintf(stderr, "fksample: FK failed at t=%.4f\n", t);
            return 1;
        }
        std::printf("%.4f %.4f %.4f %.4f\n", t, model[tip].translation.x,
                    model[tip].translation.y, model[tip].translation.z);
    }
    return 0;
}

// fkdump <glb> <clipIndex> [frames] —— 全骨骼逐帧 FK 世界坐标 TSV（确定性，动画演示数据源）。
// 头部输出拓扑（父下标表），随后每帧一行：t + 每骨骼 (x,y,z)。
int cmdFkDump(const std::string& path, int clipIndex, int frames) {
    if (frames < 2 || frames > 2048) {
        std::fprintf(stderr, "fkdump: frames must be in [2, 2048]\n");
        return 2;
    }
    nl::anim::Skeleton skeleton;
    std::vector<nl::anim::AnimClip> clips;
    std::string err;
    if (!nl::anim::loadGltfAnim(path, skeleton, clips, err)) {
        std::fprintf(stderr, "fkdump: %s\n", err.c_str());
        return 1;
    }
    if (clipIndex < 0 || static_cast<std::size_t>(clipIndex) >= clips.size()) {
        std::fprintf(stderr, "fkdump: clip index %d out of range [0, %zu)\n",
                     clipIndex, clips.size());
        return 2;
    }
    const nl::anim::AnimClip& clip = clips[static_cast<std::size_t>(clipIndex)];
    const std::size_t B = skeleton.boneCount();
    std::printf("# fkdump %s clip %d duration %.4f bones %zu frames %d\n",
                path.c_str(), clipIndex, clip.duration, B, frames);
    std::printf("# topology (parent index per bone, -1 = root)\n");
    std::printf("P");
    for (const int p : skeleton.parentIndices) std::printf(" %d", p);
    std::printf("\n# per frame: t then bones*(x y z), DFS bone order\n");
    for (int i = 0; i < frames; ++i) {
        const float t = clip.duration * static_cast<float>(i) / static_cast<float>(frames);
        std::vector<nl::anim::BonePose> local, model;
        if (!sampleClip(clip, t, local) || !localToModel(local, skeleton, model)) {
            std::fprintf(stderr, "fkdump: FK failed at t=%.4f\n", t);
            return 1;
        }
        std::printf("F %.4f", t);
        for (const nl::anim::BonePose& m : model)
            std::printf(" %.4f %.4f %.4f", m.translation.x, m.translation.y, m.translation.z);
        std::printf("\n");
    }
    return 0;
}

// skincheck <glb> [t=0] [threshold=1.5] —— 蒙皮诊断：bind/skinned bbox、
// 位移超阈值顶点数，以及最坏几个顶点的绑定与调色板信息（定位拉飞顶点）。
int cmdSkinCheck(const std::string& path, float t, float threshold) {
    nl::anim::Skeleton skeleton;
    std::vector<nl::anim::AnimClip> clips;
    nl::anim::SkinnedMeshData mesh;
    nl::anim::SkinBinding skin;
    std::string err;
    if (!nl::anim::loadGltfSkinnedModel(path, skeleton, clips, mesh, skin, err)) {
        std::fprintf(stderr, "skincheck: %s\n", err.c_str());
        return 1;
    }
    if (clips.empty()) { std::fprintf(stderr, "skincheck: no clips\n"); return 1; }
    const nl::anim::AnimClip& clip = clips[0];
    const float tc = nl::anim::wrapTime(t, clip.duration);
    std::vector<nl::anim::BonePose> local(skeleton.boneCount());
    for (std::size_t i = 0; i < skeleton.boneCount(); ++i) local[i] = skeleton.restPose[i];
    std::vector<nl::anim::BonePose> model;
    if (!nl::anim::sampleClip(clip, tc, local) ||
        !nl::anim::localToModel(local, skeleton, model)) {
        std::fprintf(stderr, "skincheck: sample/FK failed\n");
        return 1;
    }
    std::vector<nl::anim::Mat4> palette;
    nl::anim::computePalette(palette, model, skin.inverseBind);
    std::vector<nl::anim::Vec3> skinned;
    nl::anim::skinPositions(skinned, mesh.positions, palette, skin);

    auto bbox = [](const std::vector<nl::anim::Vec3>& v) {
        nl::anim::Vec3 lo = v[0], hi = v[0];
        for (const auto& p : v) {
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        }
        return std::pair<nl::anim::Vec3, nl::anim::Vec3>(lo, hi);
    };
    const auto [blo, bhi] = bbox(mesh.positions);
    const auto [slo, shi] = bbox(skinned);
    std::printf("asset %s t=%.4f (clip %.4fs) vertices %zu\n",
                path.c_str(), tc, clip.duration, mesh.positions.size());
    std::printf("bind  bbox (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n",
                blo.x, blo.y, blo.z, bhi.x, bhi.y, bhi.z);
    std::printf("skin  bbox (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n",
                slo.x, slo.y, slo.z, shi.x, shi.y, shi.z);
    for (std::size_t i = 0; i < skeleton.boneCount(); ++i)
        std::printf("bone %zu %s parent %d worldT %.4f %.4f %.4f\n", i,
                    skeleton.boneNames[i].c_str(), skeleton.parentIndices[i],
                    model[i].translation.x, model[i].translation.y,
                    model[i].translation.z);

    std::vector<std::pair<float, std::size_t>> worst;
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        const float dx = skinned[i].x - mesh.positions[i].x;
        const float dy = skinned[i].y - mesh.positions[i].y;
        const float dz = skinned[i].z - mesh.positions[i].z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > threshold) worst.push_back({ d, i });
    }
    std::sort(worst.begin(), worst.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::printf("displaced > %.2fm: %zu\n", threshold, worst.size());
    for (int k = 0; k < 5 && k < static_cast<int>(worst.size()); ++k) {
        const std::size_t i = worst[static_cast<std::size_t>(k)].second;
        std::printf("  v%zu bind(%.3f %.3f %.3f) skin(%.3f %.3f %.3f) d=%.3f j=[%d %d %d %d] w=[%.2f %.2f %.2f %.2f]\n",
                    i, mesh.positions[i].x, mesh.positions[i].y, mesh.positions[i].z,
                    skinned[i].x, skinned[i].y, skinned[i].z,
                    worst[static_cast<std::size_t>(k)].first,
                    skin.joints[i][0], skin.joints[i][1], skin.joints[i][2], skin.joints[i][3],
                    skin.weights[i][0], skin.weights[i][1], skin.weights[i][2], skin.weights[i][3]);
        for (int jb = 0; jb < 4; ++jb) {
            const int j = skin.joints[i][static_cast<std::size_t>(jb)];
            if (j < 0 || skin.weights[i][static_cast<std::size_t>(jb)] == 0.0f) continue;
            const nl::anim::Mat4& p = palette[static_cast<std::size_t>(j)];
            std::printf("    j%d %-14s palT(%.3f %.3f %.3f) modelT(%.3f %.3f %.3f)\n",
                        j, skeleton.boneNames[static_cast<std::size_t>(j)].c_str(),
                        p.m[12], p.m[13], p.m[14],
                        model[static_cast<std::size_t>(j)].translation.x,
                        model[static_cast<std::size_t>(j)].translation.y,
                        model[static_cast<std::size_t>(j)].translation.z);
        }
    }
    return 0;
}

int cmdSample() {
    nl::anim::BlendSpace1D bs;
    if (!bs.reset(makeLocoBlendSpace())) {
        std::fprintf(stderr, "sample: internal error: blendspace reset failed\n");
        return 1;
    }
    std::printf("anim-tools sample (deterministic; two runs are bit-identical)\n");
    std::printf("# section 1: blendspace sweep t=0.5 (param -> z of hips)\n");
    std::printf("param z\n");
    for (int i = 0; i <= 8; ++i) {
        const float param = 0.25f * static_cast<float>(i);
        std::vector<nl::anim::BonePose> pose;
        if (!bs.evaluate(param, 0.5f, pose)) return 1;
        std::size_t i0, i1; float w;
        bs.segmentWeights(param, i0, i1, w);
        std::printf("%.2f %.4f (seg %zu->%zu w=%.3f)\n",
                    param, pose[0].translation.z, i0, i1, w);
    }
    nl::anim::AnimationStateMachine sm;
    if (!sm.reset(makeLocoDesc())) {
        std::fprintf(stderr, "sample: internal error: statemachine reset failed\n");
        return 1;
    }
    std::printf("# section 2: statemachine scenario dt=0.1 (step speed state alpha z)\n");
    struct Step { float dt; float speed; };
    const Step steps[] = {
        { 0.1f, 0.0f }, { 0.1f, 0.5f }, { 0.1f, 0.5f }, { 0.1f, 0.5f },
        { 0.1f, 2.0f }, { 0.1f, 2.0f }, { 0.1f, 0.0f }, { 0.1f, 0.0f },
        { 0.1f, 0.0f }, { 0.1f, 0.0f },
    };
    for (int i = 0; i < 10; ++i) {
        sm.update(steps[i].dt, { steps[i].speed });
        std::vector<nl::anim::BonePose> pose;
        if (!sm.evaluate(pose)) return 1;
        std::printf("%d %.1f %s %.3f %.4f\n", i, steps[i].speed,
                    sm.currentStateName().c_str(), sm.transitionAlpha(),
                    pose[0].translation.z);
    }
    return 0;
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

// ---- M-A4b：Skeleton FK + rigged glTF 入口 --------------------------------

using nl::anim::BonePose;
using nl::anim::Skeleton;
using nl::anim::Vec3;

void testSkeletonFkAnchors() {
    using nl::anim::findBoneIndex;
    using nl::anim::localToModel;
    using nl::anim::quatFromAxisAngle;
    using nl::anim::validateSkeleton;

    // 三链：hip(-(1,0,0) 偏移? 不——直接用资产同款几何：hip 平移 (0,1,0))
    Skeleton sk;
    sk.boneNames = { "hip", "spine", "head" };
    sk.parentIndices = { -1, 0, 1 };
    sk.restPose = { { { 0, 1, 0 }, {} }, { { 0, 0.5f, 0 }, {} }, { { 0, 0.5f, 0 }, {} } };
    expectTrue(validateSkeleton(sk).empty(), "fk skeleton valid");
    expectTrue(findBoneIndex(sk, "spine") == 1, "findBoneIndex");

    // 局部：hip 绕 z 90°，spine/head identity -> 全链 90°
    // （local = 采样输出语义：静态 rest 平移已进轨道，见 gltf_anim.hpp）
    std::vector<BonePose> local = sk.restPose;
    local[0].rotation = quatFromAxisAngle({ 0, 0, 1 }, 1.57079632679f);   // 90°
    std::vector<BonePose> model;
    expectTrue(localToModel(local, sk, model), "fk eval");
    // spineWorld = (0,1) + rotZ90*(0,0.5,0) = (-0.5, 1)
    expectNear(true, "fk spine x", model[1].translation.x, -0.5f, 1e-6f);
    expectNear(true, "fk spine y", model[1].translation.y, 1.0f, 1e-6f);
    // headWorld = (-0.5,1) + rotZ90*(0,0.5,0) = (-1.0, 1)
    expectNear(true, "fk head x", model[2].translation.x, -1.0f, 1e-6f);
    expectNear(true, "fk head y", model[2].translation.y, 1.0f, 1e-6f);
    // 链复合旋转 = 90°（三层同向 identity 叠加不变）
    expectNear(true, "fk head rot z", model[2].rotation.z, 0.70710678118f, 1e-6f);
    expectNear(true, "fk head rot w", model[2].rotation.w, 0.70710678118f, 1e-6f);

    // 反向断言：无旋转时 FK = rest 平移逐级累加（local = restPose 原样）
    std::vector<BonePose> rest = sk.restPose;
    expectTrue(localToModel(rest, sk, model), "fk rest");
    expectNear(true, "fk rest head y", model[2].translation.y, 2.0f, 1e-6f);

    // 负向：尺寸不匹配
    std::vector<BonePose> two(2);
    expectTrue(!localToModel(two, sk, model), "fk size mismatch rejected");

    // 校验负向：父索引打破拓扑序 / 重名
    Skeleton bad = sk;
    bad.parentIndices[2] = 2;   // 自指（非 [0,i)）
    expectTrue(!validateSkeleton(bad).empty(), "fk topo negative");
    bad = sk;
    bad.boneNames[1] = "hip";
    expectTrue(!validateSkeleton(bad).empty(), "fk dup name negative");
}

void testGltfAssetLoad(const std::string& path) {
    using nl::anim::loadGltfAnim;
    using nl::anim::sampleClip;
    using nl::anim::localToModel;
    using nl::anim::quatFromAxisAngle;

    Skeleton sk;
    std::vector<nl::anim::AnimClip> clips;
    std::string err;
    // ⭐加载失败必须早退：后续 clips[0]/restPose[0] 直接索引加载产物，
    //   缺文件时打空 vector 是未定义行为（regression cwd 错位实测段错误）。
    if (!loadGltfAnim(path, sk, clips, err)) {
        expectTrue(false, ("gltf load: " + err).c_str());
        return;
    }
    expectTrue(true, "gltf load");
    expectTrue(sk.boneCount() == 3, "gltf bones=3");
    expectTrue(sk.boneNames[0] == "hip" && sk.boneNames[2] == "head", "gltf bone names");
    expectTrue(sk.parentIndices == std::vector<int>{ -1, 0, 1 }, "gltf parents");
    // 静态 TRS 落 restPose（空轨道兜底是零平移，必须显式单关键帧——见 gltf_anim.hpp）
    expectNear(true, "gltf rest hip y", sk.restPose[0].translation.y, 1.0f, 1e-6f);

    expectTrue(clips.size() == 1, "gltf clips=1");
    expectNear(true, "gltf clip duration", clips[0].duration, 2.0f, 1e-6f);
    expectTrue(nl::anim::validateClip(clips[0]).empty(), "gltf clip valid");

    // 锚点 t=0.5：hip=+30°、spine=0、head=0（资产生成器注释里的手算值）
    std::vector<BonePose> local, model;
    expectTrue(sampleClip(clips[0], 0.5f, local), "gltf sample t=0.5");
    expectNear(true, "sway hip rot z", local[0].rotation.z, 0.25881904510f, 1e-6f);
    expectNear(true, "sway hip rot w", local[0].rotation.w, 0.96592582628f, 1e-6f);
    expectNear(true, "sway spine rot z", local[1].rotation.z, 0.0f, 1e-7f);
    expectTrue(localToModel(local, sk, model), "sway fk");
    // spineWorld = (-0.25, 1.4330127)；headWorld = (-0.5, 1.8660254)
    expectNear(true, "sway spine wx", model[1].translation.x, -0.25f, 1e-6f);
    expectNear(true, "sway spine wy", model[1].translation.y, 1.43301270189f, 1e-6f);
    expectNear(true, "sway head wx", model[2].translation.x, -0.5f, 1e-6f);
    expectNear(true, "sway head wy", model[2].translation.y, 1.86602540378f, 1e-6f);

    // 静态平移守恒：整链 world y 基线来自 rest（1+0.5+0.5），旋转不改变链长
    const float reach = model[2].translation.x * model[2].translation.x +
                        (model[2].translation.y - 1.0f) * (model[2].translation.y - 1.0f);
    expectNear(true, "sway reach = 1.0", std::sqrt(reach), 1.0f, 1e-6f);

    // wrap 循环：t=2.5 == t=0.5（逐位一致，determinism 口径）
    std::vector<BonePose> localWrap;
    expectTrue(sampleClip(clips[0], 2.5f, localWrap), "gltf sample t=2.5");
    bool same = true;
    for (std::size_t i = 0; i < local.size(); ++i)
        same = same && local[i].rotation.x == localWrap[i].rotation.x &&
               local[i].rotation.y == localWrap[i].rotation.y &&
               local[i].rotation.z == localWrap[i].rotation.z &&
               local[i].rotation.w == localWrap[i].rotation.w &&
               local[i].translation.x == localWrap[i].translation.x &&
               local[i].translation.y == localWrap[i].translation.y;
    expectTrue(same, "gltf wrap determinism");

    // 负向：缺文件
    Skeleton sk2;
    std::vector<nl::anim::AnimClip> clips2;
    std::string err2;
    expectTrue(!loadGltfAnim(path + ".missing", sk2, clips2, err2), "gltf missing file rejected");
}

void testGltfDeterminism(const std::string& path) {
    Skeleton skA, skB;
    std::vector<nl::anim::AnimClip> a, b;
    std::string err;
    // ⭐同 testGltfAssetLoad：加载失败早退，防 a[0] 打空 vector。
    const bool okA = loadGltfAnim(path, skA, a, err);
    const bool okB = okA && loadGltfAnim(path, skB, b, err);
    if (!okA || !okB) {
        expectTrue(false, okA ? "det load B" : ("det load A: " + err).c_str());
        return;
    }
    expectTrue(true, "det load A");
    expectTrue(true, "det load B");
    bool same = skA.boneNames == skB.boneNames &&
                skA.parentIndices == skB.parentIndices &&
                a.size() == b.size() && a[0].duration == b[0].duration &&
                a[0].tracks.size() == b[0].tracks.size();
    for (std::size_t t = 0; same && t < a[0].tracks.size(); ++t) {
        same = a[0].tracks[t].bone == b[0].tracks[t].bone &&
               a[0].tracks[t].rotation.times == b[0].tracks[t].rotation.times;
        for (std::size_t k = 0; same && k < a[0].tracks[t].rotation.values.size(); ++k) {
            const nl::anim::Quat& qa = a[0].tracks[t].rotation.values[k];
            const nl::anim::Quat& qb = b[0].tracks[t].rotation.values[k];
            same = qa.x == qb.x && qa.y == qb.y && qa.z == qb.z && qa.w == qb.w;
        }
    }
    expectTrue(same, "two loads identical");
}

// ---- M-A5：蒙皮（skinning）确定性自测 ----

// 3 骨骼链（与 skeleton FK 用例同款）：hip→spine→head，rest 平移
// (0,1,0)/(0,0.5,0)/(0,0.5,0) => rest 世界 (0,1)/(0,1.5)/(0,2)。
// IBM = rest 世界矩阵的逆（纯 T）：T(0,-1)/T(0,-1.5)/T(0,-2)。
nl::anim::Skeleton makeChainSkeleton() {
    nl::anim::Skeleton sk;
    sk.boneNames = { "hip", "spine", "head" };
    sk.parentIndices = { -1, 0, 1 };
    for (const float y : { 1.0f, 0.5f, 0.5f })
        sk.restPose.push_back({ { 0, y, 0 }, nl::anim::quatIdentity() });
    return sk;
}

nl::anim::SkinBinding makeChainBinding(nl::anim::Skeleton& sk) {
    nl::anim::SkinBinding b;
    b.inverseBind = {
        nl::anim::mat4FromTR({ 0, -1, 0 }, nl::anim::quatIdentity()),
        nl::anim::mat4FromTR({ 0, -1.5f, 0 }, nl::anim::quatIdentity()),
        nl::anim::mat4FromTR({ 0, -2, 0 }, nl::anim::quatIdentity()),
    };
    (void)sk;
    return b;
}

void testSkinningRigidAnchors() {
    nl::anim::Skeleton sk = makeChainSkeleton();
    nl::anim::SkinBinding b = makeChainBinding(sk);

    // 姿态：hip 绕 Z 90°（与 FK 用例同款）→ spine(-0.5,1) / head(-1,1)
    std::vector<nl::anim::BonePose> local(3);
    local[0].translation = { 0, 1, 0 };
    local[0].rotation = nl::anim::quatFromAxisAngle({ 0, 0, 1 }, 1.57079632679f);
    local[1].translation = { 0, 0.5f, 0 };
    local[2].translation = { 0, 0.5f, 0 };
    std::vector<nl::anim::BonePose> model;
    expectTrue(nl::anim::localToModel(local, sk, model), "fk ok");

    std::vector<nl::anim::Mat4> palette;
    nl::anim::computePalette(palette, model, b.inverseBind);

    // 刚体绑定 v=(0,2,0)@head：palette*T = T(-1,-1) → (-1,1) == head 世界位
    std::vector<nl::anim::Vec3> base = { { 0, 2, 0 } };
    nl::anim::SkinBinding one = b;
    one.joints = { { 2, -1, -1, -1 } };
    one.weights = { { 1, 0, 0, 0 } };
    std::vector<nl::anim::Vec3> out;
    nl::anim::skinPositions(out, base, palette, one);
    expectNear(true, "rigid head x", out[0].x, -1.0f, 1e-5f);
    expectNear(true, "rigid head y", out[0].y, 1.0f, 1e-5f);

    // 刚体绑定 v=(0,1,0)@hip：绕髋关节转，位置不动（旋转枢轴语义）
    one.joints = { { 0, -1, -1, -1 } };
    base = { { 0, 1, 0 } };
    nl::anim::skinPositions(out, base, palette, one);
    expectNear(true, "rigid hip x", out[0].x, 0.0f, 1e-5f);
    expectNear(true, "rigid hip y", out[0].y, 1.0f, 1e-5f);

    // LBS：v=(0,1.5,0)，w=(hip 0.5, head 0.5) →
    // palette_hip(0,1.5)=(0,1.5)（绕髋不动点）、palette_head(0,1.5)=(-1,0.5)
    // → 混合 (-0.5, 1.0)
    base = { { 0, 1.5f, 0 } };
    one.joints = { { 0, 2, -1, -1 } };
    one.weights = { { 0.5f, 0.5f, 0, 0 } };
    nl::anim::skinPositions(out, base, palette, one);
    expectNear(true, "lbs x", out[0].x, -0.5f, 1e-5f);
    expectNear(true, "lbs y", out[0].y, 1.0f, 1e-5f);

    // 法线：n=(0,1,0)@head，hip 90° → (-1,0,0) 归一化
    std::vector<nl::anim::Vec3> nbase = { { 0, 1, 0 } };
    one.joints = { { 2, -1, -1, -1 } };
    one.weights = { { 1, 0, 0, 0 } };
    std::vector<nl::anim::Vec3> nout;
    nl::anim::skinNormals(nout, nbase, palette, one);
    expectNear(true, "skin normal x", nout[0].x, -1.0f, 1e-5f);
    expectNear(true, "skin normal y", nout[0].y, 0.0f, 1e-5f);
}

void testSkinningValidateNegative() {
    nl::anim::Skeleton sk = makeChainSkeleton();
    nl::anim::SkinBinding b = makeChainBinding(sk);
    b.joints = { { 2, -1, -1, -1 } };
    b.weights = { { 1, 0, 0, 0 } };
    expectTrue(nl::anim::validateSkinBinding(b, 1, 3).empty(), "valid ok");

    expectTrue(!nl::anim::validateSkinBinding(b, 2, 3).empty(), "vertex count mismatch");
    nl::anim::SkinBinding bad = b;
    bad.joints = { { 3, -1, -1, -1 } };
    expectTrue(!nl::anim::validateSkinBinding(bad, 1, 3).empty(), "joint out of range");
    bad = b;
    bad.weights = { { 0.5f, 0, 0, 0 } };
    expectTrue(!nl::anim::validateSkinBinding(bad, 1, 3).empty(), "weight sum != 1");
    bad = b;
    bad.inverseBind[0].m[0] = std::numeric_limits<float>::quiet_NaN();
    expectTrue(!nl::anim::validateSkinBinding(bad, 1, 3).empty(), "non-finite IBM");
    bad = b;
    bad.inverseBind.pop_back();
    expectTrue(!nl::anim::validateSkinBinding(bad, 1, 3).empty(), "IBM count != bones");
}

// 真实资产端到端：bind pose ≠ 节点 TRS rest 姿态的资产（CesiumMan：bind=T-pose、
// TRS=垂臂行走姿）没有「rest 调色板≈identity」保证（该前提对 RiggedChain 这类
// 程序化资产成立，对 CesiumMan 是错的——之前能过是槽位映射与骨架两错相消的假象，
// 2026-11-08 修正）。改用两条强判据：
// ① 网格撕裂检测：蒙皮后最大三角形边长 / bind 最大边长 < 2（错误关节映射会把
//    顶点拉飞，边长爆炸；正确蒙皮相邻顶点同骨骼近似刚体运动，边长几乎不变）。
// ② 主关节邻近：每个顶点到其最大权重关节世界位置的距离 < 1.2m（四肢长度量级）。
void testSkinnedAssetCoherence(const std::string& assetPath) {
    nl::anim::Skeleton skeleton;
    std::vector<nl::anim::AnimClip> clips;
    nl::anim::SkinnedMeshData mesh;
    nl::anim::SkinBinding skin;
    std::string err;
    if (!nl::anim::loadGltfSkinnedModel(assetPath, skeleton, clips, mesh, skin, err)) {
        ++g_failures;
        std::printf("  FAIL load skinned asset: %s\n", err.c_str());
        return;
    }
    expectTrue(mesh.positions.size() > 0, "asset has vertices");
    expectTrue(mesh.indices.size() % 3 == 0, "asset indices are triangles");
    if (clips.empty()) { expectTrue(false, "asset has clips"); return; }

    std::vector<nl::anim::BonePose> local(skeleton.boneCount());
    for (std::size_t i = 0; i < skeleton.boneCount(); ++i) local[i] = skeleton.restPose[i];
    std::vector<nl::anim::BonePose> model;
    if (!nl::anim::sampleClip(clips[0], 0.0f, local) ||
        !nl::anim::localToModel(local, skeleton, model)) {
        expectTrue(false, "sample/FK failed");
        return;
    }
    std::vector<nl::anim::Mat4> palette;
    nl::anim::computePalette(palette, model, skin.inverseBind);
    std::vector<nl::anim::Vec3> skinned;
    nl::anim::skinPositions(skinned, mesh.positions, palette, skin);

    auto maxEdge = [&mesh, &skinned](bool useSkinned) {
        float m = 0.0f;
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t a = mesh.indices[t + static_cast<std::size_t>(k)];
                const std::uint32_t b = mesh.indices[t + static_cast<std::size_t>((k + 1) % 3)];
                const nl::anim::Vec3& pa = useSkinned ? skinned[a] : mesh.positions[a];
                const nl::anim::Vec3& pb = useSkinned ? skinned[b] : mesh.positions[b];
                const float dx = pa.x - pb.x, dy = pa.y - pb.y, dz = pa.z - pb.z;
                m = std::max(m, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        return m;
    };
    const float edgeBind = maxEdge(false);
    const float edgeSkin = maxEdge(true);
    if (edgeSkin / edgeBind >= 2.0f) {
        ++g_failures;
        std::printf("  FAIL mesh torn: maxEdge bind %.4f -> skin %.4f (ratio %.2f)\n",
                    edgeBind, edgeSkin, edgeSkin / edgeBind);
    }

    float worstJointDist = 0.0f;
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        int dom = -1; float w = 0.0f;
        for (int k = 0; k < 4; ++k)
            if (skin.weights[i][static_cast<std::size_t>(k)] > w) {
                w = skin.weights[i][static_cast<std::size_t>(k)];
                dom = skin.joints[i][static_cast<std::size_t>(k)];
            }
        if (dom < 0) continue;
        const nl::anim::Vec3& jt = model[static_cast<std::size_t>(dom)].translation;
        const float dx = skinned[i].x - jt.x, dy = skinned[i].y - jt.y,
                    dz = skinned[i].z - jt.z;
        worstJointDist = std::max(worstJointDist,
                                  std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (worstJointDist >= 1.2f) {
        ++g_failures;
        std::printf("  FAIL vertex far from dominant joint: %.3fm\n", worstJointDist);
    }
}

int runSkinningSelftest(const std::string& assetPath) {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "rigid/lbs anchors", testSkinningRigidAnchors },
        { "validate negatives", testSkinningValidateNegative },
    };
    for (const Case& c : cases) {
        std::printf("  [%s]\n", c.name);
        std::fflush(stdout);
        c.fn();
    }
    testSkinnedAssetCoherence(assetPath);
    return g_failures == 0 ? 0 : 1;
}

int runSkeletonSelftest(const std::string& assetPath) {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "fk anchors", testSkeletonFkAnchors },
    };
    for (const Case& c : cases) {
        std::printf("  [%s]\n", c.name);
        std::fflush(stdout);
        c.fn();
    }
    testGltfAssetLoad(assetPath);
    testGltfDeterminism(assetPath);
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
        // M-A4 用例接入处（blendspace）
        if (testCase == "blendspace") {
            const int rc = runBlendSpaceSelftest();
            std::printf("anim blendspace selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        // M-A4b 用例接入处（skeleton：FK + rigged glTF 入口）
        if (testCase == "skeleton") {
            const std::string assetPath =
                argc >= 4 ? argv[3] : "assets/models/RiggedChain.glb";
            const int rc = runSkeletonSelftest(assetPath);
            std::printf("anim skeleton selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        if (testCase == "skinning") {
            const std::string assetPath =
                argc >= 4 ? argv[3] : "assets/models/rigged/CesiumMan.glb";
            const int rc = runSkinningSelftest(assetPath);
            std::printf("anim skinning selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        std::fprintf(stderr, "anim-tools: unknown selftest case '%s'\n", testCase.c_str());
        return 2;
    }
    if (argc >= 2 && std::strcmp(argv[1], "sample") == 0)
        return cmdSample();
    if (argc >= 3 && std::strcmp(argv[1], "inspect") == 0)
        return cmdInspect(argv[2]);
    if (argc >= 4 && std::strcmp(argv[1], "fksample") == 0) {
        const int clipIndex = std::atoi(argv[3]);
        const int samples = argc >= 5 ? std::atoi(argv[4]) : 200;
        return cmdFkSample(argv[2], clipIndex, samples);
    }
    if (argc >= 3 && std::strcmp(argv[1], "skincheck") == 0) {
        const float t = argc >= 4 ? static_cast<float>(std::atof(argv[3])) : 0.0f;
        const float th = argc >= 5 ? static_cast<float>(std::atof(argv[4])) : 1.5f;
        return cmdSkinCheck(argv[2], t, th);
    }
    if (argc >= 4 && std::strcmp(argv[1], "fkdump") == 0) {
        const int clipIndex = std::atoi(argv[3]);
        const int frames = argc >= 5 ? std::atoi(argv[4]) : 64;
        return cmdFkDump(argv[2], clipIndex, frames);
    }
    std::printf(
        "anim-tools - nl-engine action/animation controller (M-A track)\n"
        "usage:\n"
        "  anim-tools selftest curve|blend|statemachine|blendspace|skeleton [rigged.glb]\n"
        "    deterministic unit tests\n"
        "  anim-tools inspect <rigged.glb>   skeleton/clip stats + tip FK trajectory\n"
        "  anim-tools fksample <rigged.glb> <clipIndex> [samples=200]\n"
        "    full-cycle tip FK world trajectory (TSV, deterministic)\n"
        "  anim-tools fkdump <rigged.glb> <clipIndex> [frames=64]\n"
        "    per-frame all-bone FK world positions (TSV, demo data source)\n");
    return argc == 1 ? 0 : 2;
}
