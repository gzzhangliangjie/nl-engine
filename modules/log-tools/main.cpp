// log-tools —— A 类日志轨道 M-LOG0/M-LOG1 无头验收工具（docs/p8/P8-LOG-PLAN.md）。
// selftest 断言面：
//   ① M0.1 兼容语义保形：sink 回调收原始 message / 级别闸 / 计数口径
//     （「被级别闸滤掉的 error 也计数」逐字保留）。
//   ② 文件落盘（Q-L2）：out/log/<exe>-<backend>-<时间戳>.log 存在且含 [nl:*] 锚点。
//   ③ M-LOG1 过滤指令：正向（全局缺省 + 通道覆盖 + off）与负向（非法指令
//     显式拒绝、原面不动），bevy EnvFilter 口径。
//   ④ 并发冒烟：多线程并发写不崩、计数不丢（spdlog mt sink + wrapper mutex）。
//   ⑤ M-LOG2 帧号注入：setLogFrame 后行尾 " #<n>" 后缀（sink 收原始 message 不带
//     后缀——帧号拼在 wrapper 行组装层，不进 sink 消息面）。
//   ⑥ M-LOG2 文件滚动：setFileRollLimit 注入小阈值，写超限后 <name>.1 存在。
// 退出码：0=PASS / 1=断言失败 / 2=用法错误。

#include "nl/core/console.hpp"
#include "nl/core/log.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <process.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [ok] " : "  [FAIL] ") << what << std::endl;
    if (!ok) ++g_failures;
}

bool fileContains(const std::string& path, const std::string& needle) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

// 整行「以 suffix 结尾」判定（getline 已剥换行；防 needle 子串误匹配行尾数字）。
bool fileHasLineEnding(const std::string& path, const std::string& suffix) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line))
        if (line.size() >= suffix.size()
                && line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    return false;
}

// 本进程唯一文本锚（ctest/regression 并行跑 selftest 时，同秒建的同前缀文件
// mtime 平局会让「最新文件」选错——PID 锚定只认本进程写的行）。
std::string pidTag() {
    return "pid" + std::to_string(_getpid());
}

// 在 out/log 里找含 needle 行的 log-tools-<backend>-*.log（excludeRotated 时
// 跳过滚动产物 <name>.1.log——它是滚动前旧内容，不是「当前文件」）。
std::string findFileContaining(const std::string& needle, bool excludeRotated) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists("out/log")) return {};
    for (const auto& e : fs::directory_iterator("out/log", ec)) {
        if (e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        if (name.rfind("log-tools-", 0) != 0) continue;
        if (excludeRotated && name.find(".1.log") != std::string::npos) continue;
        if (fileContains(e.path().string(), needle)) return e.path().string();
    }
    return {};
}

// 每个用例段前重置过滤面（避免用例间串扰；全局缺省回到出厂 Debug）。
void resetFilter() {
    nl::core::log::setFilter("nl:debug");
    nl::core::log::resetSink();
    nl::core::log::resetCounters();
}

void testCompatSemantics() {
    std::cout << "segment 1: M0.1 compat semantics" << std::endl;
    resetFilter();

    std::string captured;
    nl::core::log::setSink([&](nl::core::log::Level, const std::string& m) { captured += m; });
    nl::core::log::info("hello ", 42);
    check(captured == "hello 42", "sink callback receives raw message (no prefix)");

    captured.clear();
    nl::core::log::setLevel(nl::core::log::Level::Fatal);
    nl::core::log::info("muted info");
    check(captured.empty(), "level gate mutes info at Fatal");
    nl::core::log::error("counted while muted");
    check(nl::core::log::errorCount() == 1,
          "error counted even when muted (M0.1 counter semantics)");
    nl::core::log::setLevel(nl::core::log::Level::Debug);

    captured.clear();
    nl::core::log::deviceError("SELFTEST", "synthetic");
    check(captured.find("DEVICE/SELFTEST: synthetic") != std::string::npos,
          "deviceError formats as error");
    check(nl::core::log::deviceErrorCount() == 1 && nl::core::log::errorCount() == 2,
          "deviceErrorCount / errorCount independent counters");
    resetFilter();
}

