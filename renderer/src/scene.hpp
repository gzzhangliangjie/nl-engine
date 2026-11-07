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

private:
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
};

} // namespace nl::renderer::detail
