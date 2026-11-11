#include "scene.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）
#include "nl/math/math.hpp" // M8.2：inverseMat4（viewProj 求逆做射线生成）

#include "nl/core/log.hpp"
#include "nl/core/mesh_io.hpp"     // M1.0：glTF 解析抽到 core，与 nanite-tools 共用
#include "nl/core/meshlet_io.hpp"  // M1.3：cluster 场景吃 .nmesh 资产
#include "json.hpp"                // M-PT1：.nfx v1 解析（vendored nlohmann，与 core 同源）

// M-PT2：粒子贴图解码用 stb_image 声明头。实现（STB_IMAGE_IMPLEMENTATION）只在
// nl_core 的 mesh_io.cpp 展开（tinygltf 内嵌 v2.21；thirdparty/stb/stb_image.h 为
// 同版本声明副本）——renderer 侧只引声明复用符号，禁止重复展开（重复定义链接错）。
#ifdef _MSC_VER
#  pragma warning(push, 0)
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include <cmath>
#include <algorithm>
#include <cstdlib>
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
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
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
    // M7.2：多材质对照组（D-M7.2-1）。所有 primitive 拼进共享 VB/IB，每 prim
    // 一套绑定（t0/t1 贴图 + b1 因子）按区间 drawIndexed。单 prim 资产 = 1 批，
    // 行为与 M0.4 逐字节一致（同一份顶点/索引/贴图/b1 内容，model.slang 零改动）。
    for (const auto& prim : meshData.meshes) {
        if (prim.indices.empty()) {
            outError = modelFile + std::string(" has a primitive with no indices");
            return false;
        }
    }
    const nl::core::PrimitiveMesh& prim0 = meshData.meshes[0];
    if (!prim0.hasBaseColor) {
        outError = modelFile + std::string(" has no base color texture"); return false;
    }
    size_t vertexCount = 0;
    std::uint32_t totalIndexCount = 0;
    for (const auto& p : meshData.meshes) {
        vertexCount += p.vertices.size();
        totalIndexCount += static_cast<std::uint32_t>(p.indices.size());
    }
    std::vector<SceneVertex> vertices;
    vertices.reserve(vertexCount);
    std::vector<std::uint32_t> indexAccum;
    indexAccum.reserve(totalIndexCount);
    struct PrimSpan { std::uint32_t firstIndex; std::uint32_t indexCount; };
    std::vector<PrimSpan> primSpans;
    primSpans.reserve(meshData.meshes.size());
    for (const auto& p : meshData.meshes) {
        const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
        for (const auto& mv : p.vertices) {
            vertices.push_back({ mv.pos[0], mv.pos[1], mv.pos[2],
                                 mv.normal[0], mv.normal[1], mv.normal[2],
                                 mv.uv[0], mv.uv[1] });
        }
        // prim 索引是子网格局部的 → 拼接后统一加顶点基址
        const std::uint32_t firstIndex = primSpans.empty()
            ? 0u : primSpans.back().firstIndex + primSpans.back().indexCount;
        for (std::uint32_t idx : p.indices)
            indexAccum.push_back(idx + base);
        primSpans.push_back({firstIndex,
                             static_cast<std::uint32_t>(p.indices.size())});
    }
    const std::vector<std::uint32_t>& indices = indexAccum;
    indexCount_ = totalIndexCount;
    const auto texWidth = prim0.texWidth;
    const auto texHeight = prim0.texHeight;

    // ---- 3. 建 GPU 资源（debug 名带模型名，RenderDoc / validation 里分得清）----
    const std::string vbName = std::string(modelFile) + "VB";
    const std::string ibName = std::string(modelFile) + "IB";
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

    // M7.2：每 prim 一套贴图 + 因子 + 绑定集（共享 layout/sampler/constants_），
    // 几何共享 VB/IB 按 drawIndexed(startIndex) 分区间画。缺贴图的 prim 用
    // 1x1 dummy（白基色 / roughness=1 metallic=0），与 initCull 的占位语义一致。
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed"; return false; }
    modelBatches_.clear();
    nvrhi::TextureHandle prim0BaseColor, prim0MR;
    std::vector<nvrhi::TextureHandle> bcHandles, mrHandles;
    for (size_t pi = 0; pi < meshData.meshes.size(); ++pi) {
        const nl::core::PrimitiveMesh& prim = meshData.meshes[pi];
        const std::string suffix = std::to_string(pi);
        nvrhi::TextureHandle bcHandle;
        {
            nvrhi::TextureDesc bcDesc;
            if (prim.hasBaseColor) {
                bcDesc.width = prim.texWidth;
                bcDesc.height = prim.texHeight;
            } else {
                bcDesc.width = 1;
                bcDesc.height = 1;
            }
            bcDesc.format = nvrhi::Format::RGBA8_UNORM;
            bcDesc.debugName = (std::string(modelFile) + "BaseColor" + suffix).c_str();
            bcDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            bcDesc.keepInitialState = true;
            bcHandle = device->createTexture(bcDesc);
            if (!bcHandle) { outError = "createTexture failed for base color " + suffix; return false; }
        }
        // pbr-direct：metallicRoughness 贴图（t1）。RGBA8_UNORM——MR 是线性数据
        // （G=roughness B=metallic，glTF 2.0 spec §3.9.3），千万别学 baseColor 上
        // SRGB 视图格式。无 MR 贴图的材质退 1x1 dummy（roughness=1, metallic=0）。
        nvrhi::TextureHandle mrHandle;
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
            mrDesc.debugName = (std::string(modelFile) + "MetallicRoughness" + suffix).c_str();
            mrHandle = device->createTexture(mrDesc);
            if (!mrHandle) { outError = "createTexture failed for metallicRoughness " + suffix; return false; }
        }
        // 每 prim 独立 b1（因子不同；光照/相机每帧同值写入各批）。
        nvrhi::BufferHandle lcBuf;
        {
            nvrhi::BufferDesc lcDesc;
            lcDesc.byteSize = sizeof(LightConstants);
            lcDesc.isConstantBuffer = true;
            lcDesc.isVolatile = true;
            lcDesc.maxVersions = 16;
            lcDesc.debugName = ("LightConstants" + suffix).c_str();
            lcBuf = device->createBuffer(lcDesc);
            if (!lcBuf) { outError = "createBuffer failed for light constants " + suffix; return false; }
        }

        // 布局槽位与 model.slang 的 register(b0/t0/t1/s0/b0/b1) 一致；
        // Vulkan 侧由 NVRHI 默认 VulkanBindingOffsets 映射为 CB=256 / SRV=0 / Sampler=128，
        // 与 slangc 的 -fvk-b-shift 256 -fvk-s-shift 128 严格对应。b1 = LightConstants
        //（slot 1，pbr-direct 新开；SceneConstants 160 B 老规矩不动）。
        if (!bindingLayout_) {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::All)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0))
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
            bindingLayout_ = device->createBindingLayout(layoutDesc);
            if (!bindingLayout_) { outError = "createBindingLayout failed"; return false; }
        }
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, bcHandle))
               .addItem(nvrhi::BindingSetItem::Texture_SRV(1, mrHandle))
               .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
               .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
               .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, lcBuf));
        ModelPrimBatch batch;
        batch.set = device->createBindingSet(setDesc, bindingLayout_);
        if (!batch.set) { outError = "createBindingSet failed for prim " + suffix; return false; }
        batch.lightConstants = lcBuf;
        batch.startIndex = primSpans[pi].firstIndex;
        batch.indexCount = primSpans[pi].indexCount;
        std::memcpy(batch.baseColorFactor, prim.baseColorFactor,
                    sizeof(batch.baseColorFactor));
        batch.metallicFactor = prim.metallicFactor;
        batch.roughnessFactor = prim.roughnessFactor;
        modelBatches_.push_back(batch);
        bcHandles.push_back(bcHandle);
        mrHandles.push_back(mrHandle);
        if (pi == 0) { prim0BaseColor = bcHandle; prim0MR = mrHandle; }
    }
    // 老 fields 指向 prim 0（既有消费者/日志语义不变）。
    texture_ = prim0BaseColor;
    mrTexture_ = prim0MR;
    lightConstants_ = modelBatches_[0].lightConstants;
    bindingSet_ = modelBatches_[0].set;
    std::memcpy(pbrBaseColorFactor_, prim0.baseColorFactor, sizeof(pbrBaseColorFactor_));
    pbrMetallicFactor_  = prim0.metallicFactor;
    pbrRoughnessFactor_ = prim0.roughnessFactor;

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
    // pbr-direct：MR 贴图同一命令列表上传（线性数据，无 gamma 语义）；M7.2
    // 起 per-prim 贴图逐套上传（句柄由创建循环的 bcHandles/mrHandles 给出）。
    for (size_t pi = 0; pi < meshData.meshes.size(); ++pi) {
        const nl::core::PrimitiveMesh& prim = meshData.meshes[pi];
        if (prim.hasBaseColor) {
            upload->writeTexture(bcHandles[pi], 0, 0, prim.baseColorRGBA.data(),
                                 static_cast<size_t>(prim.texWidth) * 4u);
        } else {
            const std::uint8_t white[4] = {255u, 255u, 255u, 255u};
            upload->writeTexture(bcHandles[pi], 0, 0, white, 4u);
        }
        if (prim.hasMR) {
            upload->writeTexture(mrHandles[pi], 0, 0, prim.metallicRoughnessRGBA.data(),
                                 static_cast<size_t>(prim.mrWidth) * 4u);
        } else {
            const std::uint8_t dummyMR[4] = {0u, 255u, 0u, 255u};   // roughness=1, metallic=0
            upload->writeTexture(mrHandles[pi], 0, 0, dummyMR, 4u);
        }
    }
    upload->close();
    device->executeCommandList(upload);

    // M0.7：模型名与三角形数带回给 backend（DeviceInfo）→ viewer（HUD 自证）。
    modelName_ = modelFile;
    triangleCount_ = indexCount_ / 3;

    nl::core::log::info("model ready: ", modelFile, " | vertices=", vertexCount,
                        " triangles=", triangleCount_, " indices=", indexCount_,
                        " | prims=", meshData.meshes.size(),
                        " | baseColor ", texWidth, "x", texHeight,
                        " | shader=", shaderSuffix);
    return true;
}

