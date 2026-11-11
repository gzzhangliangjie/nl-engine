#pragma once
// P8 开发者控制台 M-C0：纯逻辑内核（A 类并行轨道，零 ImGui / 零图形符号）。
// 参考口径（AGENTS.md 决策前置三处证据）：
//   - UE IConsoleManager.h（只读对照）：CVar(bool/int/float/string) 与命令统一注册表、
//     help 文本必填、SetOnChangedCallback 变更回调、命令回调带 args。
//   - Carrot console/（从零节奏参照）：registerCommand + toggleVisibility 结构；其命令
//     不支持参数（源码 TODO 挂着），参数支持取 UE 口径；不做 RuntimeOption 二级体系
//     （CVar 直接覆盖其用途）。
//   - bevy：无等价实现（无内建控制台），据实标注。
//
// 边界（写死，防误用）：
//   - Console::get() 单例（UE IConsoleManager::Get / Carrot Console::instance 同款）；
//   - 非线程安全：注册与执行约定主线程（M-C1 ImGui 前端单线程驱动）；
//   - 执行反馈不进全局 log sink，留在自身环形日志（M-C1 滚动日志区直接消费）；
//   - 解析不支持转义序列：引号段内原样取字（v1 简化，够 CVar 赋值用）；
//   - 重复注册同名：CVar 保留现值仅更新 help；命令覆盖回调——均记一条 Warn，不 assert
//     （运行时模块热插拔注册是常态，assert 会把 M-C2 内建命令集建挂）。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nl::core {
namespace console {

// ---- CVar ----

enum class CVarType { Bool, Int, Float, String };

const char* cvarTypeName(CVarType type);   // "bool" / "int" / "float" / "string"

struct CVarValue {
    CVarType     type = CVarType::Bool;
    bool         b = false;
    std::int64_t i = 0;
    double       f = 0.0;
    std::string  s;

    std::string toString() const;   // bool→true/false；int/float→十进制；string→原样
};

class CVar {
public:
    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    CVarType type() const { return value_.type; }
    const CVarValue& value() const { return value_; }

    // 文本赋值（exec 路径）。类型不符或数字格式非法 → false（不改动现值）。
    // bool 接受 true/false/1/0/on/off/yes/no。
    bool setFromString(const std::string& text);

    // 类型化赋值（引擎代码路径）。类型不符是编程错误：静默不改值（不 assert，不炸热路径）。
    void setBool(bool v);
    void setInt(std::int64_t v);
    void setFloat(double v);
    void setString(std::string v);

    // UE SetOnChangedCallback 口径：Set 发生即回调（同值也触发，语义 = "Set 发生"）。
    void setOnChanged(std::function<void(const CVar&)> cb) { onChanged_ = std::move(cb); }

private:
    friend class Console;
    CVar(std::string name, CVarValue initial, std::string help);
    void assign(CVarValue v);   // 赋值 + 触发回调（类型已由调用方保证）

    std::string name_;
    std::string help_;
    CVarValue value_;
    std::function<void(const CVar&)> onChanged_;
};

// ---- 命令 / 执行 ----

struct CommandResult {
    bool        ok = true;
    std::string text;   // 成功输出或错误说明
};

// 命令回调：args 不含命令名本身（argv[0] = 命令名的约定不适用，终端习惯 args 从第一个参数起）。
using CommandFn = std::function<CommandResult(const std::vector<std::string>& args)>;

// 执行结果分档（工具版 expectExit 约定 0/1/2 的源头，M-C3 展示台直接映射退出码）：
//   Ok             = 0 成功（含 CVar 打印 / 赋值）
//   UnknownCommand = 1 注册表里没有这个名字
//   BadRequest     = 2 解析失败（未闭合引号）/ 参数个数不符 / 值与 CVar 类型不符
//   CommandFailed  = 3 命令回调自己报了失败
enum class ExecCode { Ok = 0, UnknownCommand = 1, BadRequest = 2, CommandFailed = 3 };

struct ExecResult {
    ExecCode   code = ExecCode::Ok;
    std::string output;
    bool ok() const { return code == ExecCode::Ok; }
    int exitCode() const { return static_cast<int>(code); }
};

// 滚动日志行（M-C1 前端按级别配色）。
enum class Severity { Info, Warn, Error };
struct LogLine {
    Severity    sev = Severity::Info;
    std::string text;
};

// 引号解析：'"...' 与 '\'...' 段内原样取字，可与裸段拼接成一个 token；
// 未闭合引号 → 返回空 vector 并置 *error（非空）。
std::vector<std::string> tokenize(const std::string& line, std::string* error = nullptr);

// ---- Console：统一注册表 + 执行 + 环形日志 ----

class Console {
public:
    static constexpr std::size_t kLogCap = 512;   // 环形上限，超过丢最旧

