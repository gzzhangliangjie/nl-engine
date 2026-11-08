#pragma once

// M0.4 共享场景资源：两个后端都用这一份代码，避免把同一套
// glTF 加载 / 资源上传 / 绑定集 / pipeline 逻辑在 DX12 与 Vulkan 里各抄一遍。
// 本文件只依赖 NVRHI 公共头（nvrhi.h），不 include d3d12.h / vulkan.h。

#include "nl/renderer/renderer.hpp"
#include "nl/anim/gltf_anim.hpp"   // M-A5：CPU 蒙皮（骨架/clips/绑定，纯逻辑层）
#include "gpu_timer.hpp"     // M4.2：cull pass 计时（renderer/src 内部共享，只依赖 NVRHI 抽象）

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

    // M4.0（S4.0）：GPU vertex pulling 场景——同吃 .nmesh，但几何不展开进顶点缓冲，
    // 而是铺成三张 SSBO（顶点池 / 局部索引 / 簇元数据）+ 一张 identity 索引缓冲 +
    // 一份 indirect args 缓冲，单 drawIndexedIndirect 全簇全画。簇色由 shader 端
    // pcg(簇号) 算出，与 cluster 场景 CPU 端 clusterColor() 同式，逐像素一致。
    // M4.1（S4.1）：frustumCull=true 时再建 cull compute pass（cull.slang csCull）
    // 与可见列表 SSBO——compute 每线程 1 簇做 5 平面包围球测试，通过者 append 簇号
    // 并 atomicAdd 写 indirect args 的 instanceCount；VS 经可见列表间接寻址取簇号
    // （bevy cull_clusters.wesl 蓝本）。false 时可见列表是恒等映射、不跑剔除，
    // 行为与 M4.0 逐像素一致（s4.0 回归基线依赖这条）。
    bool initCull(nvrhi::IDevice* device, const char* shaderSuffix,
                  const char* vsEntry, const char* psEntry,
                  const char* nmeshFile, bool frustumCull, std::string& outError);

    // M-A5：CPU 蒙皮场景——rigged GLB（skin + 动画）由 nl::anim::loadGltfSkinnedModel
    // 加载进 Scene（骨架 / clips / 绑定 / bind 空间网格），白色占位贴图（标准资产
    // 纹理暂不用，先看形变），VB 按 CopyDest + keepInitialState 建立供每帧
    // writeBuffer（hud.cpp 同款动态缓冲模式）。每帧 draw(params.animTime)：
    // 采样 clip[0] -> FK -> 调色板(model*IBM) -> LBS -> 动态 VB -> 正常管线绘制。
    // 管线/常量布局与 init 完全共用（CPU 蒙皮后的顶点就是普通 POS/NORM/UV 流）。
    bool initSkin(nvrhi::IDevice* device, const char* shaderSuffix,
                  const char* vsEntry, const char* psEntry,
                  const char* modelFile, std::string& outError);

    // M4.1：轮询两帧前提交的剔除计数 readback（frame-in-flight=2 的延迟窗口，
    // mapBuffer 内部等 lastUseFence，正常零等待）。backend 在每帧开命令列表前调，
    // 结果经 visibleClusters()/clusterCount() 进 DeviceInfo → HUD / 日志。
    // 没有数据（前两帧 / 非剔除路径）时 visibleClusters() 返回 -1。
    void pollCullStats(nvrhi::IDevice* device);

    // M4.2：cull pass GPU 计时轮询。backend 必须在 hud_.build() **之前**调用——
    // viewer 每帧重建 HudFrame，build 之后写的标志活不过当帧（实测踩坑）。query
    // 延迟到第一次 HUD 路径才建（--no-hud / hud==nullptr 完全不碰，纪律同 M0.6）；
    // deterministic（截图回归）下不写回真实计时，viewer 提供固定样本保基线 MD5。
    void pollCullTimer(nvrhi::IDevice* device, HudFrame* hud);

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

    bool ready() const {
        if (cull_)
            return pipelineCullBack_ && pipelineCullNone_ && pipelineCullWireBack_
                       && pipelineCullWireNone_ && cullBindingSet_;
        return pipelineBack_ && pipelineNone_ && pipelineWireBack_
                              && pipelineWireNone_ && bindingSet_;
    }
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
    // M4.1：GPU 剔除统计（两帧前的结果；-1 = 暂无数据）。clusterCount 总簇数恒有。
    int           visibleClusters() const { return cullVisibleCount_; }
    std::uint32_t clusterCount() const { return clusterCount_; }

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

    // ---- M4.0（S4.0）：GPU vertex pulling 资源 ----
    // 与 cluster 模式共用 .nmesh 资产，但几何铺进 SSBO 由 VS 用 SV_InstanceID/SV_VertexID
    // 拉取，不走任何顶点缓冲（input layout 为 null）。cull_ 置位后 draw 走 indirect 路径。
    bool                         cull_ = false;
    nvrhi::ShaderHandle          vsCull_, psCull_;
    nvrhi::BufferHandle          vertexPool_;       // StructuredBuffer<float>，全顶点展平（8 float/顶点）
    nvrhi::BufferHandle          localIndices_;     // StructuredBuffer<uint>，每簇 3×triCount 局部索引
    nvrhi::BufferHandle          clusterMeta_;      // StructuredBuffer<uint>，每簇 [vertexBase, triCount, localBase, pad]
    nvrhi::BufferHandle          indirectArgs_;     // drawIndexedIndirect 参数缓冲（isDrawIndirectArgs）
    nvrhi::BufferHandle          identityIndex_;    // identity 索引缓冲（0..stride-1，stride=3×maxTri）
    nvrhi::BindingLayoutHandle   cullBindingLayout_;
    nvrhi::BindingSetHandle      cullBindingSet_;
    nvrhi::GraphicsPipelineHandle pipelineCullBack_, pipelineCullNone_;         // solid × cull
    nvrhi::GraphicsPipelineHandle pipelineCullWireBack_, pipelineCullWireNone_; // wireframe × cull

    // ---- M4.1（S4.1）：GPU 视锥剔除（bevy cull_clusters.wesl 蓝本）----
    bool                          frustumCull_ = false;   // --frustum off 时整条路径旁路
    std::uint32_t                 clusterCount_ = 0;      // 总簇数（static dispatch 用）
    nvrhi::BufferHandle           clusterSpheres_;  // StructuredBuffer<float4>，世界空间球
    nvrhi::BufferHandle           visibleList_;     // StructuredBuffer<uint>，compute append / VS 读
    nvrhi::ShaderHandle           csCull_;
    nvrhi::ComputePipelineHandle  cullComputePSO_;
    nvrhi::BindingLayoutHandle    cullComputeLayout_;  // UAV(0) list + UAV(1) args + SRV/CB
    nvrhi::BindingSetHandle       cullComputeSet_;
    nvrhi::BufferHandle           cullReadback_[2];    // instanceCount 回读环形（4B × 2）
    std::uint32_t                 cullSubmitted_ = 0;  // 已提交剔除的帧数（环形游标源）
    int                           cullPolledSlot_ = -1; // 已消费的最大提交序号（-1 = 无）
    int                           cullVisibleCount_ = -1;
    // 每帧 compute dispatch 前整体重写 args（indexCount 等静态字段不变、instanceCount=0
    // 由 GPU atomicAdd 重建）。init 时按 stride 一次填好。
    nvrhi::DrawIndexedIndirectArguments cullArgsReset_{};

    // ---- M-A5：CPU 蒙皮 ----
    bool skinMode_ = false;                      // draw() 走逐帧蒙皮路径
    nl::anim::Skeleton        skinSkeleton_;
    std::vector<nl::anim::AnimClip> skinClips_;
    nl::anim::SkinBinding     skinBinding_;
    std::vector<nl::anim::Vec3> skinBasePos_, skinBaseNrm_;    // bind 空间
    nl::anim::Vec3 skinCenter_{};   // bind 包围盒中心：逐帧蒙皮后平移回原点（相机 target=(0,0,0) 取景居中）
    std::vector<nl::anim::Vec3> skinPos_, skinNrm_;            // 蒙皮输出
    std::vector<nl::anim::BonePose> skinLocal_, skinModel_;    // 采样/FK 暂存
    std::vector<nl::anim::Mat4> skinPalette_;                  // model*IBM
    std::vector<SceneVertex>  skinVertices_;                   // 动态 VB 源

    // ---- M4.2：cull pass GPU 计时 ----
    // 与整帧计时（backend 的 gpuTimer_）独立的第二个 GpuTimer 实例，begin/end 只包住
    // Scene::draw 里的 compute 剔除段。query 延迟到第一次 HUD 路径的 pollCullStats 才建，
    // --no-hud 截图与历史基线完全不碰（M0.6 同款纪律）。
    GpuTimer cullTimer_;   // setLabel("cull")：resolve 日志与整帧计时区分开
};

} // namespace nl::renderer::detail
