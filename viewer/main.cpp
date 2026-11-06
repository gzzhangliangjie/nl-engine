#include "nl/core/assert.hpp"
#include "nl/core/log.hpp"
#include "nl/core/window.hpp"
#include "nl/renderer/renderer.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool ok, const char* name) {
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << name << '\n';
    if (ok) ++g_passed; else ++g_failed;
}

// 无窗口、无 GPU 的机械验收用例：CI 里 `ctest` 直接跑
int runSelfTest() {
    std::cout << "nl-engine M0.1 selftest\n";

    nl::core::log::resetCounters();

    // 1. 日志格式化 + sink（M0.2 会把 NVRHI device callback 接成 sink）
    std::string captured;
    nl::core::log::setSink([&](nl::core::log::Level, const std::string& m) { captured += m; });
    nl::core::log::info("hello ", 42);
    nl::core::log::resetSink();
    check(captured == "hello 42", "log formatting + sink");
    check(nl::core::log::errorCount() == 0, "no errors on clean run");

    // 2. 设备错误独立计数（后续「调试层零报错」的机械判据）；静音避免误读为真故障
    nl::core::log::setLevel(nl::core::log::Level::Fatal);
    nl::core::log::deviceError("SELFTEST", "synthetic device error");
    nl::core::log::setLevel(nl::core::log::Level::Debug);
    check(nl::core::log::deviceErrorCount() == 1 && nl::core::log::errorCount() == 1,
          "device error counted separately");
    nl::core::log::resetCounters();

    // 3. 断言失败可被接管（调试层第一天就位的框架）
    // handler 是函数指针，用静态变量 + 无捕获 lambda
    static bool tripped = false;
    nl::core::setAssertHandler([](const char*, int, const char*, const char*) { tripped = true; });
    volatile int one = 1;
    NL_CHECK(one == 2);
    nl::core::setAssertHandler(nullptr);
    check(tripped, "assert failure captured");

    // 4. 窗口默认配置（resizable 是每阶段 resize 回归的前提）
    check(nl::core::windowSelfTest(), "window defaults (resizable)");

    // 5. renderer 只报告编译期选定的后端，未创建设备、未链接任何图形 API。
    //    同一份源码用 -DNL_GRAPHICS_BACKEND=DX12 / VULKAN 各编一遍，
    //    两行 backend 输出不同 = 「通用管线不绑死单一后端」的机械证据。
    const auto st = nl::renderer::status();
    std::cout << "  backend: " << st.backend << " (configured, no vendor SDK)\n";
    check(st.backend != nullptr && std::strlen(st.backend) > 0 && !st.deviceReady,
          "backend selected, no device yet");

    check(nl::core::log::errorCount() == 0, "selftest ends clean");

    std::cout << (g_failed == 0 ? "SELFTEST PASS (" : "SELFTEST FAIL (")
              << g_passed << '/' << (g_passed + g_failed) << ")\n";
    return g_failed == 0 ? 0 : 1;
}

// 可见用例：弹出窗口，标题栏实时显示帧号/帧时间，按 ESC 干净退出
int runWindow() {
    nl::core::WindowDesc desc;
    desc.title = "nl-engine | M0.1 | starting...";
    nl::core::Window window(desc);

    if (!window.valid()) {
        nl::core::log::error("window creation failed");
        return 1;
    }

    const auto st = nl::renderer::status();
    nl::core::log::info("renderer: ", st.backend, " / ", st.stage);
    nl::core::log::info("case: watch the title bar frame counter, then press ESC to quit");

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    std::uint64_t frame = 0;

    while (!window.shouldClose()) {
        window.pollEvents();
        if (window.keyPressed(nl::core::Key::Escape)) {
            nl::core::log::info("ESC -> exit");
            break;
        }

        const auto now = clock::now();
        const double dtMs = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        ++frame;

        if (frame % 15 == 0) {
            std::ostringstream title;
            title << "nl-engine | M0.1 | frame " << frame << " | "
                  << std::fixed << std::setprecision(1) << dtMs << " ms | ESC to quit";
            window.setTitle(title.str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    nl::core::log::info("shutdown ok (frames=", frame, ")");
    return nl::core::log::errorCount() == 0 ? 0 : 1;
}

void printUsage() {
    std::cout << "nl-engine viewer (M0.1)  backend=" << nl::renderer::status().backend << '\n'
              << "  viewer.exe            show window; ESC to quit\n"
              << "  viewer.exe --selftest headless checks, prints SELFTEST PASS (n/n)\n"
              << "  viewer.exe --help     this text\n"
              << "  backend is selected at configure time: -DNL_GRAPHICS_BACKEND=DX12|VULKAN\n";
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) return runSelfTest();
        if (std::strcmp(argv[i], "--help") == 0) { printUsage(); return 0; }
    }
    return runWindow();
}