// ---- M1.3：cluster 场景。加载 .nmesh，每簇随机着色，簇窗口展开成全局索引 ----
bool Scene::initCluster(nvrhi::IDevice* device, const char* shaderSuffix,
                        const char* vsEntry, const char* psEntry,
                        const char* nmeshFile, std::string& outError) {
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
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
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
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
    // M8.0：资产带 fallback 段（v3 lod0）时顺路预建 BLAS（S8-PLAN：S8 直接读
    // S3 的 fallback，不重新生成）。v1 base 资产在 initBlas 内跳过，零扰动。
    if (!initBlas(device, nm, nmeshFile, outError)) return false;

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
    // clusterMeta：每簇 4 个 uint = [vertexBase, triCount, localBase, materialId]。
    // M7.2：槽 3 从 pad 升级为 materialId = 子网格序号（.nmesh NSUB 段约定，
    // D-M7.2-1 方案 A：归属构建期随簇记录）。单子网格文件无 NSUB 段恒 0，
    // cull/hiz/visbuf 光栅不读此槽（visbuf.slang 只读 0..2），老资产渲染零扰动。
    // M7.2：槽 3 从 pad 升级为 materialId = 子网格序号（.nmesh NSUB 段约定，
    // D-M7.2-1 方案 A：归属构建期随簇记录）。单子网格文件无 NSUB 段恒 0，
    // cull/hiz/visbuf 光栅不读此槽（visbuf.slang 只读 0..2），老资产渲染零扰动。
    const std::uint32_t clusterCount = static_cast<std::uint32_t>(nm.meshlets.size());
    std::vector<std::uint32_t> clusterMaterialId(clusterCount, 0);
    if (nm.submeshesPresent) {
        for (std::size_t s = 0; s < nm.submeshes.size(); ++s) {
            const auto& r = nm.submeshes[s];
            for (std::uint32_t c = 0; c < r.clusterCount; ++c)
                clusterMaterialId[r.clusterFirst + c] = static_cast<std::uint32_t>(s);
        }
    }
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
        clusterMeta.push_back(clusterMaterialId[m]);   // materialId（M7.2；老文件恒 0）
    }
    const std::uint32_t stride = maxTriCount * 3;       // identity 索引缓冲长度
    maxTriPerCluster_ = maxTriCount;                    // M7.0：visbuf triId 8 位断言
    nmeshSubmeshCount_ = nm.submeshesPresent             // M7.2：visbuf 材质数对账
                             ? static_cast<std::uint32_t>(nm.submeshes.size())
                             : 1u;

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
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
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

    // M8.0：fallback 在链最细层 lod0 的 'NFLB' 段（S3 落定），用它预建 BLAS。
    // initHiz 走本函数，dag/hiz 两模式由此覆盖；无 fallback 的资产跳过不拦路。
    if (!initBlas(device, levels[0],
                  (stem + ".lod0.nmesh").c_str(), outError)) return false;

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
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
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
                       const char* nmeshFile, bool pulling, std::string& outError) {
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

    // M7.1（S7.1）：材质反解 pass 初始化。复用 vis 簇资源（vertexPool/localIndices/
    // clusterMeta 来自 initCull），额外从同源 .glb 加载 PBR 贴图（initCull 只建
    // 了 1x1 白占位 + sampler，故这里替换 texture_、新建 mrTexture_/lightConstants_，
    // 并复用 initCull 的 sampler_），再建材质管线所需常量/布局/绑定集。
    if (pulling) {
        pulling_ = true;

        // 资产约定：.nmesh 与 .glb 同 stem（Suzanne.nmesh ⟷ Suzanne.glb）。
        std::string glb = std::string(nmeshFile);
        const std::string kNm(".nmesh");
        if (glb.size() >= kNm.size() &&
            glb.compare(glb.size() - kNm.size(), kNm.size(), kNm) == 0)
            glb.replace(glb.size() - kNm.size(), kNm.size(), ".glb");
        else
            glb += ".glb";
        const std::string glbPath = std::string(NL_ASSET_DIR "/") + glb;

        nl::core::SceneMeshData meshData;
        std::string loadWarning;
        if (!nl::core::loadGltfScene(glbPath.c_str(), meshData, loadWarning)) {
            outError = glb + std::string(": ") + loadWarning;
            return false;
        }
        if (!loadWarning.empty()) nl::core::log::debug(loadWarning);
        if (meshData.meshes.empty()) {
            outError = glb + std::string(" has no mesh primitive"); return false;
        }

        // ---- M7.2：多材质间接表（D-M7.2-1 方案 A）。每个 primitive 一个材质，
        // 数量与 .nmesh NSUB 段对账（错配 = 资产配对坏了，宁可显式失败也不静默
        // 错材质）。材质 i：基色 t4+i、MR t8+i、因子进 GPUMaterial[i]。
        const std::size_t materialCount = meshData.meshes.size();
        if (materialCount > kMaxVisMaterials) {
            outError = glb + std::string(" has ") + std::to_string(materialCount) +
                       " materials, visbuf material path allows " +
                       std::to_string(kMaxVisMaterials);
            return false;
        }
        if (materialCount != nmeshSubmeshCount_) {
            outError = glb + std::string(" has ") + std::to_string(materialCount) +
                       " materials but " + std::string(nmeshFile) + " has " +
                       std::to_string(nmeshSubmeshCount_) +
                       " submeshes (.nmesh/glb pairing broken)";
            return false;
        }

        std::vector<GPUMaterial> matTable(materialCount);
        for (std::size_t i = 0; i < materialCount; ++i) {
            const nl::core::PrimitiveMesh& prim = meshData.meshes[i];
            if (!prim.hasBaseColor) {
                outError = glb + std::string(" material ") + std::to_string(i) +
                           " has no base color texture";
                return false;
            }
            // 基色贴图（RGBA8_UNORM，无 SRGB 视图；srgbToLinear 在 shader 内）。
            {
                const std::string texName =
                    glb + "BaseColor" + std::to_string(i);
                nvrhi::TextureDesc texDesc;
                texDesc.width = prim.texWidth;
                texDesc.height = prim.texHeight;
                texDesc.format = nvrhi::Format::RGBA8_UNORM;
                texDesc.debugName = texName.c_str();
                texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
                texDesc.keepInitialState = true;
                matBaseColor_[i] = device->createTexture(texDesc);
                if (!matBaseColor_[i]) {
                    outError = "createTexture failed for material base color " +
                               std::to_string(i);
                    return false;
                }
            }
            // MR 贴图（无则 1x1 dummy：roughness=1, metallic=0）。
            {
                const std::string mrName =
                    glb + "MetallicRoughness" + std::to_string(i);
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
                mrDesc.debugName = mrName.c_str();
                matMR_[i] = device->createTexture(mrDesc);
                if (!matMR_[i]) {
                    outError = "createTexture failed for material MR " +
                               std::to_string(i);
                    return false;
                }
            }
            GPUMaterial& m = matTable[i];
            std::memcpy(m.baseColorFactor, prim.baseColorFactor, sizeof(m.baseColorFactor));
            m.mrFactor[0] = prim.metallicFactor;
            m.mrFactor[1] = prim.roughnessFactor;
            m.baseColorSlot = static_cast<std::uint32_t>(i);
            m.mrSlot = static_cast<std::uint32_t>(i);
        }
        // 材质 0 句柄沿用 texture_/mrTexture_ 老 fields（既有消费者语义不变）；
        // 因子 fields 保留材质 0 值（仅日志/兼容用途，shader 已改走材质表）。
        texture_ = matBaseColor_[0];
        mrTexture_ = matMR_[0];
        std::memcpy(pbrBaseColorFactor_, matTable[0].baseColorFactor,
                    sizeof(pbrBaseColorFactor_));
        pbrMetallicFactor_  = matTable[0].mrFactor[0];
        pbrRoughnessFactor_ = matTable[0].mrFactor[1];

        // 材质间接表 SSBO（t12）：静态上传一次，逐帧只读。
        {
            nvrhi::BufferDesc mtDesc;
            mtDesc.byteSize = static_cast<std::uint32_t>(matTable.size() *
                                                         sizeof(GPUMaterial));
            mtDesc.structStride = sizeof(GPUMaterial);
            mtDesc.debugName = "VisMaterialTable";
            materialTable_ = device->createBuffer(mtDesc);
            if (!materialTable_) {
                outError = "createBuffer failed for vis material table";
                return false;
            }
        }

        // 空槽填充：贴图数组槽位是固定 4 个，未用槽绑 dummy（白基色 / 中性 MR），
        // 否则 NVRHI 拒绝空句柄纹理绑定；shader 侧 NonUniformResourceIndex 只会
        // 命中 [0, materialCount)（clusterMeta 槽 3 由 NSUB 对账保证）。
        for (std::size_t i = materialCount; i < kMaxVisMaterials; ++i) {
            nvrhi::TextureDesc dDesc;
            dDesc.width = 1;
            dDesc.height = 1;
            dDesc.format = nvrhi::Format::RGBA8_UNORM;
            dDesc.debugName = "VisMaterialDummy";
            dDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            dDesc.keepInitialState = true;
            matBaseColor_[i] = device->createTexture(dDesc);
            matMR_[i] = matBaseColor_[i];
            if (!matBaseColor_[i]) {
                outError = "createTexture failed for material dummy";
                return false;
            }
        }

        // b1 LightConstants（材质 pass 的 PBR 直光用；initCull 未建，这里新建）。
        if (!lightConstants_) {
            nvrhi::BufferDesc lcDesc;
            lcDesc.byteSize = sizeof(LightConstants);
            lcDesc.isConstantBuffer = true;
            lcDesc.isVolatile = true;
            lcDesc.maxVersions = 16;
            lcDesc.debugName = "LightConstants";
            lightConstants_ = device->createBuffer(lcDesc);
            if (!lightConstants_) { outError = "createBuffer failed for material light constants"; return false; }
        }

        auto matPath = [shaderSuffix](const char* artifact) {
            return std::string(NL_SHADER_DIR) + "/material." + artifact + "." + shaderSuffix;
        };
        auto loadMatShader = [&](const char* artifact, nvrhi::ShaderType type,
                                 nvrhi::ShaderHandle& out) -> bool {
            std::vector<std::uint8_t> blob;
            if (!readFileBytes(matPath(artifact).c_str(), blob)) {
                outError = std::string("failed to load material shader ") + artifact +
                           " (" + shaderSuffix + ") from " NL_SHADER_DIR;
                return false;
            }
            nvrhi::ShaderDesc desc;
            desc.setShaderType(type).setEntryName("main");   // SPIR-V 恒 main；DX12 忽略
            out = device->createShader(desc, blob.data(), blob.size());
            if (!out) outError = std::string("createShader failed for material ") + artifact;
            return out != nullptr;
        };
        if (!loadMatShader("vsmatresolve", nvrhi::ShaderType::Vertex, vsMatResolve_) ||
            !loadMatShader("psmatresolve", nvrhi::ShaderType::Pixel, psMatResolve_))
            return false;

        // b2 MatResolveConstants（16B）：(width, height, _, _)。
        nvrhi::BufferDesc mrcDesc;
        mrcDesc.byteSize = 16;
        mrcDesc.isConstantBuffer = true;
        mrcDesc.isVolatile = true;
        mrcDesc.maxVersions = 16;
        mrcDesc.debugName = "MatResolveConstants";
        matResolveConstants_ = device->createBuffer(mrcDesc);
        if (!matResolveConstants_) { outError = "createBuffer failed for mat resolve constants"; return false; }

        // 材质 pass 布局（§2.7 单一 BindingSet）：槽位表抽进 addResolveLayoutItems，
        // 与 M9.0 direct.slang 的 compute 管线共用同一份定义（§2.18）。
        nvrhi::BindingLayoutDesc mDesc;
        addResolveLayoutItems(mDesc);
        matResolveLayout_ = device->createBindingLayout(mDesc);
        if (!matResolveLayout_) { outError = "createBindingLayout failed for mat resolve"; return false; }

        // 贴图与材质表上传（每材质 baseColor + MR，MR 无则 dummy）。
        nvrhi::CommandListHandle upload = device->createCommandList();
        upload->open();
        for (std::size_t i = 0; i < materialCount; ++i) {
            const nl::core::PrimitiveMesh& prim = meshData.meshes[i];
            upload->writeTexture(matBaseColor_[i], 0, 0, prim.baseColorRGBA.data(),
                                 static_cast<size_t>(prim.texWidth) * 4u);
            if (prim.hasMR) {
                upload->writeTexture(matMR_[i], 0, 0, prim.metallicRoughnessRGBA.data(),
                                     static_cast<size_t>(prim.mrWidth) * 4u);
            } else {
                const std::uint8_t dummyMR[4] = {0u, 255u, 0u, 255u};   // roughness=1, metallic=0
                upload->writeTexture(matMR_[i], 0, 0, dummyMR, 4u);
            }
        }
        upload->beginTrackingBufferState(materialTable_, nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(materialTable_, matTable.data(),
                            matTable.size() * sizeof(GPUMaterial));
        upload->setPermanentBufferState(materialTable_, nvrhi::ResourceStates::ShaderResource);
        upload->close();
        device->executeCommandList(upload);

        nl::core::log::info("visbuf material resolve ready: ", glb,
                            " | materials=", materialCount,
                            " | baseColor ", meshData.meshes[0].texWidth, "x",
                            meshData.meshes[0].texHeight,
                            " | shader=", shaderSuffix);
    }
    return true;
}

// ---- M8.0（S8.0）：BLAS 预建（S8-PLAN；NVRHI 签名见该文件预研 §1 速查表）----
bool Scene::initBlas(nvrhi::IDevice* device, const nl::core::NMeshData& nm,
                     const char* assetName, std::string& outError) {
    if (blasReady_) return true;   // initCull→initVisbuf / initDag→initHiz 双入口去重
    blasTimer_.setLabel("blas");   // resolve 日志标明这是 BLAS 构建实例（默认 label 会撞 "frame"）
    if (!nm.fallbackPresent || nm.fallbackVertices.empty() || nm.fallbackIndices.empty()) {
        // 无 fallback 段非错误：cull 模式吃 v1 base 资产、visbuf 吃 Suzanne-materials
        // （只有 NSUB 段），都不建。dag/hiz 模式 levels[0] 恒为 v3 lod0（带 NFLB）。
        nl::core::log::info("blas: no fallback section in ", assetName,
                            " — skipped (S8 BLAS needs a v3 lod0 with 'NFLB')");
        return true;
    }

    // 1. fallback 几何 build-input 缓冲（Vulkan 侧需要
    //    ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY usage，由 NVRHI 按
    //    isAccelStructBuildInput 映射；DX12 靠状态跟踪）。
    const std::uint32_t vertexCount = static_cast<std::uint32_t>(nm.fallbackVertices.size());
    const std::uint32_t indexCount  = static_cast<std::uint32_t>(nm.fallbackIndices.size());
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = nm.fallbackVertices.size() * sizeof(nl::core::MeshVertex);   // 32B/顶点
    vbDesc.setIsAccelStructBuildInput(true);
    vbDesc.debugName = "BlasFallbackVB";
    blasVB_ = device->createBuffer(vbDesc);
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = nm.fallbackIndices.size() * sizeof(std::uint32_t);
    ibDesc.setIsAccelStructBuildInput(true);
    ibDesc.debugName = "BlasFallbackIB";
    blasIB_ = device->createBuffer(ibDesc);
    if (!blasVB_ || !blasIB_) {
        outError = "createBuffer failed for BLAS fallback geometry"; return false;
    }

    // 2. createAccelStruct：BLAS 尺寸由 NVRHI 内部按 geometry 描述符自算
    // （无用户侧 scratch 容量查询，S8-PLAN 预研 §1）；build 时必须传同一描述符。
    // flags = PreferFastTrace（静态场景，UE/bevy/Carrot 三处口径一致）；
    // AllowCompaction 不加——RTXMU 缺失时 API 是 no-op 且校验层会告警（预研 §1）。
    nvrhi::rt::GeometryDesc geometry;
    geometry.geometryData.triangles
        .setVertexBuffer(blasVB_)
        .setVertexFormat(nvrhi::Format::RGB32_FLOAT)       // position 在 32B 交错布局偏移 0
        .setVertexStride(sizeof(nl::core::MeshVertex))
        .setVertexCount(vertexCount)
        .setIndexBuffer(blasIB_)
        .setIndexFormat(nvrhi::Format::R32_UINT)
        .setIndexCount(indexCount);
    nvrhi::rt::AccelStructDesc asDesc;
    asDesc.addBottomLevelGeometry(geometry);
    asDesc.setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
    asDesc.setDebugName("BlasFallback");
    blas_ = device->createAccelStruct(asDesc);
    if (!blas_) {
        // 设备不支持 RT（无 DXR / 无 VK_KHR_acceleration_structure）：跳过不拦路，
        // 既有渲染路径零扰动；真几何错误会在 DX12 校验层报 Error 由 drainMessages 统计。
        nl::core::log::warn("blas: createAccelStruct failed for ", assetName,
                            " — ray tracing unsupported? BLAS skipped");
        return true;
    }

    // 3. 上传 + build 同命令列表（M8.0 实测教训：跨列表上传 + setPermanentBufferState
    // 在 DX12 debug layer 下执行期 RaiseException(0x87d)——build-input 缓冲物理状态
    // 未真正转换，被判定 CORRUPTION。同列表内显式 setBufferState 转换后状态跟踪与
    // 物理 barrier 一致，问题消失）。NVRHI 校验层要求 AS build 记录在 Compute 能力级
    // 列表（Graphics 队列列表满足 requireType 判定）；scratch 由列表内建 manager
    // 分配；一次性提交异步执行，不 CPU 等待（S8-PLAN 底线）。
    nvrhi::CommandListHandle cmd = device->createCommandList();
    cmd->open();
    cmd->beginTrackingBufferState(blasVB_, nvrhi::ResourceStates::CopyDest);
    cmd->writeBuffer(blasVB_, nm.fallbackVertices.data(),
                     nm.fallbackVertices.size() * sizeof(nl::core::MeshVertex));
    cmd->setBufferState(blasVB_, nvrhi::ResourceStates::AccelStructBuildInput);
    cmd->beginTrackingBufferState(blasIB_, nvrhi::ResourceStates::CopyDest);
    cmd->writeBuffer(blasIB_, nm.fallbackIndices.data(),
                     nm.fallbackIndices.size() * sizeof(std::uint32_t));
    cmd->setBufferState(blasIB_, nvrhi::ResourceStates::AccelStructBuildInput);
    if (blasTimer_.ensure(device)) blasTimer_.begin(cmd);
    cmd->buildBottomLevelAccelStruct(blas_, &geometry, 1,
                                     nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
    blasTimer_.end(cmd);
    // 列末态 = 永久态（静态场景，缓冲此后恒为 build-input；M8.1 TLAS 直接引用）。
    cmd->setPermanentBufferState(blasVB_, nvrhi::ResourceStates::AccelStructBuildInput);
    cmd->setPermanentBufferState(blasIB_, nvrhi::ResourceStates::AccelStructBuildInput);
    cmd->close();
    device->executeCommandList(cmd);
    // ⚠️ 0x87d 族第二坑（M8.1 实测）：NVRHI DX12 的 DXR scratch manager 是命令列表
    // 对象的成员，AS build 的 scratch chunk（DEFAULT 堆 GPU 内存）随列表句柄析构
    // 而 final-release。若此刻 build ECL 还在飞行，debug layer 记 CORRUPTION #921
    // （OBJECT_DELETED_WHILE_STILL_IN_USE，OutputDebugString 才可见、InfoQueue 不
    // 落地）；之后帧内新 build 分配 scratch 时分配器复用该页 → GPU 写入与飞行中
    // ECL 重叠 → 帧 3（首个复用 chunk 的 ECL）执行期 fail-fast、exit 125。M8.0 就
    // 埋着这颗雷，只因当时帧路径无 AS build 未引爆。init 一次性路径 CPU 等待可
    // 忽略，不违反 S8「不逐帧等待」底线。
    device->waitForIdle();

    blasReady_ = true;
    blasTriangleCount_ = indexCount / 3;
    nl::core::log::info("blas built: ", assetName, " | vertices=", vertexCount,
                        " triangles=", blasTriangleCount_, " (build ms via GPU timer)");

    // M8.1：BLAS 就位后建 TLAS（每帧全量 rebuild 的实例引用目标）。失败不拦路，
    // 与 BLAS skip 同口径（设备不支持 RT 的机器渲染照常）。
    std::string tlasErr;
    if (!initTlas(device, tlasErr)) { outError = tlasErr; return false; }
    return true;
}

void Scene::pollBlasTimer(nvrhi::IDevice* device, HudFrame* hud) {
    if (!blasReady_ || !hud || !device) return;
    // 与 pollCullTimer 同纪律：必须在 hud_.build() 之前调用。一次性构建：resolve
    // 一次后 blasBuildMs_ 恒定。deterministic 只挡 HUD 写回（截图基线稳定），
    // resolve 本身照跑——GpuTimer 的 resolvedOnce_ 日志在截图 run 的 log 里也落
    // 一条真实构建毫秒数，作 M8.0 的机器可读证据。
    if (blasTimer_.available()) {
        float ms = 0.0f;
        if (blasTimer_.poll(ms)) blasBuildMs_ = ms;
        if (!hud->deterministic) hud->blasMs = blasBuildMs_;
    }
    hud->blasTriangleCount = blasTriangleCount_;
    hud->blasTimingAvailable = blasTimer_.available();
}

// ---- M8.1（S8.1）：TLAS 每帧全量 rebuild（S8-PLAN M8.1；D-S8-1 拍板 = UE 口径）----
// 三处口径：UE FRayTracingScene::Build 每帧无条件 rebuild（RayTracingScene.cpp，
// 无 dirty/update 分支）；Carrot update+冷却（留作升级项）；bevy 双缓冲 TLAS。
// 实例填充拍板 = CPU 组装 rt::InstanceDesc 数组 + buildTopLevelAccelStruct（CPU
// 变体，NVRHI 内部管 staging 上传；S8-PLAN 预研 §1 签名表）。
bool Scene::initTlas(nvrhi::IDevice* device, std::string& outError) {
    // 现阶段 TLAS 所有失败路径都走 skip 不拦路（outError 留给未来硬错误：如
    // TLAS 容量超限类几何错误），参数保留签名对称性。
    (void)outError;
    if (tlasReady_) return true;
    if (!blasReady_ || !blas_) {
        // BLAS 未建（无 fallback 资产 / 无 RT 设备）：TLAS 无引用目标，跳过不拦路。
        nl::core::log::info("tlas: no BLAS available — skipped (S8 TLAS needs a BLAS)");
        return true;
    }
    tlasTimer_.setLabel("tlas");
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.setTopLevelMaxInstances(kMaxTlasInstances);   // isTopLevel = true 随置位
    // 每帧全量 rebuild（UE 口径）→ 不带 AllowUpdate/PerformUpdate；PreferFastTrace
    // 与 BLAS 同口径（实例数个位数，rebuild 开销可忽略——M8.1 实测见 HUD/日志）。
    tlasDesc.setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
    tlasDesc.setDebugName("TlasPerFrame");
    tlas_ = device->createAccelStruct(tlasDesc);
    if (!tlas_) {
        nl::core::log::warn("tlas: createAccelStruct failed — ray tracing unsupported? TLAS skipped");
        return true;
    }
    tlasReady_ = true;
    nl::core::log::info("tlas created: maxInstances=", kMaxTlasInstances,
                        " | per-frame full rebuild (UE D-S8-1)");
    return true;
}

void Scene::buildTlasFrame(nvrhi::ICommandList* cmd, const FrameParams& frame) {
    if (!tlasReady_ || !blasReady_) return;

    // CPU 组装实例（当前单 mesh 场景 = 1 实例）。--tlas-spin 时绕 Y 每帧转
    // 2.5°：只改 TLAS 实例 transform，光栅化路径照常（画面基线零扰动）；固定
    // 帧序号驱动 → 截图模式天然确定（同一帧序号同一角度）。
    nvrhi::rt::InstanceDesc inst;
    if (frame.tlasSpin) {
        const float angleRad = static_cast<float>(tlasFrameIndex_) * 2.5f * 3.14159265f / 180.0f;
        const float c = std::cos(angleRad), s = std::sin(angleRad);
        const float tf[12] = { c, 0.f, s, 0.f,
                               0.f, 1.f, 0.f, 0.f,
                              -s, 0.f, c, 0.f };
        inst.setTransform(tf);
    } // else：保持 InstanceDesc ctor 的 identity transform（静态场景语义）
    inst.setInstanceID(0);
    inst.setInstanceMask(0xFF);
    inst.setInstanceContributionToHitGroupIndex(0);
    inst.setBLAS(blas_);   // AccelStructHandle 隐式转 IAccelStruct*

    // 每帧 rebuild + 计时（帧命令列表 = §2.8 帧内禁第二 cmd list；BLAS build-input
    // 在 init 列表已置永久态 AccelStructBuildInput，TLAS build 只读其 device
    // address，无需再次状态转换）。timer 4 槽环形 > frame-in-flight=2，正常不缺槽。
    if (tlasTimer_.ensure(cmd->getDevice())) tlasTimer_.begin(cmd);
    cmd->buildTopLevelAccelStruct(tlas_, &inst, 1,
                                  nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
    tlasTimer_.end(cmd);
    tlasInstanceCount_ = 1;
    if (tlasFrameIndex_ == 0)
        nl::core::log::info("tlas: per-frame rebuild active | instances=1");
    tlasFrameIndex_++;

    // ---- 探针（--tlas-probe）：TLAS build 之后 dispatch，M8.1 自检的机器可读证据。
    // 懒建（首次 probe 帧才建管线/缓冲，§2.7 每管线单一 BindingSet）。
    if (frame.tlasProbe) {
        if (!probeReady_ && !probeFailed_) ensureTlasProbe(cmd);
        if (probeReady_) {
            // probeOut_：UAV 写 → CopySource 拷回 → 下帧 UAV（NVRHI 状态跟踪插转换；
            // 首帧 beginTracking 钉起点，之后靠跟踪动态晋升，visibleList_ 同口径）。
            cmd->setBufferState(probeOut_, nvrhi::ResourceStates::UnorderedAccess);
            nvrhi::ComputeState cstate;
            cstate.setPipeline(probePSO_).addBindingSet(probeSet_);
            cmd->setComputeState(cstate);
            cmd->dispatch(1, 1, 1);
            cmd->setBufferState(probeOut_, nvrhi::ResourceStates::CopySource);
            const std::uint32_t slot = probeSubmitted_ & 1u;
            cmd->copyBuffer(probeReadback_[slot], 0, probeOut_, 0, sizeof(std::uint32_t));
            probeSubmitted_++;
        }
    }
}

// 探针管线懒建：tlas_probe.cs.{spv,dxil}（CMake _ENTRIES_TLAS_PROBE）+ 单一
// BindingSet（t0 = TLAS + u0 = 4B hit t）。失败置 probeFailed_ 只报一次（不拦路）。
bool Scene::ensureTlasProbe(nvrhi::ICommandList* cmd) {
    nvrhi::IDevice* device = cmd->getDevice();
    if (probeReady_ || probeFailed_) return probeReady_;
    std::vector<std::uint8_t> csBlob;
    const std::string path = std::string(NL_SHADER_DIR) + "/tlas_probe.cs." + shaderSuffix_;
    if (!readFileBytes(path.c_str(), csBlob)) {
        nl::core::log::warn("tlas probe: failed to load shader from ", path, " — probe disabled");
        probeFailed_ = true;
        return false;
    }
    nvrhi::ShaderDesc csDesc;
    csDesc.setShaderType(nvrhi::ShaderType::Compute).setEntryName("main");   // §2.4 两阶段规则
    csTlasProbe_ = device->createShader(csDesc, csBlob.data(), csBlob.size());

    nvrhi::BufferDesc outDesc;
    outDesc.byteSize = sizeof(std::uint32_t);
    outDesc.structStride = sizeof(std::uint32_t);
    outDesc.canHaveUAVs = true;
    outDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    outDesc.keepInitialState = true;   // 跨列表跟踪不持久（per-list），每帧自动回 UAV
    outDesc.debugName = "TlasProbeOut";
    probeOut_ = device->createBuffer(outDesc);
    for (auto& rb : probeReadback_) {
        nvrhi::BufferDesc rDesc;
        rDesc.byteSize = sizeof(std::uint32_t);
        rDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        rDesc.debugName = "TlasProbeReadback";
        rb = device->createBuffer(rDesc);
    }
    if (!csTlasProbe_ || !probeOut_ || !probeReadback_[0] || !probeReadback_[1]) {
        nl::core::log::warn("tlas probe: resource creation failed — probe disabled");
        probeFailed_ = true;
        return false;
    }
    // probeOut_ 用 initialState+keepInitialState 口径（实测：NVRHI 状态跟踪表是
    // 每命令列表独立的，跨列表不持久——只设 initialState 不跟踪、beginTracking
    // 也只管当前列表；每帧复用的缓冲必须 keepInitialState=true，每帧列表自动回
    // 初态，indirectArgs_ 同款纪律）。帧内 UAV → CopySource 由 setBufferState
    // 显式转换（0x87d 同列表纪律），列末自动转回 UAV。readback 缓冲 cpuAccess
    // Read 不做状态检查（requireBufferState 直接 return）。
    nvrhi::BindingLayoutDesc pLayout;
    pLayout.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    probeLayout_ = device->createBindingLayout(pLayout);
    nvrhi::BindingSetDesc pSet;
    pSet.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, tlas_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, probeOut_));
    probeSet_ = device->createBindingSet(pSet, probeLayout_);
    nvrhi::ComputePipelineDesc cpDesc;
    cpDesc.setComputeShader(csTlasProbe_).addBindingLayout(probeLayout_);
    probePSO_ = device->createComputePipeline(cpDesc);
    if (!probeLayout_ || !probeSet_ || !probePSO_) {
        nl::core::log::warn("tlas probe: pipeline/binding creation failed — probe disabled");
        probeFailed_ = true;
        return false;
    }
    probeReady_ = true;
    nl::core::log::info("tlas probe: pipeline ready (RayQuery fixed ray, hit t readback)");
    return true;
}

void Scene::pollTlasTimer(nvrhi::IDevice* device, HudFrame* hud) {
    if (!tlasReady_ || !hud) return;
    // 每帧 rebuild 计时：poll 取最近完成帧的 resolve（1~2 帧延迟，GpuTimer 环形）。
    // deterministic 只挡 HUD 写回；首帧 resolve 落一条 resolvedOnce_ 日志作证据。
    if (tlasTimer_.ensure(device)) {
        float ms = 0.0f;
        if (tlasTimer_.poll(ms)) tlasBuildMs_ = ms;
        if (!hud->deterministic && tlasBuildMs_ > 0.0f) hud->tlasMs = tlasBuildMs_;
        hud->tlasTimingAvailable = true;
    }
    hud->tlasInstanceCount = tlasInstanceCount_;
}

void Scene::pollTlasProbe(nvrhi::IDevice* device) {
    if (!probeReady_ || probeSubmitted_ < 2) return;      // 两帧延迟窗口未填满
    const std::uint32_t ready = probeSubmitted_ - 2;
    if (probePolledSlot_ >= 0 && static_cast<std::uint32_t>(probePolledSlot_) >= ready) return;
    const std::uint32_t slot = ready & 1u;
    void* p = device->mapBuffer(probeReadback_[slot], nvrhi::CpuAccessMode::Read);
    if (!p) return;
    float t = 0.0f;
    std::memcpy(&t, p, sizeof(float));
    device->unmapBuffer(probeReadback_[slot]);
    probePolledSlot_ = static_cast<int>(ready);
    if (t < 0.0f)
        nl::core::log::info("tlas probe: seq=", ready, " t=miss (no hit)");
    else
        nl::core::log::info("tlas probe: seq=", ready, " t=", t);
}

// ============================================================================
// M8.2（S8.2）：Ray Query 调试射线闭环（S8-PLAN M8.2 t1~t3）
// compute 每像素从相机发 1 条射线打 TLAS（bevy_solari trace_ray 口径：TMin
// 0.001 / mask 0xFF / FORCE_OPAQUE），命中距离灰度或 instanceId+triangleId
// 伪彩两档调试，写 RGBA32_FLOAT UAV 目标后全屏呈现。射线可视化与光栅化场景
// 深度吻合 = S8 验收（同一 viewProj、同一 TLAS 实例 transform）。
// ============================================================================

// ============================================================================
// M9.0 T5：present 初始化 / 呈现从 initRt 抽出，供 rt 与 shadow 两条链共用。
// shadow 走 initVisbuf(pulling)（initCull 链，无 TLAS），不调 initRt，但需要
// 同一张 rtTarget_ (RGBA32_FLOAT) 与 psRtPresent 把 compute 结果呈现进 framebuffer。
// 本组函数不碰 TLAS / BLAS / rtComputeSet_，只管 shader + 呈现 layout / PSO。
// 纯结构重构：M8.2 的资源格式、BindingSet 内容、帧内顺序与画面语义一字未改。
// ============================================================================
bool Scene::initRtPresent(nvrhi::IDevice* device, const char* shaderSuffix,
                          std::string& outError) {
    rtDevice_ = device;
    auto rtPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/rt." + artifact + "." + shaderSuffix;
    };
    auto loadPresent = [&](const char* artifact, nvrhi::ShaderType type,
                           nvrhi::ShaderHandle& out) -> bool {
        std::vector<std::uint8_t> blob;
        if (!readFileBytes(rtPath(artifact).c_str(), blob)) {
            outError = std::string("failed to load rt shader ") + artifact +
                       " (" + shaderSuffix + ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc desc;
        // 入口名 "main"：SPIR-V 恒 main；DX12 忽略 setEntryName（§2.4 两阶段规则）。
        desc.setShaderType(type).setEntryName("main");
        out = device->createShader(desc, blob.data(), blob.size());
        if (!out) outError = std::string("createShader failed for rt ") + artifact;
        return out != nullptr;
    };
    if (!loadPresent("vsrtpresent", nvrhi::ShaderType::Vertex, vsRtPresent_) ||
        !loadPresent("psrtpresent", nvrhi::ShaderType::Pixel, psRtPresent_) ||
        !loadPresent("psblittohdr", nvrhi::ShaderType::Pixel, psBlitToHdr_) ||
        !loadPresent("pstonemap", nvrhi::ShaderType::Pixel, psTonemap_))
        return false;
    shaderSuffix_ = shaderSuffix;   // T0：供 ensureRtTargets 懒建 blit/tonemap set

    // 呈现布局：t0 = rt 目标 SRV（psRtPresent 用 Texture2D.Load，无采样器）。
    nvrhi::BindingLayoutDesc pLayoutDesc;
    pLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    rtPresentLayout_ = device->createBindingLayout(pLayoutDesc);
    if (!rtPresentLayout_) { outError = "createBindingLayout failed for rt present"; return false; }

    // T0：blit / tonemap 布局（每 pass 单一 BindingSet，§2.7；shift 沿用 §2.5 set 0）。
    // t0 = 只读 SRV（blit 绑 rtTarget_、tonemap 绑 hdrSceneColor_）。
    nvrhi::BindingLayoutDesc t0LayoutDesc;
    t0LayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    blitLayout_ = device->createBindingLayout(t0LayoutDesc);
    tonemapLayout_ = device->createBindingLayout(t0LayoutDesc);
    if (!blitLayout_ || !tonemapLayout_) {
        outError = "createBindingLayout failed for T0 blit/tonemap";
        return false;
    }
    return true;
}

bool Scene::presentRtTarget(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                            const nvrhi::Viewport& viewport, std::string& outError) {
    if (!rtDevice_ || !framebuffer || !rtTarget_ || !rtPresentSet_) {
        outError = "presentRtTarget: missing device/framebuffer/target/set";
        return false;
    }
    // T0：rtTarget_(RGBA32_FLOAT) → hdrSceneColor_(RGBA16_FLOAT) 直拷（blit），
    // 再 hdrSceneColor_ → swapchain 直通 tonemap。state 跟踪见 §2.15/§2.19。
    if (!blitRtToHdr(cmd, viewport, outError)) return false;
    if (!presentTonemap(cmd, framebuffer, viewport, outError)) return false;
    return true;
}

// T0：rtTarget_(RGBA32_FLOAT) → hdrSceneColor_(RGBA16_FLOAT) 直拷。
// hdrSceneColor_ 本帧首次作为 RenderTarget，先声明初始态（§2.19：ping-pong 不设
// permanent，逐帧 beginTracking 如实登记），再切到 RT 写点。
bool Scene::blitRtToHdr(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                        std::string& outError) {
    if (!hdrSceneColor_ || !blitSet_) { outError = "blitRtToHdr: missing hdr/set"; return false; }
    const nvrhi::TextureSubresourceSet allSub(0, nvrhi::TextureSubresourceSet::AllMipLevels,
                                             0, nvrhi::TextureSubresourceSet::AllArraySlices);
    // beginTracking 声明的是上一帧末态（tonemap 收尾切回 SRV），照 shadowMap
    // 先例（scene.cpp ~4690）：先如实登记，再 setTextureState 触发 SRV→RT barrier。
    cmd->beginTrackingTextureState(hdrSceneColor_, allSub, nvrhi::ResourceStates::ShaderResource);
    cmd->setTextureState(hdrSceneColor_, allSub, nvrhi::ResourceStates::RenderTarget);
    if (!blitPSO_) {
        const nvrhi::FramebufferInfo info = hdrColorFb_->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pDesc;
        pDesc.setPrimType(nvrhi::PrimitiveType::TriangleStrip)
             .setVertexShader(vsRtPresent_)
             .setFragmentShader(psBlitToHdr_)
             .addBindingLayout(blitLayout_);
        // hdrColorFb_ 无 depth attachment——显式关深度/模板（NVRHI 默认
        // depthTestEnable=true，与「framebuffer 无深度格式」冲突，validation 拒建）。
        pDesc.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None);
        pDesc.renderState.depthStencilState
            .disableDepthTest()
            .disableDepthWrite()
            .disableStencil();
        blitPSO_ = t0Device_->createGraphicsPipeline(pDesc, info);
        if (!blitPSO_) { outError = "createGraphicsPipeline failed for T0 blit"; return false; }
    }
    nvrhi::GraphicsState pstate;
    pstate.setPipeline(blitPSO_).setFramebuffer(hdrColorFb_).addBindingSet(blitSet_);
    pstate.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(pstate);
    nvrhi::DrawArguments vargs; vargs.setVertexCount(4);   // triangle-strip 覆盖四边形
    cmd->draw(vargs);
    return true;
}

// T0：hdrSceneColor_ → swapchain 直通合成（最小 tonemap，无 gamma——gamma 已在
// producer 端烤进 scene color，见 rt.slang psTonemap 注释）。读前把 hdrSceneColor_
// 切回 ShaderResource（§2.15/§2.19 纹理状态登记）。
bool Scene::presentTonemap(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                           const nvrhi::Viewport& viewport, std::string& outError) {
    if (!hdrSceneColor_ || !tonemapSet_) { outError = "presentTonemap: missing hdr/set"; return false; }
    const nvrhi::TextureSubresourceSet allSub(0, nvrhi::TextureSubresourceSet::AllMipLevels,
                                             0, nvrhi::TextureSubresourceSet::AllArraySlices);
    cmd->setTextureState(hdrSceneColor_, allSub, nvrhi::ResourceStates::ShaderResource);
    if (!tonemapPSO_) {
        const nvrhi::FramebufferInfo info = framebuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pDesc;
        pDesc.setPrimType(nvrhi::PrimitiveType::TriangleStrip)
             .setVertexShader(vsRtPresent_)
             .setFragmentShader(psTonemap_)
             .addBindingLayout(tonemapLayout_);
        pDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::None);
        // 同 blit：全屏拷贝显式关深度/模板（不依赖 swapchain fb 恰好带 depth）。
        pDesc.renderState.depthStencilState
            .disableDepthTest()
            .disableDepthWrite()
            .disableStencil();
        // t0Device_（ensurePipelines 存，全模式有值）：raster 主路径也走 tonemap，
        // rtDevice_ 只在 rt 系 init 赋值——raster 下为 null，虚调用即段错误。
        tonemapPSO_ = t0Device_->createGraphicsPipeline(pDesc, info);
        if (!tonemapPSO_) { outError = "createGraphicsPipeline failed for T0 tonemap"; return false; }
    }
    nvrhi::GraphicsState pstate;
    pstate.setPipeline(tonemapPSO_).setFramebuffer(framebuffer).addBindingSet(tonemapSet_);
    pstate.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(pstate);
    nvrhi::DrawArguments vargs; vargs.setVertexCount(4);   // triangle-strip 覆盖四边形
    cmd->draw(vargs);
    return true;
}

// T0 T2：场景主 pass 渲到 hdrSceneColor_ 前的清屏段——颜色 = params.clearColor
//（与后端 swapchain 清屏同源，renderer.cpp 已核实）。深度不清：t0Depth_ 是 backend
// depth_ 的共享句柄（keepInitialState=DepthWrite 永驻态，后端每帧 recordFrame 先行
// 清 1.0），场景 pass 直接写入即可；scene 再 clear/tracking 会与后端的永驻态登记
// 冲突且重复清屏。hdr 颜色不设 permanent，逐帧 beginTracking（§2.15/§2.19）。
bool Scene::beginHdrSceneTarget(nvrhi::ICommandList* cmd, const float clearColor[4],
                                std::string& outError) {
    if (!hdrSceneColor_ || !t0Depth_ || !hdrSceneFb_) {
        outError = "beginHdrSceneTarget: hdr target not ready";
        return false;
    }
    const nvrhi::TextureSubresourceSet allSub(0, nvrhi::TextureSubresourceSet::AllMipLevels,
                                              0, nvrhi::TextureSubresourceSet::AllArraySlices);
    // 声明上一帧末态（hdr = tonemap 收尾的 SRV），clear/setTextureState 照 shadowMap
    // 先例顺序触发 SRV→RT barrier。
    cmd->beginTrackingTextureState(hdrSceneColor_, allSub, nvrhi::ResourceStates::ShaderResource);
    cmd->clearTextureFloat(hdrSceneColor_, allSub,
                           nvrhi::Color(clearColor[0], clearColor[1], clearColor[2], clearColor[3]));
    cmd->setTextureState(hdrSceneColor_, allSub, nvrhi::ResourceStates::RenderTarget);
    return true;
}

