#include "scene.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）

#include "nl/core/log.hpp"
#include "nl/core/mesh_io.hpp"     // M1.0：glTF 解析抽到 core，与 nanite-tools 共用
#include "nl/core/meshlet_io.hpp"  // M1.3：cluster 场景吃 .nmesh 资产

#include <cstring>
#include <vector>

namespace nl::renderer::detail {

// M1.3：与 model.slang triIdColor 完全相同的 pcg 整数 hash——簇色在 CPU 端算好
// 烘进顶点，两后端字节一致，不引入任何新 GPU 资源类型。
static void clusterColor(std::uint32_t id, float out[3]) {
    std::uint32_t h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    out[0] = static_cast<float>(h & 255u) / 255.0f;
    out[1] = static_cast<float>((h >> 8u) & 255u) / 255.0f;
    out[2] = static_cast<float>((h >> 16u) & 255u) / 255.0f;
}

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

// ---- M1.3：cluster 场景。加载 .nmesh，每簇随机着色，簇窗口展开成全局索引 ----
bool Scene::initCluster(nvrhi::IDevice* device, const char* shaderSuffix,
                        const char* vsEntry, const char* psEntry,
                        const char* nmeshFile, std::string& outError) {
    // ---- 1. shader 对（cluster 专用入口 vsCluster/psCluster：签名多带 CLUSTERCOLOR。
    // vsEntry/psEntry 由后端传，SPIR-V 恒为 main——cluster 字节码是独立编译产物，
    // 入口在 slangc 时已定为 main；此处参数仅 DXIL 用）----
    auto shaderPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/model." + artifact + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    if (!readFileBytes(shaderPath("vscluster").c_str(), vsBlob) ||
        !readFileBytes(shaderPath("pscluster").c_str(), psBlob)) {
        outError = std::string("failed to load cluster shaders (") + shaderSuffix +
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

    // ---- 2. 与 mesh 无关的 GPU 资源先建好：volatile CB + 1x1 白色占位贴图 +
    //         sampler + binding layout/set + 输入布局。M2.3 切层只换 VB/IB，
    //         这些一律不动；真实几何/索引由 uploadClusterMesh 读盘并上传。----
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(SceneConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "SceneConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for volatile constant buffer"; return false; }

    // mode 0-3 在 cluster 场景也可用（normal/uv/tri-id 用真实属性），textured 需要
    // 一个合法的 t0 绑定——1x1 白色占位，反正 cluster 模式不采样。
    nvrhi::TextureDesc texDesc;
    texDesc.width = 1; texDesc.height = 1;
    texDesc.format = nvrhi::Format::RGBA8_UNORM;
    texDesc.debugName = "ClusterWhite1x1";
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    texture_ = device->createTexture(texDesc);
    if (!texture_) { outError = "createTexture failed for cluster dummy texture"; return false; }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed"; return false; }

    // 槽位与 model.slang 的 register(b0/t0/s0) 一致（注释详见 init）。
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

    // 输入布局多一个 CLUSTERCOLOR（stride 44B）；其余三个与 SceneVertex 布局一致。
    nvrhi::VertexAttributeDesc attrs[4] = {
        nvrhi::VertexAttributeDesc().setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0)
            .setBufferIndex(0).setElementStride(sizeof(ClusterVertex)),
        nvrhi::VertexAttributeDesc().setName("NORMAL")
            .setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(12)
            .setBufferIndex(0).setElementStride(sizeof(ClusterVertex)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT).setOffset(24)
            .setBufferIndex(0).setElementStride(sizeof(ClusterVertex)),
        nvrhi::VertexAttributeDesc().setName("CLUSTERCOLOR")
            .setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(32)
            .setBufferIndex(0).setElementStride(sizeof(ClusterVertex)),
    };
    inputLayout_ = device->createInputLayout(attrs, 4, vs_);
    if (!inputLayout_) { outError = "createInputLayout failed for cluster scene"; return false; }

    // ---- 3. 占位贴图上传（1x1 白）；几何/索引由 uploadClusterMesh 上传 ----
    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    const std::uint8_t white[4] = { 255, 255, 255, 255 };
    upload->writeTexture(texture_, 0, 0, white, sizeof(white));
    upload->close();
    device->executeCommandList(upload);

