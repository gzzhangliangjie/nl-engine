#pragma once

// renderer 内部接口：两个后端各实现一份，renderer.cpp 只认这个抽象。
#include "nl/renderer/renderer.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace nl::renderer::detail {

class Backend {
public:
    virtual ~Backend() = default;

    virtual bool init(void* nativeWindow, Extent extent, bool enableDebugLayer,
                      std::string& outError) = 0;
    virtual void resize(Extent extent) = 0;

    // 一帧：clear → （可选）回读 GPU 像素写 PNG → present
    virtual bool renderFrame(ClearColor color, const char* screenshotPath,
                             std::string& outError) = 0;

    // 调试层消息转日志；返回新增设备错误数
    virtual int  drainMessages() = 0;

    virtual const DeviceInfo& info() const = 0;
    virtual Extent extent() const = 0;
};

// backend_dx12.cpp / backend_vulkan.cpp 各自按编译期宏提供唯一实现。
std::unique_ptr<Backend> createBackend();

// pixels 是 RGBA8，rowPitch 是每行字节数（可能大于 width*4）。
bool writePngRGBA8(const char* path, const void* pixels,
                   std::uint32_t width, std::uint32_t height,
                   std::uint32_t rowPitch);

} // namespace nl::renderer::detail
