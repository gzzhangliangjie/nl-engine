#include "scene.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）

#include "nl/core/log.hpp"

// tinygltf 是 header-only：整个工程只在这一个 TU 展开实现，避免重复符号。
// M0.4 只用 GLB 内嵌 PNG，不需要 stb_image_write（png_writer.cpp 有自己的那份）。
#ifdef _MSC_VER
#  pragma warning(push, 0)
#endif
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define STB_IMAGE_IMPLEMENTATION
#include <tiny_gltf.h>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include <cstring>
#include <vector>

namespace nl::renderer::detail {
namespace {

// 读 FLOAT 类型的 glTF accessor（POSITION / NORMAL / TEXCOORD_0）。
// 支持 byteStride 交错存储；Suzanne 是紧密排列，这里仍按通用写法处理。
bool readFloatAccessor(const tinygltf::Model& model, int accessorIndex,
                       int wantComponents, std::vector<float>& out,
                       std::string& outError) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
        outError = "glTF accessor index out of range"; return false;
    }
    const auto& acc = model.accessors[accessorIndex];
    if (acc.componentType != 5126 /* FLOAT */ ||
        tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type)) != wantComponents) {
        outError = "glTF accessor is not a float VEC" + std::to_string(wantComponents);
        return false;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buffer = model.buffers[view.buffer];
    const size_t elemSize = sizeof(float) * static_cast<size_t>(wantComponents);
    const size_t stride = view.byteStride > 0 ? static_cast<size_t>(view.byteStride) : elemSize;
    const size_t base = static_cast<size_t>(view.byteOffset) + acc.byteOffset;
    if (acc.count == 0 || base + stride * (acc.count - 1) + elemSize > buffer.data.size()) {
        outError = "glTF accessor data truncated"; return false;
    }
    out.resize(acc.count * static_cast<size_t>(wantComponents));
    for (size_t i = 0; i < acc.count; ++i) {
        std::memcpy(out.data() + i * wantComponents,
                    buffer.data.data() + base + i * stride, elemSize);
    }
    return true;
}

// 索引统一展开成 uint32（R32_UINT），不区分 glTF 的 UNSIGNED_BYTE/SHORT/INT。
bool readIndices(const tinygltf::Model& model, int accessorIndex,
                 std::vector<std::uint32_t>& out, std::string& outError) {
    const auto& acc = model.accessors[accessorIndex];
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buffer = model.buffers[view.buffer];
    const int compSize = tinygltf::GetComponentSizeInBytes(
        static_cast<uint32_t>(acc.componentType));
    const int comps = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type));
    if (compSize <= 0 || comps != 1 ||
        (acc.componentType != 5121 && acc.componentType != 5123 && acc.componentType != 5125)) {
        outError = "unsupported glTF index component type"; return false;
    }
    const size_t stride = view.byteStride > 0
        ? static_cast<size_t>(view.byteStride) : static_cast<size_t>(compSize);
    const size_t base = static_cast<size_t>(view.byteOffset) + acc.byteOffset;
    if (acc.count == 0 || base + stride * (acc.count - 1) + compSize > buffer.data.size()) {
        outError = "glTF index data truncated"; return false;
    }
    out.resize(acc.count);
    const unsigned char* p = buffer.data.data() + base;
    for (size_t i = 0; i < acc.count; ++i) {
        const unsigned char* src = p + i * stride;
        std::uint32_t v = 0;
        if (acc.componentType == 5121) v = src[0];
        else if (acc.componentType == 5123) { std::uint16_t u; std::memcpy(&u, src, 2); v = u; }
        else { std::memcpy(&v, src, 4); }
        out[i] = v;
    }
    return true;
}

