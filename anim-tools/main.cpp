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

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "selftest") == 0) {
        const std::string testCase = argv[2];
        if (testCase == "curve") {
            const int rc = runCurveSelftest();
            std::printf("anim curve selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
        // M-A2/M-A3/M-A4 用例接入处（blend / statemachine / blendspace）
        std::fprintf(stderr, "anim-tools: unknown selftest case '%s'\n", testCase.c_str());
        return 2;
    }
    std::printf(
        "anim-tools - nl-engine action/animation controller (M-A track)\n"
        "usage:\n"
        "  anim-tools selftest curve|blend|statemachine|blendspace  deterministic unit tests\n");
    return argc == 1 ? 0 : 2;
}
