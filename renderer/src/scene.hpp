#pragma once

// M0.4 共享场景资源：两个后端都用这一份代码，避免把同一套
// glTF 加载 / 资源上传 / 绑定集 / pipeline 逻辑在 DX12 与 Vulkan 里各抄一遍。
// 本文件只依赖 NVRHI 公共头（nvrhi.h），不 include d3d12.h / vulkan.h。

#include "nl/renderer/renderer.hpp"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

namespace nl::renderer::detail {

// 顶点布局必须与 model.slang 的 VSInput 一致：POSITION / NORMAL / TEXCOORD。
struct SceneVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};
static_assert(sizeof(SceneVertex) == 32, "SceneVertex must be tightly packed");

// M1.3：cluster 场景的顶点 = SceneVertex + 烘好的簇色（CPU 端用与 shader tri-id
// 同一个 pcg hash，从 .nmesh 的簇窗口归属算出，两后端字节一致、截图可复现）。
// 每个顶点条目只属于一个簇窗口（邻簇共享顶点在 .nmesh 里本就按窗口复制），
// 所以三角形三个顶点同色，插值恒定——不需要 SRV / PrimitiveID 查表。
struct ClusterVertex {
    SceneVertex v;
    float color[3];
};
static_assert(sizeof(ClusterVertex) == 44, "ClusterVertex must be tightly packed");

// 与 model.slang 的 SceneConstants cbuffer 逐字节对应（40 floats = 160 B）。
struct SceneConstants {
    float mvp[16];
    float model[16];
    float lightDir[4];
    float options[4];
};
static_assert(sizeof(SceneConstants) == 160, "cbuffer layout must match shader");

class Scene {
public:
    // shaderSuffix = "dxil" / "spv"；vsEntry/psEntry 按后端传（SPIR-V 恒为 main）。
    // modelFile = assets/models/ 下的 .glb 文件名（M0.7 起由 --model 传入，
    // 上层已做白名单校验）；默认仍是 Suzanne.glb，历史基线不受影响。
    // 资产路径用 CMake 注入的 NL_ASSET_DIR / NL_SHADER_DIR，不依赖当前工作目录。
    bool init(nvrhi::IDevice* device, const char* shaderSuffix,
              const char* vsEntry, const char* psEntry,
              const char* modelFile, std::string& outError);

    // M1.3：cluster 场景——加载 assets/meshlets/ 下的 .nmesh（上层白名单校验），
    // 每簇随机着色（CPU pcg hash 烘进 CLUSTERCOLOR 顶点属性），簇窗口展开成
    // 全局索引一次 drawIndexed。绑定的贴图换 1x1 白色占位（布局与 init 一致）。
    bool initCluster(nvrhi::IDevice* device, const char* shaderSuffix,
                     const char* vsEntry, const char* psEntry,
                     const char* nmeshFile, std::string& outError);

    // M2.3：切到另一份 .nmesh（<stem>.lodN.nmesh）。只重建 VB/IB，shader / pipeline /
    // binding set 全部复用；读盘或展开失败时保持原层不变（先成功后替换）。
    // 层号与 parent error 从 .nmesh v2 尾部 section 读出，供 HUD 显示。
    bool setClusterLevel(nvrhi::IDevice* device, const char* nmeshFile,
                         std::string& outError);

    // M3.2：DAG cut 预演（--mode dag-top）。渲染 topFile（链最粗层）的全部簇，
    // 但把 childFile（下一层）中第 drillGroup 组生成的父簇区间
    // [genFirst, genFirst+genCount) 挖掉，换回该组在 childFile 里的成员簇
    // （clusterGroup==drillGroup）。这正是 S5 运行时组原子 cut 的离线预演：
    // 父子衔接处必须无洞无重叠——简化时组间边界点被锁（M2.2 锁点语义），
    // 粗细两版边界顶点逐位一致。校验失败（组号越界 / 无 DAG 血缘 / gen 区间
    // 越界）一律报错退出，绝不静默画半套。
    bool setDagCut(nvrhi::IDevice* device, const char* topFile, const char* childFile,
                   int drillGroup, std::string& outError);

    // 句柄必须在 device 还活着时释放（两个后端的 shutdown 里调用）。
    void shutdown();

    // 用 framebuffer 的格式信息建四套 pipeline：(solid/wireframe) × (开/关背面剔除)，
    // C 键切剔除、V 键/HUD 复选框切线框；着色模式只是 cbuffer 值，与 pipeline 正交。
    bool ensurePipelines(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                         std::string& outError);
    void invalidatePipelines();

    // 每帧：写 volatile CB（MVP）→ 绑资源 → drawIndexed。
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport, const FrameParams& params);

    bool ready() const { return pipelineBack_ && pipelineNone_ && pipelineWireBack_
                              && pipelineWireNone_ && bindingSet_; }
    std::uint32_t indexCount() const { return indexCount_; }
    // M0.7：模型名 / 三角形数，backend 填进 DeviceInfo，HUD 截图自证加载了哪个模型。
    const std::string& modelName() const { return modelName_; }
    std::uint32_t triangleCount() const { return triangleCount_; }
    // M2.3：当前层号与 parent error（v1 资产无 lodMeta → 0 / 0）。
    int   lodLevel() const { return lodLevel_; }
    float lodMaxError() const { return lodMaxError_; }
    // M3.2：dag-top 状态（uploadClusterMesh 纯层加载后 dagGroup 复位 -1）。
    int   dagGroup() const { return dagGroup_; }
    float dagGroupError() const { return dagGroupError_; }

private:
    // M2.3：读 .nmesh → 展开成 ClusterVertex/索引 → 建新 VB/IB 上传 → 替换句柄。
    // initCluster 与 setClusterLevel 共用，保证首次加载与切层走同一条路径。
    bool uploadClusterMesh(nvrhi::IDevice* device, const char* nmeshFile,
                           std::string& outError);
    // M3.2：展开（可选按簇掩码过滤）+ 建新 VB/IB 上传 + 替换句柄与计数。
    // uploadClusterMesh / setDagCut 共用同一条落盘→显存路径。
    bool uploadClusterData(nvrhi::IDevice* device,
                           const std::vector<ClusterVertex>& vertices,
                           const std::vector<std::uint32_t>& indices,
                           const char* modelName, int lodLevel, float maxError,
                           std::string& outError);

    nvrhi::ShaderHandle           vs_, ps_;
    nvrhi::BufferHandle           vertexBuffer_, indexBuffer_, constants_;
    nvrhi::TextureHandle          texture_;
    nvrhi::SamplerHandle          sampler_;
    nvrhi::InputLayoutHandle      inputLayout_;
    nvrhi::BindingLayoutHandle    bindingLayout_;
    nvrhi::BindingSetHandle       bindingSet_;
    nvrhi::GraphicsPipelineHandle pipelineBack_, pipelineNone_;     // M0.4：solid × cull
    nvrhi::GraphicsPipelineHandle pipelineWireBack_, pipelineWireNone_; // M0.8：wireframe × cull
    std::uint32_t                 indexCount_ = 0;
    std::string                   modelName_;
    std::uint32_t                 triangleCount_ = 0;
    int                           lodLevel_ = 0;      // M2.3：来自 .nmesh v2 lodMeta
    float                         lodMaxError_ = 0.0f;
    int                           dagGroup_ = -1;     // M3.2：当前下钻组（-1 = 纯顶层）
    float                         dagGroupError_ = 0.f;
};

} // namespace nl::renderer::detail
