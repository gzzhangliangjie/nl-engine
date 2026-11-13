#pragma once

// P8 M-CAM1b/2b/3b + M-CAM4：viewer 相机模式调度封装（轨道 / 自由 / 跟车 / 轨迹 + 平滑 / 抖动）。
//
// 设计要点（为何这样落地）：
//   - 对外暴露与 OrbitCamera 同款方法名（viewProjection / view / eyePosition / model /
//     orbit / zoom / reset / setAspect / setOrbit / setFovYDegrees / setDistance /
//     yawDegrees / pitchDegrees / distance / projectionYScale / nearPlane）。
//     因此两个渲染循环（runNormal / runScene）只需把 `OrbitCamera camera;` 换成
//     `nl::viewer::ViewerCamera camera;`，其余调用原样保留——Orbit 模式逐位零回归。
//   - 实际相机：OrbitCamera（orbit 模式源 + 场景模型/投影兜底）+ CameraDirector（free/chase）
//     + CinematicCamera（track）+ CameraSmoother / CameraTrauma（M-CAM4，默认关）。
//   - 注意：本封装数据成员名为 `orbitCam`（避免与同名方法 `orbit(dx,dy)` 冲突）。
//
// 关键不变量（切模式不跳变）：
//   - gameplay 家族（Free/Chase）下，本封装的 mode 始终镜像 director.mode()：T 键
//     camToggleEdge → director 内部互切 → update() 每帧把 mode 同步成导演实际子模式。
//     因此 yawDegrees/pitchDegrees/distance/HUD 永远读对子相机，不会错读。
//   - setMode 进 gameplay 家族前，用当前连续位姿（orbit 当前或上帧渲染位姿）给目标
//     子相机下种子（Free.setPose / Chase.snapEyeTo+setPitch），切过去不瞬移。
//
// 用法（viewer 每帧）：
//   1) 设置输入字段：camToggleEdge（T 键边沿）、lookPx（鼠标增量）、chassis（Chase 跟车源）、
//      freeInput（readFreeInputs 产出）；
//   2) camera.update(dt)；
//   3) 其余 renderer 调用照旧读 viewProjection/view/eyePosition。

#include "nl/core/camera.hpp"
#include "nl/core/camera_rig.hpp"
#include "nl/core/chase_camera.hpp"
#include "nl/core/free_camera.hpp"
#include "nl/core/cinematic_camera.hpp"
#include "nl/core/camera_director.hpp"
#include "nl/core/action_map.hpp"
#include "nl/core/camera_smoother.hpp"

#include <cmath>

namespace nl::viewer {

enum class CameraMode { Orbit, Free, Chase, Track };

struct ViewerCamera {
    CameraMode mode = CameraMode::Orbit;

    nl::core::OrbitCamera     orbitCam;     // orbit 模式源 + 场景模型/投影兜底（成员名避开方法 orbit()）
    nl::core::CameraDirector  director;     // free / chase 模式（内部自管 Chase↔Free）
    nl::core::CinematicCamera cine;         // track 模式
    nl::core::CameraRig       cineRig;      // cine.sample 写入
    nl::core::CameraRig       outRig;       // 平滑 / 抖动后最终姿态（非 orbit 模式用）
    nl::core::CameraSmoother  smoother;      // M-CAM4（默认关）
    nl::core::CameraTrauma    trauma;       // M-CAM4（确定性抖动）

    // ---- 每帧输入（update 前由 viewer 填好）----
    bool camToggleEdge = false;                       // T 键边沿 → Chase↔Free 互切
    float lookPx[2]    = {0.0f, 0.0f};              // 鼠标像素增量（Chase look / Free 内部转弧度）
    const nl::core::ChassisState* chassis = nullptr;  // M-D2 车体回读（Chase 跟车源）
    nl::core::FreeDrive freeInput{};                  // readFreeInputs 产出（Free 移动）

    float fovY_   = 45.0f;
    float aspect_ = 16.0f / 9.0f;

    static constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;
    static constexpr float kRad2Deg = 180.0f / 3.14159265358979f;

    // ---- 初始化：把 orbit 起手位姿喂给 director/cine/outRig，切到玩法相机不跳变 ----
    void seedFromOrbit() {
        float e[3]; orbitCam.eyePosition(e);
        director.chase().snapEyeTo(e);
        director.free().setPose(e, orbitCam.yawDegrees(), orbitCam.pitchDegrees());
        cineRig.fovYDegrees = fovY_;  cineRig.aspect = aspect_;
        outRig.fovYDegrees  = fovY_;  outRig.aspect  = aspect_;
        outRig.eye[0] = e[0]; outRig.eye[1] = e[1]; outRig.eye[2] = e[2];
        forwardFromView(orbitCam.view(), outRig.lookAt);
        for (int i = 0; i < 3; ++i) outRig.lookAt[i] += e[i];
        smoother.reset(outRig.eye, outRig.lookAt);
    }

