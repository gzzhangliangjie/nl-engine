#pragma once

// M0.2：renderer 是唯一允许出现图形 API / NVRHI 符号的模块。
// 公开接口只暴露 POD 与 std 类型：viewer/ 与 core/ 不接触任何后端类型。
// 后端由顶层 CMake 的 NL_GRAPHICS_BACKEND 在编译期选定，切换后端不需要装任何厂商 SDK。

#include <cstdint>
#include <memory>
#include <string>

#include "nl/renderer/input_debug.hpp"   // P8 M-I2：HudFrame::inputDebug 的 POD 定义
#include "nl/renderer/nav_panel.hpp"     // P8 M-N2：HudFrame::navPanelState 的 POD 定义

namespace nl::renderer {

struct Status {
    const char* backend;      // "dx12" / "vulkan"
    const char* stage;        // 当前里程碑
    bool        deviceReady;  // status() 是编译期信息，恒为 false；设备状态看 Renderer::valid()
};

Status status();

// M0.5 HUD 的无 GPU 自检（顶点布局 / 投影矩阵 / POD 尺寸），供 --selftest 调用。
bool hudSelfTest();

// S0 第三条硬指标的机械证据：核对 NL_SHADER_DIR 下每份 .slang 都同时产出了
// SPIR-V（魔数 0x07230203）与 DXIL（魔数 'DXBC'）的 vs/ps 四个文件，共 8 个产物。
// report 里逐项列出文件名与字节数，--selftest 会打印出来。
bool shaderArtifactsSelfTest(std::string& report);

struct Extent {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

struct ClearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// M0.4 每帧参数：与 model.slang 的 SceneConstants cbuffer 逐字节对应（列主序矩阵）。
// 公开头只放 POD，viewer/core 不需要认识任何 NVRHI / 图形 API 类型。
struct FrameParams {
    float mvp[16]     = {};
    float model[16]   = {};
    float lightDir[4] = {0.45f, 0.80f, 0.50f, 0.28f};  // xyz 方向, w = 环境光
    // M-WEA1（Q-WE5 已拍板）：天气链参数，缺省 = M10.1 sky.slangh 常量/白光
    // → trace/cache/gather/refGI 各 CB 改读字段后，--weather 关时 bytes 保形；
    // --weather-seed >=0 开启时 viewer 用 WeatherState 调制值覆写。
    float weatherZenith[3]  = {0.16f, 0.30f, 0.58f};   // 天顶色（终值，CPU 已调制）
    float weatherHorizon[3] = {0.62f, 0.71f, 0.84f};   // 地平线色（终值）
    float weatherSunCol[3]  = {1.0f, 1.0f, 1.0f};      // 太阳 RGB（色温换算）
    float weatherSunIntensity = 1.0f;                  // 0..1 直光能量（×2.5 基准强度）
    // M-WEA2（Q-W2-2=C1）：高度雾参数。缺省 0 = 雾关 → T≡1 → 全链 bytes 保形。
    // 雾色不单发：消费端取 weatherHorizon 同源（D22 ②）。
    float weatherFogDensity  = 0.0f;                   // 1/m（0 = 雾关）
    float weatherFogHeight   = 0.0f;                   // m
    float options[4]  = {};
    bool  cullBack    = true;   // false = 关背面剔除，能看到内壁
    // M0.8 调试着色 / 线框：缺省值 = M0.7 行为（textured + solid），历史基线不受影响。
    int   shading     = 0;      // 0 textured / 1 normal / 2 uv / 3 tri-id（传进 g_options.x）
    bool  wireframe   = false;  // 光栅化 fill mode = wireframe（深度测试照常）
    // ---- M5.1：dag 模式遍历参数（其它模式不读，缺省值无副作用）----
    // 屏幕误差公式（bevy/zeux 口径，core/lod_error.hpp）：screenPx =
    //   worldError / max(dist(eye, center)-radius, near) * projYScale * 0.5 * viewportHeight
    float eye[3]             = {0.f, 0.f, 4.2f};  // 相机世界位置（轨道相机 eyePosition()）
    float view[16]           = {};                // M6.1：world→view 矩阵（dag/hiz 遍历的
                                                  //  Mara&Morgan 球投影消费；其它模式不读）
    float projYScale         = 2.4142f;           // 1/tan(fovY/2)，fovY=45° 时 2.4142
    float nearPlane          = 0.1f;              // 近断面（与相机投影一致）
    float viewportHeight     = 720.f;             // 投影用的视口高度（px）
    float cutThresholdPx     = 1.0f;              // 组原子 cut 阈值（px，>0）

    // M-A5：蒙皮动画时间（秒，循环采样由 Scene 侧 wrapTime）。仅 skin 模式消费；
    // 其他模式忽略（确定性截图用 --anim-time 固定，实时模式由 viewer 累加 dt）。
    float animTime    = 0.0f;

    // M6.0：Hi-Z 金字塔可视化查看的 mip 层号（hiz 模式消费；0 = mip0 原始深度，
    // 其他模式不读，缺省无副作用）。层级可视化由 GPU 覆盖层绘制，HUD 滑条选层。
    int   hizViewMip  = 0;

