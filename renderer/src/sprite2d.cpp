// M-SP0 Sprite2DPass implementation.
// Resource lifecycle copied from hud.cpp (init/ensurePipeline/draw/shutdown)
// and Particles (StructuredBuffer + SV_VertexID), both dual-backend proven.

#include "sprite2d.hpp"
#include "backend.hpp"          // readFileBytes

#include <nl/core/paths.hpp>

#include <cstring>
#include <utility>

namespace nl::render {
namespace {

// 4x4 builtin atlas: deterministic colored cells, so a future demo needs no
// external asset. Real game atlas registration is M-SP1 (game side supplies
// pixels); M-SP0 only proves the GPU path can be built and driven.
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

} // namespace

bool Sprite2DPass::ready() const {
    return pipeline_ && bindingLayout_ && bindingSet_ && constants_ &&
           instanceData_ && sampler_ && atlasTex_ && vs_ && ps_;
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

    // volatile CB: 16 B, fully overwritten every frame (§2.7).
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(Sprite2DConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
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

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instanceData_))
           .addItem(nvrhi::BindingSetItem::Texture_SRV(1, atlasTex_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_));
    bindingSet_ = device->createBindingSet(setDesc, bindingLayout_);
    if (!bindingSet_) { outError = "createBindingSet failed for sprite2d"; return false; }
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
    atlasTex_ = device_->createTexture(td);
    if (!atlasTex_) { outError = "createTexture failed for sprite2d atlas"; return false; }
    atlasPixels_ = std::move(pixels);
    atlasWidth_ = w;
    atlasPendingUpload_ = true;
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
    rt.enableBlend()
      .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
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
                         float displayWidth, float displayHeight,
                         float cameraX, float cameraY, float pixelsPerUnit) {
    gpu_.clear();
    buckets_.clear();
    buildOk_ = false;
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
        for (int k = 0; k < 4; ++k) g.color[k] = s.color[k];
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
    if (atlasPendingUpload_) {
        cmd->writeTexture(atlasTex_, 0, 0, atlasPixels_.data(),
                          static_cast<std::size_t>(atlasWidth_) * 4u);
        cmd->setTextureState(atlasTex_, nvrhi::AllSubresources,
                             nvrhi::ResourceStates::ShaderResource);
        atlasPendingUpload_ = false;
        atlasPixels_.clear();
    }
    cmd->writeBuffer(constants_, &constantsData_, sizeof(constantsData_));
    cmd->writeBuffer(instanceData_, gpu_.data(), gpu_.size() * sizeof(SpriteGpu));

    // M-SP0: one draw for the whole (sorted) batch. Per-bucket binding sets
    // arrive in M-SP1 together with real atlas registration.
    nvrhi::GraphicsState state;
    state.setPipeline(pipeline_)
         .setFramebuffer(framebuffer)
         .addBindingSet(bindingSet_);
    state.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(state);
    nvrhi::DrawArguments args;
    args.setVertexCount(6)
        .setInstanceCount(static_cast<std::uint32_t>(gpu_.size()));
    cmd->draw(args);
}

void Sprite2DPass::shutdown() {
    pipeline_ = nullptr;
    bindingSet_ = nullptr;
    bindingLayout_ = nullptr;
    instanceData_ = nullptr;
    constants_ = nullptr;
    sampler_ = nullptr;
    atlasTex_ = nullptr;
    ps_ = nullptr;
    vs_ = nullptr;
    atlasPixels_.clear();
    atlasPendingUpload_ = false;
    atlasWidth_ = 0;
    gpu_.clear();
    buckets_.clear();
    buildOk_ = false;
    device_ = nullptr;
}

} // namespace nl::render
