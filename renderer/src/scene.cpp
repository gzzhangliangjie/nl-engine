#include "scene.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）

#include "nl/core/log.hpp"
#include "nl/core/mesh_io.hpp"     // M1.0：glTF 解析抽到 core，与 nanite-tools 共用
#include "nl/core/meshlet_io.hpp"  // M1.3：cluster 场景吃 .nmesh 资产

#include <cmath>
#include <algorithm>
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

    // pbr-direct：metallicRoughness 贴图（t1）。RGBA8_UNORM——MR 是线性数据
    // （G=roughness B=metallic，glTF 2.0 spec §3.9.3），千万别学 baseColor 上
    // SRGB 视图格式。无 MR 贴图的材质退 1x1 dummy（roughness=1, metallic=0）。
    pbrBaseColorFactor_[0] = prim.baseColorFactor[0];
    pbrBaseColorFactor_[1] = prim.baseColorFactor[1];
    pbrBaseColorFactor_[2] = prim.baseColorFactor[2];
    pbrBaseColorFactor_[3] = prim.baseColorFactor[3];
    pbrMetallicFactor_  = prim.metallicFactor;
    pbrRoughnessFactor_ = prim.roughnessFactor;
    {
        nvrhi::TextureDesc mrDesc;
        mrDesc.format = nvrhi::Format::RGBA8_UNORM;
        mrDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        mrDesc.keepInitialState = true;
        if (prim.hasMR) {
            mrDesc.width = prim.mrWidth;
            mrDesc.height = prim.mrHeight;
        } else {
            mrDesc.width = 1;
            mrDesc.height = 1;
        }
        const std::string mrName = std::string(modelFile) + "MetallicRoughness";
        mrDesc.debugName = mrName.c_str();
        mrTexture_ = device->createTexture(mrDesc);
        if (!mrTexture_) { outError = "createTexture failed for metallicRoughness texture"; return false; }
    }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed"; return false; }

    // 布局槽位与 model.slang 的 register(b0/t0/t1/s0/b0/b1) 一致；
    // Vulkan 侧由 NVRHI 默认 VulkanBindingOffsets 映射为 CB=256 / SRV=0 / Sampler=128，
    // 与 slangc 的 -fvk-b-shift 256 -fvk-s-shift 128 严格对应。b1 = LightConstants
    //（slot 1，pbr-direct 新开；SceneConstants 160 B 老规矩不动）。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    bindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!bindingLayout_) { outError = "createBindingLayout failed"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture_))
           .addItem(nvrhi::BindingSetItem::Texture_SRV(1, mrTexture_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_));
    if (!lightConstants_) {
        nvrhi::BufferDesc lcDesc;
        lcDesc.byteSize = sizeof(LightConstants);
        lcDesc.isConstantBuffer = true;
        lcDesc.isVolatile = true;
        lcDesc.maxVersions = 16;
        lcDesc.debugName = "LightConstants";
        lightConstants_ = device->createBuffer(lcDesc);
        if (!lightConstants_) { outError = "createBuffer failed for light constants"; return false; }
    }
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, lightConstants_));
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
    // pbr-direct：MR 贴图同一命令列表上传（线性数据，无 gamma 语义）。
    if (prim.hasMR) {
        upload->writeTexture(mrTexture_, 0, 0, prim.metallicRoughnessRGBA.data(),
                             static_cast<size_t>(prim.mrWidth) * 4u);
    } else {
        const std::uint8_t dummyMR[4] = {0u, 255u, 0u, 255u};   // roughness=1, metallic=0
        upload->writeTexture(mrTexture_, 0, 0, dummyMR, 4u);
    }
    upload->close();
    device->executeCommandList(upload);

    // M0.7：模型名与三角形数带回给 backend（DeviceInfo）→ viewer（HUD 自证）。
    modelName_ = modelFile;
    triangleCount_ = indexCount_ / 3;

    nl::core::log::info("model ready: ", modelFile, " | vertices=", vertexCount,
                        " triangles=", triangleCount_, " indices=", indexCount_,
                        " | baseColor ", texWidth, "x", texHeight,
                        " | mr ", (prim.hasMR ? "tex" : "dummy"),
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
    maxTriPerCluster_ = maxTriCount;                    // M7.0：visbuf triId 8 位断言

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
    dcbDesc.byteSize = 128;  // 4×float4 + float4x4：DagConstants（dag.slang b0）
                             //   M6.1 扩：g_dagHiz（开关/视口宽）+ g_dagView（球投影）
    dcbDesc.debugName = "DagConstants";
    dagConstants_ = device->createBuffer(dcbDesc);
    if (!dagConstants_) { outError = "createBuffer failed for dag constants"; return false; }

    // M6.1：dag.slang t8 占位纹理（1x1 R32F）。非 hiz 模式 / 金字塔未就绪时绑定
    // 它 + g_dagHiz.x=0，shader 侧直接跳过遮挡测试（分支预测友好，无采样代价风险）。
    nvrhi::TextureDesc hdtDesc;
    hdtDesc.width = 1; hdtDesc.height = 1;
    hdtDesc.format = nvrhi::Format::R32_FLOAT;
    hdtDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    hdtDesc.keepInitialState = true;
    hdtDesc.debugName = "HizDummy1x1";
    hizDummyTex_ = device->createTexture(hdtDesc);
    if (!hizDummyTex_) { outError = "createTexture failed for hiz dummy"; return false; }

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
    //   t5 genGroup / t6 levels / t7 clusterMeta（M5.2）/ t8 hiz 金字塔（M6.1）/
    //   u0 visibleList / u1 args / u2 expanded / u3 triCounter（M5.2）。
    // M5.2：GPU 原子计数器（与 initCull 同款；dag 模式不调 initCull，各自建）。
    // M6.2：扩到 2 uint —— [0] = 可见三角数，[1] = 被 Hi-Z 剔除的簇节点数。
    nvrhi::BufferDesc tDesc;
    tDesc.byteSize = 2 * sizeof(std::uint32_t);
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
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(8))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
    dagComputeLayout_ = device->createBindingLayout(dLayoutDesc);
    if (!dagComputeLayout_) { outError = "createBindingLayout failed for dag compute"; return false; }

    // 两个 set：关闭态（t8=dummy）与 hiz 态（t8=真金字塔，ensureHizTargets 建）。
    // §2.7：每管线单一 BindingSet——两个 set 内容互斥，每帧只绑一个，不并存。
    {
        nvrhi::BindingSetDesc dSetDesc;
        dSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, dagConstants_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, clusterSpheres_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, dagClusterErrors_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, dagGroups_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, dagMembers_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, dagGenGroup_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, dagClusterLevel_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, clusterMeta_))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(8, hizDummyTex_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visibleList_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, indirectArgs_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, dagGroupExpanded_))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, triCounter_));
        dagComputeSet_ = device->createBindingSet(dSetDesc, dagComputeLayout_);
        if (!dagComputeSet_) { outError = "createBindingSet failed for dag compute"; return false; }
    }

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

    // 回读环形（M4.1 同款，M5.2 起 8B；M6.2 起 16B：[0]=可见簇数 [4]=可见三角数
    // [8]=被 Hi-Z 剔除簇节点数，dag 遍历同样每帧回读；hiz 模式 ensureHizTargets
    // 重建时再加 [12]=金字塔最高 mip）。
    for (auto& rb : cullReadback_) {
        nvrhi::BufferDesc rDesc;
        rDesc.byteSize = 4 * sizeof(std::uint32_t);
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
    // M6.2：两个槽（tris + occluded）一起清。
    const std::uint32_t dagTriZero[2] = {0, 0};
    upload->beginTrackingBufferState(triCounter_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(triCounter_, dagTriZero, sizeof(dagTriZero));

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

// M6.0（S6.0）：Hi-Z 深度预通道 + 金字塔（M6.1 拍板：max 最远表面口径）。
// 声明见 scene.hpp。
bool Scene::initHiz(nvrhi::IDevice* device, const char* shaderSuffix,
                    const char* vsEntry, const char* psEntry,
                    const char* nmeshFile, std::string& outError) {
    // S5 遍历场景原样建好——hiz 深度 pass 就画它的 indirect 输出（S6-PLAN 底线：
    // 第二遍剔除的输入接口 = S4 indirect buffer，不另起炉灶）。
    if (!initDag(device, shaderSuffix, vsEntry, psEntry, nmeshFile, outError)) return false;
    hiz_ = true;
    hizDevice_ = device;
    cullTimer_.setLabel("hiz");   // 计时段 = 遍历 + 深度预通道 + 金字塔（resolve 日志区分）

    auto hizPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/hiz." + artifact + "." + shaderSuffix;
    };
    auto loadShader = [&](const char* artifact, nvrhi::ShaderType type,
                          nvrhi::ShaderHandle& out) -> bool {
        std::vector<std::uint8_t> blob;
        if (!readFileBytes(hizPath(artifact).c_str(), blob)) {
            outError = std::string("failed to load hiz shader ") + artifact +
                       " (" + shaderSuffix + ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc desc;
        // 入口名 "main"：SPIR-V 恒 main；DX12 忽略 setEntryName（§2.4 两阶段规则）。
        desc.setShaderType(type).setEntryName("main");
        out = device->createShader(desc, blob.data(), blob.size());
        if (!out) outError = std::string("createShader failed for hiz ") + artifact;
        return out != nullptr;
    };
    if (!loadShader("vshizdepth", nvrhi::ShaderType::Vertex, vsHizDepth_) ||
        !loadShader("pshizdepth", nvrhi::ShaderType::Pixel, psHizDepth_) ||
        !loadShader("vshizviz", nvrhi::ShaderType::Vertex, vsHizViz_) ||
        !loadShader("pshizviz", nvrhi::ShaderType::Pixel, psHizViz_) ||
        !loadShader("cs", nvrhi::ShaderType::Compute, csHizDown_) ||
        !loadShader("cstop", nvrhi::ShaderType::Compute, csHizTop_)) {
        return false;
    }

    // HizConstants（hiz.slang b1，48B）。每帧写入次数 = 预通道 1 + 金字塔逐级
    // （≤10）+ top 1 + 可视化 1 ≈ 13 次，× frame-in-flight 2 → maxVersions 64 富余。
    nvrhi::BufferDesc hcDesc;
    hcDesc.byteSize = 48;
    hcDesc.isConstantBuffer = true;
    hcDesc.isVolatile = true;
    hcDesc.maxVersions = 64;
    hcDesc.debugName = "HizConstants";
    hizConstants_ = device->createBuffer(hcDesc);
    if (!hizConstants_) { outError = "createBuffer failed for hiz constants"; return false; }

    // 金字塔最高 mip 值落 4B 计数器：每帧 CPU 清零（0xFFFFFFFF=asfloat NaN）→
    // csHizTop 覆写 → copyBuffer 进回读环形槽位偏移 8（pollCullStats 延迟两帧）。
    // 状态口径与 triCounter_（M5.2）完全同款：永久 CopyDest，UAV/CopySource 用途
    // 不做状态迁移——双后端验证过的模式，勿改动态跟踪（会报 Unknown prior state）。
    nvrhi::BufferDesc htDesc;
    htDesc.byteSize = sizeof(std::uint32_t);
    htDesc.structStride = sizeof(std::uint32_t);
    htDesc.canHaveUAVs = true;
    htDesc.initialState = nvrhi::ResourceStates::CopyDest;
    htDesc.keepInitialState = true;
    htDesc.debugName = "HizTopCounter";
    hizTop_ = device->createBuffer(htDesc);
    if (!hizTop_) { outError = "createBuffer failed for hiz top counter"; return false; }

    // 预通道绑定集（单一 set，覆盖 vsHizDepth/psHizDepth 的全部资源需求）：
    // b0 SceneConstants（mvp）+ b1 HizConstants + t1..t4 vertex pulling SSBO。
    // 【教训】本项目 slangc 统一把资源编进 vk set 0（-fvk-b-shift 256 0），而
    // NVRHI Vulkan 的 legacy 模式会把多个 BindingLayout 拆成多个 vk descriptor
    // set——prepass 若拆两套（cullBindingSet_ + b1），b1 落在 vk set 1 而 shader
    // 在 set 0 找它 → 未绑定描述符读出 NaN（M6.0 首跑 mip0 模型像素 = NaN 的根因；
    // DX12 无 set 概念侥幸不炸）。管线要用几个 NVRHI 绑定集，必须保证资源都
    // 归进一个 set（本项目口径：每管线单 set）。
    nvrhi::BindingLayoutDesc hpLayoutDesc;
    hpLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    hizPrepassLayout_ = device->createBindingLayout(hpLayoutDesc);
    if (!hizPrepassLayout_) { outError = "createBindingLayout failed for hiz prepass"; return false; }
    {
        nvrhi::BindingSetDesc hpSetDesc;
        hpSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
                 .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, hizConstants_))
                 .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, vertexPool_))
                 .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, localIndices_))
                 .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, clusterMeta_))
                 .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, visibleList_));
        hizPrepassSet_ = device->createBindingSet(hpSetDesc, hizPrepassLayout_);
    }
    if (!hizPrepassSet_) { outError = "createBindingSet failed for hiz prepass"; return false; }

    // 金字塔降采样 compute：SRV(0) = mip L-1 单 mip 子资源 + UAV(0) = mip L + b1。
    // 绑定集依赖纹理（尺寸随视口），在 ensureHizTargets 懒建；layout/PSO 此处建。
    nvrhi::BindingLayoutDesc chLayoutDesc;
    chLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    chainLayout_ = device->createBindingLayout(chLayoutDesc);
    if (!chainLayout_) { outError = "createBindingLayout failed for hiz chain"; return false; }
    {
        nvrhi::ComputePipelineDesc chPsoDesc;
        chPsoDesc.setComputeShader(csHizDown_).addBindingLayout(chainLayout_);
        chainPSO_ = device->createComputePipeline(chPsoDesc);
    }
    if (!chainPSO_) { outError = "createComputePipeline failed for hiz chain"; return false; }

    // top 回读 compute：SRV(0) = 最高 mip + StructuredBuffer_UAV(0) = 计数器 + b1。
    nvrhi::BindingLayoutDesc tpLayoutDesc;
    tpLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    topLayout_ = device->createBindingLayout(tpLayoutDesc);
    if (!topLayout_) { outError = "createBindingLayout failed for hiz top"; return false; }
    {
        nvrhi::ComputePipelineDesc tpPsoDesc;
        tpPsoDesc.setComputeShader(csHizTop_).addBindingLayout(topLayout_);
        topPSO_ = device->createComputePipeline(tpPsoDesc);
    }
    if (!topPSO_) { outError = "createComputePipeline failed for hiz top"; return false; }

    // 可视化绑定：SRV(0) 全 mip（psHizViz 用绝对 mip 号 Load）+ b1。
    nvrhi::BindingLayoutDesc vzLayoutDesc;
    vzLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    vizLayout_ = device->createBindingLayout(vzLayoutDesc);
    if (!vizLayout_) { outError = "createBindingLayout failed for hiz viz"; return false; }
    // 可视化绑定集也依赖纹理 → ensureHizTargets 懒建。

    // 回读环形槽位扩到 16B（M6.2）：[0]=可见簇数 [4]=三角数 [8]=被 Hi-Z 剔除簇
    // 节点数 [12]=asuint(最高 mip 距离)。initDag 已建同尺寸版（M6.2 起 4 uint），
    // 这里重建（旧句柄引用计数释放）。重建的是新 NVRHI 对象，必须补状态：永久
    // CopyDest（只当拷贝目标 + CPU 读，与 initDag 老缓冲的 setPermanentBufferState
    // 同语义）——Vulkan 侧拷贝/回读同步依赖状态跟踪。
    for (auto& rb : cullReadback_) {
        nvrhi::BufferDesc rDesc;
        rDesc.byteSize = 4 * sizeof(std::uint32_t);
        rDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        rDesc.initialState = nvrhi::ResourceStates::CopyDest;
        rDesc.keepInitialState = true;
        rDesc.debugName = "DagCountReadback";
        rb = device->createBuffer(rDesc);
        if (!rb) { outError = "createBuffer failed for hiz readback"; return false; }
    }

    nl::core::log::info("hiz scene ready (dag traversal + depth pre-pass + max pyramid)",
                        " | shader=", shaderSuffix);
    return true;
}