    // M6.2：Hi-Z 遮蔽剔除总开关（hiz 模式消费；false = 只做视锥/误差遍历，退化
    // 可验证——S6-PLAN 底线「可整体开关的独立层」，UE r.Nanite.Culling.HZB 同定位）。
    bool  hizEnable   = true;

    // M8.1：TLAS 实例每帧 spin（--tlas-spin）与 RayQuery 探针开关（--tlas-probe）。
    // spin 只改 TLAS 实例 transform，不碰光栅化路径（画面基线不受影响）；
    // probe = 每帧 1 线程 RayQuery 固定射线，hit t 读回日志（transform 下帧生效
    // 的机器可读证据）。缺省全关 → 既有路径零扰动。
    bool  tlasSpin    = false;
    bool  tlasProbe   = false;

    // M15.0（S15）：VSM 虚拟阴影贴图开关（--vsm）。仅 --mode shadow 的 drawShadow
    // 消费：开 = 深度 pass（页池/页表）+ csDirect 阴影因子走 VSM 采样；关 = 射线
    // 路径逐字节不变（s9-shadow 基线零扰动）。其他模式不读，缺省无副作用。
    bool  vsm         = false;

    // M8.2：rt 模式消费——viewProj 矩阵（列主序，camera.viewProjection() 原样
    // 传入；renderer 侧 inverseMat4 求逆做射线生成）与调试可视化档
    // （0 = 命中距离灰度 / 1 = instanceId+triangleId 伪彩）。其它模式不读，
    // 缺省零副作用（全零矩阵仅 rt 模式使用）。
    float viewProj[16] = {};
    int   rtDebugMode  = 0;

    // M9.0：--mode shadow 的空像素必须写与 framebuffer 一致的清屏色（present
    // 全屏覆盖会盖掉 clear，psMatResolve 的 discard 语义由 UAV 自己补）。
    // Renderer::renderFrame 每帧从 ClearColor 填入；其它模式不读，缺省 =
    // viewer kColors[0]，历史基线零扰动。
    float clearColor[4] = {0.05f, 0.08f, 0.15f, 1.0f};

    // M9.2：--mode reference 的总累积帧数（viewer 传 o.frames；drawReference 读它算
    // 进度分母；其它模式不读，缺省 0 = 无进度上报）。
    std::uint32_t refFrames = 0;

    // M10.3（Q14-1）：reference 链分支开关——true = direct.csrefgi 渐进累积
    // GI 参考图（直光真值 + 单弹射蒙特卡洛）；false = M9.2 直光参考，保形。
    bool referenceGI = false;

    // M-P3：物理调试线框叠加层（默认关闭 → S7.0 渲染基线字节不变）。
    // debugLineData 指向调用方（viewer）打包好的线顶点流；每顶点 6 个 float：
    //   px, py, pz, r, g, b（位置 + 顶点色）。debugLineVertexCount 为顶点数，
    // LINE_LIST 语义下每 2 个顶点为一段线。仅当 debugLines==true 且数据非空才绘制。
    // 渲染器只读这块内存、不拥有它；调用方负责其生命周期（每帧重填）。
    bool         debugLines           = false;
    const float* debugLineData        = nullptr;
    std::uint32_t debugLineVertexCount = 0;

    // M7.0：vis buffer 可视化的解码档（仅 visbuf 模式消费，其它模式恒 0 不读）：
    //   0 = clusterId → 颜色，1 = 全局三角 id (clusterId<<8|triId) → 颜色。
    // 走 FrameParams 而不是 SceneConstants：g_options 的槽位已被 cull 遍历占用。
    int   visVizMode  = 0;

    // M-PT0：粒子叠加层总开关（viewer --particles 传入；缺省 false = 既有模式
    // 基线一像素不动）。开启时 renderer 每帧固定步长（1/60s）步进 CPU 模拟并
    // instanced billboard 绘制（主场景后、HUD 前，alpha blend 深度测试开写入关）。
    bool  particles   = false;

    // ---- M10.0：--mode probe-place 探针布点参数（其它模式不读，缺省 = 默认值）。
    // HUD 双向通道同 shading（viewer 持 local，滑条写回后回填）。baseRes 变化会
    // 触发 atlas 纹理按参数 hash 重建（ensureProbeTargets 闸内）。
    int   probeBaseRes      = 16;     // 0 级 cascade 分辨率（4..32；1~3 级逐级减半）
    float probeNormalOffset = 0.10f;  // 法线偏移系数（× 探针世界间距，UE NormalBias 口径）
    float probeDotSize      = 3.0f;   // viz 点半径（px）
    bool  probeVizIrradiance = false; // M10.1：--mode probe-viz（点色 = 八面体辐照度
                                      // 采样；false = probe-place 级别色点，保形）
    bool  probeGather       = false; // M10.2：final gather 态（Q13-3：dotMode=2，
                                      // 全屏 gather 合成 + ambient 替换，无点叠加；
                                      // 与 probeVizIrradiance 互斥，gather 优先）
    // ---- M10.3：probe 完整链路调参（Q14-3 拍板全调参面）+ --sky-bg（Q14-4）----
    float probeLevels[3]    = {3.0f, 5.0f, 7.0f}; // Q13-1 线性深度分段阈值（sceneDepth
                                      // 欧氏距离；HUD 滑条双向，viewer 持 local 回填）
    bool  probeSkyBg        = false;  // --sky-bg：背景从 clearColor 换解析天穹
                                      //（skyGradient 与探针 miss 同函数；默认关保形）

