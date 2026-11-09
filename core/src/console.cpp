// P8 开发者控制台 M-C0：纯逻辑内核实现（零 ImGui / 零图形符号）。
// 口径见 console.hpp 头注（UE 统一注册表 + Carrot 从零节奏）。

#include "nl/core/console.hpp"

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace nl::core {
namespace console {

// ---- CVarValue ----

const char* cvarTypeName(CVarType type) {
    switch (type) {
        case CVarType::Bool:   return "bool";
        case CVarType::Int:    return "int";
        case CVarType::Float:  return "float";
        case CVarType::String: return "string";
    }
    return "?";
}

std::string CVarValue::toString() const {
    switch (type) {
        case CVarType::Bool:   return b ? "true" : "false";
        case CVarType::Int: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(i));
            return buf;
        }
        case CVarType::Float: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", f);
            return buf;
        }
        case CVarType::String: return s;
    }
    return "?";
}

// ---- CVar ----

CVar::CVar(std::string name, CVarValue initial, std::string help)
    : name_(std::move(name)), help_(std::move(help)), value_(std::move(initial)) {}

void CVar::assign(CVarValue v) {
    value_ = std::move(v);
    if (onChanged_) onChanged_(*this);
}

void CVar::setBool(bool v) {
    if (value_.type != CVarType::Bool) { return; }   // 编程错误：console 层记 Error，见调用方
    CVarValue nv; nv.type = CVarType::Bool; nv.b = v; assign(std::move(nv));
}
void CVar::setInt(std::int64_t v) {
    if (value_.type != CVarType::Int) { return; }
    CVarValue nv; nv.type = CVarType::Int; nv.i = v; assign(std::move(nv));
}
void CVar::setFloat(double v) {
    if (value_.type != CVarType::Float) { return; }
    CVarValue nv; nv.type = CVarType::Float; nv.f = v; assign(std::move(nv));
}
void CVar::setString(std::string v) {
    if (value_.type != CVarType::String) { return; }
    CVarValue nv; nv.type = CVarType::String; nv.s = std::move(v); assign(std::move(nv));
}

bool CVar::setFromString(const std::string& text) {
    switch (value_.type) {
        case CVarType::Bool: {
            bool v;
            if (text == "true" || text == "1" || text == "on" || text == "yes") v = true;
            else if (text == "false" || text == "0" || text == "off" || text == "no") v = false;
            else return false;
            setBool(v);
            return true;
        }
        case CVarType::Int: {
            errno = 0;
            char* end = nullptr;
            long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno != 0 || end == text.c_str() || *end != '\0') return false;
            setInt(static_cast<std::int64_t>(v));
            return true;
        }
        case CVarType::Float: {
            errno = 0;
            char* end = nullptr;
            double v = std::strtod(text.c_str(), &end);
            if (errno != 0 || end == text.c_str() || *end != '\0') return false;
            setFloat(v);
            return true;
        }
        case CVarType::String:
            setString(text);
            return true;
    }
    return false;
}

// ---- tokenize ----

std::vector<std::string> tokenize(const std::string& line, std::string* error) {
    if (error) error->clear();
    std::vector<std::string> out;
    std::string cur;
    bool inToken = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == ' ' || c == '\t') {
            if (inToken) { out.push_back(cur); cur.clear(); inToken = false; }
            continue;
        }
        if (c == '"' || c == '\'') {
            inToken = true;
            std::size_t close = line.find(c, i + 1);
            if (close == std::string::npos) {
                if (error) *error = "unterminated quote";
                return {};
            }
            cur.append(line, i + 1, close - i - 1);
            i = close;
            continue;
        }
        cur.push_back(c);
        inToken = true;
    }
    if (inToken) out.push_back(cur);
    return out;
}

// ---- Console ----

Console& Console::get() {
    static Console inst;
    return inst;
}

CVar& Console::registerCVar(const std::string& name, CVarValue def, std::string help) {
    auto it = cvars_.find(name);
    if (it != cvars_.end()) {
        // 重复注册：保留现值，只更新 help（console.hpp 头注口径）。
        it->second->help_ = std::move(help);
        appendLog(Severity::Warn, "cvar re-registered (kept value): " + name);
        return *it->second;
    }
    auto cv = std::unique_ptr<CVar>(new CVar(name, std::move(def), std::move(help)));
    CVar& ref = *cv;
    cvars_.emplace(name, std::move(cv));
    return ref;
}

CVar& Console::registerBool(const std::string& name, bool def, std::string help) {
    CVarValue v; v.type = CVarType::Bool; v.b = def;
    return registerCVar(name, v, std::move(help));
}
CVar& Console::registerInt(const std::string& name, std::int64_t def, std::string help) {
    CVarValue v; v.type = CVarType::Int; v.i = def;
    return registerCVar(name, v, std::move(help));
}
CVar& Console::registerFloat(const std::string& name, double def, std::string help) {
    CVarValue v; v.type = CVarType::Float; v.f = def;
    return registerCVar(name, v, std::move(help));
}
CVar& Console::registerString(const std::string& name, std::string def, std::string help) {
    CVarValue v; v.type = CVarType::String; v.s = std::move(def);
    return registerCVar(name, v, std::move(help));
}

void Console::registerCommand(const std::string& name, std::string help, CommandFn fn) {
    auto it = commands_.find(name);
    if (it != commands_.end()) {
        it->second.help = std::move(help);
        it->second.fn = std::move(fn);
        appendLog(Severity::Warn, "command re-registered: " + name);
        return;
    }
    commands_.emplace(name, CommandEntry{std::move(help), std::move(fn)});
}

