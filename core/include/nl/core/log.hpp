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

} // namespace nl::core::log