    // ---- S11 M11.0：--mode cache-viz 世界缓存占用伪彩（probe 链之上叠加
    //  fill/decay/viz 三个 compute；其它模式不读，缺省 false 零扰动）。
    bool  cacheViz          = false;
    bool  cacheVizRadiance  = false;   // M11.1：viz 亮度 = 出射辐照度（0=life 模式）
    // ---- S11 M11.2：探针命中查缓存（Q11-2a=A，默认关 = 老 s10-* 卡零漂）----
    bool  probeCacheBounce  = false;   // 命中先查世界缓存，miss 才退回直光近似
    float probeLightScale   = 1.0f;    // 0..1，同步缩放 trace/cache 灯强度+ambient
    bool  probeCacheDiag    = false;   // M11.2 调试：命中查缓存结果按 RGB 伪彩（红=命中/绿=占未中/蓝=空）
                                       //（关灯余晖序列用；1.0 = 老路径基线）
    bool  probeAtlasDump     = false;   // M11.2 验收：把 irradAtlas_[0] 直出画面，绕开 gather 做开关开/关自比

    // ---- S12 M12.0：GI 分量独立时域累积（Q12-A 方案 A）----
    // 仅 --mode filter 消费；缺省全关 = 非 filter 路径零扰动（T5 保形硬边界：
    // 非 filter 的 csGatherCompose 绑定与执行顺序一字不动）。
    // 三层独立开关：时间 / 空间层已落地（M12.0 / M12.1），拒绝层留 M12.2。
    bool  filterTemporal      = false;  // 是否插入 csFilterTemporal pass
    bool  temporalEnable      = true;   // 时间层独立开关（关 = 单层当前帧 GI）
    float temporalAlpha       = 0.1f;   // 当前帧 GI 权重下限（0.01..1，越小历史越重）
    int   temporalHistoryMax  = 32;     // 历史长度上限（1..256，自适应 α=1/(len+1)）
    bool  filterSpatial       = false;  // 空间层（M12.1 已实现，--spatial 开启）
    bool  filterReject        = false;  // 拒绝层（M12.2 已实现，--filter-reject 开启）
    float rejectThreshold     = 0.01f; // M12.2：拒绝阈值（写入 g_flags.w，0 = 层关）
    bool  filterGiDump        = false;  // S12 验收：把累积后的 gatherLinear_ 直出（绕开 compose）
    // ---- M-PERF1：per-pass GPU 计时（Q-P5，--pass-timing 开启；默认关 = 渲染
    // 路径零变化，冻结基线 bytes 不动）。Scene 侧单向锁存（开了不关），query 懒建。
    bool  passTiming          = false;
};

// M0.5 HUD：viewer -> HUD 的显示数据 + HUD -> viewer 的控件值，双向 POD。
// 公开头只放 POD，viewer / core 不接触 ImGui 或任何图形 API 类型。
// M-D3 (Q-D3-3 A): drive parameter panel. Separate sibling window with a
// DEFAULT-OFF gate: when show==false it draws nothing, so none of the 35
// existing --hud cases (70 baseline PNGs) moves. ASCII-only lines.
struct DriveDebugUi {
    bool show = false;
    enum { kMaxLines = 6, kLineChars = 96 };
    char lines[kMaxLines][kLineChars] = {};
    int  lineCount = 0;
};

// M-TR0 (P8-TRACY-PLAN K1/K5/K2): standalone profiling stats window.
// SAME DEFAULT-OFF discipline as DriveDebugUi / InputDebugUi: show=false draws
// nothing, so all existing --hud baseline PNGs stay byte-identical (no kStage
// bump, no re-shoot). ASCII-only POD lines; viewer fills them from DeviceInfo.
struct ProfilingDebugUi {
    bool show = false;
    enum { kMaxLines = 6, kLineChars = 96 };
    char lines[kMaxLines][kLineChars] = {};
    int  lineCount = 0;
};

// NET M-NET2: net session status window. SAME DEFAULT-OFF discipline: only
// --net-server / --net-client runs set show=true, so no existing baseline moves.
// ASCII-only POD lines (connection state / RTT / snapshot tick delta per plan).
struct NetStatusUi {
    bool show = false;
    enum { kMaxLines = 6, kLineChars = 96 };
    char lines[kMaxLines][kLineChars] = {};
    int  lineCount = 0;
};

// M15.1（S15）：VSM clipmap 页池占用面板。同款 DEFAULT-OFF：仅 --vsm-panel 且
// --vsm 时 show=true（Scene::pollVsmHud 在 show 时填充 ASCII 行），既有基线零触碰。
struct VsmDebugUi {
    bool show = false;
    enum { kMaxLines = 8, kLineChars = 96 };
    char lines[kMaxLines][kLineChars] = {};
    int  lineCount = 0;
};

struct HudFrame {
    // ---- viewer -> HUD（显示数据）----
    float fps          = 0.0f;
    float frameMs      = 0.0f;
    float frameMsHistory[120] = {};   // CPU 帧间隔曲线样本
    int   frameMsCount = 0;
    // ---- M-C1：开发者控制台输入通道（POD；GLFW->ImGuiKey 映射在 console_ui.cpp）----
    struct ConsoleKeyEvent {
        int  key = 0;        // GLFW key code 原值
        bool down = false;   // PRESS/REPEAT = true，RELEASE = false
        bool ctrl = false, shift = false, alt = false;
    };
    ConsoleKeyEvent keyEvents[16] = {};  // 本帧键盘事件（溢出丢弃最旧）
    int  keyEventCount  = 0;
    unsigned int textChars[32] = {};     // 本帧文本字符（codepoint；` 已被 core 过滤）
    int  textCharCount  = 0;
    bool consoleOpen    = false;         // viewer 持有开关（` 边沿切换）
    bool consoleTab     = false;         // viewer 边沿：本帧按了 Tab = 补全请求
    const char* consolePrefill = "";     // 输入行预填（演示/截图用；空->非空边沿灌一次）
    // ---- M0.6：renderer 侧 NVRHI timer query 写回的真实 GPU 耗时 ----
    float gpuMs = 0.0f;              // 最近一次 resolve 的整帧 GPU 耗时（ms），异步有 1~2 帧延迟
    float gpuMsHistory[120] = {};    // GPU 耗时曲线样本（viewer 维护）
    int   gpuMsCount = 0;
    bool  gpuTimingAvailable = false; // timer query 是否可用 / 已拿到过结果
    bool  deterministic = false;      // 截图回归模式：时序样本固定，画面可复现
    float yawDeg = 0.0f, pitchDeg = 0.0f, distance = 0.0f;
    float displayWidth = 0.0f, displayHeight = 0.0f;
    float deltaSeconds = 1.0f / 60.0f;
    float mouseX = 0.0f, mouseY = 0.0f, mouseWheel = 0.0f;
    bool  mouseLeftDown = false;
    const char* backend    = "";
    const char* adapter    = "";
    const char* debugLayer = "";
    const char* mode       = "";
    const char* stage      = "";
    const char* model      = "";    // M0.7：--model 加载的 .glb 文件名
    int   triangleCount    = 0;     // M0.7：模型三角形数
    // ---- M0.8：双向——viewer 传当前值，HUD 控件写回（与 cull/color 同一条通道）----
    int   shading          = 0;     // 0 textured / 1 normal / 2 uv / 3 tri-id
    bool  wireframe        = false;
    // ---- 双向：viewer 传当前值，HUD 写回控件结果 ----
    bool cullBack          = true;
    int  colorIndex        = 0;
    bool resetCamera       = false;   // HUD 按钮置 true，viewer 消费后清零
    // M-C2：hud.show cvar 只藏面板不藏控制台（ImGui 帧保持活着，控制台窗口照画）。
    // 基线保证：viewer 仅在 hudEnabled||consoleOpen 时才传 HudFrame，缺省 true 不波及历史。
    bool panelShow         = true;
    bool wantCaptureMouse  = false;   // HUD 输出：鼠标在面板上，viewer 别拖相机
    bool wantCaptureKeyboard = false; // M-C1 HUD 输出：控制台输入行在打字，viewer 屏蔽游戏热键
    // M2.3：LOD 层切换（仅 lod 模式；lodCount<=1 时 HUD 不画滑条）----
    // 与 cull/color 同一条双向通道：viewer 每帧填当前层，滑条写回后由 viewer
    // 检测变化并调 Renderer::setLodLevel()——HUD 不直接碰渲染资源。
    int   lodLevel         = 0;       // 当前层号（双向）
    int   lodCount         = 0;       // 可用层数（0/1 = 没有可切的分层资产）
    int   lodTriangles     = 0;       // 当前层三角形数（悬停证据：切层确实换了资产）
    float lodMaxError      = 0.0f;    // 当前层 parent error（世界空间，单调不减）
    // ---- M3.2：DAG 组下钻（仅 dag-top 模式；dagGroupCount<=0 时 HUD 不画滑条）----
    // 与 lod 滑条同一条双向通道：viewer 检测变化后调 Renderer::setDagCut()。
    // -1 = 纯顶层（不下钻）；0..N-1 = 下钻该组（挖掉其父簇、换回组内子簇）。
    int   dagGroup         = -1;      // 当前下钻组（双向；-1 = 无）
    int   dagGroupCount    = 0;       // 可下钻组数（子层 dagGroups.size()）
    float dagGroupError    = 0.0f;    // 当前下钻组的组误差（UE:1593 组统一携带）
    // ---- M4.2：cull pass GPU 计时（第二个 timer 实例只包 compute 剔除段）----
    float cullMs = 0.0f;             // 最近一次 resolve 的剔除 pass GPU 耗时（ms），异步有延迟
    float cullMsHistory[120] = {};   // cull 耗时曲线样本（viewer 维护）
    int   cullMsCount = 0;
    bool  cullTimingAvailable = false; // cull 计时是否可用（仅 cull 模式 + HUD 时置位）
    // ---- M4.1：GPU 视锥剔除统计（只读显示，viewer 从 DeviceInfo 填入）----
    int   visibleClusters  = -1;      // GPU 可见簇数（两帧前结果；-1 = 暂无数据）
    int   totalClusters    = 0;       // 总簇数
    // ---- M5.2：GPU 可见三角数（与 visibleClusters 同一延迟窗口；-1 = 暂无数据）----
    int   visibleTriangles = -1;
    // ---- M8.0（S8.0）：BLAS 预建（Lumen 光追地基，init 一次性构建）----
    // blasTriangleCount>0 = 已建（dag/hiz 模式 + v3 lod0 资产）；0 = 未建不显示。
    // blasMs 为 GPU timer resolve 的构建耗时（恒定值）；deterministic 不写真实值，
    // HUD 只显示 tri 数保截图基线稳定。
    int   blasTriangleCount   = 0;
    float blasMs              = 0.0f;
    bool  blasTimingAvailable = false;
    // ---- M8.1（S8.1）：TLAS 每帧全量 rebuild（UE 口径，D-S8-1 拍板）----
    // tlasInstanceCount>0 = TLAS 已建并每帧 rebuild（跟 BLAS 同模式门控）；
    // tlasMs 为最近一帧 rebuild 的 GPU 耗时（每帧刷新）；deterministic 不写真实
    // 值，HUD 只显示实例数保截图基线稳定。
    int   tlasInstanceCount   = 0;
    float tlasMs              = 0.0f;
    bool  tlasTimingAvailable = false;
    // ---- M13.0（S13）：per-cluster BLAS 预建证据（--cluster-blas 才非零，其它
    // 模式一像素不动）。inputMB = build-input slab 实际字节（几何决定量，确定值）；
    // estMB = AS 上界估算（NVRHI 不暴露真实尺寸，量级参考）；ms 与 BLAS 行同口径
    // deterministic 不写真实值。
    int   clusterBlasCount    = 0;
    int   clusterBlasTriangles = 0;
    double clusterBlasInputMB = 0.0;
    double clusterBlasEstMB   = 0.0;
    float clusterBlasMs       = 0.0f;
    bool  clusterBlasTimingAvailable = false;
    // ---- M-PT0：粒子证据行（仅 particles 模式显示，其它模式一像素不动）。
    bool  particlesEnabled    = false;
    int   particleAlive       = 0;

