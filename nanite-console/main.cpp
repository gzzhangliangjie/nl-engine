// P8 M-C0：开发者控制台工具（A 类轨道，工具版交代）。
//   nanite-console selftest          —— core/console 纯逻辑内核机械单测（全断言，不抽样）
//   nanite-console exec "<line>"...  —— demo 注册表上执行命令行（退出码 = ExecCode，0/1/2/3）
// 只链 nl_core，零图形 API（AGENTS.md：图形符号只在 renderer/）。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nl/core/console.hpp"

using namespace nl::core::console;

static int failures = 0;
static void check(bool ok, const char* name, const std::string& detail = {}) {
    if (ok) std::printf("  ok   %s\n", name);
    else { std::printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str()); ++failures; }
}

// ---- selftest：注册 / CVar 读写 / 解析 / 执行 / 未知命令 / 变更回调 / 日志 ----
// 注意：Console 是单例，selftest 注册名全部带 st. 前缀避免与 exec demo 注册表撞名。

static int selftestConsole() {
    Console& c = Console::get();
    c.clearLog();

    std::printf("console selftest:\n");

    // 1) 注册 + help 必填 + allEntries 视图
    CVar& b = c.registerBool("st.enabled", false, "selftest bool cvar");
    CVar& i = c.registerInt("st.count", 3, "selftest int cvar");
    CVar& f = c.registerFloat("st.fov", 60.0, "selftest float cvar");
    CVar& s = c.registerString("st.name", "default", "selftest string cvar");
    c.registerCommand("st.echo", "print args joined", [](const std::vector<std::string>& args) {
        std::string out;
        for (std::size_t k = 0; k < args.size(); ++k) { if (k) out += ' '; out += args[k]; }
        return CommandResult{true, "echo: " + out};
    });
    c.registerCommand("st.fail", "always fails", [](const std::vector<std::string>&) {
        return CommandResult{false, "st.fail deliberately failed"};
    });
    check(c.findCVar("st.enabled") == &b, "register: cvar lookup");
    check(c.findCVar("st.no-such") == nullptr, "register: missing lookup null");
    auto entries = c.allEntries();
    std::size_t stCount = 0;
    for (const auto& e : entries) if (e.name.rfind("st.", 0) == 0) ++stCount;
    check(stCount == 6, "register: 6 st.* entries in allEntries", "got " + std::to_string(stCount));
    check(b.help() == "selftest bool cvar", "register: help stored");

    // 2) CVar 类型化读写 + toString
    b.setBool(true);
    i.setInt(42);
    f.setFloat(90.5);
    s.setString("hello world");
    check(b.value().b == true, "cvar: setBool/read");
    check(i.value().i == 42, "cvar: setInt/read");
    check(f.value().f == 90.5, "cvar: setFloat/read");
    check(s.value().s == "hello world", "cvar: setString/read");
    check(b.value().toString() == "true" && i.value().toString() == "42" &&
          f.value().toString() == "90.5" && s.value().toString() == "hello world",
          "cvar: toString per type");
    check(i.type() == CVarType::Int && f.type() == CVarType::Float, "cvar: type()");
    // 跨类型 typed setter 是编程错误：静默不改值
    i.setInt(-7);
    i.setBool(false);
    check(i.value().i == -7, "cvar: wrong-type typed setter no-op");

    // 3) setFromString 正向 + 负向
    check(b.setFromString("off") && b.value().b == false, "cvar: setFromString bool 'off'");
    check(b.setFromString("1") && b.value().b == true, "cvar: setFromString bool '1'");
    check(i.setFromString("-17") && i.value().i == -17, "cvar: setFromString int negative");
    check(!i.setFromString("3.5") && i.value().i == -17, "cvar: int rejects '3.5' (value kept)");
    check(!i.setFromString("abc"), "cvar: int rejects 'abc'");
    check(f.setFromString("0.25") && f.value().f == 0.25, "cvar: setFromString float");
    check(!f.setFromString("xyz") && f.value().f == 0.25, "cvar: float rejects 'xyz' (value kept)");
    check(s.setFromString("with space") && s.value().s == "with space", "cvar: setFromString string");

    // 4) tokenize：引号 / 多空白 / 未闭合
    std::string terr;
    auto t1 = tokenize("cmd a \"b c\" d", &terr);
    check(terr.empty() && t1.size() == 4 && t1[2] == "b c", "tokenize: quoted arg keeps spaces");
    auto t2 = tokenize("  spaced\t\tout  ", &terr);
    check(terr.empty() && t2.size() == 2 && t2[0] == "spaced" && t2[1] == "out", "tokenize: whitespace collapse");
    auto t3 = tokenize("p \"a b\"'c d'e", &terr);
    check(terr.empty() && t3.size() == 2 && t3[1] == "a bc de", "tokenize: quote segments concatenate");
    auto t4 = tokenize("cmd \"unclosed", &terr);
    check(!terr.empty() && t4.empty(), "tokenize: unterminated quote is error");
    auto t5 = tokenize("", &terr);
    check(terr.empty() && t5.empty(), "tokenize: empty line");

    // 5) exec：CVar 打印 / 赋值 / 命令带参 / 未知命令 / 错误反馈
    ExecResult r1 = c.exec("st.count");
    check(r1.ok() && r1.output.find("-17") != std::string::npos &&
          r1.output.find("int") != std::string::npos, "exec: cvar print value+type");
    ExecResult r2 = c.exec("st.count 100");
    check(r2.ok() && i.value().i == 100, "exec: cvar set via string");
    ExecResult r3 = c.exec("st.fov notanumber");
    check(r3.exitCode() == static_cast<int>(ExecCode::BadRequest), "exec: bad value -> BadRequest(2)");
    check(f.value().f == 0.25, "exec: failed set keeps old value");
    ExecResult r4 = c.exec("st.echo one \"two three\" four");
    check(r4.ok() && r4.output == "echo: one two three four", "exec: command receives quoted args");
    ExecResult r5 = c.exec("st.no-such");
    check(r5.exitCode() == static_cast<int>(ExecCode::UnknownCommand), "exec: unknown -> UnknownCommand(1)");
    ExecResult r6 = c.exec("st.fail");
    check(r6.exitCode() == static_cast<int>(ExecCode::CommandFailed), "exec: command fail -> CommandFailed(3)");
    ExecResult r7 = c.exec("st.name \"unclosed");
    check(r7.exitCode() == static_cast<int>(ExecCode::BadRequest), "exec: unterminated quote -> BadRequest(2)");
    ExecResult r8 = c.exec("st.enabled true junk");
    check(r8.exitCode() == static_cast<int>(ExecCode::BadRequest), "exec: cvar extra args -> BadRequest(2)");
    ExecResult r9 = c.exec("   ");
    check(r9.ok(), "exec: blank line is no-op Ok");

    // 6) 变更回调（UE SetOnChangedCallback 口径）
    int fired = 0;
    std::int64_t seen = 0;
    i.setOnChanged([&](const CVar& cv) { ++fired; seen = cv.value().i; });
    c.exec("st.count 7");
    check(fired == 1 && seen == 7, "cvar: onChanged fired on exec set");
    i.setFromString("8");
    check(fired == 2 && seen == 8, "cvar: onChanged fired on setFromString");

    // 7) 补全（M-C1 Tab 消费的前端契约）+ 日志 + 清空
    auto comp = c.completePrefix("st.");
    check(comp.size() == 6, "complete: prefix 'st.' -> 6", "got " + std::to_string(comp.size()));
    auto comp2 = c.completePrefix("st.e");
    check(comp2.size() == 2 && comp2[0] == "st.echo" && comp2[1] == "st.enabled",
          "complete: prefix 'st.e' -> echo+enabled sorted");
    auto comp3 = c.completePrefix("st.zzz");
    check(comp3.empty(), "complete: no match -> empty");
    check(c.log().size() >= 12, "log: exec lines recorded", "got " + std::to_string(c.log().size()));
    bool hasUnknown = false;
    for (const auto& l : c.log()) if (l.sev == Severity::Error && l.text.find("unknown command") != std::string::npos) hasUnknown = true;
    check(hasUnknown, "log: unknown command recorded as Error");
    c.clearLog();
    check(c.log().empty(), "log: clearLog");

    // 8) 内建命令集（M-C2：help / cvar.list / cvar.set，UE ConsoleManager 内建口径）
    c.installBuiltins();
    c.installBuiltins();   // 幂等：二次调用不得产生重复注册副作用
    ExecResult b1 = c.exec("help");
    check(b1.ok() && b1.output.find("st.echo") != std::string::npos &&
          b1.output.find("cvar.set") != std::string::npos, "builtins: help lists commands");
    ExecResult b2 = c.exec("cvar.list");
    check(b2.ok() && b2.output.find("st.count = 8") != std::string::npos &&
          b2.output.find("st.fov") != std::string::npos, "builtins: cvar.list shows names+values");
    ExecResult b3 = c.exec("cvar.set st.count 55");
    check(b3.ok() && i.value().i == 55, "builtins: cvar.set assigns via setFromString");
    ExecResult b4 = c.exec("cvar.set st.fov xyz");
    check(b4.exitCode() == static_cast<int>(ExecCode::CommandFailed) && f.value().f == 0.25,
          "builtins: cvar.set bad value -> fail, old value kept");
    ExecResult b5 = c.exec("cvar.set st.no-such 1");
    check(b5.exitCode() == static_cast<int>(ExecCode::CommandFailed), "builtins: cvar.set unknown cvar -> fail");
    ExecResult b6 = c.exec("cvar.set st.count");
    check(b6.exitCode() == static_cast<int>(ExecCode::CommandFailed), "builtins: cvar.set missing value -> usage fail");
    check(c.completePrefix("cvar.").size() == 2, "builtins: double-install no dup registrations");

    if (failures == 0) { std::printf("console selftest: PASS\n"); return 0; }
    std::printf("console selftest: FAIL (%d case(s))\n", failures);
    return 1;
}