void testFileSink() {
    std::cout << "segment 2: file sink (Q-L2)" << std::endl;
    resetFilter();
    const std::string tag = pidTag();
    nl::core::log::info("file-sink anchor line ", tag);
    const std::string path = findFileContaining("[nl:info] file-sink anchor line " + tag, false);
    check(!path.empty(), "log file created under out/log/ named <exe>-<backend>-<stamp>");
    if (!path.empty()) {
        std::cout << "  [file] " << path << std::endl;
        check(fileContains(path, "[nl:info] file-sink anchor line"),
              "file content carries [nl:info] anchor");
        check(path.find("-DX12-") != std::string::npos
              || path.find("-VULKAN-") != std::string::npos,
              "file name carries backend segment");
    }
    resetFilter();
}

void testFilterPositive() {
    std::cout << "segment 3: filter instruction (positive)" << std::endl;
    resetFilter();

    std::string captured;
    nl::core::log::setSink([&](nl::core::log::Level, const std::string& m) { captured += m; });

    check(nl::core::log::setFilter("nl:info,renderer=warn"),
          "valid multi-segment instruction accepted");
    check(nl::core::log::filterInstruction() == "nl:info,renderer=warn",
          "instruction round-trips in canonical form");
    nl::core::log::debug("global debug muted");          // 全局缺省 info -> debug 被闸
    nl::core::log::infoCh("renderer", "renderer info muted"); // renderer=warn -> info 被闸
    nl::core::log::warnCh("renderer", "renderer warn passes");
    nl::core::log::infoCh("scene", "scene info passes");  // 未覆盖通道跟随全局
    check(captured.find("global debug muted") == std::string::npos,
          "global debug muted by nl:info");
    check(captured.find("renderer info muted") == std::string::npos,
          "renderer info muted by renderer=warn");
    check(captured.find("[nl:warn|renderer] renderer warn passes") == 0
              || captured.find("renderer warn passes") != std::string::npos,
          "renderer warn passes");
    check(captured.find("scene info passes") != std::string::npos,
          "uncovered channel follows global default");

    captured.clear();
    check(nl::core::log::setFilter("renderer=off"), "off silences a channel");
    nl::core::log::fatalCh("renderer", "even fatal muted");
    check(captured.find("even fatal muted") == std::string::npos, "off silences renderer");
    resetFilter();
}

void testFilterNegative() {
    std::cout << "segment 4: filter instruction (negative, explicit errors)" << std::endl;
    resetFilter();
    check(nl::core::log::setFilter("nl:info,renderer=warn"),
          "baseline filter applied");

    const std::string before = nl::core::log::filterInstruction();

    const std::vector<std::string> bad = {
        "", "info=bogus", "Renderer=warn", "=warn", "renderer=", "renderer=warn,",
        "nl:info,,warn", "renderer=INFO",
    };
    for (const auto& instr : bad) {
        std::string err;
        const bool ok = nl::core::log::setFilter(instr, &err);
        check(!ok && !err.empty(),
              "rejected '" + (instr.empty() ? std::string("<empty>") : instr)
                  + "' with reason: " + (ok ? std::string("<accepted>") : err));
    }
    check(nl::core::log::filterInstruction() == before,
          "failed instructions leave the filter face untouched");
    resetFilter();
}

void testRuntimeChannelLevel() {
    std::cout << "segment 5: setChannelLevel runtime override" << std::endl;
    resetFilter();

    std::string captured;
    nl::core::log::setSink([&](nl::core::log::Level, const std::string& m) { captured += m; });
    nl::core::log::setChannelLevel("asset", nl::core::log::Level::Warn);
    nl::core::log::debugCh("asset", "muted debug");
    nl::core::log::errorCh("asset", "passed error");
    check(captured.find("muted debug") == std::string::npos
              && captured.find("passed error") != std::string::npos,
          "channel override gates per channel");
    resetFilter();
}

