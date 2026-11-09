#include "nl/core/assert.hpp"
#include "nl/core/camera.hpp"
#include "nl/core/console.hpp"
#include "nl/core/lod_error.hpp"
#include "nl/core/log.hpp"
#include "nl/core/modes.hpp"
#include "nl/core/window.hpp"
#include "nl/renderer/editor.hpp"
#include "nl/renderer/renderer.hpp"
#include "nl/physics/World.hpp"   // M-P3：物理调试场景（DebugScene，PIMPL 把 Jolt 头挡在 cpp 内）

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
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
// pbr-direct：5 = pbr（只有 psMain 引用 pbr 分支；cluster/cull/dag 场景的管线
// SPIR-V 不含 t1/b1，选了也只落到 mode-0 兜底，N 键在 cluster 场景钳到 0..4）
constexpr int kShadingCount = 6;
const char* shadingName(int s) {
    static const char* kNames[kShadingCount] =
        { "textured", "normal", "uv", "tri-id", "cluster", "pbr" };
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
    bool hizOn      = true;   // M6.2：Hi-Z 遮蔽剔除（--hiz off = 纯视锥/误差遍历，退化可验证）
    int  shading    = 0;      // M0.8：0 textured / 1 normal / 2 uv / 3 tri-id；M1.3：4 cluster（N 键 / HUD combo）
    bool wireframe  = false;  // M0.8：线框（V 键 / --wireframe / HUD 复选框）
    bool shadingSet = false;  // 用户显式给了 --shading（lod 模式的缺省着色不同）
    bool pbrNaive   = false;  // pbr-direct：--pbr-naive 对照开关（跳过 sRGB->linear，验收证据用）
    bool hudSet     = false;  // 用户显式给了 --hud / --no-hud
    bool hudEnabled = false;  // 最终值：交互默认开，截图回归默认关
    float cutThresholdPx = 1.0f;  // M5.0/M5.1：cut 阈值 px（--cut-thr 设初值，HUD 滑条双向）
    int  width      = 1280;
    int  height     = 720;
    int  resizeWidth  = 0;
    int  resizeHeight = 0;
    int  color      = 0;
    int  frames     = 8;
    float yawDeg    = 12.0f;  // 与 OrbitCamera 默认一致，默认正脸、截图可复现
    float pitchDeg  = 9.2f;
    bool distSet = false;                     // 用户显式给了 --dist（skin 模式缺省距离不同）
    float distance  = 4.2f;
    std::string screenshotPath;
    std::string modelFile = "Suzanne.glb";  // M0.7：--model 选 assets/ 下的资产
    bool modelSet = false;                  // M1.3：cluster 模式缺省模型是 Suzanne.nmesh
    std::string editorGraph;                // T1：--editor <file.mg> 挂材质图编辑器
    std::string modeText = "normal";
    nl::core::ViewMode mode = nl::core::ViewMode::Normal;
    int  lod = 0;             // M2.3：--lod N，lod 模式的初始层（HUD 滑条可再切）
    int  dagGroup = -1;       // M3.2：--dag-group N，dag-top 模式初始下钻组（-1 = 纯顶层）
    // M-A5：skin 模式动画时间（秒）。未指定 = 实时累加；指定 = 固定（截图确定性）。
    bool animTimeSet = false;
    float animTime = 0.0f;
    // M-C1：开发者控制台（` 呼出）。console 开关 + 预执行命令行 + 输入行预填。
    bool consoleOpen = false;
    std::vector<std::string> consoleExecLines;  // 启动时按序执行（演示/截图证据）
    std::string consolePrefill;                 // 输入行预填（Tab 补全演示）
    bool physicsDebug = false;                   // M-P3：物理调试叠加层（Jolt 碰撞形状线框）
    int  physicsScenario = 0;                    // M-P3：--physics-scenario N
    std::uint32_t physicsSeed = 0;               // M-P3：--physics-seed N（当前仅记录）
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") o.selftest = true;
        else if (a == "--help") o.help = true;
        else if (a == "--no-debug") o.debug = false;
        else if (a == "--pbr-naive") o.pbrNaive = true;   // pbr-direct：验收对照（过亮的错误路径）
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
        else if (a == "--cut-thr" && i + 1 < argc) {
            // M5.1：dag 模式屏幕误差阈值（px）。必须为正数；截图回归靠它定死选层。
            const std::string v = argv[++i];
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (!end || *end != '\0' || !(d > 0.0)) {
                std::cerr << "invalid --cut-thr '" << v << "' (expected a number > 0)\n";
                std::exit(1);
            }
            o.cutThresholdPx = static_cast<float>(d);
        }
        else if (a == "--model" && i + 1 < argc) {
            o.modelFile = argv[++i];
            o.modelSet = true;
        }
        else if (a == "--editor" && i + 1 < argc) {
            // T1：材质图编辑器（material_graph v1 文本文件）。
            o.editorGraph = argv[++i];
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
        } else if (a == "--hiz" && i + 1 < argc) {
            // M6.2：Hi-Z 遮蔽剔除开关（仅 hiz 模式消费；off = 只做视锥/误差遍历，
            // 退化可验证——UE r.Nanite.Culling.HZB 同定位；缺省 on）。
            const std::string v = argv[++i];
            if (v == "on" || v == "1" || v == "true") o.hizOn = true;
            else if (v == "off" || v == "0" || v == "none" || v == "false") o.hizOn = false;
            else {
                std::cerr << "invalid --hiz '" << v << "' (expected on|off)\n";
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
        }         else if (a == "--wireframe") {
            o.wireframe = true;
        } else if (a == "--physics-debug") {       // M-P3：物理调试叠加层（Jolt 碰撞形状线框）
            o.physicsDebug = true;
        } else if (a == "--physics-scenario" && i + 1 < argc) {   // M-P3：场景选择（0 = 落体盒/球/胶囊）
            o.physicsScenario = std::atoi(argv[++i]);
        } else if (a == "--physics-seed" && i + 1 < argc) {       // M-P3：随机种子（当前仅记录）
            o.physicsSeed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--console" && i + 1 < argc) {   // M-C1：初始开关（on|off）
            const std::string v = argv[++i];
            if (v == "on" || v == "1") o.consoleOpen = true;
            else if (v == "off" || v == "0") o.consoleOpen = false;
            else {
                std::cerr << "invalid --console '" << v << "' (expected on|off)\n";
                std::exit(1);
            }
        } else if (a == "--console-exec" && i + 1 < argc) {  // M-C1：启动时执行的命令行
            o.consoleExecLines.push_back(argv[++i]);
        } else if (a == "--console-type" && i + 1 < argc) {  // M-C1：输入行预填（补全演示）
            o.consolePrefill = argv[++i];
        } else if (a == "--yaw" && i + 1 < argc) {
            o.yawDeg = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--pitch" && i + 1 < argc) {
            o.pitchDeg = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--dist" && i + 1 < argc) {
            o.distance = static_cast<float>(std::atof(argv[++i]));
            o.distSet = true;
        } else if (a == "--anim-time" && i + 1 < argc) {
            // M-A5：固定蒙皮动画时间（秒，clip 内循环采样）——截图确定性与验收用。
            o.animTimeSet = true;
            o.animTime = static_cast<float>(std::atof(argv[++i]));
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
    check(std::strstr(st.stage, "pbr-direct") != nullptr, "pbr-direct stage reported");

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

// M-C2：真引擎参数注册表——运行中 `cam.fov 60` / `cam.dist 3` 立即生效（onChanged 回调
// 打进相机），`hud.show` 开关 HUD 面板；内建 help / cvar.list / cvar.set 由
// Console::installBuiltins 承担。echo/add 保留（日志与算术演示）。
// 返回「相机实际值 -> cvar 静默回写」的每帧同步函数：滚轮 zoom / reset 后 cam.dist
// 不至于显示陈旧值；g_syncing 挡住回写触发的 onChanged 重入（不再打相机 setter）。
// CVar* 指向 Console 注册表 map 节点，生命周期与单例同寿，lambda 捕获安全。
std::function<void()> registerViewerConsole(nl::core::OrbitCamera* camera, bool* hudEnabled) {
    using namespace nl::core::console;
    Console& c = Console::get();
    c.installBuiltins();

    // cvar 初值取当前实际状态（相机可能被 --dist/--fov 类 CLI 预置过，别写死 45/4.2）
    CVar* const fov  = &c.registerFloat("cam.fov", camera->fovYDegrees(), "vertical fov in degrees (20..120)");
    CVar* const dist = &c.registerFloat("cam.dist", camera->distance(), "orbit distance (0.45..20)");
    CVar* const hud  = &c.registerBool("hud.show", *hudEnabled, "toggle the HUD panel");

    static bool g_syncing = false;   // 回写 cvar 时挡 onChanged 重入
    fov->setOnChanged([camera](const CVar& cv) {
        if (!g_syncing) camera->setFovYDegrees(static_cast<float>(cv.value().f));
    });
    dist->setOnChanged([camera](const CVar& cv) {
        if (!g_syncing) camera->setDistance(static_cast<float>(cv.value().f));
    });
    hud->setOnChanged([hudEnabled](const CVar& cv) { *hudEnabled = cv.value().b; });

    c.registerCommand("echo", "print args joined", [](const std::vector<std::string>& args) {
        std::string out;
        for (std::size_t k = 0; k < args.size(); ++k) { if (k) out += ' '; out += args[k]; }
        return CommandResult{true, out.empty() ? std::string("echo: (no args)") : "echo: " + out};
    });
    c.registerCommand("add", "sum float args", [](const std::vector<std::string>& args) {
        if (args.empty()) return CommandResult{false, "usage: add <float>..."};
        double sum = 0.0;
        for (const auto& a : args) {
            char* end = nullptr;
            const double v = std::strtod(a.c_str(), &end);
            if (end == a.c_str() || *end != '\0')
                return CommandResult{false, "add: not a number: " + a};
            sum += v;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", sum);
        return CommandResult{true, std::string("add: ") + buf};
    });

    return [fov, dist, camera]() {
        g_syncing = true;
        fov->setFloat(camera->fovYDegrees());
        dist->setFloat(camera->distance());
        g_syncing = false;
    };
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
              bool dagTop = false, bool cullMode = false, bool dagMode = false,
              bool skinMode = false, bool hizMode = false) {
    nl::core::log::resetCounters();

    nl::core::WindowDesc desc;
    desc.width  = o.width;
    desc.height = o.height;
    desc.title  = "nl-engine | starting...";
    // 截图回归模式：隐藏窗口。像素来自 GPU readback，与窗口可见性无关；
    // 可见窗口首显必抢前台焦点，并行跑闸会打断用户输入（2026-11-08 实测修）。
    desc.visible = !o.screenshot;
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
                                                   o.frustumOn, dagMode, skinMode, hizMode);
    if (!renderer->valid()) {
        nl::core::log::error("renderer init failed: ", renderer->lastError());
        return 1;
    }

    // T1：--editor <file.mg> 挂材质图编辑器。复用当前场景模式做背景（缺省 normal），
    // 编辑器窗口经 HudFrame.editor 挂入 hud 的 NewFrame..Render 区间——ImGui 符号
    // 全在 renderer/ 内，viewer 只持指针。编辑器需要 ImGui 路径，强制开 HUD。
    std::unique_ptr<nl::renderer::NodeEditor> editor;
    bool editorActive = false;
    if (!o.editorGraph.empty()) {
        std::string err;
        editor = nl::renderer::NodeEditor::create(o.editorGraph.c_str(), &err);
        if (!editor->valid()) {
            nl::core::log::error("--editor: ", err);
            return 1;
        }
        editorActive = true;
        nl::core::log::info("editor: ", o.editorGraph, " (", editor->graphPath(), ")");
    }
    // M-P3：物理调试场景（碰撞形状线框）。PIMPL 把 Jolt 头挡在 World.cpp 内，viewer
    // 只持值对象。仅 --physics-debug 时 build；build 失败直接退出 1（绝不静默回退到
    // 无叠加层——否则「以为在看物理」是假证据）。
    nl::physics::DebugScene physicsScene;
    if (o.physicsDebug) {
        std::string perr;
        if (!physicsScene.build(o.physicsScenario, o.physicsSeed, &perr)) {
            nl::core::log::error("physics debug scene build failed: ", perr);
            return 1;
        }
        if (o.physicsScenario == 1)
            nl::core::log::info("physics debug scene: ", physicsScene.bodyCount(),
                               " static course bodies (floor + ramp/steps/platforms/steep slope/wall"
                               ", CharacterVirtual walks without a rigid body)");
        else
            nl::core::log::info("physics debug scene: ", physicsScene.bodyCount(),
                               " bodies (floor + dynamic box/sphere/capsule)");
    }

    // Options 是 const（parseArgs 定稿）；HUD 最终值 = 用户显式选择 || 编辑器强制。
    const bool hudOn = o.hudEnabled || editorActive;

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
    int hizViewLevel = 0;    // M6.0：金字塔可视化 mip 层（HUD 滑条双向）
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
    // M-A5.4：skin 模式资产尺度自适应——Fox.glb 等资产单位远大于 1m，固定缺省
    // 距离会把相机放进模型肚子里。未显式 --dist 时按 t=0 蒙皮输出半径取景，
    // max() 兜底保证 CesiumMan 级小资产维持既有 2.6 取景（基线不破）。
    float camDist = o.distance;
    if (o.mode == nl::core::ViewMode::Skin && !o.distSet) {
        camDist = std::max(camDist, renderer->info().skinRadius * 2.4f);
    }
    camera.setOrbit(o.yawDeg, o.pitchDeg, camDist);
    // M-P4：走查场固定机位——场地 x∈[-13,17]、y∈[0,6.8]，OrbitCamera pivot 固定
    // 在原点，缺省 4.2 距离取不到景；scenario=1 且未显式给 --dist 时摆侧视机位
    // （yaw 0 = 从 +z 看向原点的正面视角，pitch 28° 俯视全场）。
    if (o.physicsDebug && o.physicsScenario == 1 && !o.distSet) {
        camera.setOrbit(0.0f, 35.0f, 19.0f);
    }
    bool cullBack = o.cullBack;
    // M6.2：Hi-Z 遮蔽剔除总开关（HUD checkbox 双向；缺省开 = M6.1 行为不变）。
    bool hizEnabled = o.hizOn;
    bool hudEnabled = o.hudEnabled;   // M-C2：hud.show cvar 运行时改写（o 是 const）
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
                        " | HUD: ", hudOn ? "on" : "off",
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
                            " | hud=", hudOn ? "on" : "off");
    }

    // ---- M-C1 控制台注册表 + M-C2 真参数（cam.*/hud.* onChanged 打进相机/HUD）----
    // 预执行命令行（结果留在控制台日志 = 截图证据）。
    const auto syncCameraToCVar = registerViewerConsole(&camera, &hudEnabled);
    for (const auto& line : o.consoleExecLines) {
        const auto r = nl::core::console::Console::get().exec(line);
        nl::core::log::info("console exec [", r.exitCode(), "] ", line);
    }
    bool consoleOpen = o.consoleOpen;
    if (consoleOpen && !hudEnabled)
        nl::core::log::warn("console requested but HUD is off; add --hud to see it");
    bool hudWantsKeyboard = false;  // 上一帧控制台输入行是否在打字（屏蔽游戏热键）

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    double skinTime = 0.0;   // M-A5：skin 模式动画时间（--anim-time 固定 / 实时累加）
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
    // M6.2：整帧 GPU 时间累计（DeviceInfo.lastGpuMs 每帧由 backend poll 更新）。
    double gpuMsSum = 0.0;
    std::uint64_t gpuMsFrames = 0;

    while (!window.shouldClose()) {
        window.pollEvents();
        syncCameraToCVar();   // M-C2：相机实际值回写 cam.fov/cam.dist（滚轮 zoom / reset 后不陈旧）
        // M-C1：` 边沿切换控制台；ESC 开着控制台时先收控制台，再退出
        if (window.keyPressed(nl::core::Key::Grave)) {
            consoleOpen = !consoleOpen;
            nl::core::log::info("console -> ", consoleOpen ? "open" : "closed");
        }
        if (window.keyPressed(nl::core::Key::Escape)) {
            if (consoleOpen) {
                consoleOpen = false;
                nl::core::log::info("console -> closed (ESC)");
            } else {
                nl::core::log::info("ESC -> exit");
                break;
            }
        }
        // 控制台输入行持焦时屏蔽游戏热键（防把 cvar 值当热键吞掉）
        if (hudWantsKeyboard) {
            window.keyPressed(nl::core::Key::Space);   // 排干边沿
            window.keyPressed(nl::core::Key::C);
            window.keyPressed(nl::core::Key::V);
            window.keyPressed(nl::core::Key::N);
            window.keyPressed(nl::core::Key::R);
        } else {
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
                // cluster/cull 场景循环 0..4（pbr 只在 psMain 的 SPIR-V 里有意义）；
                // normal 场景循环 0..3（4=cluster 需要 CLUSTERCOLOR 属性）。
                const int cycleCount = (cluster || cullMode) ? (kShadingCount - 1) : 4;
                shading = (shading + 1) % cycleCount;
                nl::core::log::info("shading -> ", shadingName(shading));
            }
            if (window.keyPressed(nl::core::Key::R)) {
                camera.reset();
                nl::core::log::info("camera reset");
            }
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

        // M-C1：键盘 / 文本事件泵——HUD 关着也必须排干（防 window 侧队列无限增长）。
        // 事件随帧丢弃（不跨帧缓存）：ImGui 的按键状态以「本帧按下与否」为准。
        nl::core::KeyEvent keyBuf[16];
        unsigned int textBuf[32];
        const std::size_t nKeys = window.consumeKeyEvents(keyBuf, 16);
        const std::size_t nChars = window.consumeTextChars(textBuf, 32);

        nl::renderer::FrameParams fp;
        const auto vp = camera.viewProjection();
        const auto model = camera.model();
        std::memcpy(fp.mvp, vp.m, sizeof(fp.mvp));
        std::memcpy(fp.model, model.m, sizeof(fp.model));
        fp.cullBack = cullBack;
        fp.shading = shading;
        fp.wireframe = wireframe;
        if (o.pbrNaive) fp.options[0] = 1.0f;   // pbr-direct：g_mrF.z 对照开关
        if (dagMode || hizMode) {
            // M5.1/M6.0：遍历参数（bevy/zeux 投影公式的全部输入 + cut 阈值）。
            float eye[3];
            camera.eyePosition(eye);
            fp.eye[0] = eye[0]; fp.eye[1] = eye[1]; fp.eye[2] = eye[2];
            // M6.1：world→view 矩阵（Mara&Morgan 球投影，与 viewProjection 同源）。
            const auto vm = camera.view();
            std::memcpy(fp.view, vm.m, sizeof(fp.view));
            fp.projYScale = camera.projectionYScale();
            fp.nearPlane = camera.nearPlane();
            const auto e = renderer->extent();
            fp.viewportHeight = static_cast<float>(e.height);
            fp.cutThresholdPx = cutThresholdPx;
            if (hizMode) {
                fp.hizViewMip = hizViewLevel;   // M6.0：金字塔可视化层
                fp.hizEnable = hizEnabled;      // M6.2：遮挡剔除总开关
            }
        }
        // M-A5：skin 模式动画时间——--anim-time 固定（截图确定性），否则实时累加。
        if (skinMode) {
            skinTime = o.animTimeSet ? o.animTime : skinTime + dtMs / 1000.0;
            fp.animTime = skinTime;
        }

        // M-P3：物理调试叠加层（Jolt 碰撞形状线框）。build 一次（renderer 创建后），
        // 每帧 step + extractLines 喂给渲染器 LINE_LIST 叠加层。截图模式 240 帧足够
        // 落体落到地板稳态；交互模式逐帧步进即实时动画。extractLines 返回的 data
        // 指向 DebugScene 内部缓冲，本帧 renderFrame 后即失效（单帧消费模型）。
        if (o.physicsDebug) {
            physicsScene.step(1.0 / 60.0);
            const nl::physics::DebugLines dl = physicsScene.extractLines();
            fp.debugLines = true;
            fp.debugLineData = dl.data;
            fp.debugLineVertexCount = dl.vertexCount;
            // 截图最终帧打一行稳态机械证据：线顶点数 + 线框 Y 跨度（落体应已落到 ~0.5）。
            if (o.screenshot && frame + 1 == static_cast<std::uint64_t>(o.frames)) {
                float minY = 1e9f, maxY = -1e9f;
                for (std::uint32_t k = 0; k + 1 < dl.vertexCount; ++k) {
                    const float y = dl.data[k * 6 + 1];
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
                nl::core::log::info("phys-debug settled: vertices=", dl.vertexCount,
                                    " lineY[", minY, ", ", maxY, "]");
            }
        }

        // ---- M0.5：每帧把显示数据交给 HUD，控件结果由 HUD 写回同一个 POD ----
        // M-C2：hudEnabled=false 但控制台开着时也要跑 ImGui（面板藏、控制台在）。
        nl::renderer::HudFrame hud{};
        if (!o.editorGraph.empty()) hud.editor = editor.get();   // T1：编辑器窗口挂入
        hud.panelShow = hudEnabled;   // M-C2：面板可见性独立于 ImGui 帧存续
        if (hudEnabled || consoleOpen || editorActive) {
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
            // M-C1：控制台通道（事件在上方已泵出，这里只搬运进 POD）
            for (std::size_t k = 0; k < nKeys && k < 16; ++k) {
                hud.keyEvents[k].key = keyBuf[k].key;
                hud.keyEvents[k].down = keyBuf[k].down;
                hud.keyEvents[k].ctrl = keyBuf[k].ctrl;
                hud.keyEvents[k].shift = keyBuf[k].shift;
                hud.keyEvents[k].alt = keyBuf[k].alt;
            }
            hud.keyEventCount = static_cast<int>(std::min<std::size_t>(nKeys, 16));
            for (std::size_t k = 0; k < nChars && k < 32; ++k) hud.textChars[k] = textBuf[k];
            hud.textCharCount = static_cast<int>(std::min<std::size_t>(nChars, 32));
            hud.consoleOpen = consoleOpen;
            // Tab 边沿：真实按键，或截图演示模式下合成（applyTabCompletion 幂等：
            // 补全完成后前缀为空/无进展直接返回，每帧合成不产生副作用）。
            hud.consoleTab = consoleOpen && (window.keyPressed(nl::core::Key::Tab) ||
                            (o.screenshot && !o.consolePrefill.empty()));
            hud.consolePrefill = o.consolePrefill.c_str();
            // M4.1：GPU 剔除统计（DeviceInfo 由 backend 每帧 poll 后更新，只读显示）。
            hud.visibleClusters = info.visibleClusters;
            hud.totalClusters = static_cast<int>(info.totalClusters);
            hud.visibleTriangles = info.visibleTriangles;   // M5.2：-1 = 暂无数据
            // M6.0：Hi-Z 金字塔（hiz 模式才有 mipCount>0，HUD 才画滑条）。
            hud.hizMipCount = info.hizMipCount;
            hud.hizTopFar = info.hizTopFar;   // M6.1：最远表面口径
            hud.hizViewLevel = hizViewLevel;
            // M6.2：开关双向 + 剔除前/后对照（-1 = 暂无数据，HUD 画 -- 占位）。
            if (hizMode) {
                hud.hizEnable = hizEnabled;
                hud.hizOccluded = info.hizOccluded;
            }
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
                                   (hudEnabled || consoleOpen || editorActive) ? &hud : nullptr)) {
            nl::core::log::error("renderFrame failed: ", renderer->lastError());
            failed = true;
            break;
        }
        renderer->drainMessages();
        // M0.6：renderFrame 之后 renderer 可能已把最新 GPU 计时写回 hud.gpuMs，追加进曲线。
        if (hudEnabled && !o.screenshot && hud.gpuTimingAvailable && hud.gpuMs > 0.0f) {
            if (gpuHistoryCount < 120) {
                gpuHistory[gpuHistoryCount++] = hud.gpuMs;
            } else {
                std::memmove(gpuHistory, gpuHistory + 1, 119 * sizeof(float));
                gpuHistory[119] = hud.gpuMs;
            }
        }
        // M4.2：cull pass 计时同一条追加路（cullTimingAvailable 只在 cull 模式置位）。
        // M6.2：整帧 GPU 时间累计（lastGpuMs 由 backend 每帧 poll，真实值）。
        if (o.screenshot && info.lastGpuMs > 0.0f) {
            gpuMsSum += info.lastGpuMs;
            ++gpuMsFrames;
        }
        if (hudEnabled && !o.screenshot && hud.cullTimingAvailable && hud.cullMs > 0.0f) {
            if (cullHistoryCount < 120) {
                cullHistory[cullHistoryCount++] = hud.cullMs;
            } else {
                std::memmove(cullHistory, cullHistory + 1, 119 * sizeof(float));
                cullHistory[119] = hud.cullMs;
            }
        }
        if (hudEnabled || consoleOpen || editorActive) {   // 帧跑了才消费控件写回
            if (hud.resetCamera) camera.reset();
            cullBack = hud.cullBack;
            if (hizMode) hizEnabled = hud.hizEnable;   // M6.2：开关写回
            shading = std::clamp(hud.shading, 0, kShadingCount - 1);
            // 非簇场景唯一非法值是 4=cluster（无 CLUSTERCOLOR 属性）；5=pbr 合法
            //（psMain 的 pbr 分支，pbr-direct 起放开）。
            if (!cluster && !cullMode && shading == 4) shading = 3;
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
            // M6.0：金字塔 mip 滑条回读（GPU 覆盖层按它选层；只显示不换资源）。
            if (hizMode && info.hizMipCount > 0)
                hizViewLevel = std::clamp(hud.hizViewLevel, 0, info.hizMipCount - 1);
            // M5.0：cut 阈值滑条回读（M5.1 GPU 遍历消费同一参数）。只在滑条真的
            // 画出来时回读（HUD 侧条件 = frame.dagGroupCount > 0）：dag 遍历模式
            // 不显示滑条，hud.cutThresholdPx 保持 POD 默认 1.0，无条件回读会把
            // 命令行 --cut-thr 每帧覆盖回 1.0（M5.2 实测踩坑：far HUD 图 237/3936
            // = thr 被钳回 1.0 的结果；s5.1 near 没踩到纯属 thr 恰好 = 1.0）。
            if (dagTop && dagGroupCount > 0)
                cutThresholdPx = std::clamp(hud.cutThresholdPx, 0.25f, 4.0f);
            hudWantsMouse = hud.wantCaptureMouse;
            hudWantsKeyboard = hud.wantCaptureKeyboard;   // M-C1：输入行打字时屏蔽游戏热键
        }
        if (capturePath) {
            ++screenshotsSaved;
            nl::core::log::info("screenshot written: ", capturePath);
        }
        // M4.1：截图模式下把 GPU 剔除计数写进日志——自检脚本与 frustum_check.py
        // 逐数字对照的机械证据（HUD 在截图基线里是关闭的，日志才是可解析出口）。
        // M5.1：dag 遍历复用同一回读通路（visible = 被 emit 的簇数）。
        // M5.2：追加 GPU 可见三角数（同一延迟窗口的回读环形高 4 字节）。
        if (o.screenshot && (cullMode || dagMode) && info.totalClusters > 0 &&
            info.visibleClusters >= 0) {
            if (hizMode) {
                // M6.1：金字塔最高 mip 回读值（≈ 整屏最远表面径向距离的机械验收
                // 证据）+ 遮挡剔除后的可见簇数。-2 = 背景像素主导（max 口径合法）。
                // M6.2：追加剔除前/后对照（occluded of visited，整树语义）——
                // run.bat findstr 机器断言出口。
                const bool occValid = info.hizOccluded >= 0;
                const int visited = occValid ? info.visibleClusters + info.hizOccluded
                                             : info.visibleClusters;
                if (info.hizTopFar >= 0.0f) {
                    nl::core::log::info("hiz stats: pyramid top far ", info.hizTopFar,
                                        " (visible ", info.visibleClusters, " / ",
                                        info.totalClusters, " clusters",
                                        occValid ? ", occluded " : "",
                                        occValid ? info.hizOccluded : 0, " of ",
                                        visited, " visited, mips ",
                                        info.hizMipCount, ")");
                } else if (info.hizTopFar <= -1.5f) {
                    nl::core::log::info("hiz stats: pyramid top far background-dominates",
                                        " (visible ", info.visibleClusters, " / ",
                                        info.totalClusters, " clusters",
                                        occValid ? ", occluded " : "",
                                        occValid ? info.hizOccluded : 0, " of ",
                                        visited, " visited, mips ",
                                        info.hizMipCount, ")");
                } else {
                    nl::core::log::info("hiz stats: top far pending (mips ",
                                        info.hizMipCount, ")");
                }
            } else if (dagMode) {
                nl::core::log::info("dag stats: visible ", info.visibleClusters, " / ",
                                    info.totalClusters, " clusters (threshold ",
                                    cutThresholdPx, " px, tris ",
                                    info.visibleTriangles, ")");
            } else {
                nl::core::log::info("cull stats: visible ", info.visibleClusters, " / ",
                                    info.totalClusters, " clusters (frustum on, tris ",
                                    info.visibleTriangles, ")");
            }
        }

        if (frame % 15 == 0 || o.screenshot) {
            const auto e = renderer->extent();
            // 里程碑号取 stage 前缀（kStage 以 "M0.x ..." 开头），避免每版漏改硬编码。
            const std::string stageStr(st.stage);
            const std::string stagePrefix = stageStr.substr(0, stageStr.find(' '));
            std::ostringstream title;
            title << "nl-engine | " << stagePrefix << " | " << st.backend
                  << " | mode " << nl::core::modeName(o.mode)
                  << " | HUD " << (hudOn ? "on" : "off")
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
    // M6.2：追加整帧 GPU 时间均值（NVRHI timer 真值，deterministic 也 resolve）——
    // 帧时间下降验收的机器出口（CPU 循环在 draw 段轻时不敏感，GPU 口径才显性）。
    if (o.screenshot && frame > 1) {
        nl::core::log::info("frame timing: avg ",
                            dtSumMs / static_cast<double>(frame), " ms  max ",
                            dtMaxMs, " ms  over ", frame, " frames (unthrottled)");
        if (gpuMsSum > 0.0 && gpuMsFrames > 0)
            nl::core::log::info("gpu timing: avg ", gpuMsSum / gpuMsFrames,
                                " ms over ", gpuMsFrames, " resolved frames (NVRHI timer)");
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
    case nl::core::ViewMode::Dag:        // M5.1：GPU DAG 遍历选层（compute 显式栈）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/true, /*dagMode=*/true);
    case nl::core::ViewMode::Skin:       // M-A5：CPU 蒙皮展示（rigged GLB 逐帧 LBS）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/true);
    case nl::core::ViewMode::Hiz:        // M6.0：Hi-Z 深度预通道 + min 金字塔
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/true, /*dagMode=*/true, /*skinMode=*/false,
                         /*hizMode=*/true);
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
              << "  viewer.exe --hiz on|off            Hi-Z occlusion culling in hiz mode (default on;\n"
              << "                                     off = frustum/error traversal only, M6.2 toggle)\n"
              << "  viewer.exe --shading <name>        textured | normal | uv | tri-id | pbr (default textured;\n"
              << "                                     N key cycles 0..3; pbr = Cook-Torrance direct light)\n"
              << "  viewer.exe --pbr-naive             pbr debug A/B: skip sRGB->linear (naive UNORM read,\n"
              << "                                     acceptance comparison evidence)\n"
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
              << "  viewer.exe --editor <file.mg>      material graph editor window (T1); forces HUD on,\n"
              << "                                     scene background = current --mode (default normal)\n"
              << "  viewer.exe --console on|off        M-C1 dev console (backtick ` toggles at runtime;\n"
              << "                                     ESC closes; needs --hud)\n"
              << "  viewer.exe --console-exec <line>   execute console command(s) at startup\n"
              << "  viewer.exe --console-type <text>   prefill console input line (tab-complete demo)\n"
              << "  viewer.exe --physics-debug         M-P3 overlay: Jolt collision-shape wireframe\n"
              << "                                     (floor + falling box/sphere/capsule);\n"
              << "                                     forces --mode normal for depth reference\n"
              << "  viewer.exe --physics-scenario N    M-P3 scene select (default 0)\n"
              << "  viewer.exe --physics-seed N        M-P3 seed (recorded; no RNG yet)\n"
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
         o.mode == nl::core::ViewMode::DagTop || o.mode == nl::core::ViewMode::Cull ||
         o.mode == nl::core::ViewMode::Dag || o.mode == nl::core::ViewMode::Hiz)
        && !o.modelSet) {
        o.modelFile = "Suzanne.nmesh";
    }
    // M-A5：skin 模式缺省 rigged 资产（Khronos 经典行走用例）。
    if (o.mode == nl::core::ViewMode::Skin && !o.modelSet) {
        o.modelFile = "rigged/CesiumMan.glb";
    }
    // M-A5：skin 模式缺省距离拉近（4.2 是 Suzanne 口径；1.4m 人形在 4.2 外太小）。
    if (o.mode == nl::core::ViewMode::Skin && !o.distSet) {
        o.distance = 2.6f;
    }
    // M-P3：物理调试叠加层（验证脚手架 = 静态盒线框）。强制 normal 背景场景，
    // 由 Suzanne 提供深度参照以验证叠加层的深度遮挡纪律。
    if (o.physicsDebug) o.mode = nl::core::ViewMode::Normal;
    return runMode(o.mode, o);
}
