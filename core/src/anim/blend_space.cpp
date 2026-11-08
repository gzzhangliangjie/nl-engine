// M-A4：1D Blend Space 实现（口径见 blend_space.hpp 文件头）。

#include "nl/anim/blend_space.hpp"

#include <algorithm>
#include <cmath>

namespace nl::anim {

std::string validateBlendSpace1D(const BlendSpace1DDesc& desc) {
    if (desc.samples.empty()) return "blendspace1d: no samples";
    for (std::size_t i = 0; i < desc.samples.size(); ++i) {
        const BlendSample& s = desc.samples[i];
        if (!std::isfinite(s.param)) return "blendspace1d: non-finite param";
        const std::string clipError = validateClip(s.clip);
        if (!clipError.empty()) return "blendspace1d: invalid clip at sample " + std::to_string(i);
        if (i > 0 && !(s.param > desc.samples[i - 1].param))
            return "blendspace1d: params must be strictly increasing";
    }
    const std::size_t rigSize = desc.samples.front().clip.tracks.size();
    for (const BlendSample& s : desc.samples)
        if (s.clip.tracks.size() != rigSize)
            return "blendspace1d: rig size mismatch across samples";
    return {};
}

bool BlendSpace1D::reset(const BlendSpace1DDesc& desc) {
    if (!validateBlendSpace1D(desc).empty()) {
        valid_ = false;
        return false;
    }
    desc_ = desc;
    valid_ = true;
    return true;
}

void BlendSpace1D::segmentWeights(float param, std::size_t& i0, std::size_t& i1, float& w) const {
    const std::size_t last = desc_.samples.size() - 1;
    // clamp 到轴域（M-A1 同语义）
    if (param <= desc_.samples.front().param) {
        i0 = i1 = 0;
        w = 0.0f;
        return;
    }
    if (param >= desc_.samples.back().param) {
        i0 = i1 = last;
        w = 0.0f;
        return;
    }
    // upper_bound：第一个 param 值 > 输入的 sample -> 段 [i-1, i]
    const auto it = std::upper_bound(
        desc_.samples.begin(), desc_.samples.end(), param,
        [](float value, const BlendSample& s) { return value < s.param; });
    i1 = static_cast<std::size_t>(it - desc_.samples.begin());
    i0 = i1 - 1;
    const float p0 = desc_.samples[i0].param, p1 = desc_.samples[i1].param;
    w = (param - p0) / (p1 - p0);
}

bool BlendSpace1D::evaluate(float param, float time, std::vector<BonePose>& out) const {
    if (!valid_) return false;
    std::size_t i0 = 0, i1 = 0;
    float w = 0.0f;
    segmentWeights(param, i0, i1, w);
    if (i0 == i1)
        return sampleClip(desc_.samples[i0].clip, time, out);
    std::vector<BonePose> pa, pb;
    if (!sampleClip(desc_.samples[i0].clip, time, pa)) return false;
    if (!sampleClip(desc_.samples[i1].clip, time, pb)) return false;
    return blendPoses(pa, pb, w, out);
}

} // namespace nl::anim
