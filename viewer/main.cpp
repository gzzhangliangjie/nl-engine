// ---- M-E2：编辑器嵌入（可选启动）。windows.h 必须在最前——glfw3.h 若先定义
// APIENTRY，windows.h 再定义会 C4005（glfw 官方口径：windows.h 先含）。
#define NOMINMAX           // min/max 宏会撕碎本文件既有 std::max 调用（2469 等处）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "nl/core/assert.hpp"
#include "nl/core/camera.hpp"
#include "nl/core/console.hpp"
#include "nl/core/game_clock.hpp"  // P8 M-GL1：双帧模型（逻辑帧 60Hz accumulator）
#include "nl/core/lod_error.hpp"
#include "nl/core/log.hpp"
#include "nl/core/modes.hpp"
#include "nl/core/input.hpp"       // P8 M-I2：input-hud 模式（注入路径；单消费者铁律）
#include "nl/core/action_map.hpp"  // P8 M-I2：驾驶映射表（默认组态 + 冻结值快照）
#include "nl/core/window.hpp"
#include "nl/core/meshlet_io.hpp"  // M-E5：scene.getMaterials 读 .nmesh NSUB 子网格/材质表
#include "nl/renderer/editor.hpp"
#include "nl/audio/DebugController.hpp"  // P8 M-S2：audio.* CVar 注册（零图形）
#include "nl/core/camera_director.hpp"    // M-D3 Q-D3-4: ChassisState hook
#include "camera_mode.hpp"                 // P8 M-CAM b-segment：ViewerCamera 调度封装（orbit/free/chase/track）
#include "nl/audio/AudioWorld.hpp"       // P8 M-GL2/M-S3：场景音频（声源挂实体）
#include "nl/renderer/renderer.hpp"
#include "nl/physics/World.hpp"   // M-P3：物理调试场景（DebugScene，PIMPL 把 Jolt 头挡在 cpp 内）
#include "nl/scene/Component.hpp"   // M-E1b.0：--scene 组件库注册 + World 反序列化
#include "nl/scene/System.hpp"      // M-E1b.5：SpinSystem（Write/Read 读写集系统）
#include "nl/scene/World.hpp"
#include "nl/script/Vm.hpp"         // M-GL4：--script 脚本宿主（OnTick 接逻辑帧）
#include "nl/script/ScriptHost.hpp"
#include "nl/script/Bindings.hpp"   // ecsSetWorld（宿主侧 World 绑定）

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>              // M-E1b.0：--scene 场景文件直读
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

