// P8 M-CAM1a：相机接线封装层实现（camera_director.hpp 口径，纯 CPU 零图形 API）。
// GLFW 键码常量经 input.hpp 先例（core 内允许出现，不泄漏进公开 API 类型）。

#include "nl/core/camera_director.hpp"

#include "nl/core/input.hpp"

#include <cmath>

namespace nl::core {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kRad2Deg = 180.0f / kPi;

} // namespace

SpectatorActions buildSpectatorActionMap(ActionMap& map) {
    SpectatorActions s{};
    s.spectatorSet = map.defineSet("spectator");

    s.moveForward = map.define("move_forward", ActionMap::Type::Float);
    map.bindKey(s.moveForward, GLFW_KEY_W, +1.0f);
    map.bindKey(s.moveForward, GLFW_KEY_S, -1.0f);
    map.setMembership(s.moveForward, s.spectatorSet);

    s.moveRight = map.define("move_right", ActionMap::Type::Float);
    map.bindKey(s.moveRight, GLFW_KEY_A, -1.0f);
    map.bindKey(s.moveRight, GLFW_KEY_D, +1.0f);
    map.setMembership(s.moveRight, s.spectatorSet);

    s.moveUp = map.define("move_up", ActionMap::Type::Float);
    map.bindKey(s.moveUp, GLFW_KEY_SPACE, +1.0f);
    map.bindKey(s.moveUp, GLFW_KEY_LEFT_CONTROL, -1.0f);
    map.setMembership(s.moveUp, s.spectatorSet);

    s.boost = map.define("boost", ActionMap::Type::Bool);
    map.bindKey(s.boost, GLFW_KEY_LEFT_SHIFT);
    map.setMembership(s.boost, s.spectatorSet);

    map.setActive(s.spectatorSet, true);   // 出厂激活（driving 同款；游戏壳按模式切）
    return s;
}

FreeDrive readFreeInputs(const ActionMap& map, const SpectatorActions& spec,
                         const DrivingActions& driving) {
    FreeDrive out;
    out.move[0] = map.floatValue(spec.moveRight);
    out.move[1] = map.floatValue(spec.moveUp);
    out.move[2] = map.floatValue(spec.moveForward);
    out.boost   = map.boolValue(spec.boost);
    const Vec2f look = map.vec2Delta(driving.look);   // additive 增量（endFrame 清）
    out.lookPx[0] = look.x;
    out.lookPx[1] = look.y;
    return out;
}

// ---- CameraDirector ----

void CameraDirector::reset() {
    chase_.reset();
    free_.reset();
    mode_ = Mode::Chase;
}

void CameraDirector::setAspect(float aspect) {
    chase_.setAspect(aspect);
    free_.setAspect(aspect);
}

void CameraDirector::setFovYDegrees(float fovYDegrees) {
    chase_.setFovYDegrees(fovYDegrees);
    free_.setFovYDegrees(fovYDegrees);
}

void CameraDirector::setFarZ(float farZ) {
    chase_.setFarZ(farZ);
    free_.setFarZ(farZ);
}

void CameraDirector::setProjection(CameraRig::Projection p) {
    chase_.setProjection(p);
    free_.setProjection(p);
}

void CameraDirector::setOrthoHeight(float height) {
    chase_.setOrthoHeight(height);
    free_.setOrthoHeight(height);
}

void CameraDirector::seedFreeFromChase() {
    float eye[3];
    chase_.eyePosition(eye);
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    chase_.lookDirection(yawDeg, pitchDeg);
    free_.setPose(eye, yawDeg, pitchDeg);
}

void CameraDirector::seedChaseFromFree() {
    float eye[3];
    free_.eyePosition(eye);
    chase_.snapEyeTo(eye);
}

void CameraDirector::setMode(Mode m) {
    if (m == mode_) return;
    if (m == Mode::Free) seedFreeFromChase();
    else                 seedChaseFromFree();
    mode_ = m;
}

void CameraDirector::update(float dt, const DirectorInputs& in) {
    if (in.camToggleEdge) {
        setMode(mode_ == Mode::Chase ? Mode::Free : Mode::Chase);
        // 切换帧 = 重种子 + 冻结（位姿逐位连续，双向同口径）；本帧输入下帧生效。
        return;
    }
    if (mode_ == Mode::Chase) {
        chase_.look(in.lookPx[0], in.lookPx[1]);
        if (in.chassis && in.chassis->valid) {
            chase_.update(dt, in.chassis->pos, in.chassis->yawDegrees,
                          in.chassis->vel);
        }
        // 无车体数据：保持上一帧位姿（chase_.update 不推进，look 已累计）
    } else {
        const float lk[2] = {in.free.lookPx[0] * kLookRadiansPerPixel,
                             in.free.lookPx[1] * kLookRadiansPerPixel};
        free_.update(dt, in.free.move, lk, in.free.boost);
    }
}

Matrix4 CameraDirector::view() const {
    return mode_ == Mode::Chase ? chase_.view() : free_.view();
}

Matrix4 CameraDirector::viewProjection() const {
    return mode_ == Mode::Chase ? chase_.viewProjection() : free_.viewProjection();
}

float CameraDirector::nearPlane() const {
    // M-DEPTH0：dag 屏幕误差投影（g_dagProj.z）吃真实 near，远景放大后必须同步。
    return mode_ == Mode::Chase ? chase_.nearZ() : free_.nearZ();
}

void CameraDirector::eyePosition(float out[3]) const {
    mode_ == Mode::Chase ? chase_.eyePosition(out) : free_.eyePosition(out);
}

} // namespace nl::core