// M4.1：消费两帧前提交的 instanceCount 回读（frame-in-flight=2 的延迟窗口——
// 本帧绘制会在同一个环形槽写入新值，但那发生在本函数读完成之后，无竞争）。
// mapBuffer 内部等待 lastUseFence（DX12 d3d12-buffer.cpp / Vulkan 同构），两帧
// 延迟下正常零等待，不构成主循环里的等待型 map。
// ============================================================================
// M7.0（S7.0）：visibility buffer 打包写入 + 可视化。
// initCull 的 .nmesh / vertex pulling 全套资源原样复用（frustum 恒关 = 全簇全画，
// 遮挡关系交给硬件深度测试 + 64 位 atomicMin），这里只追加：
//   visbuf.slang 四入口 / u64 vis buffer（宽×高）/ 16B VisConstants / 两个绑定布局。
// 尺寸相关的 buffer + binding set 与两条 PSO 分别在 ensureVisbufTargets /
// ensurePipelines 里建；每帧三段式 pass 编排在 draw() 的 visbuf 分支。
// ============================================================================
bool Scene::initVisbuf(nvrhi::IDevice* device, const char* shaderSuffix,
                       const char* vsEntry, const char* psEntry,
                       const char* nmeshFile, std::string& outError) {
    if (!initCull(device, shaderSuffix, vsEntry, psEntry, nmeshFile, false, outError))
        return false;
    visbuf_ = true;
    visbufDevice_ = device;

    // 打包格式的硬前提（S7-PLAN §1）：triId 8 位、clusterId 24 位。
    if (maxTriPerCluster_ > 256u) {
        outError = "visbuf: a cluster has " + std::to_string(maxTriPerCluster_) +
                   " triangles, packing allows 256 (triId is 8 bits)";
        return false;
    }
    if (clusterCount_ >= (1u << 24)) {
        outError = "visbuf: cluster count " + std::to_string(clusterCount_) +
                   " exceeds the 24-bit clusterId field";
        return false;
    }

    auto visPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/visbuf." + artifact + "." + shaderSuffix;
    };
    auto loadVisShader = [&](const char* artifact, nvrhi::ShaderType type,
                             nvrhi::ShaderHandle& out) -> bool {
        std::vector<std::uint8_t> blob;
        if (!readFileBytes(visPath(artifact).c_str(), blob)) {
            outError = std::string("failed to load visbuf shader ") + artifact +
                       " (" + shaderSuffix + ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc desc;
        // 入口名 "main"：SPIR-V 恒 main；DX12 忽略 setEntryName（§2.4 两阶段规则）。
        desc.setShaderType(type).setEntryName("main");
        out = device->createShader(desc, blob.data(), blob.size());
        if (!out) outError = std::string("createShader failed for visbuf ") + artifact;
        return out != nullptr;
    };
    if (!loadVisShader("vsvisraster", nvrhi::ShaderType::Vertex, vsVisRaster_) ||
        !loadVisShader("psvisraster", nvrhi::ShaderType::Pixel, psVisRaster_) ||
        !loadVisShader("vsvisviz", nvrhi::ShaderType::Vertex, vsVisViz_) ||
        !loadVisShader("psvisviz", nvrhi::ShaderType::Pixel, psVisViz_))
        return false;

    // VisConstants（visbuf.slang b1，16B）：(width, height, vizMode, unused)。
    // 每帧写一次，volatile CB 避免 frame-in-flight 相互覆盖。
    nvrhi::BufferDesc vcDesc;
    vcDesc.byteSize = 16;
    vcDesc.isConstantBuffer = true;
    vcDesc.isVolatile = true;
    vcDesc.maxVersions = 16;
    vcDesc.debugName = "VisConstants";
    visConstants_ = device->createBuffer(vcDesc);
    if (!visConstants_) { outError = "createBuffer failed for vis constants"; return false; }

    // §2.7：每条管线必须单一 BindingSet。raster = b0+b1+t1..t4+u0，
    // viz = b1+t0（同一块 u64 buffer 的 SRV 视角），各自一个 set。
    // Vulkan 下 b→256、t→0、u→384 的 shift 由 CMake 编译参数保证。
    nvrhi::BindingLayoutDesc rDesc;
    rDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    visRasterLayout_ = device->createBindingLayout(rDesc);
    if (!visRasterLayout_) { outError = "createBindingLayout failed for vis raster"; return false; }

    nvrhi::BindingLayoutDesc vDesc;
    vDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    visVizLayout_ = device->createBindingLayout(vDesc);
    if (!visVizLayout_) { outError = "createBindingLayout failed for vis viz"; return false; }

    return true;
}

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
        // M6.2：dag 模式 [8]=被 Hi-Z 剔除簇节点数（triCounter[1]，整树语义：被遮
        // 父簇的子树未访问不计入）。cull 模式回读环形仍 2 uint，不读 [8]。
        const auto* vals = static_cast<const std::uint32_t*>(p);
        cullVisibleCount_ = static_cast<int>(vals[0]);
        visibleTriangles_ = static_cast<int>(vals[1]);
        hizOccluded_ = dag_ ? static_cast<int>(vals[2]) : -1;
        // M6.0：金字塔最高 mip 的 asuint 位模式（仅 hiz 模式写入；M6.2 起槽位 [12]）。
        // M6.1：top 语义 = 全屏最远表面径向距离（max 口径，hiz.slang 文件头）。
        // 三态：v ∈ [0,1e8) = 真实最远表面距离；v ≥ 1e8 = 画面有清屏背景像素
        // （max 口径下合法值，返回 -2 = 背景主导哨兵）；NaN（0xFFFFFFFF 清零值）
        // = 尚无数据，返回 -1。
        if (hiz_) {
            float v;
            std::memcpy(&v, &vals[3], sizeof(v));
            if (v >= 0.0f && v < 1e8f)     hizTopFar_ = v;
            else if (v >= 1e8f && v < 2e9f) hizTopFar_ = -2.0f;
            else                            hizTopFar_ = -1.0f;
        } else {
            hizTopFar_ = -1.0f;
        }
        device->unmapBuffer(cullReadback_[slot]);
    } else {
        cullVisibleCount_ = -1;   // map 失败如实上报，不静默沿用旧值
        visibleTriangles_ = -1;
        hizOccluded_ = -1;
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

// M6.0：金字塔/预通道帧缓冲/各级绑定集按视口尺寸懒建（FramebufferInfo 只有格式
// 没有尺寸）。resize 后尺寸变化 → 整组重建；失败置 hizFailed_ 后续帧跳过 hiz。
bool Scene::ensureHizTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                             std::string& outError) {
    const std::uint32_t w = static_cast<std::uint32_t>(viewportW + 0.5f);
    const std::uint32_t h = static_cast<std::uint32_t>(viewportH + 0.5f);
    if (w == 0 || h == 0) { outError = "hiz: zero viewport"; return false; }
    // 尺寸向上取 2 的幂（UE 的 HZB 同款 POT 口径）。D3D12/Vulkan 都限制
    // mipLevels ≤ floor(log2(maxdim))+1——非 POT 视口的 ceil 对半链（640 宽
    // 要 11 层，上限只有 10）直接超规格：DX12 debug layer 报错、Vulkan 驱动
    // 静默建坏（SRV/UAV 全踩坑，M6.0 首跑白剪影 + top pending 的根因）。
    // POT 化后链长恰 = log2+1，ceil 对半保守性成立；mip0 视口外区域每帧
    // 清 1e9，不遮挡（保守方向正确）。
    std::uint32_t pw = 1, ph = 1;
    while (pw < w) pw <<= 1;
    while (ph < h) ph <<= 1;
    if (hizPyramid_ && hizW_ == pw && hizH_ == ph) return true;   // 懒建判据按 POT 尺寸
    hizW_ = pw;
    hizH_ = ph;
    hizJustCreated_ = true;   // 帧内 cmd 补全 mip 清屏（draw 里消费后复位）

    // mip 层数：逐级 ceil 对半直至 1 texel（最高层 = 全屏最近距离）。POT 尺寸
    // 下 ceil 对半链长 == log2+1，与 D3D12/Vulkan mip 上限规则精确一致。
    hizMipCount_ = 1;
    std::uint32_t m = std::max(hizW_, hizH_);
    while (m > 1) { m = (m + 1) / 2; ++hizMipCount_; }

    // 金字塔：R32F 全 mip；mip0 兼任深度预通道颜色 RT，mip>=1 compute 写。
    nvrhi::TextureDesc pDesc;
    pDesc.width = hizW_;
    pDesc.height = hizH_;
    pDesc.format = nvrhi::Format::R32_FLOAT;
    pDesc.mipLevels = hizMipCount_;
    pDesc.isRenderTarget = true;
    pDesc.isUAV = true;
    pDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    pDesc.keepInitialState = true;
    // 优化 clear value = mip0 每帧清的 1e9（D3D12 要求 RT 创建时带 clear value，
    // 否则 ClearRenderTargetView 每帧报 info message 被计入 deviceErrors）。
    pDesc.setClearValue(nvrhi::Color(1e9f, 1e9f, 1e9f, 1e9f));
    pDesc.debugName = "HizPyramid";
    hizPyramid_ = device->createTexture(pDesc);
    if (!hizPyramid_) { outError = "createTexture failed for hiz pyramid"; return false; }

    // 预通道专用 D32 附件（硬件 less 测试保证 mip0 每像素存最近表面）。
    // 尺寸随金字塔 POT 化（Vulkan 帧缓冲要求附件等尺寸）；视口只有 w×h，
    // 视口外深度清 1.0 不参与。
    nvrhi::TextureDesc dDesc;
    dDesc.width = hizW_;
    dDesc.height = hizH_;
    dDesc.format = nvrhi::Format::D32;
    dDesc.isRenderTarget = true;
    dDesc.initialState = nvrhi::ResourceStates::DepthWrite;
    dDesc.keepInitialState = true;
    dDesc.setClearValue(nvrhi::Color(1.0f));
    dDesc.debugName = "HizPrepassDepth";
    hizDepth_ = device->createTexture(dDesc);
    if (!hizDepth_) { outError = "createTexture failed for hiz prepass depth"; return false; }

    {
        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(hizPyramid_, nvrhi::TextureSubresourceSet(0, 1, 0,
            nvrhi::TextureSubresourceSet::AllArraySlices));
        fbDesc.setDepthAttachment(hizDepth_);
        prepassFramebuffer_ = device->createFramebuffer(fbDesc);
    }
    if (!prepassFramebuffer_) { outError = "createFramebuffer failed for hiz prepass"; return false; }

    // 各级降采样绑定集：SRV(0)=mip L-1 单 mip（Load LOD 相对视图基 mip）+
    // UAV(0)=mip L + b1。索引 = 层号 L（[0] 未用）。
    chainSets_.assign(hizMipCount_, nullptr);
    for (std::uint32_t L = 1; L < hizMipCount_; ++L) {
        nvrhi::BindingSetDesc sd;
        sd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, hizPyramid_,
            nvrhi::Format::UNKNOWN,
            nvrhi::TextureSubresourceSet(L - 1, 1, 0,
                                         nvrhi::TextureSubresourceSet::AllArraySlices)))
           .addItem(nvrhi::BindingSetItem::Texture_UAV(0, hizPyramid_,
            nvrhi::Format::R32_FLOAT,
            nvrhi::TextureSubresourceSet(L, 1, 0,
                                         nvrhi::TextureSubresourceSet::AllArraySlices)))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, hizConstants_));
        chainSets_[L] = device->createBindingSet(sd, chainLayout_);
        if (!chainSets_[L]) { outError = "createBindingSet failed for hiz chain level"; return false; }
    }

    // top 回读绑定集：SRV = 最高 mip（1 texel）+ UAV = 计数器。
    {
        nvrhi::BindingSetDesc sd;
        sd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, hizPyramid_,
            nvrhi::Format::UNKNOWN,
            nvrhi::TextureSubresourceSet(hizMipCount_ - 1, 1, 0,
                                         nvrhi::TextureSubresourceSet::AllArraySlices)))
           .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, hizTop_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, hizConstants_));
        topSet_ = device->createBindingSet(sd, topLayout_);
    }
    if (!topSet_) { outError = "createBindingSet failed for hiz top"; return false; }

    // 可视化绑定集：SRV 全 mip（绝对 mip 号 Load）+ b1。
    {
        nvrhi::BindingSetDesc sd;
        sd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, hizPyramid_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, hizConstants_));
        vizSet_ = device->createBindingSet(sd, vizLayout_);
    }
    if (!vizSet_) { outError = "createBindingSet failed for hiz viz"; return false; }

    // M6.1：dag 遍历的 hiz 态绑定集（t8 = 真金字塔全 mip SRV）。布局与
    // dagComputeSet_ 同一个（§2.7 互斥使用，不并存）。
    {
        nvrhi::BindingSetDesc sd;
        sd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, dagConstants_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, clusterSpheres_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, dagClusterErrors_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, dagGroups_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, dagMembers_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, dagGenGroup_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, dagClusterLevel_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, clusterMeta_))
          .addItem(nvrhi::BindingSetItem::Texture_SRV(8, hizPyramid_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visibleList_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, indirectArgs_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, dagGroupExpanded_))
          .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, triCounter_));
        dagHizSet_ = device->createBindingSet(sd, dagComputeLayout_);
    }
    if (!dagHizSet_) { outError = "createBindingSet failed for dag hiz set"; return false; }

    // 新建/重建金字塔的全 mip 清屏放到**帧内 cmd**（draw 里 hizJustCreated_ 消费；
    // 此处处于已打开的帧命令列表作用域，NVRHI 禁止再开第二个 immediate 列表）。
    // 下一帧遍历读到「全部不遮挡」的保守缺省，避免首帧未定义数据造成假剔除。
    hizReady_ = true;   // 下一帧起遍历可开遮挡测试（本帧遍历已跑完，天然一帧延迟）
    return true;
}