// ============================================================================
// M9.0（S9）：--mode shadow —— compute 直光对照路径（Q4 方案 A，无阴影射线）。
// 复用面（副作用处置见 S9-PLAN「复用决策」表）：
//   * initVisbuf(pulling=true)：M7.1/7.2 全套 vis + 材质资源（槽位同源）
//   * initRtPresent：M8.2 的 vs/ps 呈现骨架 + rtPresentLayout_
//   * rtTarget_ (RGBA32_FLOAT)：M8.2 已有成员，不新建任何 GBuffer 资源
// 独立面：direct.cs compute 管线 + 自己的单一 BindingSet（§2.7），与 rt 链的
// rtComputeSet_（依赖 TLAS）完全隔离。
// ============================================================================
bool Scene::initShadow(nvrhi::IDevice* device, const char* shaderSuffix,
                       const char* vsEntry, const char* psEntry,
                       const char* nmeshFile, std::string& outError) {
    if (!initVisbuf(device, shaderSuffix, vsEntry, psEntry, nmeshFile,
                    /*pulling=*/true, outError)) return false;
    shadow_ = true;

    // T5 前置重构已把 present 从 initRt 拆出：这里只建 shader + layout，
    // 不碰 BLAS / TLAS / rtComputeSet_。
    if (!initRtPresent(device, shaderSuffix, outError)) return false;

    const std::string csPath = std::string(NL_SHADER_DIR) + "/direct.cs." + shaderSuffix;
    std::vector<std::uint8_t> blob;
    if (!readFileBytes(csPath.c_str(), blob)) {
        outError = "failed to load direct.cs (" + std::string(shaderSuffix) +
                   ") from " NL_SHADER_DIR;
        return false;
    }
    nvrhi::ShaderDesc desc;
    desc.setShaderType(nvrhi::ShaderType::Compute).setEntryName("main");
    csDirect_ = device->createShader(desc, blob.data(), blob.size());
    if (!csDirect_) { outError = "createShader failed for direct.cs"; return false; }

    // b3 DirectConstants（32B）：g_clearColor（16B）+ g_shadowParams（16B：bias/tMax），
    // 每帧全量覆写（volatile CB 纪律）。
    nvrhi::BufferDesc dcDesc;
    dcDesc.byteSize = 32;
    dcDesc.isConstantBuffer = true;
    dcDesc.isVolatile = true;
    dcDesc.maxVersions = 16;
    dcDesc.debugName = "DirectConstants";
    directConstants_ = device->createBuffer(dcDesc);
    if (!directConstants_) { outError = "createBuffer failed for direct constants"; return false; }

    // §2.7 单一 BindingSet：共用 resolve 槽位表 + b3 清屏色 + u0 输出 + t13 TLAS。
    // t13 = RayTracingAccelStruct（M9.1 阴影射线，照 rtComputeLayout_ 写法；t0..t12
    // 已被 resolve 槽位表占用，TLAS 落 t13 不冲突）。
    nvrhi::BindingLayoutDesc cDesc;
    addResolveLayoutItems(cDesc);
    cDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(3))
         .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
         .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(13));
    directComputeLayout_ = device->createBindingLayout(cDesc);
    if (!directComputeLayout_) {
        outError = "createBindingLayout failed for direct compute"; return false;
    }
    {
        nvrhi::ComputePipelineDesc cpDesc;
        cpDesc.setComputeShader(csDirect_).addBindingLayout(directComputeLayout_);
        directPSO_ = device->createComputePipeline(cpDesc);
    }
    if (!directPSO_) { outError = "createComputePipeline failed for direct"; return false; }

    // M9.1：TLAS 在 init 链 initTlas 已建（log 顺序 blas→tlas→本行可证），故此处
    // tlasReady_ 已定；如实反映射线是否接通，避免 review 误读为 M9.0 无射线口径。
    nl::core::log::info("shadow scene ready (compute direct lighting",
                        tlasReady_ ? ", 1 shadow ray/pixel via TLAS)" :
                                        ", shadow rays disabled: no TLAS)",
                        " | shader=", shaderSuffix);
    return true;
}

bool Scene::initRt(nvrhi::IDevice* device, const char* shaderSuffix,
                   const char* vsEntry, const char* psEntry,
                   const char* nmeshFile, std::string& outError) {
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
    // 资产链：initDag（vsCull/psCull + traverse 管线照建；BLAS/TLAS 挂在
    // initDag → initBlas → initTlas 上，rt 模式免费拿光追地基）。
    if (!initDag(device, shaderSuffix, vsEntry, psEntry, nmeshFile, outError)) return false;
    rtDevice_ = device;
    rt_ = true;

    // M9.0 T5：present shader / layout 抽成共用初始化（shadow 链不调 initRt，
    // 也要能复用 rtTarget_ + psRtPresent 呈现骨架）。
    if (!initRtPresent(device, shaderSuffix, outError)) return false;

    // ---- compute shader（hiz 模式 loadShader lambda 同款）----
    auto rtPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/rt." + artifact + "." + shaderSuffix;
    };
    auto loadRtShader = [&](const char* artifact, nvrhi::ShaderType type,
                            nvrhi::ShaderHandle& out) -> bool {
        std::vector<std::uint8_t> blob;
        if (!readFileBytes(rtPath(artifact).c_str(), blob)) {
            outError = std::string("failed to load rt shader ") + artifact +
                       " (" + shaderSuffix + ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc desc;
        // 入口名 "main"：SPIR-V 恒 main；DX12 忽略 setEntryName（§2.4 两阶段规则）。
        desc.setShaderType(type).setEntryName("main");
        out = device->createShader(desc, blob.data(), blob.size());
        if (!out) outError = std::string("createShader failed for rt ") + artifact;
        return out != nullptr;
    };
    if (!loadRtShader("cs", nvrhi::ShaderType::Compute, csRt_)) return false;

    // RtConstants（rt.slang b0，80B）：invViewProj 64B + params 16B。每帧 1 次写入
    // × frame-in-flight 2 → maxVersions 16 富余（SceneConstants 同款纪律：每帧
    // 全量覆写，不做整块 memcpy 以外的槽位特判）。
    nvrhi::BufferDesc rcDesc;
    rcDesc.byteSize = 80;
    rcDesc.isConstantBuffer = true;
    rcDesc.isVolatile = true;
    rcDesc.maxVersions = 16;
    rcDesc.debugName = "RtConstants";
    rtConstants_ = device->createBuffer(rcDesc);
    if (!rtConstants_) { outError = "createBuffer failed for rt constants"; return false; }

    // compute 布局：t0 TLAS + u0 RGBA32_FLOAT UAV + b0（单一 BindingSet，§2.7；
    // UAV 纹理依赖视口 → set 在 ensureRtTargets 懒建，layout/PSO 此处建）。
    nvrhi::BindingLayoutDesc cLayoutDesc;
    cLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    rtComputeLayout_ = device->createBindingLayout(cLayoutDesc);
    if (!rtComputeLayout_) { outError = "createBindingLayout failed for rt compute"; return false; }
    {
        nvrhi::ComputePipelineDesc cpDesc;
        cpDesc.setComputeShader(csRt_).addBindingLayout(rtComputeLayout_);
        rtPSO_ = device->createComputePipeline(cpDesc);
    }
    if (!rtPSO_) { outError = "createComputePipeline failed for rt"; return false; }

    // 呈现布局 / PSO 已由 initRtPresent / presentRtTarget 共用初始化接管。

    nl::core::log::info("rt scene ready (RayQuery per-pixel debug rays)",
                        " | shader=", shaderSuffix);
    return true;
}

// T0：backend 把 scene 深度（depth_，D32 keepInitialState）注入 Scene——离屏 HDR
// framebuffer 的 depth attachment 直接复用它。场景深度留在 overlay 深度测试的
// 原位（pre-T0 语义），后端 resize 重建 depth_ 后必须重新调用（本函数置空
// hdrSceneFb_，下一帧 ensureRtTargets 按新深度整组重建）。
void Scene::setSharedDepth(nvrhi::ITexture* depth) {
    if (t0Depth_.Get() == depth) return;
    t0Depth_ = depth;
    hdrSceneFb_ = nullptr;   // 强制 ensureRtTargets 重建（闸条件含 hdrSceneFb_）
}

bool Scene::ensureRtTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                            std::string& outError) {
    const std::uint32_t w = static_cast<std::uint32_t>(viewportW + 0.5f);
    const std::uint32_t h = static_cast<std::uint32_t>(viewportH + 0.5f);
    if (w == 0 || h == 0) { outError = "rt: zero viewport"; return false; }
    // hdrSceneFb_ 在闸内：setSharedDepth 换深度句柄时置空 fb → 强制整组重建
    //（resize / 后端深度重建路径）。
    if (rtTarget_ && hdrSceneFb_ && rtW_ == w && rtH_ == h) return true;
    rtW_ = w;
    rtH_ = h;

    // 调试目标：RGBA32_FLOAT 全尺寸单 mip（DX12 typed UAV store / Vulkan mandatory
    // storage image 双后端安全）。initialState=UnorderedAccess + keepInitialState：
    // 每帧命令列表开头自动回到 UAV 态，呈现 SRV 转换由状态跟踪插 barrier
    //（hiz 金字塔 ShaderResource 初始态同款模式）。
    nvrhi::TextureDesc tDesc;
    tDesc.width = rtW_;
    tDesc.height = rtH_;
    tDesc.format = nvrhi::Format::RGBA32_FLOAT;
    tDesc.isUAV = true;
    tDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    tDesc.keepInitialState = true;
    tDesc.debugName = "RtDebugTarget";
    rtTarget_ = device->createTexture(tDesc);
    if (!rtTarget_) { outError = "createTexture failed for rt target"; return false; }

    // T0（共享 temporal / post-chain 基建，决策 A + UE 口径）：离屏 HDR + velocity +
    // history ping-pong。随视口尺寸懒建；ping-pong 与 hdrSceneColor_ 不设 permanent，
    // 逐帧 beginTrackingTextureState 切换 RenderTarget↔ShaderResource（§2.15/§2.19）。
    // velocity_ / history0_1_ 在 T0 仅建纹理（T3 写 velocity、T4 用 history；S10/S12 消费）。
    t0W_ = w; t0H_ = h;
    {
        nvrhi::TextureDesc d;
        d.width = w; d.height = h;
        d.format = nvrhi::Format::RGBA32_FLOAT;   // 离屏 HDR 场景色：FP32 往返零量化——任意浮点管线只有 FP32 中间格式才字节精确（16F 在 8-bit 半步边界翻转，实测 5346px/±1）
        d.isRenderTarget = true;
        // 创建态 = SRV（shadowMap 同款）：与帧首 beginTrackingTextureState 声明的
        // 「上一帧末态 SRV」对齐，否则首帧 DX12 debug layer 报 COMMON/SRV 布局错位。
        d.initialState = nvrhi::ResourceStates::ShaderResource;
        d.keepInitialState = true;
        // DX12 debug layer：clear 前资源必须带 clear value，否则 perf 警告计入
        // deviceErrors（shadowMap 同款纪律）；运行期 clear 色仍按帧传参。
        d.setClearValue(nvrhi::Color(0.f, 0.f, 0.f, 1.f));
        d.debugName = "HdrSceneColor";
        hdrSceneColor_ = device->createTexture(d);
        if (!hdrSceneColor_) { outError = "createTexture failed for hdrSceneColor"; return false; }
    }
    {
        nvrhi::FramebufferDesc fb;
        fb.addColorAttachment(hdrSceneColor_);   // 仅含 hdrSceneColor_（blit 写点，无 depth）
        hdrColorFb_ = device->createFramebuffer(fb);
        if (!hdrColorFb_) { outError = "createFramebuffer failed for hdrColorFb"; return false; }
    }
    {
        nvrhi::TextureDesc d;
        d.width = w; d.height = h;
        d.format = nvrhi::Format::RG16_FLOAT;     // 独立 velocity（bevy 口径，比 Carrot 打包省 4×）
        d.isRenderTarget = true;
        // 创建态 = SRV + keepInitialState（hdrSceneColor_ 同款）：与 probe 帧首
        // beginTracking 声明的「上一帧末态 SRV」对齐，否则首帧 DX12 ECL 校验
        // fail（barrier 起点 SRV ≠ 实际 COMMON）。clear value 同理必须匹配。
        d.initialState = nvrhi::ResourceStates::ShaderResource;
        d.keepInitialState = true;
        d.setClearValue(nvrhi::Color(0.f, 0.f, 0.f, 0.f));
        d.debugName = "Velocity";
        velocity_ = device->createTexture(d);
        if (!velocity_) { outError = "createTexture failed for velocity"; return false; }
    }
    {
        nvrhi::TextureDesc d;
        d.width = w; d.height = h;
        d.format = nvrhi::Format::RGBA32_FLOAT;   // history ping-pong（FP32 同款：S12 时间累积精度 + 字节保形）
        d.isRenderTarget = true;
        d.debugName = "History0";
        history0_ = device->createTexture(d);
        d.debugName = "History1";
        history1_ = device->createTexture(d);
        if (!history0_ || !history1_) { outError = "createTexture failed for history"; return false; }
    }
    // T0 T2：离屏场景深度（D32）+ 含 depth 的 hdrSceneFb_（场景主 pass 渲点）。
    // depth 不设 permanent/keepInitialState，逐帧 beginTracking（§2.15/§2.19）。
    // T0：深度复用 backend depth_（setSharedDepth 注入，keepInitialState=DepthWrite
    // 永驻态、后端每帧先清屏）——overlay（dbgLines/particles）在 tonemap 后仍对
    // backend depth_ 做深度测试，场景深度必须留在原处，否则遮挡全失效（gl5/
    // drive-m3 全量闸实测）。不再自建 t0Depth_。
    if (!t0Depth_) {
        outError = "t0: shared scene depth not set (backend must call setSharedDepth)";
        return false;
    }
    {
        nvrhi::FramebufferDesc fb;
        fb.addColorAttachment(hdrSceneColor_);
        fb.setDepthAttachment(t0Depth_);
        hdrSceneFb_ = device->createFramebuffer(fb);
        if (!hdrSceneFb_) { outError = "createFramebuffer failed for hdrSceneFb"; return false; }
    }

    // T0：raster 主路径不经 initRtPresent（那是 rt/shadow 的 init）——blit/tonemap
    // 的 shader 与 layout 在此懒建（rt/shadow 已建时 null 守卫跳过；SPIR-V 恒 main、
    // DX12 忽略 entryName，§2.4/§2.5）。raster 只用 tonemap（场景直渲 hdrSceneColor_），
    // blit 系一并备好供 rt/shadow 复用。
    if (!vsRtPresent_ || !psBlitToHdr_ || !psTonemap_) {
        nvrhi::ShaderDesc t0sdesc;
        auto loadT0Shader = [&](const char* artifact, nvrhi::ShaderType type,
                                nvrhi::ShaderHandle& out) -> bool {
            const std::string path = std::string(NL_SHADER_DIR) + "/rt." +
                                     artifact + "." + shaderSuffix_;
            std::vector<std::uint8_t> blob;
            if (!readFileBytes(path.c_str(), blob)) return false;
            t0sdesc.setShaderType(type).setEntryName("main");
            out = device->createShader(t0sdesc, blob.data(), blob.size());
            return out != nullptr;
        };
        if (!loadT0Shader("vsrtpresent", nvrhi::ShaderType::Vertex, vsRtPresent_) ||
            !loadT0Shader("psblittohdr", nvrhi::ShaderType::Pixel, psBlitToHdr_) ||
            !loadT0Shader("pstonemap", nvrhi::ShaderType::Pixel, psTonemap_)) {
            outError = "failed to load T0 blit/tonemap shaders";
            return false;
        }
    }
    if (!blitLayout_ || !tonemapLayout_) {
        nvrhi::BindingLayoutDesc t0LayoutDesc;
        t0LayoutDesc.setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        blitLayout_ = device->createBindingLayout(t0LayoutDesc);
        tonemapLayout_ = device->createBindingLayout(t0LayoutDesc);
        if (!blitLayout_ || !tonemapLayout_) {
            outError = "createBindingLayout failed for T0 blit/tonemap";
            return false;
        }
    }

    // M9.0 T5：compute set 依赖 tlas_（M8.1 每帧 rebuild 的同一对象），只有 rt
    // 链有 TLAS；shadow 走 initVisbuf 时不能在这里建。置空让 drawRt 在
    // tlasReady_ 之后按当前 rtTarget_ 懒建——帧内顺序与改动前一致。
    rtComputeSet_ = nullptr;

    // 呈现 set（SRV 纹理重建后同步重建）——rt/shadow 链专用；raster 模式无
    // rtPresentLayout_（initRtPresent 未跑），跳过（呈现走 tonemapSet_）。
    if (rtPresentLayout_) {
        nvrhi::BindingSetDesc pd;
        pd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, rtTarget_));
        rtPresentSet_ = device->createBindingSet(pd, rtPresentLayout_);
        if (!rtPresentSet_) { outError = "createBindingSet failed for rt present set"; return false; }
    }

    // T0：blit set（t0 = rtTarget_ SRV，写进 hdrSceneColor_）；tonemap set（t0 =
    // hdrSceneColor_ SRV，写 swapchain）。每 pass 单一 BindingSet（§2.7），随纹理重建。
    {
        nvrhi::BindingSetDesc bd;
        bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, rtTarget_));
        blitSet_ = device->createBindingSet(bd, blitLayout_);
    }
    if (!blitSet_) { outError = "createBindingSet failed for T0 blit"; return false; }
    {
        nvrhi::BindingSetDesc td;
        td.addItem(nvrhi::BindingSetItem::Texture_SRV(0, hdrSceneColor_));
        tonemapSet_ = device->createBindingSet(td, tonemapLayout_);
    }
    if (!tonemapSet_) { outError = "createBindingSet failed for T0 tonemap"; return false; }

    // M9.2：reference 呈现 set（t1 SRV + b1 RefConstants），随 rtTarget_ 同步重建。
    if (reference_) {
        nvrhi::BindingSetDesc rpd;
        rpd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, rtTarget_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, refConstants_));
        refPresentSet_ = device->createBindingSet(rpd, refPresentLayout_);
        if (!refPresentSet_) {
            outError = "createBindingSet failed for ref present set"; return false;
        }
    }
    return true;
}

void Scene::drawRt(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                   const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!rtDevice_ || !framebuffer) return;
    // TLAS 未就绪（设备不支持 RT / 资产无 fallback 段）：黑屏 + 一次性日志，
    // 与 BLAS skip 同口径（渲染照常，不拦路）。
    if (!tlasReady_) {
        if (!rtNoTlasLogged_) {
            nl::core::log::info("rt: TLAS not ready (no RT support or no fallback "
                                "geometry) -- debug rays disabled");
            rtNoTlasLogged_ = true;
        }
        return;
    }

    std::string err;
    const float vpW = viewport.maxX - viewport.minX;
    const float vpH = viewport.maxY - viewport.minY;
    if (!ensureRtTargets(rtDevice_, vpW, vpH, err)) {
        nl::core::log::deviceError("rt", err);
        return;
    }

    // M9.0 T5：compute set 从 ensureRtTargets 移到这里（TLAS 就绪后按当前
    // rtTarget_ 懒建；resize 时 ensureRtTargets 置空 → 自动重建）。
    if (!rtComputeSet_) {
        nvrhi::BindingSetDesc sd;
        sd.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, tlas_))
           .addItem(nvrhi::BindingSetItem::Texture_UAV(0, rtTarget_,
                                                       nvrhi::Format::RGBA32_FLOAT))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, rtConstants_));
        rtComputeSet_ = rtDevice_->createBindingSet(sd, rtComputeLayout_);
        if (!rtComputeSet_) {
            nl::core::log::deviceError("rt", "createBindingSet failed for rt compute set");
            return;
        }
    }

    // ---- RtConstants 每帧全量覆写（80B）：invViewProj = inverse(viewProj) ----
    // viewProj 列主序（camera.hpp M0.4 口径），求逆后原布局直喂 cbuffer float4x4
    //（与 g_mvp 同一矩阵路径，两后端同一份数值）。
    nl::math::Mat4 vp{}, invVP{};
    std::memcpy(vp.m, params.viewProj, sizeof(vp.m));
    invVP = nl::math::inverseMat4(vp);
    // params.x = TMax（自适应远平面内全命中可标定）；y = 调试档；z/w = 分辨率。
    float rc[20];
    std::memcpy(rc, invVP.m, sizeof(invVP.m));
    // x = 灰度参考距离（轨道相机距离 ×2：资产半径 ~1 绕原点，t 落 [dist-1,
    // dist+1] 映到可见中灰带；hiz viz 口径近亮远暗）。
    const float eyeLen = std::sqrt(params.eye[0] * params.eye[0] +
                                   params.eye[1] * params.eye[1] +
                                   params.eye[2] * params.eye[2]);
    rc[16] = std::max(2.0f * eyeLen, 0.5f);                     // x: gray range
    rc[17] = (params.rtDebugMode == 1) ? 1.0f : 0.0f;           // y: debug mode
    rc[18] = static_cast<float>(rtW_);                          // z: width
    rc[19] = static_cast<float>(rtH_);                          // w: height
    cmd->writeBuffer(rtConstants_, rc, sizeof(rc));

    // ---- compute dispatch：每像素 1 条 RayQuery（8x8 线程组，越界线程早退）----
    nvrhi::ComputeState cstate;
    cstate.setPipeline(rtPSO_).addBindingSet(rtComputeSet_);
    cmd->setComputeState(cstate);
    cmd->dispatch((rtW_ + 7) / 8, (rtH_ + 7) / 8, 1);

    // ---- 呈现：M9.0 T5 抽成共用函数（rt / shadow 两条链复用同一 rtTarget_）----
    if (!presentRtTarget(cmd, framebuffer, viewport, err)) {
        nl::core::log::deviceError("rt", err);
    }
}

// M9.0：compute 8x8 dispatch → RGBA32_FLOAT UAV → 共用的 psRtPresent 全屏呈现。
// 与 draw() 的 visbuf 三段式同列表顺序：clear vis → raster → 本函数（读 vis SRV、
// 写 rtTarget_ UAV）→ present（读 rtTarget_ SRV），barrier 由 NVRHI 状态跟踪插入。
void Scene::drawShadow(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                       const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!rtDevice_ || !framebuffer || !directPSO_ || shadowFailed_) return;

    // M9.1：阴影射线打 M8 的 TLAS（粗代理几何）。TLAS 未就绪（设备不支持 RT /
    // 资产无 fallback 段）→ 本模式无法追迹，log 一次后返回（与 rt 模式同口径降级；
    // 主帧光栅 + 直光仍由其他 mode 提供，shadow 模式在此环境不可用）。
    if (!tlasReady_) {
        if (!shadowNoTlasLogged_) {
            nl::core::log::info("shadow: TLAS not ready (no RT support or no fallback "
                                "geometry) -- shadow rays disabled");
            shadowNoTlasLogged_ = true;
        }
        return;
    }

    std::string err;
    const float vpW = viewport.maxX - viewport.minX;
    const float vpH = viewport.maxY - viewport.minY;
    if (!ensureRtTargets(rtDevice_, vpW, vpH, err)) {
        nl::core::log::deviceError("shadow", err);
        shadowFailed_ = true;
        return;
    }
    // visW_/visH_ 与 rtW_/rtH_ 是两次独立取整；不一致会让 b2 尺寸与 dispatch
    // 尺寸错位，直接显式报错而不是画一张错图。
    if (rtW_ != visW_ || rtH_ != visH_) {
        nl::core::log::deviceError("shadow", "rt target size != vis buffer size");
        shadowFailed_ = true;
        return;
    }

    // 尺寸版本闸：resize 时 rtTarget_ / visBuffer_ 都换对象，set 必须重建。
    if (!directComputeSet_ || directW_ != rtW_ || directH_ != rtH_) {
        nvrhi::BindingSetDesc sd;
        addResolveSetItems(sd);
        sd.addItem(nvrhi::BindingSetItem::ConstantBuffer(3, directConstants_))
          .addItem(nvrhi::BindingSetItem::Texture_UAV(0, rtTarget_,
                                                      nvrhi::Format::RGBA32_FLOAT))
          .addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(13, tlas_));
        directComputeSet_ = rtDevice_->createBindingSet(sd, directComputeLayout_);
        if (!directComputeSet_) {
            nl::core::log::deviceError("shadow", "createBindingSet failed for direct compute set");
            shadowFailed_ = true;
            return;
        }
        directW_ = rtW_;
        directH_ = rtH_;
    }

    // b1/b2 与 pulling 分支共用写入；b3 = [当帧清屏色(16B) | shadow 参数(16B)]。
    // shadow 参数：x = bias（世界尺度法线偏移，防 proxy 粗代理自遮挡）；y = tMax
    //（阴影射线有限最大距离，保精度；场景半径 ~1，50 足以覆盖全部遮挡物）。
    writeResolveConstants(cmd, params);
    const float cc[8] = {params.clearColor[0], params.clearColor[1],
                         params.clearColor[2], params.clearColor[3],
                         0.01f, 50.0f, 0.0f, 0.0f};
    cmd->writeBuffer(directConstants_, cc, sizeof(cc));

    nvrhi::ComputeState cstate;
    cstate.setPipeline(directPSO_).addBindingSet(directComputeSet_);
    cmd->setComputeState(cstate);
    cmd->dispatch((rtW_ + 7) / 8, (rtH_ + 7) / 8, 1);

    if (!presentRtTarget(cmd, framebuffer, viewport, err)) {
        nl::core::log::deviceError("shadow", err);
        shadowFailed_ = true;
    }
}