    // ---- M10.0：探针 cascade 参数面板（仅 probe-place 模式且 HUD 开时画；
    // 双向通道同 hizEnable：viewer 传当前值，滑条写回，viewer 回填 FrameParams）。
    bool  probePanelShow      = false;
    int   probeBaseRes        = 16;     // 双向（4..32）
    float probeNormalOffset   = 0.10f;  // 双向（× 探针世界间距）
    float probeDotSize        = 3.0f;   // 双向（px）
    bool  probeGatherMode     = false;  // M10.2：viz 三态切换（Q13-3：false=点色
                                        // 级别/辐照度采样，true=final gather 全屏
                                        // 合成；双向 checkbox，gather 优先）
    // ---- M10.3：probe 调参（Q14-3 全调参面）+ --sky-bg（Q14-4；双向 checkbox）。
    float probeLevels[3]      = {3.0f, 5.0f, 7.0f}; // cascade 阈值滑条 ×3（双向）
    bool  probeSkyBg          = false;  // 天穹背景切换（双向 checkbox）

    // ---- S12 M12.0：filter 时域面板（默认 false = 既有 --hud 基线零布局变化）。
    // 只在 --mode filter 时 show=true，其余模式一行都不画。
    bool  filterPanelShow     = false;
    bool  temporalEnable      = true;
    float temporalAlpha       = 0.1f;
    int   temporalHistoryMax  = 32;
    // ---- S12 M12.2：三层独立开关 + 拒绝阈值，参数面板定型（默认全关 = 既有卡零漂）。
    bool  filterSpatial       = false;
    bool  filterReject        = false;
    float rejectThreshold     = 0.01f;

