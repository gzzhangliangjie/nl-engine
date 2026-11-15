// M-SP0/M-SP1 Sprite2DPass implementation.
// Resource lifecycle copied from hud.cpp (init/ensurePipeline/draw/shutdown)
// and Particles (StructuredBuffer + SV_VertexID), both dual-backend proven.
//
// M-SP1 changes (see sprite2d.hpp for the design rationale):
//   - draw(): one draw per bucket; each bucket writes the volatile CB with its
//     g_baseIndex and binds its atlas BindingSet. No setStartInstanceLocation
//     anywhere (cross-backend SV_InstanceID hazard, M-SP0 note).
//   - Multi-atlas registry keyed by textureId; textureId 0 = builtin atlas.
//   - Premultiplied alpha end-to-end (pixels on ingest, tint in build(),
//     blend One/InvSrcAlpha) so the RmlUi bridge can share the textures.

#include "sprite2d.hpp"
#include "backend.hpp"          // readFileBytes

#include <nl/core/log.hpp>
#include <nl/core/paths.hpp>

#include <cstring>
#include <utility>

namespace nl::render {
namespace {

// 4x4 builtin atlas: deterministic colored cells, so a future demo needs no
// external asset. Alpha is 255 everywhere, so premultiplication is a no-op.
void makeBuiltinAtlas(std::vector<std::uint8_t>* out, std::uint32_t* w,
                      std::uint32_t* h) {
    const std::uint32_t kCell = 16, kGrid = 4;
    *w = *h = kCell * kGrid;
    out->assign(static_cast<std::size_t>(*w) * (*h) * 4u, 0u);
    for (std::uint32_t y = 0; y < *h; ++y) {
        for (std::uint32_t x = 0; x < *w; ++x) {
            const std::uint32_t cx = x / kCell, cy = y / kCell;
            const std::uint32_t cell = cy * kGrid + cx;
            const std::size_t o = (static_cast<std::size_t>(y) * (*w) + x) * 4u;
            // Distinct hues per cell + a visible border so UV mistakes show up.
            const bool border = (x % kCell < 2) || (y % kCell < 2);
            const float v = border ? 0.15f : 1.0f;
            (*out)[o + 0] = static_cast<std::uint8_t>(((cell * 37u) % 256u) * v);
            (*out)[o + 1] = static_cast<std::uint8_t>(((cell * 91u + 40u) % 256u) * v);
            (*out)[o + 2] = static_cast<std::uint8_t>(((cell * 53u + 120u) % 256u) * v);
            (*out)[o + 3] = 255u;
        }
    }
}

// M-SP1: straight RGBA -> premultiplied RGBA, in place.
void premultiply(std::uint8_t* px, std::size_t pixelCount) {
    for (std::size_t i = 0; i < pixelCount; ++i) {
        std::uint8_t* p = px + i * 4;
        const std::uint32_t a = p[3];
        if (a == 255u) continue;                 // fast path: no-op
        if (a == 0u) { p[0] = p[1] = p[2] = 0u; continue; }
        p[0] = static_cast<std::uint8_t>((p[0] * a + 127u) / 255u);
        p[1] = static_cast<std::uint8_t>((p[1] * a + 127u) / 255u);
        p[2] = static_cast<std::uint8_t>((p[2] * a + 127u) / 255u);
    }
}

} // namespace

bool Sprite2DPass::ready() const {
    return pipeline_ && bindingLayout_ && constants_ && instanceData_ &&
           sampler_ && !atlases_.empty() && atlases_.at(0).tex && vs_ && ps_;
}

bool Sprite2DPass::init(nvrhi::IDevice* device, const char* shaderSuffix,
                        std::string& outError) {
    device_ = device;
    auto shaderPath = [shaderSuffix](const char* stage) {
        return nl::core::paths::shaderDir() + "/sprite2d." + stage + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    // readFileBytes lives in nl::renderer::detail (backend.hpp). This pass is
    // in nl::render, a sibling namespace -- an unqualified call does not
    // resolve there (C3861). HUD/Scene get away with the short name only
    // because their TUs are inside detail themselves.
    if (!nl::renderer::detail::readFileBytes(shaderPath("vs").c_str(), vsBlob) ||
        !nl::renderer::detail::readFileBytes(shaderPath("ps").c_str(), psBlob)) {
        outError = "sprite2d shader artifacts missing (" + shaderPath("vs") + ")";
        return false;
    }
    // SPIR-V always uses main; NVRHI ignores entryName for DXIL (particles
    // precedent, SHADER-CONVENTIONS §2.4). One name for both backends.
    nvrhi::ShaderDesc vsDesc;
    vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName("main");
    nvrhi::ShaderDesc psDesc;
    psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName("main");
    vs_ = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
    ps_ = device->createShader(psDesc, psBlob.data(), psBlob.size());
    if (!vs_ || !ps_) { outError = "createShader failed for sprite2d"; return false; }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed for sprite2d"; return false; }

    // volatile CB: 32 B, fully overwritten every frame (§2.7). M-SP1: one
    // writeBuffer per bucket per frame -> version budget = kMaxBucketsPerFrame.
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(Sprite2DConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = kMaxBucketsPerFrame;
    cbDesc.debugName = "Sprite2DConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for sprite2d constants"; return false; }

    nvrhi::BufferDesc instDesc;
    instDesc.byteSize = kMaxSprites * sizeof(SpriteGpu);
    instDesc.structStride = sizeof(SpriteGpu);
    instDesc.initialState = nvrhi::ResourceStates::CopyDest;
    instDesc.keepInitialState = true;
    instDesc.debugName = "Sprite2DInstances";
    instanceData_ = device->createBuffer(instDesc);
    if (!instanceData_) { outError = "createBuffer failed for sprite2d instances"; return false; }

    if (!createBuiltinAtlas(outError)) return false;

    // Slots must match sprite2d.slang: b0 CB / t0 StructuredBuffer / t1 atlas / s0 sampler.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0));
    bindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!bindingLayout_) { outError = "createBindingLayout failed for sprite2d"; return false; }
    return true;
}

bool Sprite2DPass::createBuiltinAtlas(std::string& outError) {
    std::vector<std::uint8_t> pixels;
    std::uint32_t w = 0, h = 0;
    makeBuiltinAtlas(&pixels, &w, &h);

    // Particles dummy-tex precedent (M-PT2): create with CopyDest, then upload
    // + explicit state promotion inside the frame command list on first draw.
    // No standalone command list here -- the DX12 backend keeps a persistent
    // frameCmd_, so executing an extra list mid-init is not safe (M8.1).
    nvrhi::TextureDesc td;
    td.width = w; td.height = h;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.initialState = nvrhi::ResourceStates::CopyDest;
    td.keepInitialState = true;
    td.debugName = "Sprite2DBuiltinAtlas";
    AtlasEntry e;
    e.tex = device_->createTexture(td);
    if (!e.tex) { outError = "createTexture failed for sprite2d atlas"; return false; }
    e.pending = std::move(pixels);
    e.width = w; e.height = h;
    e.pendingUpload = true;
    atlases_.emplace(0u, std::move(e));
    return true;
}

Sprite2DPass::AtlasEntry* Sprite2DPass::atlasOf(std::uint32_t textureId) {
    auto it = atlases_.find(textureId);
    if (it != atlases_.end()) return &it->second;
    // textureId 0 (builtin) is the universal fallback: unregistered ids and
    // kInvalidTextureId both render from the builtin atlas so a missing
    // registration degrades visibly instead of silently dropping the bucket.
    auto b = atlases_.find(0u);
    return b != atlases_.end() ? &b->second : nullptr;
}

bool Sprite2DPass::ingestAtlas(const nl::renderer::Sprite2DAtlasRequest& req, std::string& outError) {
    if (req.width == 0 || req.height == 0 || !req.rgba) {
        outError = "sprite2d atlas registration: bad size/pixels";
        return false;
    }
    auto it = atlases_.find(req.textureId);
    if (it == atlases_.end()) {
        // New texture: CopyDest + keepInitialState, upload promoted to
        // ShaderResource inside the frame list on the next draw (M8.1: no
        // second command list mid-frame).
        nvrhi::TextureDesc td;
        td.width = req.width; td.height = req.height;
        td.format = nvrhi::Format::RGBA8_UNORM;
        td.initialState = nvrhi::ResourceStates::CopyDest;
        td.keepInitialState = true;
        td.debugName = "Sprite2DAtlas";
        AtlasEntry e;
        e.tex = device_->createTexture(td);
        if (!e.tex) { outError = "createTexture failed for registered atlas"; return false; }
        e.width = req.width; e.height = req.height;
        e.pending.assign(req.rgba, req.rgba + std::size_t(req.width) * req.height * 4u);
        premultiply(e.pending.data(), std::size_t(req.width) * req.height);
        e.pendingUpload = true;
        atlases_.emplace(req.textureId, std::move(e));
        return true;
    }
    // Re-registration: replace contents. Same-size uploads reuse the texture;
    // a size change needs a new texture object (old handle drops with the entry).
    AtlasEntry& e = it->second;
    e.pending.assign(req.rgba, req.rgba + std::size_t(req.width) * req.height * 4u);
    premultiply(e.pending.data(), std::size_t(req.width) * req.height);
    e.pendingUpload = true;
    if (e.width != req.width || e.height != req.height) {
        nvrhi::TextureDesc td;
        td.width = req.width; td.height = req.height;
        td.format = nvrhi::Format::RGBA8_UNORM;
        td.initialState = nvrhi::ResourceStates::CopyDest;
        td.keepInitialState = true;
        td.debugName = "Sprite2DAtlasResized";
        nvrhi::TextureHandle tex = device_->createTexture(td);
        if (!tex) { outError = "createTexture failed for resized atlas"; return false; }
        e.tex = tex;
        e.set = nullptr;   // rebuilt below (texture changed)
        e.width = req.width; e.height = req.height;
    }
    if (!e.set) {
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
               .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instanceData_))
               .addItem(nvrhi::BindingSetItem::Texture_SRV(1, e.tex))
               .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_));
        e.set = device_->createBindingSet(setDesc, bindingLayout_);
        if (!e.set) { outError = "createBindingSet failed for registered atlas"; return false; }
    }
    return true;
}

