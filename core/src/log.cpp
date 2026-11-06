#include "nl/core/log.hpp"

#include <iostream>
#include <mutex>

namespace nl::core::log {
namespace {

std::mutex g_mutex;
Sink       g_sink;
Level      g_minLevel = Level::Debug;
int        g_errors = 0;
int        g_deviceErrors = 0;

} // namespace

namespace detail {

const char* levelName(Level lvl) {
    switch (lvl) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
        case Level::Fatal: return "fatal";
    }
    return "?";
}

void emit(Level lvl, std::string message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lvl == Level::Error || lvl == Level::Fatal) ++g_errors;
    if (lvl < g_minLevel) return;

    std::cout << "[nl:" << levelName(lvl) << "] " << message << std::endl;
    if (g_sink) g_sink(lvl, message);
}

} // namespace detail

void setSink(Sink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void resetSink() { setSink({}); }

void setLevel(Level minLevel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_minLevel = minLevel;
}

Level level() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_minLevel;
}

void deviceError(const std::string& source, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_deviceErrors;
    }
    error("DEVICE/", source, ": ", message);
}

int errorCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_errors;
}

int deviceErrorCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_deviceErrors;
}

void resetCounters() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_errors = 0;
    g_deviceErrors = 0;
}

} // namespace nl::core::log