    // ---- M-PERF1：per-pass GPU 计时面板（Q-P5；--pass-timing 才 show，默认关 =
    // 既有 --hud 基线零布局变化）。行名固定在 renderer 侧 PassTimers::name(i)
    //（gpu_timer.hpp，13 项），POD 只传数值；槽位序 = PassId 枚举序。
    bool  passTimingShow      = false;
    float passTimingMs[13]    = {};
    bool  passTimingValid[13] = {};

    // ---- M5.0（S5.0）：屏幕误差投影数据（世界空间；model 恒为单位阵）----
    float screenErrorPx    = 0.0f;    // viewer 算好的屏幕像素误差（只读显示）
    bool  screenErrorValid = false;   // 球数据可用（dag-top 模式 + .nmesh 有球）
    float cutThresholdPx   = 1.0f;    // cut 阈值 px（双向；M5.1 GPU 遍历消费）
    // ---- M6.0：Hi-Z 金字塔（仅 hiz 模式非零/有效；其它模式不画对应控件）----
    // ---- M6.1：top 语义修正 = 全屏最远表面径向距离（金字塔 max 口径，见
    //      hiz.slang 文件头；字段名 hizTopMin→hizTopFar 同步改名）----
    int   hizMipCount      = 0;       // 金字塔 mip 层数（0 = 非 hiz 模式）
    int   hizViewLevel     = 0;       // 可视化查看的 mip 层（双向，viewer 传 FrameParams）
    float hizTopFar        = -1.0f;   // 最高 mip = 全屏最远表面径向距离（两帧延迟；<0 无数据）
    // ---- M6.2：Hi-Z 开关 + 剔除前/后簇数对照（仅 hiz 模式消费）----
    bool  hizEnable        = true;    // 开关（双向：HUD checkbox 写回，viewer 传 FrameParams）
    int   hizOccluded      = -1;      // 被 Hi-Z 剔除的簇节点数（两帧延迟；-1 无数据）
                                  // 剔除前访问数 = visibleClusters + hizOccluded（整树
                                  // 语义：被遮父簇的子树未访问不计入）
    // ---- T1：材质图编辑器（viewer 创建并持有；null = 不画编辑器窗口，
    // 既有模式基线一像素不动。ImGui/node-editor 符号封在 renderer/src/editor.cpp）----
    class NodeEditor* editor = nullptr;
    // ---- P8 M-N2：NavMesh 调参/五阶段面板（--nav-panel 时 viewer 挂入；
    // null = 不画。POD 状态与画布见 nav_panel.hpp，ImGui 封在 nav_panel.cpp）----
    class NavPanel* navPanel = nullptr;
    NavPanel::State navPanelState;
    // ---- P8 M-I2：input-hud 调试面板（--mode input-hud 时 viewer 置 show；
    // false = 零布局，既有基线不动。POD 定义与绘制接口见 input_debug.hpp，
    // ImGui 符号封在 renderer/src/input_debug.cpp）----
    InputDebugUi inputDebug;
    DriveDebugUi driveDebug;
    ProfilingDebugUi profilingDebug;   // M-TR0：默认关的独立性能统计窗口
    NetStatusUi netStatus;             // NET M-NET2：默认关的独立网络状态窗口
    VsmDebugUi vsmDebug;               // M15.1：默认关的 VSM 页池占用窗口（--vsm-panel）
};

// M-E1b.0：ECS 场景描述 POD（viewer 从 World 组件收集，renderer 转发 backend）。
// 公开头只放 POD；meshFile 指向调用方稳定存储（viewer 侧字符串数组）。
struct SceneObjectDesc {
    float world[16];        // 列主序世界矩阵（TransformComponent::getWorldMatrix()，m[col*4+row]）
    float tint[3];          // ModelComponent::tint
    float roughness = 1.0f; // M-E1b.3：逐实例 PBR 粗糙度（缺省 1=完全粗糙）
    float metallic  = 0.0f; // M-E1b.3：逐实例 PBR 金属度（缺省 0=介电质）
    // P9 V2：场景 JSON 显式写了 roughness/metallic = true（覆盖优先）；否则
    // 实例打包用 batch 级 GLB 因子做缺省（与 [22..27] 的 GLB 权威口径对齐）。
    bool hasPbrOverride = false;
    const char* meshFile;   // assets/ 下的 .glb 文件名（viewer 侧已过白名单校验）
};
struct SceneLightDesc {
    float dir[3];           // 归一化后的光照传播方向（世界空间）
    float color[3];
    float intensity;
    // M-E1b.2 尾部追加（POD；viewer 的 `SceneLightDesc l{}` 仍按默认值零初始化）
    int   type        = 0;                    // 0=Directional / 1=Point
    float position[3] = {0.0f, 0.0f, 0.0f}; // 点光世界位置
    float range       = 12.0f;               // 衰减截断半径
    // M-E1b.4：方向灯投阴影（仅 type=0 生效；scene 侧最多取前 kMaxShadowLights 盏）
    bool  shadow      = false;
};

// 设备能力与当前后端调试层来源，验收时打印出来做机械对照。
struct DeviceInfo {
    std::string adapterName;
    std::string debugLayer;
    bool        debugLayerActive = false;
    bool        rayQuery = false;
    bool        shaderInt64Atomics = false;
    bool        indirectDraw = false;
    // M0.7：--model 加载的模型文件名与三角形数（HUD 截图自证加载了哪个模型）。
    std::string modelName;
    std::uint32_t triangleCount = 0;
    // M2.3：分层资产探测结果（assets/meshlets/<stem>.lodN.nmesh，N 从 0 连续）。
    // 0 = 没有分层文件（cluster / normal 模式恒 0）。
    int   lodLevelCount = 0;
    int   lodLevel = 0;
    float lodMaxError = 0.0f;
    // M3.2：dag-top 状态。dagGroupCount = 可下钻组数（链第二粗层的 dagGroups）。
    int   dagGroupCount = 0;
    int   dagGroup = -1;              // -1 = 纯顶层
    float dagGroupError = 0.0f;
    // M4.1：GPU 视锥剔除统计（两帧前提交的结果，延迟窗口与 GPU timer 同思路）。
    // visibleClusters = -1 表示暂无数据（前两帧 / 非剔除路径 / map 失败）。
    int           visibleClusters = -1;
    std::uint32_t totalClusters = 0;
    // M5.2：GPU 可见三角数（cull / dag compute 原子累加，同帧回读环形高 4 字节）。
    int           visibleTriangles = -1;
    // M5.0（S5.0）：屏幕误差投影的球数据源（世界空间，xyz=center, w=radius）。
    // assetSphere = 当前显示几何的联合包围球；dagGroupSphere = 下钻组记录球
    //（dagGroup>=0 时有效，来自 .nmesh meta 的组 sphere）。纯 normal 模式恒 0。
    float assetSphere[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
    float dagGroupSphere[4]  = {0.0f, 0.0f, 0.0f, 0.0f};

    // M-A5.4：skin 模式 t=0 蒙皮输出半径（最大顶点到中心距离；非 skin 模式 0）。
    // viewer 据此对未显式 --dist 的 skin 模式做资产尺度自适应取景。
    float skinRadius = 0.0f;

    // M6.0：Hi-Z 金字塔状态（仅 hiz 模式非零；DeviceInfo 由 backend 每帧 poll 更新）。
    // M6.1：top 语义 = 全屏最远表面径向距离（金字塔 max 口径）。
    int   hizMipCount = 0;       // 金字塔 mip 层数（0 = 非 hiz 模式）
    float hizTopFar   = -1.0f;   // 最高 mip 全屏最远表面径向距离（两帧延迟；<0 = 暂无数据）
    // M6.2：被 Hi-Z 剔除的簇节点数（两帧延迟；-1 无数据，非 hiz 模式恒 -1）。
    int   hizOccluded = -1;
    // M6.2：最近一次 resolve 的整帧 GPU 时间 ms（真实值，deterministic 模式也
    // 有效——只是不写回 HUD 保基线 MD5；0 = timer 不可用 / 尚无数据）。
    float lastGpuMs = 0.0f;
    // M-E1b.0：ECS 场景实例数（--scene 模式非零；backend 每帧 poll 时从 scene_ 拷）。
    std::uint32_t instanceCount = 0;
    // M-TR0：Scene::draw 主路径本帧提交的图形 draw call 数（含 initCluster 单批
    // 兜底；不含 compute dispatch 与 ImGui UI，口径见 P8-TRACY-PLAN §2.5 K1）。
    std::uint32_t drawCalls = 0;
    // M-PT0：粒子叠加层活粒子数（backend 每帧 poll 时从 scene_ 拷；--particles
    // 关闭时恒 0——viewer 据此给 HUD 喂数与打稳态日志锚点）。
    int particleAlive = 0;
};

class Renderer {
public:
    // nativeWindow：Windows 下是 HWND（core::Window::nativeHandle()）。
    // modelFile：normal 模式下是 assets/models/ 下的 .glb 文件名（--model 传入；
    // 缺省 Suzanne.glb，历史基线不变）；cluster 模式（clusterMode=true）下是
    // assets/meshlets/ 下的 .nmesh 文件名（M1.3）；cull 模式（cullMode=true，M4.0
    // GPU vertex pulling）下同样是 .nmesh 文件名。renderer 侧做白名单校验。
    // 总是返回对象；创建失败时 valid()==false，原因在 lastError()。
    static std::unique_ptr<Renderer> create(void* nativeWindow, Extent extent,
                                            bool enableDebugLayer,
                                            const char* modelFile = "Suzanne.glb",
                                            bool clusterMode = false,
                                            bool cullMode = false,
                                            bool frustumCull = true,
                                            bool dagMode = false,
                                            bool skinMode = false,
                                            bool hizMode = false,
                                            bool sceneMode = false,
                                            // M7.0：visbuf 模式吃 .nmesh，走独立
                                            // 的 u64 vis buffer 三段式 pass。
                                            bool visbufMode = false,
                                            // M7.1：在 visbuf 之上叠加材质反解
                                            // pass（pulling），复用 vis 资源 + PBR 贴图。
                                            bool pullingMode = false,
                                            // M8.2：Ray Query 调试射线闭环
                                            //（--mode rt；走 initDag 资产路径拿
                                            // BLAS/TLAS，渲染只做 compute 射线 + 呈现）。
                                            bool rtMode = false,
                                            // M9.0：--mode shadow（compute 直光对照；
                                            // 走 initVisbuf(pulling) 资产链 + 共用 rtTarget_）。
                                            bool shadowMode = false,
                                            // M9.2：--mode reference（离线参考模式；
                                            // 走 initVisbuf(pulling) 资产链 + 共用 rtTarget_）。
                                            bool referenceMode = false,
                                            // M10.0：--mode probe-place（屏幕探针 cascade
                                            // 布点；走 initVisbuf(pulling) 资产链 +
                                            // velocity prepass + 布点 compute + 点可视化）。
                                            bool probeMode = false,
                                            // M13.0：--cluster-blas（per-cluster BLAS
                                            // 预建；S13-PLAN M13.0，缺省关 = 既有
                                            // 模式零扰动）。
                                            bool clusterBlas = false);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    bool valid() const;
    const std::string& lastError() const;
    const DeviceInfo& info() const;
    Extent extent() const;

