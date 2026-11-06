#pragma once

// renderer 内部接口：两个后端各实现一份，renderer.cpp 只认这个抽象。
#include "nl/renderer/renderer.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace nl::renderer::detail {

// 读二进制文件到 out（用于加载 Slang 编译出的 SPIR-V / DXIL 字节码）。
// 失败返回 false（文件不存在 / 读不全），由调用方决定报错还是回退。
inline bool readFileBytes(const char* path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    const size_t rd = sz ? std::fread(out.data(), 1, static_cast<size_t>(sz), f) : 0;
    std::fclose(f);
    return rd == static_cast<size_t>(sz);
}

class Backend {
public:
    virtual ~Backend() = default;

    virtual bool init(void* nativeWindow, Extent extent, bool enableDebugLayer,
                      std::string& outError) = 0;
    virtual void resize(Extent extent) = 0;

    // 一帧：clear → 模型绘制（MVP / cull）→（可选）ImGui HUD 叠加
    // →（可选）回读 GPU 像素写 PNG → present。hud 为空时不碰 ImGui（历史基线不变）。
    virtual bool renderFrame(ClearColor color, const FrameParams& frame,
                             const char* screenshotPath, HudFrame* hud,
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