    if (!uploadClusterMesh(device, nmeshFile, outError)) return false;

    nl::core::log::info("cluster scene ready: ", nmeshFile,
                        " | triangles=", triangleCount_,
                        " | shader=", shaderSuffix);
    return true;
}

// M4.0（S4.0）：GPU vertex pulling 场景。同吃 .nmesh，但几何不展开进顶点缓冲，而是
// 铺成三张 SSBO（顶点池 / 局部索引 / 簇元数据）+ identity 索引缓冲 + indirect args，
// 单 drawIndexedIndirect 全簇全画（instanceCount=总簇数，验证 pulling 通路；M4.1 接
// GPU 剔除时再改写 instanceCount）。簇色由 shader 端 pcg(簇号) 算出，与 cluster 模式
// CPU 端 clusterColor() 同式，逐像素一致。
bool Scene::initCull(nvrhi::IDevice* device, const char* shaderSuffix,
                     const char* vsEntry, const char* psEntry,
                     const char* nmeshFile, std::string& outError) {
    // ---- 1. shader 对（cull 专用入口 vsCull/psCull）。DX12 入口名被忽略，Vulkan 恒 main ----
    auto shaderPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/model." + artifact + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    if (!readFileBytes(shaderPath("vscull").c_str(), vsBlob) ||
        !readFileBytes(shaderPath("pscull").c_str(), psBlob)) {
        outError = std::string("failed to load cull shaders (") + shaderSuffix +
                   ") from " NL_SHADER_DIR;
        return false;
    }
    nvrhi::ShaderDesc vsDesc;
    vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName(vsEntry);
    nvrhi::ShaderDesc psDesc;
    psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName(psEntry);
    vsCull_ = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
    psCull_ = device->createShader(psDesc, psBlob.data(), psBlob.size());
    if (!vsCull_ || !psCull_) { outError = "createShader failed for cull shaders"; return false; }

    // ---- 2. 读 .nmesh 资产 ----
    const std::string assetPath = std::string(NL_MESHLET_DIR "/") + nmeshFile;
    nl::core::NMeshData nm;
    std::string loadError;
    if (!nl::core::loadNMesh(assetPath.c_str(), nm, loadError)) {
        outError = std::string(nmeshFile) + ": " + loadError;
        return false;
    }
    if (nm.meshlets.empty() || nm.triangles.empty()) {
        outError = std::string(nmeshFile) + " has no meshlets"; return false;
    }

    // ---- 3. 铺 SSBO ----
    // vertexPool：整份 nm.vertices 展平成 float（8 float/顶点，与 MeshVertex 布局一致）。
    // 簇 m 的顶点在池里的起址 = meshlet.vertexOffset（不重排，局部索引直接加偏移即得全局）。
    std::vector<float> vertexPool;
    vertexPool.reserve(nm.vertices.size() * 8);
    for (const auto& mv : nm.vertices) {
        vertexPool.push_back(mv.pos[0]); vertexPool.push_back(mv.pos[1]); vertexPool.push_back(mv.pos[2]);
        vertexPool.push_back(mv.normal[0]); vertexPool.push_back(mv.normal[1]); vertexPool.push_back(mv.normal[2]);
        vertexPool.push_back(mv.uv[0]); vertexPool.push_back(mv.uv[1]);
    }

    // localIndices：每簇 3×triCount 个局部索引（范围 [0,vertexCount)），随簇连续铺。
    // clusterMeta：每簇 4 个 uint = [vertexBase, triCount, localBase, pad]。
    const std::uint32_t clusterCount = static_cast<std::uint32_t>(nm.meshlets.size());
    std::uint32_t maxTriCount = 0;
    std::vector<std::uint32_t> localIndices;
    std::vector<std::uint32_t> clusterMeta;
    clusterMeta.reserve(clusterCount * 4);
    for (std::uint32_t m = 0; m < clusterCount; ++m) {
        const auto& meshlet = nm.meshlets[m];
        if (meshlet.triangleCount > maxTriCount) maxTriCount = meshlet.triangleCount;
        const std::uint32_t localBase = static_cast<std::uint32_t>(localIndices.size());
        const std::uint32_t base = meshlet.triangleOffset;
        for (std::uint32_t t = 0; t < meshlet.triangleCount; ++t) {
            for (int k = 0; k < 3; ++k) {
                // nm.triangles 存簇内局部顶点号（[0,vertexCount)），与 cluster 模式展开一致；
                // shader 里 gv = vertexBase + localIdx 所得即全局顶点号。
                localIndices.push_back(nm.triangles[base + t * 3 + static_cast<std::uint32_t>(k)]);
            }
        }
        clusterMeta.push_back(meshlet.vertexOffset);    // vertexBase
        clusterMeta.push_back(meshlet.triangleCount);  // triCount
        clusterMeta.push_back(localBase);               // localIndexBase
        clusterMeta.push_back(0);                      // pad
    }
    const std::uint32_t stride = maxTriCount * 3;       // identity 索引缓冲长度

    // ---- 4. 建 GPU 资源：constants_ + 1x1 白占位 + sampler + 三张 SSBO +
    //        indirect args + identity 索引缓冲 + binding layout/set ----
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(SceneConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "SceneConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for volatile constant buffer"; return false; }

    nvrhi::TextureDesc texDesc;
    texDesc.width = 1; texDesc.height = 1;
    texDesc.format = nvrhi::Format::RGBA8_UNORM;
    texDesc.debugName = "CullWhite1x1";
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    texture_ = device->createTexture(texDesc);
    if (!texture_) { outError = "createTexture failed for cull dummy texture"; return false; }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed"; return false; }

    auto makeSsbo = [&](const char* name, std::uint32_t byteSize, nvrhi::BufferHandle& out) {
        nvrhi::BufferDesc d;
        d.byteSize = byteSize;
        d.structStride = 4;          // float/uint 各 4 字节 -> StructuredBuffer
        d.debugName = name;
        out = device->createBuffer(d);
        return out != nullptr;
    };
    if (!makeSsbo("CullVertexPool",
                  static_cast<std::uint32_t>(vertexPool.size() * sizeof(float)), vertexPool_))
        { outError = "createBuffer failed for cull vertex pool"; return false; }
    if (!makeSsbo("CullLocalIndices",
                  static_cast<std::uint32_t>(localIndices.size() * sizeof(std::uint32_t)), localIndices_))
        { outError = "createBuffer failed for cull local indices"; return false; }
    if (!makeSsbo("CullClusterMeta",
                  static_cast<std::uint32_t>(clusterMeta.size() * sizeof(std::uint32_t)), clusterMeta_))
        { outError = "createBuffer failed for cull cluster meta"; return false; }

    // indirect args 缓冲（isDrawIndirectArgs）。M4.0 阶段 instanceCount 恒 = 总簇数（全画）。
    nvrhi::BufferDesc iaDesc;
    iaDesc.byteSize = sizeof(nvrhi::DrawIndexedIndirectArguments);
    iaDesc.isDrawIndirectArgs = true;
    iaDesc.debugName = "CullIndirectArgs";
    iaDesc.initialState = nvrhi::ResourceStates::CopyDest;
    indirectArgs_ = device->createBuffer(iaDesc);
    if (!indirectArgs_) { outError = "createBuffer failed for cull indirect args"; return false; }

    // identity 索引缓冲：值 0..stride-1，SV_VertexID 即其索引号。
    std::vector<std::uint32_t> identity(stride);
    for (std::uint32_t i = 0; i < stride; ++i) identity[i] = i;
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = identity.size() * sizeof(std::uint32_t);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "CullIdentityIndex";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    identityIndex_ = device->createBuffer(ibDesc);
    if (!identityIndex_) { outError = "createBuffer failed for cull identity index"; return false; }

    // 绑定布局：t0 占位贴图 + s0 sampler + b0 常量 + t1/t2/t3 三张 SSBO。
    // Vulkan 经 -fvk-t-shift 0 0 映射为 binding 1/2/3，DX12 为 SRV slot 1/2/3。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    cullBindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!cullBindingLayout_) { outError = "createBindingLayout failed for cull"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, vertexPool_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, localIndices_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, clusterMeta_));
    cullBindingSet_ = device->createBindingSet(setDesc, cullBindingLayout_);
    if (!cullBindingSet_) { outError = "createBindingSet failed for cull"; return false; }

    // ---- 5. 一次性上传：白占位贴图 + 三张 SSBO + indirect args + identity 索引 ----
    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    const std::uint8_t white[4] = { 255, 255, 255, 255 };
    upload->writeTexture(texture_, 0, 0, white, sizeof(white));

    upload->beginTrackingBufferState(vertexPool_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(vertexPool_, vertexPool.data(), vertexPool.size() * sizeof(float));
    upload->setPermanentBufferState(vertexPool_, nvrhi::ResourceStates::ShaderResource);
    upload->beginTrackingBufferState(localIndices_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(localIndices_, localIndices.data(), localIndices.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(localIndices_, nvrhi::ResourceStates::ShaderResource);
    upload->beginTrackingBufferState(clusterMeta_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(clusterMeta_, clusterMeta.data(), clusterMeta.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(clusterMeta_, nvrhi::ResourceStates::ShaderResource);

    nvrhi::DrawIndexedIndirectArguments args;
    args.setIndexCount(stride)
        .setInstanceCount(clusterCount)
        .setStartIndexLocation(0)
        .setBaseVertexLocation(0)
        .setStartInstanceLocation(0);
    upload->beginTrackingBufferState(indirectArgs_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(indirectArgs_, &args, sizeof(args));
    upload->setPermanentBufferState(indirectArgs_, nvrhi::ResourceStates::IndirectArgument);

    upload->beginTrackingBufferState(identityIndex_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(identityIndex_, identity.data(), identity.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(identityIndex_, nvrhi::ResourceStates::IndexBuffer);
    upload->close();
    device->executeCommandList(upload);

    cull_ = true;
    modelName_ = nmeshFile;
    // 全簇全画，总三角形 = 各簇 triCount 之和（与 cluster 模式的截图几何一致）。
    std::uint32_t totalTri = 0;
    for (const auto& m : nm.meshlets) totalTri += m.triangleCount;
    triangleCount_ = totalTri;

    nl::core::log::info("cull (vertex pulling) scene ready: ", nmeshFile,
                        " | meshlets=", clusterCount,
                        " | triangles=", triangleCount_,
                        " | stride=", stride,
                        " | shader=", shaderSuffix);
    return true;
}

// M2.3/M3.2 共用：把一份 .nmesh 展开成 ClusterVertex/索引。include 非空时只
// 展开掩码为 1 的簇（dag cut 用）；簇色按**本层内**的簇号烘（两后端同一 CPU
// hash，逐字节一致）。三角形展开用 triangleOffset+t*3 —— 它是字节偏移不是
// 三角号（M3.1 踩坑，meshlet.hpp 已钉死注释）。
//
// M3.2 踩坑：索引基址不能用 meshlet.vertexOffset——那是**文件内**偏移，只在
// 「全部簇按序全量展开」时恰好等于缓冲位置（M2.3 的唯一路径，所以一直没炸）。
// dag cut 的掩码跳簇后两者分叉：被跳簇之后的每个簇索引全部错位（M3.2 实测
// kept 簇整个消失）。基址必须取本簇顶点**入缓冲时**的实际位置。
void expandClusterData(const nl::core::NMeshData& nm, const std::vector<char>* include,
                       std::vector<ClusterVertex>& vertices,
                       std::vector<std::uint32_t>& indices) {
    for (std::size_t m = 0; m < nm.meshlets.size(); ++m) {
        if (include && !(*include)[m]) continue;
        const auto& meshlet = nm.meshlets[m];
        const std::uint32_t clusterBase = static_cast<std::uint32_t>(vertices.size());
        float color[3];
        clusterColor(static_cast<std::uint32_t>(m), color);
        for (std::uint32_t i = 0; i < meshlet.vertexCount; ++i) {
            const auto& mv = nm.vertices[meshlet.vertexOffset + i];
            ClusterVertex cv;
            cv.v = { mv.pos[0], mv.pos[1], mv.pos[2],
                     mv.normal[0], mv.normal[1], mv.normal[2],
                     mv.uv[0], mv.uv[1] };
            cv.color[0] = color[0]; cv.color[1] = color[1]; cv.color[2] = color[2];
            vertices.push_back(cv);
        }
        const std::uint32_t base = meshlet.triangleOffset;
        for (std::uint32_t t = 0; t < meshlet.triangleCount; ++t) {
            for (int k = 0; k < 3; ++k) {
                indices.push_back(clusterBase +
                                  nm.triangles[base + t * 3 + static_cast<std::uint32_t>(k)]);
            }
        }
    }
}

// M2.3：读一份 .nmesh（原位层或分层资产）→ 展开成顶点/索引 → 新建 VB/IB 并上传，
// 最后才替换句柄。切层和首次加载走同一条路径，保证「L0 直读」与「滑条切到 L0」
// 的画面完全一致；失败时旧层的 buffer / 计数原封不动。
bool Scene::uploadClusterMesh(nvrhi::IDevice* device, const char* nmeshFile,
                              std::string& outError) {
    // ---- 1. 读 .nmesh 资产（M1.2 的 loadNMesh 已校验 magic/版本/截断/窗口越界）----
    const std::string assetPath = std::string(NL_MESHLET_DIR "/") + nmeshFile;
    nl::core::NMeshData nm;
    std::string loadError;
    if (!nl::core::loadNMesh(assetPath.c_str(), nm, loadError)) {
        outError = std::string(nmeshFile) + ": " + loadError;
        return false;
    }
    if (nm.meshlets.empty() || nm.triangles.empty()) {
        outError = std::string(nmeshFile) + " has no meshlets"; return false;
    }

    // ---- 2. 展开成渲染数据（全簇）----
    std::vector<ClusterVertex> vertices;
    vertices.reserve(nm.vertices.size());
    std::vector<std::uint32_t> indices;
    indices.reserve(nm.triangles.size());
    expandClusterData(nm, nullptr, vertices, indices);

    // ---- 3. 新建 VB/IB 上传并替换句柄 ----
    if (!uploadClusterData(device, vertices, indices, nmeshFile,
                           nm.lodMeta.present ? static_cast<int>(nm.lodMeta.level) : 0,
                           nm.lodMeta.present ? nm.lodMeta.maxError : 0.0f, outError)) {
        return false;
    }
    dagGroup_ = -1;          // 纯层加载：无下钻状态
    dagGroupError_ = 0.0f;
    nl::core::log::debug("cluster mesh uploaded: ", nmeshFile,
                         " vertices=", vertices.size(),
                         " triangles=", triangleCount_,
                         " meshlets=", nm.meshlets.size(),
                         " lodLevel=", lodLevel_,
                         " maxError=", lodMaxError_);
    return true;
}

// M3.2：dag-top 的组原子下钻。详见 scene.hpp 注释。
bool Scene::setDagCut(nvrhi::IDevice* device, const char* topFile, const char* childFile,
                      int drillGroup, std::string& outError) {
    const auto load = [&outError](const char* file, nl::core::NMeshData& out) -> bool {
        const std::string path = std::string(NL_MESHLET_DIR "/") + file;
        std::string err;
        if (!nl::core::loadNMesh(path.c_str(), out, err)) {
            outError = std::string(file) + ": " + err;
            return false;
        }
        if (out.meshlets.empty() || out.triangles.empty()) {
            outError = std::string(file) + " has no meshlets";
            return false;
        }
        return true;
    };
    nl::core::NMeshData top, child;
    if (!load(topFile, top) || !load(childFile, child)) return false;

    // ---- 校验 DAG 血缘：没有血缘 / 组号越界 / gen 区间越界一律显式失败 ----
    if (!child.lodMeta.dagPresent || child.lodMeta.dagGroups.empty()) {
        outError = std::string(childFile) + " has no DAG lineage (meta version 2)";
        return false;
    }
    if (drillGroup < 0 ||
        static_cast<std::size_t>(drillGroup) >= child.lodMeta.dagGroups.size()) {
        outError = "dag drill group " + std::to_string(drillGroup) + " out of range 0.." +
                   std::to_string(child.lodMeta.dagGroups.size() - 1) + " (" + childFile + ")";
        return false;
    }
    const auto& group = child.lodMeta.dagGroups[static_cast<std::size_t>(drillGroup)];
    if (group.genCount == 0) {
        outError = "dag drill group " + std::to_string(drillGroup) + " is terminal (genCount=0)";
        return false;
    }
    if (group.genFirst + group.genCount > top.meshlets.size()) {
        outError = "dag drill group " + std::to_string(drillGroup) + " gen range [" +
                   std::to_string(group.genFirst) + ", " +
                   std::to_string(group.genFirst + group.genCount) + ") exceeds " +
                   topFile + " (" + std::to_string(top.meshlets.size()) + " clusters)";
        return false;
    }
    if (child.lodMeta.clusterGroup.size() != child.meshlets.size()) {
        outError = std::string(childFile) + " clusterGroup table size mismatch";
        return false;
    }

    // ---- 顶层：挖掉该组生成的父簇；子层：只取该组成员簇 ----
    std::vector<char> topInclude(top.meshlets.size(), 1);
    for (std::uint32_t i = 0; i < group.genCount; ++i)
        topInclude[group.genFirst + i] = 0;
    std::vector<char> childInclude(child.meshlets.size(), 0);
    for (std::size_t m = 0; m < child.meshlets.size(); ++m)
        if (child.lodMeta.clusterGroup[m] == static_cast<std::uint32_t>(drillGroup))
            childInclude[m] = 1;

    std::vector<ClusterVertex> vertices;
    std::vector<std::uint32_t> indices;
    expandClusterData(top, &topInclude, vertices, indices);
    expandClusterData(child, &childInclude, vertices, indices);
    if (indices.empty()) {
        outError = "dag cut produced an empty mesh"; return false;
    }

    // 顶层元数据照旧上报（HUD 的 lod N/M 与 maxErr 描述的是被 cut 的顶层）。
    const int topLevel = top.lodMeta.present ? static_cast<int>(top.lodMeta.level) : 0;
    const float topError = top.lodMeta.present ? top.lodMeta.maxError : 0.0f;
    if (!uploadClusterData(device, vertices, indices, topFile, topLevel, topError, outError)) {
        return false;
    }
    dagGroup_ = drillGroup;
    dagGroupError_ = group.error;
    nl::core::log::info("dag cut: ", topFile, " minus parents [", group.genFirst, ", ",
                        group.genFirst + group.genCount, ") <- ", childFile,
                        " group ", drillGroup,
                        " (error=", group.error, ") | triangles=", triangleCount_);
    return true;
}

// M2.3/M3.2 共用：建新 VB/IB 上传并替换句柄与计数。老句柄此刻还在用；替换后
// 由 NVRHI 引用计数延迟释放。失败时旧 buffer / 计数原封不动（先成功后替换）。
bool Scene::uploadClusterData(nvrhi::IDevice* device,
                              const std::vector<ClusterVertex>& vertices,
                              const std::vector<std::uint32_t>& indices,
                              const char* modelName, int lodLevel, float maxError,
                              std::string& outError) {
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = vertices.size() * sizeof(ClusterVertex);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "ClusterVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    nvrhi::BufferHandle vb = device->createBuffer(vbDesc);
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = indices.size() * sizeof(std::uint32_t);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "ClusterIB";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    nvrhi::BufferHandle ib = device->createBuffer(ibDesc);
    if (!vb || !ib) { outError = "createBuffer failed for cluster VB/IB"; return false; }

    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    upload->beginTrackingBufferState(vb, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(vb, vertices.data(), vertices.size() * sizeof(ClusterVertex));
    upload->setPermanentBufferState(vb, nvrhi::ResourceStates::VertexBuffer);
    upload->beginTrackingBufferState(ib, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(ib, indices.data(), indices.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(ib, nvrhi::ResourceStates::IndexBuffer);
    upload->close();
    device->executeCommandList(upload);

    vertexBuffer_ = vb;
    indexBuffer_ = ib;
    indexCount_ = static_cast<std::uint32_t>(indices.size());
    triangleCount_ = indexCount_ / 3;
    modelName_ = modelName;
    lodLevel_ = lodLevel;
    lodMaxError_ = maxError;
    return true;
}

bool Scene::setClusterLevel(nvrhi::IDevice* device, const char* nmeshFile,
                            std::string& outError) {
    return uploadClusterMesh(device, nmeshFile, outError);
}

bool Scene::ensurePipelines(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                            std::string& outError) {
    // M4.0（S4.0）：cull 模式走 vertex pulling——pipeline 无顶点输入。
    if (cull_) {
        if (pipelineCullBack_ && pipelineCullNone_ && pipelineCullWireBack_ && pipelineCullWireNone_)
            return true;
        pipelineCullBack_ = nullptr;
        pipelineCullNone_ = nullptr;
        pipelineCullWireBack_ = nullptr;
        pipelineCullWireNone_ = nullptr;
        for (int wire = 0; wire < 2; ++wire) {
            for (int i = 0; i < 2; ++i) {
                nvrhi::GraphicsPipelineDesc desc;
                // 输入布局故意为 null：VS 只读 SV_InstanceID/SV_VertexID + SSBO，无顶点属性。
                desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                    .setVertexShader(vsCull_)
                    .setFragmentShader(psCull_)
                    .addBindingLayout(cullBindingLayout_);
                desc.renderState.rasterState
                    .setFrontCounterClockwise(true)
                    .setFillMode(wire ? nvrhi::RasterFillMode::Wireframe
                                      : nvrhi::RasterFillMode::Solid)
                    .setCullMode(i == 0 ? nvrhi::RasterCullMode::Back : nvrhi::RasterCullMode::None);
                desc.renderState.depthStencilState.enableDepthTest().enableDepthWrite();
                auto pipeline = device->createGraphicsPipeline(desc, info);
                if (!pipeline) { outError = "createGraphicsPipeline failed for cull"; return false; }
                if (wire == 0)
                    (i == 0 ? pipelineCullBack_ : pipelineCullNone_) = pipeline;
                else
                    (i == 0 ? pipelineCullWireBack_ : pipelineCullWireNone_) = pipeline;
            }
        }
        return true;
    }

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
    pipelineCullBack_ = nullptr;
    pipelineCullNone_ = nullptr;
    pipelineCullWireBack_ = nullptr;
    pipelineCullWireNone_ = nullptr;
}

void Scene::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                 const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!ready() || !framebuffer) return;

    // M4.0（S4.0）：vertex pulling 路径——无顶点缓冲，几何由 VS 从 SSBO 拉取，
    // 单 drawIndexedIndirect（instanceCount=总簇数）全簇全画。
    if (cull_) {
        SceneConstants cb{};
        std::memcpy(cb.mvp, params.mvp, sizeof(cb.mvp));
        std::memcpy(cb.model, params.model, sizeof(cb.model));
        std::memcpy(cb.lightDir, params.lightDir, sizeof(cb.lightDir));
        cb.options[0] = static_cast<float>(params.shading);   // g_options.x = 着色模式
        std::memcpy(cb.options + 1, params.options, sizeof(cb.options) - sizeof(float));
        cmd->writeBuffer(constants_, &cb, sizeof(cb));

        nvrhi::GraphicsPipelineHandle pipeline = params.wireframe
            ? (params.cullBack ? pipelineCullWireBack_ : pipelineCullWireNone_)
            : (params.cullBack ? pipelineCullBack_ : pipelineCullNone_);

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
             .setFramebuffer(framebuffer)
             .addBindingSet(cullBindingSet_)
             .setIndexBuffer(nvrhi::IndexBufferBinding()
                 .setBuffer(identityIndex_).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
        state.viewport.addViewportAndScissorRect(viewport);
        state.setIndirectParams(indirectArgs_);
        cmd->setGraphicsState(state);
        cmd->drawIndexedIndirect(0, 1);
        return;
    }

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
    pipelineCullBack_ = nullptr;
    pipelineCullNone_ = nullptr;
    pipelineCullWireBack_ = nullptr;
    pipelineCullWireNone_ = nullptr;
    cullBindingSet_ = nullptr;
    cullBindingLayout_ = nullptr;
    vertexPool_ = nullptr;
    localIndices_ = nullptr;
    clusterMeta_ = nullptr;
    indirectArgs_ = nullptr;
    identityIndex_ = nullptr;
    vsCull_ = nullptr;
    psCull_ = nullptr;
    cull_ = false;
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
