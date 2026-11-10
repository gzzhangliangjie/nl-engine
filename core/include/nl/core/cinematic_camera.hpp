#pragma once

// P8 M-CAM3a：CinematicCamera 轨迹相机（M-CAM1a 桩升真实现）。
//
// 轨迹面（P8-CAMERA-PLAN §M-CAM3，锚口径：端点逐位 / 插值逐位 / wrap 连续 /
// pause 倍速时序）：
//   - TSV keyframe 加载：列规范 t  eye_x/y/z  look_x/y/z  fovY（TAB 分隔，
//     '#' 注释行与 "t\t..." 表头行跳过；任一行列数错 = 整体加载失败，不出半成品）；
//   - 加载即归一时间轴：t' = t - t[0]（一次浮点减法固化），之后 progress 直接就是
//     轨迹时间——端点 t==duration 与 t==0 走精确分支，逐位等于关键帧值；
//   - 插值 whole-track 双模式（UE InterpCurve CIM_Linear 缺省 / bevy_curve
//     EaseFunction::Linear|SmoothStep 词汇，抄思路不抄代码）：
//       Linear     u = (t-ti)/(ti+1-ti)
//       SmoothStep s = u*u*(3-2u)
//     对 eye/lookAt/fovY 三通道同式同序，逐位确定（同 dt 序列跨机一致）；
//   - 播放语义对齐 UE MovieScene PlaybackSettings：speed_ = PlayRate（缺省 1，
//     只接受 >0），loop_ = Loop 循环（wrap 用 fmod，整圈落点精确回 t0）；
//     非 loop 播完：当帧写终帧位姿后自动回 Stopped、进度归零（下一帧 sample=false）。
//
// M-CAM1a 桩语义保留：无轨迹时 Playing 也推进进度（接线方可先行自测状态机），
// sample 恒 false 且 out 不动；有轨迹后 sample 按插值写 out 并返回 true。

#include "nl/core/camera_rig.hpp"

namespace nl::core {

// 轨迹关键帧（TSV 列规范：t  eye_x/y/z  look_x/y/z  fovY）
struct TrackKeyframe {
    float timeSeconds = 0.0f;
    float eye[3]      = {0.0f, 0.0f, 0.0f};
    float lookAt[3]   = {0.0f, 0.0f, -1.0f};
    float fovYDegrees = 45.0f;
};

class CinematicCamera {
public:
    enum class PlayState { Stopped, Playing, Paused };
    enum class InterpMode { Linear, SmoothStep };

    CinematicCamera() = default;

    // ---- 轨迹加载（校验失败显式 false，不静默假成功）----
    // 要求时间严格递增、数值全部有限；不做隐藏排序（调用方排序，确定性口径）。
    bool loadTrack(const char* tsvPath);
    bool loadTrackFromMemory(const TrackKeyframe* frames, int count);
    int  keyframeCount() const { return count_; }

    // 归一后时间轴总长（= t[last] - t[0]，加载成功后 > 0）
    float duration() const { return duration_; }

    // ---- 插值模式（whole-track；缺省 Linear 同 UE CIM_Linear 缺省）----
    void       setInterpMode(InterpMode m) { interp_ = m; }
    InterpMode interpMode() const          { return interp_; }

    // ---- 播放控制（M-CAM1a 状态机语义不变）----
    void play();                              // Stopped/Paused → Playing（Stopped 归零进度）
    void pause();                             // Playing → Paused（其余 no-op）
    void stop();                              // 任意 → Stopped（进度归零）
    void setLoop(bool on)        { loop_ = on; }
    void setSpeed(float speed)   { if (speed > 0.0f) speed_ = speed; }

    PlayState state() const           { return state_; }
    bool      loop() const            { return loop_; }
    float     speed() const           { return speed_; }
    float     progressSeconds() const { return progress_; }

    // 帧推进：有轨迹时按插值写 out 并返回 true；无轨迹 no-op 恒 false（out 不动）。
    // 非 loop 播完的当帧仍写终帧位姿（返回 true），随后自动 Stopped + 进度归零。
    bool sample(float dt, CameraRig& out);

private:
    PlayState  state_    = PlayState::Stopped;
    InterpMode interp_   = InterpMode::Linear;
    bool       loop_     = false;
    float      speed_    = 1.0f;
    float      progress_ = 0.0f;

    // 归一时间轴关键帧（time 严格递增、首帧恒 0；加载时一次固化）
    TrackKeyframe keys_[64];
    int           count_    = 0;
    float         duration_ = 0.0f;

    void writePose(float t, CameraRig& out) const;
};

} // namespace nl::core