// M9.2（S9）：--mode reference —— 离线参考模式（渐进式多帧累积直光真值图）。
// 与 initShadow 完全对称的独立初始化（不复用 initShadow 内部，守 M9.1 逐字节基线）：
// 几何/材质走 initVisbuf(pulling)；呈现骨架走 initRtPresent（vs/ps + rtPresentLayout_）；
// 多建 csReference compute 管线 + refConstants_（b4 累积参数）+ refPresent（÷samplesDone
// 再 gamma）。不新建任何独立 GBuffer 资源（Q4 方案 A）。
bool Scene::initReference(nvrhi::IDevice* device, const char* shaderSuffix,
                          const char* vsEntry, const char* psEntry,
                          const char* nmeshFile, std::string& outError) {
    if (!initVisbuf(device, shaderSuffix, vsEntry, psEntry, nmeshFile,
                    /*pulling=*/true, outError)) return false;
    reference_ = true;

    if (!initRtPresent(device, shaderSuffix, outError)) return false;

    // csReference（direct.csref）：与 csDirect 同文件、同资源槽位族，仅入口不同。
    const std::string csPath = std::string(NL_SHADER_DIR) + "/direct.csref." + shaderSuffix;
    std::vector<std::uint8_t> blob;
    if (!readFileBytes(csPath.c_str(), blob)) {
        outError = "failed to load direct.csref (" + std::string(shaderSuffix) +
                   ") from " NL_SHADER_DIR;
        return false;
    }
    nvrhi::ShaderDesc desc;
    desc.setShaderType(nvrhi::ShaderType::Compute).setEntryName("main");
    csReference_ = device->createShader(desc, blob.data(), blob.size());
    if (!csReference_) { outError = "createShader failed for direct.csref"; return false; }

    // b3 DirectConstants（32B）：清屏色 + shadow 参数（与 drawShadow 同款，每帧覆写）。
    {
        nvrhi::BufferDesc dcDesc;
        dcDesc.byteSize = 32;
        dcDesc.isConstantBuffer = true;
        dcDesc.isVolatile = true;
        dcDesc.maxVersions = 16;
        dcDesc.debugName = "DirectConstants";
        directConstants_ = device->createBuffer(dcDesc);
        if (!directConstants_) { outError = "createBuffer failed for direct constants"; return false; }
    }

    // b4 RefConstants（16B）：samplesPerFrame / samplesStart / samplesDone / pad。
    {
        nvrhi::BufferDesc rcDesc;
        rcDesc.byteSize = 16;
        rcDesc.isConstantBuffer = true;
        rcDesc.isVolatile = true;
        rcDesc.maxVersions = 16;
        rcDesc.debugName = "RefConstants";
        refConstants_ = device->createBuffer(rcDesc);
        if (!refConstants_) { outError = "createBuffer failed for ref constants"; return false; }
    }

    // §2.7 单一 BindingSet：resolve 槽位表 + b3 清屏色 + b4 累积参数 + u0 输出 + t13 TLAS。
    nvrhi::BindingLayoutDesc cDesc;
    addResolveLayoutItems(cDesc);
    cDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(3))
         .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(4))
         .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
         .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(13));
    refComputeLayout_ = device->createBindingLayout(cDesc);
    if (!refComputeLayout_) {
        outError = "createBindingLayout failed for reference compute"; return false;
    }
    {
        nvrhi::ComputePipelineDesc cpDesc;
        cpDesc.setComputeShader(csReference_).addBindingLayout(refComputeLayout_);
        refPSO_ = device->createComputePipeline(cpDesc);
    }
    if (!refPSO_) { outError = "createComputePipeline failed for reference"; return false; }

    // psRefPresent（rt.psrefpresent）：÷samplesDone 再 gamma 的全屏呈现（复用 vsRtPresent_）。
    auto rtPath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/rt." + artifact + "." + shaderSuffix;
    };
    auto loadRefPresent = [&](const char* artifact, nvrhi::ShaderType type,
                              nvrhi::ShaderHandle& out) -> bool {
        std::vector<std::uint8_t> pb;
        if (!readFileBytes(rtPath(artifact).c_str(), pb)) {
            outError = std::string("failed to load rt shader ") + artifact +
                       " (" + shaderSuffix + ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc pdesc;
        pdesc.setShaderType(type).setEntryName("main");
        out = device->createShader(pdesc, pb.data(), pb.size());
        if (!out) outError = std::string("createShader failed for rt ") + artifact;
        return out != nullptr;
    };
    if (!loadRefPresent("psrefpresent", nvrhi::ShaderType::Pixel, psRefPresent_)) return false;

    // 呈现布局：t1 = rt 目标 SRV（避开 rt.slang 的 t0 TLAS），b1 = RefConstants。
    nvrhi::BindingLayoutDesc pLayoutDesc;
    pLayoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    refPresentLayout_ = device->createBindingLayout(pLayoutDesc);
    if (!refPresentLayout_) { outError = "createBindingLayout failed for ref present"; return false; }

    nl::core::log::info("reference scene ready (progressive multi-frame direct-light",
                        tlasReady_ ? ", shadow rays via TLAS)" :
                                        ", shadow rays disabled: no TLAS)",
                        " | shader=", shaderSuffix,
                        " | spp/frame=", samplesPerFrame_);
    return true;
}

// M9.2：compute 8x8 dispatch（每帧 samplesPerFrame 条抖动阴影射线）累积进线性 sum
// buffer → presentRefTarget ÷总数 再 gamma。与 drawShadow 同列表顺序（clear vis →
// raster → 本函数），barrier 由 NVRHI 状态跟踪插入。
void Scene::drawReference(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                          const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!rtDevice_ || !framebuffer || !refPSO_ || refFailed_) return;

    // TLAS 未就绪 → 无阴影射线，参考图退化为无硬阴影直光（与 shadow 同口径降级）。
    if (!tlasReady_) {
        if (!refNoTlasLogged_) {
            nl::core::log::info("reference: TLAS not ready (no RT support or no fallback "
                                "geometry) -- shadow rays disabled");
            refNoTlasLogged_ = true;
        }
        return;
    }

    std::string err;
    const float vpW = viewport.maxX - viewport.minX;
    const float vpH = viewport.maxY - viewport.minY;
    if (!ensureRtTargets(rtDevice_, vpW, vpH, err)) {
        nl::core::log::deviceError("reference", err);
        refFailed_ = true;
        return;
    }
    if (rtW_ != visW_ || rtH_ != visH_) {
        nl::core::log::deviceError("reference", "rt target size != vis buffer size");
        refFailed_ = true;
        return;
    }

    // 尺寸版本闸：resize 时 rtTarget_ 换新 → set 重建；samplesDone_ 归零（首帧覆盖残值）。
    if (!refComputeSet_ || refW_ != rtW_ || refH_ != rtH_) {
        nvrhi::BindingSetDesc sd;
        addResolveSetItems(sd);
        sd.addItem(nvrhi::BindingSetItem::ConstantBuffer(3, directConstants_))
          .addItem(nvrhi::BindingSetItem::ConstantBuffer(4, refConstants_))
          .addItem(nvrhi::BindingSetItem::Texture_UAV(0, rtTarget_,
                                                      nvrhi::Format::RGBA32_FLOAT))
          .addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(13, tlas_));
        refComputeSet_ = rtDevice_->createBindingSet(sd, refComputeLayout_);
        if (!refComputeSet_) {
            nl::core::log::deviceError("reference", "createBindingSet failed for ref compute set");
            refFailed_ = true;
            return;
        }
        refW_ = rtW_;
        refH_ = rtH_;
        samplesDone_ = 0;   // 新尺寸：重新累积
    }

    writeResolveConstants(cmd, params);
    const float cc[8] = {params.clearColor[0], params.clearColor[1],
                         params.clearColor[2], params.clearColor[3],
                         0.01f, 50.0f, 0.0f, 0.0f};
    cmd->writeBuffer(directConstants_, cc, sizeof(cc));

    // RefConstants：本帧起点 = 已累积总数（保证每帧抖动不同）；结束 = 起点 + spp。
    const std::uint32_t start = samplesDone_;
    const std::uint32_t done  = start + samplesPerFrame_;
    const std::uint32_t rc[4] = {samplesPerFrame_, start, done, 0u};
    cmd->writeBuffer(refConstants_, rc, sizeof(rc));

    nvrhi::ComputeState cstate;
    cstate.setPipeline(refPSO_).addBindingSet(refComputeSet_);
    cmd->setComputeState(cstate);
    cmd->dispatch((rtW_ + 7) / 8, (rtH_ + 7) / 8, 1);

    if (!presentRefTarget(cmd, framebuffer, viewport, err)) {
        nl::core::log::deviceError("reference", err);
        refFailed_ = true;
    }

    samplesDone_ = done;
    refFramesTarget_ = (params.refFrames > 0) ? static_cast<std::uint32_t>(params.refFrames) : 0;
    const float totalTarget = static_cast<float>(refFramesTarget_) * static_cast<float>(samplesPerFrame_);
    refProgress_ = (totalTarget > 0.0f) ? (static_cast<float>(samplesDone_) / totalTarget) : 0.0f;
    // 进度日志（截图模式 HUD 关，靠日志看累积进度；每 8 帧一次避免刷屏）。
    if (refFramesTarget_ > 0 && (samplesDone_ % (samplesPerFrame_ * 8) == 0 ||
                                 samplesDone_ >= totalTarget)) {
        nl::core::log::info("reference: ", samplesDone_, " / ", static_cast<std::uint32_t>(totalTarget),
                            " spp (", static_cast<int>(refProgress_ * 100.0f), "%)");
    }
}

bool Scene::presentRefTarget(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                             const nvrhi::Viewport& viewport, std::string& outError) {
    if (!rtDevice_ || !framebuffer || !rtTarget_ || !refPresentSet_) {
        outError = "presentRefTarget: missing device/framebuffer/target/set";
        return false;
    }
    if (!refPresentPSO_) {
        const nvrhi::FramebufferInfo info = framebuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pDesc;
        pDesc.setPrimType(nvrhi::PrimitiveType::TriangleStrip)
             .setVertexShader(vsRtPresent_)
             .setFragmentShader(psRefPresent_)
             .addBindingLayout(refPresentLayout_);
        pDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::None);
        refPresentPSO_ = rtDevice_->createGraphicsPipeline(pDesc, info);
        if (!refPresentPSO_) {
            outError = "createGraphicsPipeline failed for ref present";
            return false;
        }
    }
    nvrhi::GraphicsState pstate;
    pstate.setPipeline(refPresentPSO_)
          .setFramebuffer(framebuffer)
          .addBindingSet(refPresentSet_);
    pstate.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(pstate);
    nvrhi::DrawArguments vargs;
    vargs.setVertexCount(4);   // triangle-strip 覆盖四边形
    cmd->draw(vargs);
    return true;
}

// ============================================================================
// M10.0（S10）：--mode probe-place —— 屏幕探针 cascade 布点（Q10-1/Q10-2 拍板
// UE 口径）。复用面（S10-PLAN「复用决策」同 shadow/reference 口径）：
//   * initVisbuf(pulling=true)：M7.1/7.2 全套 vis + 材质资源（M10.1 直光查表机器
//     已备，本里程碑只消费其几何池做 velocity prepass）
//   * velocity_：T0 已建的 RG16F 纹理（T3 写入在本里程碑落地）
// 独立面：velocity prepass（自建 probeDepth_ 深度）+ csPlace cascade 布点 +
// probe_viz 点可视化；不碰 T2 场景 PSO / 共享 depth_ / Q4 冻结面（0 新增
// GBuffer 属性资源——atlas 是探针位置，不是表面属性）。
// ============================================================================
bool Scene::initProbe(nvrhi::IDevice* device, const char* shaderSuffix,
                      const char* vsEntry, const char* psEntry,
                      const char* nmeshFile, std::string& outError) {
    if (!initVisbuf(device, shaderSuffix, vsEntry, psEntry, nmeshFile,
                    /*pulling=*/true, outError)) return false;
    probe_ = true;
    probeDevice_ = device;
    shaderSuffix_ = shaderSuffix;   // M-P3：dbgLines_.ensure 定位字节码（同 initRt）

    auto probePath = [shaderSuffix](const char* artifact) {
        return std::string(NL_SHADER_DIR) + "/" + artifact + "." + shaderSuffix;
    };
    auto loadShader = [&](const char* artifact, nvrhi::ShaderType type,
                          nvrhi::ShaderHandle& out) -> bool {
        std::vector<std::uint8_t> blob;
        if (!readFileBytes(probePath(artifact).c_str(), blob)) {
            outError = std::string("failed to load shader ") + artifact +
                       " (" + shaderSuffix + ") from " NL_SHADER_DIR;
            return false;
        }
        nvrhi::ShaderDesc desc;
        // 入口名 "main"：SPIR-V 恒 main；DX12 忽略 setEntryName（§2.4 两阶段规则）。
        desc.setShaderType(type).setEntryName("main");
        out = device->createShader(desc, blob.data(), blob.size());
        if (!out) outError = std::string("createShader failed for ") + artifact;
        return out != nullptr;
    };
    if (!loadShader("velocity.vsvelocity", nvrhi::ShaderType::Vertex, vsVelocity_) ||
        !loadShader("velocity.psvelocity", nvrhi::ShaderType::Pixel, psVelocity_) ||
        !loadShader("probe.csplace", nvrhi::ShaderType::Compute, csPlace_) ||
        !loadShader("probe_trace.cstrace", nvrhi::ShaderType::Compute, csTrace_) ||
        !loadShader("probe_viz.vsprobedots", nvrhi::ShaderType::Vertex, vsProbeDots_) ||
        !loadShader("probe_viz.psprobedots", nvrhi::ShaderType::Pixel, psProbeDots_))
        return false;

    // probeCB_（192B volatile）：布点 + viz 两管线共用一份 CB（全字段每帧覆写，
    // 批内差异字段必须进写入面——§2.15 纪律）。M10.1 扩 176→192B（g_vizExtra）。
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = 192;
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "ProbeConstants";
    probeCB_ = device->createBuffer(cbDesc);
    if (!probeCB_) { outError = "createBuffer failed for probe constants"; return false; }

    // velocityCB_（144B volatile）：velocity.slang 专用布局
    //（[0..15] cur viewProj / [16..31] prev viewProj / [32..35] 尺寸）。
    nvrhi::BufferDesc vcbDesc;
    vcbDesc.byteSize = 144;
    vcbDesc.isConstantBuffer = true;
    vcbDesc.isVolatile = true;
    vcbDesc.maxVersions = 16;
    vcbDesc.debugName = "VelocityConstants";
    velocityCB_ = device->createBuffer(vcbDesc);
    if (!velocityCB_) { outError = "createBuffer failed for velocity constants"; return false; }

    // traceCB_（64B volatile，M10.1）：probe_trace.slang 布局
    //（[0..3] 灯向 / [4..7] 灯色·强度 / [8..11] 命中 albedo·ambient / [12..15] 参数）。
    nvrhi::BufferDesc tcbDesc;
    tcbDesc.byteSize = 64;
    tcbDesc.isConstantBuffer = true;
    tcbDesc.isVolatile = true;
    tcbDesc.maxVersions = 16;
    tcbDesc.debugName = "TraceConstants";
    traceCB_ = device->createBuffer(tcbDesc);
    if (!traceCB_) { outError = "createBuffer failed for trace constants"; return false; }

    // ---- velocity prepass 布局：b0 + t1..t4（pulling 几何池，visbuf.slang 同槽）。
    nvrhi::BindingLayoutDesc vDesc;
    vDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    velocityLayout_ = device->createBindingLayout(vDesc);
    if (!velocityLayout_) { outError = "createBindingLayout failed for velocity prepass"; return false; }

    // ---- csPlace 布局：b0 + t0 depth SRV + t1 velocity SRV + u0..u4 UAV。
    nvrhi::BindingLayoutDesc pDesc;
    pDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(3))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(4));
    placeLayout_ = device->createBindingLayout(pDesc);
    if (!placeLayout_) { outError = "createBindingLayout failed for probe place"; return false; }

    // ---- dots 布局：b0 + t0..t4 SRV + t5..t8 irradiance SRV（M10.1）。
    nvrhi::BindingLayoutDesc dDesc;
    dDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(4))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(5))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(6))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(7))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(8))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(9));
    dotsLayout_ = device->createBindingLayout(dDesc);
    if (!dotsLayout_) { outError = "createBindingLayout failed for probe dots"; return false; }

    // ---- csTrace 布局（M10.1）：b0 + t0..t3 place atlas SRV + t4 TLAS + u0..u3 UAV。
    nvrhi::BindingLayoutDesc tDesc;
    tDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(4))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(3));
    traceLayout_ = device->createBindingLayout(tDesc);
    if (!traceLayout_) { outError = "createBindingLayout failed for probe trace"; return false; }

    {
        nvrhi::ComputePipelineDesc cpDesc;
        cpDesc.setComputeShader(csPlace_).addBindingLayout(placeLayout_);
        placePSO_ = device->createComputePipeline(cpDesc);
    }
    if (!placePSO_) { outError = "createComputePipeline failed for probe place"; return false; }

    {
        nvrhi::ComputePipelineDesc cpDesc;
        cpDesc.setComputeShader(csTrace_).addBindingLayout(traceLayout_);
        tracePSO_ = device->createComputePipeline(cpDesc);
    }
    if (!tracePSO_) { outError = "createComputePipeline failed for probe trace"; return false; }

    nl::core::log::info("probe scene ready (velocity prepass + 4-level cascade placement",
                        ", viz dots) | shader=", shaderSuffix);
    return true;
}

// M10.0：probe 资源懒建。velocityFb_（velocity_ + probeDepth_）与 atlas 随视口
// 尺寸重建；baseRes（ImGui 参数）入闸 → atlas/prevDelta/sets 整组重建
//（PSO 与 CB 不依赖尺寸，复用）。
bool Scene::ensureProbeTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                               std::uint32_t baseRes, std::string& outError) {
    const std::uint32_t w = static_cast<std::uint32_t>(viewportW + 0.5f);
    const std::uint32_t h = static_cast<std::uint32_t>(viewportH + 0.5f);
    const std::uint32_t res = baseRes < 4u ? 4u : (baseRes > 32u ? 32u : baseRes);
    if (w == 0 || h == 0) { outError = "probe: zero viewport"; return false; }
    if (probeDepth_ && velocityFb_ && probeW_ == w && probeH_ == h &&
        probeBaseRes_ == res)
        return true;
    probeW_ = w;
    probeH_ = h;
    probeBaseRes_ = res;

    // probeDepth_（D32）：velocity prepass 的深度附件（写）+ csPlace 的 SRV（读）。
    // 不设 permanent——逐帧 DepthWrite↔SRV tracking（§2.15/§2.19；共享 backend
    // depth_ 是 keepInitialState 永驻态，不能切 SRV）。创建态 = SRV +
    // keepInitialState：与帧首 beginTracking 声明的「上一帧末态 SRV」对齐
    //（hdrSceneColor_ 同款）——首帧创建态若为 DepthWrite，DX12 ECL 校验直接
    // fail（barrier 起点 SRV ≠ 实际 DepthWrite，2026-11-11 冒烟实锤）。
    {
        nvrhi::TextureDesc dDesc;
        dDesc.width = w;
        dDesc.height = h;
        dDesc.format = nvrhi::Format::D32;
        dDesc.isRenderTarget = true;
        dDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        dDesc.keepInitialState = true;
        dDesc.setClearValue(nvrhi::Color(1.0f));
        dDesc.debugName = "ProbeDepth";
        probeDepth_ = device->createTexture(dDesc);
        if (!probeDepth_) { outError = "createTexture failed for probe depth"; return false; }
    }
    {
        nvrhi::FramebufferDesc fb;
        fb.addColorAttachment(velocity_);
        fb.setDepthAttachment(probeDepth_);
        velocityFb_ = device->createFramebuffer(fb);
        if (!velocityFb_) { outError = "createFramebuffer failed for velocityFb"; return false; }
    }

    // cascade atlas：4 级 RGBA32_FLOAT（rgb=世界位置，a=deviceZ）× 2 帧 ping-pong。
    // 创建态 = SRV（hdrSceneColor_ 同款：与帧首 beginTracking 声明的上一帧末态对齐）。
    for (std::uint32_t parity = 0; parity < 2; ++parity) {
        for (std::uint32_t level = 0; level < 4; ++level) {
            const std::uint32_t res_l = res >> level;
            nvrhi::TextureDesc aDesc;
            aDesc.width = res_l;
            aDesc.height = res_l;
            aDesc.format = nvrhi::Format::RGBA32_FLOAT;   // typed UAV store 双后端安全（rtTarget_ 先例）
            aDesc.isUAV = true;
            aDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            aDesc.debugName = "ProbeAtlas";
            probeAtlas_[parity][level] = device->createTexture(aDesc);
            if (!probeAtlas_[parity][level]) {
                outError = "createTexture failed for probe atlas";
                return false;
            }
        }
    }
    {
        nvrhi::TextureDesc pdDesc;
        pdDesc.width = res;
        pdDesc.height = res;
        pdDesc.format = nvrhi::Format::RGBA32_FLOAT;   // RG32F typed UAV store 是 DX12 可选特性，退 32F（rtTarget_ 先例）
        pdDesc.isUAV = true;
        pdDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        pdDesc.debugName = "ProbePrevDelta";
        probePrevDelta_ = device->createTexture(pdDesc);
        if (!probePrevDelta_) { outError = "createTexture failed for probe prevDelta"; return false; }
    }

    // irradiance atlas（M10.1）：每级一张 RGBA16F，每探针占 (oct+2)²=10×10 格
    //（8 core + 1 texel border，Q12-2）。M10.1 无时间混合 → 单份不 ping-pong。
    // 创建态 = SRV（与帧首 beginTracking 声明对齐，probeDepth_ 同款纪律）。
    for (std::uint32_t level = 0; level < 4; ++level) {
        const std::uint32_t res_l = (res >> level) * 10u;
        nvrhi::TextureDesc iDesc;
        iDesc.width = res_l;
        iDesc.height = res_l;
        iDesc.format = nvrhi::Format::RGBA16_FLOAT;    // HDR 颜色 16F 足够（位置才用 32F）
        iDesc.isUAV = true;
        iDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        iDesc.debugName = "ProbeIrradAtlas";
        irradAtlas_[level] = device->createTexture(iDesc);
        if (!irradAtlas_[level]) {
            outError = "createTexture failed for probe irradiance atlas";
            return false;
        }
    }

    // 绑定集：velocity 一份（无 parity，绑自己的 144B CB）；place/dots 各两份
    //（atlas 绑定随 parity）。
    {
        nvrhi::BindingSetDesc sDesc;
        sDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, velocityCB_))
             .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, vertexPool_))
             .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, localIndices_))
             .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, clusterMeta_))
             .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, visibleList_));
        velocitySet_ = device->createBindingSet(sDesc, velocityLayout_);
        if (!velocitySet_) { outError = "createBindingSet failed for velocity prepass"; return false; }
    }
    for (std::uint32_t parity = 0; parity < 2; ++parity) {
        nvrhi::BindingSetDesc psDesc;
        psDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, probeCB_))
              .addItem(nvrhi::BindingSetItem::Texture_SRV(0, probeDepth_))
              .addItem(nvrhi::BindingSetItem::Texture_SRV(1, velocity_));
        for (std::uint32_t level = 0; level < 4; ++level)
            psDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(level, probeAtlas_[parity][level]));
        psDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(4, probePrevDelta_));
        placeSet_[parity] = device->createBindingSet(psDesc, placeLayout_);
        if (!placeSet_[parity]) { outError = "createBindingSet failed for probe place"; return false; }

        nvrhi::BindingSetDesc dsDesc;
        dsDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, probeCB_));
        for (std::uint32_t level = 0; level < 4; ++level)
            dsDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(level, probeAtlas_[parity][level]));
        dsDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(4, probePrevDelta_));
        for (std::uint32_t level = 0; level < 4; ++level)
            dsDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(5 + level, irradAtlas_[level]));
        dsDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(9, probeDepth_));   // M10.1 法线重建
        dotsSet_[parity] = device->createBindingSet(dsDesc, dotsLayout_);
        if (!dotsSet_[parity]) { outError = "createBindingSet failed for probe dots"; return false; }

        // trace set（M10.1）：读 cur parity place atlas + 写 irradiance atlas。
        // tlasReady_ 门控懒建——无 RT / 无 fallback 几何时 tlas_ 为 null，set
        // 创建必败（shadow 模式 tlasReady_ 检查同口径降级；TLAS 就绪后的某帧
        // 补建，drawProbeFrame 的 tlasReady_ 分支保证 dispatch 只在就绪时跑）。
        if (tlasReady_) {
            nvrhi::BindingSetDesc tsDesc;
            tsDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, traceCB_));
            for (std::uint32_t level = 0; level < 4; ++level)
                tsDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(level, probeAtlas_[parity][level]));
            tsDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(4, tlas_));
            for (std::uint32_t level = 0; level < 4; ++level)
                tsDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(level, irradAtlas_[level]));
            traceSet_[parity] = device->createBindingSet(tsDesc, traceLayout_);
            if (!traceSet_[parity]) { outError = "createBindingSet failed for probe trace"; return false; }
        }
    }

    // velocity PSO（RG16F+D32 framebuffer 格式锁定，照 hiz prepassInfo 先例按
    // 格式构造）：深度测试开 + 写入开（本 pass 即探针模式的深度来源），cull=Back。
    if (!velocityPSO_) {
        const nvrhi::FramebufferInfo info = velocityFb_->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc gpDesc;
        gpDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
              .setVertexShader(vsVelocity_)
              .setFragmentShader(psVelocity_)
              .addBindingLayout(velocityLayout_);
        gpDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setCullMode(nvrhi::RasterCullMode::Back);
        gpDesc.renderState.depthStencilState
            .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual)
            .enableDepthTest()
            .enableDepthWrite()
            .disableStencil();
        velocityPSO_ = device->createGraphicsPipeline(gpDesc, info);
        if (!velocityPSO_) { outError = "createGraphicsPipeline failed for velocity prepass"; return false; }
    }

    // 一次性日志锚（机器断言用）：级联结构 + 总探针数 + 追踪预算。
    if (!probeLogged_) {
        const std::uint32_t total = res * res + (res >> 1) * (res >> 1) +
                                    (res >> 2) * (res >> 2) + (res >> 3) * (res >> 3);
        nl::core::log::info("probe: cascades=4 base=", res,
                            " grid=", res, "/", (res >> 1), "/", (res >> 2), "/", (res >> 3),
                            " probes=", total,
                            " oct=", 8, " raysPerProbe=", 8 * 8);
        probeLogged_ = true;
    }
    return true;
}

