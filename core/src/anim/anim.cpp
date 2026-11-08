// M-A1：AnimClip 采样实现（口径见 anim.hpp 文件头，不自造机制）。
//
// 关键帧定位 = 二分（std::upper_bound），对齐 UE GetKeyIndicesFromTime 的
// 「秒 -> 区间 [i0,i1] + alpha」三步，只是我们关键帧自带时间戳（bevy 口径），
// 不用 UE 的均匀帧布局。
//   Linear: value = lerp(v0, v1, alpha)（UE FTransform::Blend 同式）
//   Step:   value = v0（gltf STEP：保持前 key，alpha 直接置 0）
//   Quat:   slerp（最短路径 + 归一化，见 anim_math.hpp 文件头）

#include "nl/anim/anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nl::anim {
namespace {

bool finiteVec3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finiteQuat(const Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

bool validTrackTimes(const std::vector<float>& times, std::size_t valueCount, std::string* error, const char* what) {
    if (times.empty() || times.size() != valueCount) {
        if (error) *error = std::string(what) + ": times/values size mismatch or empty";
        return false;
    }
    if (!std::isfinite(times[0]) || times[0] < 0.0f) {
        // 首 key 不必为 0（glTF 只要求递增；CesiumMan 实测 translation 通道
        // 从 0.4167s 起步）：t < 首 key 时 clamp 到首值（sample_clamped 语义
        // 已实现，域内语义自洽）。只拒绝负时间与非有限值。
        if (error) *error = std::string(what) + ": first key time must be finite and >= 0";
        return false;
    }
    for (std::size_t i = 1; i < times.size(); ++i) {
        if (!(times[i] > times[i - 1]) || !std::isfinite(times[i])) {
            if (error) *error = std::string(what) + ": key times must be finite and strictly increasing";
            return false;
        }
    }
    return true;
}

} // namespace

std::string validateClip(const AnimClip& clip) {
    if (!(clip.duration > 0.0f) || !std::isfinite(clip.duration))
        return "clip: duration must be finite and > 0";
    std::string error;
    for (const BoneTrack& track : clip.tracks) {
        if (track.bone.empty()) return "clip: bone name must not be empty";
        // 空通道 = 常量语义（与 sampleVec3Track/sampleQuatTrack 一致），跳过校验
        if (!track.translation.times.empty() &&
            !validTrackTimes(track.translation.times, track.translation.values.size(), &error, "translation"))
            return error;
        if (!track.rotation.times.empty() &&
            !validTrackTimes(track.rotation.times, track.rotation.values.size(), &error, "rotation"))
            return error;
        for (const Vec3& v : track.translation.values)
            if (!finiteVec3(v)) return "translation: non-finite value";
        for (const Quat& q : track.rotation.values) {
            if (!finiteQuat(q)) return "rotation: non-finite value";
            if (track.rotation.interp == Interp::Linear &&
                std::sqrt(dotQuat(q, q)) < 1e-6f)
                return "rotation: zero-length quaternion";
        }
    }
    for (std::size_t i = 0; i < clip.tracks.size(); ++i)
        for (std::size_t j = i + 1; j < clip.tracks.size(); ++j)
            if (clip.tracks[i].bone == clip.tracks[j].bone)
                return "clip: duplicate bone track";
    return {};
}

float wrapTime(float t, float duration) {
    if (!(duration > 0.0f)) return t;
    // std::fmod 结果与 t 同号；负 t 一次性加回（不循环取模，负回绕无循环语义需求）
    float wrapped = std::fmod(t, duration);
    if (wrapped < 0.0f) wrapped += duration;
    return wrapped;
}

Vec3 sampleVec3Track(const Vec3Track& track, float t) {
    if (track.times.empty()) return {};
    if (t <= track.times.front()) return track.values.front();   // clamp 下界
    if (t >= track.times.back()) return track.values.back();     // clamp 上界
    // upper_bound：第一个 > t 的 key -> 区间 [i-1, i]
    const auto it = std::upper_bound(track.times.begin(), track.times.end(), t);
    const std::size_t i1 = static_cast<std::size_t>(it - track.times.begin());
    const std::size_t i0 = i1 - 1;
    if (track.interp == Interp::Step) return track.values[i0];
    const float t0 = track.times[i0], t1 = track.times[i1];
    const float alpha = (t - t0) / (t1 - t0);
    return lerpVec3(track.values[i0], track.values[i1], alpha);
}

Quat sampleQuatTrack(const QuatTrack& track, float t) {
    if (track.times.empty()) return quatIdentity();
    if (t <= track.times.front()) return track.values.front();
    if (t >= track.times.back()) return track.values.back();
    const auto it = std::upper_bound(track.times.begin(), track.times.end(), t);
    const std::size_t i1 = static_cast<std::size_t>(it - track.times.begin());
    const std::size_t i0 = i1 - 1;
    if (track.interp == Interp::Step) return track.values[i0];
    const float t0 = track.times[i0], t1 = track.times[i1];
    const float alpha = (t - t0) / (t1 - t0);
    return normalizeQuat(slerpQuat(track.values[i0], track.values[i1], alpha));
}

bool sampleClip(const AnimClip& clip, float t, std::vector<BonePose>& out) {
    if (!validateClip(clip).empty()) return false;
    const float time = wrapTime(t, clip.duration);
    out.clear();
    out.reserve(clip.tracks.size());
    for (const BoneTrack& track : clip.tracks) {
        BonePose pose;
        pose.translation = sampleVec3Track(track.translation, time);
        pose.rotation = sampleQuatTrack(track.rotation, time);
        out.push_back(pose);
    }
    return true;
}

std::size_t findTrackIndex(const AnimClip& clip, const std::string& bone) {
    for (std::size_t i = 0; i < clip.tracks.size(); ++i)
        if (clip.tracks[i].bone == bone) return i;
    return static_cast<std::size_t>(-1);
}

// ---- M-A2：混合 ------------------------------------------------------------

BonePose lerpPose(const BonePose& a, const BonePose& b, float w) {
    if (w <= 0.0f) return a;
    if (w >= 1.0f) return b;
    BonePose out;
    out.translation = lerpVec3(a.translation, b.translation, w);
    out.rotation = normalizeQuat(slerpQuat(a.rotation, b.rotation, w));
    return out;
}

bool blendPoses(const std::vector<BonePose>& a, const std::vector<BonePose>& b, float w,
                std::vector<BonePose>& out) {
    if (a.size() != b.size()) return false;
    out.clear();
    out.reserve(a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        out.push_back(lerpPose(a[i], b[i], w));
    return true;
}

BonePose addPose(const BonePose& base, const BonePose& delta, float w) {
    BonePose out = base;
    out.translation = base.translation + delta.translation * w;
    out.rotation = normalizeQuat(mulQuat(scaleQuat(delta.rotation, w), base.rotation));
    return out;
}

bool blendClips(const AnimClip& a, float timeA,
                const AnimClip& b, float timeB, float w,
                std::vector<BonePose>& out) {
    if (a.tracks.size() != b.tracks.size()) return false;
    if (!validateClip(a).empty() || !validateClip(b).empty()) return false;
    std::vector<BonePose> pa, pb;
    sampleClip(a, timeA, pa);
    sampleClip(b, timeB, pb);
    return blendPoses(pa, pb, w, out);
}

} // namespace nl::anim
