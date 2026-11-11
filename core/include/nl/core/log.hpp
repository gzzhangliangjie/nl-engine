#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <utility>

namespace nl::core::log {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

using Sink = std::function<void(Level, const std::string&)>;

// 输出目标：M0.2 起把 NVRHI device callback 接成 sink，
// 让「验证层零输出」变成可机械统计的判据，而不是靠肉眼看控制台。
void setSink(Sink sink);
void resetSink();
void setLevel(Level minLevel);
Level level();

// 设备 / 验证层错误专用入口，独立计数；验收要求为 0。
void deviceError(const std::string& source, const std::string& message);
int  errorCount();
int  deviceErrorCount();
void resetCounters();

namespace detail {
const char* levelName(Level level);
void emit(Level level, std::string message);
void emitCh(Level level, const char* channel, std::string message); // M-LOG1
} // namespace detail

template <typename... Args>
void write(Level lvl, Args&&... args) {
    std::ostringstream os;
    (os << ... << std::forward<Args>(args));
    detail::emit(lvl, os.str());
}

template <typename... Args> void trace(Args&&... a) { write(Level::Trace, std::forward<Args>(a)...); }
template <typename... Args> void debug(Args&&... a) { write(Level::Debug, std::forward<Args>(a)...); }
template <typename... Args> void info (Args&&... a) { write(Level::Info,  std::forward<Args>(a)...); }
template <typename... Args> void warn (Args&&... a) { write(Level::Warn,  std::forward<Args>(a)...); }
template <typename... Args> void error(Args&&... a) { write(Level::Error, std::forward<Args>(a)...); }
template <typename... Args> void fatal(Args&&... a) { write(Level::Fatal, std::forward<Args>(a)...); }

// ---------------------------------------------------------------------------
// M-LOG0 / M-LOG1 扩面（2026-11-11，Q-L1~L3 拍板，docs/p8/P8-LOG-PLAN.md §6）。
// 兼容铁律：上面全部老入口签名与行为零改动；spdlog 内核不泄漏进本头文件。
// ---------------------------------------------------------------------------

// 分类通道：显式带通道名（renderer / scene / asset / ...）。
// 级别闸 = 通道覆盖级（setFilter/setChannelLevel）未设置时跟随全局 minLevel。
// 输出行格式 `[nl:<level>|<channel>] message`（老入口格式不变，锚点回归不受影响）。
template <typename... Args>
void writeCh(Level lvl, const char* channel, Args&&... args) {
    std::ostringstream os;
    (os << ... << std::forward<Args>(args));
    detail::emitCh(lvl, channel, os.str());
}

template <typename... Args> void traceCh(const char* ch, Args&&... a) { writeCh(Level::Trace, ch, std::forward<Args>(a)...); }
template <typename... Args> void debugCh(const char* ch, Args&&... a) { writeCh(Level::Debug, ch, std::forward<Args>(a)...); }
template <typename... Args> void infoCh (const char* ch, Args&&... a) { writeCh(Level::Info,  ch, std::forward<Args>(a)...); }
template <typename... Args> void warnCh (const char* ch, Args&&... a) { writeCh(Level::Warn,  ch, std::forward<Args>(a)...); }
template <typename... Args> void errorCh(const char* ch, Args&&... a) { writeCh(Level::Error, ch, std::forward<Args>(a)...); }
template <typename... Args> void fatalCh(const char* ch, Args&&... a) { writeCh(Level::Fatal, ch, std::forward<Args>(a)...); }

// 字符串过滤指令（bevy EnvFilter 口径，学 UE 语义按需裁剪）：
//   "nl:info,renderer=warn"  — 全局缺省 info，renderer 通道单放 warn
//   "renderer=off"           — 静默单通道
// 语法：逗号分隔；段 = `<level>`（设全局缺省，等价 setLevel）或
// `<channel>=<level>`；"nl:" 前缀可有可无；level ∈ trace/debug/info/warn/
// error/fatal/off；channel = 小写字母开头、[a-z0-9_-] 组成、≤32 字符。
// 非法指令返回 false 并把原因写进 *errOut（可空），当前过滤面保持不变（显式
// 报错，不静默吞）。合法指令替换整个过滤面（全局 + 通道表）。
bool setFilter(const std::string& instruction, std::string* errOut = nullptr);

// 当前生效过滤指令回读（"nl:info,renderer=warn" 规范形态；tool 卡 / console 用）。
std::string filterInstruction();

// 单通道运行时调整（不动全局缺省；level 传 Fatal 之上无意义，off 请用 setFilter）。
void setChannelLevel(const std::string& channel, Level lvl);

} // namespace nl::core::log