// tinygltf 用 stb_image 解出的像素可能是 1/2/3/4 通道，统一补成 RGBA8。
bool toRGBA8(const tinygltf::Image& image, std::vector<std::uint8_t>& out,
             std::string& outError) {
    if (image.width <= 0 || image.height <= 0 || image.image.empty() || image.bits != 8 ||
        image.component < 1 || image.component > 4) {
        outError = "glTF base color image is not 8-bit decoded RGBA-capable data";
        return false;
    }
    const size_t pixels = static_cast<size_t>(image.width) * image.height;
    if (image.image.size() < pixels * static_cast<size_t>(image.component)) {
        outError = "glTF base color image data truncated"; return false;
    }
    out.assign(pixels * 4, 255);
    for (size_t i = 0; i < pixels; ++i) {
        const unsigned char* s = image.image.data() + i * image.component;
        unsigned char* d = out.data() + i * 4;
        d[0] = s[0];
        d[1] = image.component >= 3 ? s[1] : s[0];
        d[2] = image.component >= 3 ? s[2] : s[0];
        d[3] = image.component == 4 ? s[3] : 255;
    }
    return true;
}

} // namespace

bool Scene::init(nvrhi::IDevice* device, const char* shaderSuffix,
                 const char* vsEntry, const char* psEntry, std::string& outError) {
    // ---- 1. 加载 Slang 双目标字节码（同一份 model.slang 出的 spv / dxil）----
    auto shaderPath = [shaderSuffix](const char* stage) {
        return std::string(NL_SHADER_DIR) + "/model." + stage + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    if (!readFileBytes(shaderPath("vs").c_str(), vsBlob) ||
        !readFileBytes(shaderPath("ps").c_str(), psBlob)) {
        outError = std::string("failed to load model shader (") + shaderSuffix +
                   ") from " NL_SHADER_DIR;
        return false;
    }
    nvrhi::ShaderDesc vsDesc;
    vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName(vsEntry);
    nvrhi::ShaderDesc psDesc;
    psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName(psEntry);
    vs_ = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
    ps_ = device->createShader(psDesc, psBlob.data(), psBlob.size());
    if (!vs_ || !ps_) { outError = "createShader failed for model shaders"; return false; }

    // ---- 2. 解析 GLB：Suzanne 的 POSITION / NORMAL / TEXCOORD_0 / indices / baseColor ----
    tinygltf::Model gltf;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    if (!loader.LoadBinaryFromFile(&gltf, &err, &warn, NL_ASSET_DIR "/Suzanne.glb")) {
        outError = "LoadBinaryFromFile(Suzanne.glb) failed: " + err;
        return false;
    }
    if (!warn.empty()) nl::core::log::debug("tinygltf: ", warn);
    if (gltf.meshes.empty() || gltf.meshes[0].primitives.empty()) {
        outError = "Suzanne.glb has no mesh primitive"; return false;
    }
    const auto& prim = gltf.meshes[0].primitives[0];
    auto findAttr = [&prim](const char* name) {
        auto it = prim.attributes.find(name);
        return it == prim.attributes.end() ? -1 : it->second;
    };
    std::vector<float> positions, normals, uvs;
    std::vector<std::uint32_t> indices;
    if (!readFloatAccessor(gltf, findAttr("POSITION"), 3, positions, outError) ||
        !readFloatAccessor(gltf, findAttr("NORMAL"), 3, normals, outError) ||
        !readFloatAccessor(gltf, findAttr("TEXCOORD_0"), 2, uvs, outError) ||
        !readIndices(gltf, prim.indices, indices, outError)) return false;
    const size_t vertexCount = positions.size() / 3;
    if (normals.size() / 3 != vertexCount || uvs.size() / 2 != vertexCount) {
        outError = "Suzanne.glb attribute counts disagree"; return false;
    }

    int imageIndex = -1;
    if (prim.material >= 0 && prim.material < static_cast<int>(gltf.materials.size())) {
        const int texIndex = gltf.materials[prim.material].pbrMetallicRoughness.baseColorTexture.index;
        if (texIndex >= 0 && texIndex < static_cast<int>(gltf.textures.size()))
            imageIndex = gltf.textures[texIndex].source;
    }
    if (imageIndex < 0 || imageIndex >= static_cast<int>(gltf.images.size())) {
        outError = "Suzanne.glb has no base color texture"; return false;
    }
    std::vector<std::uint8_t> rgba;
    if (!toRGBA8(gltf.images[imageIndex], rgba, outError)) return false;
    const auto texWidth = static_cast<std::uint32_t>(gltf.images[imageIndex].width);
    const auto texHeight = static_cast<std::uint32_t>(gltf.images[imageIndex].height);

    std::vector<SceneVertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        vertices[i] = {
            positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2],
            normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2],
            uvs[i * 2], uvs[i * 2 + 1]
        };
    }
    indexCount_ = static_cast<std::uint32_t>(indices.size());

    // ---- 3. 建 GPU 资源 ----
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = vertices.size() * sizeof(SceneVertex);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "SuzanneVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    vertexBuffer_ = device->createBuffer(vbDesc);
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = indices.size() * sizeof(std::uint32_t);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "SuzanneIB";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    indexBuffer_ = device->createBuffer(ibDesc);
    if (!vertexBuffer_ || !indexBuffer_) { outError = "createBuffer failed for model VB/IB"; return false; }

    // M0.4 的 MVP 走 NVRHI volatile constant buffer：语义接近 push constant，
    // 但两后端（DX12 root CBV / Vulkan push-descriptor 版本池）由 NVRHI 统一管理。
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(SceneConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "SceneConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for volatile constant buffer"; return false; }

    nvrhi::TextureDesc texDesc;
    texDesc.width = texWidth;
    texDesc.height = texHeight;
    texDesc.format = nvrhi::Format::RGBA8_UNORM;
    texDesc.debugName = "SuzanneBaseColor";
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    texture_ = device->createTexture(texDesc);
    if (!texture_) { outError = "createTexture failed for base color texture"; return false; }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed"; return false; }

    // 布局槽位与 model.slang 的 register(b0/t0/s0) 一致；
    // Vulkan 侧由 NVRHI 默认 VulkanBindingOffsets 映射为 CB=256 / SRV=0 / Sampler=128，
    // 与 slangc 的 -fvk-b-shift 256 -fvk-s-shift 128 严格对应。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    bindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!bindingLayout_) { outError = "createBindingLayout failed"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_));
    bindingSet_ = device->createBindingSet(setDesc, bindingLayout_);
    if (!bindingSet_) { outError = "createBindingSet failed"; return false; }

    nvrhi::VertexAttributeDesc attrs[3] = {
        nvrhi::VertexAttributeDesc().setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0)
            .setBufferIndex(0).setElementStride(sizeof(SceneVertex)),
        nvrhi::VertexAttributeDesc().setName("NORMAL")
            .setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(12)
            .setBufferIndex(0).setElementStride(sizeof(SceneVertex)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT).setOffset(24)
            .setBufferIndex(0).setElementStride(sizeof(SceneVertex)),
    };
    inputLayout_ = device->createInputLayout(attrs, 3, vs_);
    if (!inputLayout_) { outError = "createInputLayout failed for model"; return false; }

    // ---- 4. 一次性上传：staging → VB/IB/Texture ----
    // NVRHI 的 writeBuffer/writeTexture 内部走 upload manager 的 staging buffer +
    // CopyBufferRegion / CopyTextureRegion（Vulkan 是 vkCmdCopyBufferToImage），
    // 并按自动 barrier 跟踪把资源转到 VertexBuffer / IndexBuffer / ShaderResource；
    // 这就是 M0.4 要「看懂」的那层转换，不需要手写 VkImageMemoryBarrier。
    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    upload->beginTrackingBufferState(vertexBuffer_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(vertexBuffer_, vertices.data(), vertices.size() * sizeof(SceneVertex));
    upload->setPermanentBufferState(vertexBuffer_, nvrhi::ResourceStates::VertexBuffer);
    upload->beginTrackingBufferState(indexBuffer_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(indexBuffer_, indices.data(), indices.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(indexBuffer_, nvrhi::ResourceStates::IndexBuffer);
    upload->writeTexture(texture_, 0, 0, rgba.data(),
                         static_cast<size_t>(texWidth) * 4u);
    upload->close();
    device->executeCommandList(upload);

    nl::core::log::info("model ready: Suzanne.glb | vertices=", vertexCount,
                        " indices=", indexCount_,
                        " | baseColor ", texWidth, "x", texHeight,
                        " | shader=", shaderSuffix);
    return true;
}

bool Scene::ensurePipelines(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                            std::string& outError) {
    if (pipelineBack_ && pipelineNone_) return true;
    pipelineBack_ = nullptr;
    pipelineNone_ = nullptr;
    for (int i = 0; i < 2; ++i) {
        nvrhi::GraphicsPipelineDesc desc;
        desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setInputLayout(inputLayout_)
            .setVertexShader(vs_)
            .setFragmentShader(ps_)
            .addBindingLayout(bindingLayout_);
        // glTF 外表面是 CCW（NDC），视口 Y 向下后 framebuffer 里为 CW；但 Slang 生成的
        // DXIL/SPIR-V 与 NVRHI 的 Vulkan 视口换算叠加后，实测外表面被判成 back-face
        // （cull=back 会剔掉正脸、只剩内壁）。故统一把 CCW 指定为正面，两后端一致。
        desc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setCullMode(i == 0 ? nvrhi::RasterCullMode::Back : nvrhi::RasterCullMode::None);
        desc.renderState.depthStencilState.enableDepthTest().enableDepthWrite();
        auto pipeline = device->createGraphicsPipeline(desc, info);
        if (!pipeline) { outError = "createGraphicsPipeline failed for model"; return false; }
        if (i == 0) pipelineBack_ = pipeline; else pipelineNone_ = pipeline;
    }
    return true;
}

void Scene::invalidatePipelines() {
    pipelineBack_ = nullptr;
    pipelineNone_ = nullptr;
}

void Scene::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                 const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!ready() || !framebuffer) return;

    SceneConstants cb{};
    std::memcpy(cb.mvp, params.mvp, sizeof(cb.mvp));
    std::memcpy(cb.model, params.model, sizeof(cb.model));
    std::memcpy(cb.lightDir, params.lightDir, sizeof(cb.lightDir));
    std::memcpy(cb.options, params.options, sizeof(cb.options));
    cmd->writeBuffer(constants_, &cb, sizeof(cb));   // volatile CB 每帧写一版（maxVersions=16）

    nvrhi::GraphicsState state;
    state.setPipeline(params.cullBack ? pipelineBack_ : pipelineNone_)
         .setFramebuffer(framebuffer)
         .addBindingSet(bindingSet_)
         .addVertexBuffer(nvrhi::VertexBufferBinding()
             .setBuffer(vertexBuffer_).setSlot(0).setOffset(0))
         .setIndexBuffer(nvrhi::IndexBufferBinding()
             .setBuffer(indexBuffer_).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
    state.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(state);
    nvrhi::DrawArguments args;
    args.setVertexCount(indexCount_);   // drawIndexed 复用 vertexCount 字段表示索引数
    cmd->drawIndexed(args);
}

void Scene::shutdown() {
    pipelineBack_ = nullptr;
    pipelineNone_ = nullptr;
    bindingSet_ = nullptr;
    bindingLayout_ = nullptr;
    inputLayout_ = nullptr;
    sampler_ = nullptr;
    texture_ = nullptr;
    constants_ = nullptr;
    indexBuffer_ = nullptr;
    vertexBuffer_ = nullptr;
    ps_ = nullptr;
    vs_ = nullptr;
    indexCount_ = 0;
}

} // namespace nl::renderer::detail
