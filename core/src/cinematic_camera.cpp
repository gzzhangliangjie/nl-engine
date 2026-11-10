// P8 M-CAM3a：CinematicCamera 实现（M-CAM1a 桩升真实现，语义见头文件口径注释）。

#include "nl/core/cinematic_camera.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace nl::core {

namespace {

// TAB 分列；容许 \r\n 行尾。列数必须恰为 8，否则该行判失败。
bool parseTsvLine(const char* line, TrackKeyframe& kf) {
    // 跳过行首空白
    while (*line == ' ' || *line == '\t') ++line;
    std::vector<float> v;
    v.reserve(8);
    const char* p = line;
    while (*p) {
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p) return false;              // 非数值 token
        v.push_back(static_cast<float>(d));
        p = end;
        if (*p == '\t') { ++p; continue; }
        break;
    }
    while (*p == ' ' || *p == '\r' || *p == '\n') ++p;
    if (*p != '\0') return false;                // 尾部残渣
    if (v.size() != 8) return false;
    kf.timeSeconds = v[0];
    kf.eye[0] = v[1]; kf.eye[1] = v[2]; kf.eye[2] = v[3];
    kf.lookAt[0] = v[4]; kf.lookAt[1] = v[5]; kf.lookAt[2] = v[6];
    kf.fovYDegrees = v[7];
    return true;
}

bool allFinite(const TrackKeyframe& kf) {
    if (!std::isfinite(kf.timeSeconds) || !std::isfinite(kf.fovYDegrees)) return false;
    for (int j = 0; j < 3; ++j) {
        if (!std::isfinite(kf.eye[j]) || !std::isfinite(kf.lookAt[j])) return false;
    }
    return true;
}

} // namespace

bool CinematicCamera::loadTrackFromMemory(const TrackKeyframe* frames, int count) {
    if (!frames || count <= 0) return false;
    // 严格递增 + 有限校验（不做隐藏排序）
    for (int i = 0; i < count; ++i) {
        if (!allFinite(frames[i])) return false;
        if (i > 0 && !(frames[i].timeSeconds > frames[i - 1].timeSeconds)) return false;
    }
    if (count > 64) return false;                // keys_ 容量上限（固定容量，无运行时分配）
    const float t0 = frames[0].timeSeconds;
    for (int i = 0; i < count; ++i) {
        keys_[i] = frames[i];
        keys_[i].timeSeconds = frames[i].timeSeconds - t0;   // 归一时间轴，一次固化
    }
    count_ = count;
    duration_ = keys_[count - 1].timeSeconds;
    return true;
}

bool CinematicCamera::loadTrack(const char* tsvPath) {
    if (!tsvPath) return false;
    std::FILE* fp = std::fopen(tsvPath, "r");
    if (!fp) return false;
    char line[512];
    std::vector<TrackKeyframe> frames;
    bool sawData = false;
    while (std::fgets(line, sizeof(line), fp)) {
        // 跳过空行 / '#' 注释 / "t\t..." 表头行
        const char* q = line;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q == '\0' || *q == '\n' || *q == '\r' || *q == '#') continue;
        if (q[0] == 't' && (q[1] == '\t' || q[1] == ' ')) continue;
        TrackKeyframe kf;
        if (!parseTsvLine(q, kf)) { std::fclose(fp); return false; }
        frames.push_back(kf);
        sawData = true;
    }
    std::fclose(fp);
    if (!sawData) return false;
    return loadTrackFromMemory(frames.data(), static_cast<int>(frames.size()));
}

void CinematicCamera::play() {
    if (state_ == PlayState::Stopped) progress_ = 0.0f;
    state_ = PlayState::Playing;
}

void CinematicCamera::pause() {
    if (state_ == PlayState::Playing) state_ = PlayState::Paused;
}

void CinematicCamera::stop() {
    state_ = PlayState::Stopped;
    progress_ = 0.0f;
}

void CinematicCamera::writePose(float t, CameraRig& out) const {
    const int n = count_;
    if (t <= keys_[0].timeSeconds) {             // 含 t==0 精确分支
        out.eye[0] = keys_[0].eye[0]; out.eye[1] = keys_[0].eye[1]; out.eye[2] = keys_[0].eye[2];
        out.lookAt[0] = keys_[0].lookAt[0]; out.lookAt[1] = keys_[0].lookAt[1]; out.lookAt[2] = keys_[0].lookAt[2];
        out.fovYDegrees = keys_[0].fovYDegrees;
        return;
    }
    if (t >= duration_) {                        // 含 t==duration 精确分支
        out.eye[0] = keys_[n - 1].eye[0]; out.eye[1] = keys_[n - 1].eye[1]; out.eye[2] = keys_[n - 1].eye[2];
        out.lookAt[0] = keys_[n - 1].lookAt[0]; out.lookAt[1] = keys_[n - 1].lookAt[1]; out.lookAt[2] = keys_[n - 1].lookAt[2];
        out.fovYDegrees = keys_[n - 1].fovYDegrees;
        return;
    }
    int i = 0;
    while (i + 2 < n && t >= keys_[i + 1].timeSeconds) ++i;   // keys_[i].t <= t < keys_[i+1].t
    const float span = keys_[i + 1].timeSeconds - keys_[i].timeSeconds;
    const float u = (t - keys_[i].timeSeconds) / span;
    const float s = (interp_ == InterpMode::SmoothStep) ? u * u * (3.0f - 2.0f * u) : u;
    for (int j = 0; j < 3; ++j) {
        out.eye[j]    = keys_[i].eye[j]    + (keys_[i + 1].eye[j]    - keys_[i].eye[j])    * s;
        out.lookAt[j] = keys_[i].lookAt[j] + (keys_[i + 1].lookAt[j] - keys_[i].lookAt[j]) * s;
    }
    out.fovYDegrees = keys_[i].fovYDegrees +
                      (keys_[i + 1].fovYDegrees - keys_[i].fovYDegrees) * s;
}

bool CinematicCamera::sample(float dt, CameraRig& out) {
    if (state_ != PlayState::Playing) return false;
    progress_ += (dt > 0.0f ? dt : 0.0f) * speed_;

    if (count_ == 0) return false;               // 桩语义保留：无轨迹推进进度即可

    if (progress_ >= duration_) {
        if (loop_) {
            progress_ = std::fmod(progress_, duration_);   // 整圈精确回 0
            writePose(progress_, out);
            return true;
        }
        // 非 loop 播完：当帧写终帧位姿后自动收尾（下一帧 sample=false）
        writePose(duration_, out);
        state_ = PlayState::Stopped;
        progress_ = 0.0f;
        return true;
    }
    writePose(progress_, out);
    return true;
}

} // namespace nl::core
