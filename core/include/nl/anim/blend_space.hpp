#pragma once

// M-A4：1D Blend Space（按参数在多个 clip 间插值， locomotion 速度轴起步）。
//
// 口径来源（ACTION-PLAN §2.2/§2.4 拍板表）：
//   - UE UBlendSpace（BlendSpace.h:561/954）：资产只存 sample 点集，1D 运行时
//     纯「找段 + 线性插权重」（2D 才需要预三角剖分 + 重心坐标，后置）；段端点
//     归一化坐标是编辑期细节，逻辑层直接用真实参数值。
//   - 段外参数 clamp 到最近 sample（与 M-A1 域外 clamp 同语义）。
//   - 单 sample 退化 = 常量 clip（合法，validate 放行）。
// 简化声明：不做 UE 的运行时权重时间平滑（TargetWeightInterpolationSpeedPerSec
// 输入滤波）——那是手感层，蒙皮接入期再议；本层输出即瞬时权重混合。
// 纪律：纯 CPU 确定性；零图形 API；混合复用 M-A2 blendPoses（等长骨架）。

#include "nl/anim/anim.hpp"

#include <string>
#include <vector>

namespace nl::anim {

struct BlendSample {
    float param = 0.0f;      // 轴参数（如速度 m/s）
    AnimClip clip;
};

struct BlendSpace1DDesc {
    std::vector<BlendSample> samples;   // param 严格递增
};

// 校验：空串合法；否则第一处错误（ASCII）。规则：
// samples 非空、param 有限且严格递增、clip 全过 validateClip、各 sample
// 骨架尺寸一致（混合前提，同 M-A2）。
std::string validateBlendSpace1D(const BlendSpace1DDesc& desc);

class BlendSpace1D {
public:
    bool reset(const BlendSpace1DDesc& desc);

    // 采样：param clamp 到轴域，找段线性插权重，两 clip 各自按 time 采样后混合。
    // 单 sample 直接返回该 clip。失败（未 reset/骨架不一致）返回 false。
    bool evaluate(float param, float time, std::vector<BonePose>& out) const;

    // 观测（测试/调试）：param 所在段 [i0,i1] 与权重 w（w=0 -> sample[i0]，
    // w=1 -> sample[i1]；clamp 后落在端点 sample 上时 i0==i1、w=0）。
    void segmentWeights(float param, std::size_t& i0, std::size_t& i1, float& w) const;

private:
    BlendSpace1DDesc desc_;
    bool valid_ = false;
};

} // namespace nl::anim
