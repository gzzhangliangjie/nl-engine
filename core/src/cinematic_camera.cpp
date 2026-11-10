// P8 M-CAM1a：CinematicCamera 桩实现（播放状态机真实现；轨迹面 M-CAM3a 落）。

#include "nl/core/cinematic_camera.hpp"

namespace nl::core {

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

bool CinematicCamera::sample(float dt, CameraRig& /*out*/) {
    if (state_ != PlayState::Playing) return false;
    // 无轨迹桩：状态推进语义保持（进度走 speed 倍速），位姿面恒 false。
    // M-CAM3a 在此落插值 + wrap + 写 out。
    progress_ += (dt > 0.0f ? dt : 0.0f) * speed_;
    return false;
}

} // namespace nl::core
