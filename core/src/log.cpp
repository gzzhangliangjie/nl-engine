#include "nl/core/log.hpp"

// M-LOG0（2026-11-11，Q-L1 拍板）：内核换 vendored spdlog v1.17.0（MIT）。
// 兼容铁律（P8-LOG-PLAN §4）：老入口签名与行为零改动——stdout 行格式
// `[nl:<level>] message`、sink 回调收原始 message、errorCount /
// deviceErrorCount 计数口径（含「被级别闸滤掉也计数」）全部逐字保留。
// spdlog 用法：直构 logger（不进全局 registry）、level=trace（闸全在 wrapper）、
// stdout/ODS pattern "%v"（行文本 wrapper 拼好，锚点格式不漂）、文件路每条 flush。
// 实现决策留档：M-LOG0 阶梯原文写「异步写」，落码取同步 + 每条 flush——日志量小、
// 正确性优先，避免 async 线程池退出期丢尾行破坏回归取证；M-LOG2 拍板 ④ 不重开
// async（保持同步），文件路换 rotating_file_sink_mt（拍板 ③ 大小滚动一代）。

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#ifndef _WIN32
#error "nl::core::log M-LOG0: ODS 转发与 exe 名提取按 Windows 口径实现（当前目标平台只有 Windows）"
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nl::core::log {
namespace {

std::mutex g_mutex;
Sink       g_sink;
Level      g_minLevel = Level::Debug;
int        g_errors = 0;
int        g_deviceErrors = 0;

// ---- spdlog 广播层（懒建，首次 emit 初始化；文件路再懒一步） ----
std::shared_ptr<spdlog::logger> g_logger;
bool g_fileInit = false;

// ---- M-LOG2：帧号上下文 + 文件滚动 ----
std::uint64_t g_frame = 0;                 // 0 = 未注入，行尾无后缀
std::size_t   g_rollLimit = 50u << 20;     // 拍板 ③：单文件 50 MiB 上限，滚动一代

// ---- M-LOG1 过滤面 ----
// off 通道静默；nullopt 表示「未覆盖，跟随全局」。
// 通道表用保序 vector（指令回读文本顺序确定；通道数小，线性扫够用）。
using ChEntry = std::pair<std::string, std::optional<Level>>;
std::vector<ChEntry> g_chLevels;
std::string g_filterText = "nl:debug"; // 规范形态回读（初始 = 默认全局 Debug）

const char* levelNameUnlocked(Level lvl) {
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

std::string backendName() {
    // nl_core 编译期注入（根 CMakeLists：NL_LOG_BACKEND_NAME="DX12|VULKAN"）。
#ifdef NL_LOG_BACKEND_NAME
    return NL_LOG_BACKEND_NAME;
#else
    return "unknown";
#endif
}

std::string exeBaseName() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::filesystem::path p(path);
    return p.stem().string();
}

std::string timeStamp() {
    // yyyymmdd-hhmmss 本地时间（Q-L2 拍板形态）。
    std::tm tmv{};
    std::time_t now = std::time(nullptr);
    localtime_s(&tmv, &now);
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::shared_ptr<spdlog::logger> buildLogger(bool withFile) {
    std::vector<spdlog::sink_ptr> sinks;
    auto stdoutSink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    stdoutSink->set_pattern("%v"); // 行文本已含 [nl:...] 前缀，原样出
    sinks.push_back(stdoutSink);
    // ODS 转发（L3 调试链路）：不判调试器是否在场——DebugView/DBWIN 取证
    // 不挂调试器（M8.1 dbwin_capture.py 同路），必须常开。
    auto odsSink = std::make_shared<spdlog::sinks::msvc_sink_mt>(false);
    odsSink->set_pattern("%v");
    sinks.push_back(odsSink);
    if (withFile) {
        std::error_code ec;
        std::filesystem::create_directories("out/log", ec); // 相对进程 cwd（Q-L2）
        if (!ec) {
            // 同秒启动的多个进程会撞同名文件（truncate 互相吞）：碰撞时加 -2/-3
            // 后缀消歧，拍板形态 <exe>-<backend>-<yyyymmdd-hhmmss>.log 保持不变。
            const std::string base = "out/log/" + exeBaseName() + "-" + backendName()
                                     + "-" + timeStamp();
            std::string name = base + ".log";
            int n = 2;
            while (n <= 99 && std::filesystem::exists(name))
                name = base + "-" + std::to_string(n++) + ".log";
            // M-LOG2 拍板 ③：rotating_file_sink 运行中超 g_rollLimit 时把当前文件
            // 滚为 <name>.1（保留一代，再滚覆盖旧的），当前 .log 始终有效。
            // 文件名带时间戳每次启动唯一，sink 启动期的初始轮转不会误伤旧文件
            // （Q-L2「旧文件不清理」拍板不变）。
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                name, g_rollLimit, 1);
            fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v"); // 文件路带时间戳，锚点仍是子串
            sinks.push_back(fileSink);
        }
        // 目录创建失败：文件路静默放弃，stdout/ODS 照常（取证不丢主路）。
    }
    auto logger = std::make_shared<spdlog::logger>("nl", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace); // 级别闸全在 wrapper（M0.1 语义）
    logger->flush_on(spdlog::level::trace);  // 每条刷盘：量小，取证正确性优先
    return logger;
}

void ensureLogger() { // mutex held；进程内一次性建齐三路（stdout/ODS/文件）
    if (g_fileInit) return;
    g_fileInit = true;
    g_logger = buildLogger(true);
}

void broadcast(const std::string& line) { // mutex held
    ensureLogger();
    if (g_logger) g_logger->log(spdlog::level::trace, std::string_view(line));
}

// ---- 过滤指令解析（M-LOG1，bevy EnvFilter 口径） ----

bool validChannelName(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    if (name[0] < 'a' || name[0] > 'z') return false;
    return std::all_of(name.begin() + 1, name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool parseLevel(const std::string& s, std::optional<Level>& out) {
    if (s == "trace") { out = Level::Trace; return true; }
    if (s == "debug") { out = Level::Debug; return true; }
    if (s == "info")  { out = Level::Info;  return true; }
    if (s == "warn")  { out = Level::Warn;  return true; }
    if (s == "error") { out = Level::Error; return true; }
    if (s == "fatal") { out = Level::Fatal; return true; }
    if (s == "off")   { out = std::nullopt; return true; } // 静默单通道
    return false;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// 成功才落表（g_minLevel + g_chLevels + g_filterText）；失败原面不动。
void rebuildFilterTextLocked(); // 规范形态重拼（定义见下）

bool applyFilter(const std::string& instruction, std::string& err) {
    std::string text = instruction;
    if (text.rfind("nl:", 0) == 0) text = text.substr(3); // "nl:" 前缀可有可无

    std::optional<Level> newGlobal;
    std::vector<ChEntry> newCh;

    size_t begin = 0;
    int segIdx = 0;
    while (true) {
        size_t end = text.find(',', begin);
        std::string seg(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        ++segIdx;
        std::string t(trim(seg));
        if (t.empty()) { err = "invalid filter segment #" + std::to_string(segIdx) + " '" + seg + "': empty"; return false; }
        std::optional<Level> lvl;
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            // 段 = `<level>`：设全局缺省（多个全局段以后者为准，bevy 同语义）
            if (!parseLevel(t, lvl)) {
                err = "invalid filter segment '" + t + "': expected <level> or <channel>=<level>";
                return false;
            }
            newGlobal = lvl;
        } else {
            std::string ch(trim(std::string_view(t).substr(0, eq)));
            std::string lv(trim(std::string_view(t).substr(eq + 1)));
            if (!validChannelName(ch)) {
                err = "invalid channel name '" + ch + "': lowercase letters, digits, '_', '-' only";
                return false;
            }
            if (!parseLevel(lv, lvl)) {
                err = "invalid level '" + lv + "' for channel '" + ch
                    + "': trace|debug|info|warn|error|fatal|off";
                return false;
            }
            // 同通道重复出现：后者覆盖前者（首次出现的顺序保留）
            auto it = std::find_if(newCh.begin(), newCh.end(),
                                   [&](const ChEntry& e) { return e.first == ch; });
            if (it != newCh.end()) it->second = lvl;
            else newCh.emplace_back(ch, lvl);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }

    if (!newGlobal) newGlobal = Level::Debug; // 指令未给全局缺省时保持出厂值
    g_minLevel = *newGlobal;
    g_chLevels = std::move(newCh);
    rebuildFilterTextLocked();
    return true;
}

bool channelOpen(const std::string& channel, Level lvl) { // mutex held
    for (const auto& [ch, lv] : g_chLevels)
        if (ch == channel) return lv && lvl >= *lv;
    return lvl >= g_minLevel;
}

// filterInstruction() 的规范形态重拼（mutex held；事实源 = 全局缺省 + 通道表）。
void rebuildFilterTextLocked() {
    g_filterText = std::string("nl:") + levelNameUnlocked(g_minLevel);
    for (const auto& [ch, lv] : g_chLevels) {
        g_filterText += ",";
        g_filterText += ch;
        g_filterText += "=";
        g_filterText += lv ? levelNameUnlocked(*lv) : "off";
    }
}

} // namespace

namespace detail {

const char* levelName(Level lvl) { return levelNameUnlocked(lvl); }

void emit(Level lvl, std::string message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lvl == Level::Error || lvl == Level::Fatal) ++g_errors;
    if (lvl < g_minLevel) return;

    std::string line = std::string("[nl:") + levelNameUnlocked(lvl) + "] " + message;
    if (g_frame) line += " #" + std::to_string(g_frame); // M-LOG2 拍板 ①
    broadcast(line); // stdout + ODS（+ 文件路若已建）
    if (g_sink) g_sink(lvl, message);
}

void emitCh(Level lvl, const char* channel, std::string message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lvl == Level::Error || Level::Fatal == lvl) ++g_errors;
    if (!channelOpen(channel, lvl)) return;

    std::string line = std::string("[nl:") + levelNameUnlocked(lvl) + "|" + channel + "] " + message;
    if (g_frame) line += " #" + std::to_string(g_frame); // M-LOG2 拍板 ①
    broadcast(line);
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

bool setFilter(const std::string& instruction, std::string* errOut) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string err;
    if (!applyFilter(instruction, err)) {
        if (errOut) *errOut = err;
        return false;
    }
    return true;
}

std::string filterInstruction() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_filterText;
}

void setChannelLevel(const std::string& channel, Level lvl) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = std::find_if(g_chLevels.begin(), g_chLevels.end(),
                           [&](const ChEntry& e) { return e.first == channel; });
    if (it != g_chLevels.end()) it->second = lvl;
    else g_chLevels.emplace_back(channel, lvl);
    rebuildFilterTextLocked();
}

// ---- M-LOG2 ----

void setLogFrame(std::uint64_t frame) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frame = frame;
}

void clearLogFrame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frame = 0;
}

void setFileRollLimit(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rollLimit = bytes < 1024 ? 1024 : bytes; // 防呆下限 1 KiB
    // 文件 sink 的 max_size 在构造时固化：若文件路已建，热重建 logger 让新上限
    // 立即生效（新文件按新时间戳另起，旧文件不清理——Q-L2 拍板不变）。
    if (g_logger) g_logger = buildLogger(true);
}

std::size_t fileRollLimit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_rollLimit;
}

} // namespace nl::core::log