    void resize(Extent extent);

    // 画一帧：清屏 + 模型绘制（MVP / 剔除开关来自 FrameParams）+（可选）ImGui HUD 叠加
    // + 可选同帧回读写 PNG + present。hud 为空时完全不碰 ImGui，历史版本的截图基线保持不变。
    // screenshotPath 非空时用 NVRHI staging texture 真回读 GPU 像素，不是 CPU 画个假图。
    bool renderFrame(ClearColor color, const FrameParams& frame,
                     const char* screenshotPath = nullptr, HudFrame* hud = nullptr);

    // 把当前后端调试层里积累的消息转成日志并返回新增的设备错误数（验收要求为 0）。
    int drainMessages();

    // M2.3：切到第 level 层（0 = 最细原始层）。只对 lod 场景有效：
    // 读 assets/meshlets/<stem>.lod<level>.nmesh，重建 VB/IB 后一次性上传，
    // pipeline / binding set 不动。越界或文件缺失返回 false（lastError() 带可用层数），
    // 失败时保持原层不变（不会把画面留在半加载状态）。
    // 分层文件由 `nanite-tools lod <base>.nmesh -o <dir>/<stem>` 产出。
    bool setLodLevel(int level);
    int  lodLevel() const;      // 当前层；无分层资产时 0
    int  lodLevelCount() const; // 探测到的层数；0 = 无分层资产

