// log-tools —— A 类日志轨道 M-LOG0/M-LOG1 无头验收工具（docs/p8/P8-LOG-PLAN.md）。
// selftest 断言面：
//   ① M0.1 兼容语义保形：sink 回调收原始 message / 级别闸 / 计数口径
//     （「被级别闸滤掉的 error 也计数」逐字保留）。
//   ② 文件落盘（Q-L2）：out/log/<exe>-<backend>-<时间戳>.log 存在且含 [nl:*] 锚点。
//   ③ M-LOG1 过滤指令：正向（全局缺省 + 通道覆盖 + off）与负向（非法指令
//     显式拒绝、原面不动），bevy EnvFilter 口径。
//   ④ 并发冒烟：多线程并发写不崩、计数不丢（spdlog mt sink + wrapper mutex）。
// 退出码：0=PASS / 1=断言失败 / 2=用法错误。

#include "nl/core/log.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
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

std::string newestLogFile() {
    namespace fs = std::filesystem;
    std::string best;
    std::filesystem::file_time_type bestT{};
    std::error_code ec;
    if (!fs::exists("out/log")) return {};
    for (const auto& e : fs::directory_iterator("out/log", ec)) {
        if (e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        if (name.rfind("log-tools-", 0) != 0 || name.find(".log") == std::string::npos) continue;
        auto t = e.last_write_time(ec);
        if (!ec && (best.empty() || t > bestT)) { best = e.path().string(); bestT = t; }
    }
    return best;
}

bool fileContains(const std::string& path, const std::string& needle) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return true;
    return false;
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
    nl::core::log::info("file-sink anchor line");
    const std::string path = newestLogFile();
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

void printUsage() {
    std::cout << "log-tools - nl-engine logging tools (A-class track, M-LOG0)\n"
                 "usage: log-tools selftest [--negative-only]\n"
                 "       log-tools demo [--filter <instruction>]\n";
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

    printUsage();
    return 2;
}