    static Console& get();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // ---- 注册（help 文本必填，UE 口径）----
    CVar& registerBool   (const std::string& name, bool         def, std::string help);
    CVar& registerInt    (const std::string& name, std::int64_t def, std::string help);
    CVar& registerFloat  (const std::string& name, double        def, std::string help);
    CVar& registerString (const std::string& name, std::string   def, std::string help);
    void  registerCommand(const std::string& name, std::string help, CommandFn fn);

    // 注销命令（M-SC1 脚本绑定引入：C# 注册命令在热重载/M-SC3 侧需要解绑）。
    // 未知命令返回 false；CVar 不在本表（findCVar 路径），不会误删。
    bool unregisterCommand(const std::string& name);

    // M-C2 内建命令集（UE ConsoleManager 承担 help/dump 的口径）：
    //   help           列全部命令（名字 + help）
    //   cvar.list      列全部 CVar（名字 = 现值 (类型) help）
    //   cvar.set <名> <值>  setFromString 赋值（与 `名字 值` 同路径，给脚本化执行用）
    // 幂等：重复调用只登记一次（内置命令被业务方覆盖后不复活）。
    void installBuiltins();

    CVar* findCVar(const std::string& name);

    // Tab 自动补全候选（前缀匹配，注册序=字典序），M-C1 前端消费。
    std::vector<std::string> completePrefix(const std::string& prefix) const;

    // help / cvar.list（M-C2 内建命令）消费的全量视图：name + help，按字典序。
    struct Entry { std::string name; std::string help; bool isCVar; };
    std::vector<Entry> allEntries() const;

    // ---- 解析 + 执行（结果同时进环形日志）----
    ExecResult exec(const std::string& line);

    // ---- 环形日志（M-C1 滚动区直接消费）----
    const std::vector<LogLine>& log() const { return log_; }
    void clearLog() { log_.clear(); }

private:
    Console() = default;
    CVar& registerCVar(const std::string& name, CVarValue def, std::string help);
    void appendLog(Severity sev, std::string text);
    ExecResult finish(ExecCode code, std::string output, Severity sev);
    bool builtinsInstalled_ = false;   // installBuiltins 幂等闸

    std::map<std::string, std::unique_ptr<CVar>> cvars_;
    struct CommandEntry { std::string help; CommandFn fn; };
    std::map<std::string, CommandEntry> commands_;
    std::vector<LogLine> log_;
};

// M-LOG2 拍板 ②（docs/p8/P8-LOG-PLAN.md §9）：log.* 四命令注册，单一事实源——
// viewer（ImGui 前端）与 log-tools（无头工具）共用同一实现，不各自复制。
//   log.level <level>            全局缺省（setFilter 单段语义）
//   log.channel <ch> <lv|off>    单通道调整（追加段走 setFilter 复用解析面）
//   log.filter <instruction...>  整面替换（bevy EnvFilter 口径）
//   log.status                   回读当前过滤指令
// 全部复用 nl::core::log 过滤面 API；非法指令显式报错（CommandFailed，exit 3）。
void registerLogCommands(Console& console);

} // namespace console
} // namespace nl::core