// M10.0：velocity prepass + cascade 布点（pulling 分支内、tonemap 前调用）。
// 状态纪律（§2.15/§2.19）：probeDepth_ / velocity_ / prevDelta / atlas 全部非
// permanent，帧首 beginTracking 声明上一帧末态，后续转换由 setTextureState /
// 绑定 require 自动插 barrier。
void Scene::drawProbeFrame(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                           const FrameParams& params) {
    if (!probe_ || probeFailed_ || !probeDevice_ || !velocity_) return;
    std::string err;
    const float vpW = viewport.maxX - viewport.minX;
    const float vpH = viewport.maxY - viewport.minY;
    if (!ensureProbeTargets(probeDevice_, vpW, vpH,
                            static_cast<std::uint32_t>(params.probeBaseRes), err)) {
        nl::core::log::deviceError("probe", err);
        probeFailed_ = true;   // 只报一次，后续帧跳过（与 visbuf/hiz 懒建失败同口径）
        return;
    }

    const nvrhi::TextureSubresourceSet allSub(0, nvrhi::TextureSubresourceSet::AllMipLevels,
                                              0, nvrhi::TextureSubresourceSet::AllArraySlices);
    // ---- 帧首状态声明（上一帧末态：depth=SRV（csPlace 读）/ velocity=SRV /
    // prevDelta=SRV（dots 读）/ atlas[cur]=SRV（dots 读）/ irrad=SRV（dots 读））。
    cmd->beginTrackingTextureState(probeDepth_, allSub, nvrhi::ResourceStates::ShaderResource);
    cmd->beginTrackingTextureState(velocity_, allSub, nvrhi::ResourceStates::ShaderResource);
    cmd->beginTrackingTextureState(probePrevDelta_, allSub, nvrhi::ResourceStates::ShaderResource);
    for (std::uint32_t level = 0; level < 4; ++level)
        cmd->beginTrackingTextureState(probeAtlas_[probeParity_][level], allSub,
                                       nvrhi::ResourceStates::ShaderResource);
    for (std::uint32_t level = 0; level < 4; ++level)
        cmd->beginTrackingTextureState(irradAtlas_[level], allSub,
                                       nvrhi::ResourceStates::ShaderResource);
    // ---- 清屏（都在首个 setGraphicsState 之前——Vulkan render pass 纪律）。
    cmd->setTextureState(probeDepth_, allSub, nvrhi::ResourceStates::DepthWrite);
    cmd->clearDepthStencilTexture(probeDepth_, nvrhi::TextureSubresourceSet(),
                                  true, 1.0f, false, 0);
    cmd->clearTextureFloat(velocity_, allSub, nvrhi::Color(0.f, 0.f, 0.f, 0.f));
    cmd->setTextureState(velocity_, allSub, nvrhi::ResourceStates::RenderTarget);

    // ---- 首帧：prevViewProj = 本帧（velocity=0，截图首帧即稳态，无大速度污染）。
    if (probeFirstFrame_) {
        probeFirstFrame_ = false;
        std::memcpy(probePrevViewProj_, params.viewProj, sizeof(probePrevViewProj_));
    }

    // ---- velocityCB_ 每帧全量覆写（144B）：[0..15] cur viewProj /
    // [16..31] prev viewProj / [32..35] (width, height, pad, pad)。
    float vcb[36] = {};
    std::memcpy(vcb, params.viewProj, sizeof(params.viewProj));
    std::memcpy(vcb + 16, probePrevViewProj_, sizeof(probePrevViewProj_));
    vcb[32] = vpW;
    vcb[33] = vpH;
    cmd->writeBuffer(velocityCB_, vcb, sizeof(vcb));

    // ---- probeCB_ 每帧全量覆写（192B）：布局见 probe.slang。
    //   [0..15] invViewProj / [16..19] eye(+projYScale) / [20..23] placeParams /
    //   [24..39] vizViewProj / [40..43] vizParams / [44..47] vizExtra。
    float cb[48] = {};
    {
        nl::math::Mat4 vp{}, invVP{};
        std::memcpy(vp.m, params.viewProj, sizeof(vp.m));
        invVP = nl::math::inverseMat4(vp);
        std::memcpy(cb, invVP.m, sizeof(invVP.m));
        // eye：rt 模式同源（FrameParams.eye 由 viewer 在 probe 模式填入）。
        cb[16] = params.eye[0];
        cb[17] = params.eye[1];
        cb[18] = params.eye[2];
        cb[19] = params.projYScale;
        cb[20] = vpW;
        cb[21] = vpH;
        cb[22] = static_cast<float>(probeBaseRes_);
        cb[23] = params.probeNormalOffset;
        // viz 侧：viewProj = 本帧；dotRadius / 尺寸 / baseRes。
        std::memcpy(cb + 24, params.viewProj, sizeof(params.viewProj));
        cb[40] = params.probeDotSize;
        cb[41] = vpW;
        cb[42] = vpH;
        cb[43] = static_cast<float>(probeBaseRes_);
        // vizExtra：dotMode = probe-viz 模式开辐照度点色；TLAS 未就绪时降级回
        // 级别色（probe-place 保形，无 RT 设备 M10.0 行为不变）。
        cb[44] = (params.probeVizIrradiance && tlasReady_) ? 1.0f : 0.0f;
    }
    cmd->writeBuffer(probeCB_, cb, sizeof(cb));

    // ---- traceCB_ 每帧全量覆写（64B）：灯参数与 writeResolveConstants 同源
    //（白光强度 2.5）；命中材质 = M10.1 降级口径（统一 albedo + ambient）；
    // shadow 参数 = M9.1 g_shadowParams 同值（bias 0.01 / tMax 50）。
    {
        const float tcb[16] = {
            params.lightDir[0], params.lightDir[1], params.lightDir[2], 0.0f,
            1.0f, 1.0f, 1.0f, 2.5f,
            0.45f, 0.39f, 0.30f, params.lightDir[3],   // 统一模型色（暖灰）+ 环境光
            0.01f, 50.0f,
            static_cast<float>(probeBaseRes_), 8.0f,
        };
        cmd->writeBuffer(traceCB_, tcb, sizeof(tcb));
    }

    // ---- (1) velocity prepass：与 visRaster 同一间接几何（identityIndex +
    // indirectArgs），换自己的 VS/PS 与 framebuffer（Q10-1 独立 pass 口径）。
    {
        nvrhi::GraphicsState vstate;
        vstate.setPipeline(velocityPSO_)
              .setFramebuffer(velocityFb_)
              .addBindingSet(velocitySet_)
              .setIndexBuffer(nvrhi::IndexBufferBinding()
                  .setBuffer(identityIndex_).setFormat(nvrhi::Format::R32_UINT)
                  .setOffset(0));
        vstate.viewport.addViewportAndScissorRect(viewport);
        vstate.setIndirectParams(indirectArgs_);
        cmd->setGraphicsState(vstate);
        cmd->drawIndexedIndirect(0, 1);
        // 本帧矩阵存为上一帧（下一帧 velocity 的 prev 侧；首帧已初始化 = 本帧）。
        std::memcpy(probePrevViewProj_, params.viewProj, sizeof(probePrevViewProj_));
    }

    // ---- (2) csPlace cascade 布点 dispatch（depth/velocity/prevDelta/atlas 的
    // SRV↔UAV 转换由绑定 require 自动插 barrier）。
    {
        nvrhi::ComputeState cstate;
        cstate.setPipeline(placePSO_).addBindingSet(placeSet_[probeParity_]);
        cmd->setComputeState(cstate);
        cmd->dispatch((probeBaseRes_ + 7u) / 8u, (probeBaseRes_ + 7u) / 8u, 1u);
    }

    // ---- (3) csTrace 探针追踪 dispatch（M10.1）：place atlas cur parity SRV ←
    // 刚写完 → irradiance atlas UAV 写；SRV↔UAV 转换由 trace set require 自动插。
    // TLAS 未就绪（无 RT / 无 fallback 几何）→ 一次性日志后跳过（probe-place
    // 的布点/点可视化不受影响，M10.0 行为保形；shadow 模式同口径降级）。
    if (tlasReady_) {
        const std::uint32_t store = 10u;
        nvrhi::ComputeState tstate;
        tstate.setPipeline(tracePSO_).addBindingSet(traceSet_[probeParity_]);
        cmd->setComputeState(tstate);
        cmd->dispatch(((probeBaseRes_ * store) + 7u) / 8u,
                      ((probeBaseRes_ * store) + 7u) / 8u, 4u);
    } else if (!probeTraceLogged_) {
        nl::core::log::info("probe: TLAS not ready (no RT support or no fallback "
                            "geometry) -- probe tracing disabled");
        probeTraceLogged_ = true;
    }
}

// M10.0：探针点可视化（tonemap 之后、swapchain 上）。PSO 对 swapchain 格式懒建
//（blitPSO_ 同款）；atlas/prevDelta 的 UAV→SRV 转换由 dots set require 自动插。
void Scene::drawProbeDots(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                          const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!probe_ || probeFailed_ || !framebuffer) return;
    if (!dotsPSO_) {
        const nvrhi::FramebufferInfo info = framebuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc gpDesc;
        gpDesc.setPrimType(nvrhi::PrimitiveType::TriangleStrip)
              .setVertexShader(vsProbeDots_)
              .setFragmentShader(psProbeDots_)
              .addBindingLayout(dotsLayout_);
        gpDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setCullMode(nvrhi::RasterCullMode::None);
        // overlay 语义：深度关（全探针可见，布点覆盖性证据优先于遮挡正确性）。
        gpDesc.renderState.depthStencilState
            .disableDepthTest()
            .disableDepthWrite()
            .disableStencil();
        dotsPSO_ = probeDevice_->createGraphicsPipeline(gpDesc, info);
        if (!dotsPSO_) {
            nl::core::log::deviceError("probe", "createGraphicsPipeline failed for dots");
            probeFailed_ = true;
            return;
        }
    }
    nvrhi::GraphicsState state;
    state.setPipeline(dotsPSO_)
         .setFramebuffer(framebuffer)
         .addBindingSet(dotsSet_[probeParity_]);
    state.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(state);
    const std::uint32_t total = probeBaseRes_ * probeBaseRes_ +
                                (probeBaseRes_ >> 1) * (probeBaseRes_ >> 1) +
                                (probeBaseRes_ >> 2) * (probeBaseRes_ >> 2) +
                                (probeBaseRes_ >> 3) * (probeBaseRes_ >> 3);
    nvrhi::DrawArguments args;
    args.setVertexCount(4)          // triangle-strip quad
        .setInstanceCount(total);
    cmd->draw(args);
    // atlas ping-pong 翻面：本帧 cur 变下一帧 prev（Q10-2）。
    probeParity_ ^= 1u;
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

// ============================================================================
// M9.0：material.slang（全屏 PS）与 direct.slang（compute CS）共用同一份绑定
// 槽位表 / 常量写入，避免两条链各写一份而漂移。§2.18 以本函数为实读结论。
// ============================================================================
void Scene::addResolveLayoutItems(nvrhi::BindingLayoutDesc& desc) const {
    desc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12));
    for (std::uint32_t slot = 4; slot < 4 + kMaxVisMaterials; ++slot)
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));   // t4..t7 基色
    for (std::uint32_t slot = 8; slot < 8 + kMaxVisMaterials; ++slot)
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));   // t8..t11 MR
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
}

void Scene::addResolveSetItems(nvrhi::BindingSetDesc& desc) const {
    desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, lightConstants_))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(2, matResolveConstants_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, visBuffer_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, vertexPool_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, localIndices_))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, clusterMeta_))
        .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_));
    for (std::uint32_t i = 0; i < kMaxVisMaterials; ++i)
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(4 + i, matBaseColor_[i]));
    for (std::uint32_t i = 0; i < kMaxVisMaterials; ++i)
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(8 + i, matMR_[i]));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, materialTable_));
}

void Scene::writeResolveConstants(nvrhi::ICommandList* cmd, const FrameParams& params) {
    // 写 b1 光照常量 + b2 尺寸常量（每帧；原 pulling 分支逐字搬移，两链共享）。
    LightConstants lc{};
    lc.lightDirW[0] = params.lightDir[0];
    lc.lightDirW[1] = params.lightDir[1];
    lc.lightDirW[2] = params.lightDir[2];
    lc.lightColor[0] = 1.0f; lc.lightColor[1] = 1.0f; lc.lightColor[2] = 1.0f;
    lc.lightColor[3] = 2.5f;                // 白光强度 2.5：无 IBL 只剩直射高光
    lc.cameraPos[0] = params.eye[0];
    lc.cameraPos[1] = params.eye[1];
    lc.cameraPos[2] = params.eye[2];
    std::memcpy(lc.baseColorFactor, pbrBaseColorFactor_, sizeof(lc.baseColorFactor));
    lc.mrFactor[0] = pbrMetallicFactor_;
    lc.mrFactor[1] = pbrRoughnessFactor_;
    lc.mrFactor[2] = params.options[0];     // --pbr-naive 对照开关
    cmd->writeBuffer(lightConstants_, &lc, sizeof(lc));

    const float mc[4] = {static_cast<float>(visW_), static_cast<float>(visH_),
                         0.f, 0.f};
    cmd->writeBuffer(matResolveConstants_, mc, sizeof(mc));
}

bool Scene::ensureVisbufTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                                std::string& outError) {
    const std::uint32_t w = static_cast<std::uint32_t>(viewportW + 0.5f);
    const std::uint32_t h = static_cast<std::uint32_t>(viewportH + 0.5f);
    if (w == 0 || h == 0) { outError = "visbuf: zero viewport"; return false; }
    if (visBuffer_ && visW_ == w && visH_ == h && visRasterSet_ && visVizSet_
        && (!pulling_ || matResolveSet_)) return true;
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

    // M7.1 / M9.0：材质反解与 compute 直光共用同一份槽位表。visBuffer_ 换对象
    // 时 directComputeSet_ 也必须重建（drawShadow 另有尺寸版本闸兜底）。
    directComputeSet_ = nullptr;
    if (pulling_) {
        nvrhi::BindingSetDesc mSet;
        addResolveSetItems(mSet);
        matResolveSet_ = device->createBindingSet(mSet, matResolveLayout_);
        if (!matResolveSet_) { outError = "createBindingSet failed for mat resolve"; return false; }
    }
    return true;
}

bool Scene::ensurePipelines(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                            std::string& outError) {
    // T0：所有模式都会经后端 init 走到这里——存 device 供 raster 主路径在 draw()
    // 里懒建 ensureRtTargets（Scene 不存通用 device，rtDevice_ 只在 rt 系模式赋值）。
    t0Device_ = device;
    // M-P3：物理调试线框管线（LINE_LIST）。与主管线共用 framebuffer 格式，只建一次，
    // 默认不画；debugLines 关闭时零开销（S7.0 渲染基线字节不变）。
    // 入口名传 "main"：SPIR-V 恒 main（NVRHI Vulkan 把 desc.entryName 写进
    // pName，传 vsMain 会令 vkCreateGraphicsPipelines 以 VK_ERROR_UNKNOWN 失败）；
    // DX12 完全忽略 setEntryName、直接用 blob（§2.4 两阶段规则，compute 同款先例）。
    if (!dbgLines_.ensure(device, info, shaderSuffix_.c_str(), "main", "main", outError))
        return false;

    // T0 T2：场景主 pass 改渲到离屏 HDR（RGBA16_FLOAT + D32）后再经 tonemap 直通
    // 合成到 swapchain（Q-H3=UE 口径）。PSO 与渲染目标格式锁定，场景管线必须按
    // hdr 格式建——照 hiz prepassInfo 先例直接按格式构造 FramebufferInfo，不依赖
    // 真实纹理（hdrSceneFb_ 随视口在 ensureRtTargets 懒建）。swapchain RGBA8 的
    // info 仍用于 dbgLines_/visRaster（overlay 画在 tonemap 输出之上）。
    nvrhi::FramebufferInfo hdrInfo;
    hdrInfo.addColorFormat(nvrhi::Format::RGBA32_FLOAT);
    hdrInfo.setDepthFormat(nvrhi::Format::D32);

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
                auto pipeline = device->createGraphicsPipeline(desc, hdrInfo);
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
        vizPSO_ = device->createGraphicsPipeline(vDesc, hdrInfo);
        if (!vizPSO_) { outError = "createGraphicsPipeline failed for hiz viz"; return false; }
    }
    // M7.0（S7.0）：visbuf 两条管线。必须排在 cull 分支之前——visbuf 复用 cull_
    // 的资源通路，但那四套 (fill×cull) 管线在 visbuf 下完全用不上。
    if (visbuf_) {
        // M9.0：shadow 链不用 matResolvePSO_；放行条件改查 directPSO_，否则
        // material PSO 建失败会连 vis raster 一起被跳过 → 全屏黑。
        if (visRasterPSO_ && visVizPSO_ &&
            (shadow_ ? (directPSO_ != nullptr) : (!pulling_ || matResolvePSO_))) return true;
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
        visVizPSO_ = device->createGraphicsPipeline(vDesc, hdrInfo);
        if (!visVizPSO_) { outError = "createGraphicsPipeline failed for vis viz"; return false; }

        // M7.1（S7.1）：材质反解全屏 pass（pulling 模式替换 viz）。全屏三角形，
        // 深度测试/写入关、cull none——与 viz 同款屏幕覆盖，只是片元改走材质着色。
        if (pulling_) {
            nvrhi::GraphicsPipelineDesc mDesc;
            mDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                 .setVertexShader(vsMatResolve_)
                 .setFragmentShader(psMatResolve_)
                 .addBindingLayout(matResolveLayout_);
            mDesc.renderState.rasterState
                .setFrontCounterClockwise(true)
                .setFillMode(nvrhi::RasterFillMode::Solid)
                .setCullMode(nvrhi::RasterCullMode::None);
            matResolvePSO_ = device->createGraphicsPipeline(mDesc, hdrInfo);
            if (!matResolvePSO_) { outError = "createGraphicsPipeline failed for mat resolve"; return false; }
        }
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
                auto pipeline = device->createGraphicsPipeline(desc, hdrInfo);
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
            auto pipeline = device->createGraphicsPipeline(desc, hdrInfo);
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
    // M-P3：调试管线随主管线一起重建（格式不变，重 ensure 即重建同格式 PSO）。
    dbgLines_.invalidate();
    // M-PT0：粒子管线随主管线一起重建（实例缓冲/模拟状态不清，draw 下帧懒建）。
    particles_.invalidate();
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
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
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

// ---- M-E1b.4：阴影 CPU 数学辅助（全部纯 CPU、列主序、D3D 深度 z∈[0,1] 约定，
// 与 nl::core::Matrix4 的口径一致；core 层相机只做轨道透视，这里独立实现
// 正交 lookAt，不改动 core）。双后端共享同一份 CPU 数值（bytes 口径前提）。----

// 列主序矩阵变换点（w=1，不含透视除法——正交下 w 恒 1）。
static void shadowTransformPoint(const float* m, const float p[3], float out[3]) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

// 列主序 3x3 部分的最大列长（均匀/非均匀缩放下联合球半径的保守放大）。
static float shadowMaxScale(const float* m) {
    float s = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float dx = m[c * 4 + 0], dy = m[c * 4 + 1], dz = m[c * 4 + 2];
        s = std::max(s, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return s;
}

// 左手 lookAt（D3D XMMatrixLookAtLH 口径：视线沿 +z，与 shadowOrtho 的 D3D
// z∈[0,1] 深度映射配对——右手 lookAt 会让 clip.z 全负、深度图全被近平面裁掉）。
// 列主序。
static void shadowLookAt(const float eye[3], const float center[3], const float up[3],
                         float out[16]) {
    float z[3] = { center[0] - eye[0], center[1] - eye[1], center[2] - eye[2] };
    const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& v : z) v /= zl;
    float x[3] = {
        up[1] * z[2] - up[2] * z[1],
        up[2] * z[0] - up[0] * z[2],
        up[0] * z[1] - up[1] * z[0],
    };
    const float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (float& v : x) v /= xl;
    const float y[3] = {
        z[1] * x[2] - z[2] * x[1],
        z[2] * x[0] - z[0] * x[2],
        z[0] * x[1] - z[1] * x[0],
    };
    out[0] = x[0]; out[1] = y[0]; out[2] = z[0]; out[3] = 0.0f;
    out[4] = x[1]; out[5] = y[1]; out[6] = z[1]; out[7] = 0.0f;
    out[8] = x[2]; out[9] = y[2]; out[10] = z[2]; out[11] = 0.0f;
    out[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    out[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    out[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    out[15] = 1.0f;
}

// D3D 正交投影（z 映射 [0,1]），列主序。
static void shadowOrtho(float l, float r, float b, float t, float n, float f, float out[16]) {
    const float xx = 2.0f / (r - l), yy = 2.0f / (t - b), zz = 1.0f / (f - n);
    out[0] = xx; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    out[4] = 0.0f; out[5] = yy; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = 0.0f; out[9] = 0.0f; out[10] = zz; out[11] = 0.0f;
    out[12] = -(r + l) / (r - l);
    out[13] = -(t + b) / (t - b);
    out[14] = -n / (f - n);
    out[15] = 1.0f;
}

// out = a * b（列主序 4x4）。
static void shadowMul(const float* a, const float* b, float out[16]) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

bool Scene::initScene(nvrhi::IDevice* device, const char* shaderSuffix,
                      const char* vsEntry, const char* psEntry,
                      const SceneObjectDesc* objects, std::uint32_t objectCount,
                      const SceneLightDesc* lights, std::uint32_t lightCount,
                      float ambient, const char* sceneName, std::string& outError) {
    shaderSuffix_ = shaderSuffix;   // M-P3：供 dbgLines_.ensure 定位 debug_line 字节码
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
    // M-E1b.5：固化分组映射，updateObjects 每帧按同一规则重打包（槽号一一对应）。
    sceneBatchObjects_ = batchObjects;
    sceneObjectCount_ = objectCount;

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
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
        // M-E1b.4：b2 阴影矩阵 + t7 R32F 阴影图数组——无阴影场景也恒绑定
        //（enabled=0 时 shader 分支不进、输出与 E1b.3 逐字节一致）。
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(2))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(7));
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
    // M-E1b.4：方向灯阴影槽位分配——按场景序取前 kMaxShadowLights 盏 shadow=true
    // 的方向灯；超出显式报错（不静默降级）。t6 的 posRng.w 对方向灯复用为
    // shadowSlot+1（0=无阴影；方向灯分支原本不读该字段）。
    std::vector<std::int32_t> lightShadowSlot(lightCount, -1);
    std::uint32_t shadowSlotCount = 0;
    for (std::uint32_t i = 0; i < lightCount; ++i) {
        if (lights[i].type != 0 || !lights[i].shadow) continue;
        if (shadowSlotCount >= Scene::kMaxShadowLights) {
            outError = "initScene: too many shadow-casting directional lights (max 4)";
            return false;
        }
        lightShadowSlot[i] = static_cast<std::int32_t>(shadowSlotCount++);
    }
    shadowCount_ = shadowSlotCount;
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
        // M-E1b.4：方向灯 posRng.w = shadowSlot+1（0=无阴影）；点光保持 InvRadius。
        if (lights[i].type == 0)
            d2[3] = static_cast<float>(lightShadowSlot[i] + 1);
        else
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

    // ---- M-E1b.4：场景联合 AABB（世界空间）→ 每阴影灯的正交 lightViewProj ----
    // 逐实例：batch 局部包围球经世界矩阵变换（中心变换 + 半径按最大列长放大），
    // 累积成全场 AABB；灯视锥 = AABB 放大 10% + 0.25 margin，深度范围包住全场。
    // 矩阵 CPU 一次预算、每帧原样写 b2（shader 只消费，双后端同数值）。
    if (shadowCount_ > 0) {
        float mn[3] = { 0.0f, 0.0f, 0.0f }, mx[3] = { 0.0f, 0.0f, 0.0f };
        bool first = true;
        for (std::size_t b = 0; b < batchFiles.size(); ++b) {
            const PreparedBatch& pb = prepared[b];
            for (std::uint32_t i : batchObjects[b]) {
                const float* w = objects[i].world;
                float c[3], r3[3];
                shadowTransformPoint(w, pb.sphere, c);
                const float r = pb.sphere[3] * shadowMaxScale(w);
                for (int a = 0; a < 3; ++a) {
                    r3[a] = r;
                    if (first) { mn[a] = c[a] - r; mx[a] = c[a] + r; }
                    else { mn[a] = std::min(mn[a], c[a] - r); mx[a] = std::max(mx[a], c[a] + r); }
                }
                first = false;
            }
        }
        const float center[3] = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                                  (mn[2] + mx[2]) * 0.5f };
        const float radius = 0.5f * std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) +
                                              (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                              (mx[2] - mn[2]) * (mx[2] - mn[2]));
        const float half = radius * 1.1f + 0.25f;
        const float dist = radius * 2.5f + 2.0f;
        const float nearZ = std::max(0.05f, dist - radius - 0.5f);
        const float farZ = dist + radius + 0.5f;
        float ortho[16];
        shadowOrtho(-half, half, -half, half, nearZ, farZ, ortho);
        for (std::uint32_t i = 0; i < lightCount; ++i) {
            const std::int32_t slot = lightShadowSlot[i];
            if (slot < 0) continue;
            const float* dir = lights[i].dir;   // 光照传播方向（已归一化）
            const float eye[3] = { center[0] - dir[0] * dist,
                                   center[1] - dir[1] * dist,
                                   center[2] - dir[2] * dist };
            // 灯方向接近竖直时换上轴，避免 lookAt 退化。
            float up[3] = { 0.0f, 1.0f, 0.0f };
            if (std::abs(dir[1]) > 0.99f) { up[1] = 0.0f; up[2] = 1.0f; }
            float view[16];
            shadowLookAt(eye, center, up, view);
            shadowMul(ortho, view, shadowViewProj_[slot]);
            // 固定 bias：正交 + 场景尺寸 ~个位数单位，0.002~0.005（归一化深度）实测干净。
            shadowBias_[slot] = 0.003f;
        }
    }

    nvrhi::BufferDesc lightDesc;
    lightDesc.byteSize = lightData.size() * sizeof(float);
    lightDesc.structStride = 16;             // StructuredBuffer<float4>
    lightDesc.canHaveTypedViews = true;
    lightDesc.debugName = "SceneLights";
    lightDesc.initialState = nvrhi::ResourceStates::CopyDest;
    sceneLights_ = device->createBuffer(lightDesc);
    if (!sceneLights_) { outError = "createBuffer failed for ecs scene lights"; return false; }

    // ---- 5.6 M-E1b.4：阴影资源（恒建，无阴影场景也绑定空转的 b2/t7——
    // §2.7 单 set 纪律下 layout 与 set 必须对齐；R32F 1024x1024x4 ≈ 16MB，
    // POT 尺寸沿 §2.8 金字塔先例）----
    {
        nvrhi::TextureDesc smDesc;
        smDesc.width = 1024;
        smDesc.height = 1024;
        smDesc.arraySize = Scene::kMaxShadowLights;
        smDesc.format = nvrhi::Format::R32_FLOAT;
        smDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
        smDesc.isRenderTarget = true;
        smDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        smDesc.setClearValue(nvrhi::Color(1.0f));   // 每帧 clear=最远；DX12 要求资源带 clear value（否则 perf 警告计数）
        smDesc.debugName = "ShadowMapArray";
        shadowMap_ = device->createTexture(smDesc);
        if (!shadowMap_) { outError = "createTexture failed for shadow map array"; return false; }
        for (std::uint32_t s = 0; s < Scene::kMaxShadowLights; ++s) {
            nvrhi::FramebufferDesc fbDesc;
            fbDesc.addColorAttachment(shadowMap_,
                nvrhi::TextureSubresourceSet(0, 1, s, 1));
            shadowFb_[s] = device->createFramebuffer(fbDesc);
            if (!shadowFb_[s]) { outError = "createFramebuffer failed for shadow layer"; return false; }
        }
        // shadow pass 的 b0：与 SceneConstants 同 160B 形状（只用 g_mvp = lightViewProj）。
        nvrhi::BufferDesc slcDesc;
        slcDesc.byteSize = sizeof(SceneConstants);
        slcDesc.isConstantBuffer = true;
        slcDesc.isVolatile = true;
        slcDesc.maxVersions = 16;
        slcDesc.debugName = "ShadowLightCB";
        shadowLightCB_ = device->createBuffer(slcDesc);
        if (!shadowLightCB_) { outError = "createBuffer failed for shadow light CB"; return false; }
        // psScene 读的 b2（ShadowConstantsCB 320B）。
        nvrhi::BufferDesc scDesc;
        scDesc.byteSize = sizeof(ShadowConstantsCB);
        scDesc.isConstantBuffer = true;
        scDesc.isVolatile = true;
        scDesc.maxVersions = 16;
        scDesc.debugName = "ShadowConstants";
        shadowConstants_ = device->createBuffer(scDesc);
        if (!shadowConstants_) { outError = "createBuffer failed for shadow constants"; return false; }

        // shadow pass shader（vsShadow/psShadow 双目标产物；两阶段入口名约定同 initScene 第 1 步）。
        std::vector<std::uint8_t> shVsBlob, shPsBlob;
        if (!readFileBytes(shaderPath("vsshadow").c_str(), shVsBlob) ||
            !readFileBytes(shaderPath("psshadow").c_str(), shPsBlob)) {
            outError = std::string("failed to load shadow shaders (") + shaderSuffix +
                       ") from " NL_SHADER_DIR;
            return false;
        }
        // 入口名 "main"：SPIR-V 恒 main；DX12 忽略 setEntryName、直接用 blob 唯一
        // 入口（编译期 -entry vsShadow/psShadow 写入）——§2.4 两阶段规则（M4.1 同款）。
        nvrhi::ShaderDesc shVsDesc;
        shVsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName("main");
        nvrhi::ShaderDesc shPsDesc;
        shPsDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName("main");
        vsShadow_ = device->createShader(shVsDesc, shVsBlob.data(), shVsBlob.size());
        psShadow_ = device->createShader(shPsDesc, shPsBlob.data(), shPsBlob.size());
        if (!vsShadow_ || !psShadow_) { outError = "createShader failed for shadow pass"; return false; }

        nvrhi::VertexAttributeDesc shAttrs[3] = {
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
        shadowInputLayout_ = device->createInputLayout(shAttrs, 3, vsShadow_);
        if (!shadowInputLayout_) { outError = "createInputLayout failed for shadow pass"; return false; }

        // shadow pass 单一 BindingSet：b0（lightViewProj）+ t5（实例）。
        nvrhi::BindingLayoutDesc shLayoutDesc;
        shLayoutDesc.setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
        shadowBindingLayout_ = device->createBindingLayout(shLayoutDesc);
        if (!shadowBindingLayout_) { outError = "createBindingLayout failed for shadow pass"; return false; }

        // shadow 管线：cull=None（abs(NdotL) 双面照亮的几何同样投影）+ 无深度测试。
        nvrhi::GraphicsPipelineDesc shPipeDesc;
        shPipeDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setInputLayout(shadowInputLayout_)
            .setVertexShader(vsShadow_)
            .setFragmentShader(psShadow_)
            .addBindingLayout(shadowBindingLayout_);
        shPipeDesc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::None);
        // 阴影图无 depth attachment——显式关深度/模板（默认状态会启用深度操作，
        // 与「framebuffer 无深度格式」冲突，DX12 validation 直接拒绝建管线）。
        shPipeDesc.renderState.depthStencilState
            .disableDepthTest()
            .disableDepthWrite()
            .disableStencil();
        // M-E1b.4：nearest-depth 用 R32F MIN 混合实现（src/dst One + BlendOp::Min，
        // 语义 = 深度测试 LessEqual 但顺序无关，免建 depth attachment）。
        // D3D12 对 float32 RT 强制支持 MIN/MAX blend；Vulkan 规范同一张强制表
        // （R32_SFLOAT 要求支持 VK_BLEND_OP_MIN/MAX），双后端对称。
        // 踩坑：无深度测试 + 无混合时 psShadow 无条件写 → 后画的（地面盘覆盖
        // 整个灯视锥）冲掉先画的物体深度 → 阴影图全是地面深度 → 永远判亮。
        shPipeDesc.renderState.blendState.setRenderTarget(0,
            nvrhi::BlendState::RenderTarget()
                .setBlendEnable(true)
                .setSrcBlend(nvrhi::BlendFactor::One)
                .setDestBlend(nvrhi::BlendFactor::One)
                .setBlendOp(nvrhi::BlendOp::Min)
                .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                .setDestBlendAlpha(nvrhi::BlendFactor::One)
                .setBlendOpAlpha(nvrhi::BlendOp::Min));
        pipelineShadow_ = device->createGraphicsPipeline(shPipeDesc,
                                                         shadowFb_[0]->getFramebufferInfo());
        if (!pipelineShadow_) { outError = "createGraphicsPipeline failed for shadow pass"; return false; }
    }

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
               .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, sceneLights_))
               // M-E1b.4：b2 阴影矩阵 + t7 阴影图数组（全 batch 共享同一资源）。
               .addItem(nvrhi::BindingSetItem::ConstantBuffer(2, shadowConstants_))
               .addItem(nvrhi::BindingSetItem::Texture_SRV(7, shadowMap_));
        batch.set = device->createBindingSet(setDesc, sceneBindingLayout_);
        if (!batch.set) { outError = "createBindingSet failed for ecs scene batch"; return false; }

        // shadow pass 的 set：b0 = shadowLightCB_ + t5 实例（同一 batch 缓冲复用）。
        nvrhi::BindingSetDesc shSetDesc;
        shSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, shadowLightCB_))
                 .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, batch.instances));
        batch.shadowSet = device->createBindingSet(shSetDesc, shadowBindingLayout_);
        if (!batch.shadowSet) { outError = "createBindingSet failed for shadow pass batch"; return false; }
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
        // M-E1b.5：instances **不设 permanent**（SRV 冻结会让每帧 writeBuffer 的
        // require(CopyDest) 直接报错——NVRHI 对 permanent buffer 不插 barrier）。
        // 改走 tracking：末态由 instancesInSRV_ 跨列表跟踪（update 末态=CopyDest、
        // draw 末态=SRV，序列完全确定），draw/update 各自在列表 open 后如实声明。
        // 末态初值 = CopyDest（本列表 writeBuffer 无转换直接落盘）。
        instanceTrackSRV_ = false;
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
                        " shadowLights=", shadowCount_,
                        " | batches=", sceneBatches_.size(),
                        " | triangles=", triangleCount_,
                        " | shader=", shaderSuffix);
    return true;
}