void testConcurrentSmoke() {
    std::cout << "segment 6: concurrency smoke (4 writers)" << std::endl;
    resetFilter();
    std::atomic<int> lines{0};
    nl::core::log::setSink([&](nl::core::log::Level, const std::string&) { ++lines; });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < 200; ++i)
                nl::core::log::debug("writer ", t, " line ", i);
        });
    for (auto& th : threads) th.join();
    check(lines.load() == 800, "800 concurrent lines all delivered, no tear/crash");
    check(nl::core::log::errorCount() == 0, "no errors during concurrent writes");
    resetFilter();
}

void testFrameContext() {
    std::cout << "segment 7: M-LOG2 frame context injection" << std::endl;
    resetFilter();

    // 帧号拼在行组装层：sink 仍收原始 message（不带后缀），stdout 行带 " #<n>"。
    const std::string tag = pidTag();
    std::string captured;
    nl::core::log::setSink([&](nl::core::log::Level, const std::string& m) { captured += m; });
    nl::core::log::setLogFrame(1234);
    nl::core::log::info("frame ctx line ", tag);
    check(captured == "frame ctx line " + tag,
          "sink message stays raw (frame suffix not in message)");

    // 文件路同格式：行尾 #1234（行内容 = [nl:info] ... #1234）。
    nl::core::log::infoCh("renderer", "frame ctx channel line ", tag);
    const std::string path = findFileContaining("frame ctx line " + tag, false);
    check(!path.empty(), "file exists for frame-suffix check");
    if (!path.empty()) {
        // channel 行 + 帧号后缀的组合形态
        check(fileContains(path, "[nl:info|renderer] frame ctx channel line " + tag + " #1234"),
              "channel line carries frame suffix in file");
        check(fileContains(path, "[nl:info] frame ctx line " + tag + " #1234"),
              "plain line carries frame suffix in file");
    }

    nl::core::log::clearLogFrame();
    nl::core::log::info("no frame ctx ", tag);
    // 整行尾匹配：确认行尾无 "#<n>"（fileContains 的子串语义挡不住 "#1234" 类尾迹）
    check(fileHasLineEnding(path, "[nl:info] no frame ctx " + tag),
          "clearLogFrame drops suffix");
    resetFilter();
}

void testFileRolling() {
    std::cout << "segment 8: M-LOG2 file rolling (size limit, one generation)" << std::endl;
    resetFilter();
    // 文件路懒建于首条 emit——注入小阈值必须在此之前。
    nl::core::log::setFileRollLimit(8 * 1024); // 8 KiB：几十行即滚
    check(nl::core::log::fileRollLimit() == 8 * 1024, "roll limit setter takes effect");

    // 打 ~64KB 行文（约 220 行 x 350B）触发滚动；PID 锚定本进程行
    //（历史轮/并行进程的 filler 行不认）。
    const std::string tag = pidTag();
    const std::string payload(300, 'x');
    for (int i = 0; i < 220; ++i)
        nl::core::log::info("roll filler ", i, " ", payload, " ", tag);

    namespace fs = std::filesystem;
    // 当前文件 = 含本进程最后一行 filler（219）的非滚动文件
    //（.1.log 是滚动前旧代，最高只到中段序号，且被排除）。
    const std::string cur = findFileContaining("roll filler 219 " + payload + " " + tag, true);
    check(!cur.empty(), "current log file alive after rolling");
    std::string rolled;
    if (!cur.empty()) {
        // spdlog rotating 原生命名：calc_filename("x.log", 1) = "x.1.log"
        //（数字插 basename 与扩展名之间，rotating_file_sink-inl.h:52 实读）。
        // 碰撞形态 x-2.log -> x-2.1.log 同规则。拍板口径「<name>.log.1」按实现
        // 名留档为「<name>.1.log（spdlog 原生）」。
        const std::string rotated = cur.substr(0, cur.size() - 4) + ".1.log";
        if (fs::exists(rotated)) rolled = rotated;
        check(!rolled.empty(), "rotated file <name>.1.log exists after exceeding limit");
        if (!rolled.empty()) {
            std::error_code ec;
            const auto sz1 = fs::file_size(cur, ec);
            const auto sz0 = fs::file_size(rolled, ec);
            // 一代保留语义：8KB 限下 220 行滚 ~9 次，.1 只装「最后一滚」的尾部段
            //（filler 0 早已被后续滚动覆盖），所以验证「含滚动前的 filler 行」即可。
            check(!ec && sz0 > 0 && fileContains(rolled, "roll filler"),
                  "rolled file carries pre-rotation content");
            std::cout << "  [roll] current=" << sz1 << "B rotated=" << sz0 << "B" << std::endl;
        }
    }
    // 不恢复默认上限：文件 sink 的 max_size 构造时固化（热重建会再开新文件），
    // 本进程即将退出；下一进程按默认 50 MiB 建，无跨进程状态。
    resetFilter();
}

