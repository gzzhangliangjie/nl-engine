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
        // M-A3/M-A4 用例接入处（statemachine / blendspace）
        std::fprintf(stderr, "anim-tools: unknown selftest case '%s'\n", testCase.c_str());
        return 2;
    }
    std::printf(
        "anim-tools - nl-engine action/animation controller (M-A track)\n"
        "usage:\n"
        "  anim-tools selftest curve|blend|statemachine|blendspace  deterministic unit tests\n");
    return argc == 1 ? 0 : 2;
}