// M-E1b.5 渲染用例：每帧刷新实例数据。灯缓冲/顶点/纹理/管线全不动，只重打包
// float4 实例（矩阵 4 列 + tint + roughness/metallic）后 writeBuffer 各 batch.instances。
// upload 三步（beginTracking CopyDest → writeBuffer → setPermanent ShaderResource）
// 与 initScene 第 6 步同款；close+execute 同帧生效（renderFrame 在其后才开命令列表）。
bool Scene::updateObjects(nvrhi::IDevice* device,
                          const SceneObjectDesc* objects, std::uint32_t objectCount,
                          std::string& outError) {
    if (!device) { outError = "updateObjects: device null"; return false; }
    // 闸门不能用 ready()：四条 scene 管线在首次 renderFrame 的 ensurePipeline
    // 才建（resize 也会置空重建），initScene 之后、首帧之前 ready() 恒 false。
    // 这里只关心实例缓冲是否已建（initScene 第 5 步）。
    if (sceneMode_ == false || sceneBatches_.empty() || !sceneBatches_[0].instances) {
        outError = "updateObjects: scene instances not initialized (initScene first)";
        return false;
    }
    if (objectCount != sceneObjectCount_) {
        outError = "updateObjects: objectCount mismatch (initScene " +
                   std::to_string(sceneObjectCount_) + " vs update " +
                   std::to_string(objectCount) + ")";
        return false;
    }
    // §2.8 铁律「帧内禁开第二 cmd list」精神：所有 batch 合并进单个 upload
    // 命令列表，一次 open/close/execute（viewer 帧首调用，renderFrame 随后才开）。
    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    for (std::size_t b = 0; b < sceneBatches_.size(); ++b) {
        SceneMeshBatch& batch = sceneBatches_[b];
        const std::vector<std::uint32_t>& slotMap = sceneBatchObjects_[b];
        std::vector<float> data(static_cast<std::size_t>(slotMap.size()) * 7u * 4u, 0.0f);
        std::uint32_t slot = 0;
        for (std::uint32_t i : slotMap) {
            const float* w = objects[i].world;   // 列主序 m[col*4+row]
            float* dst = data.data() + static_cast<std::size_t>(slot) * 7 * 4;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    dst[c * 4 + r] = w[c * 4 + r];
            dst[16] = objects[i].tint[0];
            dst[17] = objects[i].tint[1];
            dst[18] = objects[i].tint[2];
            dst[19] = 0.0f;
            dst[20] = objects[i].roughness;
            dst[21] = objects[i].metallic;
            dst[22] = 0.0f;
            dst[23] = 0.0f;
            ++slot;
        }
        // beginTrackingBufferState 参数 = **当前**状态（NVRHI 语义：informs the
        // tracker of the current state）。instances 走 tracking 不冻结（见 initScene
        // 注释），当前态由 instanceTrackSRV_ 跨列表跟踪；writeBuffer require
        // CopyDest 由 tracker 自动插 barrier，write 后末态 = CopyDest。
        upload->beginTrackingBufferState(batch.instances,
            instanceTrackSRV_ ? nvrhi::ResourceStates::ShaderResource
                              : nvrhi::ResourceStates::CopyDest);
        upload->writeBuffer(batch.instances, data.data(), data.size() * sizeof(float));
    }
    upload->close();
    device->executeCommandList(upload);
    instanceTrackSRV_ = false;   // update 列表末态 = CopyDest（writeBuffer 无转换落盘）
    return true;
}