// ---- exec demo 注册表：M-C3 展示台 usage / 正向 / 负向用例的真实执行体 ----

static void registerDemo() {
    Console& c = Console::get();
    c.installBuiltins();   // M-C2：demo 注册表也挂内建 help / cvar.list / cvar.set（M-C3 展示台用例用）
    c.registerBool("demo.enabled", true, "demo bool cvar");
    c.registerInt("demo.count", 3, "demo int cvar");
    c.registerFloat("demo.fov", 60.0, "demo fov cvar");
    c.registerString("demo.name", "nl-engine", "demo string cvar");
    c.registerCommand("echo", "print args joined", [](const std::vector<std::string>& args) {
        std::string out;
        for (std::size_t k = 0; k < args.size(); ++k) { if (k) out += ' '; out += args[k]; }
        return CommandResult{true, "echo: " + out};
    });
    c.registerCommand("add", "sum float args", [](const std::vector<std::string>& args) {
        if (args.empty()) return CommandResult{false, "usage: add <float>..."};
        double sum = 0.0;
        for (const auto& a : args) {
            char* end = nullptr;
            double v = std::strtod(a.c_str(), &end);
            if (end == a.c_str() || *end != '\0') return CommandResult{false, "add: not a number: " + a};
            sum += v;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", sum);
        return CommandResult{true, std::string("add: ") + buf};
    });
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "selftest")
        return selftestConsole();

    if (argc >= 2 && std::string(argv[1]) == "exec") {
        if (argc < 3) {
            std::printf("usage: nanite-console exec \"<command line>\" [...]\n");
            std::printf("  demo registry: demo.* cvars + echo/add commands\n");
            std::printf("  exit code = ExecCode (0 ok / 1 unknown / 2 bad request / 3 command failed)\n");
            return 2;
        }
        registerDemo();
        int worst = 0;
        for (int k = 2; k < argc; ++k) {
            ExecResult r = Console::get().exec(argv[k]);
            std::printf("[%d] %s\n", r.exitCode(), r.output.empty() ? "(ok)" : r.output.c_str());
            if (r.exitCode() > worst) worst = r.exitCode();
        }
        return worst;
    }

    std::printf("usage:\n"
                "  nanite-console selftest              deterministic unit tests\n"
                "  nanite-console exec \"<line>\" [...]   run lines on demo registry\n");
    return 2;
}