void printUsage() {
    std::cout << "log-tools - nl-engine logging tools (A-class track, M-LOG0)\n"
                 "usage: log-tools selftest [--negative-only]\n"
                 "       log-tools demo [--filter <instruction>]\n"
                 "       log-tools exec <line> [<line>...]  (console log.* commands)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 2; }
    const std::string cmd = argv[1];

    if (cmd == "selftest") {
        // --negative-only：只跑负向段（ctest 最小负向卡）；无旗标跑全序 6 段。
        const bool negativeOnly = (argc >= 3 && std::string(argv[2]) == "--negative-only");
        if (negativeOnly) {
            testFilterNegative();
        } else {
            testCompatSemantics();
            testFileSink();
            testFilterPositive();
            testFilterNegative();
            testRuntimeChannelLevel();
            testConcurrentSmoke();
            testFrameContext();
            testFileRolling();
        }
        std::cout << "log selftest: " << (g_failures ? "FAIL" : "PASS")
                  << " (" << g_failures << " failures)" << std::endl;
        return g_failures ? 1 : 0;
    }

    if (cmd == "demo") {
        // 过滤指令实跑演示（tool 卡运行用例）：同一组行打两遍，中间应用 filter，
        // stdout 直接可见有/无差异。非法 filter -> exit 1（负向卡）。
        auto emitBatch = [](const char* tag) {
            nl::core::log::info("[" + std::string(tag) + "] plain info line");
            nl::core::log::debug("[" + std::string(tag) + "] plain debug line");
            nl::core::log::infoCh("renderer", "[" + std::string(tag) + "] renderer info");
            nl::core::log::warnCh("renderer", "[" + std::string(tag) + "] renderer warn");
            nl::core::log::infoCh("asset", "[" + std::string(tag) + "] asset info");
        };
        std::cout << "filter instruction (initial): " << nl::core::log::filterInstruction() << std::endl;
        emitBatch("before");
        if (argc >= 4 && std::string(argv[2]) == "--filter") {
            std::string err;
            if (!nl::core::log::setFilter(argv[3], &err)) {
                std::cout << "[nl:error] invalid filter: " << err << std::endl;
                return 1;
            }
            std::cout << "filter instruction (applied): " << nl::core::log::filterInstruction() << std::endl;
            emitBatch("after");
            return 0;
        }
        printUsage();
        return 2;
    }

    if (cmd == "exec") {
        // M-LOG2 拍板 ② 工具面验收：console log.* 四命令经 nl::core::Console
        // 无头执行（registerLogCommands 单一事实源，与 viewer --console-exec
        // 同一路径）。任一行失败 -> exit 1（ExecCode 1/2/3 都算失败）。
        if (argc < 3) { printUsage(); return 2; }
        nl::core::console::Console& c = nl::core::console::Console::get();
        c.installBuiltins();
        nl::core::console::registerLogCommands(c);
        int bad = 0;
        for (int i = 2; i < argc; ++i) {
            const auto r = c.exec(argv[i]);
            std::cout << r.output << std::endl;
            if (!r.ok()) bad = 1;
        }
        return bad;
    }

    printUsage();
    return 2;
}