bool Console::unregisterCommand(const std::string& name) {
    const auto it = commands_.find(name);
    if (it == commands_.end()) return false;
    commands_.erase(it);
    appendLog(Severity::Info, "command unregistered: " + name);
    return true;
}

CVar* Console::findCVar(const std::string& name) {
    auto it = cvars_.find(name);
    return it == cvars_.end() ? nullptr : it->second.get();
}

std::vector<std::string> Console::completePrefix(const std::string& prefix) const {
    std::vector<std::string> out;
    // std::map 有序：lower_bound 起步顺序扫即可拿到全部前缀匹配。
    auto scan = [&](const auto& table) {
        for (auto it = table.lower_bound(prefix); it != table.end(); ++it) {
            if (it->first.compare(0, prefix.size(), prefix) != 0) break;
            out.push_back(it->first);
        }
    };
    scan(cvars_);
    scan(commands_);
    // 两个 map 各自有序，但合并后乱序：统一排序保证补全候选稳定。
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Console::Entry> Console::allEntries() const {
    std::vector<Entry> out;
    out.reserve(cvars_.size() + commands_.size());
    for (const auto& [n, cv] : cvars_)    out.push_back({n, cv->help(), true});
    for (const auto& [n, cmd] : commands_) out.push_back({n, cmd.help, false});
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

void Console::appendLog(Severity sev, std::string text) {
    log_.push_back({sev, std::move(text)});
    if (log_.size() > kLogCap) log_.erase(log_.begin(), log_.begin() + (log_.size() - kLogCap));
}

ExecResult Console::finish(ExecCode code, std::string output, Severity sev) {
    if (!output.empty()) appendLog(sev, output);
    return ExecResult{code, std::move(output)};
}

// M-C2 内建命令集。UE ConsoleManager 把 help/dump 类查询内建在管理器本体（DumpObjects），
// 这里同口径：注册表自己承担查询类命令，业务侧只注册引擎参数。
// 注意 lambda 里不能用 exec()（会二次记 "> cmd" 日志行），直接走注册表原语。
void Console::installBuiltins() {
    if (builtinsInstalled_) return;
    builtinsInstalled_ = true;

    registerCommand("help", "list all commands", [this](const std::vector<std::string>&) {
        std::string out = "commands:";
        for (const auto& e : allEntries())
            if (!e.isCVar) out += "\n  " + e.name + "  -  " + e.help;
        out += "\n(use cvar.list for variables)";
        return CommandResult{true, std::move(out)};
    });

    registerCommand("cvar.list", "list all registered cvars", [this](const std::vector<std::string>&) {
        std::string out = "cvars:";
        for (const auto& e : allEntries()) {
            if (!e.isCVar) continue;
            const CVar* cv = findCVar(e.name);
            out += "\n  " + e.name + " = " + (cv ? cv->value().toString() : std::string("?")) +
                   "  (" + (cv ? cvarTypeName(cv->type()) : "?") + ")  " + e.help;
        }
        return CommandResult{true, std::move(out)};
    });

    registerCommand("cvar.set", "set a cvar: cvar.set <name> <value>", [this](const std::vector<std::string>& args) {
        if (args.size() != 2)
            return CommandResult{false, "usage: cvar.set <name> <value>"};
        CVar* cv = findCVar(args[0]);
        if (!cv)
            return CommandResult{false, "unknown cvar: " + args[0]};
        if (!cv->setFromString(args[1]))
            return CommandResult{false, "invalid value for " + args[0] + ": expected " +
                                 std::string(cvarTypeName(cv->type())) + ", got '" + args[1] + "'"};
        return CommandResult{true, args[0] + " = " + cv->value().toString()};
    });
}

ExecResult Console::exec(const std::string& line) {
    appendLog(Severity::Info, "> " + line);

    std::string err;
    std::vector<std::string> tokens = tokenize(line, &err);
    if (!err.empty())
        return finish(ExecCode::BadRequest, "parse error: " + err, Severity::Error);
    if (tokens.empty())
        return ExecResult{ExecCode::Ok, ""};

    const std::string& name = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());

    // 统一注册表（UE 口径）：CVar 与命令同名不可能（分表注册时先查 CVar 再查命令）。
    if (CVar* cv = findCVar(name)) {
        if (args.empty()) {
            std::string out = name + " = " + cv->value().toString() +
                              "  (" + cvarTypeName(cv->type()) + ")  " + cv->help();
            return finish(ExecCode::Ok, std::move(out), Severity::Info);
        }
        if (args.size() > 1)
            return finish(ExecCode::BadRequest,
                          "usage: " + name + " <" + cvarTypeName(cv->type()) + ">  (" + cv->help() + ")",
                          Severity::Error);
        if (!cv->setFromString(args[0]))
            return finish(ExecCode::BadRequest,
                          "invalid value for " + name + ": expected " + cvarTypeName(cv->type()) +
                          ", got '" + args[0] + "'",
                          Severity::Error);
        std::string out = name + " = " + cv->value().toString();
        return finish(ExecCode::Ok, std::move(out), Severity::Info);
    }

    auto it = commands_.find(name);
    if (it == commands_.end())
        return finish(ExecCode::UnknownCommand, "unknown command: " + name, Severity::Error);

    CommandResult r = it->second.fn(args);
    if (r.ok)
        return finish(ExecCode::Ok, r.text, Severity::Info);
    return finish(ExecCode::CommandFailed, r.text, Severity::Error);
}

} // namespace console
} // namespace nl::core
