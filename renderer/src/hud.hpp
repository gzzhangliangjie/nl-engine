#pragma once

// M0.5 共享 ImGui HUD 渲染后端：两个后端都只调用这一份代码（与 Scene 同构）。
// 本文件只依赖 NVRHI 公共头；ImGui 头只在 hud.cpp 里 include。
// viewer / core 只通过 nl::renderer::HudFrame 这个 POD 与 HUD 交换数据。

#include "nl/renderer/renderer.hpp"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nl::renderer::detail {

// CPU 侧把 ImDrawVert 展开成 8 个 float（避免 R8G8B8A8_UNORM 顶点格式在两后端的差异）。
// 必须与 hud.slang 的 VSInput 顺序一致：POSITION / TEXCOORD / COLOR。
struct HudVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};
static_assert(sizeof(HudVertex) == 32, "HudVertex must be tightly packed");

// hud.slang 的 HudConstants：float4 g_projection（16 B）。
struct HudConstants {
    float projection[4];
};
static_assert(sizeof(HudConstants) == 16, "HUD cbuffer layout must match shader");

// 屏幕像素 -> NDC：xy = 2/displaySize，zw = -1 - displayPos*xy。纯算术，便于自检。
HudConstants hudProjection(float displayWidth, float displayHeight,
                           float displayPosX = 0.0f, float displayPosY = 0.0f);

class Hud {
public:
    // shaderSuffix = "dxil" / "spv"；vsEntry/psEntry 按后端传（SPIR-V 恒为 main）。
    bool init(nvrhi::IDevice* device, const char* shaderSuffix,
              const char* vsEntry, const char* psEntry, std::string& outError);

    // 句柄必须在 device 还活着时释放（两个后端的 shutdown 里调用）。
    void shutdown();

    // 用 framebuffer 的格式信息建 pipeline，resize 后由 invalidatePipeline 触发重建。
    bool ensurePipeline(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                        std::string& outError);
    void invalidatePipeline();

    bool ready() const {
        return pipeline_ && bindingSet_ && fontTexture_ && vertexBuffer_ &&
               indexBuffer_ && constants_;
    }

    // CPU 侧：ImGui NewFrame -> 组面板 -> Render，把控件值写回 frame，
    // 并把 draw data 摊平成顶点 / 索引 / draw cmd 列表，供 draw() 提交。
    void build(HudFrame& frame);

    // GPU 侧：在 scene.draw() 之后、截图回读之前，把 HUD 叠到当前 framebuffer。
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport);

private:
    struct DrawCmd {
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // scissor（已 clamp 到 viewport）
    };

    bool ensureCapacity(std::size_t vertexCount, std::size_t indexCount,
                        std::string& outError);

    nvrhi::IDevice* device_ = nullptr;

    nvrhi::ShaderHandle           vs_, ps_;
    nvrhi::TextureHandle          fontTexture_;
    nvrhi::SamplerHandle          sampler_;
    nvrhi::BufferHandle           constants_, vertexBuffer_, indexBuffer_;
    nvrhi::InputLayoutHandle      inputLayout_;
    nvrhi::BindingLayoutHandle    bindingLayout_;
    nvrhi::BindingSetHandle       bindingSet_;
    nvrhi::GraphicsPipelineHandle pipeline_;

    std::size_t vertexCapacity_ = 0;
    std::size_t indexCapacity_  = 0;
    std::vector<HudVertex>      vertices_;
    std::vector<std::uint32_t>  indices_;
    std::vector<DrawCmd>        commands_;
    HudConstants                projection_{};
    bool buildOk_ = false;
};

} // namespace nl::renderer::detail