bool Scene::ensureVisbufTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                                std::string& outError) {
    const std::uint32_t w = static_cast<std::uint32_t>(viewportW + 0.5f);
    const std::uint32_t h = static_cast<std::uint32_t>(viewportH + 0.5f);
    if (w == 0 || h == 0) { outError = "visbuf: zero viewport"; return false; }
    if (visBuffer_ && visW_ == w && visH_ == h && visRasterSet_ && visVizSet_) return true;
    visW_ = w;
    visH_ = h;

    // 每像素一个 u64：高 32 = asuint(deviceZ)，低 32 = clusterId<<8 | triId。
    // structStride=8 是 RWStructuredBuffer<uint64_t> 的 UAV 视角要求；
    // canHaveUAVs 同时让 NVRHI 能建 DX12 的 R32_UINT typed clear UAV
    // （clearBufferUInt(0xFFFFFFFF) → 每个 u32 全 1 → u64 = VIS_EMPTY）。
    // keepInitialState：每条命令列表从 CopyDest 出发，帧内由状态跟踪动态晋升
    // clear(CopyDest/UAV) → raster(UAV) → viz(ShaderResource)，visibleList_ 同款。
    nvrhi::BufferDesc bd;
    bd.byteSize = static_cast<std::uint64_t>(w) * h * sizeof(std::uint64_t);
    bd.structStride = 8;
    bd.canHaveUAVs = true;
    bd.initialState = nvrhi::ResourceStates::CopyDest;
    bd.keepInitialState = true;
    bd.debugName = "VisBufferU64";
    visBuffer_ = device->createBuffer(bd);
    if (!visBuffer_) { outError = "createBuffer failed for vis buffer"; return false; }

    nvrhi::BindingSetDesc rSet;
    rSet.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, visConstants_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, vertexPool_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, localIndices_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, clusterMeta_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, visibleList_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visBuffer_));
    visRasterSet_ = device->createBindingSet(rSet, visRasterLayout_);
    if (!visRasterSet_) { outError = "createBindingSet failed for vis raster"; return false; }

    // viz set 把同一块 buffer 绑成 SRV(t0)：与 raster 的 UAV(u0) 只是两个视图，
    // 不涉及拷贝。§2.7 单 BindingSet 指每条管线一个 set，不是全场只能有一个。
    nvrhi::BindingSetDesc vSet;
    vSet.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, visConstants_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, visBuffer_));
    visVizSet_ = device->createBindingSet(vSet, visVizLayout_);
    if (!visVizSet_) { outError = "createBindingSet failed for vis viz"; return false; }
    return true;
}

