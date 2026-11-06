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
};

// M0.5 HUD：viewer -> HUD 的显示数据 + HUD -> viewer 的控件值，双向 POD。
// 公开头只放 POD，viewer / core 不接触 ImGui 或任何图形 API 类型。
struct HudFrame {
    // ---- viewer -> HUD（显示数据）----
    float fps          = 0.0f;
    float frameMs      = 0.0f;
    float frameMsHistory[120] = {};   // 帧时间曲线样本
    int   frameMsCount = 0;
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
    // ---- 双向：viewer 传当前值，HUD 写回控件结果 ----
    bool cullBack          = true;
    int  colorIndex        = 0;
    bool resetCamera       = false;   // HUD 按钮置 true，viewer 消费后清零
    bool wantCaptureMouse  = false;   // HUD 输出：鼠标在面板上，viewer 别拖相机
};

// 设备能力与当前后端调试层来源，验收时打印出来做机械对照。
struct DeviceInfo {
    std::string adapterName;
    std::string debugLayer;
    bool        debugLayerActive = false;
    bool        rayQuery = false;
    bool        shaderInt64Atomics = false;
    bool        indirectDraw = false;
};

class Renderer {
public:
    // nativeWindow：Windows 下是 HWND（core::Window::nativeHandle()）。
    // 总是返回对象；创建失败时 valid()==false，原因在 lastError()。
    static std::unique_ptr<Renderer> create(void* nativeWindow, Extent extent, bool enableDebugLayer);
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

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::renderer