    void setMode(CameraMode m) {
        if (m == mode) return;
        float e[3], l[3];
        currentEye(e); currentLook(l);   // 切换前捕获当前连续位姿，给目标相机下种子
        if (m == CameraMode::Orbit) { mode = m; smoother.reset(e, l); trauma.trauma = 0.0f; return; }
        if (m == CameraMode::Free) {
            director.setMode(nl::core::CameraDirector::Mode::Free);
            director.free().setPose(e, yawOf(l, e), pitchOf(l, e));
        } else if (m == CameraMode::Chase) {
            director.setMode(nl::core::CameraDirector::Mode::Chase);
            director.chase().snapEyeTo(e);
            director.chase().setPitchDegrees(pitchOf(l, e));
        } else if (m == CameraMode::Track) {
            if (cine.keyframeCount() > 0) cine.play();
        }
        mode = m;
        smoother.reset(e, l);
        trauma.trauma = 0.0f;
    }
    CameraMode modeOf() const { return mode; }

    bool loadTrack(const char* path) {
        const bool ok = cine.loadTrack(path);
        if (ok && mode == CameraMode::Track) cine.play();
        return ok;
    }

    // T 键程序化切换（与 camToggleEdge→update 内部互切等价，用于 console 命令）。
    void toggleGameplay() {
        setMode(director.mode() == nl::core::CameraDirector::Mode::Free
                    ? CameraMode::Chase : CameraMode::Free);
    }

    // ---- 交互（仅 Orbit 模式生效；其余模式由 update 接管）----
    void orbit(float dx, float dy) { if (mode == CameraMode::Orbit) orbitCam.orbit(dx, dy); }
    void zoom(float s)             { if (mode == CameraMode::Orbit) orbitCam.zoom(s); }
    void reset() {
        if (mode == CameraMode::Orbit) orbitCam.reset();
        director.reset(); cine.stop(); trauma.trauma = 0.0f;
        float e[3], l[3]; currentEye(e); currentLook(l); smoother.reset(e, l);
    }
    void setAspect(float a) {
        aspect_ = a; orbitCam.setAspect(a); director.setAspect(a);
        cineRig.aspect = a; outRig.aspect = a;
    }
    void setOrbit(float yaw, float pitch, float dist) {
        orbitCam.setOrbit(yaw, pitch, dist);
        seedFromOrbit();
    }
    void setFovYDegrees(float f) {
        fovY_ = f; orbitCam.setFovYDegrees(f); director.setFovYDegrees(f);
        cineRig.fovYDegrees = f; outRig.fovYDegrees = f;
    }
    void setDistance(float d) { if (mode == CameraMode::Orbit) orbitCam.setDistance(d); }
    float fovYDegrees() const { return fovY_; }

    // ---- 每帧推进 ----
    void update(float dt) {
        if (mode == CameraMode::Orbit) return;   // orbit 由交互直接更新
        float rawEye[3], rawLook[3];
        if (mode == CameraMode::Track) {
            cine.sample(dt, cineRig);
            fovY_ = cineRig.fovYDegrees;          // 轨迹自带 fov 生效
            cineRig.eyePosition(rawEye);
            forwardFromView(cineRig.view(), rawLook);
        } else {
            nl::core::DirectorInputs in;
            in.camToggleEdge = camToggleEdge;
            in.lookPx[0] = lookPx[0]; in.lookPx[1] = lookPx[1];
            in.chassis = chassis; in.free = freeInput;
            director.update(dt, in);
            // 同步：gameplay 家族 mode 镜像导演实际子模式（T 键互切后不读错相机）
            mode = (director.mode() == nl::core::CameraDirector::Mode::Free)
                       ? CameraMode::Free : CameraMode::Chase;
            director.eyePosition(rawEye);
            forwardFromView(director.view(), rawLook);
        }
        for (int i = 0; i < 3; ++i) rawLook[i] += rawEye[i];

        if (smoother.enabled) {
            smoother.setTarget(rawEye, rawLook);
            smoother.step(dt);
            smoother.smoothedEye(rawEye);
            smoother.smoothedLookAt(rawLook);
        }
        // 确定性 trauma 抖动（位置后处理偏移；不改阻尼态、不污染轨迹）
        if (trauma.trauma01() > 0.0f) {
            float y = 0.0f, p = 0.0f, r = 0.0f;
            trauma.step(dt); trauma.offsets(y, p, r);
            applyPositionalShake(rawEye, rawLook, y, p);
        } else {
            trauma.step(dt);
        }
        outRig.eye[0] = rawEye[0]; outRig.eye[1] = rawEye[1]; outRig.eye[2] = rawEye[2];
        outRig.lookAt[0] = rawLook[0]; outRig.lookAt[1] = rawLook[1]; outRig.lookAt[2] = rawLook[2];
        outRig.fovYDegrees = fovY_; outRig.aspect = aspect_;
    }

