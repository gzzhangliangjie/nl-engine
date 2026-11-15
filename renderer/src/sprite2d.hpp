#pragma once

// M-SP0/M-SP1 Sprite2DPass — world-space 2D sprite batch (renderer side, the
// ONLY place where NVRHI symbols appear for the 2D cluster).
//
// Frozen split (user decision 2026-11-15 option A):
//   core/    = POD atlas / flipbook / sprite_desc + CPU binning (zero graphics)
//   renderer/= this pass + sprite2d.slang (one source, DXIL + SPIR-V)
//
// Shape follows M-PT0 Particles (dual-backend proven): NO vertex buffer, NO
// input layout, NO index buffer. VS expands 6 vertices from SV_VertexID and
// reads a StructuredBuffer via SV_InstanceID.
//
// M-SP1 scope (user decision 2026-11-15: all four sub-items, kStage not
// bumped): per-bucket binding-set draws + multi-atlas registration.
//   - Bucketing stays CPU-side (nl/sprite/sprite_desc.hpp, untouched).
//   - draw() loops buckets; each bucket binds its atlas' BindingSet and draws
//     only its own instance range. Cross-backend instance-index hazard (M-SP0
//     note below): DrawArguments::setStartInstanceLocation is NOT used --
//     SV_InstanceID excludes firstInstance on DX12 while Vulkan's
//     gl_InstanceIndex includes it. Instead each bucket writes the volatile CB
//     with g_baseIndex = bucket.begin, and the VS indexes
//     g_sprites[iid + g_baseIndex]. SV_InstanceID starts at 0 per draw on BOTH
//     backends, so one shader stays byte-consistent.
//   - Atlas registration: Sprite2DFrame::atlases (id -> RGBA pixels). First
//     frame creates texture (CopyDest) + BindingSet; pending uploads happen
//     inside the frame command list in draw() (builtin-atlas precedent; no
//     second cmd list mid-frame, §2.8). Pixels are premultiplied on ingest so
//     the RmlUi bridge (M-SP1-d) can share the same texture under RmlUi's
//     premultiplied-alpha pipeline. Sprite tint is premultiplied in build()
//     and the blend state is One/InvSrcAlpha (RmlUi-compatible).
//   - textureId 0 / unregistered ids / kInvalidTextureId all fall back to the
//     builtin 4x4 atlas.
//
// Layout discipline: SpriteGpu must stay byte-identical to
// renderer/src/sprite2d.slang :: SpriteGpu (static_asserts both sides).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "nl/sprite/sprite_desc.hpp"
#include "nl/renderer/renderer.hpp"   // Sprite2DAtlasRequest

namespace nl::render {

struct SpriteGpu {
    float posSize[4];  // xy = world pos, zw = size
    float uvRect[4];   // u0,v0,u1,v1
    float color[4];    // PREMULTIPLIED RGBA (M-SP1: tint premultiplied in build())
    float misc[4];     // x = rotation, y/z = origin, w = layer
};
static_assert(sizeof(SpriteGpu) == 64, "SpriteGpu must be 64B and match sprite2d.slang");

struct Sprite2DConstants {
    float projection[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // xy scale, zw offset
    std::uint32_t baseIndex = 0;      // M-SP1: per-bucket offset into g_sprites
    std::uint32_t pad[3] = { 0, 0, 0 };
};
static_assert(sizeof(Sprite2DConstants) == 32, "Sprite2D cbuffer must be 32B");

class Sprite2DPass {
public:
    static constexpr std::uint32_t kMaxSprites = 4096;
    // Volatile CB version budget: one writeBuffer per bucket per frame. Real
    // batches have (atlas count x layers) buckets, far below this; the guard
    // in draw() merges the tail buckets if a pathological batch exceeds it.
    static constexpr std::uint32_t kMaxBucketsPerFrame = 64;

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
    // draw()). Converts SpriteDesc to SpriteGpu, computes stable buckets, and
    // ingests this frame's atlas registrations (create texture + binding set
    // lazily; pixels stay pending until draw() uploads them in-frame).
    void build(const nl::sprite::SpriteDesc* sprites, std::size_t count,
               const nl::renderer::Sprite2DAtlasRequest* atlases, std::uint32_t atlasCount,
               float displayWidth, float displayHeight,
               float cameraX, float cameraY, float pixelsPerUnit);

    // GPU side: uploads pending atlases, writes CB + instance data, then one
    // draw per bucket with the bucket's atlas binding set.
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport);

    std::uint32_t bucketCount() const { return static_cast<std::uint32_t>(buckets_.size()); }
    std::uint32_t spriteCount() const { return static_cast<std::uint32_t>(gpu_.size()); }
    std::uint32_t atlasCount() const { return static_cast<std::uint32_t>(atlases_.size()); }

    // M-SP1-d RmlUi bridge: texture handle for a registered id (builtin atlas
    // when unregistered). Premultiplied contents -- safe to hand to RmlUi's
    // premultiplied pipeline. Returns nullptr only before init().
    nvrhi::ITexture* atlasTexture(std::uint32_t textureId);

private:
    struct AtlasEntry {
        nvrhi::TextureHandle tex;
        nvrhi::BindingSetHandle set;
        std::vector<std::uint8_t> pending;   // premultiplied; cleared after upload
        std::uint32_t width = 0, height = 0;
        bool pendingUpload = false;
    };

    bool createBuiltinAtlas(std::string& outError);
    AtlasEntry* atlasOf(std::uint32_t textureId);   // miss -> builtin entry
    bool ingestAtlas(const nl::renderer::Sprite2DAtlasRequest& req, std::string& outError);

    nvrhi::IDevice* device_ = nullptr;
    nvrhi::ShaderHandle        vs_, ps_;
    nvrhi::BufferHandle        constants_, instanceData_;
    nvrhi::SamplerHandle       sampler_;
    nvrhi::BindingLayoutHandle bindingLayout_;
    nvrhi::GraphicsPipelineHandle pipeline_;

    // textureId 0 = builtin 4x4 atlas (created in init, never replaced).
    std::unordered_map<std::uint32_t, AtlasEntry> atlases_;

    std::vector<SpriteGpu>              gpu_;
    std::vector<nl::sprite::SpriteBucket> buckets_;
    Sprite2DConstants                   constantsData_{};
    bool buildOk_ = false;
    bool warnedBucketFlood_ = false;
};

} // namespace nl::render
