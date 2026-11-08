#pragma once

// M0.2：renderer 是唯一允许出现图形 API / NVRHI 符号的模块。
// 公开接口只暴露 POD 与 std 类型：viewer/ 与 core/ 不接触任何后端类型。
// 后端由顶层 CMake 的 NL_GRAPHICS_BACKEND 在编译期选定，切换后端不需要装任何厂商 SDK。

#include <cstdint>
#include <memory>
#include <string>

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
    float options[4]  = {};
    bool  cullBack    = true;   // false = 关背面剔除，能看到内壁
    // M0.8 调试着色 / 线框：缺省值 = M0.7 行为（textured + solid），历史基线不受影响。
    int   shading     = 0;      // 0 textured / 1 normal / 2 uv / 3 tri-id（传进 g_options.x）
    bool  wireframe   = false;  // 光栅化 fill mode = wireframe（深度测试照常）
};

// M0.5 HUD：viewer -> HUD 的显示数据 + HUD -> viewer 的控件值，双向 POD。
// 公开头只放 POD，viewer / core 不接触 ImGui 或任何图形 API 类型。
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
    // ---- M5.0（S5.0）：屏幕误差投影数据（世界空间；model 恒为单位阵）----
    float screenErrorPx    = 0.0f;    // viewer 算好的屏幕像素误差（只读显示）
    bool  screenErrorValid = false;   // 球数据可用（dag-top 模式 + .nmesh 有球）
    float cutThresholdPx   = 1.0f;    // cut 阈值 px（双向；M5.1 GPU 遍历消费）
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
    // M5.0（S5.0）：屏幕误差投影的球数据源（世界空间，xyz=center, w=radius）。
    // assetSphere = 当前显示几何的联合包围球；dagGroupSphere = 下钻组记录球
    //（dagGroup>=0 时有效，来自 .nmesh meta 的组 sphere）。纯 normal 模式恒 0。
    float assetSphere[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
    float dagGroupSphere[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
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
                                            bool frustumCull = true);
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

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::renderer