bool Scene::ensurePipelines(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                            std::string& outError) {
    // M-E1b.0：ECS 场景 pipeline（vsScene_/psScene_/sceneInputLayout_）。
    // 深度/格式/fillMode/cullMode 与既有 pipeline 建法完全同款（2×2 组合）。
    if (sceneMode_) {
        if (pipelineSceneBack_ && pipelineSceneNone_ && pipelineSceneWireBack_ && pipelineSceneWireNone_)
            return true;
        pipelineSceneBack_ = nullptr;
        pipelineSceneNone_ = nullptr;
        pipelineSceneWireBack_ = nullptr;
        pipelineSceneWireNone_ = nullptr;
        for (int wire = 0; wire < 2; ++wire) {
            for (int i = 0; i < 2; ++i) {
                nvrhi::GraphicsPipelineDesc desc;
                desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                    .setInputLayout(sceneInputLayout_)
                    .setVertexShader(vsScene_)
                    .setFragmentShader(psScene_)
                    .addBindingLayout(sceneBindingLayout_);
                // 正面朝向口径与既有 pipeline 一致（init 处注释：CCW 指定为正面）。
                desc.renderState.rasterState
                    .setFrontCounterClockwise(true)
                    .setFillMode(wire ? nvrhi::RasterFillMode::Wireframe
                                      : nvrhi::RasterFillMode::Solid)
                    .setCullMode(i == 0 ? nvrhi::RasterCullMode::Back : nvrhi::RasterCullMode::None);
                desc.renderState.depthStencilState.enableDepthTest().enableDepthWrite();
                auto pipeline = device->createGraphicsPipeline(desc, info);
                if (!pipeline) { outError = "createGraphicsPipeline failed for ecs scene"; return false; }
                if (wire == 0)
                    (i == 0 ? pipelineSceneBack_ : pipelineSceneNone_) = pipeline;
                else
                    (i == 0 ? pipelineSceneWireBack_ : pipelineSceneWireNone_) = pipeline;
            }
        }
        return true;
    }
    // M6.0：hiz 管线。预通道 FramebufferInfo 直接按格式构造（R32F + D32），
    // 不依赖真实纹理——纹理/帧缓冲/绑定集随视口尺寸在 ensureHizTargets 懒建。
    if (hiz_ && !prepassPSO_) {
        nvrhi::FramebufferInfo prepassInfo;
        prepassInfo.addColorFormat(nvrhi::Format::R32_FLOAT);
        prepassInfo.setDepthFormat(nvrhi::Format::D32);
        nvrhi::GraphicsPipelineDesc pDesc;
        pDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
             .setVertexShader(vsHizDepth_)
             .setFragmentShader(psHizDepth_)
             .addBindingLayout(hizPrepassLayout_);
        pDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::Back);
        pDesc.renderState.depthStencilState.enableDepthTest().enableDepthWrite();
        prepassPSO_ = device->createGraphicsPipeline(pDesc, prepassInfo);
        if (!prepassPSO_) { outError = "createGraphicsPipeline failed for hiz prepass"; return false; }

        nvrhi::GraphicsPipelineDesc vDesc;
        vDesc.setPrimType(nvrhi::PrimitiveType::TriangleStrip)
             .setVertexShader(vsHizViz_)
             .setFragmentShader(psHizViz_)
             .addBindingLayout(vizLayout_);
        vDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::None);
        // 深度测试/写入关（缺省即关）：覆盖层画在主场景之上，只读金字塔。
        vizPSO_ = device->createGraphicsPipeline(vDesc, info);
        if (!vizPSO_) { outError = "createGraphicsPipeline failed for hiz viz"; return false; }
    }
    // M7.0（S7.0）：visbuf 两条管线。必须排在 cull 分支之前——visbuf 复用 cull_
    // 的资源通路，但那四套 (fill×cull) 管线在 visbuf 下完全用不上。
    if (visbuf_) {
        if (visRasterPSO_ && visVizPSO_) return true;
        // 簇光栅：无顶点输入（VS 只读 SV_InstanceID/SV_VertexID + SSBO），
        // 深度测试照旧；PS 的 SV_Target 只是占位，真正产物是 u0 的 atomicMin。
        nvrhi::GraphicsPipelineDesc rDesc;
        rDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
             .setVertexShader(vsVisRaster_)
             .setFragmentShader(psVisRaster_)
             .addBindingLayout(visRasterLayout_);
        rDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::Back);
        rDesc.renderState.depthStencilState.enableDepthTest().enableDepthWrite();
        visRasterPSO_ = device->createGraphicsPipeline(rDesc, info);
        if (!visRasterPSO_) { outError = "createGraphicsPipeline failed for vis raster"; return false; }

        // 全屏可视化：三条顶点的大三角形，深度测试/写入关（缺省即关），
        // 整屏覆盖光栅 pass 的占位色。M7.1 会在这里换成 material pass。
        nvrhi::GraphicsPipelineDesc vDesc;
        vDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
             .setVertexShader(vsVisViz_)
             .setFragmentShader(psVisViz_)
             .addBindingLayout(visVizLayout_);
        vDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::None);
        visVizPSO_ = device->createGraphicsPipeline(vDesc, info);
        if (!visVizPSO_) { outError = "createGraphicsPipeline failed for vis viz"; return false; }
        return true;
    }
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
    // M6.0：hiz 管线随主管线一起重建（纹理/绑定集按视口尺寸懒建，不受影响）。
    prepassPSO_ = nullptr;
    vizPSO_ = nullptr;
    // M7.0：visbuf 两条管线同样随 framebuffer 格式重建；
    // vis buffer / 两个 binding set 按视口尺寸懒建，不在这里清。
    visRasterPSO_ = nullptr;
    visVizPSO_ = nullptr;
    pipelineSceneBack_ = nullptr;      // M-E1b.0：ECS 场景四套随 resize 一起重建
    pipelineSceneNone_ = nullptr;
    pipelineSceneWireBack_ = nullptr;
    pipelineSceneWireNone_ = nullptr;
    pipelineBack_ = nullptr;
    pipelineNone_ = nullptr;
    pipelineWireBack_ = nullptr;
    pipelineWireNone_ = nullptr;
    pipelineCullBack_ = nullptr;
    pipelineCullNone_ = nullptr;
    pipelineCullWireBack_ = nullptr;
    pipelineCullWireNone_ = nullptr;
}

// ---- M-A5：CPU 蒙皮场景（口径见 scene.hpp initSkin 注释）----
bool Scene::initSkin(nvrhi::IDevice* device, const char* shaderSuffix,
                     const char* vsEntry, const char* psEntry,
                     const char* modelFile, std::string& outError) {
    skinMode_ = true;

    // ---- 1. 同一份 model.slang 字节码：CPU 蒙皮后的顶点就是普通 POS/NORM/UV 流，
    //         管线/常量布局与 init 完全共用，不新增 shader（一份 .slang 单源纪律）----
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

    // ---- 2. rigged GLB：骨架 + clips + 绑定 + bind 空间网格（nl::anim 纯逻辑层）----
    const std::string modelPath = std::string(NL_ASSET_DIR "/") + modelFile;
    nl::anim::SkinnedMeshData meshData;
    std::string err;
    if (!nl::anim::loadGltfSkinnedModel(modelPath, skinSkeleton_, skinClips_,
                                        meshData, skinBinding_, err)) {
        outError = std::string(modelFile) + ": " + err;
        return false;
    }
    if (skinClips_.empty()) {
        outError = std::string(modelFile) + ": rigged model has no animation clip";
        return false;
    }
    const std::size_t vertexCount = meshData.positions.size();
    skinBasePos_ = meshData.positions;
    skinBaseNrm_ = meshData.normals;

    // M-A5：蒙皮输出包围盒中心——rigged 资产的网格在 bind 空间（中心≈原点），
    // 但骨骼动画采样后输出站姿（如 CesiumMan 站在 y=0、身高 1.4），而
    // OrbitCamera target 硬编码 (0,0,0)。init 时先做一次 t=0 蒙皮，取其输出
    // 包围盒中心作为逐帧后蒙皮刚体平移量（不碰蒙皮数学），模型居中于原点。
    skinCenter_ = {0.0f, 0.0f, 0.0f};
    skinPos_.resize(vertexCount);
    skinNrm_.resize(vertexCount);
    if (!skinClips_.empty() && vertexCount > 0) {
        skinLocal_.resize(skinSkeleton_.boneCount());
        skinModel_.resize(skinSkeleton_.boneCount());
        const nl::anim::AnimClip& probe = skinClips_[0];
        if (nl::anim::sampleClip(probe, 0.0f, skinLocal_) &&
            nl::anim::localToModel(skinLocal_, skinSkeleton_, skinModel_)) {
            nl::anim::computePalette(skinPalette_, skinModel_, skinBinding_.inverseBind);
            nl::anim::skinPositions(skinPos_, skinBasePos_, skinPalette_, skinBinding_);
            nl::anim::Vec3 mn = skinPos_.front(), mx = skinPos_.front();
            for (const auto& p : skinPos_) {
                mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
                mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
            }
            skinCenter_ = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
            // t=0 蒙皮输出半径（最大顶点到中心距离）：供 viewer 按资产尺度自动取景
            //（Fox.glb 等资产单位远大于 1m，固定缺省距离会把相机放进模型肚子里）。
            skinRadius_ = 0.0f;
            for (const auto& p : skinPos_) {
                const float dx = p.x - skinCenter_.x, dy = p.y - skinCenter_.y,
                            dz = p.z - skinCenter_.z;
                skinRadius_ = std::max(skinRadius_, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
    }

    skinVertices_.resize(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        skinVertices_[i] = { skinBasePos_[i].x, skinBasePos_[i].y, skinBasePos_[i].z,
                             skinBaseNrm_[i].x, skinBaseNrm_[i].y, skinBaseNrm_[i].z,
                             0.0f, 0.0f };
    }
    indexCount_ = static_cast<std::uint32_t>(meshData.indices.size());

    // ---- 3. GPU 资源：VB 走 hud.cpp 同款动态模式（CopyDest + keepInitialState，
    //         每帧 draw 内 writeBuffer 重写）；IB/贴图/采样/布局与 init 同构 ----
    const std::string vbName = std::string(modelFile) + "SkinVB";
    const std::string ibName = std::string(modelFile) + "SkinIB";
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = skinVertices_.size() * sizeof(SceneVertex);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = vbName.c_str();
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    vbDesc.keepInitialState = true;    // 每帧命令列表开头自动回到 CopyDest → 可反复 writeBuffer
    vertexBuffer_ = device->createBuffer(vbDesc);
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = meshData.indices.size() * sizeof(std::uint32_t);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = ibName.c_str();
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    indexBuffer_ = device->createBuffer(ibDesc);
    if (!vertexBuffer_ || !indexBuffer_) { outError = "createBuffer failed for skin VB/IB"; return false; }

    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(SceneConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "SceneConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for volatile constant buffer"; return false; }

    nvrhi::TextureDesc texDesc;
    texDesc.width = 1;
    texDesc.height = 1;
    texDesc.format = nvrhi::Format::RGBA8_UNORM;
    texDesc.debugName = "SkinWhiteTex";
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    texture_ = device->createTexture(texDesc);
    if (!texture_) { outError = "createTexture failed for skin white texture"; return false; }

    // pbr-direct：skin 场景的 MR 贴图也是 1x1 dummy（rigged 资产暂不解析 PBR
    // 材质）：roughness=1、metallic=0，psMain 的 pbr 分支绑定面必须齐（§2.7）。
    {
        nvrhi::TextureDesc mrDesc = texDesc;
        mrDesc.debugName = "SkinDummyMR";
        mrTexture_ = device->createTexture(mrDesc);
        if (!mrTexture_) { outError = "createTexture failed for skin dummy MR"; return false; }
        nvrhi::BufferDesc lcDesc;
        lcDesc.byteSize = sizeof(LightConstants);
        lcDesc.isConstantBuffer = true;
        lcDesc.isVolatile = true;
        lcDesc.maxVersions = 16;
        lcDesc.debugName = "LightConstants";
        lightConstants_ = device->createBuffer(lcDesc);
        if (!lightConstants_) { outError = "createBuffer failed for skin light constants"; return false; }
    }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed"; return false; }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))    // pbr-direct：psMain 引用 t1
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));   // b1 LightConstants
    bindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!bindingLayout_) { outError = "createBindingLayout failed"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture_))
           .addItem(nvrhi::BindingSetItem::Texture_SRV(1, mrTexture_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, lightConstants_));
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
    if (!inputLayout_) { outError = "createInputLayout failed for skin"; return false; }

    // IB 一次性上传；VB 不预传（首帧 draw 前的 writeBuffer 就是首份数据）。
    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    upload->beginTrackingBufferState(indexBuffer_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(indexBuffer_, meshData.indices.data(),
                        meshData.indices.size() * sizeof(std::uint32_t));
    upload->setPermanentBufferState(indexBuffer_, nvrhi::ResourceStates::IndexBuffer);
    const std::uint8_t white[4] = { 255, 255, 255, 255 };
    upload->writeTexture(texture_, 0, 0, white, sizeof(white));
    const std::uint8_t dummyMR[4] = { 0, 255, 0, 255 };   // roughness=1, metallic=0
    upload->writeTexture(mrTexture_, 0, 0, dummyMR, sizeof(dummyMR));
    upload->close();
    device->executeCommandList(upload);

    modelName_ = modelFile;
    triangleCount_ = indexCount_ / 3;
    nl::core::log::info(
        "skin model ready: ", modelFile,
        " | bones=", skinSkeleton_.boneCount(),
        " clips=", skinClips_.size(),
        " vertices=", vertexCount,
        " | shader=", shaderSuffix);
    return true;
}

// ---------------------------------------------------------------------------
// M-E1b.0：ECS 场景（--scene）。口径见 scene.hpp initScene 注释与 model.slang
// 文件头 M-E1b.0 段（资源布局；多灯 = 方向灯数组，Carrot ModelRenderSystem 蓝本）。
// ---------------------------------------------------------------------------