bool Sprite2DPass::ensurePipeline(nvrhi::IDevice* device,
                                  const nvrhi::FramebufferInfo& info,
                                  std::string& outError) {
    if (pipeline_) return true;
    // No input layout: VS pulls from SV_VertexID / SV_InstanceID (particles path).
    nvrhi::GraphicsPipelineDesc desc;
    desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setVertexShader(vs_)
        .setFragmentShader(ps_)
        .addBindingLayout(bindingLayout_);
    desc.renderState.rasterState.setCullNone().setFrontCounterClockwise(true);
    desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
    auto& rt = desc.renderState.blendState.targets[0];
    // M-SP1: premultiplied alpha (One / InvSrcAlpha), matching the RmlUi
    // backend (rmlui_backend.cpp) so shared atlas textures render identically
    // under both pipelines. Tint premultiplication happens in build().
    rt.enableBlend()
      .setSrcBlend(nvrhi::BlendFactor::One)
      .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOp(nvrhi::BlendOp::Add)
      .setSrcBlendAlpha(nvrhi::BlendFactor::One)
      .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOpAlpha(nvrhi::BlendOp::Add);
    pipeline_ = device->createGraphicsPipeline(desc, info);
    if (!pipeline_) { outError = "createGraphicsPipeline failed for sprite2d"; return false; }
    return true;
}

