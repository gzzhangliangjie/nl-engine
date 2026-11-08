#include "scene.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）

#include "nl/core/log.hpp"
#include "nl/core/mesh_io.hpp"     // M1.0：glTF 解析抽到 core，与 nanite-tools 共用
#include "nl/core/meshlet_io.hpp"  // M1.3：cluster 场景吃 .nmesh 资产

#include <cmath>
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
// 单 drawIndexedIndirect 全簇全画。簇色由 shader 端 pcg(簇号) 算出，与 cluster 模式
// CPU 端 clusterColor() 同式，逐像素一致。
// M4.1（S4.1）：frustumCull=true 时加 cull.slang compute pass——簇包围球 SSBO +
// 可见列表 SSBO + compute pipeline，每线程 1 簇 5 平面球测试（bevy 蓝本），通过者
// append 簇号 + atomicAdd 写 args.instanceCount；VS 经可见列表取簇号。false 时
// 可见列表恒等映射、不建 compute 资源，行为与 M4.0 一致。
bool Scene::initCull(nvrhi::IDevice* device, const char* shaderSuffix,
                     const char* vsEntry, const char* psEntry,
                     const char* nmeshFile, bool frustumCull, std::string& outError) {
    frustumCull_ = frustumCull;
    cullTimer_.setLabel("cull");   // M4.2：resolve 日志标明这是 cull pass 实例
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
    clusterCount_ = clusterCount;
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

    // M4.1：簇包围球（世界空间 center.xyz + radius.w，M1.0 Ritter 近似产物）。
    // 剔除开启时才需要；nm.bounds 与 nm.meshlets 一一对应。
    std::vector<float> clusterSpheres;
    if (frustumCull_) {
        clusterSpheres.reserve(clusterCount * 4);
        for (const auto& b : nm.bounds) {
            clusterSpheres.push_back(b.sphere[0]);
            clusterSpheres.push_back(b.sphere[1]);
            clusterSpheres.push_back(b.sphere[2]);
            clusterSpheres.push_back(b.sphere[3]);
        }
    }

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

    // M4.1：剔除路径的簇包围球 SSBO + 可见列表 SSBO。
    // visibleList 同时被 compute 写（UAV）和 VS 读（SRV），创建时必须声明
    // canHaveUAVs（NVRHI validation 实测抓出），permanent 态给 UnorderedAccess，
    // 帧内由 NVRHI 状态跟踪在 UAV/SRV 间插转换。
    if (frustumCull_) {
        if (!makeSsbo("CullClusterSpheres",
                      static_cast<std::uint32_t>(clusterSpheres.size() * sizeof(float)), clusterSpheres_))
            { outError = "createBuffer failed for cull cluster spheres"; return false; }
    }
    // M5.1 修潜伏 bug：visibleList 恒创建恒绑定。M4.1 起布局无条件声明 t4，但 set
    // 只在 frustum on 时绑 → --frustum off 一直 init 失败（viewer 默认 on，从未被
    // 用例踩到）。frustum off 时列表内容 = 恒等映射，VS 寻址与 M4.0 直通等价。
    {
        nvrhi::BufferDesc vlDesc;
        vlDesc.byteSize = clusterCount * sizeof(std::uint32_t);
        vlDesc.structStride = 4;
        vlDesc.canHaveUAVs = true;
        // keepInitialState：动态状态跟踪不跨命令列表，每条列表从 CopyDest 重新出发，
        // 帧内 UAV(compute 写)/SRV(VS 读) 动态晋升（NVRHI validation 指路的标准写法）。
        vlDesc.initialState = nvrhi::ResourceStates::CopyDest;
        vlDesc.keepInitialState = true;
        vlDesc.debugName = "CullVisibleList";
        visibleList_ = device->createBuffer(vlDesc);
        if (!visibleList_) { outError = "createBuffer failed for cull visible list"; return false; }
    }

    // indirect args 缓冲（isDrawIndirectArgs）。frustumCull=false 时 instanceCount 恒 =
    // 总簇数（M4.0 行为）；frustumCull=true 时 GPU 每帧经 compute atomicAdd 写 [1]。
    // structStride=4：同一块缓冲在 compute 里以 RWStructuredBuffer<uint> 视角访问
    // （NVRHI validation 要求 UAV 视角必须带 structStride）。
    nvrhi::BufferDesc iaDesc;
    iaDesc.byteSize = sizeof(nvrhi::DrawIndexedIndirectArguments);
    iaDesc.isDrawIndirectArgs = true;
    iaDesc.structStride = 4;
    // frustumCull=true 时 compute 以 RWStructuredBuffer 视角 atomicAdd 写 instanceCount，
    // canHaveUAVs 必须开（NVRHI validation 实测；false 时无害）。
    iaDesc.canHaveUAVs = true;
    iaDesc.debugName = "CullIndirectArgs";
    iaDesc.initialState = nvrhi::ResourceStates::CopyDest;
    // frustumCull 时同帧三态轮转（CopyDest→UAV→IndirectArgument），keepInitialState
    // 让每条命令列表从 CopyDest 重新出发；frustumCull=false 走 M4.0 的 permanent 路径。
    iaDesc.keepInitialState = frustumCull_;
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

    // 绑定布局：t0 占位贴图 + s0 sampler + b0 常量 + t1/t2/t3 三张 SSBO + t4 可见列表。
    // Vulkan 经 -fvk-t-shift 0 0 映射为 binding 1/2/3/4，DX12 为 SRV slot 1/2/3/4。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    cullBindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!cullBindingLayout_) { outError = "createBindingLayout failed for cull"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, vertexPool_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, localIndices_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, clusterMeta_));
    // M5.1：t4 恒绑定（见上方 visibleList 恒创建的注释）。
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, visibleList_));
    cullBindingSet_ = device->createBindingSet(setDesc, cullBindingLayout_);
    if (!cullBindingSet_) { outError = "createBindingSet failed for cull"; return false; }

    // M4.1：cull.slang compute pipeline。binding 与 cull.slang 的寄存器一一对应：
    //   b0 volatile CB / t1 clusterMeta / t2 clusterSpheres / u0 visibleList / u1 args
    //   / u2 triCounter（M5.2 三角计数）。
    // compute pipeline 不依赖 framebuffer 格式，init 时一次建成（graphics pipeline
    // 仍留在 ensurePipelines）。
    if (frustumCull_) {
        // M5.2：GPU 原子三角计数器（1 uint）。与 args 同帧多态轮转（writeBuffer 清零
        // = CopyDest → compute atomicAdd = UAV → copyBuffer 回读 = CopySource），
        // keepInitialState 每条命令列表开头回到 CopyDest（dagGroupExpanded_ 同款纪律）。
        nvrhi::BufferDesc tDesc;
        tDesc.byteSize = sizeof(std::uint32_t);
        tDesc.structStride = sizeof(std::uint32_t);
        tDesc.canHaveUAVs = true;
        tDesc.initialState = nvrhi::ResourceStates::CopyDest;
        tDesc.keepInitialState = true;
        tDesc.debugName = "TriCounter";
        triCounter_ = device->createBuffer(tDesc);
        if (!triCounter_) { outError = "createBuffer failed for tri counter"; return false; }

        auto cullShaderPath = [shaderSuffix](const char* artifact) {
            return std::string(NL_SHADER_DIR) + "/cull." + artifact + "." + shaderSuffix;
        };
        std::vector<std::uint8_t> csBlob;
        if (!readFileBytes(cullShaderPath("cs").c_str(), csBlob)) {
            outError = std::string("failed to load cull compute shader (") + shaderSuffix +
                       ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc csDesc;
        // 入口名传 "main"：SPIR-V 恒 main；DX12 完全忽略 setEntryName、直接用 blob
        // 唯一入口（编译期 -entry csCull 写入）——两后端都正确（§2.4 两阶段规则）。
        csDesc.setShaderType(nvrhi::ShaderType::Compute).setEntryName("main");
        csCull_ = device->createShader(csDesc, csBlob.data(), csBlob.size());
        if (!csCull_) { outError = "createShader failed for cull compute"; return false; }

        nvrhi::BindingLayoutDesc cLayoutDesc;
        cLayoutDesc.setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2));
        cullComputeLayout_ = device->createBindingLayout(cLayoutDesc);
        if (!cullComputeLayout_) { outError = "createBindingLayout failed for cull compute"; return false; }

        nvrhi::BindingSetDesc cSetDesc;
        cSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, clusterMeta_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, clusterSpheres_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visibleList_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, indirectArgs_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, triCounter_));
        cullComputeSet_ = device->createBindingSet(cSetDesc, cullComputeLayout_);
        if (!cullComputeSet_) { outError = "createBindingSet failed for cull compute"; return false; }

        nvrhi::ComputePipelineDesc cpDesc;
        cpDesc.setComputeShader(csCull_).addBindingLayout(cullComputeLayout_);
        cullComputePSO_ = device->createComputePipeline(cpDesc);
        if (!cullComputePSO_) { outError = "createComputePipeline failed for cull"; return false; }

        // 回读环形（M5.2 起 8B）：每帧 compute 后 copyBuffer 两段——[0]=args[1]
        // （可见簇数）、[4]=triCounter[0]（可见三角数），pollCullStats 延迟两帧
        // map（cpuAccess=Read，map 内部等 lastUseFence）。
        for (auto& rb : cullReadback_) {
            nvrhi::BufferDesc rDesc;
            rDesc.byteSize = 2 * sizeof(std::uint32_t);
            rDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
            rDesc.debugName = "CullCountReadback";
            rb = device->createBuffer(rDesc);
            if (!rb) { outError = "createBuffer failed for cull readback"; return false; }
        }
    }

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

    // M4.1：包围球（只读 SRV）+ 可见列表（初始恒等映射；permanent 态 UnorderedAccess，
    // 帧内 compute 写 UAV、VS 读 SRV，NVRHI 状态跟踪负责插转换）。
    if (frustumCull_) {
        upload->beginTrackingBufferState(clusterSpheres_, nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(clusterSpheres_, clusterSpheres.data(),
                            clusterSpheres.size() * sizeof(float));
        upload->setPermanentBufferState(clusterSpheres_, nvrhi::ResourceStates::ShaderResource);
    }
    // M5.1：恒等列表恒上传——frustum off 时 VS 经它直通（M4.0 语义）。
    {
        std::vector<std::uint32_t> identityClusters(clusterCount);
        for (std::uint32_t i = 0; i < clusterCount; ++i) identityClusters[i] = i;
        // M4.1：可见列表不钉 permanent 态——同一帧内 compute 写 UAV、VS 读 SRV、
        // 由状态跟踪动态晋升（permanent 态会报 "doesn't have the right state bits"）。
        upload->beginTrackingBufferState(visibleList_, nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(visibleList_, identityClusters.data(),
                            identityClusters.size() * sizeof(std::uint32_t));
    }

    nvrhi::DrawIndexedIndirectArguments args;
    args.setIndexCount(stride)
        .setInstanceCount(frustumCull_ ? 0u : clusterCount)
        .setStartIndexLocation(0)
        .setBaseVertexLocation(0)
        .setStartInstanceLocation(0);
    // M4.1：剔除开启时每帧用这份模板整体重写 args（instanceCount=0，atomicAdd 从零累加）。
    cullArgsReset_ = args;
    cullArgsReset_.setInstanceCount(0);
    upload->beginTrackingBufferState(indirectArgs_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(indirectArgs_, &args, sizeof(args));
    if (frustumCull_) {
        // M4.1：args 同帧三态轮转（writeBuffer=CopyDest → compute=UAV → draw=
        // IndirectArgument），保持动态跟踪由状态跟踪插转换；钉 permanent 会报错。
        for (auto& rb : cullReadback_) {
            upload->beginTrackingBufferState(rb, nvrhi::ResourceStates::CopyDest);
            upload->setPermanentBufferState(rb, nvrhi::ResourceStates::CopyDest);
        }
        // M5.2：三角计数器初始清零（动态跟踪，与 args 同款轮转）。
        const std::uint32_t triZero = 0;
        upload->beginTrackingBufferState(triCounter_, nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(triCounter_, &triZero, sizeof(triZero));
    } else {
        // M4.0 路径：args 恒 IndirectArgument，钉 permanent 与历史行为一致。
        upload->setPermanentBufferState(indirectArgs_, nvrhi::ResourceStates::IndirectArgument);
    }

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
                        " | frustum=", frustumCull_ ? "on (compute cull pass)" : "off",
                        " | shader=", shaderSuffix);
    return true;
}

// M5.1（S5.1）：GPU DAG 遍历选层场景。声明见 scene.hpp。
// 图形侧与 M4.0/4.1 同构（SSBO vertex pulling + 可见列表 + indirect draw，vsCull/psCull
// 原样复用——多层拼接 meta 已重定基，VS 寻址 clusterId*4 不变），差异只在：
//   1) 数据 = 全部 .lodN.nmesh 层拼接（vertexBase/localBase 重定基）；
//   2) 新增 DAG 拍平 SSBO（误差/层级/生成组/组成员 CSR）；
//   3) compute 换 dag.slang csDag（显式栈遍历，输出仍是 u0/u1 同一接口）。
// 刻意不复用 initCull 的资源（M4 路径冻结保回归），样板代码重复是已接受的代价。
bool Scene::initDag(nvrhi::IDevice* device, const char* shaderSuffix,
                    const char* vsEntry, const char* psEntry,
                    const char* nmeshFile, std::string& outError) {
    frustumCull_ = true;          // 复用 M4 的 indirect + 计时 + 回读路径开关
    cullTimer_.setLabel("dag");   // resolve 日志标明这是 dag 遍历实例

    // ---- 1. shader：vsCull/psCull 与 M4 同一份；compute 读 dag.cs.<suffix> ----
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
    if (!vsCull_ || !psCull_) { outError = "createShader failed for dag graphics shaders"; return false; }

    // ---- 2. 逐层加载 + 拍平拼接 ----
    std::string stem(nmeshFile ? nmeshFile : "");
    const std::string ext = ".nmesh";
    if (stem.size() < ext.size() ||
        stem.compare(stem.size() - ext.size(), ext.size(), ext) != 0) {
        outError = std::string("dag scene expects a .nmesh file, got '") + stem + "'";
        return false;
    }
    stem.resize(stem.size() - ext.size());

    constexpr int kMaxLevels = 16;
    std::vector<nl::core::NMeshData> levels;
    for (int L = 0; L < kMaxLevels; ++L) {
        const std::string path = std::string(NL_MESHLET_DIR "/") + stem + ".lod" +
                                 std::to_string(L) + ".nmesh";
        nl::core::NMeshData nm;
        std::string err;
        if (!nl::core::loadNMesh(path.c_str(), nm, err)) break;   // 连续层到此为止
        if (nm.meshlets.empty() || nm.triangles.empty()) break;
        levels.push_back(std::move(nm));
    }
    if (levels.size() < 2) {
        outError = "dag scene needs >= 2 consecutive <stem>.lodN.nmesh levels under " NL_MESHLET_DIR;
        return false;
    }
    for (std::size_t L = 0; L < levels.size(); ++L) {
        if (!levels[L].lodMeta.dagPresent || levels[L].lodMeta.clusterGroup.size() !=
                                             levels[L].meshlets.size()) {
            outError = stem + ".lod" + std::to_string(L) +
                       ".nmesh has no DAG lineage (meta version 2 required)";
            return false;
        }
    }
    const std::uint32_t dagLevelCount = static_cast<std::uint32_t>(levels.size());

    // 拼接目标数组（M4.0 同布局；vertexBase 重定基，vsCull 寻址不变；
    // localBase 逐簇取 localIndices.size()，天然就是全局地址，无需额外重定基）。
    std::vector<float>         vertexPool;
    std::vector<std::uint32_t> localIndices, clusterMeta;
    std::vector<float>         clusterSpheresF, clusterErrorsF;
    std::vector<float>         groupsF;          // 每组 2×float4：(cx,cy,cz,r),(err,first,count,0)
    std::vector<std::uint32_t> membersF, genGroupF, levelF;
    std::uint32_t gvBase = 0, gcBase = 0, ggBase = 0;
    std::uint32_t maxTriCount = 0, finestTri = 0;
    std::vector<std::uint32_t> levelClusterCount;

    for (std::uint32_t L = 0; L < dagLevelCount; ++L) {
        const auto& nm = levels[L];
        const std::uint32_t lc = static_cast<std::uint32_t>(nm.meshlets.size());
        levelClusterCount.push_back(lc);

        for (std::uint32_t c = 0; c < lc; ++c) {
            const auto& meshlet = nm.meshlets[c];
            if (meshlet.triangleCount > maxTriCount) maxTriCount = meshlet.triangleCount;
            const std::uint32_t localBase = static_cast<std::uint32_t>(localIndices.size());
            const std::uint32_t base = meshlet.triangleOffset;
            for (std::uint32_t t = 0; t < meshlet.triangleCount; ++t)
                for (int k = 0; k < 3; ++k)
                    localIndices.push_back(nm.triangles[base + t * 3 +
                                                        static_cast<std::uint32_t>(k)]);
            clusterMeta.push_back(meshlet.vertexOffset + gvBase);
            clusterMeta.push_back(meshlet.triangleCount);
            clusterMeta.push_back(localBase);
            clusterMeta.push_back(0);
            const auto& b = nm.bounds[c];
            clusterSpheresF.insert(clusterSpheresF.end(), {b.sphere[0], b.sphere[1],
                                                           b.sphere[2], b.sphere[3]});
            clusterErrorsF.push_back(b.error);
            levelF.push_back(L);
        }
        for (const auto& mv : nm.vertices) {
            vertexPool.insert(vertexPool.end(),
                              {mv.pos[0], mv.pos[1], mv.pos[2],
                               mv.normal[0], mv.normal[1], mv.normal[2],
                               mv.uv[0], mv.uv[1]});
        }

        // 组记录 + 成员 CSR。成员方向不保证连续（2026-11-08 资产实测 lod0 全部
        // 31 组非连续），所以拍成显式列表而非区间——bevy cull_bvh 节点同样显式存
        // child_offset+count，同因。
        for (std::uint32_t g = 0; g < nm.lodMeta.dagGroups.size(); ++g) {
            const auto& dg = nm.lodMeta.dagGroups[g];
            groupsF.insert(groupsF.end(), {dg.sphere[0], dg.sphere[1], dg.sphere[2], dg.sphere[3]});
            const std::uint32_t memberFirst = static_cast<std::uint32_t>(membersF.size());
            std::uint32_t memberCount = 0;
            for (std::uint32_t c = 0; c < lc; ++c) {
                if (nm.lodMeta.clusterGroup[c] == g) {
                    membersF.push_back(gcBase + c);
                    ++memberCount;
                }
            }
            // first/count 打包进 float：全局值 < 2^24，float 精确无损（324 量级）。
            groupsF.insert(groupsF.end(),
                           {dg.error, static_cast<float>(memberFirst),
                            static_cast<float>(memberCount), 0.f});
        }

        // 生成方向区间反转：level L 的组（genFirst/genCount 指 L+1 层局部簇号，
        // 不变量实测：恰一生成组、无重叠无遗漏）→ L+1 层每簇的生成组（全局组号）。
        if (L + 1 < dagLevelCount) {
            const std::uint32_t nextBase = gcBase + lc;
            for (std::uint32_t g = 0; g < nm.lodMeta.dagGroups.size(); ++g) {
                const auto& dg = nm.lodMeta.dagGroups[g];
                for (std::uint32_t c = 0; c < dg.genCount; ++c) {
                    const std::uint32_t globalC = nextBase + dg.genFirst + c;
                    if (genGroupF.size() <= globalC) genGroupF.resize(globalC + 1, 0);
                    genGroupF[globalC] = ggBase + g;
                }
            }
        }
        if (L == 0)
            for (const auto& m : nm.meshlets) finestTri += m.triangleCount;

        gvBase += static_cast<std::uint32_t>(nm.vertices.size());
        gcBase += lc;
        ggBase += static_cast<std::uint32_t>(nm.lodMeta.dagGroups.size());
    }
    genGroupF.resize(gcBase, 0);   // 叶层/未写项补 0（遍历只读有父簇的项）
    if (genGroupF.size() != gcBase) { outError = "dag: genGroup size mismatch"; return false; }

    const std::uint32_t totalClusters = gcBase;
    const std::uint32_t stride = maxTriCount * 3;   // identity 索引缓冲长度
    const std::uint32_t groupTotal = ggBase;

    // ---- 3. 建 GPU 资源：图形侧与 initCull 同构（M4.0 样板），DAG 侧另加 ----
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(SceneConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "SceneConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for volatile constant buffer"; return false; }

    nvrhi::BufferDesc dcbDesc = cbDesc;
    dcbDesc.byteSize = 48;   // 3×float4：DagConstants（dag.slang b0）
    dcbDesc.debugName = "DagConstants";
    dagConstants_ = device->createBuffer(dcbDesc);
    if (!dagConstants_) { outError = "createBuffer failed for dag constants"; return false; }

    nvrhi::TextureDesc texDesc;
    texDesc.width = 1; texDesc.height = 1;
    texDesc.format = nvrhi::Format::RGBA8_UNORM;
    texDesc.debugName = "DagWhite1x1";
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    texture_ = device->createTexture(texDesc);
    if (!texture_) { outError = "createTexture failed for dag dummy texture"; return false; }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed"; return false; }

    auto makeSsbo = [&](const char* name, std::uint32_t byteSize, nvrhi::BufferHandle& out) {
        nvrhi::BufferDesc d;
        d.byteSize = byteSize;
        d.structStride = 4;
        d.debugName = name;
        out = device->createBuffer(d);
        return out != nullptr;
    };
    if (!makeSsbo("DagVertexPool",
                  static_cast<std::uint32_t>(vertexPool.size() * sizeof(float)), vertexPool_))
        { outError = "createBuffer failed for dag vertex pool"; return false; }
    if (!makeSsbo("DagLocalIndices",
                  static_cast<std::uint32_t>(localIndices.size() * sizeof(std::uint32_t)), localIndices_))
        { outError = "createBuffer failed for dag local indices"; return false; }
    if (!makeSsbo("DagClusterMeta",
                  static_cast<std::uint32_t>(clusterMeta.size() * sizeof(std::uint32_t)), clusterMeta_))
        { outError = "createBuffer failed for dag cluster meta"; return false; }
    if (!makeSsbo("DagClusterSpheres",
                  static_cast<std::uint32_t>(clusterSpheresF.size() * sizeof(float)), clusterSpheres_))
        { outError = "createBuffer failed for dag cluster spheres"; return false; }
    if (!makeSsbo("DagClusterErrors",
                  static_cast<std::uint32_t>(clusterErrorsF.size() * sizeof(float)), dagClusterErrors_))
        { outError = "createBuffer failed for dag cluster errors"; return false; }
    if (!makeSsbo("DagGroups",
                  static_cast<std::uint32_t>(groupsF.size() * sizeof(float)), dagGroups_))
        { outError = "createBuffer failed for dag groups"; return false; }
    if (!makeSsbo("DagMembers",
                  static_cast<std::uint32_t>(membersF.size() * sizeof(std::uint32_t)), dagMembers_))
        { outError = "createBuffer failed for dag members"; return false; }
    if (!makeSsbo("DagGenGroup",
                  static_cast<std::uint32_t>(genGroupF.size() * sizeof(std::uint32_t)), dagGenGroup_))
        { outError = "createBuffer failed for dag gen group"; return false; }
    if (!makeSsbo("DagClusterLevel",
                  static_cast<std::uint32_t>(levelF.size() * sizeof(std::uint32_t)), dagClusterLevel_))
        { outError = "createBuffer failed for dag cluster level"; return false; }
    // 组展开声明标志：每帧 writeBuffer(CopyDest) 清零 → compute UAV(atomicExchange)，
    // 状态由 NVRHI 动态跟踪插转换（visibleList 同款三态纪律）。
    {
        nvrhi::BufferDesc exDesc;
        exDesc.byteSize = groupTotal * sizeof(std::uint32_t);
        exDesc.structStride = 4;
        exDesc.canHaveUAVs = true;
        exDesc.initialState = nvrhi::ResourceStates::CopyDest;
        exDesc.keepInitialState = true;
        exDesc.debugName = "DagGroupExpanded";
        dagGroupExpanded_ = device->createBuffer(exDesc);
    }
    if (!dagGroupExpanded_) { outError = "createBuffer failed for dag group expanded"; return false; }

    // 可见列表：容量 = 全层簇总和（不变量：每簇至多被 emit 一次，不可能越界）。
    nvrhi::BufferDesc vlDesc;
    vlDesc.byteSize = totalClusters * sizeof(std::uint32_t);
    vlDesc.structStride = 4;
    vlDesc.canHaveUAVs = true;
    vlDesc.initialState = nvrhi::ResourceStates::CopyDest;
    vlDesc.keepInitialState = true;
    vlDesc.debugName = "DagVisibleList";
    visibleList_ = device->createBuffer(vlDesc);
    if (!visibleList_) { outError = "createBuffer failed for dag visible list"; return false; }

    nvrhi::BufferDesc iaDesc;
    iaDesc.byteSize = sizeof(nvrhi::DrawIndexedIndirectArguments);
    iaDesc.isDrawIndirectArgs = true;
    iaDesc.structStride = 4;
    iaDesc.canHaveUAVs = true;
    iaDesc.debugName = "DagIndirectArgs";
    iaDesc.initialState = nvrhi::ResourceStates::CopyDest;
    iaDesc.keepInitialState = true;    // 同帧三态轮转（M4.1 同款）
    indirectArgs_ = device->createBuffer(iaDesc);
    if (!indirectArgs_) { outError = "createBuffer failed for dag indirect args"; return false; }

    std::vector<std::uint32_t> identity(stride);
    for (std::uint32_t i = 0; i < stride; ++i) identity[i] = i;
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = identity.size() * sizeof(std::uint32_t);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "DagIdentityIndex";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    identityIndex_ = device->createBuffer(ibDesc);
    if (!identityIndex_) { outError = "createBuffer failed for dag identity index"; return false; }

    // 图形绑定（t0 贴图 + s0 sampler + b0 常量 + t1/t2/t3 SSBO + t4 可见列表）——M4 同构。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    cullBindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!cullBindingLayout_) { outError = "createBindingLayout failed for dag"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, vertexPool_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, localIndices_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, clusterMeta_))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, visibleList_));
    cullBindingSet_ = device->createBindingSet(setDesc, cullBindingLayout_);
    if (!cullBindingSet_) { outError = "createBindingSet failed for dag"; return false; }

    // DAG 遍历 compute 绑定（dag.slang 寄存器一一对应）：
    //   b0 DagConstants / t1 spheres / t2 errors / t3 groups / t4 members /
    //   t5 genGroup / t6 levels / t7 clusterMeta（M5.2）/ u0 visibleList / u1 args /
    //   u2 expanded / u3 triCounter（M5.2）。
    // M5.2：GPU 原子三角计数器（与 initCull 同款；dag 模式不调 initCull，各自建）。
    nvrhi::BufferDesc tDesc;
    tDesc.byteSize = sizeof(std::uint32_t);
    tDesc.structStride = sizeof(std::uint32_t);
    tDesc.canHaveUAVs = true;
    tDesc.initialState = nvrhi::ResourceStates::CopyDest;
    tDesc.keepInitialState = true;
    tDesc.debugName = "DagTriCounter";
    triCounter_ = device->createBuffer(tDesc);
    if (!triCounter_) { outError = "createBuffer failed for dag tri counter"; return false; }

    nvrhi::BindingLayoutDesc dLayoutDesc;
    dLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
    dagComputeLayout_ = device->createBindingLayout(dLayoutDesc);
    if (!dagComputeLayout_) { outError = "createBindingLayout failed for dag compute"; return false; }

    nvrhi::BindingSetDesc dSetDesc;
    dSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, dagConstants_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, clusterSpheres_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, dagClusterErrors_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, dagGroups_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, dagMembers_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, dagGenGroup_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, dagClusterLevel_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, clusterMeta_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visibleList_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, indirectArgs_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, dagGroupExpanded_))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, triCounter_));
    dagComputeSet_ = device->createBindingSet(dSetDesc, dagComputeLayout_);
    if (!dagComputeSet_) { outError = "createBindingSet failed for dag compute"; return false; }

    {
        auto dagShaderPath = [shaderSuffix](const char* artifact) {
            return std::string(NL_SHADER_DIR) + "/dag." + artifact + "." + shaderSuffix;
        };
        std::vector<std::uint8_t> csBlob;
        if (!readFileBytes(dagShaderPath("cs").c_str(), csBlob)) {
            outError = std::string("failed to load dag compute shader (") + shaderSuffix +
                       ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc csDesc;
        // 入口名 "main"：SPIR-V 恒 main；DX12 忽略（§2.4 两阶段规则，M4.1 同款）。
        csDesc.setShaderType(nvrhi::ShaderType::Compute).setEntryName("main");
        csDag_ = device->createShader(csDesc, csBlob.data(), csBlob.size());
        if (!csDag_) { outError = "createShader failed for dag compute"; return false; }
        nvrhi::ComputePipelineDesc cpDesc;
        cpDesc.setComputeShader(csDag_).addBindingLayout(dagComputeLayout_);
        dagComputePSO_ = device->createComputePipeline(cpDesc);
        if (!dagComputePSO_) { outError = "createComputePipeline failed for dag"; return false; }
    }

    // 回读环形（M4.1 同款，M5.2 起 8B：[0]=可见簇数 [4]=可见三角数，dag 遍历同样
    // 每帧回读）。
    for (auto& rb : cullReadback_) {
        nvrhi::BufferDesc rDesc;
        rDesc.byteSize = 2 * sizeof(std::uint32_t);
        rDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        rDesc.debugName = "DagCountReadback";
        rb = device->createBuffer(rDesc);
        if (!rb) { outError = "createBuffer failed for dag readback"; return false; }
    }

    // ---- 4. 一次性上传（M4.0 同款状态纪律）----
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
    upload->beginTrackingBufferState(clusterSpheres_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(clusterSpheres_, clusterSpheresF.data(),
                        clusterSpheresF.size() * sizeof(float));
    upload->setPermanentBufferState(clusterSpheres_, nvrhi::ResourceStates::ShaderResource);
    upload->beginTrackingBufferState(dagClusterErrors_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(dagClusterErrors_, clusterErrorsF.data(),
                        clusterErrorsF.size() * sizeof(float));
    upload->setPermanentBufferState(dagClusterErrors_, nvrhi::ResourceStates::ShaderResource);
    upload->beginTrackingBufferState(dagGroups_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(dagGroups_, groupsF.data(), groupsF.size() * sizeof(float));
    upload->setPermanentBufferState(dagGroups_, nvrhi::ResourceStates::ShaderResource);
    upload->beginTrackingBufferState(dagMembers_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(dagMembers_, membersF.data(), membersF.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(dagMembers_, nvrhi::ResourceStates::ShaderResource);
    upload->beginTrackingBufferState(dagGenGroup_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(dagGenGroup_, genGroupF.data(), genGroupF.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(dagGenGroup_, nvrhi::ResourceStates::ShaderResource);
    upload->beginTrackingBufferState(dagClusterLevel_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(dagClusterLevel_, levelF.data(), levelF.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(dagClusterLevel_, nvrhi::ResourceStates::ShaderResource);
    std::vector<std::uint32_t> expandedZeros(groupTotal, 0);
    upload->beginTrackingBufferState(dagGroupExpanded_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(dagGroupExpanded_, expandedZeros.data(), expandedZeros.size() * sizeof(std::uint32_t));

    std::vector<std::uint32_t> identityClusters(totalClusters);
    for (std::uint32_t i = 0; i < totalClusters; ++i) identityClusters[i] = i;
    upload->beginTrackingBufferState(visibleList_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(visibleList_, identityClusters.data(),
                        identityClusters.size() * sizeof(std::uint32_t));

    nvrhi::DrawIndexedIndirectArguments args;
    args.setIndexCount(stride)
        .setInstanceCount(0u)
        .setStartIndexLocation(0)
        .setBaseVertexLocation(0)
        .setStartInstanceLocation(0);
    cullArgsReset_ = args;   // dag 遍历每帧同款重写（instanceCount 由 GPU atomicAdd 重建）
    upload->beginTrackingBufferState(indirectArgs_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(indirectArgs_, &args, sizeof(args));
    for (auto& rb : cullReadback_) {
        upload->beginTrackingBufferState(rb, nvrhi::ResourceStates::CopyDest);
        upload->setPermanentBufferState(rb, nvrhi::ResourceStates::CopyDest);
    }
    // M5.2：三角计数器初始清零（动态跟踪，writeBuffer/UAV/CopySource 轮转）。
    const std::uint32_t dagTriZero = 0;
    upload->beginTrackingBufferState(triCounter_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(triCounter_, &dagTriZero, sizeof(dagTriZero));

    upload->beginTrackingBufferState(identityIndex_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(identityIndex_, identity.data(), identity.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(identityIndex_, nvrhi::ResourceStates::IndexBuffer);
    upload->close();
    device->executeCommandList(upload);

    // ---- 5. 状态落位 ----
    cull_ = true;
    dag_ = true;
    clusterCount_ = totalClusters;
    dagRootCount_ = levelClusterCount.back();
    dagLevelCount_ = dagLevelCount;
    dagGroupCount_ = groupTotal;
    modelName_ = nmeshFile;
    triangleCount_ = finestTri;
    // M5.0 投影球：dag 模式给最粗层联合球（HUD s5 err 行的数据源口径不变）。
    {
        const auto& top = levels.back();
        float minx = 1e30f, miny = 1e30f, minz = 1e30f;
        float maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;
        for (const auto& mv : top.vertices) {
            minx = std::min(minx, mv.pos[0]); maxx = std::max(maxx, mv.pos[0]);
            miny = std::min(miny, mv.pos[1]); maxy = std::max(maxy, mv.pos[1]);
            minz = std::min(minz, mv.pos[2]); maxz = std::max(maxz, mv.pos[2]);
        }
        const float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f,
                    cz = (minz + maxz) * 0.5f;
        float r2 = 0.f;
        for (const auto& mv : top.vertices) {
            const float dx = mv.pos[0] - cx, dy = mv.pos[1] - cy, dz = mv.pos[2] - cz;
            r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
        }
        assetSphere_[0] = cx; assetSphere_[1] = cy; assetSphere_[2] = cz;
        assetSphere_[3] = std::sqrt(r2);
    }

    nl::core::log::info("dag (traversal) scene ready: ", stem, ".lod0..lod",
                        dagLevelCount - 1,
                        " | levels=", dagLevelCount,
                        " | clusters=", totalClusters,
                        " | groups=", groupTotal,
                        " | roots=", dagRootCount_,
                        " | triangles(finest)=", triangleCount_,
                        " | stride=", stride,
                        " | shader=", shaderSuffix);
    return true;
}

// M4.1：消费两帧前提交的 instanceCount 回读（frame-in-flight=2 的延迟窗口——
// 本帧绘制会在同一个环形槽写入新值，但那发生在本函数读完成之后，无竞争）。
// mapBuffer 内部等待 lastUseFence（DX12 d3d12-buffer.cpp / Vulkan 同构），两帧
// 延迟下正常零等待，不构成主循环里的等待型 map。
void Scene::pollCullTimer(nvrhi::IDevice* device, HudFrame* hud) {
    if (!cull_ || !frustumCull_ || !hud) return;
    // M4.2：cull pass 计时轮询。必须在 hud_.build() 之前调用（viewer 每帧重建
    // HudFrame，晚于 build 写标志 = 永远显示不出来，M4.2 实测踩坑）。
    if (cullTimer_.ensure(device)) {
        float cullMs = 0.0f;
        if (!hud->deterministic && cullTimer_.poll(cullMs)) hud->cullMs = cullMs;
        hud->cullTimingAvailable = true;
    }
}
void Scene::pollCullStats(nvrhi::IDevice* device) {
    if (!cull_ || !frustumCull_) return;
    if (cullSubmitted_ < 2) return;                       // 窗口未填满，尚无数据
    const std::uint32_t ready = cullSubmitted_ - 2;
    if (cullPolledSlot_ >= 0 && static_cast<std::uint32_t>(cullPolledSlot_) >= ready) return;
    const std::uint32_t slot = ready & 1u;
    void* p = device->mapBuffer(cullReadback_[slot], nvrhi::CpuAccessMode::Read);
    if (p) {
        // M5.2：槽位 8B —— [0]=可见簇数（args[1]）、[4]=可见三角数（triCounter[0]）。
        const auto* vals = static_cast<const std::uint32_t*>(p);
        cullVisibleCount_ = static_cast<int>(vals[0]);
        visibleTriangles_ = static_cast<int>(vals[1]);
        device->unmapBuffer(cullReadback_[slot]);
    } else {
        cullVisibleCount_ = -1;   // map 失败如实上报，不静默沿用旧值
        visibleTriangles_ = -1;
    }
    cullPolledSlot_ = static_cast<int>(ready);
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
    dagGroupSphere_[0] = dagGroupSphere_[1] = dagGroupSphere_[2] = dagGroupSphere_[3] = 0.0f;
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
    for (int i = 0; i < 4; ++i) dagGroupSphere_[i] = group.sphere[i];
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

    // M5.0：显示几何的联合包围球（模型空间 == 世界空间，model 恒为单位阵）。
    // 中心 = 包围盒中心、半径 = 中心到最远顶点；S5 屏幕误差投影的默认球数据源
    //（下钻时 viewer 优先用组记录球 dagGroupSphere_，更贴组原子 cut 语义）。
    if (!vertices.empty()) {
        float minX = vertices[0].v.px, minY = vertices[0].v.py, minZ = vertices[0].v.pz;
        float maxX = minX, maxY = minY, maxZ = minZ;
        for (const auto& v : vertices) {
            if (v.v.px < minX) minX = v.v.px;
            if (v.v.py < minY) minY = v.v.py;
            if (v.v.pz < minZ) minZ = v.v.pz;
            if (v.v.px > maxX) maxX = v.v.px;
            if (v.v.py > maxY) maxY = v.v.py;
            if (v.v.pz > maxZ) maxZ = v.v.pz;
        }
        const float cx = 0.5f * (minX + maxX);
        const float cy = 0.5f * (minY + maxY);
        const float cz = 0.5f * (minZ + maxZ);
        float r2 = 0.0f;
        for (const auto& v : vertices) {
            const float dx = v.v.px - cx, dy = v.v.py - cy, dz = v.v.pz - cz;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) r2 = d2;
        }
        assetSphere_[0] = cx;
        assetSphere_[1] = cy;
        assetSphere_[2] = cz;
        assetSphere_[3] = std::sqrt(r2);
    } else {
        assetSphere_[0] = assetSphere_[1] = assetSphere_[2] = assetSphere_[3] = 0.0f;
    }
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
        // M4.1：cull.slang 从 g_options.y 读总簇数（整数 ≤2^24 float 精确无损）。
        if (frustumCull_) cb.options[1] = static_cast<float>(clusterCount_);
        cmd->writeBuffer(constants_, &cb, sizeof(cb));

        nvrhi::GraphicsPipelineHandle pipeline = params.wireframe
            ? (params.cullBack ? pipelineCullWireBack_ : pipelineCullWireNone_)
            : (params.cullBack ? pipelineCullBack_ : pipelineCullNone_);

        // M4.1：剔除 compute pass（bevy cull_clusters.wesl 蓝本）。每帧：
        //   1) CPU 整体重写 args（instanceCount=0，静态字段不变——atomicAdd 从 0 累加）；
        //   2) 静态 dispatch ceil(簇数/128)，每线程 1 簇 5 平面球测试，通过者
        //      atomicAdd(args[1]) + append 簇号到可见列表；
        //   3) copyBuffer 把 args[1] 拷进回读环形（pollCullStats 延迟两帧消费）；
        //      M5.2 起再拷 triCounter[0] 到槽位偏移 4（同帧同环形）；
        //   4) graphics 读 args + 可见列表，一次 drawIndexedIndirect。
        // UAV→SRV/IndirectArgument 的状态转换由 NVRHI 状态跟踪插入（validation 实测）。
        // M4.2：cull pass 单独计时——begin/end 只包剔除段（args 重写 + dispatch +
        // 回读拷贝），整帧计时仍由 backend 的 gpuTimer_ 负责。query 未建（--no-hud）
        // 时 begin 返回 false，本帧不计时、不报错（与整帧计时同款静默降级）。
        if (frustumCull_) {
            const bool cullTiming = cullTimer_.begin(cmd);
            cmd->writeBuffer(indirectArgs_, &cullArgsReset_, sizeof(cullArgsReset_));
            // M5.2：三角计数器每帧清零（GPU atomicAdd 从 0 累加，与 args[1] 同款）。
            const std::uint32_t triZero = 0;
            cmd->writeBuffer(triCounter_, &triZero, sizeof(triZero));

            // M5.1：dag 遍历换 dag.slang csDag（显式栈）+ 自己的绑定/常量；
            // M4.1 路径保持原样。输出接口（visibleList + args）两者完全一致。
            if (dag_) {
                const float dc[12] = {
                    params.projYScale, params.viewportHeight, params.nearPlane,
                    params.cutThresholdPx,
                    params.eye[0], params.eye[1], params.eye[2],
                    static_cast<float>(dagRootCount_),
                    // 根簇全局基址 = 总簇数 − 最粗层簇数（最粗层拼在最后）。
                    static_cast<float>(clusterCount_ - dagRootCount_), 0.f, 0.f, 0.f};
                cmd->writeBuffer(dagConstants_, dc, sizeof(dc));
                // 组展开声明标志每帧清零（遍历 compute 的 atomicExchange 从干净态出发）。
                std::uint32_t zeros[64] = {};
                std::uint32_t cleared = 0;
                while (cleared < dagGroupCount_) {
                    const std::uint32_t n = std::min<std::uint32_t>(64u, dagGroupCount_ - cleared);
                    cmd->writeBuffer(dagGroupExpanded_, zeros, n * sizeof(std::uint32_t),
                                     cleared * sizeof(std::uint32_t));
                    cleared += n;
                }
            }

            nvrhi::ComputeState cstate;
            cstate.setPipeline(dag_ ? dagComputePSO_ : cullComputePSO_)
                  .addBindingSet(dag_ ? dagComputeSet_ : cullComputeSet_);
            cmd->setComputeState(cstate);
            const std::uint32_t threads = dag_ ? dagRootCount_ : clusterCount_;
            cmd->dispatch((threads + 127u) / 128u, 1, 1);

            // 回读拷贝两段：槽位 [0]=args[1]（可见簇数，offset 4 字节处）、
            // [4]=triCounter[0]（可见三角数）。pollCullStats 延迟两帧一起消费。
            cmd->copyBuffer(cullReadback_[cullSubmitted_ & 1u], 0,
                            indirectArgs_, sizeof(std::uint32_t), sizeof(std::uint32_t));
            cmd->copyBuffer(cullReadback_[cullSubmitted_ & 1u], sizeof(std::uint32_t),
                            triCounter_, 0, sizeof(std::uint32_t));
            if (cullTiming) cullTimer_.end(cmd);
            ++cullSubmitted_;
        }

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
    cullComputeSet_ = nullptr;
    cullComputeLayout_ = nullptr;
    cullComputePSO_ = nullptr;
    csCull_ = nullptr;
    // M5.1：DAG 遍历资源。
    dag_ = false;
    dagRootCount_ = 0;
    dagLevelCount_ = 0;
    dagConstants_ = nullptr;
    dagClusterErrors_ = nullptr;
    dagGroups_ = nullptr;
    dagMembers_ = nullptr;
    dagGenGroup_ = nullptr;
    dagClusterLevel_ = nullptr;
    dagGroupExpanded_ = nullptr;
    csDag_ = nullptr;
    dagComputePSO_ = nullptr;
    dagComputeLayout_ = nullptr;
    dagComputeSet_ = nullptr;
    visibleList_ = nullptr;
    clusterSpheres_ = nullptr;
    for (auto& rb : cullReadback_) rb = nullptr;
    triCounter_ = nullptr;   // M5.2：GPU 原子三角计数器
    cullSubmitted_ = 0;
    cullPolledSlot_ = -1;
    cullVisibleCount_ = -1;
    visibleTriangles_ = -1;
    cullTimer_.shutdown();   // M4.2：device 仍存活时释放 cull 计时 query
    clusterCount_ = 0;
    frustumCull_ = false;
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