// .glb 白名单：与 renderer.cpp 的 validAssetFileName 同一套逻辑（实现锁在那边
// 的匿名 namespace 里，这里复制语义，两处注释互相指向不许漂移）。viewer 侧
// 收集 SceneObjectDesc 时已按同规则校验，这里兜底拒绝脏数据（不静默）。
static bool validGlbAssetName(const char* name) {
    if (!name || !*name) return false;
    const std::string s(name);
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    const std::string ext = ".glb";
    if (s.size() < ext.size() + 1 || s.compare(s.size() - ext.size(), ext.size(), ext) != 0)
        return false;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) return false;   // 可打印 ASCII，不含空格
    }
    return true;
}

bool Scene::initScene(nvrhi::IDevice* device, const char* shaderSuffix,
                      const char* vsEntry, const char* psEntry,
                      const SceneObjectDesc* objects, std::uint32_t objectCount,
                      const SceneLightDesc* lights, std::uint32_t lightCount,
                      float ambient, const char* sceneName, std::string& outError) {
    sceneMode_ = true;
    sceneAmbient_ = ambient;
    sceneLightCount_ = lightCount;
    // 入参兜底校验（viewer 侧已过滤；这里显式拒绝，绝不静默画空场景）。
    if (!objects || objectCount == 0 || (lightCount > 0 && !lights)) {
        outError = "initScene: objects must be non-empty (lights null iff lightCount==0)";
        return false;
    }

    // ---- 1. model.vsscene/pscene 双目标字节码（照 Scene::init 第 1 步）----
    auto shaderPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/model." + artifact + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    if (!readFileBytes(shaderPath("vsscene").c_str(), vsBlob) ||
        !readFileBytes(shaderPath("pscene").c_str(), psBlob)) {
        outError = std::string("failed to load ecs scene shaders (") + shaderSuffix +
                   ") from " NL_SHADER_DIR;
        return false;
    }
    nvrhi::ShaderDesc vsDesc;
    vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName(vsEntry);
    nvrhi::ShaderDesc psDesc;
    psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName(psEntry);
    vsScene_ = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
    psScene_ = device->createShader(psDesc, psBlob.data(), psBlob.size());
    if (!vsScene_ || !psScene_) { outError = "createShader failed for ecs scene shaders"; return false; }

    // ---- 2. meshFile 白名单 + 按文件分组（保持首次出现序，稳定确定）----
    std::vector<std::string> batchFiles;
    std::vector<std::vector<std::uint32_t>> batchObjects;
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        if (!validGlbAssetName(objects[i].meshFile)) {
            outError = std::string("invalid meshFile '") +
                       (objects[i].meshFile ? objects[i].meshFile : "<null>") +
                       "' (expected an ASCII *.glb file name in assets/models/)";
            return false;
        }
        const std::string file(objects[i].meshFile);
        std::size_t b = 0;
        while (b < batchFiles.size() && batchFiles[b] != file) ++b;
        if (b == batchFiles.size()) {
            batchFiles.push_back(file);
            batchObjects.emplace_back();
        }
        batchObjects[b].push_back(i);
    }

    // ---- 3. 每组读 GLB（首 primitive，照 Scene::init 第 2 步的校验与顶点展开）----
    struct PreparedBatch {
        std::vector<SceneVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::uint32_t texWidth = 0, texHeight = 0;
        std::vector<std::uint8_t> texRGBA;
        float sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // 局部包围球 xyz=center w=radius
    };
    std::vector<PreparedBatch> prepared(batchFiles.size());
    for (std::size_t b = 0; b < batchFiles.size(); ++b) {
        const std::string modelPath = std::string(NL_ASSET_DIR "/") + batchFiles[b];
        nl::core::SceneMeshData meshData;
        std::string loadWarning;
        if (!nl::core::loadGltfScene(modelPath.c_str(), meshData, loadWarning)) {
            outError = batchFiles[b] + std::string(": ") + loadWarning; return false;
        }
        if (!loadWarning.empty()) nl::core::log::debug(loadWarning);
        if (meshData.meshes.empty()) {
            outError = batchFiles[b] + std::string(" has no mesh primitive"); return false;
        }
        const nl::core::PrimitiveMesh& prim = meshData.meshes[0];
        if (!prim.hasBaseColor) {
            outError = batchFiles[b] + std::string(" has no base color texture"); return false;
        }
        if (prim.indices.empty()) {
            outError = batchFiles[b] + std::string(" first primitive has no indices"); return false;
        }
        PreparedBatch& pb = prepared[b];
        pb.vertices.resize(prim.vertices.size());
        for (std::size_t i = 0; i < prim.vertices.size(); ++i) {
            const auto& mv = prim.vertices[i];
            pb.vertices[i] = { mv.pos[0], mv.pos[1], mv.pos[2],
                               mv.normal[0], mv.normal[1], mv.normal[2],
                               mv.uv[0], mv.uv[1] };
        }
        pb.indices = prim.indices;
        pb.texWidth = prim.texWidth;
        pb.texHeight = prim.texHeight;
        pb.texRGBA = prim.baseColorRGBA;
        // 局部包围球：中心 = 包围盒中心，半径 = 最远顶点到中心距离
        //（uploadClusterData 算式语义；世界联合球在第 6 步经实例矩阵变换）。
        float mn[3] = { pb.vertices.front().px, pb.vertices.front().py, pb.vertices.front().pz };
        float mx[3] = { mn[0], mn[1], mn[2] };
        for (const SceneVertex& v : pb.vertices) {
            mn[0] = std::min(mn[0], v.px); mn[1] = std::min(mn[1], v.py); mn[2] = std::min(mn[2], v.pz);
            mx[0] = std::max(mx[0], v.px); mx[1] = std::max(mx[1], v.py); mx[2] = std::max(mx[2], v.pz);
        }
        pb.sphere[0] = (mn[0] + mx[0]) * 0.5f;
        pb.sphere[1] = (mn[1] + mx[1]) * 0.5f;
        pb.sphere[2] = (mn[2] + mx[2]) * 0.5f;
        float radius = 0.0f;
        for (const SceneVertex& v : pb.vertices) {
            const float dx = v.px - pb.sphere[0], dy = v.py - pb.sphere[1], dz = v.pz - pb.sphere[2];
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        pb.sphere[3] = radius;
    }

    // ---- 4. 建 GPU 资源：volatile CB + 每 batch VB/IB/贴图/采样/布局槽位 ----
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(SceneConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "SceneConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for volatile constant buffer"; return false; }

    // 槽位与 model.slang 的 register(b0/t0/s0/t5/t6) 一致；t5/t6 经 slangc 的
    // -fvk-t-shift 0 0 在 Vulkan 映射为 binding 5/6，DX12 为 SRV slot 5/6。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))   // b1 LightConstants（M-E1b.3：cameraPos）
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
    sceneBindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!sceneBindingLayout_) { outError = "createBindingLayout failed for ecs scene"; return false; }

    // M-E1b.3：b1 LightConstants（含 cameraPos）—— psScene 视向量 V 从 b1 取；
    // 复用 pbr-direct 同款 80B cbuffer，每帧由 sceneMode_ 渲染分支写 params.eye。
    {
        nvrhi::BufferDesc lcDesc;
        lcDesc.byteSize = sizeof(LightConstants);
        lcDesc.isConstantBuffer = true;
        lcDesc.isVolatile = true;
        lcDesc.maxVersions = 16;
        lcDesc.debugName = "LightConstants(EcsScene)";
        lightConstants_ = device->createBuffer(lcDesc);
        if (!lightConstants_) { outError = "createBuffer failed for ecs scene LightConstants"; return false; }
    }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);

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
    sceneInputLayout_ = device->createInputLayout(attrs, 3, vsScene_);
    if (!sceneInputLayout_) { outError = "createInputLayout failed for ecs scene"; return false; }

    sceneBatches_.resize(batchFiles.size());
    for (std::size_t b = 0; b < batchFiles.size(); ++b) {
        const PreparedBatch& pb = prepared[b];
        SceneMeshBatch& batch = sceneBatches_[b];
        const std::string vbName = batchFiles[b] + "VB";
        const std::string ibName = batchFiles[b] + "IB";
        const std::string texName = batchFiles[b] + "BaseColor";
        nvrhi::BufferDesc vbDesc;
        vbDesc.byteSize = pb.vertices.size() * sizeof(SceneVertex);
        vbDesc.isVertexBuffer = true;
        vbDesc.debugName = vbName.c_str();
        vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
        batch.vb = device->createBuffer(vbDesc);
        nvrhi::BufferDesc ibDesc;
        ibDesc.byteSize = pb.indices.size() * sizeof(std::uint32_t);
        ibDesc.isIndexBuffer = true;
        ibDesc.debugName = ibName.c_str();
        ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
        batch.ib = device->createBuffer(ibDesc);
        if (!batch.vb || !batch.ib) { outError = "createBuffer failed for ecs scene VB/IB"; return false; }
        batch.indexCount = static_cast<std::uint32_t>(pb.indices.size());

        nvrhi::TextureDesc texDesc;
        texDesc.width = pb.texWidth;
        texDesc.height = pb.texHeight;
        texDesc.format = nvrhi::Format::RGBA8_UNORM;
        texDesc.debugName = texName.c_str();
        texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        texDesc.keepInitialState = true;
        batch.tex = device->createTexture(texDesc);
        if (!batch.tex) { outError = "createTexture failed for ecs scene base color"; return false; }

        batch.sampler = device->createSampler(samplerDesc);
        if (!batch.sampler) { outError = "createSampler failed for ecs scene"; return false; }

        // 每 batch 的 BindingSet 延后到实例/灯缓冲建好之后再创建（见第 5.5 步）：
        // NVRHI 不允许 BindingSet 里绑空句柄，而 t5/t6 要先有实缓冲。
    }

    // ---- 5. 每 batch 实例缓冲 + 全场灯缓冲（StructuredBuffer<float4>）----
    // 实例：每实例 7 个 float4（M-E1b.3 由 5 扩到 7）—— [0..3] 列主序世界矩阵 4 列，
    // [4]=(tint.rgb,0)、[5]=(roughness, metallic, 0, 0)。
    // 灯：1+lightCount*3 个 float4 —— [0]=(lightCount, ambient, 1/ΣI, pointCount)，每灯
    // {dir.xyz, intensity}、{color.rgb, type}、{position.xyz, InvRadius}。
    // M-E1b.1：实例缓冲按 batch 各建一份，SV_InstanceID 就是本 batch 内 0-based
    // 槽号。旧做法把全场实例放一个缓冲、用 startInstanceLocation 划窗口，但 DX12
    // 的 SV_InstanceID 不含 StartInstanceLocation（2-batch 探针：两物体叠成一团）；
    // Vulkan 侧若改用 SRV 子范围又会撞 minStorageBufferOffsetAlignment。故彻底
    // 不依赖 base 语义：单 batch 时与 M-E1b.0 逐字节同构，既有基线无需重拍。
    // batch 内保持 objects 原序（稳定确定）。
    std::vector<std::vector<float>> batchInstanceData(batchFiles.size());
    for (std::size_t b = 0; b < batchFiles.size(); ++b) {
        SceneMeshBatch& batch = sceneBatches_[b];
        batch.instanceCount = static_cast<std::uint32_t>(batchObjects[b].size());
        std::vector<float>& data = batchInstanceData[b];
        data.assign(static_cast<std::size_t>(batch.instanceCount) * 7u * 4u, 0.0f);
        std::uint32_t slot = 0;
        for (std::uint32_t i : batchObjects[b]) {
            const float* w = objects[i].world;   // 列主序 m[col*4+row]
            float* dst = data.data() + static_cast<std::size_t>(slot) * 7 * 4;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    dst[c * 4 + r] = w[c * 4 + r];
            dst[16] = objects[i].tint[0];
            dst[17] = objects[i].tint[1];
            dst[18] = objects[i].tint[2];
            dst[19] = 0.0f;
            // M-E1b.3：逐实例 PBR 材质（缺省 roughness=1 / metallic=0）。
            dst[20] = objects[i].roughness;
            dst[21] = objects[i].metallic;
            dst[22] = 0.0f;
            dst[23] = 0.0f;
            ++slot;
        }
        nvrhi::BufferDesc instDesc;
        instDesc.byteSize = data.size() * sizeof(float);
        instDesc.structStride = 16;              // StructuredBuffer<float4>
        instDesc.canHaveTypedViews = true;
        instDesc.debugName = "SceneInstances";
        instDesc.initialState = nvrhi::ResourceStates::CopyDest;
        batch.instances = device->createBuffer(instDesc);
        if (!batch.instances) { outError = "createBuffer failed for ecs scene instances"; return false; }
    }

    // M-E1b.2：每灯 3 个 float4 —— {dir.xyz,I}、{color.rgb,type}、{pos.xyz,InvRadius}。
    // 头 [0]=(lightCount, ambient, 1/ΣI(方向灯), pointCount)；InvRadius=1/range 由
    // CPU 预算（UE LightData.InvRadius 同款），shader 只做乘法避免两后端除法微差。
    const std::uint32_t lightFloats = (1u + lightCount * 3u) * 4u;
    std::vector<float> lightData(lightFloats, 0.0f);
    lightData[0] = static_cast<float>(lightCount);
    lightData[1] = ambient;
    float sumIntensity = 0.0f;     // 只统计方向灯：点光按绝对强度独立累加
    std::uint32_t pointCount = 0u;
    for (std::uint32_t i = 0; i < lightCount; ++i) {
        float* d0 = lightData.data() + (1u + i * 3u) * 4u;
        float* d1 = d0 + 4;
        float* d2 = d0 + 8;
        d0[0] = lights[i].dir[0]; d0[1] = lights[i].dir[1]; d0[2] = lights[i].dir[2];
        d0[3] = lights[i].intensity;
        d1[0] = lights[i].color[0]; d1[1] = lights[i].color[1]; d1[2] = lights[i].color[2];
        d1[3] = static_cast<float>(lights[i].type);
        d2[0] = lights[i].position[0]; d2[1] = lights[i].position[1];
        d2[2] = lights[i].position[2];
        d2[3] = (lights[i].range > 0.0f) ? (1.0f / lights[i].range) : 0.0f;
        if (lights[i].type == 0) sumIntensity += lights[i].intensity;
        else ++pointCount;
    }
    // [0].z = 方向灯归一化因子的倒数，CPU 端算好让 shader 只做乘法：
    // shader 内做除法时 DXIL / SPIR-V 的浮点精度差会在个别像素上差 1~2/255，
    // 预乘倒数后两后端读同一份缓冲、运算完全一致（双后端 bytes 口径）。
    // [0].w = 点光数量，仅作 uniform 分支判据（无点光时走 M-E1b.0 原表达式）。
    lightData[2] = (sumIntensity > 0.0f) ? (1.0f / sumIntensity) : 0.0f;
    lightData[3] = static_cast<float>(pointCount);
    nvrhi::BufferDesc lightDesc;
    lightDesc.byteSize = lightData.size() * sizeof(float);
    lightDesc.structStride = 16;             // StructuredBuffer<float4>
    lightDesc.canHaveTypedViews = true;
    lightDesc.debugName = "SceneLights";
    lightDesc.initialState = nvrhi::ResourceStates::CopyDest;
    sceneLights_ = device->createBuffer(lightDesc);
    if (!sceneLights_) { outError = "createBuffer failed for ecs scene lights"; return false; }

    // ---- 5.5 每 batch 一个 BindingSet（§2.7 单 set 纪律）----
    // 必须在实例/灯缓冲创建之后：t5/t6 绑空句柄会被 NVRHI 校验直接拒绝。
    // 内容同 layout，t0 换各自贴图，t5/t6 实例/灯缓冲与 b0 常量全场景共享。
    for (std::size_t b = 0; b < batchFiles.size(); ++b) {
        SceneMeshBatch& batch = sceneBatches_[b];
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, batch.tex))
               .addItem(nvrhi::BindingSetItem::Sampler(0, batch.sampler))
               .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
               .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, lightConstants_))   // b1（M-E1b.3：cameraPos）
               .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, batch.instances))
               .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, sceneLights_));
        batch.set = device->createBindingSet(setDesc, sceneBindingLayout_);
        if (!batch.set) { outError = "createBindingSet failed for ecs scene batch"; return false; }
    }

    // ---- 6. 一次性上传：staging → 每 batch VB/IB/贴图 + 实例/灯缓冲 ----
    // 写法照 Scene::init 第 4 步；实例/灯缓冲上传后钉 permanent ShaderResource
    //（只读 SRV，每帧不重写——与 clusterSpheres_ 同款静态路径）。
    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    for (std::size_t b = 0; b < batchFiles.size(); ++b) {
        const PreparedBatch& pb = prepared[b];
        SceneMeshBatch& batch = sceneBatches_[b];
        upload->beginTrackingBufferState(batch.vb, nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(batch.vb, pb.vertices.data(), pb.vertices.size() * sizeof(SceneVertex));
        upload->setPermanentBufferState(batch.vb, nvrhi::ResourceStates::VertexBuffer);
        upload->beginTrackingBufferState(batch.ib, nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(batch.ib, pb.indices.data(), pb.indices.size() * sizeof(std::uint32_t));
        upload->setPermanentBufferState(batch.ib, nvrhi::ResourceStates::IndexBuffer);
        upload->writeTexture(batch.tex, 0, 0, pb.texRGBA.data(),
                             static_cast<size_t>(pb.texWidth) * 4u);
        upload->beginTrackingBufferState(batch.instances, nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(batch.instances, batchInstanceData[b].data(),
                            batchInstanceData[b].size() * sizeof(float));
        upload->setPermanentBufferState(batch.instances, nvrhi::ResourceStates::ShaderResource);
    }
    upload->beginTrackingBufferState(sceneLights_, nvrhi::ResourceStates::CopyDest);
    upload->writeBuffer(sceneLights_, lightData.data(), lightData.size() * sizeof(float));
    upload->setPermanentBufferState(sceneLights_, nvrhi::ResourceStates::ShaderResource);
    upload->close();
    device->executeCommandList(upload);

    // ---- 7. 联合包围球：局部球经各实例世界矩阵变换后取并集 → assetSphere_ ----
    // 球心乘矩阵、半径乘三列最大尺度（非均匀 scale 近似，Carrot/UE 同病）。
    float gmn[3] = { 0.0f, 0.0f, 0.0f }, gmx[3] = { 0.0f, 0.0f, 0.0f };
    bool first = true;
    for (std::size_t b = 0; b < batchFiles.size(); ++b) {
        const float* sph = prepared[b].sphere;
        for (std::uint32_t oi : batchObjects[b]) {
            const float* w = objects[oi].world;
            // 世界球心 = M * center（列主序列向量变换）。
            const float cx = w[0] * sph[0] + w[4] * sph[1] + w[8]  * sph[2] + w[12];
            const float cy = w[1] * sph[0] + w[5] * sph[1] + w[9]  * sph[2] + w[13];
            const float cz = w[2] * sph[0] + w[6] * sph[1] + w[10] * sph[2] + w[14];
            const float sx = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            const float sy = std::sqrt(w[4] * w[4] + w[5] * w[5] + w[6] * w[6]);
            const float sz = std::sqrt(w[8] * w[8] + w[9] * w[9] + w[10] * w[10]);
            const float r = sph[3] * std::max(sx, std::max(sy, sz));
            if (first) {
                // 第一个球也要按 ±r 展开包围盒（漏掉会让联合球偏小 → 自动取景过近）。
                gmn[0] = cx - r; gmx[0] = cx + r;
                gmn[1] = cy - r; gmx[1] = cy + r;
                gmn[2] = cz - r; gmx[2] = cz + r;
                first = false;
            } else {
                gmn[0] = std::min(gmn[0], cx - r); gmx[0] = std::max(gmx[0], cx + r);
                gmn[1] = std::min(gmn[1], cy - r); gmx[1] = std::max(gmx[1], cy + r);
                gmn[2] = std::min(gmn[2], cz - r); gmx[2] = std::max(gmx[2], cz + r);
            }
        }
    }
    assetSphere_[0] = (gmn[0] + gmx[0]) * 0.5f;
    assetSphere_[1] = (gmn[1] + gmx[1]) * 0.5f;
    assetSphere_[2] = (gmn[2] + gmx[2]) * 0.5f;
    {
        float radius = 0.0f;
        for (std::size_t b = 0; b < batchFiles.size(); ++b) {
            const float* sph = prepared[b].sphere;
            for (std::uint32_t oi : batchObjects[b]) {
                const float* w = objects[oi].world;
                const float cx = w[0] * sph[0] + w[4] * sph[1] + w[8]  * sph[2] + w[12];
                const float cy = w[1] * sph[0] + w[5] * sph[1] + w[9]  * sph[2] + w[13];
                const float cz = w[2] * sph[0] + w[6] * sph[1] + w[10] * sph[2] + w[14];
                const float sx = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                const float sy = std::sqrt(w[4] * w[4] + w[5] * w[5] + w[6] * w[6]);
                const float sz = std::sqrt(w[8] * w[8] + w[9] * w[9] + w[10] * w[10]);
                const float r = sph[3] * std::max(sx, std::max(sy, sz));
                const float dx = cx - assetSphere_[0], dy = cy - assetSphere_[1], dz = cz - assetSphere_[2];
                radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz) + r);
            }
        }
        assetSphere_[3] = radius;
    }

    // ---- 8. 计数与名字回带（backend → DeviceInfo → viewer HUD/日志）----
    modelName_ = sceneName;
    sceneInstanceCount_ = objectCount;
    triangleCount_ = 0;
    for (const SceneMeshBatch& b : sceneBatches_) triangleCount_ += b.indexCount / 3;

    nl::core::log::info("ecs scene ready: ", sceneName,
                        " | objects=", objectCount, " lights=", lightCount,
                        " ambient=", ambient,
                        " | batches=", sceneBatches_.size(),
                        " | triangles=", triangleCount_,
                        " | shader=", shaderSuffix);
    return true;
}

