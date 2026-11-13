#pragma once
// 天气参数层 · 天气状态层（WEA 轨道 M-WEA0，纯 CPU 零图形符号，计划 docs/WEATHER-PLAN.md）。
//
// 形态拍板（2026-11-13 用户拍板 Q-WEA0-A/B）：
//   - 时间线 = 离散天气态 + 平滑过渡（bevy_weather forecast() 口径：seed 驱动
//     晴/少云/阴/雨 离散态状态机，态间 smoothstep 平滑插值；cvar/CLI 手调
//     「强制某天气」= preset 锁定语义自然）。
//   - 落点 = nl_core 新模块（与 game_clock 同级同纪律：纯 double、无隐藏状态、
//     零图形 API；M-WEA1 viewer 直接 include 消费）。
//
// 三处参考口径（2026-11-11/12 实读，详 WEATHER-PLAN-DRAFT.md §2/§3）：
//   - bevy_weather 0.2.0（MIT/Apache，联网实读）：天气 = 时钟 + seed 纯函数，
//     确定性可回放；GroundConditions.wetness 是雨后累积的记忆状态（对历史
//     雨强的确定性积分，非瞬时函数）。
//   - Wicked Engine（MIT）WeatherComponent：sunDirection/sunColor/sunEnergy、
//     horizon/zenith、fog、云 coverage —— WeatherState 字段集的范围标尺。
//   - Carrot（无时变参数，反面教材）/ UE5.8.4（天气层是 Lumen 一等输入）：
//     M-WEA0 只备齐渲染侧将来要吃的参数，不碰渲染（零基线影响）。
//
// 确定性纪律（对齐 game_clock.hpp）：
//   - 纯 double 运算、无隐藏状态、无全局量；同 seed + 同时间轴逐字节重现
//     （CPU 侧保持 bytes，Q11-1 口径）。
//   - RNG = splitmix64（纯整数哈希，跨平台一致）；所有浮点只走 double。
//   - 固定步长积分（wetness）同源：kWetStep 恒定，禁止按调用方 dt 变步长。
//
// 纯函数契约：WeatherTimeline 一次性折迭（fold）出时间轴（状态机推进 + wetness
// 积分），sample(t) 只读查表；evaluateWeather(seed, t) 为无状态便捷入口
// （内部建时间轴，O(t)，一次性查询用）。
//
// 边界：单线程使用；不做日夜光照正确性（M-WEA1 渲染侧才对口径）；本模块
// 输出的参数范围约定见 WEATHER-TOOLS-GUIDE.md。

#include <cstdint>