void Scene::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                 const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!ready() || !framebuffer) return;

    // M8.2：rt 模式——不走光栅化路径，compute RayQuery 调试射线 + 全屏呈现
    //（buildTlasFrame 已在 recordFrame 里先记录，同列表顺序保证射线打本帧 TLAS）。
    if (rt_) {
        drawRt(cmd, framebuffer, viewport, params);
        return;
    }

    // T0 T2：raster 主路径（sceneMode/cull/lod/hiz/skin）渲到离屏 HDR——懒建
    // ensureRtTargets（rt/shadow 在各自分支懒建、reference 在 initReference 建，
    // 均自管呈现，不在此列）。管线按 R11G11B10+D32 fbInfo 重建（hdrInfo，见
    // ensurePipelines），故 raster 各模式必须走 HDR 帧缓冲——直渲 RGBA8 swapchain
    // 会因 PSO 格式失配报 validation 错。建失败则回落直渲（t0Active=false）。
    std::string t0Err;
    bool t0Active = false;
    nvrhi::IFramebuffer* sceneFb = framebuffer;
    if (!rt_ && !shadow_ && !reference_ && framebuffer) {
        if (!hdrSceneColor_) {
            if (!t0Device_) {
                nl::core::log::deviceError("t0", "device not captured yet");
            } else {
                const float vpW = viewport.maxX - viewport.minX;
                const float vpH = viewport.maxY - viewport.minY;
                if (ensureRtTargets(t0Device_, vpW, vpH, t0Err))
                    nl::core::log::info("t0: offscreen HDR scene targets ready (raster path)");
                else
                    nl::core::log::deviceError("t0", t0Err);
            }
        }
        t0Active = hdrSceneFb_ && t0Depth_ && hdrSceneColor_;
        if (t0Active) sceneFb = hdrSceneFb_;
        if (t0Active && !beginHdrSceneTarget(cmd, params.clearColor, t0Err)) {
            t0Active = false;
            sceneFb = framebuffer;
            nl::core::log::deviceError("t0", t0Err);
        }
    }

    // M-E1b.1：ECS 场景路径——逐 batch 绑定各自的 set/VB/IB/t5 实例缓冲，
    // drawIndexed 的 SV_InstanceID 从 0 起，直接按 id*5 寻址本 batch 实例数据。
    // 不设 startInstanceLocation：DX12 不把 StartInstanceLocation 计入 SV_InstanceID，
    // 旧实现多 batch 时所有批次都读实例槽 0（详见第 5 步与 MILESTONES 踩坑）。
    if (sceneMode_) {
        // ---- M-E1b.5：instances 状态如实登记（不设 permanent，见 initScene 注释）。
        // 跟踪态不跨命令列表（NVRHI commandListSubmitted 清空），每个新列表必须在
        // 首次绑定前声明当前态；SRV 绑定的 require 由 tracker 插 barrier，列表
        // 末态 = SRV。
        const nvrhi::ResourceStates instState = instanceTrackSRV_
            ? nvrhi::ResourceStates::ShaderResource
            : nvrhi::ResourceStates::CopyDest;
        for (const SceneMeshBatch& batch : sceneBatches_)
            cmd->beginTrackingBufferState(batch.instances, instState);

        // ---- M-E1b.4：b2 阴影常量 + 阴影图状态登记（无条件做——主 pass 恒绑定
        // b2/t7，无阴影场景也要写零版 b2（enabled=0 → psScene 阴影分支不进、
        // factor 恒 1.0）并登记纹理状态，否则 volatile CB 未写即用 = NVRHI 报错）。
        ShadowConstantsCB sc{};
        for (std::uint32_t s = 0; s < shadowCount_; ++s) {
            std::memcpy(sc.viewProj[s], shadowViewProj_[s], sizeof(sc.viewProj[s]));
            sc.params[s][0] = 1.0f;
            sc.params[s][1] = shadowBias_[s];
            sc.params[s][2] = 1024.0f;   // 与 shadowMap_ 尺寸一致
        }
        cmd->writeBuffer(shadowConstants_, &sc, sizeof(sc));
        // 本命令列表先登记阴影图初始状态（NVRHI 校验：未知 prior state 报错）。
        cmd->beginTrackingTextureState(shadowMap_,
            nvrhi::TextureSubresourceSet(0, nvrhi::TextureSubresourceSet::AllMipLevels,
                                         0, nvrhi::TextureSubresourceSet::AllArraySlices),
            nvrhi::ResourceStates::ShaderResource);

        // ---- shadow passes（先于主 pass；clear 在任何 setGraphicsState 之前——
        // 与 visbuf clearBufferUInt 同款 Vulkan render pass 纪律）。
        // 每阴影灯一层 R32F：clear=1.0（最远）→ 渲染深度 → 状态切回 SRV 供 psScene。
        if (shadowCount_ > 0 && pipelineShadow_) {
            for (std::uint32_t s = 0; s < shadowCount_; ++s) {
                cmd->clearTextureFloat(shadowMap_,
                    nvrhi::TextureSubresourceSet(0, 1, s, 1), nvrhi::Color(1.0f));
            }
            cmd->setTextureState(shadowMap_,
                nvrhi::TextureSubresourceSet(0, nvrhi::TextureSubresourceSet::AllMipLevels,
                                             0, nvrhi::TextureSubresourceSet::AllArraySlices),
                nvrhi::ResourceStates::RenderTarget);

            SceneConstants lcb{};   // b0 g_mvp = lightViewProj（shadow pass 语义）
            for (std::uint32_t s = 0; s < shadowCount_; ++s) {
                std::memcpy(lcb.mvp, shadowViewProj_[s], sizeof(lcb.mvp));
                cmd->writeBuffer(shadowLightCB_, &lcb, sizeof(lcb));
                for (const SceneMeshBatch& b : sceneBatches_) {
                    nvrhi::GraphicsState state;
                    state.setPipeline(pipelineShadow_)
                         .setFramebuffer(shadowFb_[s])
                         .addBindingSet(b.shadowSet)
                         .addVertexBuffer(nvrhi::VertexBufferBinding()
                             .setBuffer(b.vb).setSlot(0).setOffset(0))
                         .setIndexBuffer(nvrhi::IndexBufferBinding()
                             .setBuffer(b.ib).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
                    state.viewport.addViewportAndScissorRect(
                        nvrhi::Viewport(1024.0f, 1024.0f));
                    cmd->setGraphicsState(state);
                    nvrhi::DrawArguments args;
                    args.setVertexCount(b.indexCount)
                        .setInstanceCount(b.instanceCount);
                    cmd->drawIndexed(args);
                }
            }
            cmd->setTextureState(shadowMap_,
                nvrhi::TextureSubresourceSet(0, nvrhi::TextureSubresourceSet::AllMipLevels,
                                             0, nvrhi::TextureSubresourceSet::AllArraySlices),
                nvrhi::ResourceStates::ShaderResource);
        }

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
                 .setFramebuffer(sceneFb)   // T0 T2：场景渲到离屏 HDR（t0Active=false 回落 swapchain）
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
        instanceTrackSRV_ = true;   // 本列表末态 = SRV（所有绑定 require 已插 barrier）
        // T0 T2：HDR 场景色 → swapchain 直通合成（HUD/overlay 在其后叠加）。
        if (t0Active) {
            std::string tmErr;
            if (!presentTonemap(cmd, framebuffer, viewport, tmErr))
                nl::core::log::deviceError("t0", tmErr);
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
            if (!visbufFailed_ && visRasterPSO_ &&
                (shadow_ ? (directPSO_ != nullptr)
                         : (pulling_ ? (matResolvePSO_ != nullptr)
                                     : (visVizPSO_ != nullptr)))) {
                // (1) b0 只写 MVP（visbuf.slang / material.slang 的 b0 与 model.slang
                // 同 160B 布局；material.slang 的 g_options.x = 着色模式 = params.shading）。
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

                // (5) reference ? 离线参考 : shadow ? compute 直光 : pulling ? 材质反解 : 全屏 viz。
                if (reference_) {
                    // M9.2 优先于 shadow：渐进式多帧累积直光真值图（裁判 M9.1 阴影射线）。
                    drawReference(cmd, framebuffer, viewport, params);
                } else if (shadow_) {
                    // b1/b2 与 pulling 共用写入（逐像素一致的前提）；b3 清屏色、
                    // dispatch、present 都在 drawShadow 内。
                    drawShadow(cmd, framebuffer, viewport, params);
                } else if (pulling_) {
                    writeResolveConstants(cmd, params);

                    // 全屏三角形覆盖整屏：每像素反解 vis → 顶点拉取 → 材质着色。
                    nvrhi::GraphicsState mstate;
                    mstate.setPipeline(matResolvePSO_)
                          .setFramebuffer(sceneFb)   // T0 T2：场景渲到离屏 HDR（t0Active=false 回落 swapchain）
                          .addBindingSet(matResolveSet_);
                    mstate.viewport.addViewportAndScissorRect(viewport);
                    cmd->setGraphicsState(mstate);
                    nvrhi::DrawArguments margs;
                    margs.setVertexCount(3);   // 覆盖屏的大三角形
                    cmd->draw(margs);
                    // M10.0：probe-place 的 velocity prepass + cascade 布点
                    //（matResolve 之后——布点读的是本帧几何已经画完的状态；
                    // tonemap 之前——布点与本帧 HDR 输出同帧）。
                    if (probe_) drawProbeFrame(cmd, viewport, params);
                    // T0 T2：HDR → swapchain 直通合成（reference/shadow 走自己的
                    // 呈现路径，tonemap 只挂在本分支，避免双重呈现）。
                    if (t0Active) {
                        std::string tmErr;
                        if (!presentTonemap(cmd, framebuffer, viewport, tmErr))
                            nl::core::log::deviceError("t0", tmErr);
                    }
                    // M10.0：探针点可视化（tonemap 之后画在 swapchain 上，
                    // 深度关——全探针可见，布点覆盖性证据优先于遮挡正确性）。
                    if (probe_) drawProbeDots(cmd, framebuffer, viewport, params);
                } else {
                    // 全屏 viz：读同一块 buffer 的 SRV 视角，解包 → 簇色/三角色。
                    // 深度测试/写入关，整屏覆盖 (4) 的占位输出。
                    nvrhi::GraphicsState vstate;
                    vstate.setPipeline(visVizPSO_)
                          .setFramebuffer(sceneFb)   // T0 T2：场景渲到离屏 HDR（t0Active=false 回落 swapchain）
                          .addBindingSet(visVizSet_);
                    vstate.viewport.addViewportAndScissorRect(viewport);
                    cmd->setGraphicsState(vstate);
                    nvrhi::DrawArguments vargs;
                    vargs.setVertexCount(3);   // 覆盖屏的大三角形
                    cmd->draw(vargs);
                    // T0 T2：HDR → swapchain 直通合成（同 pulling 分支口径）。
                    if (t0Active) {
                        std::string tmErr;
                        if (!presentTonemap(cmd, framebuffer, viewport, tmErr))
                            nl::core::log::deviceError("t0", tmErr);
                    }
                }
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
             .setFramebuffer(sceneFb)   // T0 T2：场景渲到离屏 HDR（t0Active=false 回落 swapchain）
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
                  .setFramebuffer(sceneFb)   // T0 T2：overlay 合成进 HDR（tonemap 统一输出）
                  .addBindingSet(vizSet_);
            vstate.viewport.addViewportAndScissorRect(viewport);
            cmd->setGraphicsState(vstate);
            nvrhi::DrawArguments vargs;
            vargs.setVertexCount(4);   // triangle-strip 覆盖四边形
            cmd->draw(vargs);
        }
        // T0 T2：HDR 场景色 → swapchain 直通合成（HUD/overlay 在其后叠加）。
        if (t0Active) {
            std::string tmErr;
            if (!presentTonemap(cmd, framebuffer, viewport, tmErr))
                nl::core::log::deviceError("t0", tmErr);
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

    // pbr-direct：b1 LightConstants 同款 volatile 每帧写。M7.2：per-prim 批次
    // 各写各的 b1（光照/相机同值，因子按批不同）。只有 init/initSkin 的绑定集
    // 携带 b1（psMain 引用）；initCluster 场景（lod 模式同样走这里）没有 b1
    // 槽位、modelBatches_ 为空，绝不能碰——写空句柄 = NVRHI 内部空指针段错误。
    if (skinMode_) {
        // M8.2 修漏（pbr-direct merge 起的既有回归）：initSkin 的绑定集同样
        // 携带 b1（psMain 引用），但 skin 不走 per-prim 批次（modelBatches_
        // 为空）——下面的循环写不到 skin 绑定的 lightConstants_ 成员，NVRHI
        // 每帧报「volatile CB before written into」（gl1 双卡 120/100 帧各
        // 计 1 次 deviceError，验收闸直接挂）。skin 补写成员句柄，口径与
        // 批次路径一致（rigged 资产无 PBR 材质 → 因子用 dummy MR 值）。
        LightConstants lc{};
        lc.lightDirW[0] = params.lightDir[0];
        lc.lightDirW[1] = params.lightDir[1];
        lc.lightDirW[2] = params.lightDir[2];
        lc.lightColor[0] = 1.0f; lc.lightColor[1] = 1.0f; lc.lightColor[2] = 1.0f;
        lc.lightColor[3] = 2.5f;
        lc.cameraPos[0] = params.eye[0];
        lc.cameraPos[1] = params.eye[1];
        lc.cameraPos[2] = params.eye[2];
        lc.mrFactor[1] = 1.0f;   // roughness=1 / metallic=0（skin dummy MR 同款）
        lc.mrFactor[2] = params.options[0];
        cmd->writeBuffer(lightConstants_, &lc, sizeof(lc));
    }
    for (const ModelPrimBatch& b : modelBatches_) {
        LightConstants lc{};
        lc.lightDirW[0] = params.lightDir[0];   // 方向沿用 b0 g_lightDir 的口径与缺省值
        lc.lightDirW[1] = params.lightDir[1];
        lc.lightDirW[2] = params.lightDir[2];
        lc.lightColor[0] = 1.0f; lc.lightColor[1] = 1.0f; lc.lightColor[2] = 1.0f;
        lc.lightColor[3] = 2.5f;                // 白光强度 2.5：金属体无 IBL 只剩直射高光
        lc.cameraPos[0] = params.eye[0];
        lc.cameraPos[1] = params.eye[1];
        lc.cameraPos[2] = params.eye[2];
        // M7.2：因子按批取各自材质（建批时从 glTF 提取存进批次）；mrFactor.z =
        // --pbr-naive 对照开关。
        std::memcpy(lc.baseColorFactor, b.baseColorFactor, sizeof(lc.baseColorFactor));
        lc.mrFactor[0] = b.metallicFactor;
        lc.mrFactor[1] = b.roughnessFactor;
        lc.mrFactor[2] = params.options[0];
        cmd->writeBuffer(b.lightConstants, &lc, sizeof(lc));
    }

    // M0.8：pipeline 按 (fill × cull) 四套里选一套，shader 不变。
    nvrhi::GraphicsPipelineHandle pipeline = params.wireframe
        ? (params.cullBack ? pipelineWireBack_ : pipelineWireNone_)
        : (params.cullBack ? pipelineBack_ : pipelineNone_);

    // initCluster 场景（lod/dag/s5 层次遍历等）modelBatches_ 为空：走老单批
    // 路径（initCluster 自己的 b0-only 绑定集 + 全量索引）。M7.2 重写时曾把
    // 这条兜底丢掉 → lod/dag/s5 三模式空画面（回归实测 s2.0/s3.2/s5.0 HUD
    // 基线 6.4 万 px diff 才抓出来）。
    if (modelBatches_.empty()) {
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
             .setFramebuffer(sceneFb)   // T0 T2：场景渲到离屏 HDR（t0Active=false 回落 swapchain）
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
        // T0 T2：HDR 场景色 → swapchain 直通合成（单批兜底路径出口）。
        if (t0Active) {
            std::string tmErr;
            if (!presentTonemap(cmd, framebuffer, viewport, tmErr))
                nl::core::log::deviceError("t0", tmErr);
        }
        return;
    }

    // M7.2：每 prim 一批（共享 VB/IB，按 startIndex 区间画）；单 prim 资产 =
    // 1 批，与 M0.4 逐字节一致。本 NVRHI 无逐 draw setBindingSet，照 M-E1b
    // sceneBatch 先例每批重建 GraphicsState（含 addBindingSet）。
    for (const ModelPrimBatch& b : modelBatches_) {
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
             .setFramebuffer(sceneFb)   // T0 T2：场景渲到离屏 HDR（t0Active=false 回落 swapchain）
             .addBindingSet(b.set)
             .addVertexBuffer(nvrhi::VertexBufferBinding()
                 .setBuffer(vertexBuffer_).setSlot(0).setOffset(0))
             .setIndexBuffer(nvrhi::IndexBufferBinding()
                 .setBuffer(indexBuffer_).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
        state.viewport.addViewportAndScissorRect(viewport);
        cmd->setGraphicsState(state);
        nvrhi::DrawArguments args;
        args.setVertexCount(b.indexCount)   // drawIndexed 复用 vertexCount 表示索引数
            .setStartIndexLocation(b.startIndex);
        cmd->drawIndexed(args);
    }
    // T0 T2：HDR 场景色 → swapchain 直通合成（per-prim 批次路径出口）。
    if (t0Active) {
        std::string tmErr;
        if (!presentTonemap(cmd, framebuffer, viewport, tmErr))
            nl::core::log::deviceError("t0", tmErr);
    }
}

// ---- M-P3：物理调试线框叠加管线（LINE_LIST）----
// 固定容量动态 VB：ensure 时一次建足（kDebugLineCap 顶点），每帧 writeBuffer 重写
// 当前帧的线段顶点流，绝不中途重建——规避 frame-in-flight 期间重建 VB 的隐患。
namespace {
constexpr std::uint32_t kDebugLineCap = 262144u;  // 顶点数（= 131072 段线），调试够用
}

bool DebugLines::ensure(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                        const char* shaderSuffix, const char* vsEntry, const char* psEntry,
                        std::string& outError) {
    if (pipeline_) return true;

    // 加载 debug_line 双目标字节码（同一份 .slang 出 spv/dxil）。
    auto path = [shaderSuffix](const char* stage) {
        return std::string(NL_SHADER_DIR) + "/debug_line." + stage + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    if (!readFileBytes(path("vs").c_str(), vsBlob) ||
        !readFileBytes(path("ps").c_str(), psBlob)) {
        outError = std::string("failed to load debug_line shader (") + shaderSuffix + ")";
        return false;
    }
    nvrhi::ShaderDesc vsDesc;
    vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName(vsEntry);
    nvrhi::ShaderDesc psDesc;
    psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName(psEntry);
    vs_ = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
    ps_ = device->createShader(psDesc, psBlob.data(), psBlob.size());
    if (!vs_ || !ps_) { outError = "createShader failed for debug_line"; return false; }

    // volatile CB：mvp（float4x4 = 64 B）
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = 64;
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "DebugLineConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for debug line constants"; return false; }

    // 固定容量动态 VB（LINE_LIST 顶点流：6 float/顶点 = 24 B）
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = kDebugLineCap * 24u;
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "DebugLineVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    vbDesc.keepInitialState = true;   // 每帧命令列表开头回 CopyDest → 可反复 writeBuffer
    vertexBuffer_ = device->createBuffer(vbDesc);
    if (!vertexBuffer_) { outError = "createBuffer failed for debug line VB"; return false; }
    capacity_ = kDebugLineCap;

    // 绑定布局：只有 b0（volatile CB）
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    bindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!bindingLayout_) { outError = "createBindingLayout failed for debug lines"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_));
    bindingSet_ = device->createBindingSet(setDesc, bindingLayout_);
    if (!bindingSet_) { outError = "createBindingSet failed for debug lines"; return false; }

    // 输入布局：POSITION(float3) + COLOR(float3)，stride 24
    nvrhi::VertexAttributeDesc attrs[2] = {
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(0).setBufferIndex(0).setElementStride(24),
        nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(12).setBufferIndex(0).setElementStride(24),
    };
    inputLayout_ = device->createInputLayout(attrs, 2, vs_);
    if (!inputLayout_) { outError = "createInputLayout failed for debug lines"; return false; }

    nvrhi::GraphicsPipelineDesc desc;
    desc.setPrimType(nvrhi::PrimitiveType::LineList)
        .setVertexShader(vs_).setFragmentShader(ps_)
        .setInputLayout(inputLayout_).addBindingLayout(bindingLayout_);
    desc.renderState.rasterState
        .setFrontCounterClockwise(true)
        .setFillMode(nvrhi::RasterFillMode::Solid)
        .setCullMode(nvrhi::RasterCullMode::None);
    // 叠加层：depth test 开、depth write 关——被实体遮挡的线段不显示，但不写深度。
    desc.renderState.depthStencilState.enableDepthTest().disableDepthWrite();
    pipeline_ = device->createGraphicsPipeline(desc, info);
    if (!pipeline_) { outError = "createGraphicsPipeline failed for debug lines"; return false; }
    return true;
}

void DebugLines::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                      const nvrhi::Viewport& viewport, const FrameParams& params) {
    if (!pipeline_ || !params.debugLineData || params.debugLineVertexCount == 0) return;
    std::uint32_t n = params.debugLineVertexCount;
    if (n > capacity_) {
        // 超出固定容量（调试用途通常不会）：截断并告警一次，避免越界写。
        nl::core::log::deviceError("DebugLines",
            std::string("line vertex count ")
                + std::to_string(n) + " exceeds fixed capacity "
                + std::to_string(capacity_) + "; clamped");
        n = capacity_;
    }
    // 每帧重写 mvp（volatile CB）+ 线段顶点流（动态 VB，CopyDest+keepInitialState）。
    cmd->writeBuffer(constants_, params.mvp, 64);
    cmd->writeBuffer(vertexBuffer_, params.debugLineData, n * 24u);

    nvrhi::GraphicsState state;
    state.setPipeline(pipeline_)
         .setFramebuffer(framebuffer)
         .addBindingSet(bindingSet_)
         .addVertexBuffer(nvrhi::VertexBufferBinding()
             .setBuffer(vertexBuffer_).setSlot(0).setOffset(0));
    state.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(state);
    nvrhi::DrawArguments args;
    args.setVertexCount(n);   // LINE_LIST：顶点数即绘制顶点数
    cmd->draw(args);
}

void DebugLines::invalidate() {
    pipeline_     = nullptr;
    inputLayout_  = nullptr;
    bindingLayout_ = nullptr;
    bindingSet_   = nullptr;
    vs_ = nullptr;
    ps_ = nullptr;
    constants_    = nullptr;
    vertexBuffer_ = nullptr;
    capacity_     = 0;
}

void DebugLines::shutdown() {
    invalidate();
}

void Scene::drawDebugLines(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                           const nvrhi::Viewport& viewport, const FrameParams& params) {
    // M-P3：仅当显式开启且数据非空才绘制（缺省零开销，S7.0 基线不变）。
    if (!params.debugLines) return;
    dbgLines_.draw(cmd, framebuffer, viewport, params);
}

// ---- M-PT0：粒子叠加层（CPU 固定步长模拟 + instanced billboard）----

namespace {

// 与 M1.3 clusterColor 同族的 pcg 整数 hash——粒子参数从发射序列号推导，
// 无时间/时钟输入：run-to-run 与跨后端逐位一致（截图确定性前提）。
std::uint32_t particleHash(std::uint32_t v) {
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    v = (v >> 22u) ^ v;
    return v;
}
// [0,1) 均匀（24 位精度）。
float particleRand01(std::uint32_t v) {
    return static_cast<float>(v & 0xFFFFFFu) / 16777215.0f;
}

// 喷泉发射器常数：M-PT1 起参数面进 Particles::Params（scene.hpp），此处只留
// legacy 锚点使用的发射点与固定步长（nfx 路径经 params_ 覆写）。
constexpr float kParticleStepDt  = 1.0f / 60.0f;  // 固定逻辑步长（M-GL 物理同款纪律）
constexpr float kEmitterPos[3] = {0.0f, -0.55f, 0.0f};

// CB 布局（与 particles.slang 的 cbuffer b0 逐字节对应，128 B）。
struct ParticleConstantsCB {
    float mvp[16];
    float camRight[4];
    float camUp[4];
    float params[4];   // x = billboard 整体尺寸倍率；y = 贴图开关；z = Y/X 比；w = flipbook 开关
    float uvRect[4];   // M-PT3：flipbook UV 区域（xy=offset zw=cell；非 flipbook = 0,0,1,1）
};
static_assert(sizeof(ParticleConstantsCB) == 128, "particle cbuffer layout must match shader");

} // namespace

bool Particles::ensure(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                       const char* shaderSuffix, std::string& outError) {
    // M-PT2：贴图惰性建（loadNfx 存 basename 后首帧触发；失败回退软圆盘不炸帧）。
    // 纹理只在这里建（device 可用）；像素上传推迟到 draw 里（writeTexture 需要
    // cmd list，DX12/Vulkan 走 upload buffer 内部 copy）。
    if (textureDirty_) {
        textureDirty_ = false;
        std::string texErr;
        if (!ensureTexture(device, texErr)) {
            nl::core::log::deviceError("Particles", texErr + " (soft-disc fallback)");
            texture_ = nullptr;
        }
    }
    if (pipeline_) return true;

    // 加载 particles 双目标字节码（同一份 .slang 出 spv/dxil）。
    auto path = [shaderSuffix](const char* stage) {
        return std::string(NL_SHADER_DIR) + "/particles." + stage + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    if (!readFileBytes(path("vs").c_str(), vsBlob) ||
        !readFileBytes(path("ps").c_str(), psBlob)) {
        outError = std::string("failed to load particles shader (") + shaderSuffix + ")";
        return false;
    }
    // SPIR-V 恒 main、DXIL 入口由 -entry 定（NVRHI 忽略 DX12 entryName）——
    // 两个后端统一传 "main"（debug_line 同款，§2.4）。
    nvrhi::ShaderDesc vsDesc;
    vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName("main");
    nvrhi::ShaderDesc psDesc;
    psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName("main");
    vs_ = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
    ps_ = device->createShader(psDesc, psBlob.data(), psBlob.size());
    if (!vs_ || !ps_) { outError = "createShader failed for particles"; return false; }

    // volatile CB：128 B（mvp + 相机基向量 + 预留），每帧全量覆写（§2.7 纪律）。
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(ParticleConstantsCB);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "ParticleConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for particle constants"; return false; }

    // 实例数据 SSBO：每实例 48 B（float4×3，M-PT3 扩 progressMisc 段；已有成员
    // 偏移不变），structStride 必须与 shader 视角一致。每帧 CopyDest 起步
    // （writeBuffer）→ 帧内 SRV 动态晋升（visibleList 同款状态纪律，§2.15）。
    nvrhi::BufferDesc instDesc;
    instDesc.byteSize = kMaxParticles * sizeof(ParticleGpu);
    instDesc.structStride = sizeof(ParticleGpu);
    instDesc.initialState = nvrhi::ResourceStates::CopyDest;
    instDesc.keepInitialState = true;
    instDesc.debugName = "ParticleInstances";
    instanceData_ = device->createBuffer(instDesc);
    if (!instanceData_) { outError = "createBuffer failed for particle instances"; return false; }

    // 绑定布局：b0 volatile CB + t0 StructuredBuffer + t1 贴图 + s0 采样器
    // （M-PT2；§2.7：单管线单一 set）。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0));
    bindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!bindingLayout_) { outError = "createBindingLayout failed for particles"; return false; }

    // M-PT2：1x1 白 dummy（无贴图占位，引擎空纹理槽先例，过 NVRHI 校验）+ Linear
    // clamp 采样器。像素上传走 draw 里的 writeTexture（dummyPendingUpload_ 门控）。
    if (!dummyTex_) {
        nvrhi::TextureDesc d;
        d.setWidth(1).setHeight(1);
        d.setFormat(nvrhi::Format::RGBA8_UNORM);
        d.setInitialState(nvrhi::ResourceStates::CopyDest);
        d.keepInitialState = true;
        d.debugName = "ParticleTexDummy";
        dummyTex_ = device->createTexture(d);
        if (!dummyTex_) { outError = "createTexture failed for particle dummy"; return false; }
        dummyPendingUpload_ = true;
    }
    if (!sampler_) {
        nvrhi::SamplerDesc sDesc;
        sDesc.setAllFilters(true)   // bool 语义：true = Linear（nvrhi.h 惯例）
            .setMipFilter(true)     // M-PT3：mip 线性（纹理带全 mip 链；dummy 1x1 无 mip 不受影响）
            .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        sampler_ = device->createSampler(sDesc);
        if (!sampler_) { outError = "createSampler failed for particles"; return false; }
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instanceData_));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, texture_ ? texture_ : dummyTex_));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, sampler_));
    bindingSet_ = device->createBindingSet(setDesc, bindingLayout_);
    if (!bindingSet_) { outError = "createBindingSet failed for particles"; return false; }

    // 管线：TriangleList + 无 input layout（VS 用 SV_VertexID/SV_InstanceID 顶点
    // 拉取，cull 路径同款）。alpha blend + depth test 开 / write 关 + cull None
    // （billboard 两面可见）。不写深度 → 不遮挡后续 HUD。
    // M-PT1：additive 变体管线懒建（同一份 shader/绑定布局，仅混合方程不同；
    // §2.7 单管线单一 BindingSet——两条管线共享同一 bindingLayout_/bindingSet_）。
    auto makePipeline = [&](nvrhi::BlendFactor srcColor, nvrhi::BlendFactor dstColor) {
        nvrhi::GraphicsPipelineDesc desc;
        desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setVertexShader(vs_).setFragmentShader(ps_)
            .addBindingLayout(bindingLayout_);
        desc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::None);
        desc.renderState.depthStencilState.enableDepthTest().disableDepthWrite();
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(true);
        rt.setSrcBlend(srcColor);
        rt.setDestBlend(dstColor);
        rt.setSrcBlendAlpha(nvrhi::BlendFactor::One);
        rt.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
        desc.renderState.blendState.setRenderTarget(0, rt);
        return device->createGraphicsPipeline(desc, info);
    };
    pipeline_ = makePipeline(nvrhi::BlendFactor::SrcAlpha, nvrhi::BlendFactor::InvSrcAlpha);
    if (!pipeline_) { outError = "createGraphicsPipeline failed for particles"; return false; }
    return true;
}

// M-PT2：nfx 贴图惰性建。读 <repo>/assets/efx/textures/<basename> → stbi 解码
// （强制 4 通道 RGBA8）→ 建空 RGBA8_UNORM 纹理 + 像素暂存（draw 里 writeTexture
// 上传——建纹时无 cmd list 可用）。颜色口径：原样采样不做 sRGB 转换，与引擎粒子
// 颜色线性直接算的既有口径一致（M-PT0 起全链无 gamma）。
// 注意 NL_ASSET_DIR 注入的是 assets/models（M0.7 model 白名单口径），粒子资产
// 在 assets/efx——取其父目录拼 efx/textures。
bool Particles::ensureTexture(nvrhi::IDevice* device, std::string& outError) {
    const std::string path = std::string(NL_ASSET_DIR "/../efx/textures/") + textureBasename_;
    std::vector<std::uint8_t> buf;
    if (!readFileBytes(path.c_str(), buf)) {
        outError = "nfx texture not found: " + path;
        return false;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()),
                                            &w, &h, &comp, 4);
    if (!pixels || w <= 0 || h <= 0) {
        outError = "nfx texture decode failed: " + textureBasename_;
        return false;
    }
    const std::uint32_t uW = static_cast<std::uint32_t>(w);
    const std::uint32_t uH = static_cast<std::uint32_t>(h);
    texturePixels_.assign(reinterpret_cast<const std::uint8_t*>(pixels),
                          reinterpret_cast<const std::uint8_t*>(pixels) + uW * uH * 4u);
    stbi_image_free(pixels);
    texW_ = uW;
    texH_ = uH;

    // M-PT3：CPU box-filter mip 链（逐层 2×2 均值下采样，纯整数/加法运算
    // = 跨次运行逐位确定；奇数尺寸层取 floor，右/下边缘用 clamp 取样）。
    // 全链存进 texturePixels_ 平铺（层 0 = W×H，层 n = (W>>n)×(H>>n)，
    // 每层行距 = 层宽×4 无 padding），上传时按层 writeTexture。
    {
        const std::uint32_t maxDim = (uW > uH) ? uW : uH;
        std::uint32_t levels = 1;
        while ((maxDim >> levels) > 0) ++levels;   // log2(maxDim)+1（1x1 封底）
        texMipCount_ = levels;
        std::vector<std::uint8_t> chain = texturePixels_;  // 层 0
        std::uint32_t pw = uW, ph = uH;
        for (std::uint32_t l = 1; l < levels; ++l) {
            const std::uint32_t nw = (pw > 1) ? (pw >> 1) : 1;
            const std::uint32_t nh = (ph > 1) ? (ph >> 1) : 1;
            std::vector<std::uint8_t> nl(size_t(nw) * nh * 4u);
            for (std::uint32_t y = 0; y < nh; ++y) {
                for (std::uint32_t x = 0; x < nw; ++x) {
                    // 2×2 均值；pw/ph 为 1 时采样越界 → clamp 到 0（同像素）
                    const std::uint32_t x1 = (x * 2u + 1u < pw) ? x * 2u + 1u : x * 2u;
                    const std::uint32_t y1 = (y * 2u + 1u < ph) ? y * 2u + 1u : y * 2u;
                    const std::uint8_t* s00 = &chain[(size_t(y * 2u) * pw + x * 2u) * 4u];
                    const std::uint8_t* s01 = &chain[(size_t(y * 2u) * pw + x1) * 4u];
                    const std::uint8_t* s10 = &chain[(size_t(y1) * pw + x * 2u) * 4u];
                    const std::uint8_t* s11 = &chain[(size_t(y1) * pw + x1) * 4u];
                    std::uint8_t* dst = &nl[(size_t(y) * nw + x) * 4u];
                    for (int c = 0; c < 4; ++c)
                        dst[c] = std::uint8_t((s00[c] + s01[c] + s10[c] + s11[c]) >> 2);
                }
            }
            chain.swap(nl);
            texturePixels_.insert(texturePixels_.end(), chain.begin(), chain.end());
            pw = nw; ph = nh;
        }
    }

    nvrhi::TextureDesc d;
    d.setWidth(uW).setHeight(uH);
    d.setFormat(nvrhi::Format::RGBA8_UNORM);
    d.setMipLevels(texMipCount_);   // M-PT3：全 mip 链（远处粒子抗闪烁）
    d.setInitialState(nvrhi::ResourceStates::CopyDest);   // writeTexture 后转 ShaderResource
    d.keepInitialState = true;
    d.debugName = "ParticleTexture";
    texture_ = device->createTexture(d);
    if (!texture_) {
        texturePixels_.clear();
        texMipCount_ = 1;
        outError = "createTexture failed for " + textureBasename_;
        return false;
    }
    texturePendingUpload_ = true;
    nl::core::log::info("[Particles] nfx texture: ", textureBasename_,
                        " (", w, "x", h, ")");
    return true;
}

void Particles::step(float dt) {
    // 1. 积分 + 摘除死亡（swap-remove；换上来的元素 continue 重进本帧积分，
    //    数组序 = spawn 序保持稳定，确定性不受摘除方式影响）。
    //    M-PT1：加速度走 params_.accel（nfx 折算的常加速度；legacy = -y 重力）。
    for (std::size_t i = 0; i < pool_.size(); ) {
        Sim& p = pool_[i];
        p.life -= dt;
        if (p.life <= 0.0f) {
            p = pool_.back();
            pool_.pop_back();
            continue;
        }
        p.vel[0] += params_.accel[0] * dt;
        p.vel[1] += params_.accel[1] * dt;
        p.vel[2] += params_.accel[2] * dt;
        p.pos[0] += p.vel[0] * dt;
        p.pos[1] += p.vel[1] * dt;
        p.pos[2] += p.vel[2] * dt;
        ++i;
    }
    // 2. 发射：发射率积分 + floor 取整（固定 dt 下每步发射数确定）。
    spawnAccum_ += params_.rate * dt;
    std::uint32_t spawn = static_cast<std::uint32_t>(spawnAccum_);
    spawnAccum_ -= static_cast<float>(spawn);
    for (std::uint32_t k = 0; k < spawn && pool_.size() < kMaxParticles; ++k) {
        const std::uint32_t s = spawnSeq_++;
        Sim p;
        if (!nfxActive_) {
            // ---- M-PT0 legacy 喷泉（逐位不动：M-PT0 基线字节复现的前提）----
            const float rx = particleRand01(particleHash(s * 5u + 0u)) * 2.0f - 1.0f;
            const float rz = particleRand01(particleHash(s * 5u + 1u)) * 2.0f - 1.0f;
            const float rSpd = particleRand01(particleHash(s * 5u + 2u));
            const float rLife = particleRand01(particleHash(s * 5u + 3u));
            const float rSize = particleRand01(particleHash(s * 5u + 4u));
            p.pos[0] = kEmitterPos[0]; p.pos[1] = kEmitterPos[1]; p.pos[2] = kEmitterPos[2];
            // 锥形初速：向上为主 + 水平散布，归一化后乘幅值。
            const float dx = rx * params_.spread, dy = 1.0f, dz = rz * params_.spread;
            const float invLen = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
            const float speed = params_.speedMin + rSpd * params_.speedSpan;
            p.vel[0] = dx * invLen * speed;
            p.vel[1] = dy * invLen * speed;
            p.vel[2] = dz * invLen * speed;
            p.maxLife = params_.lifeMin + rLife * params_.lifeSpan;
            p.life = p.maxLife;
            p.size = params_.sizeMin + rSize * params_.sizeSpan;
        } else {
            // ---- M-PT1 .nfx：box 初速 min/max 逐轴 lerp + 定长寿命/尺寸 ----
            const float rv0 = particleRand01(particleHash(s * 5u + 0u));
            const float rv1 = particleRand01(particleHash(s * 5u + 1u));
            const float rv2 = particleRand01(particleHash(s * 5u + 2u));
            const float rLife = particleRand01(particleHash(s * 5u + 3u));
            p.pos[0] = params_.spawn[0];
            p.pos[1] = params_.spawn[1];
            p.pos[2] = params_.spawn[2];
            p.vel[0] = params_.velMin[0] + rv0 * (params_.velMax[0] - params_.velMin[0]);
            p.vel[1] = params_.velMin[1] + rv1 * (params_.velMax[1] - params_.velMin[1]);
            p.vel[2] = params_.velMin[2] + rv2 * (params_.velMax[2] - params_.velMin[2]);
            p.maxLife = params_.lifeMin + rLife * params_.lifeSpan;
            p.life = p.maxLife;
            p.size = params_.sizeMin;
        }
        pool_.push_back(p);
    }
    // 3. 填 GPU 绘制流：颜色/alpha 生命周期曲线在 CPU 端算好（shader 只画）。
    gpuData_.clear();
    gpuData_.reserve(pool_.size());
    for (const Sim& p : pool_) {
        const float t = 1.0f - p.life / p.maxLife;   // 0 = 新生，1 = 死亡
        ParticleGpu g;
        g.posSize[0] = p.pos[0];
        g.posSize[1] = p.pos[1];
        g.posSize[2] = p.pos[2];
        g.posSize[3] = p.size * (0.6f + 0.4f * (1.0f - t));   // 随寿命略收缩
        if (!nfxActive_) {
            // legacy：黄 → 橙红；alpha：出生快速淡入 × 死亡淡出（乘积曲线，无开关跳变）。
            g.colorAlpha[0] = 1.0f;
            g.colorAlpha[1] = 0.85f - 0.55f * t;
            g.colorAlpha[2] = 0.35f - 0.27f * t;
            const float aIn  = std::min(1.0f, t * 8.0f);
            const float aOut = std::min(1.0f, (1.0f - t) * 3.0f);
            g.colorAlpha[3] = aIn * aOut;
        } else {
            // nfx：colorStart→colorEnd 线性 lerp；fadeIn/fadeOut（秒）乘积曲线。
            g.colorAlpha[0] = params_.colorStart[0] + (params_.colorEnd[0] - params_.colorStart[0]) * t;
            g.colorAlpha[1] = params_.colorStart[1] + (params_.colorEnd[1] - params_.colorStart[1]) * t;
            g.colorAlpha[2] = params_.colorStart[2] + (params_.colorEnd[2] - params_.colorStart[2]) * t;
            float aIn = 1.0f, aOut = 1.0f;
            if (params_.fadeIn > 0.0f)
                aIn = std::min(1.0f, (1.0f - p.life / p.maxLife) * p.maxLife / params_.fadeIn);
            if (params_.fadeOut > 0.0f)
                aOut = std::min(1.0f, p.life / params_.fadeOut);
            g.colorAlpha[3] = params_.colorStart[3] * aIn * aOut;
        }
        // M-PT3：flipbook 帧号 CPU 折算（Effekseer GetUV 同式，纯整数运算确定性
        // 无碍）。elapsed 恢复出生起算秒数后 ×60 = LivingTime（Efke 帧单位），
        // +0.5f 取整消除逐帧浮点减法累积误差的截断抖动。loop 折算三式与
        // Effekseer UVFunctions::GetUV 逐式对应。非 flipbook 恒 (0,0)（配合
        // CB uvRect=(0,0,1,1)，VS 统一公式恒等于 M-PT2 texUv）。
        if (params_.flipbook.enabled) {
            const float elapsed = p.maxLife - p.life;   // 出生起算秒数
            const int livingFrames = int(elapsed * 60.0f + 0.5f);
            int frameNum = int(float(livingFrames) / params_.flipbook.frameLength);
            const int fc = params_.flipbook.countX * params_.flipbook.countY;
            if (params_.flipbook.loopType == 0) {          // once：末帧驻留
                if (frameNum >= fc) frameNum = fc - 1;
            } else if (params_.flipbook.loopType == 1) {   // loop
                frameNum %= fc;
            } else {                                       // reverse（来回循环）
                const bool rev = (frameNum / fc) % 2 == 1;
                frameNum %= fc;
                if (rev) frameNum = fc - 1 - frameNum;
            }
            g.progressMisc[0] = float(frameNum % params_.flipbook.countX);
            g.progressMisc[1] = float(frameNum / params_.flipbook.countX);
        } else {
            g.progressMisc[0] = 0.0f; g.progressMisc[1] = 0.0f;
        }
        g.progressMisc[2] = 0.0f; g.progressMisc[3] = 0.0f;
        gpuData_.push_back(g);
    }
    aliveCount_ = static_cast<std::uint32_t>(gpuData_.size());
}