void Scene::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                 const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!ready() || !framebuffer) return;

    // M-E1b.1：ECS 场景路径——逐 batch 绑定各自的 set/VB/IB/t5 实例缓冲，
    // drawIndexed 的 SV_InstanceID 从 0 起，直接按 id*5 寻址本 batch 实例数据。
    // 不设 startInstanceLocation：DX12 不把 StartInstanceLocation 计入 SV_InstanceID，
    // 旧实现多 batch 时所有批次都读实例槽 0（详见第 5 步与 MILESTONES 踩坑）。
    if (sceneMode_) {
        SceneConstants cb{};
        std::memcpy(cb.mvp, params.mvp, sizeof(cb.mvp));
        const float identity[16] = { 1.f, 0.f, 0.f, 0.f,   // model = 单位阵
                                     0.f, 1.f, 0.f, 0.f,   //（世界矩阵在 vsScene 里乘）
                                     0.f, 0.f, 1.f, 0.f,
                                     0.f, 0.f, 0.f, 1.f };
        std::memcpy(cb.model, identity, sizeof(cb.model));
        std::memcpy(cb.lightDir, params.lightDir, sizeof(cb.lightDir));  // w = ambient（viewer 透传）
        cb.options[0] = 0.0f;   // psScene 不读 options；保持 0
        cmd->writeBuffer(constants_, &cb, sizeof(cb));   // volatile CB 每帧写一版（maxVersions=16）

        // M-E1b.3：b1 每帧写相机世界坐标（psScene 视向量 V 来源）。其余字段 E1b 不用。
        LightConstants lc{};
        lc.cameraPos[0] = params.eye[0];
        lc.cameraPos[1] = params.eye[1];
        lc.cameraPos[2] = params.eye[2];
        cmd->writeBuffer(lightConstants_, &lc, sizeof(lc));

        nvrhi::GraphicsPipelineHandle pipeline = params.wireframe
            ? (params.cullBack ? pipelineSceneWireBack_ : pipelineSceneWireNone_)
            : (params.cullBack ? pipelineSceneBack_ : pipelineSceneNone_);
        if (!pipeline) return;

        for (const SceneMeshBatch& b : sceneBatches_) {
            nvrhi::GraphicsState state;
            state.setPipeline(pipeline)
                 .setFramebuffer(framebuffer)
                 .addBindingSet(b.set)
                 .addVertexBuffer(nvrhi::VertexBufferBinding()
                     .setBuffer(b.vb).setSlot(0).setOffset(0))
                 .setIndexBuffer(nvrhi::IndexBufferBinding()
                     .setBuffer(b.ib).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
            state.viewport.addViewportAndScissorRect(viewport);
            cmd->setGraphicsState(state);
            nvrhi::DrawArguments args;
            args.setVertexCount(b.indexCount)          // drawIndexed 复用 vertexCount 表示索引数
                .setInstanceCount(b.instanceCount);
            cmd->drawIndexed(args);
        }
        return;   // sceneMode_ 分支必须 return，不走既有路径
    }

    // M4.0（S4.0）：vertex pulling 路径——无顶点缓冲，几何由 VS 从 SSBO 拉取，
    // 单 drawIndexedIndirect（instanceCount=总簇数）全簇全画。
    if (cull_) {
        // ---- M7.0（S7.0）：vis buffer 三段式（Carrot 的 clear → rasterize → debug
        // 编排；M7.1 把 debug 换成 material pass）。frustum 恒关 = 全簇全画，
        // 遮挡由硬件深度测试 + 64 位 atomicMin 共同决定。----
        if (visbuf_) {
            if (!visbufFailed_) {
                std::string visErr;
                if (!ensureVisbufTargets(visbufDevice_, viewport.maxX - viewport.minX,
                                         viewport.maxY - viewport.minY, visErr)) {
                    nl::core::log::deviceError("visbuf", visErr);
                    visbufFailed_ = true;   // 只报一次，后续帧跳过 vis 段
                }
            }
            if (!visbufFailed_ && visRasterPSO_ && visVizPSO_) {
                // (1) b0 只写 MVP（visbuf.slang 的 b0 与 model.slang 同 160B 布局）。
                SceneConstants cb{};
                std::memcpy(cb.mvp, params.mvp, sizeof(cb.mvp));
                std::memcpy(cb.model, params.model, sizeof(cb.model));
                std::memcpy(cb.lightDir, params.lightDir, sizeof(cb.lightDir));
                cb.options[0] = static_cast<float>(params.shading);
                cmd->writeBuffer(constants_, &cb, sizeof(cb));

                // (2) b1 = (宽, 高, 可视化档, 未用)。宽高用 float 传递，像素数
                // ≤2^24 时无损；shader 侧 uint() 还原。
                const float vc[4] = {static_cast<float>(visW_), static_cast<float>(visH_),
                                     static_cast<float>(params.visVizMode), 0.f};
                cmd->writeBuffer(visConstants_, vc, sizeof(vc));

                // (3) clear = max。clearBufferUInt 把整块 buffer 的每个 u32 写成
                // 0xFFFFFFFF → 每个 u64 = 0xFFFFFFFFFFFFFFFF = VIS_EMPTY；
                // 任意真实写入的打包值都小于它，故不需要 UE 的 cluster+1 空哨兵。
                // 必须在首个 setGraphicsState 之前（Vulkan 实现会 endRenderPass）。
                cmd->clearBufferUInt(visBuffer_, 0xFFFFFFFFu);

                // (4) 簇光栅：PS 对 u64 做 InterlockedMin，深度测试照旧。
                nvrhi::GraphicsState rstate;
                rstate.setPipeline(visRasterPSO_)
                      .setFramebuffer(framebuffer)
                      .addBindingSet(visRasterSet_)
                      .setIndexBuffer(nvrhi::IndexBufferBinding()
                          .setBuffer(identityIndex_).setFormat(nvrhi::Format::R32_UINT)
                          .setOffset(0));
                rstate.viewport.addViewportAndScissorRect(viewport);
                rstate.setIndirectParams(indirectArgs_);
                cmd->setGraphicsState(rstate);
                cmd->drawIndexedIndirect(0, 1);

                // (5) 全屏 viz：读同一块 buffer 的 SRV 视角，解包 → 簇色/三角色。
                // 深度测试/写入关，整屏覆盖 (4) 的占位输出。
                nvrhi::GraphicsState vstate;
                vstate.setPipeline(visVizPSO_)
                      .setFramebuffer(framebuffer)
                      .addBindingSet(visVizSet_);
                vstate.viewport.addViewportAndScissorRect(viewport);
                cmd->setGraphicsState(vstate);
                nvrhi::DrawArguments vargs;
                vargs.setVertexCount(3);   // 覆盖屏的大三角形
                cmd->draw(vargs);
            }
            return;
        }

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
            // M6.2：dag 模式两个槽（tris + occluded）一起清；cull 模式 buffer 仍
            // 1 uint 只清 [0]。
            const std::uint32_t triZero[2] = {0, 0};
            cmd->writeBuffer(triCounter_, &triZero,
                             dag_ ? sizeof(triZero) : sizeof(std::uint32_t));

            // M5.1：dag 遍历换 dag.slang csDag（显式栈）+ 自己的绑定/常量；
            // M4.1 路径保持原样。输出接口（visibleList + args）两者完全一致。
            if (dag_) {
                // M6.1：DagConstants 扩到 128B（4×float4 + view 矩阵）。
                // hizCullActive = hiz 模式且金字塔已就绪且用户开关开（M6.2，
                // FrameParams.hizEnable：关 = 只做视锥/误差遍历，退化可验证）——
                // 遍历消费的是**上一帧**金字塔（本帧金字塔在下面的预通道段才建，
                // 一帧延迟，S6-PLAN 明文接受；UE OCCLUSION_MAIN / bevy first pass
                // 同口径）。
                const bool hizCullActive = hiz_ && !hizFailed_ && hizReady_
                                           && params.hizEnable;
                const float dc[32] = {
                    params.projYScale, params.viewportHeight, params.nearPlane,
                    params.cutThresholdPx,
                    params.eye[0], params.eye[1], params.eye[2],
                    static_cast<float>(dagRootCount_),
                    // 根簇全局基址 = 总簇数 − 最粗层簇数（最粗层拼在最后）。
                    static_cast<float>(clusterCount_ - dagRootCount_), 0.f, 0.f, 0.f,
                    hizCullActive ? 1.f : 0.f,                    // [12] g_dagHiz.x 开关
                    static_cast<float>(viewport.maxX - viewport.minX),  // [13] 视口宽
                    0.f, 0.f,
                    // [16..31] g_dagView = world→view（Mara&Morgan 球投影；与
                    // camera.view() 同源，viewer 每帧填 FrameParams.view）。
                    params.view[0],  params.view[1],  params.view[2],  params.view[3],
                    params.view[4],  params.view[5],  params.view[6],  params.view[7],
                    params.view[8],  params.view[9],  params.view[10], params.view[11],
                    params.view[12], params.view[13], params.view[14], params.view[15]};
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
            {
                const bool hizCullActive = hiz_ && !hizFailed_ && hizReady_
                                           && params.hizEnable;   // M6.2 开关同口径
                cstate.setPipeline(dag_ ? dagComputePSO_ : cullComputePSO_)
                      .addBindingSet(dag_ ? (hizCullActive ? dagHizSet_ : dagComputeSet_)
                                          : cullComputeSet_);
            }
            cmd->setComputeState(cstate);
            const std::uint32_t threads = dag_ ? dagRootCount_ : clusterCount_;
            cmd->dispatch((threads + 127u) / 128u, 1, 1);

            // 回读拷贝：槽位 [0]=args[1]（可见簇数，offset 4 字节处）、[4]=triCounter[0]
            // （可见三角数）；M6.2 起 dag 模式再加 [8]=triCounter[1]（被 Hi-Z 剔除
            // 簇节点数）。pollCullStats 延迟两帧一起消费。cull 模式回读环形仍 2 uint
            // （initCull 建的），第三段只 dag 模式拷。
            cmd->copyBuffer(cullReadback_[cullSubmitted_ & 1u], 0,
                            indirectArgs_, sizeof(std::uint32_t), sizeof(std::uint32_t));
            cmd->copyBuffer(cullReadback_[cullSubmitted_ & 1u], sizeof(std::uint32_t),
                            triCounter_, 0, sizeof(std::uint32_t));
            if (dag_) {
                cmd->copyBuffer(cullReadback_[cullSubmitted_ & 1u],
                                2 * sizeof(std::uint32_t), triCounter_,
                                sizeof(std::uint32_t), sizeof(std::uint32_t));
            }

            // ---- M6.0：Hi-Z 深度预通道 + 金字塔（hiz 模式）----
            // 顺序：遍历 compute（上面，消费**上一帧**金字塔做遮挡测试）→ 深度
            // 预通道（mip0 = 视点径向距离）→ 逐级 2x2 max（M6.1 拍板：最远表面
            // 口径）→ 最高 mip 落 4B 计数器。全部发生在主绘制之前；本段产出的
            // 金字塔供下一帧遍历使用（一帧延迟，S6-PLAN 明文接受）。
            if (hiz_ && !hizFailed_) {
                std::string hizErr;
                if (!ensureHizTargets(hizDevice_, viewport.maxX - viewport.minX,
                                      viewport.maxY - viewport.minY, hizErr)) {
                    nl::core::log::deviceError("hiz", hizErr);
                    hizFailed_ = true;   // 只报一次，后续帧跳过 hiz 段（主绘制不受影响）
                } else {
                    // farRange：可视化对比度参考 = 相机到资产中心距离 + 半径。
                    const float ecx = params.eye[0] - assetSphere_[0];
                    const float ecy = params.eye[1] - assetSphere_[1];
                    const float ecz = params.eye[2] - assetSphere_[2];
                    hizFarRange_ = std::sqrt(ecx * ecx + ecy * ecy + ecz * ecz) +
                                   std::max(assetSphere_[3], 1.0f);
                    const float w = static_cast<float>(hizW_);
                    const float h = static_cast<float>(hizH_);

                    // M6.1：金字塔刚（重）建 → 帧内 cmd 补全 mip 清 1e9（保守缺省，
                    // 防下一帧遍历读到未定义数据假剔除）。mip0 的常规清屏在下面。
                    if (hizJustCreated_) {
                        cmd->clearTextureFloat(hizPyramid_,
                            nvrhi::TextureSubresourceSet(0, hizMipCount_, 0,
                                                         nvrhi::TextureSubresourceSet::AllArraySlices),
                            nvrhi::Color(1e9f, 1e9f, 1e9f, 1e9f));
                        hizJustCreated_ = false;
                    }

                    // 预通道常量：viewMip=0，dims=mip0 尺寸。
                    {
                        const float hc[12] = {
                            params.eye[0], params.eye[1], params.eye[2], hizFarRange_,
                            0.f, 0.f, 0.f, 0.f,
                            w, h, w, h};
                        cmd->writeBuffer(hizConstants_, hc, sizeof(hc));
                    }
                    // 空 mip0 像素 = 大值 = 不遮挡任何东西（保守方向的正确缺省）；
                    // 预通道深度清 1.0（far）。
                    cmd->clearTextureFloat(hizPyramid_,
                        nvrhi::TextureSubresourceSet(0, 1, 0,
                                                     nvrhi::TextureSubresourceSet::AllArraySlices),
                        nvrhi::Color(1e9f, 1e9f, 1e9f, 1e9f));
                    cmd->clearDepthStencilTexture(hizDepth_, nvrhi::TextureSubresourceSet(),
                                                  true, 1.0f, false, 0);

                    nvrhi::GraphicsState pstate;
                    pstate.setPipeline(prepassPSO_)
                          .setFramebuffer(prepassFramebuffer_)
                          .addBindingSet(hizPrepassSet_)
                          .setIndexBuffer(nvrhi::IndexBufferBinding()
                              .setBuffer(identityIndex_).setFormat(nvrhi::Format::R32_UINT)
                              .setOffset(0));
                    pstate.viewport.addViewportAndScissorRect(viewport);
                    pstate.setIndirectParams(indirectArgs_);
                    cmd->setGraphicsState(pstate);
                    cmd->drawIndexedIndirect(0, 1);

                    // 金字塔逐级 min：每级一版 HizConstants（src/tgt 尺寸）+ 一组绑定。
                    {
                        const std::uint32_t topZero = 0xFFFFFFFFu;
                        cmd->writeBuffer(hizTop_, &topZero, sizeof(topZero));
                    }
                    for (std::uint32_t L = 1; L < hizMipCount_; ++L) {
                        const float sw = static_cast<float>(std::max(1u, hizW_ >> (L - 1)));
                        const float sh = static_cast<float>(std::max(1u, hizH_ >> (L - 1)));
                        const float tw = static_cast<float>(std::max(1u, hizW_ >> L));
                        const float th = static_cast<float>(std::max(1u, hizH_ >> L));
                        const float hcL[12] = {
                            params.eye[0], params.eye[1], params.eye[2], hizFarRange_,
                            static_cast<float>(L), 0.f, 0.f, 0.f,
                            sw, sh, tw, th};
                        cmd->writeBuffer(hizConstants_, hcL, sizeof(hcL));
                        nvrhi::ComputeState cst;
                        cst.setPipeline(chainPSO_).addBindingSet(chainSets_[L]);
                        cmd->setComputeState(cst);
                        const std::uint32_t tgw = std::max(1u, hizW_ >> L);
                        const std::uint32_t tgh = std::max(1u, hizH_ >> L);
                        cmd->dispatch((tgw + 7u) / 8u, (tgh + 7u) / 8u, 1);
                    }

                    // 最高 mip（1 texel）→ 计数器 → 回读环形槽位偏移 12
                    //（M6.2 起 [12]；[8] 让位给 occluded 计数）。
                    {
                        const float hcT[12] = {
                            params.eye[0], params.eye[1], params.eye[2], hizFarRange_,
                            static_cast<float>(hizMipCount_ - 1), 0.f, 0.f, 0.f,
                            1.f, 1.f, 1.f, 1.f};
                        cmd->writeBuffer(hizConstants_, hcT, sizeof(hcT));
                        nvrhi::ComputeState tst;
                        tst.setPipeline(topPSO_).addBindingSet(topSet_);
                        cmd->setComputeState(tst);
                        cmd->dispatch(1, 1, 1);
                    }
                    cmd->copyBuffer(cullReadback_[cullSubmitted_ & 1u],
                                    3 * sizeof(std::uint32_t), hizTop_, 0,
                                    sizeof(std::uint32_t));
                }
            }
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

        // M6.0：右下角 160x160 层级可视化覆盖层（只读金字塔，近亮远暗）。
        // 画在主绘制之后、HUD 之前——截图基线里可见 = 层级可视化的验收证据。
        if (hiz_ && !hizFailed_ && prepassPSO_ && vizPSO_) {
            // vsHizViz/psHizViz 从 g_hizViz.y/z（[1].y/[1].z）读视口宽高、
            // g_hizViz.x 读 mip 号——viz 路径 dims 必须放 [1]，不能放 [2]。
            const float hcV[12] = {
                params.eye[0], params.eye[1], params.eye[2], hizFarRange_,
                static_cast<float>(std::clamp(params.hizViewMip, 0,
                                              static_cast<int>(hizMipCount_) - 1)),
                static_cast<float>(hizW_), static_cast<float>(hizH_), 0.f,
                0.f, 0.f, 0.f, 0.f};
            cmd->writeBuffer(hizConstants_, hcV, sizeof(hcV));
            nvrhi::GraphicsState vstate;
            vstate.setPipeline(vizPSO_)
                  .setFramebuffer(framebuffer)
                  .addBindingSet(vizSet_);
            vstate.viewport.addViewportAndScissorRect(viewport);
            cmd->setGraphicsState(vstate);
            nvrhi::DrawArguments vargs;
            vargs.setVertexCount(4);   // triangle-strip 覆盖四边形
            cmd->draw(vargs);
        }
        return;
    }

    // M-A5：CPU 蒙皮——采样 clip[0] → FK → 调色板(model*IBM) → LBS → 动态 VB。
    // VB 是 CopyDest+keepInitialState（hud.cpp 同款）：每帧命令列表开头自动回到
    // CopyDest，writeBuffer 可反复执行，无需手工状态跟踪。
    if (skinMode_) {
        const nl::anim::AnimClip& clip = skinClips_[0];
        const float t = nl::anim::wrapTime(params.animTime, clip.duration);
        if (!nl::anim::sampleClip(clip, t, skinLocal_) ||
            !nl::anim::localToModel(skinLocal_, skinSkeleton_, skinModel_))
            return;
        nl::anim::computePalette(skinPalette_, skinModel_, skinBinding_.inverseBind);
        nl::anim::skinPositions(skinPos_, skinBasePos_, skinPalette_, skinBinding_);
        nl::anim::skinNormals(skinNrm_, skinBaseNrm_, skinPalette_, skinBinding_);
        for (std::size_t i = 0; i < skinVertices_.size(); ++i) {
            skinVertices_[i].px = skinPos_[i].x - skinCenter_.x;   // 后蒙皮平移居中
            skinVertices_[i].py = skinPos_[i].y - skinCenter_.y;
            skinVertices_[i].pz = skinPos_[i].z - skinCenter_.z;
            skinVertices_[i].nx = skinNrm_[i].x;
            skinVertices_[i].ny = skinNrm_[i].y;
            skinVertices_[i].nz = skinNrm_[i].z;
        }
        cmd->writeBuffer(vertexBuffer_, skinVertices_.data(),
                         skinVertices_.size() * sizeof(SceneVertex));
    }

    SceneConstants cb{};
    std::memcpy(cb.mvp, params.mvp, sizeof(cb.mvp));
    std::memcpy(cb.model, params.model, sizeof(cb.model));
    std::memcpy(cb.lightDir, params.lightDir, sizeof(cb.lightDir));
    cb.options[0] = static_cast<float>(params.shading);   // M0.8：g_options.x = 着色模式
    std::memcpy(cb.options + 1, params.options, sizeof(cb.options) - sizeof(float));
    cmd->writeBuffer(constants_, &cb, sizeof(cb));   // volatile CB 每帧写一版（maxVersions=16）

    // pbr-direct：b1 LightConstants 同款 volatile 每帧写。只有 init/initSkin 的
    // 绑定集携带 b1（psMain 引用）；initCluster 场景（lod 模式同样走这里）没有
    // b1 槽位、句柄为空，绝不能碰——写空句柄 = NVRHI 内部空指针段错误。
    if (lightConstants_) {
        LightConstants lc{};
        lc.lightDirW[0] = params.lightDir[0];   // 方向沿用 b0 g_lightDir 的口径与缺省值
        lc.lightDirW[1] = params.lightDir[1];
        lc.lightDirW[2] = params.lightDir[2];
        lc.lightColor[0] = 1.0f; lc.lightColor[1] = 1.0f; lc.lightColor[2] = 1.0f;
        lc.lightColor[3] = 2.5f;                // 白光强度 2.5：金属体无 IBL 只剩直射高光
        lc.cameraPos[0] = params.eye[0];
        lc.cameraPos[1] = params.eye[1];
        lc.cameraPos[2] = params.eye[2];
        std::memcpy(lc.baseColorFactor, pbrBaseColorFactor_, sizeof(lc.baseColorFactor));
        lc.mrFactor[0] = pbrMetallicFactor_;
        lc.mrFactor[1] = pbrRoughnessFactor_;
        lc.mrFactor[2] = params.options[0];     // --pbr-naive 对照开关（viewer 填 1）
        cmd->writeBuffer(lightConstants_, &lc, sizeof(lc));
    }

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
    // M-E1b.0：ECS 场景资源（先于既有路径清空；任何模式下都安全）。
    sceneMode_ = false;
    vsScene_ = nullptr;
    psScene_ = nullptr;
    sceneLights_ = nullptr;
    sceneInputLayout_ = nullptr;
    sceneBindingLayout_ = nullptr;
    sceneBatches_.clear();
    pipelineSceneBack_ = nullptr;
    pipelineSceneNone_ = nullptr;
    pipelineSceneWireBack_ = nullptr;
    pipelineSceneWireNone_ = nullptr;
    sceneAmbient_ = 0.28f;
    sceneLightCount_ = 0;
    sceneInstanceCount_ = 0;
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
    dagHizSet_ = nullptr;      // M6.1：hiz 态遍历绑定集
    hizDummyTex_ = nullptr;    // M6.1：t8 占位纹理
    hizReady_ = false;         // M6.1：金字塔就绪标志
    visibleList_ = nullptr;
    clusterSpheres_ = nullptr;
    for (auto& rb : cullReadback_) rb = nullptr;
    triCounter_ = nullptr;   // M5.2：GPU 原子三角计数器
    // M6.0：Hi-Z 金字塔资源。
    hiz_ = false;
    hizFailed_ = false;
    hizDevice_ = nullptr;
    hizMipCount_ = 0;
    hizW_ = 0;
    hizH_ = 0;
    vsHizDepth_ = nullptr;
    psHizDepth_ = nullptr;
    vsHizViz_ = nullptr;
    psHizViz_ = nullptr;
    csHizDown_ = nullptr;
    csHizTop_ = nullptr;
    hizConstants_ = nullptr;
    hizTop_ = nullptr;
    hizPyramid_ = nullptr;
    hizDepth_ = nullptr;
    prepassFramebuffer_ = nullptr;
    prepassPSO_ = nullptr;
    vizPSO_ = nullptr;
    hizPrepassLayout_ = nullptr;
    hizPrepassSet_ = nullptr;
    vizLayout_ = nullptr;
    vizSet_ = nullptr;
    chainLayout_ = nullptr;
    chainSets_.clear();
    chainPSO_ = nullptr;
    topLayout_ = nullptr;
    topSet_ = nullptr;
    topPSO_ = nullptr;
    hizTopFar_ = -1.0f;
    hizOccluded_ = -1;
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
    // M7.0：vis buffer 资源必须在 device 存活期间释放（同上）。
    visbuf_ = false;
    visbufDevice_ = nullptr;
    visbufFailed_ = false;
    visW_ = visH_ = 0;
    visRasterPSO_ = nullptr;
    visVizPSO_ = nullptr;
    visRasterSet_ = nullptr;
    visVizSet_ = nullptr;
    visRasterLayout_ = nullptr;
    visVizLayout_ = nullptr;
    visBuffer_ = nullptr;
    visConstants_ = nullptr;
    vsVisRaster_ = nullptr;
    psVisRaster_ = nullptr;
    vsVisViz_ = nullptr;
    psVisViz_ = nullptr;
    maxTriPerCluster_ = 0;
    // pbr-direct：新资源句柄同样要在 device 存活期间释放。
    mrTexture_ = nullptr;
    lightConstants_ = nullptr;
    constants_ = nullptr;
    indexBuffer_ = nullptr;
    vertexBuffer_ = nullptr;
    ps_ = nullptr;
    vs_ = nullptr;
    indexCount_ = 0;
}

} // namespace nl::renderer::detail
