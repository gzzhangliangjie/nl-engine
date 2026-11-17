#pragma once

// P8 M-CAM1a：相机接线封装层（track camera；无头可测，viewer 胶水等 drive M-D3）。
//
// 2026-11-10 用户拍板「封装先行，函数空在那里也行，这都是基础设施得先有」：
// CAM1~CAM3 卡 M-D3 的只是 viewer/main.cpp 胶水（glfw 轮询 → InputState →
// ActionMap → 相机 → 投影矩阵替换），本层把这条链里**不依赖窗口/图形**的部分
// 先落库并测完——M-D3 之后 viewer 侧只剩消费本层的几行调用。
//
// 内容三件：
//   1. SpectatorActions + buildSpectatorActionMap：自由飞行键位组（新 SetId，
//      driving set 零改动——计划 §输入面口径）；look / camToggle 复用 driving
//      set 既有动作（鼠标增量轴 + C 边沿，两模式共用同一语义）。
//   2. readFreeInputs：ActionMap → FreeCamera 轴口径适配（归一化轴 + 像素 look
//      + boost）；弧度换算单点在 CameraDirector::kLookRadiansPerPixel。
//   3. CameraDirector：Chase↔Free 模式管理（camToggle 边沿互切 + 切换帧位姿
//      连续种子），吃 ChassisState 车体回读（M-D2 落真实来源）。
//
// 车体回读：M-CAM0 的 ChaseCamera::update(dt, pos, yaw, vel) 已是终态签名，
// 本层只定 ChassisState 规范结构；M-D2/M-D3 的 phys 回读实现该结构的填充方。
// OrbitCamera 不入 director（viewer 既有路径零改动口径）；Cinematic 见
// cinematic_camera.hpp（桩，M-CAM3a 落实现）。
//
// 确定性契约：本层纯 CPU、零 wall-clock 零随机；固定输入序列逐位可复现
//（cam-tools selftest director-f 节锚定）。

#include "nl/core/action_map.hpp"
#include "nl/core/chase_camera.hpp"
#include "nl/core/free_camera.hpp"

namespace nl::core {

// ---- spectator 动作组（出厂激活，同 buildDrivingActionMap 口径；游戏壳按模式切）----
struct SpectatorActions {
    ActionMap::SetId    spectatorSet = ActionMap::kInvalidSet;
    ActionMap::ActionId moveForward  = ActionMap::kInvalidAction; // Float：W=+1 / S=-1
    ActionMap::ActionId moveRight    = ActionMap::kInvalidAction; // Float：D=+1 / A=-1
    ActionMap::ActionId moveUp       = ActionMap::kInvalidAction; // Float：Space=+1 / LCtrl=-1
    ActionMap::ActionId boost        = ActionMap::kInvalidAction; // Bool：LeftShift
};

SpectatorActions buildSpectatorActionMap(ActionMap& map);

// ---- M-D2 车体回读规范形态（drive 轨负责填充；本里程碑只定结构）----
struct ChassisState {
    float pos[3]     = {0.0f, 0.0f, 0.0f};
    float yawDegrees = 0.0f;
    float vel[3]     = {0.0f, 0.0f, 0.0f};
    bool  valid      = false;   // false = 本帧无车体数据（M-D2 前 / 无训练场）
};

// ---- Free 相机输入适配（ActionMap 冻结值 → FreeDrive）----
struct FreeDrive {
    float move[3]   = {0.0f, 0.0f, 0.0f};  // [右, 上, 前]（FreeCamera::update 口径）
    float lookPx[2] = {0.0f, 0.0f};        // 鼠标像素增量（弧度换算在 director 单点）
    bool  boost     = false;
};

// look 读 driving set 的 Vec2 增量动作——viewer 两模式都需要鼠标 look，driving set
// 保持 active；若场景要关 driving set，调用方自行填 FreeDrive::lookPx。
FreeDrive readFreeInputs(const ActionMap& map, const SpectatorActions& spec,
                         const DrivingActions& driving);

// ---- 每帧输入包（viewer 收集好塞进来；core 不碰窗口/图形）----
struct DirectorInputs {
    bool  camToggleEdge = false;            // C justPressed（driving set）
    float lookPx[2]     = {0.0f, 0.0f};     // 鼠标像素增量（Chase 直喂；Free 内部转弧度）
    const ChassisState* chassis = nullptr;  // M-D2 回读；nullptr = Chase 模式保持上一帧位姿
    FreeDrive free{};                       // readFreeInputs 产出（或手工填）
};

// ---- CameraDirector：Chase↔Free 模式管理 + 切换位姿连续 ----
class CameraDirector {
public:
    enum class Mode { Chase, Free };

    CameraDirector() = default;

    void reset();
    void setAspect(float aspect);            // 两台相机同透传
    void setFovYDegrees(float fovYDegrees);
    void setFarZ(float farZ);
    void setProjection(CameraRig::Projection p);
    void setOrthoHeight(float height);

    Mode mode() const { return mode_; }
    void setMode(Mode m);                    // 直接切（位姿连续种子，与边沿切换同口径）

    // camToggleEdge=true 时 Chase↔Free 互切；切换帧 = 重种子 + 冻结（位姿逐位
    // 连续，双向同口径；本帧输入下帧生效）。Chase 模式：chassis 非 null 且 valid
    // 才推进 chase_（无车体数据 = 保持位姿）。Free 模式：lookPx 乘
    // kLookRadiansPerPixel 转弧度喂 free_。
    void update(float dt, const DirectorInputs& in);

    // ---- 只读输出（按当前模式分派，同 OrbitCamera 口径）----
    Matrix4 view() const;
    Matrix4 viewProjection() const;
    float   nearPlane() const;   // M-DEPTH0：分派当前模式相机的真实 near（fp.nearPlane → dag 屏幕误差投影）
    void eyePosition(float out[3]) const;

    // 参数实调口（console cvar / 测试直调；viewer 不直接持有两台相机）
    ChaseCamera& chase() { return chase_; }
    FreeCamera&  free()  { return free_; }

    static constexpr float kLookRadiansPerPixel = 0.005f;  // Free look 换算单点

private:
    void seedFreeFromChase();   // Chase→Free：free 位姿 = chase 当前 eye + 视线方向
    void seedChaseFromFree();   // Free→Chase：chase rig eye 直设（下一帧起恢复阻尼）

    Mode        mode_ = Mode::Chase;
    ChaseCamera chase_;
    FreeCamera  free_;
};

} // namespace nl::core
