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

    // 画一帧：清屏 + 模型绘制（MVP / 剔除开关来自 FrameParams）+ 可选同帧回读写 PNG + present。
    // screenshotPath 非空时用 NVRHI staging texture 真回读 GPU 像素，不是 CPU 画个假图。
    bool renderFrame(ClearColor color, const FrameParams& frame,
                     const char* screenshotPath = nullptr);

    // 把当前后端调试层里积累的消息转成日志并返回新增的设备错误数（验收要求为 0）。
    int drainMessages();

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nl::renderer
