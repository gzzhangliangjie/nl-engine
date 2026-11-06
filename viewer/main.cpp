#include "nl/core/assert.hpp"
#include "nl/core/log.hpp"
#include "nl/core/window.hpp"
#include "nl/renderer/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

const nl::renderer::ClearColor kColors[] = {
    {0.05f, 0.08f, 0.15f, 1.0f},  // 深蓝
    {0.95f, 0.35f, 0.10f, 1.0f},  // 橙
    {0.10f, 0.65f, 0.35f, 1.0f},  // 绿
    {0.45f, 0.20f, 0.80f, 1.0f},  // 紫
};
constexpr int kColorCount = static_cast<int>(sizeof(kColors) / sizeof(kColors[0]));

struct Options {
    bool selftest   = false;
    bool help       = false;
    bool debug      = true;
    bool screenshot = false;
    bool sizeSet    = false;
    bool resize     = false;
    int  width      = 1280;
    int  height     = 720;
    int  resizeWidth  = 0;
    int  resizeHeight = 0;
    int  color      = 0;
    int  frames     = 8;
    std::string screenshotPath;
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") o.selftest = true;
        else if (a == "--help") o.help = true;
        else if (a == "--no-debug") o.debug = false;
        else if (a == "--screenshot" && i + 1 < argc) {
            o.screenshot = true;
            o.screenshotPath = argv[++i];
        } else if (a == "--color" && i + 1 < argc) {
            o.color = std::atoi(argv[++i]) % kColorCount;
        } else if (a == "--frames" && i + 1 < argc) {
            o.frames = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                o.width = w; o.height = h; o.sizeSet = true;
            }
        } else if (a == "--resize" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                o.resize = true; o.resizeWidth = w; o.resizeHeight = h;
            }
        }
    }
    // 截图模式默认小窗口：两后端 PNG 逐字节对照，跑得快、文件小。
    if (o.screenshot && !o.sizeSet) { o.width = 640; o.height = 360; }
    if (o.frames < 1) o.frames = 1;
    return o;
}

// 无窗口、无 GPU 的机械验收用例：CI 里 ctest 直接跑
int runSelfTest() {
    std::cout << "nl-engine M0.2 selftest\n";

    nl::core::log::resetCounters();

    std::string captured;
    nl::core::log::setSink([&](nl::core::log::Level, const std::string& m) { captured += m; });
    nl::core::log::info("hello ", 42);
    nl::core::log::resetSink();
    check(captured == "hello 42", "log formatting + sink");
    check(nl::core::log::errorCount() == 0, "no errors on clean run");

    nl::core::log::setLevel(nl::core::log::Level::Fatal);
    nl::core::log::deviceError("SELFTEST", "synthetic device error");
    nl::core::log::setLevel(nl::core::log::Level::Debug);
    check(nl::core::log::deviceErrorCount() == 1 && nl::core::log::errorCount() == 1,
          "device error counted separately");
    nl::core::log::resetCounters();

    static bool tripped = false;
    nl::core::setAssertHandler([](const char*, int, const char*, const char*) { tripped = true; });
    volatile int one = 1;
    NL_CHECK(one == 2);
    nl::core::setAssertHandler(nullptr);
    check(tripped, "assert failure captured");

    check(nl::core::windowSelfTest(), "window defaults (resizable)");

    const auto st = nl::renderer::status();
    std::cout << "  backend: " << st.backend << " (configured, no vendor SDK)\n"
              << "  stage:   " << st.stage << '\n';
    check(st.backend != nullptr && std::strlen(st.backend) > 0 && !st.deviceReady,
          "backend selected");
    check(std::strstr(st.stage, "M0.3") != nullptr, "M0.3 stage reported");

    // 空窗口句柄必须安全失败，不能崩、不能假装有设备
    auto bad = nl::renderer::Renderer::create(nullptr, {640, 360}, false);
    check(!bad->valid() && !bad->lastError().empty(), "renderer rejects null window safely");

    check(nl::core::log::errorCount() == 0, "selftest ends clean");

    std::cout << (g_failed == 0 ? "SELFTEST PASS (" : "SELFTEST FAIL (")
              << g_passed << '/' << (g_passed + g_failed) << ")\n";
    return g_failed == 0 ? 0 : 1;
}

bool prepareScreenshotDirs(const std::string& path, std::string& outError) {
    std::error_code ec;
    const auto parent = std::filesystem::absolute(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) { outError = "cannot create screenshot dir: " + ec.message(); return false; }
    return true;
}