void Particles::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                     const nvrhi::Viewport& viewport, const FrameParams& params,
                     const char* shaderSuffix) {
    // 懒建（随视口格式一次建好；失败不炸整帧，本帧跳过下帧重试——hizFailed_ 同款口径）。
    if (!pipeline_) {
        std::string err;
        if (!ensure(cmd->getDevice(), framebuffer->getFramebufferInfo(), shaderSuffix, err)) {
            nl::core::log::deviceError("Particles", err + " (skipping this frame)");
            return;
        }
    }
    // M-PT1：additive 管线懒建（仅 nfx blend=additive 时需要；同 shader/绑定布局）。
    nvrhi::GraphicsPipelineHandle activePipeline = pipeline_;
    if (nfxActive_ && params_.additive && !pipelineAdditive_) {
        nvrhi::GraphicsPipelineDesc desc;
        desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setVertexShader(vs_).setFragmentShader(ps_)
            .addBindingLayout(bindingLayout_);
        desc.renderState.rasterState
            .setFrontCounterClockwise(true)
            .setFillMode(nvrhi::RasterFillMode::Solid)
            .setCullMode(nvrhi::RasterCullMode::None);
        desc.renderState.depthStencilState.enableDepthTest().disableDepthWrite();
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(true);
        rt.setSrcBlend(nvrhi::BlendFactor::One);          // additive：src*1 + dst*1
        rt.setDestBlend(nvrhi::BlendFactor::One);
        rt.setSrcBlendAlpha(nvrhi::BlendFactor::One);
        rt.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
        desc.renderState.blendState.setRenderTarget(0, rt);
        pipelineAdditive_ = cmd->getDevice()->createGraphicsPipeline(
            desc, framebuffer->getFramebufferInfo());
        if (!pipelineAdditive_) {
            nl::core::log::deviceError("Particles", "createGraphicsPipeline failed for additive (fall back to alpha)");
        }
    }
    if (nfxActive_ && params_.additive && pipelineAdditive_)
        activePipeline = pipelineAdditive_;
    // 每帧固定步长一步：粒子状态 = 已渲染帧数的纯函数（截图确定性前提）。
    step(kParticleStepDt);
    if (aliveCount_ == 0) return;

    // M-PT2：贴图像素上传（建纹时无 cmd list，推迟到首帧这里；writeTexture +
    // setTextureState 同列表——0x87d 第一坑同源纪律：上传与状态转换不跨列表）。
    if (texturePendingUpload_ || dummyPendingUpload_) {
        static const std::uint8_t kWhite[4] = {255, 255, 255, 255};
        if (texturePendingUpload_) {
            // M-PT3：全 mip 层逐层上传（texturePixels_ 平铺布局见 ensureTexture；
            // writeTexture 第 3 参 = mip 层号，NVRHI staging 内部按 subresource 路由）。
            {
                const std::uint8_t* base = texturePixels_.data();
                std::uint32_t pw = texW_, ph = texH_;
                std::size_t off = 0;
                for (std::uint32_t l = 0; l < texMipCount_; ++l) {
                    cmd->writeTexture(texture_, 0, l, base + off, pw * 4u);
                    off += std::size_t(pw) * ph * 4u;
                    pw = (pw > 1) ? (pw >> 1) : 1;
                    ph = (ph > 1) ? (ph >> 1) : 1;
                }
            }
            cmd->setTextureState(texture_, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
            texturePixels_.clear();
            texturePixels_.shrink_to_fit();
            texturePendingUpload_ = false;
        }
        if (dummyPendingUpload_) {
            cmd->writeTexture(dummyTex_, 0, 0, kWhite, 4u);
            cmd->setTextureState(dummyTex_, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
            dummyPendingUpload_ = false;
        }
    }

    // 每帧重写 CB（volatile 全量覆写）：mvp + 相机基向量。
    // 列主序 view 矩阵的 right/up = 第 0/1 行的 xyz（m[col*4+row]）。
    ParticleConstantsCB cb;
    std::memcpy(cb.mvp, params.mvp, sizeof(cb.mvp));
    cb.camRight[0] = params.view[0]; cb.camRight[1] = params.view[4];
    cb.camRight[2] = params.view[8]; cb.camRight[3] = 0.0f;
    cb.camUp[0] = params.view[1]; cb.camUp[1] = params.view[5];
    cb.camUp[2] = params.view[9]; cb.camUp[3] = 0.0f;
    cb.params[0] = 1.0f;
    cb.params[1] = texture_ ? 1.0f : 0.0f;  // M-PT2：PS 贴图开关（0 = 软圆盘路径）
    cb.params[2] = params_.ratioY;          // M-PT2：Y/X 尺寸比（方形恒 1.0 保形）
    cb.params[3] = params_.flipbook.enabled ? 1.0f : 0.0f; // M-PT3：VS flipbook UV 开关
    // M-PT3：flipbook UV 区域。非 flipbook 恒 (0,0,1,1)——VS 统一公式在此取值下
    // 精确恒等于 M-PT2 的 texUv（×1.0/+0.0 IEEE 恒等，pt0/pt1/pt2 软圆盘+恒等
    // 贴图路径零漂移）；flipbook 时 = .nfx offset/cell。
    if (params_.flipbook.enabled) {
        cb.uvRect[0] = params_.flipbook.offsetU;
        cb.uvRect[1] = params_.flipbook.offsetV;
        cb.uvRect[2] = params_.flipbook.cellW;
        cb.uvRect[3] = params_.flipbook.cellH;
    } else {
        cb.uvRect[0] = 0.0f; cb.uvRect[1] = 0.0f;
        cb.uvRect[2] = 1.0f; cb.uvRect[3] = 1.0f;
    }
    cmd->writeBuffer(constants_, &cb, sizeof(cb));
    cmd->writeBuffer(instanceData_, gpuData_.data(),
                     aliveCount_ * static_cast<std::uint32_t>(sizeof(ParticleGpu)));

    nvrhi::GraphicsState state;
    state.setPipeline(activePipeline)
         .setFramebuffer(framebuffer)
         .addBindingSet(bindingSet_);
    state.viewport.addViewportAndScissorRect(viewport);
    cmd->setGraphicsState(state);
    nvrhi::DrawArguments args;
    args.setVertexCount(6);          // 每实例两枚三角形（VS 按 SV_VertexID 展开）
    args.setInstanceCount(aliveCount_);
    cmd->draw(args);
}

// M-PT1：.nfx v1 加载端。JSON → 模拟参数折算；运行时零 Effekseer 依赖
// （转换期已把 Effekseer 参数面折进 NFX1 子集，子集外根本到不了这里）。
// 成功后清空模拟状态重启时间线（spawnSeq 归零 = 确定性流重新出发）。
bool Particles::loadNfx(const char* path, std::string& outError) {
    std::vector<std::uint8_t> buf;
    if (!readFileBytes(path, buf)) {
        outError = std::string("cannot open .nfx: ") + path;
        return false;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(buf.data(), buf.data() + buf.size());
    } catch (const std::exception& e) {
        outError = std::string(".nfx json parse failed: ") + e.what();
        return false;
    }
    if (!j.is_object() || j.value("magic", "") != "NFX1") {
        outError = ".nfx bad magic (want NFX1)";
        return false;
    }
    if (!j.contains("emitter") || !j.contains("render")) {
        outError = ".nfx missing emitter/render section";
        return false;
    }
    const nlohmann::json& e = j["emitter"];
    const nlohmann::json& r = j["render"];
    auto getArr2 = [&](const nlohmann::json& o, const char* k, float out[2]) {
        if (!o.contains(k) || !o[k].is_array() || o[k].size() != 2) return false;
        out[0] = o[k][0].get<float>();
        out[1] = o[k][1].get<float>();
        return true;
    };
    auto getArr3 = [&](const nlohmann::json& o, const char* k, float out[3]) {
        if (!o.contains(k) || !o[k].is_array() || o[k].size() != 3) return false;
        for (int a = 0; a < 3; ++a) out[a] = o[k][a].get<float>();
        return true;
    };
    auto getArr4 = [&](const nlohmann::json& o, const char* k, float out[4]) {
        if (!o.contains(k) || !o[k].is_array() || o[k].size() != 4) return false;
        for (int c = 0; c < 4; ++c) out[c] = o[k][c].get<float>();
        return true;
    };

    float rate[2], life[2], spawn[3], vMin[3], vMax[3], accel[3];
    float cStart[4], cEnd[4], size[2];
    if (!getArr2(e, "rate", rate) || !getArr2(e, "life", life) ||
        !getArr3(e, "spawnPoint", spawn) || !getArr3(e, "velocityMin", vMin) ||
        !getArr3(e, "velocityMax", vMax) || !getArr3(e, "accel", accel) ||
        !getArr4(r, "colorStart", cStart) || !getArr4(r, "colorEnd", cEnd) ||
        !r.contains("size") || !r["size"].is_array() || r["size"].size() != 2) {
        outError = ".nfx malformed arrays";
        return false;
    }
    size[0] = r["size"][0].get<float>();
    size[1] = r["size"][1].get<float>();

    Params p;             // 从 NFX1 缺省起折算（不复带 legacy 字段）
    p.rate = 0.5f * (rate[0] + rate[1]);            // min/max 取均值（区间留档在 .nfx）
    p.lifeMin = life[0];
    p.lifeSpan = life[1] > life[0] ? (life[1] - life[0]) : 0.0f;
    p.spawn[0] = spawn[0]; p.spawn[1] = spawn[1]; p.spawn[2] = spawn[2];
    p.velMin[0] = vMin[0]; p.velMin[1] = vMin[1]; p.velMin[2] = vMin[2];
    p.velMax[0] = vMax[0]; p.velMax[1] = vMax[1]; p.velMax[2] = vMax[2];
    p.accel[0] = accel[0]; p.accel[1] = accel[1]; p.accel[2] = accel[2];
    p.sizeMin = size[0];
    p.ratioY = (size[0] > 0.0f && size[1] != size[0])
                   ? (size[1] / size[0]) : 1.0f;    // M-PT2：Y/X 比（方形精确 1.0 保形）
    p.sizeSpan = 0.0f;                               // v1 定长尺寸
    for (int c = 0; c < 4; ++c) {
        p.colorStart[c] = cStart[c] / 255.0f;        // .nfx 色值 0..255 → 线性 0..1
        p.colorEnd[c]   = cEnd[c] / 255.0f;
    }
    p.fadeIn = r.value("fadeIn", 0.0f);
    p.fadeOut = r.value("fadeOut", 0.0f);
    std::string blend = r.value("blend", "alpha");
    p.additive = (blend == "additive");

    // M-PT3：flipbook 可选字段（缺省/缺字段 = 无）。字段校验从严——畸形即拒
    // （exit 路径返回 false，加载端保持 legacy 参数不变）。
    if (r.contains("flipbook")) {
        const nlohmann::json& fb = r["flipbook"];
        if (!fb.is_object() || !fb.contains("frameLength") || !fb.contains("countX") ||
            !fb.contains("countY") || !fb.contains("loop") || !fb.contains("offset") ||
            !fb.contains("cell") || !fb["offset"].is_array() || fb["offset"].size() != 2 ||
            !fb["cell"].is_array() || fb["cell"].size() != 2) {
            outError = ".nfx malformed flipbook object";
            return false;
        }
        p.flipbook.enabled = true;
        p.flipbook.frameLength = fb["frameLength"].get<float>();
        p.flipbook.countX = fb["countX"].get<int>();
        p.flipbook.countY = fb["countY"].get<int>();
        p.flipbook.loopType = fb["loop"].get<int>();
        p.flipbook.offsetU = fb["offset"][0].get<float>();
        p.flipbook.offsetV = fb["offset"][1].get<float>();
        p.flipbook.cellW = fb["cell"][0].get<float>();
        p.flipbook.cellH = fb["cell"][1].get<float>();
        if (p.flipbook.frameLength <= 0.0f || p.flipbook.countX <= 0 || p.flipbook.countY <= 0 ||
            p.flipbook.loopType < 0 || p.flipbook.loopType > 2 ||
            p.flipbook.cellW <= 0.0f || p.flipbook.cellH <= 0.0f) {
            outError = ".nfx flipbook params out of range";
            return false;
        }
    }

    // M-PT2：贴图 basename（.efkefc 内部相对路径只取尾段；运行时解析为
    // assets/efx/textures/<basename>，首帧 ensureTexture 找不到/解码失败回退软圆盘）。
    // 注意 texture 字段可能为 JSON null（M-PT1 转换器对无贴图特效的产物）——
    // value() 遇 null 抛 type_error 而非返回默认值，必须显式判 null。
    const std::string tex = (r.contains("texture") && r["texture"].is_string())
                                ? r["texture"].get<std::string>() : "";
    textureBasename_.clear();
    textureDirty_ = false;
    if (!tex.empty() && tex != "null") {
        const std::size_t tp = tex.find_last_of("/\\");
        textureBasename_ = (tp == std::string::npos) ? tex : tex.substr(tp + 1);
        textureDirty_ = true;
    }

    params_ = p;
    nfxActive_ = true;
    nfxName_ = path;
    const std::size_t slash = nfxName_.find_last_of("/\\");
    if (slash != std::string::npos) nfxName_ = nfxName_.substr(slash + 1);

    // 模拟状态归零重启：粒子时间线从加载帧出发（spawnSeq=0 → pcg 流确定性重启）。
    pool_.clear();
    gpuData_.clear();
    aliveCount_ = 0;
    spawnAccum_ = 0.0f;
    spawnSeq_ = 0;
    return true;
}

void Particles::invalidate() {
    pipeline_      = nullptr;
    pipelineAdditive_ = nullptr;
    bindingLayout_ = nullptr;
    bindingSet_    = nullptr;
    vs_ = nullptr;
    ps_ = nullptr;
    constants_     = nullptr;
    instanceData_  = nullptr;
    // M-PT2：texture_/dummyTex_/sampler_ 是 device 级资源，随 resize 重建管线时
    // 保留（bindingSet 重建会重新引用现句柄）；textureDirty_/pending 状态不动。
    // CPU 模拟状态（pool_/gpuData_/spawnAccum_/spawnSeq_）不清：管线随 resize
    // 重建是显示层的事，模拟时间线不能断（断续帧会破坏确定性叙事，也丢画面连续性）。
}

void Particles::shutdown() {
    invalidate();
    texture_ = nullptr;
    dummyTex_ = nullptr;
    sampler_ = nullptr;
    textureDirty_ = false;
    texturePendingUpload_ = false;
    dummyPendingUpload_ = false;
    textureBasename_.clear();
    texturePixels_.clear();
    texW_ = 0; texH_ = 0;
    pool_.clear();
    gpuData_.clear();
    aliveCount_ = 0;
    spawnAccum_ = 0.0f;
    spawnSeq_   = 0;
}

void Scene::drawParticles(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                          const nvrhi::Viewport& viewport, const FrameParams& params) {
    // M-PT0：仅当显式开启才步进 + 绘制（缺省零开销，既有模式基线一像素不动）。
    if (!params.particles) return;
    particles_.draw(cmd, framebuffer, viewport, params, shaderSuffix_.c_str());
}

void Scene::shutdown() {
    // M8.1：TLAS 资源（AS 句柄 + 探针管线，device 存活期释放）。
    tlasReady_ = false;
    tlasInstanceCount_ = 0;
    tlasBuildMs_ = 0.0f;
    tlasFrameIndex_ = 0;
    tlasTimer_.shutdown();
    tlas_ = nullptr;
    probeReady_ = false;
    probeFailed_ = false;
    probeSubmitted_ = 0;
    probePolledSlot_ = -1;
    csTlasProbe_ = nullptr;
    probeLayout_ = nullptr;
    probeSet_ = nullptr;
    probePSO_ = nullptr;
    probeOut_ = nullptr;
    for (auto& rb : probeReadback_) rb = nullptr;
    // M8.2：rt 调试射线资源（compute/呈现管线 + UAV 目标，device 存活期释放）。
    rt_ = false;
    rtNoTlasLogged_ = false;
    rtDevice_ = nullptr;
    t0Device_ = nullptr;
    rtW_ = 0;
    rtH_ = 0;
    csRt_ = nullptr;
    vsRtPresent_ = nullptr;
    psRtPresent_ = nullptr;
    rtPSO_ = nullptr;
    rtPresentPSO_ = nullptr;
    rtComputeLayout_ = nullptr;
    rtComputeSet_ = nullptr;
    rtPresentLayout_ = nullptr;
    rtPresentSet_ = nullptr;
    rtConstants_ = nullptr;
    rtTarget_ = nullptr;
    // T0：离屏 HDR / velocity / history ping-pong（raster 主路径懒建，device 存活期
    // 释放——不在此置空会拖到 Scene 析构（晚于 vkDestroyDevice）→ use-after-free
    // → 退出段错误（139），与上方 NVRHI device 包装的关停次序陷阱同机理）。
    hdrSceneColor_ = nullptr;
    hdrColorFb_ = nullptr;
    velocity_ = nullptr;
    history0_ = nullptr;
    history1_ = nullptr;
    t0Depth_ = nullptr;
    hdrSceneFb_ = nullptr;
    t0W_ = 0;
    t0H_ = 0;
    t0Parity_ = 0;
    psBlitToHdr_ = nullptr;
    blitPSO_ = nullptr;
    blitLayout_ = nullptr;
    blitSet_ = nullptr;
    psTonemap_ = nullptr;
    tonemapPSO_ = nullptr;
    tonemapLayout_ = nullptr;
    tonemapSet_ = nullptr;
    // M9.0：shadow 链句柄（rtTarget_ / present 共用对象已在上面统一释放）。
    shadow_ = false;
    shadowFailed_ = false;
    shadowNoTlasLogged_ = false;
    csDirect_ = nullptr;
    directPSO_ = nullptr;
    directComputeLayout_ = nullptr;
    directComputeSet_ = nullptr;
    directConstants_ = nullptr;
    directW_ = 0;
    directH_ = 0;
    // M9.2：reference 链句柄（rtTarget_ / present 共用对象已在上面统一释放）。
    reference_ = false;
    refFailed_ = false;
    refNoTlasLogged_ = false;
    samplesDone_ = 0;
    refFramesTarget_ = 0;
    refProgress_ = 0.0f;
    refW_ = 0;
    refH_ = 0;
    csReference_ = nullptr;
    psRefPresent_ = nullptr;
    refPSO_ = nullptr;
    refPresentPSO_ = nullptr;
    refComputeLayout_ = nullptr;
    refComputeSet_ = nullptr;
    refPresentLayout_ = nullptr;
    refPresentSet_ = nullptr;
    refConstants_ = nullptr;
    // M10.0：probe-place 资源（velocity/probe/dots 三管线 + 自建深度 + atlas
    // ping-pong，device 存活期释放——T0 教训：漏登记 = 析构晚于 vkDestroyDevice UAF）。
    probe_ = false;
    probeFailed_ = false;
    probeDevice_ = nullptr;
    probeFirstFrame_ = true;
    probeParity_ = 0;
    probeW_ = 0;
    probeH_ = 0;
    probeBaseRes_ = 0;
    probeLogged_ = false;
    probeTraceLogged_ = false;
    vsVelocity_ = nullptr;
    psVelocity_ = nullptr;
    csPlace_ = nullptr;
    csTrace_ = nullptr;
    vsProbeDots_ = nullptr;
    psProbeDots_ = nullptr;
    velocityCB_ = nullptr;
    probeCB_ = nullptr;
    traceCB_ = nullptr;
    velocityLayout_ = nullptr;
    velocitySet_ = nullptr;
    placeLayout_ = nullptr;
    placeSet_[0] = nullptr;
    placeSet_[1] = nullptr;
    traceLayout_ = nullptr;
    traceSet_[0] = nullptr;
    traceSet_[1] = nullptr;
    dotsLayout_ = nullptr;
    dotsSet_[0] = nullptr;
    dotsSet_[1] = nullptr;
    velocityPSO_ = nullptr;
    placePSO_ = nullptr;
    tracePSO_ = nullptr;
    dotsPSO_ = nullptr;
    probeDepth_ = nullptr;
    velocityFb_ = nullptr;
    for (auto& row : probeAtlas_)
        for (auto& t : row) t = nullptr;
    for (auto& t : irradAtlas_) t = nullptr;
    probePrevDelta_ = nullptr;
    // M8.0：BLAS 资源（AS 句柄与 build-input 缓冲，device 存活期释放）。
    // M-PT0：粒子资源随场景一并释放（模拟状态归零）。
    particles_.shutdown();
    blasReady_ = false;
    blasTriangleCount_ = 0;
    blasBuildMs_ = 0.0f;
    blasTimer_.shutdown();
    blas_ = nullptr;
    blasVB_ = nullptr;
    blasIB_ = nullptr;
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
    // M-E1b.4：阴影资源随场景一并清空（数组句柄置空 + 计数/矩阵归零）。
    vsShadow_ = nullptr;
    psShadow_ = nullptr;
    shadowInputLayout_ = nullptr;
    shadowBindingLayout_ = nullptr;
    shadowLightCB_ = nullptr;
    shadowConstants_ = nullptr;
    shadowMap_ = nullptr;
    for (auto& fb : shadowFb_) fb = nullptr;
    pipelineShadow_ = nullptr;
    shadowCount_ = 0;
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
    dbgLines_.shutdown();    // M-P3：释放调试线框管线 / 缓冲
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
    // M7.1：材质反解资源同上——漏掉会让 PSO 活到 device 析构之后，
    // RootSignature 释放时摸 device 内部已释放的缓存（ASan 实锤 UAF）。
    vsMatResolve_ = nullptr;
    psMatResolve_ = nullptr;
    matResolveConstants_ = nullptr;
    matResolveLayout_ = nullptr;
    matResolveSet_ = nullptr;
    matResolvePSO_ = nullptr;
    // M7.2：多材质资源（句柄活到 device 析构之后 = use-after-free，M7.1 教训）。
    for (auto& t : matBaseColor_) t = nullptr;
    for (auto& t : matMR_) t = nullptr;
    materialTable_ = nullptr;
    for (auto& b : modelBatches_) {
        b.set = nullptr;
        b.lightConstants = nullptr;
    }
    constants_ = nullptr;
    indexBuffer_ = nullptr;
    vertexBuffer_ = nullptr;
    ps_ = nullptr;
    vs_ = nullptr;
    indexCount_ = 0;
}

} // namespace nl::renderer::detail