void Sprite2DPass::invalidatePipeline() { pipeline_ = nullptr; }

void Sprite2DPass::build(const nl::sprite::SpriteDesc* sprites, std::size_t count,
                         const nl::renderer::Sprite2DAtlasRequest* atlases, std::uint32_t atlasCount,
                         float displayWidth, float displayHeight,
                         float cameraX, float cameraY, float pixelsPerUnit) {
    gpu_.clear();
    buckets_.clear();
    buildOk_ = false;

    // Atlas registrations first: they are idempotent per frame and idempotent
    // across frames (re-push only refreshes pending pixels). Failure is logged
    // by the caller via outError contract -- here a bad request just leaves
    // that id falling back to the builtin atlas.
    std::string atlasErr;
    for (std::uint32_t i = 0; atlases && i < atlasCount; ++i) {
        if (!ingestAtlas(atlases[i], atlasErr)) {
            atlasErr.clear();   // degrade to builtin; no frame failure
        }
    }

    if (!sprites || count == 0 || displayWidth <= 0.0f || displayHeight <= 0.0f) return;

    nl::sprite::SpriteBatch batch;
    for (std::size_t i = 0; i < count && i < kMaxSprites; ++i) batch.add(sprites[i]);
    batch.sortByTexture();

    gpu_.reserve(batch.size());
    for (const nl::sprite::SpriteDesc& s : batch.sprites()) {
        SpriteGpu g{};
        g.posSize[0] = s.x;  g.posSize[1] = s.y;
        g.posSize[2] = s.width; g.posSize[3] = s.height;
        g.uvRect[0] = s.uv.u0; g.uvRect[1] = s.uv.v0;
        g.uvRect[2] = s.uv.u1; g.uvRect[3] = s.uv.v1;
        // M-SP1: premultiply the tint so (premultiplied tex) * (premultiplied
        // tint) stays premultiplied for the One/InvSrcAlpha blend.
        const float a = s.color[3];
        g.color[0] = s.color[0] * a;
        g.color[1] = s.color[1] * a;
        g.color[2] = s.color[2] * a;
        g.color[3] = a;
        g.misc[0] = s.rotation;
        g.misc[1] = s.originX;
        g.misc[2] = s.originY;
        g.misc[3] = static_cast<float>(s.layer);
        gpu_.push_back(g);
    }
    buckets_ = batch.buckets();

    // World -> NDC: x right / y up on screen. pixelsPerUnit maps world units
    // to pixels; Y is negated for the D3D/Vulkan NDC convention.
    const float ppu = pixelsPerUnit > 0.0f ? pixelsPerUnit : 1.0f;
    const float sx = 2.0f * ppu / displayWidth;
    const float sy = -2.0f * ppu / displayHeight;
    constantsData_.projection[0] = sx;
    constantsData_.projection[1] = sy;
    constantsData_.projection[2] = -1.0f - cameraX * sx;
    constantsData_.projection[3] = 1.0f + cameraY * sy;
    buildOk_ = true;
}

