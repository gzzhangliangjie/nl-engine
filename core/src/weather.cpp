// 天气参数层 · 天气状态层实现（WEA 轨道 M-WEA0，计划 docs/WEATHER-PLAN.md）。
// 形态/落点/参考口径见 weather.hpp 头注释；本文件只落实现与公式。
//
// 确定性纪律：纯 double、splitmix64 整数哈希、固定步长积分；无隐藏状态、
// 无全局量。任何常量改动 = 破坏已回放时间轴的逐字节契约（weather.hpp 命名空间
// weather_detail 内常量冻结）。

#include "nl/core/weather.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nl::core {

namespace {

using weather_detail::kBlendWindow;
using weather_detail::kDayLength;
using weather_detail::kDryRate;
using weather_detail::kEpochLength;
using weather_detail::kPersistProb;
using weather_detail::kWetRate;
using weather_detail::kWetStep;

// ---- splitmix64（纯整数，跨平台位一致）----
std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// [0,1) 均匀 double（取 53 位尾数，确定性）。
double uniform01(std::uint64_t h) {
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

// 组合哈希：seed + 槽位（纪元号/选择号）。
std::uint64_t hashAt(std::uint64_t seed, std::uint64_t slot) {
    return splitmix64(seed ^ splitmix64(slot + 0x2545F4914F6CDD1Dull));
}

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

// smoothstep（Hermite 3 次插值，输入先钳 0..1）。
double smoothstep(double x) {
    double t = clamp01(x);
    return t * t * (3.0 - 2.0 * t);
}

// 各离散态参数轮廓（常量表，顺序对齐 WeatherKind）。
WeatherProfile profileOf(WeatherKind k) {
    switch (k) {
        case WeatherKind::PartlyCloudy: return {0.0, 0.45, 3.0, 0.001, 40.0};
        case WeatherKind::Overcast:     return {0.0, 0.85, 4.5, 0.002, 50.0};
        case WeatherKind::Rain:         return {1.0, 1.0, 6.0, 0.004, 60.0};
        case WeatherKind::Clear:
        default:                        return {0.0, 0.05, 2.0, 0.0005, 30.0};
    }
}

// 状态机：纪元 e 的天气态。e==0 由 seed 直接定；否则按持续性概率维持
// 上一态，其余从「不同于上一态」的三个态里由 seed 选。
WeatherKind kindAtEpoch(const std::uint8_t* kinds, std::uint64_t seed,
                        std::int64_t e) {
    if (e <= 0) {
        std::uint64_t h = hashAt(seed, 0);
        return static_cast<WeatherKind>(h % static_cast<std::uint64_t>(static_cast<int>(WeatherKind::kCount)));
    }
    WeatherKind prev = static_cast<WeatherKind>(kinds[e - 1]);
    std::uint64_t h = hashAt(seed, static_cast<std::uint64_t>(e));
    if (uniform01(h) < kPersistProb) return prev;
    // 从其余三态中选：选择号与态号错开一位，避免与持续性分支相关。
    std::uint64_t pick = hashAt(seed, static_cast<std::uint64_t>(e) + 0x9E37ull) %
                         (static_cast<std::uint64_t>(static_cast<int>(WeatherKind::kCount)) - 1);
    int v = static_cast<int>(pick);
    if (v >= static_cast<int>(prev)) v += 1;
    return static_cast<WeatherKind>(v);
}

// 双态参数混合（逐字段 lerp）。
WeatherProfile lerpProfile(const WeatherProfile& a, const WeatherProfile& b,
                           double s) {
    WeatherProfile r;
    r.rainIntensity    = a.rainIntensity    + (b.rainIntensity    - a.rainIntensity)    * s;
    r.cloudCoverage    = a.cloudCoverage    + (b.cloudCoverage    - a.cloudCoverage)    * s;
    r.turbidity        = a.turbidity        + (b.turbidity        - a.turbidity)        * s;
    r.fogDensity       = a.fogDensity       + (b.fogDensity       - a.fogDensity)       * s;
    r.fogHeight        = a.fogHeight        + (b.fogHeight        - a.fogHeight)        * s;
    return r;
}

// t 时刻的混合轮廓（wetting 折迭与 sample 共用，唯一事实源）。
// 过渡窗口 = 跨纪元边界共 kBlendWindow 秒（前半=上一纪元尾，后半=下一纪元头），
// smoothstep 因子 s 单调 0→1 走完一次 kinds[e]→kinds[e+1]：
//   尾半 u∈[1-hw,1]：s = smoothstep((u-(1-hw))/(2hw))，边界处恰 0.5；
//   头半 u∈[0,hw]（e>0）：s = smoothstep((u+hw)/(2hw))，从 0.5 续走到 1。
// 两段拼接连续且对称式保证「边界瞬时 u=0」与「上一纪元 u=1」同值。
WeatherProfile blendedProfileAt(const std::uint8_t* kinds, std::int64_t epochCount,
                                double t) {
    std::int64_t e = static_cast<std::int64_t>(t / kEpochLength);
    if (e >= epochCount) e = epochCount - 1;
    if (e < 0) e = 0;
    double u = (t - static_cast<double>(e) * kEpochLength) / kEpochLength;
    if (u <= 0.0 && e > 0) { e -= 1; u = 1.0; } // 边界瞬时归一（端点 clamp 收敛）
    double hw = (kBlendWindow * 0.5) / kEpochLength;
    WeatherProfile p = profileOf(static_cast<WeatherKind>(kinds[e]));
    if (u > 1.0 - hw) {
        WeatherProfile next = profileOf(static_cast<WeatherKind>(kinds[e + 1]));
        double s = smoothstep((u - (1.0 - hw)) / (2.0 * hw));
        p = lerpProfile(p, next, s);
    } else if (u < hw && e > 0) {
        // 延续上一边界（kinds[e-1] -> kinds[e]）的后半程。
        WeatherProfile prev = profileOf(static_cast<WeatherKind>(kinds[e - 1]));
        double s = smoothstep((u + hw) / (2.0 * hw));
        p = lerpProfile(prev, p, s);
    }
    return p;
}

} // namespace

WeatherTimeline::WeatherTimeline(std::uint64_t seed, double durationSec,
                                 WeatherKind preset)
    : seed_(seed), preset_(preset) {
    if (!(durationSec > 0.0)) durationSec = static_cast<double>(1); // 防御：0/NaN 归 1s
    durationSec_ = durationSec;
    epochCount_ = static_cast<std::int64_t>(
        std::ceil(durationSec_ / kEpochLength));
    if (epochCount_ < 1) epochCount_ = 1;
    // 多折一个纪元（kinds_ 实际含 epochCount_+1 项）：让最后一段的纪元尾
    // 过渡窗口有后继态可混，保证 sample(t) 与 evaluateWeather(t) 在任意
    // duration 语义下逐字段一致（否则尾窗内 blend 恒 1，时间轴越长越假）。
    kinds_ = new std::uint8_t[static_cast<size_t>(epochCount_) + 1];

    // 状态机折迭：逐纪元推进（preset 锁定 = 全部同态）。
    for (std::int64_t e = 0; e <= epochCount_; ++e) {
        kinds_[e] = (preset_ != WeatherKind::kCount)
                        ? static_cast<std::uint8_t>(static_cast<int>(preset_))
                        : static_cast<std::uint8_t>(static_cast<int>(
                              kindAtEpoch(kinds_, seed_, e)));
    }

    // wetness 固定步长积分折迭（kWetStep 网格；样本点 = 步末值）。
    wetCount_ = static_cast<std::int64_t>(durationSec_ / kWetStep) + 1;
    wetness_ = new double[static_cast<size_t>(wetCount_)];
    double wet = 0.0;
    for (std::int64_t i = 0; i < wetCount_; ++i) {
        double t = static_cast<double>(i) * kWetStep;
        // 该子步的瞬时雨强 = 当刻混合轮廓（与 sample 共用 blendedProfileAt）。
        WeatherProfile p = blendedProfileAt(kinds_, epochCount_, t);
        // 前向欧拉固定步长（同源：sample 查表，不重算）。
        wet += (p.rainIntensity * kWetRate - (1.0 - p.rainIntensity) * kDryRate * wet) * kWetStep;
        wet = clamp01(wet);
        wetness_[i] = wet;
    }
}

WeatherTimeline::~WeatherTimeline() {
    delete[] kinds_;
    delete[] wetness_;
}

WeatherState WeatherTimeline::sample(double t) const {
    if (!std::isfinite(t)) t = 0.0;
    t = std::max(0.0, std::min(durationSec_, t));

    WeatherState st;
    st.t = t;

    // ---- 日内相位（常量 kDayLength， midnight 起步）----
    double dayPhase = std::fmod(t, kDayLength) / kDayLength;
    // 太阳角：midnight -pi/2 → 日出 0（dayPhase=0.25）→ 正午 +pi/2（0.5）→ 日落 0（0.75）。
    double elev = std::sin(2.0 * 3.14159265358979323846 * dayPhase - 3.14159265358979323846 * 0.5)
                  * (3.14159265358979323846 * 0.5);
    st.sunAzimuth = 2.0 * 3.14159265358979323846 * dayPhase;
    st.sunElevation = elev;
    double sinElev = std::sin(elev);
    // 昼因子 = smoothstep(sinElev)：日出/日落全程平滑渐暗（M-WEA1 曲线拍板
    // 2026-11-14：旧 clamp01(sinElev*8) 渐变窗口仅 ~8s，黄昏无渐暗过程）。
    double daylight = smoothstep(clamp01(sinElev));

    // ---- 纪元定位 + 过渡混合（与 wetness 折迭共用 blendedProfileAt）----
    std::int64_t e = static_cast<std::int64_t>(t / kEpochLength);
    if (e >= epochCount_) e = epochCount_ - 1;
    if (e < 0) e = 0;
    double u = (t - static_cast<double>(e) * kEpochLength) / kEpochLength;
    if (u <= 0.0 && e > 0) { e -= 1; u = 1.0; } // 边界瞬时归一（与共享函数同式）
    double hw = (kBlendWindow * 0.5) / kEpochLength;
    WeatherKind cur = static_cast<WeatherKind>(kinds_[e]);
    WeatherProfile p = blendedProfileAt(kinds_, epochCount_, t);
    st.kind = cur;
    st.blend = 1.0;
    if (u > 1.0 - hw) {
        // 过渡窗口前半（纪元尾）：kinds[e] -> kinds[e+1]，s 0 -> 0.5。
        WeatherKind nextK = static_cast<WeatherKind>(kinds_[e + 1]);
        double s = smoothstep((u - (1.0 - hw)) / (2.0 * hw));
        st.kind = (s > 0.5) ? nextK : cur;
        st.blend = (s > 0.5) ? s : 1.0 - s; // 主导态权重
    } else if (u < hw && e > 0) {
        // 过渡窗口后半（纪元头）：延续 kinds[e-1] -> kinds[e]，s 0.5 -> 1。
        WeatherKind prevK = static_cast<WeatherKind>(kinds_[e - 1]);
        double s = smoothstep((u + hw) / (2.0 * hw));
        st.kind = (s > 0.5) ? cur : prevK;
        st.blend = (s > 0.5) ? s : 1.0 - s;
    }

    st.rainIntensity = p.rainIntensity;
    st.cloudCoverage = clamp01(p.cloudCoverage);
    st.turbidity = p.turbidity;
    st.fogDensity = std::max(0.0, p.fogDensity);
    st.fogHeight = std::max(0.0, p.fogHeight);
    st.cloudTransmittance = clamp01(1.0 - st.cloudCoverage * 0.85);

    // ---- 太阳能量 / 色温 / 太阳盘 ----
    st.sunIntensity = daylight * st.cloudTransmittance;
    st.sunColorTempK = 2200.0 + (6500.0 - 2200.0) * clamp01(sinElev * 1.5); // 暖阳窗口放宽（M-WEA1 拍板）
    st.sunDiskIntensity = clamp01(daylight * st.cloudTransmittance * st.cloudTransmittance);

    // ---- 天穹（M10.1 skyGradient 常量同源基色）----
    // 基色：zenith(0.16,0.30,0.58) / horizon(0.62,0.71,0.84)。
    // M-WEA1 曲线拍板（2026-11-14）：调制顺序 = warm → 灰化 → 夜压暗（最后）。
    // 旧序把云灰拉向常数 0.35 发生在夜压暗之后 → 夜空被抬回 0.25 死灰；
    // 新序夜空 = 基色×nightScale ≈ 深蓝近黑（蓝分量最高，自然夜色）。
    double nightScale = 0.02 + 0.98 * daylight;
    // 暖色窗口放宽到 sinElev<0.4（旧 0.25 峰值恰逢日落瞬间天已黑），
    // 增益 0.40→0.85：晴态黄昏 horizon R 明显高于 B（晚霞可辨）。
    double warm = clamp01(1.0 - std::fabs(sinElev) * 2.5); // 地平线附近=1
    // 地平线余光（M-WEA1 曲线拍板 2026-11-14）：黄昏 warm 高时 horizon 衰减
    // 慢于天顶（低层大气散射余光）——日落地平线橙红带 + 天顶近黑 = 黄昏锚；
    // 深夜 warm→0 余光自然熄灭。
    double nightScaleH = 0.02 + 0.98 * std::max(daylight, warm * 0.35);
    double grayMix = clamp01((st.turbidity - 2.0) / 4.0) * 0.5; // 浊度去饱和
    double cloudMix = st.cloudCoverage * 0.7;                   // 云遮灰化
    // 分通道处理（灰化目标 = 通道亮度近似，取 RGB 均值保持确定性简单式）。
    struct Chan { double z, h; };
    Chan cz{0.16, 0.62}, cg{0.30, 0.71}, cb{0.58, 0.84};
    auto mixGray = [](double v, double gray, double m) {
        return v + (gray - v) * m;
    };
    auto apply = [&](Chan& c, double warmGain, double warmLoss) {
        // 顺序（M-WEA1 拍板）：暖染色 → 浊度/云灰化 → 夜压暗（最后）。
        c.h *= 1.0 + warm * warmGain;
        c.z *= 1.0 + warm * warmGain * 0.5;
        c.h *= 1.0 - warm * warmLoss;
        c.z *= 1.0 - warm * warmLoss * 0.5;
        double avg = (c.z + c.h) * 0.5;
        c.z = mixGray(c.z, avg * 0.9, grayMix);
        c.h = mixGray(c.h, avg * 0.9, grayMix);
        c.z = mixGray(c.z, 0.35, cloudMix);
        c.h = mixGray(c.h, 0.35, cloudMix);
        c.z *= nightScale;
        c.h *= nightScaleH;   // 地平线余光：衰减慢于天顶（黄昏锚）
        c.z = clamp01(c.z);
        c.h = clamp01(c.h);
    };
    apply(cz, 0.85, 0.30);
    apply(cg, 0.10, 0.10);
    apply(cb, 0.00, 0.45);   // B 随 warm 压暗（M-WEA1 拍板：晚霞去紫偏橙红）
    st.skyZenithR = cz.z; st.skyZenithG = cg.z; st.skyZenithB = cb.z;
    st.skyHorizonR = cz.h; st.skyHorizonG = cg.h; st.skyHorizonB = cb.h;

    // ---- 地面湿ness：查积分网格（步末值 + 步内线性插值，确定性连续）----
    double wpos = t / kWetStep;
    std::int64_t wi = static_cast<std::int64_t>(wpos);
    if (wi < 0) wi = 0;
    if (wi > wetCount_ - 1) wi = wetCount_ - 1;
    double frac = wpos - static_cast<double>(wi); // [0,1)
    double w0 = wetness_[wi];
    double w1 = (wi + 1 < wetCount_) ? wetness_[wi + 1] : w0;
    st.groundWetness = clamp01(w0 + (w1 - w0) * frac);

    return st;
}

WeatherState evaluateWeather(std::uint64_t seed, double t, WeatherKind preset) {
    WeatherTimeline tl(seed, t + 1.0, preset); // +1s 保证 t 落在折迭范围内
    return tl.sample(t);
}

const char* weatherKindName(WeatherKind k) {
    switch (k) {
        case WeatherKind::PartlyCloudy: return "partly";
        case WeatherKind::Overcast:     return "overcast";
        case WeatherKind::Rain:         return "rain";
        case WeatherKind::Clear:
        default:                        return "clear";
    }
}

WeatherKind weatherKindFromName(const char* name) {
    if (!name) return WeatherKind::kCount;
    if (std::strcmp(name, "clear") == 0) return WeatherKind::Clear;
    if (std::strcmp(name, "partly") == 0) return WeatherKind::PartlyCloudy;
    if (std::strcmp(name, "overcast") == 0) return WeatherKind::Overcast;
    if (std::strcmp(name, "rain") == 0) return WeatherKind::Rain;
    return WeatherKind::kCount;
}

} // namespace nl::core
