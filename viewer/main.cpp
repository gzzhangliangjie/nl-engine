#include "nl/core/assert.hpp"
#include "nl/core/camera.hpp"
#include "nl/core/lod_error.hpp"
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

// M4.2：cull pass GPU 耗时的固定样本（同上理由；Suzanne 量级剔除段远小于整帧）。
const float kFixedCullSamples[kFixedSampleCount] = {
    0.08f, 0.09f, 0.08f, 0.10f, 0.08f, 0.09f, 0.11f, 0.08f
};

struct Options {
    bool selftest   = false;
    bool help       = false;
    bool debug      = true;
    bool screenshot = false;
    bool sizeSet    = false;
    bool resize     = false;
    bool cullBack   = true;   // C 键 / --cull / HUD 复选框控制
    bool frustumOn  = true;   // M4.1：GPU 视锥剔除（--frustum off 走恒等列表 = M4.0 行为）
    int  shading    = 0;      // M0.8：0 textured / 1 normal / 2 uv / 3 tri-id；M1.3：4 cluster（N 键 / HUD combo）
    bool wireframe  = false;  // M0.8：线框（V 键 / --wireframe / HUD 复选框）
    bool shadingSet = false;  // 用户显式给了 --shading（lod 模式的缺省着色不同）
    bool hudSet     = false;  // 用户显式给了 --hud / --no-hud
    bool hudEnabled = false;  // 最终值：交互默认开，截图回归默认关
    float cutThresholdPx = 1.0f;  // M5.0：cut 阈值 px（HUD 滑条双向，M5.1 遍历消费）
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
    int  lod = 0;             // M2.3：--lod N，lod 模式的初始层（HUD 滑条可再切）
    int  dagGroup = -1;       // M3.2：--dag-group N，dag-top 模式初始下钻组（-1 = 纯顶层）
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
        else if (a == "--lod" && i + 1 < argc) {
            // M2.3：层号必须是十进制整数；越界（相对的层数上限）在 renderer 探测后
            // 再报错——这里只拒绝非数字与负数，绝不静默落回 0。
            const std::string v = argv[++i];
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (!end || *end != '\0' || n < 0) {
                std::cerr << "invalid --lod '" << v << "' (expected an integer >= 0)\n";
                std::exit(1);
            }
            o.lod = static_cast<int>(n);
        }
        else if (a == "--dag-group" && i + 1 < argc) {
            // M3.2：dag-top 模式初始下钻组。-1 = 纯顶层；其它负数/非数字直接拒
            // （组数上限在 renderer 侧探测后校验，同样绝不静默落回 -1）。
            const std::string v = argv[++i];
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (!end || *end != '\0' || n < -1) {
                std::cerr << "invalid --dag-group '" << v << "' (expected an integer >= -1)\n";
                std::exit(1);
            }
            o.dagGroup = static_cast<int>(n);
        }
        else if (a == "--model" && i + 1 < argc) {
            o.modelFile = argv[++i];
            o.modelSet = true;
        }
        else if (a == "--cull" && i + 1 < argc) {
            const std::string v = argv[++i];
            o.cullBack = (v != "none" && v != "off" && v != "0");
        } else if (a == "--frustum" && i + 1 < argc) {
            // M4.1：GPU 视锥剔除开关（仅 cull 模式消费；off = 恒等可见列表，M4.0 行为）。
            const std::string v = argv[++i];
            if (v == "on" || v == "1" || v == "true") o.frustumOn = true;
            else if (v == "off" || v == "0" || v == "none" || v == "false") o.frustumOn = false;
            else {
                std::cerr << "invalid --frustum '" << v << "' (expected on|off)\n";
                std::exit(1);
            }
        } else if (a == "--shading" && i + 1 < argc) {
            const std::string v = argv[++i];
            o.shading = -1;
            o.shadingSet = true;
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
    check(nl::core::lodErrorSelfTest(), "M5.0 screen-error projection, hand-computed cases");
    check(nl::core::modeSelfTest(), "view mode dispatcher (normal + cluster + lod + dag-top)");

    const auto st = nl::renderer::status();
    std::cout << "  backend: " << st.backend << " (configured, no vendor SDK)\n"
              << "  stage:   " << st.stage << '\n';
    check(st.backend != nullptr && std::strlen(st.backend) > 0 && !st.deviceReady,
          "backend selected");
    check(std::strstr(st.stage, "S5.0") != nullptr, "S5.0 stage reported");

    nl::renderer::FrameParams fp;
    check(fp.cullBack && sizeof(fp.mvp) == 64 && fp.lightDir[3] > 0.0f,
          "frame params (MVP/cull/light) POD defaults");
    check(fp.shading == 0 && !fp.wireframe,
          "M0.8 debug shading / wireframe POD defaults (= M0.7 behavior)");
    check(nl::renderer::hudSelfTest(), "HUD POD + projection arithmetic");
    std::string shaderReport;
    check(nl::renderer::shaderArtifactsSelfTest(shaderReport),
          "Slang single-source -> SPIR-V + DXIL (18 artifacts)");
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
// M2.3：lod=true 时也走 cluster 场景（同一份顶点布局），但资产是分层文件
// <stem>.lodN.nmesh，--lod N 指定初始层、HUD 滑条切层；缺省着色 normal（几何不连续
// 在逐面法线色上最刺眼，是「相邻层无裂缝」验收最敏感的口径）。
// M3.2：dagTop=true 时同一 cluster 场景，但初始渲染链最粗层（DAG 顶层组产出的
// 父簇集合 = 正确粗模），--dag-group N 任选组下钻一层（挖掉其父簇、换回组内
// 子簇）验证父子衔接无洞——S5 组原子 cut 的离线预演。
int runNormal(const Options& o, bool cluster = false, bool lodMode = false,
              bool dagTop = false, bool cullMode = false) {
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
                                                   o.modelFile.c_str(), cluster, cullMode,
                                                   o.frustumOn);
    if (!renderer->valid()) {
        nl::core::log::error("renderer init failed: ", renderer->lastError());
        return 1;
    }

    // M2.3：lod 模式先把 --lod N 落到具体层。层数由 renderer 扫描
    // assets/meshlets/<stem>.lodN.nmesh 得出（N 从 0 连续）；没有分层资产或
    // N 越界都直接退出 1，不静默回退到最细层——否则「以为在看 L3」是假证据。
    int lodLevel = 0;
    int lodCount = 0;
    if (lodMode || dagTop) {
        lodCount = renderer->lodLevelCount();
        if (lodCount <= 0) {
            nl::core::log::error("--mode ", nl::core::modeName(o.mode),
                                 ": no layered assets for ", o.modelFile,
                                 " (expected assets/meshlets/<stem>.lodN.nmesh; run "
                                 "nanite-tools lod <base>.nmesh -o assets/meshlets/<stem>)");
            return 1;
        }
    }
    // M3.2：dag-top 模式先把初始状态落到具体组。-1 = 纯顶层；>=0 需要至少两层，
    // 且组号必须落在可下钻范围内（renderer 探测第二粗层 dagGroups），越界退出 1。
    int dagGroup = -1;
    int dagGroupCount = 0;
    // M5.0：cut 阈值 px 运行时状态（HUD 滑条双向写回，M5.1 GPU 遍历消费）。
    float cutThresholdPx = o.cutThresholdPx;
    if (dagTop) {
        dagGroupCount = renderer->dagGroupCount();
        if (o.dagGroup >= 0) {
            if (lodCount < 2) {
                nl::core::log::error("--dag-group ", o.dagGroup, " needs >= 2 layers, ",
                                     o.modelFile, " has ", lodCount);
                return 1;
            }
            if (o.dagGroup >= dagGroupCount) {
                nl::core::log::error("--dag-group ", o.dagGroup, " out of range 0..",
                                     dagGroupCount - 1);
                return 1;
            }
            if (!renderer->setDagCut(o.dagGroup)) {
                nl::core::log::error("dag cut failed: ", renderer->lastError());
                return 1;
            }
            dagGroup = o.dagGroup;
        } else if (!renderer->setLodLevel(lodCount - 1)) {
            // 纯顶层：直接渲染最粗层（与 lod 模式同一条 uploadClusterMesh 路径）。
            nl::core::log::error("top level load failed: ", renderer->lastError());
            return 1;
        }
        lodLevel = lodCount - 1;
    } else if (lodMode) {
        if (o.lod >= lodCount) {
            nl::core::log::error("--lod ", o.lod, " out of range: ", o.modelFile,
                                 " has ", lodCount, " layers (0..", lodCount - 1, ")");
            return 1;
        }
        if (!renderer->setLodLevel(o.lod)) {
            nl::core::log::error("lod level ", o.lod, " load failed: ", renderer->lastError());
            return 1;
        }
        lodLevel = o.lod;
    }

    nl::core::OrbitCamera camera;
    camera.setAspect(static_cast<float>(extent.width) / static_cast<float>(extent.height));
    camera.setOrbit(o.yawDeg, o.pitchDeg, o.distance);
    bool cullBack = o.cullBack;
    // M1.3：cluster 模式缺省看簇色。M2.3：lod 模式缺省 normal（几何不连续最刺眼），
    // 显式 --shading 则听用户的——注意 lod 也是 cluster 场景，必须在 cluster 缺省
    // 之后再判，否则显式 --shading normal 会被 4=cluster 静默吞掉。
    int shading = (cluster || cullMode) ? 4 : o.shading;
    if (lodMode) shading = o.shadingSet ? o.shading : 1;
    // M3.2：dag-top 同 lod 模式缺省 normal（父子衔接的几何不连续最刺眼），
    // 显式 --shading 则听用户的。
    if (dagTop) shading = o.shadingSet ? o.shading : 1;
    bool wireframe = o.wireframe;

    const auto st = nl::renderer::status();
    const auto& info = renderer->info();
    nl::core::log::info("backend: ", st.backend, " | ", info.adapterName);
    nl::core::log::info("debug layer: ", info.debugLayer,
                        " | active=", info.debugLayerActive ? "yes" : "no");
    const std::string lodLine = lodMode
        ? " | lod layers: " + std::to_string(lodCount) + " (level " + std::to_string(lodLevel) + ")"
        : std::string();
    // M3.2：dag-top 日志自证顶层/下钻状态（不写进 HUD 的话至少日志有据可查）。
    std::string dagLine;
    if (dagTop) {
        dagLine = " | dag layers: " + std::to_string(lodCount) + " (top "
                + std::to_string(lodCount - 1) + ")";
        dagLine += dagGroup >= 0
            ? " | drill g" + std::to_string(dagGroup) + " (" + std::to_string(dagGroupCount) + " groups)"
            : " | no drill (" + std::to_string(dagGroupCount) + " groups available)";
    }
    nl::core::log::info("mode: ", nl::core::modeName(o.mode),
                        " | HUD: ", o.hudEnabled ? "on" : "off",
                        " | shading: ", shadingName(shading), wireframe ? " | wireframe" : "",
                        lodLine, dagLine,
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
    float cullHistory[120] = {};   // M4.2：cull pass GPU ms 曲线（同上，renderer 写回值追加）
    int cullHistoryCount = 0;
    const std::uint64_t resizeFrame =
        (o.resize && o.frames > 2) ? std::max<std::uint64_t>(2, o.frames / 2) : 0;
    // M4.2：截图模式的 CPU 帧耗时机械证据（60fps 验收判据 = avg/max 均 ≤16.6ms）。
    // 只进日志不进画面，任何 PNG 基线不受影响。截图模式无 16ms 节流 sleep，
    // 测的是纯渲染吞吐上限（无节流都能 ≤16.6ms，节流后必然稳 60fps）。
    double dtSumMs = 0.0;
    double dtMaxMs = 0.0;

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
            const int cycleCount = (cluster || cullMode) ? kShadingCount : 4;
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
        if (o.screenshot) {
            dtSumMs += dtMs;
            if (dtMs > dtMaxMs) dtMaxMs = dtMs;
        }

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
            // M2.3：lod 模式把当前层喂给滑条；其他模式 lodCount=0，HUD 不画滑条。
            // M3.2：dag-top 模式同理反着用——不喂 lod 滑条（viewer 不消费它），
            // 喂 dag 下钻滑条；两套滑条不同时出现，HUD 布局互不干扰。
            if (dagTop) {
                hud.lodCount = 0;
                hud.lodTriangles = static_cast<int>(info.triangleCount);
                hud.dagGroup = dagGroup;
                hud.dagGroupCount = dagGroupCount;
                hud.dagGroupError = info.dagGroupError;
                // M5.0：屏幕误差投影——下钻时用组记录球 + 组误差（组原子 cut 语义），
                // 纯顶层用资产联合球 + 顶层 maxErr。转相机时该值随距离单调变化。
                const bool drilled = (dagGroup >= 0 && info.dagGroup >= 0);
                const float* sph = drilled ? info.dagGroupSphere : info.assetSphere;
                const float worldErr = drilled ? info.dagGroupError : info.lodMaxError;
                hud.cutThresholdPx = cutThresholdPx;
                if (sph[3] > 0.0f) {
                    float eye[3];
                    camera.eyePosition(eye);
                    hud.screenErrorPx = nl::core::screenErrorPixels(
                        worldErr, sph, sph[3], eye, camera.projectionYScale(),
                        hud.displayHeight, camera.nearPlane());
                    hud.screenErrorValid = true;
                }
            } else {
                hud.lodLevel = lodLevel;
                hud.lodCount = lodCount;
                hud.lodTriangles = static_cast<int>(info.triangleCount);
                hud.lodMaxError = info.lodMaxError;
            }
            hud.deterministic = o.screenshot;
            // M4.1：GPU 剔除统计（DeviceInfo 由 backend 每帧 poll 后更新，只读显示）。
            hud.visibleClusters = info.visibleClusters;
            hud.totalClusters = static_cast<int>(info.totalClusters);
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
                // M4.2：cull pass 耗时同样固定（renderer 侧 deterministic 不写回真实值）。
                hud.cullMs = 0.08f;
                hud.cullMsCount = kFixedSampleCount;
                std::memcpy(hud.cullMsHistory, kFixedCullSamples, sizeof(kFixedCullSamples));
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
                // M4.2：cull pass 曲线同一条累积路（renderer 写回 hud.cullMs）。
                hud.cullMsCount = cullHistoryCount;
                std::memcpy(hud.cullMsHistory, cullHistory,
                            static_cast<std::size_t>(cullHistoryCount) * sizeof(float));
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
        // M4.2：cull pass 计时同一条追加路（cullTimingAvailable 只在 cull 模式置位）。
        if (o.hudEnabled && !o.screenshot && hud.cullTimingAvailable && hud.cullMs > 0.0f) {
            if (cullHistoryCount < 120) {
                cullHistory[cullHistoryCount++] = hud.cullMs;
            } else {
                std::memmove(cullHistory, cullHistory + 1, 119 * sizeof(float));
                cullHistory[119] = hud.cullMs;
            }
        }
        if (o.hudEnabled) {
            if (hud.resetCamera) camera.reset();
            cullBack = hud.cullBack;
            shading = std::clamp(hud.shading, 0, kShadingCount - 1);
            if (!cluster && !cullMode) shading = std::min(shading, 3);  // 非簇场景无 CLUSTERCOLOR 属性
            wireframe = hud.wireframe;
            colorIndex = std::clamp(hud.colorIndex, 0, kColorCount - 1);
            // M2.3：滑条写了新层号才真的换资产（渲染资源只在 viewer 这一侧被动，
            // HUD 永远不直接碰 GPU）。失败保持原层并把这次运行判为失败。
            if (lodMode && lodCount > 0) {
                const int want = std::clamp(hud.lodLevel, 0, lodCount - 1);
                if (want != lodLevel) {
                    if (renderer->setLodLevel(want)) {
                        lodLevel = want;
                    } else {
                        nl::core::log::error("lod switch failed: ", renderer->lastError());
                        failed = true;
                    }
                }
            }
            // M3.2：dag 下钻滑条同一套双向约定；失败保持原画面并判失败。
            if (dagTop && dagGroupCount > 0) {
                const int want = std::clamp(hud.dagGroup, -1, dagGroupCount - 1);
                if (want != dagGroup) {
                    if (renderer->setDagCut(want)) {
                        dagGroup = want;
                    } else {
                        nl::core::log::error("dag cut switch failed: ", renderer->lastError());
                        failed = true;
                    }
                }
            }
            // M5.0：cut 阈值滑条回读（M5.1 GPU 遍历消费同一参数）。
            if (dagTop) cutThresholdPx = std::clamp(hud.cutThresholdPx, 0.25f, 4.0f);
            hudWantsMouse = hud.wantCaptureMouse;
        }
        if (capturePath) {
            ++screenshotsSaved;
            nl::core::log::info("screenshot written: ", capturePath);
        }
        // M4.1：截图模式下把 GPU 剔除计数写进日志——自检脚本与 frustum_check.py
        // 逐数字对照的机械证据（HUD 在截图基线里是关闭的，日志才是可解析出口）。
        if (o.screenshot && cullMode && info.totalClusters > 0 && info.visibleClusters >= 0) {
            nl::core::log::info("cull stats: visible ", info.visibleClusters, " / ",
                                info.totalClusters, " clusters (frustum on)");
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
            // M2.3：lod 模式在标题栏也标层号，交互时不必盯着 HUD 滑条。
            if (lodMode) title << " | lod " << lodLevel << "/" << lodCount;
            // M3.2：dag-top 标题栏标下钻组（-1 = 纯顶层）。
            if (dagTop) title << " | dag " << (dagGroup >= 0 ? "g" + std::to_string(dagGroup) : "top");
            window.setTitle(title.str());
        }

        if (o.screenshot && frame >= static_cast<std::uint64_t>(o.frames)) break;
        if (!o.screenshot) std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    nl::core::log::info("shutdown ok (frames=", frame,
                        " screenshots=", screenshotsSaved,
                        " deviceErrors=", nl::core::log::deviceErrorCount(), ")");
    // M4.2：60fps 验收的 CPU 侧机械证据（截图模式无节流，测渲染吞吐上限）。
    if (o.screenshot && frame > 1) {
        nl::core::log::info("frame timing: avg ",
                            dtSumMs / static_cast<double>(frame), " ms  max ",
                            dtMaxMs, " ms  over ", frame, " frames (unthrottled)");
    }

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
    case nl::core::ViewMode::Lod:       // M2.3：同一个 cluster 场景，资产换成分层 .nmesh
        return runNormal(o, /*cluster=*/true, /*lodMode=*/true);
    case nl::core::ViewMode::DagTop:    // M3.2：DAG 顶层 + 任选组下钻一层
        return runNormal(o, /*cluster=*/true, /*lodMode=*/false, /*dagTop=*/true);
    case nl::core::ViewMode::Cull:       // M4.0：vertex pulling 渲染（单 drawIndexedIndirect，全簇全画）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false, /*cullMode=*/true);
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
              << "  viewer.exe --frustum on|off        GPU frustum culling in cull mode (default on;\n"
              << "                                     off = identity visible list, M4.0 behavior)\n"
              << "  viewer.exe --shading <name>        textured | normal | uv | tri-id (default textured; N key cycles)\n"
              << "  viewer.exe --wireframe             raster wireframe fill mode (V key toggles)\n"
              << "  viewer.exe --yaw D --pitch D       fixed orbit angles in degrees (screenshot reproducibility)\n"
              << "  viewer.exe --model <name>          load assets/<name>: .glb for normal mode,\n"
              << "                                     .nmesh for cluster/lod mode (default per mode)\n"
              << "  viewer.exe --lod N                 lod mode: start at layer N (default 0); the\n"
              << "                                     HUD slider switches layers at runtime\n"
              << "  viewer.exe --dag-group N           dag-top mode: drill into group N of the\n"
              << "                                     second-coarsest layer (-1 = top only, default);\n"
              << "                                     the HUD slider switches groups at runtime\n"
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
    // M2.3：lod 模式同样缺省 Suzanne（分层文件 Suzanne.lodN.nmesh 随之探测）。
    // M3.2：dag-top 同缺省（顶层渲染 + 下钻都用同一套分层资产）。
// M4.0：cull=true 走 vertex pulling 场景（assets/meshlets/*.nmesh，单 drawIndexedIndirect
// 全簇全画，M4.1 才接 GPU 剔除），缺省着色 4=cluster；簇色由 shader 端 pcg(簇号) 算出，
// 与 cluster 模式 CPU 端 clusterColor() 同式，逐像素一致。
    if ((o.mode == nl::core::ViewMode::Cluster || o.mode == nl::core::ViewMode::Lod ||
         o.mode == nl::core::ViewMode::DagTop || o.mode == nl::core::ViewMode::Cull)
        && !o.modelSet) {
        o.modelFile = "Suzanne.nmesh";
    }
    return runMode(o.mode, o);
}