    // M3.2：dag-top 组原子下钻（S5 运行时 cut 的离线预演）。group = 链第二粗层的
    // 组号：把顶层（最粗层）中该组生成的父簇挖掉、换回组内子簇，渲染混合集合。
    // group < 0 = 回到纯顶层。组号越界 / 无血缘返回 false 且保持原画面。
    // 父子衔接必须无洞无重叠——机制保证是 M2.2 的组间边界锁点（粗细两版
    // 边界顶点逐位一致）；验收 = dag-top 模式截图肉眼核对。
    bool setDagCut(int group);
    int  dagGroupCount() const; // 可下钻组数；0 = 无 DAG 血缘或层数不足

    // M-E1b.0：ECS 场景加载（--scene 模式；sceneMode=true 的 Renderer 专用）。
    // objects/lights 由 viewer 从 World 组件收集（O(N)），本方法转发 backend 的
    // scene_.initScene：建实例缓冲/灯缓冲/逐 mesh 批次/绑定集，成功后 info() 回拷
    // （modelName=sceneName、triangleCount、instanceCount、assetSphere——照
    // setLodLevel 的回拷写法）。失败返回 false（lastError() 带原因），场景保持空。
    bool loadEcsScene(const SceneObjectDesc* objects, std::uint32_t objectCount,
                      const SceneLightDesc* lights, std::uint32_t lightCount,
                      float ambient, const char* sceneName);

    // M-E1b.5 渲染用例：每帧刷新实例数据（世界矩阵/tint/材质），灯与拓扑不动。
    // objectCount 必须与 loadEcsScene 一致（batch 槽号一一对应）。viewer 在每帧
    // renderFrame 之前调用：World tick（serial/parallel）→ 收集矩阵 → 这里上传。
    bool updateEcsObjects(const SceneObjectDesc* objects, std::uint32_t objectCount);

    // M-PT1：.nfx v1 加载端（viewer --nfx <path>）。转发 backend 的
    // scene_.loadParticlesNfx：解析 NFX1 JSON → 折进粒子模拟参数面。运行时
    // 零 Effekseer 依赖。失败返回 false（lastError() 带原因），legacy 喷泉不受影响。
    bool loadParticlesNfx(const char* path);

    // M10.3：--compare <png> 参考图加载（Q14-2 拍板：画面左半 live 右半参考图同屏）。
    // stb_image 解码 → NVRHI RGBA8 纹理；加载成功后每帧 renderFrame 在右半屏
    // 贴参考图（present 层 quad，HUD 之下）。失败返回 false（lastError() 带原因），
    // 画面保持全 live 不受影响。
    bool loadCompareImage(const char* path);

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::renderer
