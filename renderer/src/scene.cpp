#include "scene.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）

#include "nl/core/log.hpp"
#include "nl/core/mesh_io.hpp"   // M1.0：glTF 解析抽到 core，与 nanite-tools 共用

#include <cstring>
#include <vector>

namespace nl::renderer::detail {

bool Scene::init(nvrhi::IDevice* device, const char* shaderSuffix,
                 const char* vsEntry, const char* psEntry,
                 const char* modelFile, std::string& outError) {
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

    // ---- 2. 解析 GLB（core::mesh_io：场景图遍历 + 节点变换烘进顶点）----
    // M0.7：模型文件名由 --model 传入（上层 Renderer::create 已做白名单校验：
    // 仅 assets/models/ 下的 ASCII .glb 文件名，无路径分隔符）。缺省 Suzanne.glb。
    const std::string modelPath = std::string(NL_ASSET_DIR "/") + modelFile;
    nl::core::SceneMeshData meshData;
    std::string loadWarning;
    if (!nl::core::loadGltfScene(modelPath.c_str(), meshData, loadWarning)) {
        outError = modelFile + std::string(": ") + loadWarning; return false;
    }
    if (!loadWarning.empty()) nl::core::log::debug(loadWarning);
    // M0.4 起的约定：只画第一个子网格；其余子网格留给 nanite-tools 侧处理。
    if (meshData.meshes.empty()) {
        outError = modelFile + std::string(" has no mesh primitive"); return false;
    }
    const nl::core::PrimitiveMesh& prim = meshData.meshes[0];
    if (!prim.hasBaseColor) {
        outError = modelFile + std::string(" has no base color texture"); return false;
    }
    const size_t vertexCount = prim.vertices.size();
    if (prim.indices.empty()) {
        outError = modelFile + std::string(" first primitive has no indices"); return false;
    }
    const std::vector<std::uint32_t>& indices = prim.indices;

    std::vector<SceneVertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        const auto& mv = prim.vertices[i];
        vertices[i] = { mv.pos[0], mv.pos[1], mv.pos[2],
                        mv.normal[0], mv.normal[1], mv.normal[2],
                        mv.uv[0], mv.uv[1] };
    }
    indexCount_ = static_cast<std::uint32_t>(indices.size());
    const auto texWidth = prim.texWidth;
    const auto texHeight = prim.texHeight;

    // ---- 3. 建 GPU 资源（debug 名带模型名，RenderDoc / validation 里分得清）----
    const std::string vbName = std::string(modelFile) + "VB";
    const std::string ibName = std::string(modelFile) + "IB";
    const std::string texName = std::string(modelFile) + "BaseColor";
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = vertices.size() * sizeof(SceneVertex);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = vbName.c_str();
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    vertexBuffer_ = device->createBuffer(vbDesc);
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = indices.size() * sizeof(std::uint32_t);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = ibName.c_str();
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
    texDesc.debugName = texName.c_str();
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
    upload->writeTexture(texture_, 0, 0, prim.baseColorRGBA.data(),
                         static_cast<size_t>(texWidth) * 4u);
    upload->close();
    device->executeCommandList(upload);

    // M0.7：模型名与三角形数带回给 backend（DeviceInfo）→ viewer（HUD 自证）。
    modelName_ = modelFile;
    triangleCount_ = indexCount_ / 3;

    nl::core::log::info("model ready: ", modelFile, " | vertices=", vertexCount,
                        " triangles=", triangleCount_, " indices=", indexCount_,
                        " | baseColor ", texWidth, "x", texHeight,
                        " | shader=", shaderSuffix);
    return true;
}

bool Scene::ensurePipelines(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                            std::string& outError) {
    if (pipelineBack_ && pipelineNone_ && pipelineWireBack_ && pipelineWireNone_) return true;
    pipelineBack_ = nullptr;
    pipelineNone_ = nullptr;
    pipelineWireBack_ = nullptr;
    pipelineWireNone_ = nullptr;
    // M0.8：2×2 组合——fill(solid/wireframe) × cull(back/none)。着色模式走 cbuffer，
    // 不进 pipeline；四套共用同一对 shader。
    for (int wire = 0; wire < 2; ++wire) {
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
                .setFillMode(wire ? nvrhi::RasterFillMode::Wireframe
                                  : nvrhi::RasterFillMode::Solid)
                .setCullMode(i == 0 ? nvrhi::RasterCullMode::Back : nvrhi::RasterCullMode::None);
            desc.renderState.depthStencilState.enableDepthTest().enableDepthWrite();
            auto pipeline = device->createGraphicsPipeline(desc, info);
            if (!pipeline) { outError = "createGraphicsPipeline failed for model"; return false; }
            if (wire == 0)
                (i == 0 ? pipelineBack_ : pipelineNone_) = pipeline;
            else
                (i == 0 ? pipelineWireBack_ : pipelineWireNone_) = pipeline;
        }
    }
    return true;
}

void Scene::invalidatePipelines() {
    pipelineBack_ = nullptr;
    pipelineNone_ = nullptr;
    pipelineWireBack_ = nullptr;
    pipelineWireNone_ = nullptr;
}

void Scene::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                 const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!ready() || !framebuffer) return;

    SceneConstants cb{};
    std::memcpy(cb.mvp, params.mvp, sizeof(cb.mvp));
    std::memcpy(cb.model, params.model, sizeof(cb.model));
    std::memcpy(cb.lightDir, params.lightDir, sizeof(cb.lightDir));
    cb.options[0] = static_cast<float>(params.shading);   // M0.8：g_options.x = 着色模式
    std::memcpy(cb.options + 1, params.options, sizeof(cb.options) - sizeof(float));
    cmd->writeBuffer(constants_, &cb, sizeof(cb));   // volatile CB 每帧写一版（maxVersions=16）

    // M0.8：pipeline 按 (fill × cull) 四套里选一套，shader 不变。
    nvrhi::GraphicsPipelineHandle pipeline = params.wireframe
        ? (params.cullBack ? pipelineWireBack_ : pipelineWireNone_)
        : (params.cullBack ? pipelineBack_ : pipelineNone_);

    nvrhi::GraphicsState state;
    state.setPipeline(pipeline)
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
    pipelineWireBack_ = nullptr;
    pipelineWireNone_ = nullptr;
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
