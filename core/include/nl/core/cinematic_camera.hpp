#pragma once

// P8 M-CAM1a：CinematicCamera 桩（轨迹相机 API 先立，实现 M-CAM3a 落）。
//
// 2026-11-10 用户拍板「封装先行，函数空在那里也行」：游戏壳 / viewer 可以先对
// 本 API 写接线（M-CAM3b 的消费面），轨迹加载与采样函数当前显式返回失败——
// 不静默假成功，接线错误会在锚上炸出来（M-I0 负向锚口径）。
//
// M-CAM3a 待落（P8-CAMERA-PLAN §M-CAM3）：
//   - loadTrack / loadTrackFromMemory：TSV keyframe 加载（逐位确定解析）；
//   - sample：linear / smoothstep 插值 + loop wrap + 倍速 + play/pause 时序。
// 播放状态机（play/pause/stop/progress）本桩已真实现——无轨迹时 sample 恒
// false，状态推进语义与终版一致，接线方可先行自测。

#include "nl/core/camera_rig.hpp"

namespace nl::core {

// 轨迹关键帧（M-CAM3a TSV 列规范：t  eye_x/y/z  look_x/y/z  fovY）
struct TrackKeyframe {
    float timeSeconds = 0.0f;
    float eye[3]      = {0.0f, 0.0f, 0.0f};
    float lookAt[3]   = {0.0f, 0.0f, -1.0f};
    float fovYDegrees = 45.0f;
};

class CinematicCamera {
public:
    enum class PlayState { Stopped, Playing, Paused };

    CinematicCamera() = default;

    // ---- 轨迹加载（M-CAM3a 实现；当前恒 false = 无轨迹）----
    bool loadTrack(const char* /*tsvPath*/) { return false; }
    bool loadTrackFromMemory(const TrackKeyframe* /*frames*/, int /*count*/) { return false; }
    int  keyframeCount() const { return 0; }   // 桩恒 0

    // ---- 播放控制（状态机真实现；sample 无轨迹恒 false）----
    void play();                              // Stopped/Paused → Playing（Stopped 归零进度）
    void pause();                             // Playing → Paused（其余 no-op）
    void stop();                              // 任意 → Stopped（进度归零）
    void setLoop(bool on)        { loop_ = on; }
    void setSpeed(float speed)   { if (speed > 0.0f) speed_ = speed; }

    PlayState state() const           { return state_; }
    bool      loop() const            { return loop_; }
    float     speed() const           { return speed_; }
    float     progressSeconds() const { return progress_; }

    // 帧推进：有轨迹时按插值写 out 并返回 true（M-CAM3a）；无轨迹 no-op 恒 false
    //（out 不动——调用方不该拿到半成品位姿）。
    bool sample(float dt, CameraRig& out);

private:
    PlayState state_    = PlayState::Stopped;
    bool      loop_     = false;
    float     speed_    = 1.0f;
    float     progress_ = 0.0f;
};

} // namespace nl::core