void Sprite2DPass::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                        const nvrhi::Viewport& viewport) {
    if (!buildOk_ || !ready() || !framebuffer || gpu_.empty()) return;

    // Pending atlas uploads: builtin + every registered id, inside the frame
    // command list (M8.1: no second cmd list; builtin-atlas M-PT2 precedent).
    for (auto& kv : atlases_) {
        AtlasEntry& e = kv.second;
        if (!e.pendingUpload) continue;
        cmd->writeTexture(e.tex, 0, 0, e.pending.data(),
                          static_cast<std::size_t>(e.width) * 4u);
        cmd->setTextureState(e.tex, nvrhi::AllSubresources,
                             nvrhi::ResourceStates::ShaderResource);
        // Registered ids also need their binding set created after the texture
        // exists (created in ingestAtlas; here only for the builtin which was
        // created before bindingLayout_).
        if (!e.set) {
            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
                   .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instanceData_))
                   .addItem(nvrhi::BindingSetItem::Texture_SRV(1, e.tex))
                   .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_));
            e.set = device_->createBindingSet(setDesc, bindingLayout_);
        }
        e.pendingUpload = false;
        e.pending.clear();
        e.pending.shrink_to_fit();
    }
    cmd->writeBuffer(instanceData_, gpu_.data(), gpu_.size() * sizeof(SpriteGpu));

    // M-SP1: one draw per bucket, each with its atlas' binding set. Instance
    // data for bucket k lives at [begin, begin+count); the VS adds the CB's
    // g_baseIndex so SV_InstanceID stays 0-based on BOTH backends (no
    // setStartInstanceLocation, see sprite2d.hpp).
    std::size_t bucketCount = buckets_.size();
    if (bucketCount > kMaxBucketsPerFrame) {
        // Volatile CB version budget guard: merge the tail into one draw bound
        // to the builtin atlas is WRONG (wrong textures) -- instead clamp to
        // the budget by drawing only the first kMaxBucketsPerFrame buckets and
        // warn once. Real batches never hit this (buckets = atlases x layers).
        if (!warnedBucketFlood_) {
            nl::core::log::warnCh("renderer",
                "sprite2d: bucket count exceeds volatile CB budget, tail buckets dropped");
            warnedBucketFlood_ = true;
        }
        bucketCount = kMaxBucketsPerFrame;
    }
    for (std::size_t b = 0; b < bucketCount; ++b) {
        const nl::sprite::SpriteBucket& bucket = buckets_[b];
        AtlasEntry* entry = atlasOf(bucket.textureId);
        if (!entry || !entry->set) continue;
        Sprite2DConstants perBucket = constantsData_;
        perBucket.baseIndex = bucket.begin;
        cmd->writeBuffer(constants_, &perBucket, sizeof(perBucket));

        // NVRHI GraphicsState has no binding-set clear -- build a fresh state
        // per bucket (setPipeline/setFramebuffer are handle copies, negligible).
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline_)
             .setFramebuffer(framebuffer)
             .addBindingSet(entry->set);
        state.viewport.addViewportAndScissorRect(viewport);
        cmd->setGraphicsState(state);
        nvrhi::DrawArguments args;
        args.setVertexCount(6)
            .setInstanceCount(bucket.count);
        cmd->draw(args);
    }
}

nvrhi::ITexture* Sprite2DPass::atlasTexture(std::uint32_t textureId) {
    AtlasEntry* e = atlasOf(textureId);
    return (e && e->tex) ? e->tex.Get() : nullptr;
}

void Sprite2DPass::shutdown() {
    pipeline_ = nullptr;
    bindingLayout_ = nullptr;
    instanceData_ = nullptr;
    constants_ = nullptr;
    sampler_ = nullptr;
    atlases_.clear();   // textures + binding sets drop with the map entries
    ps_ = nullptr;
    vs_ = nullptr;
    gpu_.clear();
    buckets_.clear();
    buildOk_ = false;
    warnedBucketFlood_ = false;
    device_ = nullptr;
}

} // namespace nl::render