namespace nl::core {

// 离散天气态（kCount 供 preset 判定「未锁定」）。
enum class WeatherKind : int {
    Clear = 0,        // 晴：少云、低浊度、能见度好
    PartlyCloudy = 1, // 少云
    Overcast = 2,     // 阴：厚云、直光大幅衰减、浊度高
    Rain = 3,         // 雨：全覆盖 + 高雾 + 地面湿化
    kCount = 4,
};

// 单个天气态的参数轮廓（内部折迭用，输出经 WeatherState 呈现）。
struct WeatherProfile {
    double rainIntensity;  // 0..1 雨强（wetness 积分源）
    double cloudCoverage;  // 0..1 云覆盖
    double turbidity;      // 2~6 大气浊度（天穹灰度/衰减趋势）
    double fogDensity;     // 高度雾密度（渲染侧线性衰减系数，1/米）
    double fogHeight;      // 高度雾标高（米）
};

// 天气状态快照（同 seed + 同 t 逐字节可重现；全部 double，禁 float 混入）。
struct WeatherState {
    double t = 0.0;              // 输入回显：时间轴秒
    WeatherKind kind = WeatherKind::Clear; // 当前主导天气态（过渡期取后继态）
    double blend = 0.0;          // 0..1 过渡窗口内的混合因子（1=纯当前态）
    // --- 太阳（日内相位驱动） ---
    double sunAzimuth = 0.0;     // rad，随日相位全圆旋转
    double sunElevation = 0.0;   // rad，正=白昼；午夜 -pi/2、正午 +pi/2
    double sunIntensity = 0.0;   // 0..1 直光能量（昼因子 x 云透射）
    double sunColorTempK = 6500.0; // 色温 K（低仰角偏暖，渲染侧换 RGB）
    // --- 天穹（M10.1 skyGradient 常量同源基色，经浊度/云/夜调制） ---
    double skyZenithR = 0.0, skyZenithG = 0.0, skyZenithB = 0.0;
    double skyHorizonR = 0.0, skyHorizonG = 0.0, skyHorizonB = 0.0;
    double turbidity = 2.0;
    double sunDiskIntensity = 1.0; // 太阳盘视觉强度（云遮/地平线衰减）
    // --- 云 / 雾 ---
    double cloudCoverage = 0.0;      // 0..1
    double cloudTransmittance = 1.0; // 0..1 直光透射（bevy 透射率口径）
    double fogDensity = 0.0;
    double fogHeight = 0.0;
    // --- 地面（记忆状态：雨后仍湿） ---
    double groundWetness = 0.0;  // 0..1
    double rainIntensity = 0.0;  // 0..1 当前瞬时雨强
};

// ---- 常量（全部写死，禁运行期改；改动 = 破坏已回放时间轴的确定性契约）----
namespace weather_detail {
// 日长（秒）：整圈昼夜相位周期；决定 sunAzimuth/Elevation 与色温。
constexpr double kDayLength = 600.0;
// 天气纪元（秒）：状态机每推进一格的时长；雨/晴的最短持续感由此定。
constexpr double kEpochLength = 120.0;
// 过渡窗口（秒）：纪元边界两侧各占一半，smoothstep 平滑插值。
constexpr double kBlendWindow = 20.0;
// 状态机持续性概率：纪元推进时维持上一态的概率（雨/晴有持续感）。
constexpr double kPersistProb = 0.55;
// wetness 积分子步（秒）：固定步长确定性积分（M-N6 教训：累加必须同源）。
constexpr double kWetStep = 1.0;
// wetness 动力学：雨中增湿速率 / 雨停干燥速率（一阶衰减）。
constexpr double kWetRate = 0.08;   // /s，雨强 1 时约 12.5s 从 0 到 1
constexpr double kDryRate = 0.02;   // /s，一阶干燥系数
} // namespace weather_detail

// 一次性折迭的天气时间轴：构造即冻结（seed + 时长 + 可选 preset 锁定），
// 之后 sample 只读。preset != kCount 时锁定该天气态（cvar/CLI 手调语义）。
class WeatherTimeline {
public:
    // preset 传 WeatherKind::kCount 表示不锁定（自动状态机）。
    WeatherTimeline(std::uint64_t seed, double durationSec,
                    WeatherKind preset = WeatherKind::kCount);
    ~WeatherTimeline();
    WeatherTimeline(const WeatherTimeline&) = delete;
    WeatherTimeline& operator=(const WeatherTimeline&) = delete;

    // 查询 t 秒处状态；t 越界钳到 [0, duration]（防御性，NaN 归 0）。
    WeatherState sample(double t) const;

    double duration() const { return durationSec_; }
    std::uint64_t seed() const { return seed_; }
    WeatherKind preset() const { return preset_; }

private:
    std::uint64_t seed_;
    double durationSec_;
    WeatherKind preset_;
    std::int64_t epochCount_;     // 折迭出的纪元数（覆盖 durationSec_）
    std::uint8_t* kinds_;         // epochCount_+1 个 WeatherKind（多折一纪元供尾过渡）
    double* wetness_;             // 每个积分步的 wetness（kWetStep 网格）
    std::int64_t wetCount_;
};

// 无状态便捷入口：内部构造 WeatherTimeline 后 sample（O(t)，一次性查询用）。
// preset 传 WeatherKind::kCount 表示不锁定。
WeatherState evaluateWeather(std::uint64_t seed, double t,
                             WeatherKind preset = WeatherKind::kCount);

// WeatherKind 的 ASCII 名（TSV/HUD 用；禁非 ASCII）。
const char* weatherKindName(WeatherKind k);
// ASCII 名解析（大小写敏感，非法返回 kCount）。
WeatherKind weatherKindFromName(const char* name);

} // namespace nl::core