// 可见用例：窗口清屏色按空格切换、F12/--screenshot 真回读 GPU 像素写 PNG、缩放不黑屏。
int runWindow(const Options& o) {
    nl::core::log::resetCounters();

    nl::core::WindowDesc desc;
    desc.width  = o.width;
    desc.height = o.height;
    desc.title  = "nl-engine | M0.2 | starting...";
    nl::core::Window window(desc);
    if (!window.valid()) {
        nl::core::log::error("window creation failed");
        return 1;
    }

    const auto nativeSize = window.framebufferSize();
    const nl::renderer::Extent extent{
        static_cast<std::uint32_t>(nativeSize.width),
        static_cast<std::uint32_t>(nativeSize.height)
    };
    auto renderer = nl::renderer::Renderer::create(window.nativeHandle(), extent, o.debug);
    if (!renderer->valid()) {
        nl::core::log::error("renderer init failed: ", renderer->lastError());
        return 1;
    }

    const auto st = nl::renderer::status();
    const auto& info = renderer->info();
    nl::core::log::info("backend: ", st.backend, " | ", info.adapterName);
    nl::core::log::info("debug layer: ", info.debugLayer,
                        " | active=", info.debugLayerActive ? "yes" : "no");
    nl::core::log::info("features: rayQuery=", info.rayQuery ? 1 : 0,
                        " shaderInt64Atomics=", info.shaderInt64Atomics ? 1 : 0,
                        " indirectDraw=", info.indirectDraw ? 1 : 0);
    nl::core::log::info("case: SPACE changes clear color, F12 saves PNG, resize must not go black");

    if (o.screenshot) {
        std::string err;
        if (!prepareScreenshotDirs(o.screenshotPath, err)) {
            nl::core::log::error(err);
            return 1;
        }
        nl::core::log::info("screenshot mode: ", o.frames, " frames -> ",
                            std::filesystem::absolute(o.screenshotPath).string());
    }

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    std::uint64_t frame = 0;
    int colorIndex = o.color;
    int screenshotsSaved = 0;
    bool failed = false;
    const std::uint64_t resizeFrame =
        (o.resize && o.frames > 2) ? std::max<std::uint64_t>(2, o.frames / 2) : 0;

    while (!window.shouldClose()) {
        window.pollEvents();
        if (window.keyPressed(nl::core::Key::Escape)) {
            nl::core::log::info("ESC -> exit");
            break;
        }
        if (window.keyPressed(nl::core::Key::Space)) {
            colorIndex = (colorIndex + 1) % kColorCount;
            nl::core::log::info("clear color -> ", colorIndex);
        }

        const char* capturePath = nullptr;
        std::string manualPath;
        std::string beforePath;
        if (window.keyPressed(nl::core::Key::F12)) {
            manualPath = "docs/screenshots/m0.2-" + std::string(st.backend) + "-manual.png";
            std::string err;
            if (prepareScreenshotDirs(manualPath, err)) capturePath = manualPath.c_str();
        }
        // resize 回归：缩放前先存一张，最后一帧再存一张，前后对比不黑屏
        if (o.screenshot && resizeFrame != 0 && frame + 1 == resizeFrame) {
            beforePath = o.screenshotPath + ".before.png";
            capturePath = beforePath.c_str();
        }
        if (o.screenshot && frame + 1 == static_cast<std::uint64_t>(o.frames)) {
            capturePath = o.screenshotPath.c_str();
        }

        nl::core::Extent resized{};
        if (window.consumeResize(resized) && resized.width > 0 && resized.height > 0) {
            renderer->resize({static_cast<std::uint32_t>(resized.width),
                              static_cast<std::uint32_t>(resized.height)});
        }
        if (resizeFrame != 0 && frame + 1 == resizeFrame) {
            window.setSize(o.resizeWidth, o.resizeHeight);
            nl::core::log::info("resize case: request ", o.resizeWidth, "x", o.resizeHeight);
        }

        const auto now = clock::now();
        const double dtMs = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        ++frame;

        if (!renderer->renderFrame(kColors[colorIndex], capturePath)) {
            nl::core::log::error("renderFrame failed: ", renderer->lastError());
            failed = true;
            break;
        }
        renderer->drainMessages();
        if (capturePath) {
            ++screenshotsSaved;
            nl::core::log::info("screenshot written: ", capturePath);
        }

        if (frame % 15 == 0 || o.screenshot) {
            const auto e = renderer->extent();
            std::ostringstream title;
            title << "nl-engine | M0.2 | " << st.backend
                  << " | frame " << frame
                  << " | " << std::fixed << std::setprecision(1) << dtMs << " ms"
                  << " | color " << colorIndex
                  << " | " << e.width << "x" << e.height;
            window.setTitle(title.str());
        }

        if (o.screenshot && frame >= static_cast<std::uint64_t>(o.frames)) break;
        if (!o.screenshot) std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    nl::core::log::info("shutdown ok (frames=", frame,
                        " screenshots=", screenshotsSaved,
                        " deviceErrors=", nl::core::log::deviceErrorCount(), ")");

    if (o.screenshot && screenshotsSaved == 0) {
        nl::core::log::error("screenshot was not written");
        failed = true;
    }
    if (nl::core::log::deviceErrorCount() != 0 || nl::core::log::errorCount() != 0) {
        nl::core::log::error("acceptance failed: device/error counters must be zero");
        failed = true;
    }
    return failed ? 1 : 0;
}

void printUsage() {
    std::cout << "nl-engine viewer (M0.2)  backend=" << nl::renderer::status().backend << '\n'
              << "  viewer.exe                         window; ESC quit, SPACE color, F12 PNG\n"
              << "  viewer.exe --selftest              headless checks\n"
              << "  viewer.exe --screenshot <path>     render N frames, read back GPU pixels to PNG\n"
              << "  viewer.exe --frames N              frames before screenshot (default 8)\n"
              << "  viewer.exe --color 0..3            initial clear color (default 0)\n"
              << "  viewer.exe --size WxH              window size (default 1280x720; screenshot 640x360)\n"
              << "  viewer.exe --resize WxH            mid-run resize regression; saves <path>.before.png too\n"
              << "  viewer.exe --no-debug              disable current backend debug layer\n"
              << "  viewer.exe --help                  this text\n"
              << "  backend at configure time: -DNL_GRAPHICS_BACKEND=DX12|VULKAN\n";
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parseArgs(argc, argv);
    if (o.selftest) return runSelfTest();
    if (o.help) { printUsage(); return 0; }
    return runWindow(o);
}
