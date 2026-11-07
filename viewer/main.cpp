#include "nl/core/assert.hpp"
#include "nl/core/camera.hpp"
#include "nl/core/log.hpp"
#include "nl/core/modes.hpp"
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

// M0.8：调试着色模式（与 model.slang 的 g_options.x / HUD combo 一致）
// M1.3：4 = cluster（只有 cluster 场景提供了 CLUSTERCOLOR 属性；normal 场景选它会
// 读到未绑定属性的未定义值，所以 N 键循环与 HUD 回写时在 normal 模式下钳到 0..3）
constexpr int kShadingCount = 5;
const char* shadingName(int s) {
    static const char* kNames[kShadingCount] =
        { "textured", "normal", "uv", "tri-id", "cluster" };
    return (s >= 0 && s < kShadingCount) ? kNames[s] : "?";
}

// --screenshot 回归模式下 HUD 的时序样本固定：实时 FPS 每次都不同，写进 PNG
// 就会让基线 MD5 永远对不上。交互窗口里显示的是真实采样。
constexpr int kFixedSampleCount = 8;
const float kFixedSamples[kFixedSampleCount] = {
    16.6f, 16.8f, 16.5f, 16.9f, 16.7f, 16.6f, 17.1f, 16.4f
};

// M0.6：GPU 耗时的固定样本，与 CPU 样本独立——截图回归模式下真实 GPU 计时
// 每次运行都不同，写进 PNG 会毁掉基线 MD5，故固定并在面板标 fixed samples。
const float kFixedGpuSamples[kFixedSampleCount] = {
    0.62f, 0.64f, 0.61f, 0.66f, 0.63f, 0.62f, 0.68f, 0.60f
};