// ---- M-E2：编辑器嵌入（可选启动）。bridge 头不含 winsock/windows，无包含序风险。
#include "rpc_bridge.hpp"
#include "embed_window.hpp"

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
    int  visVizMode = 0;     // M7.0：visbuf 可视化档（0 cluster / 1 triangle）
    // M8.2：--rt-debug <dist|id>（0 = 命中距离灰度 / 1 = instanceId+triangleId 伪彩）。
    int  rtDebugMode = 0;
    int  shading    = 0;      // M0.8：0 textured / 1 normal / 2 uv / 3 tri-id；M1.3：4 cluster（N 键 / HUD combo）
    bool wireframe  = false;  // M0.8：线框（V 键 / --wireframe / HUD 复选框）
    bool shadingSet = false;  // 用户显式给了 --shading（lod 模式的缺省着色不同）
    bool pbrNaive   = false;  // pbr-direct：--pbr-naive 对照开关（跳过 sRGB->linear，验收证据用）
    bool probeGather = false; // M10.2：--probe-gather（probe-viz gather 态，Q13-3；
                              // 默认关保 M10.1 probe-viz 点色行为与 s10-probe 基线保形）
    bool tlasSpin   = false;  // M8.1：--tlas-spin（TLAS 实例每帧 spin，光栅化不受影响）
    bool tlasProbe  = false;  // M8.1：--tlas-probe（RayQuery 探针 dispatch + hit t 读回日志）
    bool inputDemo  = false;  // P8 M-I2：--input-demo（input-hud 模式的帧序号驱动注入脚本）
    bool driveDemo  = false;  // M-D3 Q-D3-2 A：--drive-demo（帧序号驾驶脚本，确定性截图源）
    bool driveHud   = false;  // M-D3 Q-D3-3 A：--drive-hud（默认关的独立参数窗口）
    std::string cameraMode = "orbit";   // P8 M-CAM b-segment：初始相机模式 orbit|free|chase|track
    std::string cameraTrack;            // --camera-track <file>：track 模式轨迹 tsv
    int  inputDemoStart = 5;  // 脚本起始迭代号（1-based；W@start D@+1 Space@+2 Esc@+3）
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
    std::string scenePath;                  // M-E1b.0：--scene <file>（World JSON，非 assets 资产）
    // M-E1b.5 渲染用例：--tick off|serial|parallel。off（缺省）= 静态场景（既有
    // 基线零风险）；serial/parallel = 帧循环注册 spin+read 系统并逐帧 tick（固定
    // dt=1/60，截图确定性——parallel/serial 同帧数逐字节一致）。
    std::string tickMode = "off";
    std::string editorGraph;                // T1：--editor <file.mg> 挂材质图编辑器
    std::string modeText = "normal";
    nl::core::ViewMode mode = nl::core::ViewMode::Normal;
    int  lod = 0;             // M2.3：--lod N，lod 模式的初始层（HUD 滑条可再切）
    int  dagGroup = -1;       // M3.2：--dag-group N，dag-top 模式初始下钻组（-1 = 纯顶层）
    // M-A5：skin 模式动画时间（秒）。未指定 = 实时累加；指定 = 固定（截图确定性）。
    bool animTimeSet = false;
    float animTime = 0.0f;
    // M-GL1：--fixed-dt <ms> 把渲染帧 delta 钉成常量（喂 GameClock + 帧统计）。
    // 0 = 关（真实 dt）。截图/对拍确定性用：暂停帧、倍速帧的 PNG 才能逐字节对拍。
    // 优先级：--anim-time > 时钟（animTimeSet 时 skinTime 直接钉值，绕过 GameClock，
    // 既有基线逐字节零风险）；--fixed-dt 只钉「每帧喂多少」，不钉绝对时间。
    double fixedDtMs = 0.0;
    // M-GL3：--interp 渲染帧插值（alpha = accumulator/step，bevy Fixed
    // overstep/timestep 同口径）。渲染专用不回写 sim state；截图模式恒走
    // 不插值原路径（PNG 与不插值逐字节同），插值证据走捕获帧 probe 日志。
    bool interp = false;
    // M-GL4：--script <示例工程目录>（scene 模式）——脚本宿主装载 + 宿主级
    // OnTick 接逻辑帧（nl-script 工具同款产物定位：proj/bin/Debug/net8.0/）。
    // 空串 = 不装载脚本（viewer 默认零脚本依赖）。typeName 缺省推导
    // "NlScript.Examples.<stem>Script, <stem>"；bootstrap 缺省仓库根相对路径。
    std::string scriptProject;                 // --script
    std::string scriptType;                    // --script-type（可省，缺省推导）
    std::string scriptBootstrap = "script/bootstrap/bin/Debug/net8.0/NlScriptBootstrap.dll"; // --script-bootstrap
    // M-C1：开发者控制台（` 呼出）。console 开关 + 预执行命令行 + 输入行预填。
    bool consoleOpen = false;
    std::vector<std::string> consoleExecLines;  // 启动时按序执行（演示/截图证据）
    std::string consolePrefill;                 // 输入行预填（Tab 补全演示）
    bool physicsDebug = false;                   // M-P3：物理调试叠加层（Jolt 碰撞形状线框）
    int  physicsScenario = 0;                    // M-P3：--physics-scenario N
    std::uint32_t physicsSeed = 0;               // M-P3：--physics-seed N（当前仅记录）
    bool particles = false;                      // M-PT0：粒子叠加层（instanced billboard 喷泉）
    std::string nfxPath;                         // M-PT1：.nfx v1 加载（隐含 --particles）
    bool editorBridge = false;                   // M-E2：--editor-bridge 连接外置编辑器（连不上降级单机）
    // M-E6：传输 endpoint（TCP 默认 / 命名管道可选）+ 多视口路由键。
    // 同一编辑器里跑多个 viewer 进程时，各带一个不同 --view-id，各自嵌入独立面板。
    std::string editorEndpoint = "tcp://127.0.0.1:29110";
    std::string viewId = "main";
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") o.selftest = true;
        else if (a == "--editor-bridge") o.editorBridge = true;   // M-E2：外置编辑器嵌入
        else if (a == "--editor-endpoint" && i + 1 < argc) o.editorEndpoint = argv[++i]; // M-E6
        else if (a == "--view-id" && i + 1 < argc) o.viewId = argv[++i];                 // M-E6
        else if (a == "--help") o.help = true;
        else if (a == "--no-debug") o.debug = false;
        else if (a == "--pbr-naive") o.pbrNaive = true;   // pbr-direct：验收对照（过亮的错误路径）
        else if (a == "--probe-gather") o.probeGather = true;   // M10.2：final gather 态
        // M8.1：TLAS 实例每帧 spin / RayQuery 探针（仅 dag/hiz 等 BLAS 就位模式生效）。
        else if (a == "--tlas-spin") o.tlasSpin = true;
        else if (a == "--tlas-probe") o.tlasProbe = true;
        else if (a == "--rt-debug" && i + 1 < argc) {   // M8.2：rt 模式调试可视化档
            const std::string v = argv[++i];
            if (v == "dist" || v == "0") o.rtDebugMode = 0;
            else if (v == "id" || v == "1") o.rtDebugMode = 1;
            else {
                std::cerr << "invalid --rt-debug '" << v << "' (expected dist|id)\n";
                std::exit(1);
            }
        }
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
        else if (a == "--scene" && i + 1 < argc) {
            // M-E1b.0：ECS 场景文件（World::serialise 的 JSON）。路径按传入直读
            //（不是 assets 白名单资产）；ASCII + 存在性校验在 runScene 落地，
            // 失败走用法错误码 2。
            o.scenePath = argv[++i];
        }
        else if (a == "--editor" && i + 1 < argc) {
            // T1：材质图编辑器（material_graph v1 文本文件）。
            o.editorGraph = argv[++i];
        }
        else if (a == "--cull" && i + 1 < argc) {
            const std::string v = argv[++i];
            o.cullBack = (v != "none" && v != "off" && v != "0");
        } else if (a == "--camera" && i + 1 < argc) {
            o.cameraMode = argv[++i];
            if (o.cameraMode != "orbit" && o.cameraMode != "free" &&
                o.cameraMode != "chase" && o.cameraMode != "track") {
                std::cerr << "invalid --camera '" << o.cameraMode
                          << "' (expected orbit|free|chase|track)\n";
                o.help = true;
            }
        } else if (a == "--camera-track" && i + 1 < argc) {
            o.cameraTrack = argv[++i];
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
        } else if (a == "--vis-viz" && i + 1 < argc) {
            // M7.0：vis buffer 解码档（仅 visbuf 模式消费；其它模式恒 0 不读）。
            const std::string v = argv[++i];
            if (v == "cluster" || v == "0") o.visVizMode = 0;
            else if (v == "triangle" || v == "tri" || v == "1") o.visVizMode = 1;
            else {
                std::cerr << "invalid --vis-viz '" << v << "' (expected cluster|triangle)\n";
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
        } else if (a == "--particles") {           // M-PT0：粒子叠加层（instanced billboard 喷泉）
            o.particles = true;
        } else if (a == "--nfx" && i + 1 < argc) { // M-PT1：.nfx v1 加载（隐含 --particles）
            o.nfxPath = argv[++i];
            o.particles = true;
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
        } else if (a == "--tick" && i + 1 < argc) {   // M-E1b.5：off|serial|parallel
            o.tickMode = argv[++i];
            if (o.tickMode != "off" && o.tickMode != "serial" && o.tickMode != "parallel") {
                std::cerr << "invalid --tick '" << o.tickMode << "' (expected off|serial|parallel)\n";
                std::exit(1);
            }
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
        } else if (a == "--fixed-dt" && i + 1 < argc) {
            // M-GL1：钉渲染帧 dt（毫秒，>0）。供 GameClock 双帧模型的对拍确定性。
            const double v = std::atof(argv[++i]);
            if (!(v > 0.0)) {
                std::cerr << "invalid --fixed-dt (expected positive ms)\n";
                std::exit(1);
            }
            o.fixedDtMs = v;
        } else if (a == "--interp") {
            // M-GL3：渲染帧插值开关（见 Options::interp 注释）。
            o.interp = true;
        } else if (a == "--script" && i + 1 < argc) {
            // M-GL4：脚本示例工程目录（scene 模式装载脚本宿主 + OnTick 接逻辑帧）。
            o.scriptProject = argv[++i];
        } else if (a == "--script-type" && i + 1 < argc) {
            o.scriptType = argv[++i];
        } else if (a == "--script-bootstrap" && i + 1 < argc) {
            o.scriptBootstrap = argv[++i];
        } else if (a == "--screenshot" && i + 1 < argc) {
            o.screenshot = true;
            o.screenshotPath = argv[++i];
        } else if (a == "--color" && i + 1 < argc) {
            o.color = std::atoi(argv[++i]) % kColorCount;
        } else if (a == "--frames" && i + 1 < argc) {
            o.frames = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--input-demo") {
            // P8 M-I2：注入脚本开关；可选起始迭代号（1-based，缺省 5；仅 input-hud
            // 模式消费）。无值形式兼容 flag 直跟其它 --arg 的用法。
            o.inputDemo = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                o.inputDemoStart = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--drive-demo") {
            o.driveDemo = true;
        } else if (a == "--drive-hud") {
            o.driveHud = true;
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
    // 锚点跟随当前 kStage 前缀；每次 kStage bump 同步改这里（M-N0 修存量债：
    // 原硬编码 "S7.2"，M8.0 bump 后从未重跑 selftest，s8-blas 收口漏网）。
    check(std::strstr(st.stage, "S8.2") != nullptr, "S8.2 stage reported");

    nl::renderer::FrameParams fp;
    check(fp.cullBack && sizeof(fp.mvp) == 64 && fp.lightDir[3] > 0.0f,
          "frame params (MVP/cull/light) POD defaults");
    check(fp.shading == 0 && !fp.wireframe,
          "M0.8 debug shading / wireframe POD defaults (= M0.7 behavior)");
    check(nl::renderer::hudSelfTest(), "HUD POD + projection arithmetic");
    std::string shaderReport;
    check(nl::renderer::shaderArtifactsSelfTest(shaderReport),
          "Slang single-source -> SPIR-V + DXIL (66 artifacts)");
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
// ---- M-D3 (Q-D3-2 A / GAME-DEV-ORDER 第 3 步 "console 实调") ----
struct VehicleCVars {
    nl::core::console::CVar* throttleScale = nullptr;
    nl::core::console::CVar* steerScale    = nullptr;
    nl::core::console::CVar* debug         = nullptr;
};
VehicleCVars registerVehicleConsole(bool* driveHudOn) {
    using namespace nl::core::console;
    Console& c = Console::get();
    VehicleCVars v;
    v.throttleScale = &c.registerFloat("vehicle.throttleScale", 1.0,
                                       "M-D3 drive throttle multiplier [0,2]");
    v.steerScale    = &c.registerFloat("vehicle.steerScale", 1.0,
                                       "M-D3 drive steer multiplier [0,2]");
    v.debug         = &c.registerBool("vehicle.debug", false,
                                      "show the M-D3 drive debug window");
    v.debug->setOnChanged([driveHudOn](const CVar& cv) { *driveHudOn = cv.value().b; });
    return v;
}

// Q-D3-4 "folded hook": projection source resolution lives in ONE place.
// M-D3 ships exactly one active source (OrbitCamera). Camera b-segment only
// replaces the Chase return below AFTER M-D3 closes -- no camera glue, no Chase
// baselines, no five-anchor camera acceptance in this milestone.
static auto resolveViewProjection(nl::viewer::ViewerCamera& camera,
                                  nl::physics::DebugScene& scene)
    -> decltype(camera.viewProjection()) {
    (void)scene;   // P8 M-CAM b-segment：chassis 钩子已在主循环每帧 feed（camera.chassis）
    return camera.viewProjection();
}

std::function<void()> registerViewerConsole(nl::viewer::ViewerCamera* camera, bool* hudEnabled) {
    using namespace nl::core::console;
    Console& c = Console::get();
    c.installBuiltins();

    // cvar 初值取当前实际状态（相机可能被 --dist/--fov 类 CLI 预置过，别写死 45/4.2）
    CVar* const fov  = &c.registerFloat("cam.fov", camera->fovYDegrees(), "vertical fov in degrees (20..120)");
    CVar* const dist = &c.registerFloat("cam.dist", camera->distance(), "orbit distance (0.45..20)");
    CVar* const hud  = &c.registerBool("hud.show", *hudEnabled, "toggle the HUD panel");
    // P8 M-CAM b-segment + M-CAM4：相机模式 / 平滑 / 抖动 / 子相机参数（运行时调参）
    c.registerCommand("cam.mode", "set camera mode: orbit|free|chase|track",
        [camera](const std::vector<std::string>& args) {
            if (args.empty())
                return CommandResult{false, "usage: cam.mode <orbit|free|chase|track>"};
            const std::string& m = args[0];
            if (m == "orbit")      camera->setMode(nl::viewer::CameraMode::Orbit);
            else if (m == "free")  camera->setMode(nl::viewer::CameraMode::Free);
            else if (m == "chase") camera->setMode(nl::viewer::CameraMode::Chase);
            else if (m == "track") camera->setMode(nl::viewer::CameraMode::Track);
            else return CommandResult{false, "unknown mode: " + m};
            return CommandResult{true, std::string("camera mode -> ") + camera->modeName()};
        });
    c.registerBool("cam.smooth.on", camera->smoother.enabled,
                   "M-CAM4：位置/朝向二阶弹簧平滑（默认关，开时叠加在 chase/free 阻尼之上）")
        .setOnChanged([camera](const CVar& cv) { camera->smoother.enabled = cv.value().b; });
    c.registerFloat("cam.smooth.time", camera->smoother.smoothTime, "M-CAM4：平滑收敛时间常数（秒）")
        .setOnChanged([camera](const CVar& cv) { camera->smoother.smoothTime = cv.value().f; });
    c.registerCommand("cam.shake.add", "M-CAM4：注入 trauma [0..1]（确定性抖动，trauma² 模型）",
        [camera](const std::vector<std::string>& args) {
            const float v = args.empty() ? 1.0f
                                        : static_cast<float>(std::strtod(args[0].c_str(), nullptr));
            camera->trauma.addTrauma(v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
            return CommandResult{true, "trauma -> " + std::to_string(camera->trauma.trauma01())};
        });
    c.registerFloat("cam.shake.decay", camera->trauma.decay, "M-CAM4：trauma 每秒线性衰减")
        .setOnChanged([camera](const CVar& cv) { camera->trauma.decay = cv.value().f; });
    c.registerFloat("cam.shake.amp", camera->trauma.amplitude, "M-CAM4：trauma 最大偏移（弧度）")
        .setOnChanged([camera](const CVar& cv) { camera->trauma.amplitude = cv.value().f; });
    c.registerFloat("cam.shake.freq", camera->trauma.frequency, "M-CAM4：trauma 抖动频率（Hz 风格，确定性非 wall-clock）")
        .setOnChanged([camera](const CVar& cv) { camera->trauma.frequency = cv.value().f; });
    c.registerFloat("cam.chase.dist", camera->director.chase().distance(), "Chase 机位臂长（clamp 1..50）")
        .setOnChanged([camera](const CVar& cv) { camera->director.chase().setDistance(cv.value().f); });
    c.registerFloat("cam.chase.stiff", camera->director.chase().stiffness(), "Chase 位置阻尼系数（clamp 0.1..50）")
        .setOnChanged([camera](const CVar& cv) { camera->director.chase().setStiffness(cv.value().f); });
    c.registerFloat("cam.chase.height", camera->director.chase().heightOffset(), "Chase 机位抬升（clamp 0..10）")
        .setOnChanged([camera](const CVar& cv) { camera->director.chase().setHeightOffset(cv.value().f); });
    c.registerFloat("cam.chase.lead", camera->director.chase().leadTime(), "Chase look-ahead 前视时长（clamp 0..1）")
        .setOnChanged([camera](const CVar& cv) { camera->director.chase().setLeadTime(cv.value().f); });
    c.registerFloat("cam.free.speed", camera->director.free().speed(), "Free 相机移动速度（世界单位/秒，clamp 0..500）")
        .setOnChanged([camera](const CVar& cv) { camera->director.free().setSpeed(cv.value().f); });

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

// M-GL1：game.* 控制台命令——双帧模型运行时控制（PLAN §3「运行时控制面」）。
//   game.pause [on|off]    无参=翻转；暂停=逻辑时钟停摆（bevy 语义，simTime 冻结，
//                          渲染帧照常——HUD 活、相机活，画面静止）。
//   game.timeScale <f>     无参=查询；[0,1024] 钳制（GameClock::setTimeScale 同口径）。
//   game.status            打印 simTime/ticks/paused/scale/alpha（截图日志证据行）。
//   game.tickgroups        打印逻辑帧组序（枚举序=执行序）+ 各组注册状态（M-GL5）。
// 命令（非 CVar）：--console-exec "game.pause" 字面即用，且 pause 无「赋值后回读」
// 的每帧同步需求（与 cam.fov 的 CVar 选型差异）。
void registerGameConsole(nl::core::GameClock* clock) {
    using namespace nl::core::console;
    Console& c = Console::get();
    c.registerCommand("game.pause", "toggle/set logic clock pause (on|off)",
                      [clock](const std::vector<std::string>& args) {
        bool target;
        if (args.empty()) {
            target = !clock->paused();
        } else if (args[0] == "on" || args[0] == "1") {
            target = true;
        } else if (args[0] == "off" || args[0] == "0") {
            target = false;
        } else {
            return CommandResult{false, "usage: game.pause [on|off]"};
        }
        clock->setPaused(target);
        // P8 M-GL2 拍板（2026-11-10 用户选 B，UE 流派）：game.pause 连音频播放
        // 一起冻——复用 M-S2 DebugController.setFrozen（与 audio.freeze 同一机制），
        // 恢复时同步解冻。「游戏暂停」= 世界 + 声音全冻结。
        nl::audio::AudioDebugController::get().setFrozen(target);
        return CommandResult{true, std::string("game.pause -> ") +
                             (target ? "on (logic clock frozen; render frames keep running)"
                                     : "off (logic clock running)")};
    });
    c.registerCommand("game.timeScale", "query/set logic time scale ([0,1024])",
                      [clock](const std::vector<std::string>& args) {
        if (args.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", clock->timeScale());
            return CommandResult{true, std::string("game.timeScale = ") + buf};
        }
        char* end = nullptr;
        const double v = std::strtod(args[0].c_str(), &end);
        if (end == args[0].c_str() || *end != '\0')
            return CommandResult{false, "game.timeScale: not a number: " + args[0]};
        clock->setTimeScale(v);   // 越界/NaN 钳制在 GameClock 内（selftest 锚定）
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", clock->timeScale());
        return CommandResult{true, std::string("game.timeScale -> ") + buf};
    });
    c.registerCommand("game.status", "print logic clock state (simTime/ticks/paused/scale/alpha)",
                      [clock](const std::vector<std::string>&) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "game: simTime=%.6fs ticks=%llu paused=%d scale=%g alpha=%.3f",
                      clock->simTime(),
                      static_cast<unsigned long long>(clock->tickCount()),
                      clock->paused() ? 1 : 0, clock->timeScale(), clock->alpha());
        return CommandResult{true, std::string(buf)};
    });
    // M-GL5：组序显式契约的运行时视图——order 列恒为枚举序（执行序），active 列
    // 只列本模式已注册组。--console-exec "game.tickgroups" 即验收锚（expectLog）。
    c.registerCommand("game.tickgroups", "print logic-frame tick group order and registration",
                      [clock](const std::vector<std::string>&) {
        const nl::core::TickScheduler& s = clock->scheduler();
        std::string order, active;
        for (int g = 0; g < static_cast<int>(nl::core::TickGroup::Count); ++g) {
            const auto grp = static_cast<nl::core::TickGroup>(g);
            if (g) order += " -> ";
            order += nl::core::tickGroupName(grp);
            order += s.has(grp) ? "(on)" : "(off)";
            if (s.has(grp)) {
                if (!active.empty()) active += " -> ";
                active += nl::core::tickGroupName(grp);
            }
        }
        return CommandResult{true, "game.tickgroups: order " + order + " | active " +
                             (active.empty() ? "(none)" : active)};
    });
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
// ---- M-E2：编辑器嵌入辅助（--editor-bridge 时启用，否则零开销零触碰）----
// 职责：连接编辑器 RpcHost(29110) + 上报 HWND（engine.announce）+ 每帧泵 update。
// attach 由编辑器主动 SetParent（EDITOR-PLAN §6 拍板），引擎侧无 attach handler。
// 连接失败 = 静默降级单机运行（编辑器是可选外挂，崩溃隔离：引擎照常自渲染）。
struct EditorEmbed {
    nl::editor::RpcBridge bridge;
    bool enabled = false;      // M-E4a：--editor-bridge 且非截图模式才启用泵/重连
    bool connected = false;
    void* engineHwnd = nullptr;
    nl::core::OrbitCamera* camera = nullptr;   // M-E3：相机 RPC 的操作目标（两处主循环各 bind 一次）
    std::chrono::steady_clock::time_point nextRetry{};  // M-E4a：重连退避时间点
    // M-E6：endpoint 与多视口路由键（由 --editor-endpoint / --view-id 注入）
    std::string endpoint = "tcp://127.0.0.1:29110";
    std::string viewId = "main";

    void start(void* engineHwnd_, bool enable,
               const std::string& ep = "tcp://127.0.0.1:29110",
               const std::string& vid = "main") {
        engineHwnd = engineHwnd_;
        enabled = enable;
        endpoint = ep;
        viewId = vid.empty() ? "main" : vid;
        if (!enabled) return;
        bridge.registerHandler("engine.echo",
                               [](const nl::editor::RpcValue& p) { return p; });
        tryConnect(/*first=*/true);
    }

    // M-E4a/M-E4b：连接 + announce（caps 带窗体像素尺寸与渲染后端名）。
    // 连不上不再永久降级：pump() 每 2s 重试，编辑器后启/重启都能被接管。
    void tryConnect(bool first = false) {
        if (bridge.connect(endpoint)) {
            connected = true;
            nl::editor::msgpack::Map caps;
            RECT rc{};
            if (engineHwnd && GetClientRect(static_cast<HWND>(engineHwnd), &rc)) {
                caps["w"] = (std::int64_t)(rc.right - rc.left);
                caps["h"] = (std::int64_t)(rc.bottom - rc.top);
            }
            caps["backend"] = std::string(NL_BACKEND_NAME);
            // M-E6：viewId 让编辑器把本进程 HWND 嵌进对应视口面板
            nl::editor::EmbedWindow::announce(bridge, engineHwnd,
                                              static_cast<int>(::GetCurrentProcessId()), 1,
                                              nl::editor::RpcValue(std::move(caps)), viewId);
            nl::core::log::info(std::string("editor bridge: connected view=") + viewId
                                + " endpoint=" + bridge.endpoint()
                                + " (backend=" + NL_BACKEND_NAME + ")");
        } else {
            nextRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            if (first) nl::core::log::info(std::string("editor bridge: no editor at ") + endpoint
                                           + " -> standalone (retry every 2s)");
        }
    }

    // M-E3：相机控制 handler（编辑器插件经 RPC 驱动引擎相机）。
    // params 统一命名 Map（RPC-SPEC）；set 类回读 clamp 后实际值，编辑器 UI 直接对齐。
    // 未连接也照常注册（无 RPC 泵就不会被调到），standalone 零开销。
    void bindCamera(nl::core::OrbitCamera* cam) {
        camera = cam;
        if (!camera) return;
        auto field = [](const nl::editor::RpcValue& p, const char* k, double def) -> double {
            if (p.isMap()) {
                auto it = p.asMap().find(k);
                if (it != p.asMap().end() && it->second.isDouble()) return it->second.asDouble();
                if (it != p.asMap().end() && it->second.isInt()) return (double)it->second.asInt();
            }
            return def;
        };
        bridge.registerHandler("camera.getState", [this](const nl::editor::RpcValue&) -> nl::editor::RpcValue {
            nl::editor::msgpack::Map m;
            m["fov"]   = (double)camera->fovYDegrees();
            m["dist"]  = (double)camera->distance();
            m["yaw"]   = (double)camera->yawDegrees();
            m["pitch"] = (double)camera->pitchDegrees();
            return nl::editor::RpcValue(std::move(m));
        });
        bridge.registerHandler("camera.setFov", [this, field](const nl::editor::RpcValue& p) -> nl::editor::RpcValue {
            camera->setFovYDegrees((float)field(p, "fov", camera->fovYDegrees()));  // 内部 clamp 20..120
            return nl::editor::RpcValue((double)camera->fovYDegrees());
        });
        bridge.registerHandler("camera.setDistance", [this, field](const nl::editor::RpcValue& p) -> nl::editor::RpcValue {
            camera->setDistance((float)field(p, "dist", camera->distance()));       // 内部 clamp
            return nl::editor::RpcValue((double)camera->distance());
        });
        bridge.registerHandler("camera.orbit", [this, field](const nl::editor::RpcValue& p) -> nl::editor::RpcValue {
            camera->orbit((float)field(p, "dx", 0.0), (float)field(p, "dy", 0.0));  // 增量（度）
            nl::editor::msgpack::Map m;
            m["yaw"]   = (double)camera->yawDegrees();
            m["pitch"] = (double)camera->pitchDegrees();
            return nl::editor::RpcValue(std::move(m));
        });
        bridge.registerHandler("camera.reset", [this](const nl::editor::RpcValue&) -> nl::editor::RpcValue {
            camera->reset();
            nl::core::log::info("camera reset (via editor RPC)");
            return nl::editor::RpcValue();  // nil = ack
        });
    }

    // M-E4d：场景树数据源。runScene 传 &objects（每帧原地刷新，指针即实时）；
    // runNormal 传 nullptr（model 模式：单模型无实例表）。
    std::vector<nl::renderer::SceneObjectDesc>* sceneObjects = nullptr;

    void bindScene(std::vector<nl::renderer::SceneObjectDesc>* objs) {
        sceneObjects = objs;
        if (!camera) return;
        // getState 两模式都注册：model 模式返回 mode="model" 空表，编辑器可优雅显示
        bridge.registerHandler("scene.getState", [this](const nl::editor::RpcValue&) -> nl::editor::RpcValue {
            nl::editor::msgpack::Array arr;
            if (sceneObjects) {
                arr.reserve(sceneObjects->size());
                for (const auto& d : *sceneObjects) {
                    nl::editor::msgpack::Map o;
                    o["mesh"] = std::string(d.meshFile ? d.meshFile : "?");
                    // 世界矩阵平移分量（列主序 m[12..14]）
                    nl::editor::msgpack::Array pos;
                    pos.push_back((double)d.world[12]);
                    pos.push_back((double)d.world[13]);
                    pos.push_back((double)d.world[14]);
                    o["pos"] = nl::editor::RpcValue(std::move(pos));
                    nl::editor::msgpack::Array tint;
                    tint.push_back((double)d.tint[0]);
                    tint.push_back((double)d.tint[1]);
                    tint.push_back((double)d.tint[2]);
                    o["tint"] = nl::editor::RpcValue(std::move(tint));
                    o["rough"] = (double)d.roughness;
                    o["metal"] = (double)d.metallic;
                    arr.push_back(nl::editor::RpcValue(std::move(o)));
                }
            }
            nl::editor::msgpack::Map m;
            m["mode"]  = std::string(sceneObjects ? "scene" : "model");
            m["count"] = (std::int64_t)(sceneObjects ? sceneObjects->size() : 0);
            m["objects"] = nl::editor::RpcValue(std::move(arr));
            m["yaw"]   = (double)camera->yawDegrees();
            m["pitch"] = (double)camera->pitchDegrees();
            m["dist"]  = (double)camera->distance();
            return nl::editor::RpcValue(std::move(m));
        });
        if (!sceneObjects) return;   // model 模式：无实例表，focus 无意义
        // 聚焦：把 orbit 相机对准对象方向（eye 落在 origin→P 射线上，距离取 |P|·2.5）。
        // 相机恒看 origin，因此对准 = 让 eye 方向与 P 同向，对象近似画面中心。
        bridge.registerHandler("scene.focus", [this](const nl::editor::RpcValue& p) -> nl::editor::RpcResult {
            auto field = [](const nl::editor::RpcValue& v, const char* k) -> double {
                if (v.isMap()) {
                    auto it = v.asMap().find(k);
                    if (it != v.asMap().end() && it->second.isInt()) return (double)it->second.asInt();
                    if (it != v.asMap().end() && it->second.isDouble()) return it->second.asDouble();
                }
                return -1.0;
            };
            const long long idx = (long long)field(p, "i");
            if (idx < 0 || idx >= (long long)sceneObjects->size())
                return nl::editor::RpcError{100, "scene.focus: bad index"};
            const auto& d = (*sceneObjects)[(size_t)idx];
            const float px = d.world[12], py = d.world[13], pz = d.world[14];
            const float rXZ = std::sqrt(px * px + pz * pz);
            const float len = std::sqrt(px * px + py * py + pz * pz);
            if (len < 1e-4f)
                return nl::editor::RpcError{100, "scene.focus: object at origin"};
            constexpr float kPiF = 3.14159265358979323846f;
            const float yawDeg   = std::atan2(px, pz) * 180.0f / kPiF;
            const float pitchDeg = std::atan2(py, rXZ) * 180.0f / kPiF;
            camera->setOrbit(yawDeg, pitchDeg, len * 2.5f);
            nl::core::log::info("scene.focus[", (long long)idx, "] -> yaw=", yawDeg, " pitch=", pitchDeg, " dist=", len * 2.5f);
            nl::editor::msgpack::Map m;
            m["yaw"]  = (double)camera->yawDegrees();
            m["pitch"]= (double)camera->pitchDegrees();
            m["dist"] = (double)camera->distance();
            return nl::editor::RpcValue(std::move(m));
        });
    }

    void pump() {
        if (!enabled) return;
        if (connected) {
            bridge.update();   // 泵入站 + 心跳（编辑器挂了也照跑）
            if (!bridge.isConnected()) {   // M-E4a：掉线转重连态，2s 后重试
                connected = false;
                nextRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
                nl::core::log::info("editor bridge lost -> will retry connect every 2s");
            }
        } else if (std::chrono::steady_clock::now() >= nextRetry) {
            tryConnect();
        }
    }

    // ---- M-E5：材质检查接口 ----
    std::string modelFile_;   // runNormal 传入（model 模式 .nmesh 探测用）；scene 模式留空

    // .nmesh NSUB 子网格/材质表读取（assets/meshlets/<file>；材质 id = 子网格序号，
    // M7.2 约定）。非 .nmesh 返回 nil；读取失败返回 {error: msg}。
    nl::editor::RpcValue nmeshInfo(const std::string& file) {
        if (file.size() < 6 || file.compare(file.size() - 6, 6, ".nmesh") != 0)
            return nl::editor::RpcValue();   // nil = 非 .nmesh 资产
#ifdef NL_MESHLET_DIR
        const std::string path = std::string(NL_MESHLET_DIR "/") + file;
#else
        const std::string path = "assets/meshlets/" + file;
#endif
        nl::core::NMeshData nm;
        std::string err;
        if (!nl::core::loadNMesh(path.c_str(), nm, err)) {
            nl::editor::msgpack::Map e;
            e["error"] = err;
            return nl::editor::RpcValue(std::move(e));
        }
        nl::editor::msgpack::Map m;
        m["name"] = nm.name;
        m["meshlets"] = (std::int64_t)nm.meshlets.size();
        const std::int64_t subCount = nm.submeshesPresent ? (std::int64_t)nm.submeshes.size() : 1;
        m["submeshCount"] = subCount;
        nl::editor::msgpack::Array subs;
        if (nm.submeshesPresent) {
            for (std::size_t i = 0; i < nm.submeshes.size(); ++i) {
                nl::editor::msgpack::Map s;
                s["materialId"]   = (std::int64_t)i;   // 材质 id = 子网格序号（M7.2）
                s["clusterFirst"] = (std::int64_t)nm.submeshes[i].clusterFirst;
                s["clusterCount"] = (std::int64_t)nm.submeshes[i].clusterCount;
                subs.push_back(nl::editor::RpcValue(std::move(s)));
            }
        }
        m["submeshes"] = nl::editor::RpcValue(std::move(subs));
        return nl::editor::RpcValue(std::move(m));
    }

    // M-E5：scene.getMaterials（读）+ scene.setMaterial（写，仅 scene 模式）。
    // setMaterial 改 (*sceneObjects)[i] 的 tint/rough/metal——viewer 每帧原地刷新
    // 传给 renderer updateEcsObjects，改动下一帧可见。params 统一命名 Map，逐字段可选。
    void bindMaterials(const char* modelFile) {
        if (modelFile) modelFile_ = modelFile;
        bridge.registerHandler("scene.getMaterials", [this](const nl::editor::RpcValue&) -> nl::editor::RpcValue {
            nl::editor::msgpack::Map m;
            if (!sceneObjects) {
                m["mode"] = std::string("model");
                m["file"] = modelFile_;
                m["nmesh"] = nmeshInfo(modelFile_);
                return nl::editor::RpcValue(std::move(m));
            }
            m["mode"]  = std::string("scene");
            m["count"] = (std::int64_t)sceneObjects->size();
            nl::editor::msgpack::Array arr;
            arr.reserve(sceneObjects->size());
            for (std::size_t i = 0; i < sceneObjects->size(); ++i) {
                const auto& d = (*sceneObjects)[i];
                nl::editor::msgpack::Map o;
                o["i"] = (std::int64_t)i;
                o["mesh"] = std::string(d.meshFile ? d.meshFile : "?");
                nl::editor::msgpack::Array tint;
                tint.push_back((double)d.tint[0]);
                tint.push_back((double)d.tint[1]);
                tint.push_back((double)d.tint[2]);
                o["tint"]  = nl::editor::RpcValue(std::move(tint));
                o["rough"] = (double)d.roughness;
                o["metal"] = (double)d.metallic;
                arr.push_back(nl::editor::RpcValue(std::move(o)));
            }
            m["objects"] = nl::editor::RpcValue(std::move(arr));
            return nl::editor::RpcValue(std::move(m));
        });
        bridge.registerHandler("scene.setMaterial", [this](const nl::editor::RpcValue& p) -> nl::editor::RpcResult {
            if (!sceneObjects)
                return nl::editor::RpcError{100, "scene.setMaterial: model mode has no instance table"};
            if (!p.isMap())
                return nl::editor::RpcError{100, "scene.setMaterial: params must be a map"};
            auto field = [](const nl::editor::RpcValue& v, const char* k) -> double {
                auto it = v.asMap().find(k);
                if (it != v.asMap().end() && it->second.isDouble()) return it->second.asDouble();
                if (it != v.asMap().end() && it->second.isInt())    return (double)it->second.asInt();
                return -1.0;
            };
            const long long idx = (long long)field(p, "i");
            if (idx < 0 || idx >= (long long)sceneObjects->size())
                return nl::editor::RpcError{100, "scene.setMaterial: bad index"};
            auto& d = (*sceneObjects)[(std::size_t)idx];
            auto itTint = p.asMap().find("tint");
            if (itTint != p.asMap().end() && itTint->second.isArray() &&
                itTint->second.asArray().size() == 3) {
                const auto& ta = itTint->second.asArray();
                for (int c = 0; c < 3; ++c) {
                    if (ta[c].isDouble()) {
                        d.tint[c] = (float)ta[c].asDouble();
                        d.tint[c] = std::min(1.0f, std::max(0.0f, d.tint[c]));
                    }
                }
            }
            bool touched = false;
            auto itRough = p.asMap().find("rough");
            if (itRough != p.asMap().end() && itRough->second.isDouble()) {
                d.roughness = std::min(1.0f, std::max(0.0f, (float)itRough->second.asDouble()));
                touched = true;
            }
            auto itMetal = p.asMap().find("metal");
            if (itMetal != p.asMap().end() && itMetal->second.isDouble()) {
                d.metallic = std::min(1.0f, std::max(0.0f, (float)itMetal->second.asDouble()));
                touched = true;
            }
            nl::editor::msgpack::Map m;
            m["ok"] = (std::int64_t)1;
            m["i"] = idx;
            nl::editor::msgpack::Array tint;
            tint.push_back((double)d.tint[0]);
            tint.push_back((double)d.tint[1]);
            tint.push_back((double)d.tint[2]);
            m["tint"]  = nl::editor::RpcValue(std::move(tint));
            m["rough"] = (double)d.roughness;
            m["metal"] = (double)d.metallic;
            return nl::editor::RpcValue(std::move(m));
        });
    }
};

int runNormal(const Options& o, bool cluster = false, bool lodMode = false,
              bool dagTop = false, bool cullMode = false, bool dagMode = false,
              bool skinMode = false, bool hizMode = false,
              bool visbufMode = false, bool pullingMode = false,
              bool inputHudMode = false, bool rtMode = false,
              bool shadowMode = false, bool referenceMode = false,
              bool probeMode = false) {
    nl::core::log::resetCounters();
    // P8 M-I2：input-hud 调试面板模式（唯一触碰渲染处，只加不改）。
    const bool inputHud = inputHudMode;

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

    // M-E2：外置编辑器嵌入（--editor-bridge；连不上自动降级单机）。
    // 截图回归模式不启用（隐藏窗口 + 定帧退出，嵌入无意义）。
    EditorEmbed edEmbed;
    edEmbed.start(window.nativeHandle(), o.editorBridge && !o.screenshot,
                  o.editorEndpoint, o.viewId);   // M-E4a/M-E6：enabled 内部判定，pump 统一重试

    const auto nativeSize = window.framebufferSize();
    const nl::renderer::Extent extent{
        static_cast<std::uint32_t>(nativeSize.width),
        static_cast<std::uint32_t>(nativeSize.height)
    };
    auto renderer = nl::renderer::Renderer::create(window.nativeHandle(), extent, o.debug,

                                                   o.modelFile.c_str(), cluster, cullMode,
                                                   o.frustumOn, dagMode, skinMode, hizMode,
                                                   /*sceneMode=*/false,
                                                   visbufMode, pullingMode, rtMode,
                                                   shadowMode, referenceMode, probeMode);
    if (!renderer->valid()) {
        nl::core::log::error("renderer init failed: ", renderer->lastError());
        return 1;
    }

    // M-PT1：--nfx <path> 加载 .nfx v1（成功后模拟走 nfx 参数面；失败退 legacy 喷泉）。
    // 机械锚点（run.bat 断言）：nfx loaded: <file>
    if (!o.nfxPath.empty()) {
        if (!renderer->loadParticlesNfx(o.nfxPath.c_str())) {
            nl::core::log::error("--nfx: ", renderer->lastError(), " (fallback to legacy fountain)");
        } else {
            const std::size_t slash = o.nfxPath.find_last_of("/\\");
            nl::core::log::info("nfx loaded: ",
                                slash == std::string::npos ? o.nfxPath : o.nfxPath.substr(slash + 1));
        }
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
        else if (o.physicsScenario == 2)
            nl::core::log::info("M-D3 drive scene: ", physicsScene.bodyCount(),
                               " bodies (ground + lane markers + wall + VehicleConstraint chassis)");
        else
            nl::core::log::info("physics debug scene: ", physicsScene.bodyCount(),
                               " bodies (floor + dynamic box/sphere/capsule)");
    }

    // Options 是 const（parseArgs 定稿）；HUD 最终值 = 用户显式选择 || 编辑器强制。
    // M-I2：input-hud 模式的面板就是本体，强制开（截图回归 --hud 同口径）。
    const bool hudOn = o.hudEnabled || editorActive || inputHud;

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

    nl::viewer::ViewerCamera camera;
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
    // P8 M-CAM b-segment：加载可选轨迹并按 CLI 选择相机模式（orbit 缺省 = 零回归）
    if (!o.cameraTrack.empty()) {
        if (!camera.loadTrack(o.cameraTrack.c_str()))
            nl::core::log::warn("camera track load failed: ", o.cameraTrack);
    }
    {
        nl::viewer::CameraMode m = nl::viewer::CameraMode::Orbit;
        if (o.cameraMode == "free")       m = nl::viewer::CameraMode::Free;
        else if (o.cameraMode == "chase") m = nl::viewer::CameraMode::Chase;
        else if (o.cameraMode == "track" || !o.cameraTrack.empty()) m = nl::viewer::CameraMode::Track;
        if (m != nl::viewer::CameraMode::Orbit) camera.setMode(m);
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
    // M10.0：probe-place 探针参数（HUD 滑条双向；FrameParams 缺省值已在 renderer.hpp）。
    int probeBaseRes = 16;
    float probeNormalOffset = 0.10f;
    float probeDotSize = 3.0f;
    // M10.2：viz 三态（Q13-3）——默认关（M10.1 probe-viz 点色保形），--probe-gather
    // 或 HUD checkbox 开；probe-place 模式恒 false。
    bool probeGatherMode = (o.mode == nl::core::ViewMode::ProbeViz) && o.probeGather;

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
    // M-GL1：GameClock 与 game.* 命令必须在 console-exec 之前注册（预执行行才找得到）。
    nl::core::GameClock gameClock;   // 双帧模型：渲染帧喂 realDt，逻辑帧 60Hz 消费
    // P8 M-GL5：物理调试 step 挂 Physics 组（TickGroup 注册制，组序契约见
    // tick_groups.hpp）。M-GL2 之前是 tick lambda 内联判断，现迁移为按组注册——
    // 行为逐位一致（本模式只注册 Physics 组，空组 runTick 时跳过）。
    if (o.physicsDebug) {
        gameClock.scheduler().set(nl::core::TickGroup::Physics,
                                  [&physicsScene](double, std::uint64_t) {
            physicsScene.step(1.0 / 60.0);   // M-P3：Jolt 固定步长
        });
    }
    const auto syncCameraToCVar = registerViewerConsole(&camera, &hudEnabled);
    edEmbed.bindCamera(&camera);   // M-E3：相机 RPC handler（normal 模式）
    edEmbed.bindScene(nullptr);           // M-E4d：model 模式（无实例表，getState 返回 mode=model）
    edEmbed.bindMaterials(o.modelFile.c_str());   // M-E5：材质查询（model 模式 .nmesh NSUB 表）
    // M-D3: drive CVars + gates. Registered BEFORE --console-exec so
    // "vehicle.throttleScale 0.5" / "vehicle.debug true" work at startup.
    bool driveHudOn = o.driveHud;
    const VehicleCVars vehicleCVars = registerVehicleConsole(&driveHudOn);
    const bool driveEnabled = o.physicsDebug && o.physicsScenario == 2;
    // P8 M-S2：audio.* 调试 CVar（audio.volume/debug/solo/freeze + audio.dump）。
    nl::audio::registerAudioConsole(nl::core::console::Console::get());
    registerGameConsole(&gameClock);   // P8 M-GL1：game.pause / game.timeScale / game.status
    for (const auto& line : o.consoleExecLines) {
        const auto r = nl::core::console::Console::get().exec(line);
        // M-GL5：对齐 runScene——命令输出也进日志（否则 runNormal 模式下
        // game.tickgroups 等查询命令的验收锚落不了盘）。
        if (r.ok()) {
            nl::core::log::info("console exec [0] ", line, " -> ", r.output);
        } else {
            nl::core::log::error("console exec [", r.exitCode(), "] ", line, " -> ", r.output);
        }
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

    // ---- P8 M-I2：input-hud 注入管线（唯一新增的输入通路；单消费者铁律——
    // 绝不调 updateFromWindow，现有热键/ImGui 键盘通道零触碰）。注入来源两个：
    // ① 面板 checkbox（HudFrame 每帧新建，请求态持久在本局部数组，renderFrame
    // 后读回写回值）；② --input-demo 帧序号脚本（确定性截图）。
    // 帧节奏 = feed 边沿 -> map.update -> 快照 -> endFrame。
    nl::core::InputState inputState;
    nl::core::ActionMap drivingMap;
    const nl::core::DrivingActions dActs = nl::core::buildDrivingActionMap(drivingMap);
    drivingMap.setActive(dActs.uiSet, true);   // 面板要演示 pause 动作，出厂关的 ui 组显式开
    // 注入请求持久态（W/A/S/D/Space/Esc/LMB + ui 组开关期望值；与 hud.inputDebug.req* 双向同步）
    bool reqState[7] = {};
    bool reqUiSetActive = true;
    bool prevReq[7] = {};      // 上一帧实际按下的态（边沿转换用）
    // demo 脚本已按下的键（脚本只 press 不 release；W/D/Space/Esc 持续到退出）
    bool demoHeld[4] = {};

    while (!window.shouldClose()) {
        window.pollEvents();
        edEmbed.pump();   // M-E2：编辑器 RPC 泵（未连接时空操作）
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
        // P8 M-CAM b-segment：喂玩法相机输入（orbit 模式 orbit() 已吃 dx/dy，lookPx 在其下被忽略；
        // 控制台/面板占输入时清空，避免打字或拖 HUD 时相机乱动）。
        {
            const bool camBlocked = hudWantsMouse || hudWantsKeyboard || consoleOpen;
            if (camBlocked) {
                camera.lookPx[0] = 0.0f; camera.lookPx[1] = 0.0f;
                camera.freeInput = nl::core::FreeDrive{};
            } else {
                camera.lookPx[0] = dx; camera.lookPx[1] = dy;
                nl::core::FreeDrive fd{};
                fd.move[2] = (window.rawKeyDown(87) ? 1.0f : 0.0f) - (window.rawKeyDown(83) ? 1.0f : 0.0f); // W/S
                fd.move[0] = (window.rawKeyDown(68) ? 1.0f : 0.0f) - (window.rawKeyDown(65) ? 1.0f : 0.0f); // D/A
                fd.move[1] = (window.rawKeyDown(32) ? 1.0f : 0.0f) - (window.rawKeyDown(341) ? 1.0f : 0.0f); // Space/LCtrl
                fd.boost   = window.rawKeyDown(340) != 0;  // LeftShift
                camera.freeInput = fd;
            }
            camera.camToggleEdge = !camBlocked && window.keyPressed(nl::core::Key::T);
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
        double dtMs = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        // M-GL1：--fixed-dt 钉渲染帧 delta（GameClock 输入 + 帧统计同源）。
        // 不钉绝对时间——只保证「每帧喂多少」确定，暂停/倍速对拍才可逐字节复现。
        if (o.fixedDtMs > 0.0) dtMs = o.fixedDtMs;
        ++frame;
        if (o.screenshot) {
            dtSumMs += dtMs;
            if (dtMs > dtMaxMs) dtMaxMs = dtMs;
        }

        // ---- M-D3 (Q-D3-2 A): drive input injection --------------------------
        // Extends the M-I2 injection pipeline. viewer STILL never calls
        // updateFromWindow(): realtime keys use Window::rawKeyDown(), a pure
        // read-only glfwGetKey query that consumes NO event queue; --drive-demo
        // uses the very same feedKey path with a frame-indexed script so both
        // backends reproduce byte-identical screenshots. Render frame feeds
        // edges -> ActionMap::update -> Physics logic frame consumes.
        if (driveEnabled) {
            static bool driveHeld[5] = {};
            auto feedEdge = [&](int code, bool want, bool& held) {
                if (want != held) { inputState.feedKey(code, want); held = want; }
            };
            bool kw = false, ks = false, ka = false, kd = false, ksp = false;
            if (o.driveDemo) {
                // Deterministic script: accelerate -> steer -> brake into wall.
                kw  = frame >= 2 && frame < 210;
                kd  = frame >= 90 && frame < 150;
                ksp = frame >= 210;
            } else {
                kw  = window.rawKeyDown(GLFW_KEY_W);
                ks  = window.rawKeyDown(GLFW_KEY_S);
                ka  = window.rawKeyDown(GLFW_KEY_A);
                kd  = window.rawKeyDown(GLFW_KEY_D);
                ksp = window.rawKeyDown(GLFW_KEY_SPACE);
            }
            feedEdge(GLFW_KEY_W, kw, driveHeld[0]);
            feedEdge(GLFW_KEY_S, ks, driveHeld[1]);
            feedEdge(GLFW_KEY_A, ka, driveHeld[2]);
            feedEdge(GLFW_KEY_D, kd, driveHeld[3]);
            feedEdge(GLFW_KEY_SPACE, ksp, driveHeld[4]);
            drivingMap.update(inputState);
            const float thr = drivingMap.floatValue(dActs.throttle) *
                              static_cast<float>(vehicleCVars.throttleScale->value().f);
            const float str = drivingMap.floatValue(dActs.steer) *
                              static_cast<float>(vehicleCVars.steerScale->value().f);
            nl::physics::DriveInputState di;
            di.forward   = thr > 0.0f ? thr : 0.0f;
            di.brake     = thr < 0.0f ? -thr : 0.0f;
            di.right     = str;
            di.handBrake = drivingMap.boolValue(dActs.handbrake) ? 1.0f : 0.0f;
            physicsScene.setDriveInput(di);
            inputState.endFrame();
            // Q-D3-4 hook evidence: the Jolt-free POD really reaches the
            // camera-side ChassisState shape (no camera glue is installed).
            if (o.screenshot && frame == static_cast<std::uint64_t>(o.frames)) {
                const auto dsLog = physicsScene.driveState();
                nl::core::log::info("drive scene: speed=", dsLog.speed,
                                    " grounded=", dsLog.grounded,
                                    " wallHits=", dsLog.wallHits,
                                    " contacts=", dsLog.contactAdded, "/",
                                    dsLog.contactPersisted, "/", dsLog.contactRemoved,
                                    " fwd=", di.forward, " hb=", di.handBrake);
                const nl::physics::DriveChassisState dcs = physicsScene.chassisState();
                nl::core::ChassisState cs{};
                cs.pos[0] = dcs.pos[0]; cs.pos[1] = dcs.pos[1]; cs.pos[2] = dcs.pos[2];
                cs.yawDegrees = dcs.yawDegrees;
                cs.vel[0] = dcs.vel[0]; cs.vel[1] = dcs.vel[1]; cs.vel[2] = dcs.vel[2];
                cs.valid = dcs.valid;
                nl::core::log::info("drive hook: chassis valid=", cs.valid ? 1 : 0,
                                    " posZ=", cs.pos[2], " yawDeg=", cs.yawDegrees);
            }
        }

        // M-C1：键盘 / 文本事件泵——HUD 关着也必须排干（防 window 侧队列无限增长）。
        // 事件随帧丢弃（不跨帧缓存）：ImGui 的按键状态以「本帧按下与否」为准。
        nl::core::KeyEvent keyBuf[16];
        unsigned int textBuf[32];
        const std::size_t nKeys = window.consumeKeyEvents(keyBuf, 16);
        const std::size_t nChars = window.consumeTextChars(textBuf, 32);

        nl::renderer::FrameParams fp;
        // P8 M-CAM b-segment：每帧把车体回读（仅 drive scenario 有效）喂 Chase 相机，并推进玩法相机
        // （orbit 模式 update 内部直出，逐位零回归）。chassisState 在 scenario==2 才有有效车体。
        {
            nl::core::ChassisState chassisState{};
            if (driveEnabled) {
                const nl::physics::DriveChassisState dcs = physicsScene.chassisState();
                chassisState.pos[0] = dcs.pos[0]; chassisState.pos[1] = dcs.pos[1]; chassisState.pos[2] = dcs.pos[2];
                chassisState.yawDegrees = dcs.yawDegrees;
                chassisState.vel[0] = dcs.vel[0]; chassisState.vel[1] = dcs.vel[1]; chassisState.vel[2] = dcs.vel[2];
                chassisState.valid = dcs.valid;
            }
            camera.chassis = chassisState.valid ? &chassisState : nullptr;
            const float camDt = static_cast<float>(dtMs) * 0.001f;
            camera.update(camDt);
        }
        const auto vp = resolveViewProjection(camera, physicsScene);
        const auto model = camera.model();
        std::memcpy(fp.mvp, vp.m, sizeof(fp.mvp));
        std::memcpy(fp.model, model.m, sizeof(fp.model));
        fp.cullBack = cullBack;
        fp.shading = shading;
        fp.wireframe = wireframe;
        fp.visVizMode = o.visVizMode;       // M7.0：visbuf 解码档（其它模式不读）
        fp.particles = o.particles;         // M-PT0：粒子叠加层开关（其它模式不读）
        // M-PT0：billboard 相机基向量取 view 矩阵（dag/hiz 分支已填；其它模式只有
        // particles 读它，缺省零阵 = 粒子退化不画——所以这里补填）。
        if (o.particles && !(dagMode || hizMode)) {
            const auto vm = camera.view();
            std::memcpy(fp.view, vm.m, sizeof(fp.view));
        }
        if (o.pbrNaive) fp.options[0] = 1.0f;   // pbr-direct：g_mrF.z 对照开关
        fp.tlasSpin  = o.tlasSpin;              // M8.1：TLAS 实例 spin / 探针（BLAS 未就位时
        fp.tlasProbe = o.tlasProbe;             // renderer 内部直接返回，其它模式零扰动）
        fp.refFrames = static_cast<std::uint32_t>(o.frames);  // M9.2：reference 进度分母
        // M8.2：rt 模式射线生成输入——viewProj（camera.viewProjection 纯 view×proj，
        // 列主序）+ 调试档 + eye（灰度参考距离 = 相机轨道距离 ×2）。其它模式不读，
        // 缺省零副作用。
        std::memcpy(fp.viewProj, vp.m, sizeof(fp.viewProj));
        fp.rtDebugMode = o.rtDebugMode;
        if (rtMode) {
            float rtEye[3];
            camera.eyePosition(rtEye);
            fp.eye[0] = rtEye[0]; fp.eye[1] = rtEye[1]; fp.eye[2] = rtEye[2];
        }
        // M10.0：probe-place——eye/projYScale（探针法线偏移公式输入）独立填，
        // 不借 dag/hiz 分支（viz 模式不开 velocity prepass 也一样要布点）。
        if (probeMode) {
            float prEye[3];
            camera.eyePosition(prEye);
            fp.eye[0] = prEye[0]; fp.eye[1] = prEye[1]; fp.eye[2] = prEye[2];
            fp.projYScale = camera.projectionYScale();
        }
        fp.probeBaseRes = probeBaseRes;
        fp.probeNormalOffset = probeNormalOffset;
        fp.probeDotSize = probeDotSize;
        // M10.1：probe-viz 模式 = 点色改八面体辐照度采样（管线与 probe-place 全同，
        // 仅 viz 分支差异——trace pass 两模式都跑，布点+追踪一体）。
        // M10.2（Q13-3）：gather 态 = 全屏 final gather + ambient 替换（与点色
        // 互斥，gather 优先）；probe-place 模式恒 false 保形。
        const bool isProbeViz = (o.mode == nl::core::ViewMode::ProbeViz);
        fp.probeVizIrradiance = (isProbeViz && !probeGatherMode);
        fp.probeGather = (isProbeViz && probeGatherMode);
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
        // M-GL1：非 pin 路径改走 GameClock 双帧模型——渲染帧喂 dtMs，逻辑帧 60Hz
        // 消费（暂停=时钟停摆、timeScale 缩放、单帧大 delta 截断 250ms）；
        // simTime 量化到 tick（起步不插值，PLAN §2 拍板），渲染帧取最新逻辑帧状态。
        // P8 M-GL2：双帧模型全量接线——每渲染帧喂一次时钟，逻辑帧 60Hz callback
        // 消费（本循环的消费方 = 物理调试 step；场景模式的 ECS tick + 音频同步
        // 在 runScene 同构）。暂停/倍速/250ms 截断都在时钟内（M-GL0 语义）。
        // P8 M-GL5：lambda 内联判断迁移为 Physics 组注册（帧循环前一次性 set），
        // tick(dtMs) 走按组调度路径——执行序 = 枚举序，逐位同旧路径。
        gameClock.tick(dtMs);
        if (skinMode) {
            // M-A5：--anim-time 直接钉值（取值绕过时钟，既有基线零风险）；时钟本身
            // 照常推进——pin 模式下物理等其它消费方仍然活着（M-GL2 口径）。
            skinTime = o.animTimeSet ? o.animTime : gameClock.simTime();
            fp.animTime = skinTime;
            // 截图终帧机械证据：双帧模型状态一行（对拍日志锚）。注意口径：捕获判定
            // （上方 capturePath）在 ++frame 之前求值、持上一迭代 frame 值，PNG 实际
            // 落在本循环最后一个迭代（frame == o.frames）；此处已在 ++frame 之后，
            // 同一迭代即 frame == o.frames——日志与 PNG 同帧。
            if (o.screenshot && frame == static_cast<std::uint64_t>(o.frames)) {
                nl::core::log::info("game clock: simTime=", gameClock.simTime(),
                                    "s ticks=", gameClock.tickCount(),
                                    " paused=", gameClock.paused() ? 1 : 0,
                                    " scale=", gameClock.timeScale(),
                                    " fixedDtMs=", o.fixedDtMs);
            }
        }

        // M-P3：物理调试叠加层（Jolt 碰撞形状线框）。build 一次（renderer 创建后），
        // 逻辑帧 step（M-GL2 起 step 在 gameClock callback 内，暂停即停）+ 每渲染帧
        // extractLines 喂给渲染器 LINE_LIST 叠加层。截图模式 240 帧足够落体落到
        // 地板稳态；交互模式逐逻辑帧步进即实时动画。extractLines 返回的 data
        // 指向 DebugScene 内部缓冲，本帧 renderFrame 后即失效（单帧消费模型）。
        if (o.physicsDebug) {
            const nl::physics::DebugLines dl = physicsScene.extractLines();
            fp.debugLines = true;
            fp.debugLineData = dl.data;
            fp.debugLineVertexCount = dl.vertexCount;
            // 截图最终帧打一行稳态机械证据：线顶点数 + 线框 Y 跨度（落体应已落到 ~0.5）。
            // 口径同 M-GL1：此处已在 ++frame 之后，frame == o.frames 才与 PNG 同帧。
            if (o.screenshot && frame == static_cast<std::uint64_t>(o.frames)) {
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
        if (inputHud) hud.panelShow = true;   // M-I2：input-hud 模式常驻面板即本体
        // M-D3 (Q-D3-3 A): separate sibling window, default OFF. Only this
        // milestone's own cards turn it on, so 70 existing baseline PNGs stay put.
        if (driveEnabled && driveHudOn) {
            const nl::physics::DriveViewState ds = physicsScene.driveState();
            hud.driveDebug.show = true;
            int n = 0;
            // Short lines only: AutoResize fits the longest line, and a long
            // first line used to get clipped at the window's right edge.
            std::snprintf(hud.driveDebug.lines[n++], nl::renderer::DriveDebugUi::kLineChars,
                          "DRIVE  valid=%d  v=%.3fm/s  yaw=%.3f",
                          ds.valid ? 1 : 0, ds.speed, ds.yaw);
            std::snprintf(hud.driveDebug.lines[n++], nl::renderer::DriveDebugUi::kLineChars,
                          "POS    x=%.2f  y=%.2f  z=%.2f", ds.pos[0], ds.pos[1], ds.pos[2]);
            std::snprintf(hud.driveDebug.lines[n++], nl::renderer::DriveDebugUi::kLineChars,
                          "WHEELS grounded=%d / 4", ds.grounded);
            std::snprintf(hud.driveDebug.lines[n++], nl::renderer::DriveDebugUi::kLineChars,
                          "CONTACT add=%u per=%u rem=%u",
                          ds.contactAdded, ds.contactPersisted, ds.contactRemoved);
            std::snprintf(hud.driveDebug.lines[n++], nl::renderer::DriveDebugUi::kLineChars,
                          "WALL   hits=%u", ds.wallHits);
            std::snprintf(hud.driveDebug.lines[n++], nl::renderer::DriveDebugUi::kLineChars,
                          "CVAR   thr=%.2f str=%.2f",
                          vehicleCVars.throttleScale->value().f,
                          vehicleCVars.steerScale->value().f);
            hud.driveDebug.lineCount = n;
        }
        if (hudEnabled || consoleOpen || editorActive || inputHud || driveHudOn) {
            // ---- P8 M-I2：input-hud 注入管线（见主循环前的局部变量注释）----
            if (inputHud) {
                // ① 请求持久态 -> feed 边沿（W/A/S/D/Space/Esc 走键盘，LMB 走鼠标键）
                static constexpr int kReqCodes[7] = {87, 65, 83, 68, 32, 256, 0};  // GLFW W/A/S/D/Space/Escape + LMB
                for (int i = 0; i < 7; ++i) {
                    if (reqState[i] != prevReq[i]) {
                        if (i < 6) inputState.feedKey(kReqCodes[i], reqState[i]);
                        else       inputState.feedMouseButton(kReqCodes[i], reqState[i]);
                        prevReq[i] = reqState[i];
                    }
                }
                drivingMap.setActive(dActs.uiSet, reqUiSetActive);
                // ② --input-demo 脚本：W@start / D@+1 / Space@+2 / Esc@+3（只 press；
                // 缺省 start=5 + frames=8 → Esc 的 justPressed 边沿正好落在截图帧上）
                if (o.inputDemo) {
                    const std::uint64_t it = frame;
                    struct DemoStep { std::uint64_t at; int key; };
                    static constexpr DemoStep kScript[4] = {
                        {0, GLFW_KEY_W}, {1, GLFW_KEY_D}, {2, GLFW_KEY_SPACE},
                        {3, GLFW_KEY_ESCAPE}};  // +start 偏移
                    for (int i = 0; i < 4; ++i) {
                        const std::uint64_t at = o.inputDemoStart + kScript[i].at;
                        if (it == at && !demoHeld[i]) {
                            inputState.feedKey(kScript[i].key, true);
                            demoHeld[i] = true;
                            nl::core::log::info("input-demo: inject key ", kScript[i].key,
                                                " at iter ", it);
                        }
                    }
                }
                // ③ 解析映射层（M-I1 冻结语义；必须在 endFrame 之前）
                drivingMap.update(inputState);
                // ④ 面板快照（边沿在 endFrame 清零前采样）+ 注入请求回填
                hud.inputDebug.show = true;
                int rowN = 0;
                const auto fillRow = [&](const char* label, int code, bool mouse) {
                    if (rowN >= hud.inputDebug.kMaxRows) return;
                    auto& r = hud.inputDebug.rows[rowN++];
                    r.label = label;
                    r.down = mouse ? inputState.mouseIsDown(code) : inputState.isDown(code);
                    r.pressed = mouse ? inputState.mouseJustPressed(code) : inputState.justPressed(code);
                    r.released = mouse ? inputState.mouseJustReleased(code) : inputState.justReleased(code);
                };
                fillRow("W", GLFW_KEY_W, false);
                fillRow("A", GLFW_KEY_A, false);
                fillRow("S", GLFW_KEY_S, false);
                fillRow("D", GLFW_KEY_D, false);
                fillRow("Space", GLFW_KEY_SPACE, false);
                fillRow("Esc", GLFW_KEY_ESCAPE, false);
                fillRow("LMB", GLFW_MOUSE_BUTTON_LEFT, true);
                hud.inputDebug.rowCount = rowN;
                hud.inputDebug.throttle = drivingMap.floatValue(dActs.throttle);
                hud.inputDebug.steer = drivingMap.floatValue(dActs.steer);
                const auto lookV = drivingMap.vec2Value(dActs.look);
                hud.inputDebug.lookX = lookV.x;
                hud.inputDebug.lookY = lookV.y;
                hud.inputDebug.handbrake = drivingMap.boolValue(dActs.handbrake) ? 1.0f : 0.0f;
                hud.inputDebug.handbrakePressed = drivingMap.boolJustPressed(dActs.handbrake);
                hud.inputDebug.handbrakeReleased = drivingMap.boolJustReleased(dActs.handbrake);
                hud.inputDebug.pause = drivingMap.boolValue(dActs.pause) ? 1.0f : 0.0f;
                hud.inputDebug.pausePressed = drivingMap.boolJustPressed(dActs.pause);
                hud.inputDebug.pauseReleased = drivingMap.boolJustReleased(dActs.pause);
                hud.inputDebug.drivingSetActive = drivingMap.isActive(dActs.drivingSet);
                hud.inputDebug.uiSetActive = drivingMap.isActive(dActs.uiSet);
                hud.inputDebug.reqW = reqState[0];
                hud.inputDebug.reqA = reqState[1];
                hud.inputDebug.reqS = reqState[2];
                hud.inputDebug.reqD = reqState[3];
                hud.inputDebug.reqSpace = reqState[4];
                hud.inputDebug.reqEsc = reqState[5];
                hud.inputDebug.reqLmb = reqState[6];
                hud.inputDebug.reqUiSetActive = reqUiSetActive;
                // ⑤ 帧末清边沿（快照已拷贝；下一帧边沿从零开始）
                inputState.endFrame();
            }
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
            // M-PT0：粒子证据行数据（仅 particles 模式显示，其它模式一像素不动）。
            hud.particlesEnabled = o.particles;
            hud.particleAlive = info.particleAlive;
            hud.hizTopFar = info.hizTopFar;   // M6.1：最远表面口径
            hud.hizViewLevel = hizViewLevel;
            // M6.2：开关双向 + 剔除前/后对照（-1 = 暂无数据，HUD 画 -- 占位）。
            if (hizMode) {
                hud.hizEnable = hizEnabled;
                hud.hizOccluded = info.hizOccluded;
            }
            // M10.0：探针参数面板（probe-place 模式专属；双向通道同 hizEnable）。
            if (probeMode) {
                hud.probePanelShow = true;
                hud.probeBaseRes = probeBaseRes;
                hud.probeNormalOffset = probeNormalOffset;
                hud.probeDotSize = probeDotSize;
                hud.probeGatherMode = isProbeViz && probeGatherMode;   // M10.2 双向
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
                                   (hudEnabled || consoleOpen || editorActive || inputHud || driveHudOn) ? &hud : nullptr)) {
            nl::core::log::error("renderFrame failed: ", renderer->lastError());
            failed = true;
            break;
        }
        renderer->drainMessages();
        // M-PT0：稳态机械证据——最终帧的活粒子数（固定步长下 = 帧号的纯函数；
        // run.bat 断言落在发射率×平均寿命的理论区间 [rate*lifeMin, rate*lifeMax]）。
        // M-PT1：--nfx 时锚点换 nfx 标记（rate/life 从资产读，数值进 .nfx 内容）。
        if (o.particles && o.screenshot && frame + 1 == static_cast<std::uint64_t>(o.frames)) {
            if (o.nfxPath.empty()) {
                nl::core::log::info("particles steady: alive=", renderer->info().particleAlive,
                                    " (rate=300/s, life=[1.6,2.3]s)");
            } else {
                nl::core::log::info("particles steady: alive=", renderer->info().particleAlive,
                                    " (nfx)");
            }
        }
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
        if (hudEnabled || consoleOpen || editorActive || inputHud || driveHudOn) {   // 帧跑了才消费控件写回
            // P8 M-I2：注入请求读回（checkbox 双向态；下一帧帧首转 feed 边沿）
            if (inputHud) {
                reqState[0] = hud.inputDebug.reqW;
                reqState[1] = hud.inputDebug.reqA;
                reqState[2] = hud.inputDebug.reqS;
                reqState[3] = hud.inputDebug.reqD;
                reqState[4] = hud.inputDebug.reqSpace;
                reqState[5] = hud.inputDebug.reqEsc;
                reqState[6] = hud.inputDebug.reqLmb;
                reqUiSetActive = hud.inputDebug.reqUiSetActive;
            }
            if (hud.resetCamera) camera.reset();
            cullBack = hud.cullBack;
            if (hizMode) hizEnabled = hud.hizEnable;   // M6.2：开关写回
            // M10.0：探针滑条写回（clamp 同 ensureProbeTargets 闸口径 4..32）。
            if (probeMode) {
                probeBaseRes = std::clamp(hud.probeBaseRes, 4, 32);
                probeNormalOffset = hud.probeNormalOffset;
                probeDotSize = hud.probeDotSize;
                if (isProbeViz) probeGatherMode = hud.probeGatherMode;   // M10.2 写回
            }
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
        // M9.2：reference 模式截图后写同名 .json 侧车（相机/灯光/采样元数据），
        // 供回归对拍与裁判 M9.1 阴影射线用。仅 reference 模式产出，且只在末帧触发。
        if (referenceMode && capturePath) {
            std::string jsonPath = capturePath;
            const std::string pngExt = ".png";
            if (jsonPath.size() >= pngExt.size() &&
                jsonPath.compare(jsonPath.size() - pngExt.size(), pngExt.size(), pngExt) == 0)
                jsonPath.replace(jsonPath.size() - pngExt.size(), pngExt.size(), ".json");
            else
                jsonPath += ".json";
            nlohmann::json side;
            side["mode"] = "reference";
            side["model"] = o.modelFile;
            side["size"] = {static_cast<int>(o.width), static_cast<int>(o.height)};
            side["yaw"] = camera.yawDegrees();
            side["pitch"] = camera.pitchDegrees();
            side["dist"] = camera.distance();
            side["lightDir"] = {fp.lightDir[0], fp.lightDir[1], fp.lightDir[2]};
            side["lightColor"] = {1.0f, 1.0f, 1.0f, 2.5f};
            side["frames"] = static_cast<int>(o.frames);
            side["sppPerFrame"] = 64;
            side["spp"] = static_cast<int>(o.frames) * 64;
            std::ofstream js(jsonPath);
            if (js) {
                js << side.dump(2) << '\n';
                nl::core::log::info("reference sidecar written: ", jsonPath);
            } else {
                nl::core::log::warn("reference sidecar write failed: ", jsonPath);
            }
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
                  << " | cam " << camera.modeName()
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

// ---------------------------------------------------------------------------
// P8 M-GL3：渲染帧插值（帧间插值，alpha = accumulator/step）。
// 参考口径（2026-11-10 实读）：bevy v0.16.1 crates/bevy_time/src/fixed.rs:178-183
// —— Fixed 时钟暴露 overstep()/timestep() 分数即插值 alpha（与 GameClock::alpha()
// 同构）；bevy core 全树无内建 transform 插值器（git ls-tree 0 命中，插值本体是
// 社区 crate bevy_transform_interpolation）。采用其公开设计纪律：插值结果
// **渲染专用、绝不回写 sim state**（组件态只在逻辑帧内变化）。Carrot/UE 不插值
// （PLAN §3 既有结论）。世界空间 TRS 插值（非局部递归）：E1a 层级子实体无需重算
// 父链；均匀 scale 场景下分解严格成立（与 Component::setWorldTransform 同口径）。
// 截图模式恒走不插值原路径（截图=确定性产物，PNG 与不插值逐字节同）；
// 插值数值证据走捕获帧 probe 日志（verify.py 孪生复算）。
// ---------------------------------------------------------------------------
struct WorldSample {
    nl::math::Vec3 pos{};
    nl::math::Quat rot{};
    nl::math::Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Mat4 -> TRS 分解（列主序：三列长度 = scale、归一化列 = 旋转、m[12..14] = pos）。
// 四元数用 Shepperd 法（quatToMat4 的逆变换，trace 判主元分支，数值稳定）；
// 退化保护：任一 scale 列近零时不除（旋转按单位阵，组件层不允许奇异但防御）。
static WorldSample decomposeWorldTRS(const nl::math::Mat4& m) {
    WorldSample s;
    s.pos = {m.m[12], m.m[13], m.m[14]};
    const float sx = std::sqrt(m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2]);
    const float sy = std::sqrt(m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6]);
    const float sz = std::sqrt(m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10]);
    s.scale = {sx, sy, sz};
    if (sx < 1e-8f || sy < 1e-8f || sz < 1e-8f) return s;  // 退化：单位旋转
    const float r[3][3] = {
        {m.m[0] / sx, m.m[4] / sy, m.m[8] / sz},
        {m.m[1] / sx, m.m[5] / sy, m.m[9] / sz},
        {m.m[2] / sx, m.m[6] / sy, m.m[10] / sz},
    };
    const float tr = r[0][0] + r[1][1] + r[2][2];
    nl::math::Quat q{};   // Quat{} = identity（quatIdentity 同默认）
    if (tr > 0.0f) {
        const float t = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * t;
        q.x = (r[2][1] - r[1][2]) / t;
        q.y = (r[0][2] - r[2][0]) / t;
        q.z = (r[1][0] - r[0][1]) / t;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float t = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q.w = (r[2][1] - r[1][2]) / t;
        q.x = 0.25f * t;
        q.y = (r[0][1] + r[1][0]) / t;
        q.z = (r[0][2] + r[2][0]) / t;
    } else if (r[1][1] > r[2][2]) {
        const float t = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q.w = (r[0][2] - r[2][0]) / t;
        q.x = (r[0][1] + r[1][0]) / t;
        q.y = 0.25f * t;
        q.z = (r[1][2] + r[2][1]) / t;
    } else {
        const float t = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q.w = (r[1][0] - r[0][1]) / t;
        q.x = (r[0][2] + r[2][0]) / t;
        q.y = (r[1][2] + r[2][1]) / t;
        q.z = 0.25f * t;
    }
    s.rot = nl::math::normalizeQuat(q);
    return s;
}

// ---------------------------------------------------------------------------
// M-E1b.0：ECS 场景模式（--scene <file>）。场景文件不是 assets 白名单资产，
// 按传入路径直读；组件库显式注册后 World::deserialise（负向 = 用法错误码 2，
// 与 nanite-scene deserialise-check 口径一致），O(N) 遍历实体收集
// SceneObjectDesc / SceneLightDesc，sceneMode=true 的 Renderer + loadEcsScene。
// ---------------------------------------------------------------------------
int runScene(const Options& o) {
    // ---- 1. 读场景文件 + World 反序列化 ----
    const std::string& scenePath = o.scenePath;
    if (scenePath.empty()) { nl::core::log::error("--scene: path is empty"); return 2; }
    for (const char c : scenePath) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) {
            nl::core::log::error("--scene: path must be printable ASCII: ", scenePath);
            return 2;
        }
    }
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::path(scenePath), ec) || ec) {
            nl::core::log::error("--scene: file not found: ", scenePath);
            return 2;
        }
    }
    nl::scene::World world;
    float ambient = 0.28f;   // 场景文件缺省（ambient 为场景级扩展键，World 格式不感知）
    {
        std::ifstream f(scenePath, std::ios::binary);
        if (!f) { nl::core::log::error("--scene: cannot open ", scenePath); return 2; }
        nlohmann::json doc;
        try {
            f >> doc;
        } catch (const nlohmann::json::exception& ex) {
            nl::core::log::error("--scene: malformed json: ", ex.what());
            return 2;
        }
        // 注册组件库（与 nanite-scene 同款显式注册）→ World::deserialise。
        world.getComponentLibrary().registerComponent<nl::scene::TransformComponent>();
        world.getComponentLibrary().registerComponent<nl::scene::ModelComponent>();
        world.getComponentLibrary().registerComponent<nl::scene::LightComponent>();
        std::string err;
        bool ok = false;
        try {
            ok = world.deserialise(doc, err);
        } catch (const std::exception& ex) {
            nl::core::log::error("--scene: deserialise threw: ", ex.what());
            return 2;
        }
        if (!ok) {
            nl::core::log::error("--scene: deserialise rejected: ", err);
            return 2;
        }
        if (doc.contains("ambient") && doc["ambient"].is_number())
            ambient = doc["ambient"].get<float>();
    }

    // ---- 2. O(N) 遍历实体收集渲染描述 ----
    // meshFile 白名单校验（与 renderer.cpp validAssetFileName 同一套逻辑，
    // scene.cpp 兜底同款复制；非法 = 显式报错退出 2，不静默跳过）。
    auto validGlbAssetName = [](const std::string& s) {
        if (s.empty()) return false;
        if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
        if (s.find("..") != std::string::npos) return false;
        const std::string ext = ".glb";
        if (s.size() < ext.size() + 1 || s.compare(s.size() - ext.size(), ext.size(), ext) != 0)
            return false;
        for (const char c : s) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x21 || u > 0x7E) return false;
        }
        return true;
    };
    std::vector<nl::renderer::SceneLightDesc> lights;
    std::vector<std::string> meshFiles;
    std::vector<nl::scene::EntityID> modelIds;
    for (nl::scene::EntityID id : world.getEntityIDs()) {
        const auto* light = world.getComponent<nl::scene::LightComponent>(id);
        if (light) {
            nl::renderer::SceneLightDesc l{};
            const float len = std::sqrt(light->direction.x * light->direction.x +
                                        light->direction.y * light->direction.y +
                                        light->direction.z * light->direction.z);
            if (len > 1e-8f) {   // 归一化（零向量保原样，退化灯交给 shader abs(dot) 口径）
                l.dir[0] = light->direction.x / len;
                l.dir[1] = light->direction.y / len;
                l.dir[2] = light->direction.z / len;
            }
            l.color[0] = light->color.x;
            l.color[1] = light->color.y;
            l.color[2] = light->color.z;
            l.intensity = light->intensity;
            // M-E1b.2：点光字段透传 + 显式拒绝（非法 type / 非正 range 退出码 2）
            l.type = light->type;
            if (l.type != 0 && l.type != 1) {
                nl::core::log::error("--scene: entity ", id, " has unknown light type ", l.type);
                return 2;
            }
            l.position[0] = light->position.x;
            l.position[1] = light->position.y;
            l.position[2] = light->position.z;
            l.range = light->range;
            if (l.type == 1 && l.range <= 0.0f) {
                nl::core::log::error("--scene: entity ", id, " point light range must be > 0");
                return 2;
            }
            // M-E1b.4：方向灯阴影开关透传；点光不投阴影（2D shadow map 只做方向灯），
            // 带键显式拒绝（不静默忽略，五路负向口径）。
            l.shadow = light->shadow;
            if (l.type == 1 && l.shadow) {
                nl::core::log::error("--scene: entity ", id,
                                     " point light does not support shadow (directional only)");
                return 2;
            }
            lights.push_back(l);
        }
        const auto* model = world.getComponent<nl::scene::ModelComponent>(id);
        if (model) {
            if (!world.getComponent<nl::scene::TransformComponent>(id)) {
                nl::core::log::error("--scene: entity ", id, " has Model without Transform");
                return 2;
            }
            if (!validGlbAssetName(model->meshFile)) {
                nl::core::log::error("--scene: entity ", id, " meshFile '",
                                     model->meshFile, "' rejected by asset whitelist (.glb)");
                return 2;
            }
            meshFiles.push_back(model->meshFile);   // 指针先落到稳定存储再填 POD
            modelIds.push_back(id);
        }
    }
    std::vector<nl::renderer::SceneObjectDesc> objects;
    objects.reserve(modelIds.size());
    for (std::size_t k = 0; k < modelIds.size(); ++k) {
        const auto* model = world.getComponent<nl::scene::ModelComponent>(modelIds[k]);
        const auto* t = world.getComponent<nl::scene::TransformComponent>(modelIds[k]);
        nl::renderer::SceneObjectDesc d{};
        const auto wm = t->getWorldMatrix();   // nl::math Mat4 列主序 m[col*4+row]，按列直拷
        std::memcpy(d.world, wm.m, sizeof(d.world));
        d.tint[0] = model->tint.x;
        d.tint[1] = model->tint.y;
        d.tint[2] = model->tint.z;
        d.roughness = model->roughness;   // M-E1b.3：逐实例 PBR 材质（缺省 1/0）
        d.metallic  = model->metallic;
        d.meshFile = meshFiles[k].c_str();
        objects.push_back(d);
    }

    // ---- P8 M-GL3：插值样本（prev/curr 世界 TRS，per modelId）----
    // 每渲染帧 tick 前 snapshot prev（上一 tick 边界态）、tick 后 snapshot curr；
    // 无条件维护（开销 = 每实体两次矩阵分解），game.interp 运行时切换零毛刺。
    std::vector<WorldSample> interpPrev(modelIds.size());
    std::vector<WorldSample> interpCurr(modelIds.size());
    for (std::size_t k = 0; k < modelIds.size(); ++k) {
        const auto* t = world.getComponent<nl::scene::TransformComponent>(modelIds[k]);
        interpPrev[k] = interpCurr[k] = decomposeWorldTRS(t->getWorldMatrix());
    }

    // ---- 3. 窗口 + sceneMode Renderer ----
    nl::core::log::resetCounters();
    nl::core::WindowDesc desc;
    desc.width  = o.width;
    desc.height = o.height;
    desc.title  = "nl-engine | starting...";
    // 截图回归模式隐藏窗口（与 runNormal 同口径：可见窗口会抢前台焦点）。
    desc.visible = !o.screenshot;
    nl::core::Window window(desc);
    if (!window.valid()) {
        nl::core::log::error("window creation failed");
        return 1;
    }

    // M-E2：外置编辑器嵌入（--editor-bridge；连不上自动降级单机）。
    EditorEmbed edEmbed;
    edEmbed.start(window.nativeHandle(), o.editorBridge && !o.screenshot,
                  o.editorEndpoint, o.viewId);   // M-E4a/M-E6：enabled 内部判定，pump 统一重试

    const auto nativeSize = window.framebufferSize();
    const nl::renderer::Extent extent{
        static_cast<std::uint32_t>(nativeSize.width),
        static_cast<std::uint32_t>(nativeSize.height)
    };
    auto renderer = nl::renderer::Renderer::create(window.nativeHandle(), extent, o.debug,
                                                   /*modelFile=*/nullptr,
                                                   /*cluster=*/false, /*cullMode=*/false,
                                                   /*frustumCull=*/false, /*dagMode=*/false,
                                                   /*skinMode=*/false, /*hizMode=*/false,
                                                   /*sceneMode=*/true);
    if (!renderer->valid()) {
        nl::core::log::error("renderer init failed: ", renderer->lastError());
        return 1;
    }
    const std::string sceneName =
        std::filesystem::path(scenePath).filename().string();
    // 机器出口（run.bat findstr 断言）：对象数 / 灯数（含点光计数）/ ambient 一行。
    std::uint32_t pointLights = 0;
    for (const auto& l : lights) if (l.type == 1) ++pointLights;
    nl::core::log::info("ecs scene: ", objects.size(), " objects | ", lights.size(),
                        " lights (", pointLights, " point) | ambient ", ambient);
    if (!renderer->loadEcsScene(objects.data(),
                                static_cast<std::uint32_t>(objects.size()),
                                lights.empty() ? nullptr : lights.data(),
                                static_cast<std::uint32_t>(lights.size()),
                                ambient, sceneName.c_str())) {
        nl::core::log::error("ecs scene load failed: ", renderer->lastError());
        return 1;
    }

    // ---- M-E1b.5 渲染用例：--tick serial|parallel（帧循环逐帧 tick + 刷实例）----
    // SpinSystem（Write<TransformComponent>）绕 Y 轴旋转，角速度按实体序号差异化
    // （画面可辨：各物体转速不同）；ReadCountSystem（Read<TransformComponent>）
    // 纯读统计——与 SpinSystem 无组件冲突，tickParallel 求解器让两者并行。
    // 渲染结果只由 SpinSystem 决定：parallel 与 serial 同固定 dt 序列下
    // 逐字节一致（浮点确定性由 tickParallel 串行对拍契约保证）。
    // Spin 基础角速度 rad/s（首实体），逐实体 ×(1+0.5k)——转速差异画面可辨。
    constexpr float kSpinBaseSpeed = 0.9f;
    class SpinSystem final : public nl::scene::SignedSystem<nl::scene::Write<nl::scene::TransformComponent>> {
    public:
        using Base = nl::scene::SignedSystem<nl::scene::Write<nl::scene::TransformComponent>>;
        explicit SpinSystem(nl::scene::World& w) : Base(w) {}
        const char* getName() const override { return "Spin"; }
        // duplicate = 无状态拷贝（SpinSystem 无数据成员）。
        std::unique_ptr<nl::scene::System> duplicate(nl::scene::World& newOwner) const override {
            return std::make_unique<SpinSystem>(newOwner);
        }
        // 局部类禁静态数据成员（C2871），基础角速度用函数级常量 kSpinBaseSpeed。
        void tick(double dt) override {
            const float baseAngle = static_cast<float>(dt) * kSpinBaseSpeed;
            std::uint32_t idx = 0;
            forEachEntity([baseAngle, &idx](nl::scene::Entity,
                                            nl::scene::TransformComponent& t) {
                const float angle = baseAngle * (1.0f + 0.5f * static_cast<float>(idx++));
                const auto q = nl::math::quatFromAxisAngle(
                    nl::math::Vec3{0.0f, 1.0f, 0.0f}, angle);
                t.local.rotation = nl::math::mulQuat(q, t.local.rotation);
            });
        }
    };
    class ReadCountSystem final : public nl::scene::SignedSystem<nl::scene::Read<nl::scene::TransformComponent>> {
    public:
        using Base = nl::scene::SignedSystem<nl::scene::Read<nl::scene::TransformComponent>>;
        explicit ReadCountSystem(nl::scene::World& w) : Base(w) {}
        const char* getName() const override { return "ReadCount"; }
        std::unique_ptr<nl::scene::System> duplicate(nl::scene::World& newOwner) const override {
            auto copy = std::make_unique<ReadCountSystem>(newOwner);
            copy->count = count;
            return copy;
        }
        void tick(double) override {
            count = 0;
            forEachEntity([this](nl::scene::Entity, const nl::scene::TransformComponent&) {
                ++count;
            });
        }
        int count = 0;
    };
    const bool ticking = (o.tickMode != "off");
    if (ticking) {
        world.registerSystem(std::make_unique<SpinSystem>(world));
        world.registerSystem(std::make_unique<ReadCountSystem>(world));
        nl::core::log::info("ecs tick: ", o.tickMode,
                            " | systems=Spin(Write<Transform>)+ReadCount(Read<Transform>)",
                            " | fixed dt=1/60");
    }

    const bool hudOn = o.hudEnabled;
    // 相机：用 assetSphere 自动取景（未显式 --dist 时；skin 模式 skinRadius 同款
    // 系数 2.4——assetSphere 是场景联合球，N 物体铺开尺度由它自适应）。
    float camDist = o.distance;
    if (!o.distSet) {
        const float* sph = renderer->info().assetSphere;
        if (sph[3] > 0.0f) camDist = std::max(camDist, sph[3] * 2.4f);
    }
    nl::viewer::ViewerCamera camera;
    camera.setAspect(static_cast<float>(extent.width) / static_cast<float>(extent.height));
    camera.setOrbit(o.yawDeg, o.pitchDeg, camDist);
    // P8 M-CAM b-segment：加载可选轨迹并按 CLI 选择相机模式（orbit 缺省 = 零回归）
    if (!o.cameraTrack.empty()) {
        if (!camera.loadTrack(o.cameraTrack.c_str()))
            nl::core::log::warn("camera track load failed: ", o.cameraTrack);
    }
    {
        nl::viewer::CameraMode m = nl::viewer::CameraMode::Orbit;
        if (o.cameraMode == "free")       m = nl::viewer::CameraMode::Free;
        else if (o.cameraMode == "chase") m = nl::viewer::CameraMode::Chase;
        else if (o.cameraMode == "track" || !o.cameraTrack.empty()) m = nl::viewer::CameraMode::Track;
        if (m != nl::viewer::CameraMode::Orbit) camera.setMode(m);
    }
    edEmbed.bindCamera(&camera);   // M-E3：相机 RPC handler（scene 模式）
    edEmbed.bindScene(&objects);          // M-E4d：场景树数据源（每帧原地刷新）
    edEmbed.bindMaterials(nullptr);       // M-E5：材质读写（scene 模式实例表）

    // ---- P8 M-GL2：场景模式双帧时钟 + game.* console + M-S3 音频场景集成 ----
    nl::core::GameClock gameClock;
    nl::core::console::Console& sceneConsole = nl::core::console::Console::get();
    registerGameConsole(&gameClock);          // game.pause/timeScale/status
    nl::audio::registerAudioConsole(sceneConsole);
    // P8 M-CAM b-segment：相机 cvars（cam.mode/cam.smooth.*/cam.shake.*/cam.chase.*/cam.free.*）
    bool viewerHudEnabled = o.hudEnabled;
    registerViewerConsole(&camera, &viewerHudEnabled);
    // M-S3：AudioWorld（realtime；设备不可用降级 offline——位置读回同步不依赖设备，
    // 截图自动化无扬声器也能跑）。声源绑定实体（按名），每个逻辑帧同步位置。
    nl::audio::AudioConfig audioCfg{};
    audioCfg.offline = false;
    audioCfg.spatialization = true;
    std::unique_ptr<nl::audio::AudioWorld> audioWorld;
    struct AudioBind { std::uint32_t src = 0; nl::scene::Entity ent{}; std::string name; };
    std::vector<AudioBind> audioBinds;
    auto ensureAudioWorld = [&]() -> nl::audio::AudioWorld* {
        if (audioWorld) return audioWorld.get();
        auto w = std::make_unique<nl::audio::AudioWorld>(audioCfg);
        if (!w->ok()) {
            nl::core::log::info("audio: device init failed (", w->error(), ") -> offline fallback");
            audioCfg.offline = true;
            w = std::make_unique<nl::audio::AudioWorld>(audioCfg);
        }
        if (!w->ok()) {
            nl::core::log::error("audio: AudioWorld init failed: ", w->error());
            return nullptr;
        }
        audioWorld = std::move(w);
        return audioWorld.get();
    };
    sceneConsole.registerCommand("audio.play",
        "create+start a looping spatial source bound to a scene entity: audio.play <wav> <entityName>",
        [&](const std::vector<std::string>& args) -> nl::core::console::CommandResult {
            if (args.size() != 2)
                return {false, "usage: audio.play <wav> <entityName>"};
            auto* w = ensureAudioWorld();
            if (!w) return {false, "audio.play: AudioWorld unavailable"};
            std::string createErr;
            const auto id = w->createSource(args[0], &createErr);
            if (id == 0) return {false, "audio.play: createSource failed: " + createErr};
            const auto ent = world.findEntityByName(args[1]);
            if (!ent) return {false, "audio.play: entity not found: " + args[1]};
            const auto* t = world.getComponent<nl::scene::TransformComponent>(ent->id);
            if (!t) return {false, "audio.play: entity has no Transform: " + args[1]};
            const auto p = t->getWorldPosition();
            w->configureSource(id, 1.0f, /*looping=*/true);
            w->setSourcePosition(id, {p.x, p.y, p.z});
            w->startSource(id);
            audioBinds.push_back({id, *ent, args[1]});
            char buf[128];
            std::snprintf(buf, sizeof(buf), "audio.play: src %u <-> %s @(%.3f, %.3f, %.3f)",
                          id, args[1].c_str(), p.x, p.y, p.z);
            return {true, std::string(buf)};
        });
    // M-GL3：渲染帧插值开关（--interp 初值；game.interp 运行时切换）。
    bool interpOn = o.interp;
    sceneConsole.registerCommand("game.interp",
        "toggle/set render-frame interpolation (on|off); render-only, never written back to sim",
        [&](const std::vector<std::string>& args) -> nl::core::console::CommandResult {
            if (args.size() == 1) {
                const auto& v = args[0];
                if (v == "on" || v == "1" || v == "true") interpOn = true;
                else if (v == "off" || v == "0" || v == "false") interpOn = false;
                else return {false, "usage: game.interp [on|off]"};
            } else if (args.size() > 1) {
                return {false, "usage: game.interp [on|off]"};
            } else {
                interpOn = !interpOn;   // 无参 = 翻转（game.pause 同款）
            }
            return {true, std::string("game.interp -> ") + (interpOn ? "on" : "off")};
        });
    // ---- M-GL4：脚本宿主装载（--script，scene 模式）----
    // 时序：exec lines 之前（脚本 Entry 注册的命令对 --console-exec 立即可用）；
    // ecsSetWorld 绑定后 OnTick/ECS 导出才可用。产物定位 = nl-script 工具同款
    //（proj/bin/Debug/net8.0/，GenerateRuntimeConfigurationFiles 保证唯一 rtc）。
    std::unique_ptr<nl::script::Vm> scriptVm;
    bool scriptLoaded = false;
    if (!o.scriptProject.empty()) {
        namespace fs = std::filesystem;
        const fs::path proj = o.scriptProject;
        const fs::path buildDir = proj / "bin" / "Debug" / "net8.0";
        // 产物定位（nl-script 工具同款 glob 口径）：runtimeconfig 唯一，dll 名
        // 按 AssemblyName（= runtimeconfig 名）——不按工程目录名（两者可不同）。
        fs::path rtc, dll;
        if (fs::is_directory(buildDir)) {
            for (const auto& e : fs::directory_iterator(buildDir)) {
                const std::string n = e.path().filename().string();
                if (n.size() >= 19 && n.rfind(".runtimeconfig.json") == n.size() - 19)
                    rtc = e.path();
            }
            if (!rtc.empty()) {
                // dll 名 = runtimeconfig 名去掉 ".runtimeconfig.json" 后缀 + ".dll"
                //（不能 replace_extension——那会从最后一个点替换成 *.runtimeconfig.dll）。
                const std::string n = rtc.filename().string();
                dll = buildDir / (n.substr(0, n.size() - 19) + ".dll");
            }
        }
        if (rtc.empty() || !fs::is_regular_file(dll)) {
            nl::core::log::error("script: build outputs missing under ", buildDir.string(),
                                 " (*.runtimeconfig.json + *.dll) — run: dotnet build ", proj.string());
            return 1;
        }
        nl::script::Result vr;
        // hosting API 口径：assembly/runtimeconfig 路径一律绝对化（相对路径在
        // load_assembly_and_get_function_pointer 一步可能 0x80131509）。
        scriptVm = nl::script::Vm::create(fs::absolute(rtc).string(), &vr);
        if (!scriptVm) {
            nl::core::log::error("script: Vm::create failed: ", vr.message);
            return 1;
        }
        const auto ir = nl::script::ScriptHost::get().init(
            scriptVm.get(), fs::absolute(o.scriptBootstrap).string());
        if (!ir.ok) {
            nl::core::log::error("script: ScriptHost init failed: ", ir.message);
            return 1;
        }
        // typeName 缺省推导（nl-script 工具同款双锚约定）：类名 = 工程目录名
        // 首字母大写 + "Script"，程序集名 = AssemblyName（= rtc 文件名，dll 同名）。
        std::string dirStem = proj.filename().string();
        if (!dirStem.empty())
            dirStem[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(dirStem[0])));
        // 程序集名 = rtc 文件名去掉 ".runtimeconfig.json"（19 字符）；stem() 只去
        // ".json" 会残留 ".runtimeconfig"（BootCall -101 的教训）。
        const std::string asmName = rtc.filename().string().substr(0, rtc.filename().string().size() - 19);
        const std::string typeName = o.scriptType.empty()
            ? ("NlScript.Examples." + dirStem + "Script, " + asmName) : o.scriptType;
        // World 绑定必须先于 loadScript：Entry（装载期）就会 FindByName，
        // 未绑 World = EcsCode.NoWorld → Entry 返回非 0（M-GL4 实证教训）。
        nl::script::ecsSetWorld(&world);
        const auto lr2 = nl::script::ScriptHost::get().loadScript(
            fs::absolute(dll).string(), typeName);
        if (!lr2.ok) {
            nl::core::log::error("script: loadScript failed: ", lr2.message);
            return 1;
        }
        scriptLoaded = true;
        nl::core::log::info("script: loaded ", dll.string(), " type=", typeName,
                            " (host-level OnTick on logic frame)");
    }
    sceneConsole.registerCommand("game.script",
        "script host status (loaded dll/type, tick enabled/count); requires --script",
        [&](const std::vector<std::string>&) -> nl::core::console::CommandResult {
            if (!scriptLoaded) return {false, "game.script: no script loaded (run with --script <projectDir>)"};
            const auto& h = nl::script::ScriptHost::get();
            char sbuf[256];
            std::snprintf(sbuf, sizeof(sbuf),
                          "script: %s type=%s tick=%s count=%llu",
                          h.scriptDll().c_str(), h.scriptType().c_str(),
                          h.tickEnabled() ? "on" : "off",
                          static_cast<unsigned long long>(h.tickCount()));
            return {true, std::string(sbuf)};
        });
    // P8 M-GL5：TickGroup 注册制——逻辑帧组序的显式契约（tick_groups.hpp）。
    // 注册必须在 console-exec 预执行之前（否则 game.tickgroups 读到全 off——
    // 本轮实测教训：首版注册放帧循环前、预执行后，验收第一跑即暴露）。
    // M-GL2 既有执行顺序原样固化到组序：script OnTick → ECS world.tick → audio
    // 声源同步，映射为 PrePhysics → PostPhysics → PostUpdateWork（Physics 组本
    // 模式不注册；空组 runTick 时跳过）。语义逐位同旧 lambda：
    //   - scriptTick(dt) 先行（spawn/remove 排队，tick 开头 flush 语义不变）；
    //   - tickParallel 失败置 failed（原 return 跳过当帧 audio → audio 组以
    //     failed 为闸保持同语义）；
    //   - audioBinds 运行期可被 audio.play 追加 → 各 lambda 按引用读活值。
    bool failed = false;
    gameClock.scheduler().set(nl::core::TickGroup::PrePhysics,
                              [&scriptLoaded](double dt, std::uint64_t) {
        if (!scriptLoaded) return;
        const auto sr = nl::script::ScriptHost::get().scriptTick(dt);
        if (!sr.ok)
            nl::core::log::error("script: ", sr.message);
    });
    gameClock.scheduler().set(nl::core::TickGroup::PostPhysics,
                              [&o, &world, &failed](double dt, std::uint64_t) {
        if (o.tickMode == "serial") {
            world.tick(dt);
        } else {
            std::string tickErr;
            if (!world.tickParallel(dt, 4, tickErr)) {
                nl::core::log::error("ecs tickParallel failed: ", tickErr);
                failed = true;
            }
        }
    });
    gameClock.scheduler().set(nl::core::TickGroup::PostUpdateWork,
                              [&failed, &world, &audioBinds, &audioWorld](double, std::uint64_t) {
        if (failed) return;   // ECS tick 失败当帧跳过（旧 return 语义）
        for (const auto& b : audioBinds) {
            const auto* t = world.getComponent<nl::scene::TransformComponent>(b.ent.id);
            if (!t) continue;
            const auto p = t->getWorldPosition();
            audioWorld->setSourcePosition(b.src, {p.x, p.y, p.z});
        }
    });
    nl::core::log::info("tick groups: ", nl::core::tickGroupName(nl::core::TickGroup::PrePhysics),
                        "->", nl::core::tickGroupName(nl::core::TickGroup::PostPhysics),
                        "->", nl::core::tickGroupName(nl::core::TickGroup::PostUpdateWork),
                        " (registered=", gameClock.scheduler().registeredCount(), ")");
    for (const auto& line : o.consoleExecLines) {
        const auto r = sceneConsole.exec(line);
        if (r.ok()) {
            nl::core::log::info("console exec [0] ", line, " -> ", r.output);
        } else {
            nl::core::log::error("console exec [", r.exitCode(), "] ", line, " -> ", r.output);
        }
    }

    const auto st = nl::renderer::status();
    const auto& info = renderer->info();
    nl::core::log::info("backend: ", st.backend, " | ", info.adapterName);
    nl::core::log::info("mode: scene | objects=", info.instanceCount,
                        " triangles=", info.triangleCount,
                        " model=", info.modelName,
                        " | HUD: ", hudOn ? "on" : "off",
                        " | case: ESC=quit, F12=PNG");

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
    double dtSumMs = 0.0;
    double dtMaxMs = 0.0;

    while (!window.shouldClose()) {
        window.pollEvents();
        edEmbed.pump();   // M-E2：编辑器 RPC 泵（未连接时空操作）
        if (window.keyPressed(nl::core::Key::Escape)) {
            nl::core::log::info("ESC -> exit");
            break;
        }
        float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
        window.consumeMouse(dx, dy, scroll);
        if (window.leftButtonDown() && (dx != 0.0f || dy != 0.0f)) camera.orbit(dx, dy);
        if (scroll != 0.0f) camera.zoom(scroll);
        // P8 M-CAM b-segment：玩法相机输入（WASD 移动 + 鼠标 look + T 切换 Chase↔Free）
        camera.lookPx[0] = dx; camera.lookPx[1] = dy;
        {
            nl::core::FreeDrive fd{};
            fd.move[2] = (window.rawKeyDown(87) ? 1.0f : 0.0f) - (window.rawKeyDown(83) ? 1.0f : 0.0f); // W/S
            fd.move[0] = (window.rawKeyDown(68) ? 1.0f : 0.0f) - (window.rawKeyDown(65) ? 1.0f : 0.0f); // D/A
            fd.move[1] = (window.rawKeyDown(32) ? 1.0f : 0.0f) - (window.rawKeyDown(341) ? 1.0f : 0.0f); // Space/LCtrl
            fd.boost   = window.rawKeyDown(340) != 0;  // LeftShift
            camera.freeInput = fd;
        }
        camera.camToggleEdge = window.keyPressed(nl::core::Key::T);

        const char* capturePath = nullptr;
        if (window.keyPressed(nl::core::Key::F12)) {
            capturePath = o.screenshot ? o.screenshotPath.c_str() : nullptr;
        }
        if (o.screenshot && frame + 1 == static_cast<std::uint64_t>(o.frames))
            capturePath = o.screenshotPath.c_str();

        nl::core::Extent resized{};
        if (window.consumeResize(resized) && resized.width > 0 && resized.height > 0) {
            renderer->resize({static_cast<std::uint32_t>(resized.width),
                              static_cast<std::uint32_t>(resized.height)});
            camera.setAspect(static_cast<float>(resized.width) / static_cast<float>(resized.height));
        }

        const auto now = clock::now();
        const double dtMs = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        // P8 M-GL2：钉渲染帧 delta（对拍确定性）——fixed-dt 下 240 帧恰好 240 tick，
        // 与旧「逐渲染帧固定步长」轨迹逐位一致 → 既有基线零重拍。
        const double clockDtMs = (o.fixedDtMs > 0.0) ? o.fixedDtMs : dtMs;
        ++frame;
        if (o.screenshot) {
            dtSumMs += dtMs;
            if (dtMs > dtMaxMs) dtMaxMs = dtMs;
        }

        // ---- M-E1b.5 渲染用例：逐帧 tick + 刷实例矩阵 ----
        // 固定 dt = 1/60（截图确定性：真实 dt 会让 parallel/serial 截图不可比；
        // HUD deterministic 已有固定样本先例）。tick 在 renderFrame 之前——
        // 本帧 tick 的矩阵本帧就上屏（updateEcsObjects 的 upload 先于
        // renderFrame 的命令列表执行）。
        if (ticking) {
            // 逻辑帧步长由 GameClock::kStep 携带（M-GL5 起组回调经 dt 参数透传）。
            // P8 M-GL3：tick 前 snapshot prev（上一 tick 边界态 = 上帧 tick 后态）。
            // 无条件维护（不依赖 interpOn——game.interp 运行时切换零毛刺）。
            for (std::size_t k = 0; k < modelIds.size(); ++k) {
                const auto* t = world.getComponent<nl::scene::TransformComponent>(modelIds[k]);
                interpPrev[k] = decomposeWorldTRS(t->getWorldMatrix());
            }
            // P8 M-GL2：逐渲染帧固定步长 → GameClock 双帧驱动（暂停=game.pause、
            // 倍速=game.timeScale、250ms 截断全在时钟内）。逻辑帧内：ECS tick +
            // M-S3 音频同步（声源随实体）。tickParallel 失败置 failed，callback 外断出。
            // P8 M-GL5：回调体迁移为 TickGroup 组注册（上方 PrePhysics/PostPhysics/
            // PostUpdateWork 三组），这里只喂时钟——组序 = 枚举序 = 旧 lambda 顺序。
            gameClock.tick(clockDtMs);
            if (failed) break;
            // listener 随相机（渲染帧节奏——漫游时暂停听筒仍活；截图模式相机不动）。
            if (audioWorld) {
                float eye3[3];
                camera.eyePosition(eye3);
                audioWorld->setListenerPosition({eye3[0], eye3[1], eye3[2]});
            }
            // M-GL3：tick 后 snapshot curr（本帧最终 tick 边界态）。
            for (std::size_t k = 0; k < modelIds.size(); ++k) {
                const auto* t = world.getComponent<nl::scene::TransformComponent>(modelIds[k]);
                interpCurr[k] = decomposeWorldTRS(t->getWorldMatrix());
            }
            // 刷新实例数据：Transform 重取世界矩阵（getWorldMatrix 递归现算，
            // 改 rotation 即生效）。tint/材质不变，updateEcsObjects 全量覆盖。
            // M-GL3：交互态 interpOn 时渲染帧用 prev/curr 插值态（alpha =
            // accumulator/step，bevy overstep/timestep 同口径），渲染专用不回写
            // 组件。截图模式恒走原 getWorldMatrix() 路径（截图=确定性产物：
            // 与不插值同一代码路径 → PNG 逐字节同；分解→重构 roundtrip 非逐位，
            // 钉 alpha 仍会引入 1ulp 级差异，故不采用钉 alpha 方案——首版实测
            // PNG MD5 不等即此因）。插值数值证据走捕获帧 probe 日志。
            for (std::size_t k = 0; k < modelIds.size(); ++k) {
                const auto* t = world.getComponent<nl::scene::TransformComponent>(modelIds[k]);
                const auto* m = world.getComponent<nl::scene::ModelComponent>(modelIds[k]);
                nl::math::Mat4 wm;
                if (interpOn && !o.screenshot) {
                    const float a = static_cast<float>(gameClock.alpha());
                    const auto& s0 = interpPrev[k];
                    const auto& s1 = interpCurr[k];
                    wm = nl::math::mat4TRS(nl::math::lerpVec3(s0.pos, s1.pos, a),
                                           nl::math::slerpQuat(s0.rot, s1.rot, a),
                                           nl::math::lerpVec3(s0.scale, s1.scale, a));
                } else {
                    wm = t->getWorldMatrix();
                }
                std::memcpy(objects[k].world, wm.m, sizeof(objects[k].world));
                objects[k].tint[0] = m->tint.x;
                objects[k].tint[1] = m->tint.y;
                objects[k].tint[2] = m->tint.z;
                objects[k].roughness = m->roughness;
                objects[k].metallic  = m->metallic;
            }
            if (!renderer->updateEcsObjects(objects.data(),
                                            static_cast<std::uint32_t>(objects.size()))) {
                nl::core::log::error("ecs updateEcsObjects failed: ", renderer->lastError());
                failed = true;
                break;
            }
        }

        // M-GL3 机器证据（捕获帧）：插值探针——PNG 恒走 tick 边界原路径（见上），
        // 日志记录真实 alpha 与 prev/curr/interp 数值（verify.py 孪生复算
        // lerp/slerp + SpinSystem 轨道位置）。暂停帧 prev==curr，插值态恒等于冻结态。
        if (o.screenshot && frame == static_cast<std::uint64_t>(o.frames) && interpOn &&
            !modelIds.empty()) {
            const double probeAlpha = gameClock.alpha();
            for (std::size_t k = 0; k < modelIds.size(); ++k) {
                const auto& s0 = interpPrev[k];
                const auto& s1 = interpCurr[k];
                const auto ip = nl::math::lerpVec3(s0.pos, s1.pos,
                                                   static_cast<float>(probeAlpha));
                const auto iq = nl::math::slerpQuat(s0.rot, s1.rot,
                                                    static_cast<float>(probeAlpha));
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                              "interp probe: k=%zu alpha=%.6f"
                              " prev=(%.6f,%.6f,%.6f) curr=(%.6f,%.6f,%.6f)"
                              " interp=(%.6f,%.6f,%.6f)"
                              " pq=(%.6f,%.6f,%.6f,%.6f) cq=(%.6f,%.6f,%.6f,%.6f)"
                              " iq=(%.6f,%.6f,%.6f,%.6f)",
                              k, probeAlpha,
                              s0.pos.x, s0.pos.y, s0.pos.z,
                              s1.pos.x, s1.pos.y, s1.pos.z,
                              ip.x, ip.y, ip.z,
                              s0.rot.x, s0.rot.y, s0.rot.z, s0.rot.w,
                              s1.rot.x, s1.rot.y, s1.rot.z, s1.rot.w,
                              iq.x, iq.y, iq.z, iq.w);
                nl::core::log::info(buf);
            }
        }

        // M-S3 机器证据（捕获帧，口径 frame == o.frames 与 PNG 同迭代）：AudioWorld
        // snapshot 是 DebugController 之后的权威状态——声源位置应与实体世界位置一致。
        // 暂停用例里这行即「暂停即停」的数值证词（位置冻结在暂停时刻）。
        if (o.screenshot && frame == static_cast<std::uint64_t>(o.frames) &&
            audioWorld && !audioBinds.empty()) {
            for (const auto& b : audioBinds) {
                for (const auto& s : audioWorld->snapshot()) {
                    if (s.id != b.src) continue;
                    const auto* t = world.getComponent<nl::scene::TransformComponent>(b.ent.id);
                    const auto p = t ? t->getWorldPosition() : nl::math::Vec3{};
                    const bool eq = std::fabs(p.x - s.position.x) < 1e-6f &&
                                    std::fabs(p.y - s.position.y) < 1e-6f &&
                                    std::fabs(p.z - s.position.z) < 1e-6f;
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "audio readback: src=%u entity=%s entityPos=(%.6f, %.6f, %.6f)"
                                  " srcPos=(%.6f, %.6f, %.6f) equal=%d",
                                  s.id, b.name.c_str(), p.x, p.y, p.z,
                                  s.position.x, s.position.y, s.position.z, eq ? 1 : 0);
                    nl::core::log::info(buf);
                }
            }
            nl::core::log::info("audio clock state: simTime=", gameClock.simTime(),
                                " ticks=", gameClock.tickCount(),
                                " paused=", gameClock.paused() ? 1 : 0,
                                " scale=", gameClock.timeScale());
        }

        // M-GL4 机器证据（捕获帧，口径 frame == o.frames 与 PNG 同迭代）：脚本实体
        // 世界位置读回 + 宿主 tick 计数——verify.py 三重孪生（时钟 ticks 序列复推、
        // 轨道公式、脚本态 t 与宿主 tickCount 相等性）。暂停用例里 ticks=0 且位置
        // 停在场景初始态 = 「脚本随逻辑帧冻结」的数值证词。
        if (o.screenshot && frame == static_cast<std::uint64_t>(o.frames) && scriptLoaded) {
            const auto orb = world.findEntityByName("script-orb");
            if (orb) {
                const auto* t = world.getComponent<nl::scene::TransformComponent>(orb->id);
                if (t) {
                    const auto p = t->getWorldPosition();
                    char sbuf[200];
                    std::snprintf(sbuf, sizeof(sbuf),
                                  "script probe: ticks=%llu pos=(%.6f,%.6f,%.6f)",
                                  static_cast<unsigned long long>(nl::script::ScriptHost::get().tickCount()),
                                  p.x, p.y, p.z);
                    nl::core::log::info(sbuf);
                }
            }
            nl::core::log::info("script clock state: simTime=", gameClock.simTime(),
                                " ticks=", gameClock.tickCount(),
                                " paused=", gameClock.paused() ? 1 : 0);
        }

        nl::renderer::FrameParams fp;
        // P8 M-CAM b-segment：runScene 无物理车体源（chassis 留空），仅推进玩法相机
        camera.update(static_cast<float>(dtMs) * 0.001f);
        const auto vp = camera.viewProjection();
        std::memcpy(fp.mvp, vp.m, sizeof(fp.mvp));   // model = 单位阵 → mvp = viewProj
        // M8.2：rt 模式射线生成输入（runScene 同构接线；其它模式不读）。
        std::memcpy(fp.viewProj, vp.m, sizeof(fp.viewProj));
        fp.rtDebugMode = o.rtDebugMode;
        fp.lightDir[3] = ambient;                    // w = ambient 透传（scene draw 也写灯缓冲）
        fp.cullBack = o.cullBack;
        fp.shading = 0;                              // psScene 固定 mode 0 风格着色
        fp.wireframe = o.wireframe;
        // M-E1b.3：视向量 V 来源——真实相机世界坐标（psScene 从 b1 cameraPos 读）。
        float eye3[3];
        camera.eyePosition(eye3);
        fp.eye[0] = eye3[0]; fp.eye[1] = eye3[1]; fp.eye[2] = eye3[2];

        nl::renderer::HudFrame hud{};
        if (hudOn) {
            const auto e = renderer->extent();
            hud.displayWidth  = static_cast<float>(e.width);
            hud.displayHeight = static_cast<float>(e.height);
            hud.yawDeg = camera.yawDegrees();
            hud.pitchDeg = camera.pitchDegrees();
            hud.distance = camera.distance();
            hud.backend = st.backend;
            hud.adapter = info.adapterName.c_str();
            hud.debugLayer = info.debugLayer.c_str();
            hud.mode = camera.modeName();       // P8 M-CAM b-segment：显示当前相机模式（orbit/free/chase/track）
            hud.stage = st.stage;
            hud.model = info.modelName.c_str();
            hud.triangleCount = static_cast<int>(info.triangleCount);
            hud.cullBack = o.cullBack;
            hud.colorIndex = colorIndex;
            hud.deterministic = o.screenshot;
            if (o.screenshot) {
                hud.fps = 60.0f;
                hud.frameMs = 16.67f;
                hud.frameMsCount = kFixedSampleCount;
                std::memcpy(hud.frameMsHistory, kFixedSamples, sizeof(kFixedSamples));
                hud.gpuMs = 0.63f;
                hud.gpuMsCount = kFixedSampleCount;
                std::memcpy(hud.gpuMsHistory, kFixedGpuSamples, sizeof(kFixedGpuSamples));
                hud.deltaSeconds = 1.0f / 60.0f;
                hud.mouseX = -100.0f; hud.mouseY = -100.0f;
            } else {
                hud.fps = dtMs > 0.0 ? static_cast<float>(1000.0 / dtMs) : 0.0f;
                hud.frameMs = static_cast<float>(dtMs);
                hud.deltaSeconds = static_cast<float>(std::max(dtMs / 1000.0, 1.0 / 1000.0));
            }
        }

        if (!renderer->renderFrame(kColors[colorIndex], fp, capturePath, hudOn ? &hud : nullptr)) {
            nl::core::log::error("renderFrame failed: ", renderer->lastError());
            failed = true;
            break;
        }
        renderer->drainMessages();
        if (capturePath) {
            ++screenshotsSaved;
            nl::core::log::info("screenshot written: ", capturePath);
        }
        if (o.screenshot && frame % 15 == 0) {
            nl::core::log::info("ecs stats: objects ", info.instanceCount,
                                " | triangles ", info.triangleCount,
                                " | lights ", lights.size());
        }
        if (o.screenshot && frame >= static_cast<std::uint64_t>(o.frames)) break;
        if (!o.screenshot) std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // M-E1b.5 机器出口（run.bat findstr 断言）：tick 模式 + 帧数 + 物体数。
    if (ticking) {
        nl::core::log::info("ecs tick summary: mode=", o.tickMode, " frames=", frame,
                            " spinObjects=", modelIds.size());
    }
    nl::core::log::info("shutdown ok (frames=", frame,
                        " screenshots=", screenshotsSaved,
                        " deviceErrors=", nl::core::log::deviceErrorCount(), ")");
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
    // M-GL4 收尾（次序铁律）：先解绑 World（热重载窗口兜底口径，s7g 同源）→
    // 卸脚本（内部先清 tickFn_ / 命令 thunk）→ 析构 Vm（hostfxr context 收官）。
    if (scriptLoaded) {
        nl::script::ecsSetWorld(nullptr);
        const auto ur = nl::script::ScriptHost::get().unloadScript();
        if (!ur.ok)
            nl::core::log::error("script: unloadScript failed: ", ur.message);
        scriptVm.reset();
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
    case nl::core::ViewMode::Scene:      // M-E1b.0：ECS 场景（--scene 文件 → N 物体 M 灯）
        return runScene(o);
    case nl::core::ViewMode::Visbuf:     // M7.0：u64 vis buffer 打包 + 簇/三角可视化
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/true);
    case nl::core::ViewMode::Pulling:    // M7.1：vis buffer 材质反解（pulling）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/true, /*pullingMode=*/true);
    case nl::core::ViewMode::InputHud:   // P8 M-I2：输入调试面板（normal 场景打底 + 注入面板）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/false, /*pullingMode=*/false,
                         /*inputHudMode=*/true);
    case nl::core::ViewMode::Rt:         // M8.2：Ray Query 调试射线闭环（.nmesh 资产链拿 BLAS/TLAS）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/false, /*pullingMode=*/false,
                         /*inputHudMode=*/false, /*rtMode=*/true);
    case nl::core::ViewMode::Shadow: // M9.0：compute 直光对照（Q4 方案 A，无阴影射线）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/false, /*pullingMode=*/false,
                         /*inputHudMode=*/false, /*rtMode=*/false, /*shadowMode=*/true);
    case nl::core::ViewMode::Reference: // M9.2：离线参考模式（渐进式多帧累积直光真值图）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/false, /*pullingMode=*/false,
                         /*inputHudMode=*/false, /*rtMode=*/false, /*shadowMode=*/false,
                         /*referenceMode=*/true);
    case nl::core::ViewMode::ProbePlace: // M10.0：屏幕探针 cascade 布点 + 点可视化
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/true, /*pullingMode=*/true,
                         /*inputHudMode=*/false, /*rtMode=*/false, /*shadowMode=*/false,
                         /*referenceMode=*/false, /*probeMode=*/true);
    case nl::core::ViewMode::ProbeViz:   // M10.1：探针辐照度可视化（追踪 + 点色采样）
        return runNormal(o, /*cluster=*/false, /*lodMode=*/false, /*dagTop=*/false,
                         /*cullMode=*/false, /*dagMode=*/false, /*skinMode=*/false,
                         /*hizMode=*/false, /*visbufMode=*/true, /*pullingMode=*/true,
                         /*inputHudMode=*/false, /*rtMode=*/false, /*shadowMode=*/false,
                         /*referenceMode=*/false, /*probeMode=*/true);
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
              << "  viewer.exe --editor-bridge         connect external editor at 127.0.0.1:29110;\n"
              << "                                     window gets embedded via SetParent (M-E2);\n"
              << "                                     no editor running -> standalone (graceful)\n"
              << "  viewer.exe --editor-endpoint <ep>  M-E6: tcp://127.0.0.1:29110 (default) or pipe://name\n"
              << "  viewer.exe --view-id <id>          M-E6: viewport key for multi-view editors (default main)\n"
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
              << "  viewer.exe --vis-viz cluster|triangle\n"
              << "                                     visbuf mode: decode vis buffer by clusterId\n"
              << "                                     or by global triangle id (default cluster)\n"
              << "  viewer.exe --shading <name>        textured | normal | uv | tri-id | pbr (default textured;\n"
              << "                                     N key cycles 0..3; pbr = Cook-Torrance direct light)\n"
              << "  viewer.exe --pbr-naive             pbr debug A/B: skip sRGB->linear (naive UNORM read,\n"
              << "                                     acceptance comparison evidence)\n"
              << "  viewer.exe --wireframe             raster wireframe fill mode (V key toggles)\n"
              << "  viewer.exe --yaw D --pitch D       fixed orbit angles in degrees (screenshot reproducibility)\n"
              << "  viewer.exe --model <name>          load assets/<name>: .glb for normal/skin mode,\n"
              << "                                     .nmesh for cluster/lod/visbuf/pulling mode (default per mode)\n"
              << "  viewer.exe --scene <file>          M-E1b.0: load an ECS scene JSON (World::serialise\n"
              << "                                     format); forces --mode scene. Objects/lights come\n"
              << "                                     from Model/Transform/Light components.\n"
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
              << "  viewer.exe --physics-scenario 2 --physics-debug [--drive-demo]\n"
              << "                                     M-D3 drive training ground; --drive-demo =\n"
              << "                                     deterministic frame-indexed input script\n"
              << "  viewer.exe --drive-hud             M-D3 separate drive debug window (default off)\n"
              << "                                     CVars: vehicle.throttleScale / steerScale / debug\n"
              << "  viewer.exe --fixed-dt <ms>         M-GL1: pin per-frame delta fed to the game clock\n"
              << "  viewer.exe --interp                M-GL3: render-frame interpolation (alpha = accumulator/step)\n"
              << "  viewer.exe --script <projectDir>   M-GL4: load script host (scene mode); host-level\n"
              << "                                     OnTick driven by logic frame (--script-type/-bootstrap)\n"
              << "  console: game.pause|game.timeScale|game.status (M-GL1/GL2 dual-frame)\n"
              << "  console: game.script (M-GL4: script host status)\n"
              << "  console: audio.play <wav> <entity> (M-S3: spatial source bound to entity)\n"
              << "                                     (deterministic pause/timescale screenshot runs)\n"
              << "  viewer.exe --console-type <text>   prefill console input line (tab-complete demo)\n"
              << "  viewer.exe --physics-debug         M-P3 overlay: Jolt collision-shape wireframe\n"
              << "                                     (floor + falling box/sphere/capsule);\n"
              << "                                     forces --mode normal for depth reference\n"
              << "  viewer.exe --physics-scenario N    M-P3 scene select (default 0)\n"
              << "  viewer.exe --physics-seed N        M-P3 seed (recorded; no RNG yet)\n"
              << "  viewer.exe --particles             M-PT0 overlay: instanced billboard fountain\n"
              << "  viewer.exe --nfx <file.nfx>        M-PT1: load .nfx v1 (implies --particles)\n"
              << "                                     (fixed-step CPU sim, HUD alive count)\n"
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
         o.mode == nl::core::ViewMode::Dag || o.mode == nl::core::ViewMode::Hiz ||
         o.mode == nl::core::ViewMode::Visbuf || o.mode == nl::core::ViewMode::Pulling ||
         o.mode == nl::core::ViewMode::Rt || o.mode == nl::core::ViewMode::Shadow ||
         o.mode == nl::core::ViewMode::Reference || o.mode == nl::core::ViewMode::ProbePlace)
        && !o.modelSet) {
        o.modelFile = "Suzanne.nmesh";
    }
    // M10.1：probe-viz 缺省带 fallback 段的资产——探针追踪打 TLAS（M8 粗代理
    // 需 v3 lod0 'NFLB'），Suzanne.nmesh 无 fallback = TLAS 不就绪 trace 降级。
    if (o.mode == nl::core::ViewMode::ProbeViz && !o.modelSet) {
        o.modelFile = "Suzanne-materials.nmesh";
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
    // M-E1b.0：给了 --scene 就强制 mode=Scene（--mode 同给以 Scene 为准）。
    if (!o.scenePath.empty() && !o.physicsDebug) {
        o.mode = nl::core::ViewMode::Scene;
    }
    return runMode(o.mode, o);
}