    // ---- 输出（同 OrbitCamera 口径）----
    nl::core::Matrix4 viewProjection() const {
        return mode == CameraMode::Orbit ? orbitCam.viewProjection() : outRig.viewProjection();
    }
    nl::core::Matrix4 view() const {
        return mode == CameraMode::Orbit ? orbitCam.view() : outRig.view();
    }
    void eyePosition(float out[3]) const {
        if (mode == CameraMode::Orbit) orbitCam.eyePosition(out);
        else outRig.eyePosition(out);
    }
    nl::core::Matrix4 model() const { return orbitCam.model(); }   // 场景模型 = 单位阵
    float projectionYScale() const { return 1.0f / std::tan(0.5f * fovY_ * kDeg2Rad); }
    float nearPlane() const { return 0.1f; }

    float yawDegrees() {
        if (mode == CameraMode::Orbit) return orbitCam.yawDegrees();
        if (mode == CameraMode::Track) {
            float f[3]; forwardFromView(outRig.view(), f);
            return std::atan2(f[0], f[2]) * kRad2Deg;
        }
        if (director.mode() == nl::core::CameraDirector::Mode::Free)
            return director.free().yawDegrees();
        float y = 0.0f, p = 0.0f; director.chase().lookDirection(y, p); return y;
    }
    float pitchDegrees() {
        if (mode == CameraMode::Orbit) return orbitCam.pitchDegrees();
        if (mode == CameraMode::Track) {
            float f[3]; forwardFromView(outRig.view(), f);
            float s = f[1] < -1.0f ? -1.0f : (f[1] > 1.0f ? 1.0f : f[1]);
            return std::asin(s) * kRad2Deg;
        }
        if (director.mode() == nl::core::CameraDirector::Mode::Free)
            return director.free().pitchDegrees();
        float y = 0.0f, p = 0.0f; director.chase().lookDirection(y, p); return p;
    }
    float distance() {
        if (mode == CameraMode::Orbit) return orbitCam.distance();
        if (mode == CameraMode::Chase) return director.chase().distance();
        float e[3]; eyePosition(e);
        return std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    }
    const char* modeName() const {
        switch (mode) {
            case CameraMode::Orbit: return "orbit";
            case CameraMode::Free:  return "free";
            case CameraMode::Chase: return "chase";
            case CameraMode::Track: return "track";
        }
        return "orbit";
    }

private:
    static void forwardFromView(const nl::core::Matrix4& v, float fwd[3]) {
        // 列主序 view 矩阵：camera backward（zaxis = -forward）在第三**行**，
        // 列主序存储下是 m[2] / m[6] / m[10]。
        // M-CAM5 勘误：原实现读 m[8..10]（那是第三**列** = (right.z, up.z,
        // backward.z)，不是 backward），导致 chase/track 的渲染视线错 45° 级——
        // lookAt 由 eye + 错误 forward 反推，画面主体偏离画面中心（Orbit 路径
        // 自带视图矩阵不经此函数，故 70 张既有基线零波及）。
        fwd[0] = -v.m[2]; fwd[1] = -v.m[6]; fwd[2] = -v.m[10];
    }
    static float yawOf(const float l[3], const float e[3]) {
        return std::atan2(l[0] - e[0], l[2] - e[2]) * kRad2Deg;
    }
    static float pitchOf(const float l[3], const float e[3]) {
        const float fy = l[1] - e[1];
        const float horiz = std::sqrt((l[0] - e[0]) * (l[0] - e[0]) + (l[2] - e[2]) * (l[2] - e[2]));
        return std::atan2(fy, horiz) * kRad2Deg;
    }
    void currentEye(float e[3]) const { eyePosition(e); }
    void currentLook(float l[3]) const {
        eyePosition(l);
        const nl::core::Matrix4& vm = (mode == CameraMode::Orbit) ? orbitCam.view() : outRig.view();
        float f[3]; forwardFromView(vm, f);
        for (int i = 0; i < 3; ++i) l[i] += f[i];
    }
    static void applyPositionalShake(const float eye[3], float look[3], float yawRad, float pitchRad) {
        // 沿相机 right / up 做小位移抖动（确定性，纯位置后处理）
        float f[3] = {look[0] - eye[0], look[1] - eye[1], look[2] - eye[2]};
        float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (len < 1e-6f) return;
        f[0] /= len; f[1] /= len; f[2] /= len;
        // right = cross(f, worldUp) = (-f.z, 0, f.x)
        float r[3] = {-f[2], 0.0f, f[0]};
        float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
        if (rl > 1e-6f) { r[0] /= rl; r[2] /= rl; }
        // up = cross(r, f)
        float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        const float k = 0.15f;   // 抖动位移尺度（世界单位）
        for (int i = 0; i < 3; ++i) look[i] += (r[i] * yawRad + u[i] * pitchRad) * k;
    }
};

} // namespace nl::viewer