struct Options {
    bool selftest   = false;
    bool help       = false;
    bool debug      = true;
    bool screenshot = false;
    bool sizeSet    = false;
    bool resize     = false;
    bool cullBack   = true;   // C 键 / --cull / HUD 复选框控制
    int  shading    = 0;      // M0.8：0 textured / 1 normal / 2 uv / 3 tri-id；M1.3：4 cluster（N 键 / HUD combo）
    bool wireframe  = false;  // M0.8：线框（V 键 / --wireframe / HUD 复选框）
    bool hudSet     = false;  // 用户显式给了 --hud / --no-hud
    bool hudEnabled = false;  // 最终值：交互默认开，截图回归默认关
    int  width      = 1280;
    int  height     = 720;
    int  resizeWidth  = 0;
    int  resizeHeight = 0;
    int  color      = 0;
    int  frames     = 8;
    float yawDeg    = 12.0f;  // 与 OrbitCamera 默认一致，默认正脸、截图可复现
    float pitchDeg  = 9.2f;
    float distance  = 4.2f;
    std::string screenshotPath;
    std::string modelFile = "Suzanne.glb";  // M0.7：--model 选 assets/ 下的资产
    bool modelSet = false;                  // M1.3：cluster 模式缺省模型是 Suzanne.nmesh
    std::string modeText = "normal";
    nl::core::ViewMode mode = nl::core::ViewMode::Normal;
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") o.selftest = true;
        else if (a == "--help") o.help = true;
        else if (a == "--no-debug") o.debug = false;
        else if (a == "--hud") { o.hudSet = true; o.hudEnabled = true; }
        else if (a == "--no-hud") { o.hudSet = true; o.hudEnabled = false; }
        else if (a == "--mode" && i + 1 < argc) o.modeText = argv[++i];
        else if (a == "--model" && i + 1 < argc) {
            o.modelFile = argv[++i];
            o.modelSet = true;
        }
        else if (a == "--cull" && i + 1 < argc) {
            const std::string v = argv[++i];
            o.cullBack = (v != "none" && v != "off" && v != "0");
        } else if (a == "--shading" && i + 1 < argc) {
            const std::string v = argv[++i];
            o.shading = -1;
            for (int s = 0; s < kShadingCount; ++s)
                if (v == shadingName(s)) o.shading = s;
            if (o.shading < 0) {
                // 也接受 0..3；除此之外一律报错退出，绝不静默落回 textured
                char* end = nullptr;
                const long n = std::strtol(v.c_str(), &end, 10);
                if (end && *end == '\0' && n >= 0 && n < kShadingCount) o.shading = static_cast<int>(n);
            }
            if (o.shading < 0 || o.shading >= kShadingCount) {
                std::cerr << "unknown --shading '" << v << "'. available: "
                          << "textured, normal, uv, tri-id\n";
                std::exit(1);
            }
        } else if (a == "--wireframe") {
            o.wireframe = true;
        } else if (a == "--yaw" && i + 1 < argc) {
            o.yawDeg = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--pitch" && i + 1 < argc) {
            o.pitchDeg = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--dist" && i + 1 < argc) {
            o.distance = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--screenshot" && i + 1 < argc) {
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
    // 截图模式默认小窗口：两后端 PNG 逐像素对照，跑得快、文件小。
    if (o.screenshot && !o.sizeSet) { o.width = 640; o.height = 360; }
    if (o.frames < 1) o.frames = 1;
    if (!o.hudSet) o.hudEnabled = !o.screenshot;
    return o;
}

// 无窗口、无 GPU 的机械验收用例：CI 里 ctest 直接跑
int runSelfTest() {
    std::cout << "nl-engine M0.9 selftest\n";

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
    check(nl::core::cameraSelfTest(), "orbit camera matrices, zoom clamps, reset");
    check(nl::core::modeSelfTest(), "view mode dispatcher (normal + cluster in S1)");

    const auto st = nl::renderer::status();
    std::cout << "  backend: " << st.backend << " (configured, no vendor SDK)\n"
              << "  stage:   " << st.stage << '\n';
    check(st.backend != nullptr && std::strlen(st.backend) > 0 && !st.deviceReady,
          "backend selected");
    check(std::strstr(st.stage, "S1.0") != nullptr, "S1.0 stage reported");

    nl::renderer::FrameParams fp;
    check(fp.cullBack && sizeof(fp.mvp) == 64 && fp.lightDir[3] > 0.0f,
          "frame params (MVP/cull/light) POD defaults");
    check(fp.shading == 0 && !fp.wireframe,
          "M0.8 debug shading / wireframe POD defaults (= M0.7 behavior)");
    check(nl::renderer::hudSelfTest(), "HUD POD + projection arithmetic");
    std::string shaderReport;
    check(nl::renderer::shaderArtifactsSelfTest(shaderReport),
          "Slang single-source -> SPIR-V + DXIL (12 artifacts)");
    std::cout << "  shaders:" << shaderReport << '\n';

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

// 可见用例：带贴图 Suzanne + 轨道相机 +（默认）ImGui HUD；HUD 控件能改剔除 /
// 清屏色 / 重置相机；F12/--screenshot 真回读 GPU 像素写 PNG；resize 不黑屏。
// M1.3：cluster=true 时走 cluster 场景（assets/meshlets/*.nmesh，每簇随机着色），
// 缺省着色 4=cluster；簇色烘在 CPU，两后端截图逐字节一致。
int runNormal(const Options& o, bool cluster = false) {
    nl::core::log::resetCounters();

    nl::core::WindowDesc desc;
    desc.width  = o.width;
    desc.height = o.height;
    desc.title  = "nl-engine | starting...";
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
    auto renderer = nl::renderer::Renderer::create(window.nativeHandle(), extent, o.debug,
                                                   o.modelFile.c_str(), cluster);
    if (!renderer->valid()) {
        nl::core::log::error("renderer init failed: ", renderer->lastError());
        return 1;
    }

    nl::core::OrbitCamera camera;
    camera.setAspect(static_cast<float>(extent.width) / static_cast<float>(extent.height));
    camera.setOrbit(o.yawDeg, o.pitchDeg, o.distance);
    bool cullBack = o.cullBack;
    int shading = cluster ? 4 : o.shading;   // M1.3：cluster 模式缺省看簇色
    bool wireframe = o.wireframe;

    const auto st = nl::renderer::status();
    const auto& info = renderer->info();
    nl::core::log::info("backend: ", st.backend, " | ", info.adapterName);
    nl::core::log::info("debug layer: ", info.debugLayer,
                        " | active=", info.debugLayerActive ? "yes" : "no");
    nl::core::log::info("mode: ", nl::core::modeName(o.mode),
                        " | HUD: ", o.hudEnabled ? "on" : "off",
                        " | shading: ", shadingName(shading), wireframe ? " | wireframe" : "",
                        " | case: drag=orbit, wheel=zoom, C=culling, V=wire, N=shading, R=reset, SPACE=clear, F12=PNG");

    if (o.screenshot) {
        std::string err;
        if (!prepareScreenshotDirs(o.screenshotPath, err)) {
            nl::core::log::error(err);
            return 1;
        }
        nl::core::log::info("screenshot mode: ", o.frames, " frames -> ",
                            std::filesystem::absolute(o.screenshotPath).string(),
                            " | cull=", cullBack ? "back" : "none",
                            " | hud=", o.hudEnabled ? "on" : "off");
    }

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    std::uint64_t frame = 0;
    int colorIndex = o.color;
    int screenshotsSaved = 0;
    bool failed = false;
    bool hudWantsMouse = false;   // 上一帧 HUD 是否占着鼠标（避免拖面板时转相机）
    float frameHistory[120] = {};
    int frameHistoryCount = 0;
    float gpuHistory[120] = {};    // M0.6：GPU ms 曲线（renderFrame 后由 renderer 写回值追加）
    int gpuHistoryCount = 0;
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
        if (window.keyPressed(nl::core::Key::C)) {
            cullBack = !cullBack;
            nl::core::log::info("back-face culling -> ", cullBack ? "on" : "off (inner wall visible)");
        }
        if (window.keyPressed(nl::core::Key::V)) {   // M0.8：线框开关
            wireframe = !wireframe;
            nl::core::log::info("wireframe -> ", wireframe ? "on" : "off");
        }
        if (window.keyPressed(nl::core::Key::N)) {   // M0.8：循环切调试着色模式
            // cluster 场景五个模式全可用（textured 采 1x1 白占位）；normal 场景 4 会读
            // 未绑定的 CLUSTERCOLOR，循环范围限 0..3。
            const int cycleCount = cluster ? kShadingCount : 4;
            shading = (shading + 1) % cycleCount;
            nl::core::log::info("shading -> ", shadingName(shading));
        }
        if (window.keyPressed(nl::core::Key::R)) {
            camera.reset();
            nl::core::log::info("camera reset");
        }
        float dx = 0.0f, dy = 0.0f, scroll = 0.0f, mouseX = 0.0f, mouseY = 0.0f;
        window.consumeMouse(dx, dy, scroll);
        window.mousePosition(mouseX, mouseY);
        if (!hudWantsMouse) {
            if (window.leftButtonDown() && (dx != 0.0f || dy != 0.0f)) camera.orbit(dx, dy);
            if (scroll != 0.0f) camera.zoom(scroll);
        }

        const char* capturePath = nullptr;
        std::string manualPath;
        std::string beforePath;
        if (window.keyPressed(nl::core::Key::F12)) {
            manualPath = "docs/screenshots/m0.5-" + std::string(st.backend) + "-manual.png";
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
            camera.setAspect(static_cast<float>(resized.width) / static_cast<float>(resized.height));
        }
        if (resizeFrame != 0 && frame + 1 == resizeFrame) {
            window.setSize(o.resizeWidth, o.resizeHeight);
            nl::core::log::info("resize case: request ", o.resizeWidth, "x", o.resizeHeight);
        }

        const auto now = clock::now();
        const double dtMs = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        ++frame;

        nl::renderer::FrameParams fp;
        const auto vp = camera.viewProjection();
        const auto model = camera.model();
        std::memcpy(fp.mvp, vp.m, sizeof(fp.mvp));
        std::memcpy(fp.model, model.m, sizeof(fp.model));
        fp.cullBack = cullBack;
        fp.shading = shading;
        fp.wireframe = wireframe;

        // ---- M0.5：每帧把显示数据交给 HUD，控件结果由 HUD 写回同一个 POD ----
        nl::renderer::HudFrame hud{};
        if (o.hudEnabled) {
            const auto e = renderer->extent();
            hud.displayWidth  = static_cast<float>(e.width);
            hud.displayHeight = static_cast<float>(e.height);
            hud.yawDeg = camera.yawDegrees();
            hud.pitchDeg = camera.pitchDegrees();
            hud.distance = camera.distance();
            hud.backend = st.backend;
            hud.adapter = info.adapterName.c_str();
            hud.debugLayer = info.debugLayer.c_str();
            hud.mode = nl::core::modeName(o.mode);
            hud.stage = st.stage;
            hud.model = info.modelName.c_str();
            hud.triangleCount = static_cast<int>(info.triangleCount);
            hud.cullBack = cullBack;
            hud.colorIndex = colorIndex;
            hud.shading = shading;
            hud.wireframe = wireframe;
            hud.deterministic = o.screenshot;
            if (o.screenshot) {
                // 确定性回放：固定样本 + 固定 dt，PNG 的 MD5 才能与基线逐字节对上。
                hud.fps = 60.0f;
                hud.frameMs = 16.67f;
                hud.frameMsCount = kFixedSampleCount;
                std::memcpy(hud.frameMsHistory, kFixedSamples, sizeof(kFixedSamples));
                // M0.6：GPU ms 也用固定样本；renderer 侧看到 deterministic 就不写回真实计时。
                hud.gpuMs = 0.63f;
                hud.gpuMsCount = kFixedSampleCount;
                std::memcpy(hud.gpuMsHistory, kFixedGpuSamples, sizeof(kFixedGpuSamples));
                hud.deltaSeconds = 1.0f / 60.0f;
                hud.mouseX = -100.0f; hud.mouseY = -100.0f;  // 不落在 HUD 窗口内
                hud.mouseLeftDown = false;
                hud.mouseWheel = 0.0f;
            } else {
                hud.fps = dtMs > 0.0 ? static_cast<float>(1000.0 / dtMs) : 0.0f;
                hud.frameMs = static_cast<float>(dtMs);
                hud.deltaSeconds = static_cast<float>(std::max(dtMs / 1000.0, 1.0 / 1000.0));
                hud.mouseX = mouseX;
                hud.mouseY = mouseY;
                hud.mouseLeftDown = window.leftButtonDown();
                hud.mouseWheel = scroll;
                if (frameHistoryCount < 120) {
                    frameHistory[frameHistoryCount++] = hud.frameMs;
                } else {
                    std::memmove(frameHistory, frameHistory + 1, 119 * sizeof(float));
                    frameHistory[119] = hud.frameMs;
                }
                hud.frameMsCount = frameHistoryCount;
                std::memcpy(hud.frameMsHistory, frameHistory,
                            static_cast<std::size_t>(frameHistoryCount) * sizeof(float));
                // M0.6：GPU 曲线由上一帧 renderFrame 写回的 hud.gpuMs 累积（异步 1~2 帧延迟）。
                hud.gpuMsCount = gpuHistoryCount;
                std::memcpy(hud.gpuMsHistory, gpuHistory,
                            static_cast<std::size_t>(gpuHistoryCount) * sizeof(float));
            }
        }

        if (!renderer->renderFrame(kColors[colorIndex], fp, capturePath,
                                   o.hudEnabled ? &hud : nullptr)) {
            nl::core::log::error("renderFrame failed: ", renderer->lastError());
            failed = true;
            break;
        }
        renderer->drainMessages();
        // M0.6：renderFrame 之后 renderer 可能已把最新 GPU 计时写回 hud.gpuMs，追加进曲线。
        if (o.hudEnabled && !o.screenshot && hud.gpuTimingAvailable && hud.gpuMs > 0.0f) {
            if (gpuHistoryCount < 120) {
                gpuHistory[gpuHistoryCount++] = hud.gpuMs;
            } else {
                std::memmove(gpuHistory, gpuHistory + 1, 119 * sizeof(float));
                gpuHistory[119] = hud.gpuMs;
            }
        }
        if (o.hudEnabled) {
            if (hud.resetCamera) camera.reset();
            cullBack = hud.cullBack;
            shading = std::clamp(hud.shading, 0, kShadingCount - 1);
            if (!cluster) shading = std::min(shading, 3);  // normal 场景无 CLUSTERCOLOR 属性
            wireframe = hud.wireframe;
            colorIndex = std::clamp(hud.colorIndex, 0, kColorCount - 1);
            hudWantsMouse = hud.wantCaptureMouse;
        }
        if (capturePath) {
            ++screenshotsSaved;
            nl::core::log::info("screenshot written: ", capturePath);
        }

        if (frame % 15 == 0 || o.screenshot) {
            const auto e = renderer->extent();
            // 里程碑号取 stage 前缀（kStage 以 "M0.x ..." 开头），避免每版漏改硬编码。
            const std::string stageStr(st.stage);
            const std::string stagePrefix = stageStr.substr(0, stageStr.find(' '));
            std::ostringstream title;
            title << "nl-engine | " << stagePrefix << " | " << st.backend
                  << " | mode " << nl::core::modeName(o.mode)
                  << " | HUD " << (o.hudEnabled ? "on" : "off")
                  << " | frame " << frame
                  << " | " << std::fixed << std::setprecision(1) << dtMs << " ms"
                  << " | yaw " << std::setprecision(1) << camera.yawDegrees()
                  << " pitch " << camera.pitchDegrees()
                  << " dist " << camera.distance()
                  << " | cull " << (cullBack ? "back" : "none")
                  << " | shading " << shadingName(shading) << (wireframe ? " wire" : "")
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

// S0 定型：所有里程碑共用这一个分发器，新增模式只加 case，不改 main。
int runMode(nl::core::ViewMode mode, const Options& o) {
    switch (mode) {
    case nl::core::ViewMode::Normal:
        return runNormal(o);
    case nl::core::ViewMode::Cluster:   // M1.3
        return runNormal(o, /*cluster=*/true);
    default: {
        const char* name = nl::core::modeName(mode);
        nl::core::log::error("mode has no implementation yet: ", name ? name : "<unknown>");
        return 1;
    }
    }
}

void printUsage() {
    const auto& st0 = nl::renderer::status();
    const std::string stageStr0(st0.stage);
    const std::string stagePrefix0 = stageStr0.substr(0, stageStr0.find(' '));
    std::cout << "nl-engine viewer (" << stagePrefix0 << ")  backend=" << st0.backend << '\n'
              << "  viewer.exe                         textured Suzanne + ImGui HUD; drag=orbit, wheel=zoom\n"
              << "                                     C=culling, V=wireframe, N=shading, R=reset, SPACE=clear, F12=PNG, ESC=quit\n"
              << "  viewer.exe --selftest              headless checks\n"
              << "  viewer.exe --screenshot <path>     render N frames, read back GPU pixels to PNG\n"
              << "  viewer.exe --frames N              frames before screenshot (default 8)\n"
              << "  viewer.exe --color 0..3            initial clear color (default 0)\n"
              << "  viewer.exe --size WxH              window size (default 1280x720; screenshot 640x360)\n"
              << "  viewer.exe --resize WxH            mid-run resize regression; saves <path>.before.png too\n"
              << "  viewer.exe --cull back|none        back-face culling (default back; none shows inner wall)\n"
              << "  viewer.exe --shading <name>        textured | normal | uv | tri-id (default textured; N key cycles)\n"
              << "  viewer.exe --wireframe             raster wireframe fill mode (V key toggles)\n"
              << "  viewer.exe --yaw D --pitch D       fixed orbit angles in degrees (screenshot reproducibility)\n"
              << "  viewer.exe --model <name>          load assets/<name>: .glb for normal mode,\n"
              << "                                     .nmesh for cluster mode (default per mode)\n"
              << "  viewer.exe --hud / --no-hud        force ImGui HUD on/off (window default on,\n"
              << "                                     --screenshot default off to keep M0.1~M0.4 baselines)\n"
              << "  viewer.exe --mode <name>           run mode; available: ";
    for (int i = 0; i < nl::core::modeCount(); ++i) {
        if (i) std::cout << ", ";
        std::cout << nl::core::modeNameAt(i);
    }
    std::cout << '\n'
              << "  viewer.exe --no-debug              disable current backend debug layer\n"
              << "  viewer.exe --help                  this text\n"
              << "  backend at configure time: -DNL_GRAPHICS_BACKEND=DX12|VULKAN\n";
}

} // namespace

int main(int argc, char** argv) {
    Options o = parseArgs(argc, argv);
    if (o.selftest) return runSelfTest();
    if (o.help) { printUsage(); return 0; }
    if (!nl::core::parseViewMode(o.modeText.c_str(), o.mode)) {
        std::cerr << "unknown --mode '" << o.modeText << "'. available: ";
        for (int i = 0; i < nl::core::modeCount(); ++i) {
            if (i) std::cerr << ", ";
            std::cerr << nl::core::modeNameAt(i);
        }
        std::cerr << '\n';
        return 1;
    }
    // M1.3：cluster 模式的缺省资产是 assets/meshlets/Suzanne.nmesh（用户没给 --model 时）
    if (o.mode == nl::core::ViewMode::Cluster && !o.modelSet) o.modelFile = "Suzanne.nmesh";
    return runMode(o.mode, o);
}
