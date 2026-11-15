#pragma once

// M-SP0 Sprite2DPass — world-space 2D sprite batch (renderer side, the ONLY
// place where NVRHI symbols appear for the 2D cluster).
//
// Frozen split (user decision 2026-11-15 option A):
//   core/    = POD atlas / flipbook / sprite_desc + CPU binning (zero graphics)
//   renderer/= this pass + sprite2d.slang (one source, DXIL + SPIR-V)
//
// Shape follows M-PT0 Particles (dual-backend proven): NO vertex buffer, NO
// input layout, NO index buffer. VS expands 6 vertices from SV_VertexID and
// reads a StructuredBuffer via SV_InstanceID.
//
// M-SP0 scope note: CPU-side texture-keyed buckets are computed and exposed
// (bucketCount()) for verification, but the GPU submits ONE draw for the whole
// batch. Reason: DrawArguments::setStartInstanceLocation() has no precedent in
// this engine and SV_InstanceID does not include firstInstance on DX12 while
// Vulkan's gl_InstanceIndex does -- a silent cross-backend divergence. Per-bucket
// draws with per-bucket binding sets belong to M-SP1 (atlas registration),
// where a real GPU demo can validate it. M-SP0 takes the proven particles path.

#include <cstdint>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "nl/sprite/sprite_desc.hpp"

namespace nl::render {

struct SpriteGpu {
    float posSize[4];  // xy = world pos, zw = size
    float uvRect[4];   // u0,v0,u1,v1
    float color[4];    // straight RGBA
    float misc[4];     // x = rotation, y/z = origin, w = layer
};
static_assert(sizeof(SpriteGpu) == 64, "SpriteGpu must be 64B and match sprite2d.slang");

struct Sprite2DConstants {
    float projection[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // xy scale, zw offset
};
static_assert(sizeof(Sprite2DConstants) == 16, "Sprite2D cbuffer must be 16B");

class Sprite2DPass {
public:
    static constexpr std::uint32_t kMaxSprites = 4096;

    // shaderSuffix = "dxil" (D3D12) / "spv" (Vulkan). Entry names are always
    // "main": SPIR-V always uses main and NVRHI ignores entryName for DXIL
    // (particles.slang precedent, SHADER-CONVENTIONS §2.4).
    bool init(nvrhi::IDevice* device, const char* shaderSuffix, std::string& outError);
    void shutdown();
    bool ensurePipeline(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                        std::string& outError);
    void invalidatePipeline();
    bool ready() const;

    // CPU side, called in renderFrame BEFORE cmd->open() (or at least before
    // draw()). Converts SpriteDesc to SpriteGpu and computes stable buckets.
    void build(const nl::sprite::SpriteDesc* sprites, std::size_t count,
               float displayWidth, float displayHeight,
               float cameraX, float cameraY, float pixelsPerUnit);

    // GPU side: writes CB + instance data, one draw for the whole batch.
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport);

    std::uint32_t bucketCount() const { return static_cast<std::uint32_t>(buckets_.size()); }
    std::uint32_t spriteCount() const { return static_cast<std::uint32_t>(gpu_.size()); }

private:
    bool createBuiltinAtlas(std::string& outError);

    nvrhi::IDevice* device_ = nullptr;
    nvrhi::ShaderHandle        vs_, ps_;
    nvrhi::BufferHandle        constants_, instanceData_;
    nvrhi::SamplerHandle       sampler_;
    nvrhi::TextureHandle       atlasTex_;
    nvrhi::BindingLayoutHandle bindingLayout_;
    nvrhi::BindingSetHandle    bindingSet_;
    nvrhi::GraphicsPipelineHandle pipeline_;

    std::vector<SpriteGpu>              gpu_;
    std::vector<nl::sprite::SpriteBucket> buckets_;
    std::vector<std::uint8_t>           atlasPixels_;   // 首帧 draw 内上传
    std::uint32_t                       atlasWidth_ = 0;
    bool                                atlasPendingUpload_ = false;
    Sprite2DConstants                   constantsData_{};
    bool buildOk_ = false;
};

} // namespace nl::render
