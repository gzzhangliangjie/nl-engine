#pragma once

// M-A1：动作片段（AnimClip）数据结构 + 曲线采样。
// 上层控制器（状态机 / 混合树，M-A3/M-A4）坐在本层之上；蒙皮（PARALLEL-TRACKS 2.3）
// 坐在本层之下（吃 sampleClip 产出的骨骼局部 pose）。
//
// 口径来源（三处对照，见 docs/action-system/ACTION-PLAN.md §2.4 拍板表）：
//   - Clip 按**骨骼名**组织通道轨道（bevy 按目标 / Carrot 骨骼名 map 的折中；
//     不做 bevy 的路径哈希 UUID——纯逻辑层先保简单，重定向后置）。
//   - 关键帧 = (time[s], value) 对，**时间单位秒**（UE/bevy 一致；UE 帧
//     率只是元数据）；域外 **clamp**（bevy sample_clamped 语义）。
//   - 插值：Linear（lerp / slerp+归一化）与 Step（gltf STEP：保持前一个
//     key）两种起步；Cubic 按需后置（gltf CubicSpline 罕用）。
//   - 循环：wrapTime 把 t 归一到 [0, duration)（bevy RepeatAnimation::Forever
//     的 modulo 语义；负 t 一次性回绕）。
// 纪律：core 零图形 API、零文件 IO（数据由调用方构造/将来由 glTF 导入器喂）；
//       采样是纯函数 = 确定性单测。

#include <nl/math/math.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nl::anim {

// 数学本体在 nl/math/math.hpp（2026-11-09 上移，docs/p8/P8-ECS-PLAN.md）。
// 定向注入（非 using namespace，防泄漏）：nl/anim 内 unqualified 使用即可。
using nl::math::Vec3;
using nl::math::Quat;
using nl::math::quatIdentity;

enum class Interp : std::uint8_t {
    Linear,   // lerp / slerp（最短路径 + 归一化）
    Step,     // 保持前一个 key（gltf STEP）
};

struct Vec3Track {
    Interp interp = Interp::Linear;
    std::vector<float> times;    // 严格递增，size == values.size，>=1
    std::vector<Vec3> values;
};

struct QuatTrack {
    Interp interp = Interp::Linear;
    std::vector<float> times;
    std::vector<Quat> values;
};

// 一根骨骼的通道集合。scale 轨道省略：rigger GLB 到场（蒙皮后挂）再补，
// glTF 节点 scale 绝大多数为常量 1（常量 = 空 Vec3Track 语义，见 sampleScale）。
struct BoneTrack {
    std::string bone;
    Vec3Track translation;
    QuatTrack rotation;
};

struct AnimClip {
    float duration = 0.0f;                  // 秒；必须 > 0
    std::vector<BoneTrack> tracks;          // 每骨骼一条；bone 名不得重复
};

// 单骨骼采样结果（局部空间 TR）。
struct BonePose {
    Vec3 translation{};
    Quat rotation = quatIdentity();
};

// 校验：返回空串 = 合法；否则返回第一处错误的英文描述（ASCII，给工具日志）。
// 硬规则：duration>0；bone 非空且不重复；非空轨道 times/values 等长且
// 严格递增、数值有限（NaN/Inf 拒绝——确定性单测的前置护栏）。
// 空通道（times 为空）= 常量语义，合法（与采样侧一致；一根骨骼可以只有
// rotation 通道，如 glTF 里无位移动画的节点）。
std::string validateClip(const AnimClip& clip);

// 循环归一：把任意 t 映射到 [0, duration)。duration<=0 原样返回（调用方先 validate）。
float wrapTime(float t, float duration);

// 单轨道采样：t 先 clamp 到轨道 domain（首末 key），再按 interp 插值。
// 空轨道 = 常量语义：Vec3 返回零值，Quat 返回 identity（调用方无需特判）。
Vec3 sampleVec3Track(const Vec3Track& track, float t);
Quat sampleQuatTrack(const QuatTrack& track, float t);

// 整片采样：out 按 clip.tracks 顺序输出每骨骼 pose（out.resize 由本函数负责）。
// t 先 wrapTime 到 [0, duration)，再逐轨道采样。返回 false = clip 未过 validate。
bool sampleClip(const AnimClip& clip, float t, std::vector<BonePose>& out);

// 按骨骼名找轨道下标；无则返回 SIZE_MAX（状态机按名取 pose 用）。
std::size_t findTrackIndex(const AnimClip& clip, const std::string& bone);

// ---- M-A2：混合（口径见 ACTION-PLAN §2.4 拍板表） --------------------------
//
// 线性混合：UE FTransform::Blend / bevy BlendInput 同式——平移/缩放 lerp、
// 旋转 slerp（最短路径）。w=0 -> a，w=1 -> b，域外 clamp。
// 加法混合：bevy blendable.rs 语义——平移 base + delta*w；
// 旋转 slerp(IDENTITY, delta, w) * base（相对旋转叠加，见 nl/math/math.hpp scaleQuat）。
// 混合要求两侧 pose 数组等长且骨骼顺序一致（同一骨架的两次采样天然满足；
// 跨骨架按名重排在状态机层做，本层不背包袱）。

BonePose lerpPose(const BonePose& a, const BonePose& b, float w);

// 逐骨骼线性混合；a/b 尺寸不一致返回 false（out 保持不变）。
bool blendPoses(const std::vector<BonePose>& a, const std::vector<BonePose>& b, float w,
                std::vector<BonePose>& out);

// 加法叠加：out = base ⊕ delta*w。
BonePose addPose(const BonePose& base, const BonePose& delta, float w);

// 便捷：两 clip 各自采样后混合。任一 clip 非法或骨架尺寸不一致返回 false。
bool blendClips(const AnimClip& a, float timeA,
                const AnimClip& b, float timeB, float w,
                std::vector<BonePose>& out);

} // namespace nl::anim
