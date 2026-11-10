#pragma once

// M0.4 共享场景资源：两个后端都用这一份代码，避免把同一套
// glTF 加载 / 资源上传 / 绑定集 / pipeline 逻辑在 DX12 与 Vulkan 里各抄一遍。
// 本文件只依赖 NVRHI 公共头（nvrhi.h），不 include d3d12.h / vulkan.h。

#include "nl/renderer/renderer.hpp"
#include "nl/anim/gltf_anim.hpp"   // M-A5：CPU 蒙皮（骨架/clips/绑定，纯逻辑层）
#include "nl/core/meshlet_io.hpp"  // M8.0：initBlas 签名吃 nl::core::NMeshData（fallback 段）
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

// pbr-direct：与 model.slang 的 LightConstants cbuffer（b1）逐字节对应（80 B）。
// 不塞进 SceneConstants——它严格 160 B 且禁整块 memcpy 的老规矩不变。
struct LightConstants {
    float lightDirW[4];        // xyz = 指向光源的方向（与 b0 g_lightDir 同口径），w 未用
    float lightColor[4];       // rgb = 颜色，a = 强度
    float cameraPos[4];        // xyz = 世界空间相机位置（FrameParams.eye）
    float baseColorFactor[4];  // glTF baseColorFactor
    float mrFactor[4];         // x = metallicFactor，y = roughnessFactor，z = pbr-naive 对照开关
};
static_assert(sizeof(LightConstants) == 80, "cbuffer layout must match shader");

// M7.2（S7.2）：vis buffer 材质间接表条目（shader 侧 GPUMaterial 同布局，见
// visbuf_common.slangh——两处 static_assert 锁 32B，漂移编译期就爆）。
// 归属链：.nmesh NSUB 段（子网格 → 簇区间）→ clusterMeta 槽 3 = materialId
// → 本表条目 → 贴图槽号（shader NonUniformResourceIndex 选槽）。
struct GPUMaterial {
    float baseColorFactor[4];  // glTF baseColorFactor
    float mrFactor[2];         // x = metallicFactor, y = roughnessFactor
    std::uint32_t baseColorSlot;
    std::uint32_t mrSlot;
};
static_assert(sizeof(GPUMaterial) == 32, "GPUMaterial must be 32B and match shader");
inline constexpr std::uint32_t kMaxVisMaterials = 4;

// M-E1b.4：与 model.slang 的 ShadowConstants cbuffer（b2，psScene 专用）逐字节对应。
// 每阴影灯一个正交 lightViewProj（列主序，D3D z∈[0,1]）；params = enabled/bias/mapSize/未用。
struct ShadowConstantsCB {
    float viewProj[4][16];     // 4 个 float4x4（未启用槽保持零矩阵 + enabled=0）
    float params[4][4];        // x=enabled, y=depthBias, z=mapSize, w 未用
};
static_assert(sizeof(ShadowConstantsCB) == 320, "cbuffer layout must match shader");

// M-P3：物理调试线框叠加管线（LINE_LIST）。与 model.slang 同款单源/双目标约定：
// 一份 debug_line.slang 出 spv/dxil；这里只负责收线顶点、画彩色线段叠加层。
// depth test 开、depth write 关——尊重已有深度（被实体挡住的线段不显示），
// 但不写深度（不遮挡后续 HUD）。默认不建/不画，debugLines 关闭时零开销。
// 顶点布局（每顶点 6 float = 24 B）：POSITION(float3) + COLOR(float3)。
class DebugLines {
public:
    // 用 framebuffer 格式信息建唯一一条 LINE_LIST 管线（只建一次）。shaderSuffix
    // = "spv" / "dxil"；入口恒为 vsMain / psMain（debug_line.slang 只有这对）。
    bool ensure(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                const char* shaderSuffix, const char* vsEntry, const char* psEntry,
                std::string& outError);
    // 每帧：写 mvp 常量 → 上传线顶点（固定容量动态 VB，超容量截断）→ LINE_LIST 绘制。
    // 调用方保证 debugLineData 在 cmd 记录期间有效（viewer 持有）。
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport, const FrameParams& params);
    void invalidate();
    void shutdown();

private:
    nvrhi::ShaderHandle           vs_, ps_;
    nvrhi::BufferHandle           constants_;    // volatile CB：mvp (64 B)
    nvrhi::BufferHandle           vertexBuffer_; // 动态 VB（LINE_LIST 顶点流）
    nvrhi::InputLayoutHandle      inputLayout_;
    nvrhi::BindingLayoutHandle    bindingLayout_;
    nvrhi::BindingSetHandle       bindingSet_;
    nvrhi::GraphicsPipelineHandle pipeline_;
    std::uint32_t                 capacity_ = 0; // 当前 VB 容量（顶点数）
};

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

    // M5.1（S5.1）：GPU DAG 遍历选层。nmeshFile 只取 stem（"Suzanne.nmesh" →
    // "Suzanne"），探测 <stem>.lod0..lodN.nmesh 全层驻留：顶点池/局部索引/簇元数据
    // 拼接（vertexBase/localBase 重定基，vsCull 零改动）、逐簇误差/层级/生成组拍平、
    // 组成员 CSR 拍平（组成员不保证连续，2026-11-08 资产实测）。遍历 compute
    // （dag.slang csDag，显式栈）每帧从最粗层根簇下钻，输出 = M4 的
    // visibleList_ + indirectArgs_ 同一接口（S5-PLAN 底线：不另起炉灶）。
    bool initDag(nvrhi::IDevice* device, const char* shaderSuffix,
                 const char* vsEntry, const char* psEntry,
                 const char* nmeshFile, std::string& outError);

    // M6.0（S6.0）：Hi-Z 深度预通道 + 手写 min 金字塔。先走 initDag（S5 遍历
    // 原样复用——S6-PLAN 底线：第二遍剔除的输入接口就是 S4 indirect buffer），
    // 再追加：hiz.slang 六入口（深度预通道 vs/ps、金字塔 min 降采样 + top 回读
    // 两个 compute、层级可视化 vs/ps）、HizConstants volatile CB、金字塔纹理
    // （R32F 全 mip，尺寸随 framebuffer 在 ensurePipelines 建）+ 深度预通道专用
    // D32 附件。每帧顺序 = 遍历 compute → 深度预通道（mip0 = 视点径向距离）→
    // min 金字塔逐级 → 最高 mip 回读 → 主绘制 → 右下角 mip 灰度覆盖层。
    bool initHiz(nvrhi::IDevice* device, const char* shaderSuffix,
                 const char* vsEntry, const char* psEntry,
                 const char* nmeshFile, std::string& outError);

    // M7.0（S7.0）：visibility buffer 打包写入 + 可视化。复用 initCull 的全部
    // .nmesh / vertex pulling 资源（frustum 恒关 = 全簇全画），再追加一个
    // RWStructuredBuffer<uint64_t>（宽×高）与两条管线。每帧三段式：
    //   clearBufferUInt(0xFFFFFFFF) → 簇光栅 atomicMin 写 vis → 全屏 viz 解包输出。
    // 64 位布局/打包函数唯一权威见 renderer/shaders/visbuf_common.slangh。
    bool initVisbuf(nvrhi::IDevice* device, const char* shaderSuffix,
                    const char* vsEntry, const char* psEntry,
                    const char* nmeshFile, bool pulling, std::string& outError);

    // M-A5：CPU 蒙皮场景——rigged GLB（skin + 动画）由 nl::anim::loadGltfSkinnedModel
    // 加载进 Scene（骨架 / clips / 绑定 / bind 空间网格），白色占位贴图（标准资产
    // 纹理暂不用，先看形变），VB 按 CopyDest + keepInitialState 建立供每帧
    // writeBuffer（hud.cpp 同款动态缓冲模式）。每帧 draw(params.animTime)：
    // 采样 clip[0] -> FK -> 调色板(model*IBM) -> LBS -> 动态 VB -> 正常管线绘制。
    // 管线/常量布局与 init 完全共用（CPU 蒙皮后的顶点就是普通 POS/NORM/UV 流）。
    bool initSkin(nvrhi::IDevice* device, const char* shaderSuffix,
                  const char* vsEntry, const char* psEntry,
                  const char* modelFile, std::string& outError);

    // M-E1b.0：ECS 场景（viewer --scene）。objects/lights 由 viewer 从 World 组件
    // O(N) 收集（meshFile 已过白名单）；流程：model.vsscene/pscene 双目标字节码 →
    // meshFile 分组（保持首次出现序）→ 每组 loadGltfScene 首 primitive 建 VB/IB/
    // 贴图/采样/绑定集 → 实例缓冲（每实例 5 个 float4：世界矩阵列主序 4 列 + tint）
    // 与灯缓冲（1+2/灯）一次 writeBuffer → 单 BindingLayout + 每 batch 一个
    // BindingSet（§2.7 单 set 纪律）→ inputLayout（vsScene 三属性）→ 联合包围球写
    // assetSphere_。device 不初始化 Scene 时（backend init sceneMode=true）由此
    // 方法独占建场景资源；shaderSuffix/vsEntry/psEntry 规则同 init 系列。
    bool initScene(nvrhi::IDevice* device, const char* shaderSuffix,
                   const char* vsEntry, const char* psEntry,
                   const SceneObjectDesc* objects, std::uint32_t objectCount,
                   const SceneLightDesc* lights, std::uint32_t lightCount,
                   float ambient, const char* sceneName, std::string& outError);

    // M-E1b.5 渲染用例：每帧刷新实例数据（世界矩阵/tint/材质），灯与拓扑不动。
    // objectCount 必须与 initScene 一致（batch 分组按 meshFile 首现序稳定重建，
    // batch 内保持 objects 原序——与 initScene 同一规则，槽号一一对应）。
    // 世界矩阵逐字节来自 Transform::toMatrix()，parallel/serial 同 dt 序列下
    // 输出逐位一致（spin 系统确定性由 tickParallel 串行对拍保证）。
    bool updateObjects(nvrhi::IDevice* device,
                       const SceneObjectDesc* objects, std::uint32_t objectCount,
                       std::string& outError);

    // M8.0（S8.0）：BLAS 预建——Lumen 光追地基（S8-PLAN）。每个 mesh 一个 BLAS，
    // 几何 = .nmesh v3 'NFLB' fallback 段（S3 落定：S8 直接读，不重新生成）。
    // 零拷贝核对结论（M8.0 开工项）：MeshVertex 32B 交错、position 在偏移 0，
    // GeometryTriangles.vertexStride=32 直指专用 fallback buffer，无需拆
    // position-only。构建走专用命令列表一次性提交（NVRHI 校验层要求 AS build
    // 记录在 Compute 能力级列表上——Graphics 队列列表满足，见 validation
    // requireType 判定），构建耗时 GPU timer 计量进 HUD。
    // 资产无 fallback 段（v1 base / Suzanne-materials）或设备不支持 RT 时跳过
    // （blasReady_ 保持 false，既有模式零扰动）；几何/构建失败才是硬错误。
    bool initBlas(nvrhi::IDevice* device, const nl::core::NMeshData& nm,
                  const char* assetName, std::string& outError);
    // M8.0：BLAS 构建耗时轮询。backend 在 hud_.build() 之前调用（与 pollCullTimer
    // 同纪律：viewer 每帧重建 HudFrame，build 之后写的标志活不过当帧）。
    // 一次性构建：resolve 一次后 blasBuildMs_ 恒定；deterministic 不写真实值保基线。
    void pollBlasTimer(nvrhi::IDevice* device, HudFrame* hud);
    bool blasReady() const { return blasReady_; }
    int  blasTriangleCount() const { return blasTriangleCount_; }

    // ---- M8.1（S8.1）：TLAS 每帧全量 rebuild（UE 口径，D-S8-1 拍板）----
    // initTlas：BLAS 成功后建 TLAS（setTopLevelMaxInstances 预留容量；TLAS 尺寸
    // NVRHI 自算，prebuild 查询可选不用）。设备不支持 RT 或 BLAS 未建时跳过不拦路。
    bool initTlas(nvrhi::IDevice* device, std::string& outError);
    // buildTlasFrame：每帧在**帧命令列表**上记录（§2.8 帧内禁第二 cmd list，正好
    // 满足 0x87d 同列表纪律）。CPU 组装 rt::InstanceDesc 数组（S8-PLAN 拍板：
    // 实例数个位数不值得 GPU 填充）→ buildTopLevelAccelStruct 全量 rebuild（无
    // AllowUpdate）。frame.tlasSpin 时实例绕 Y 旋转（只改 TLAS，不碰光栅化）；
    // frame.tlasProbe 时再记录 1 线程 RayQuery 探针 dispatch（hit t 读回日志）。
    void buildTlasFrame(nvrhi::ICommandList* cmd, const FrameParams& frame);
    // TLAS rebuild 耗时轮询（每帧 begin/end，HUD 非确定性模式显示最近值）。
    void pollTlasTimer(nvrhi::IDevice* device, HudFrame* hud);
    // 探针 readback 轮询（两帧延迟窗口，同 pollCullStats 纪律；--tlas-probe 才有数据）。
    void pollTlasProbe(nvrhi::IDevice* device);
    bool tlasReady() const { return tlasReady_; }
    int  tlasInstanceCount() const { return tlasInstanceCount_; }
  private:
    bool ensureTlasProbe(nvrhi::ICommandList* cmd);
  public:

    // M4.2：轮询两帧前提交的剔除计数 readback（frame-in-flight=2 的延迟窗口，
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

    // M-P3：物理调试线框叠加层绘制入口。后端在 scene_.draw 之后、HUD 之前调用。
    // 仅当 params.debugLines 且数据非空才绘制（缺省零开销）。
    void drawDebugLines(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                       const nvrhi::Viewport& viewport, const FrameParams& params);

    bool ready() const {
        if (sceneMode_)   // M-E1b.0：场景资源齐 + 四套 pipeline 已建
            return pipelineSceneBack_ && pipelineSceneNone_ && pipelineSceneWireBack_
                   && pipelineSceneWireNone_ && !sceneBatches_.empty();
        // M7.0：visbuf 复用 cull_ 的资源通路，但自己的两条管线才是判据——
        // cull 的四套 (fill×cull) 管线在 visbuf 下不必建（ensurePipelines 分支）。
        // vis buffer / 两个 binding set 随视口尺寸在 draw() 里懒建，不进 ready()：
        // backend 正是「!ready() 才调 ensurePipelines」，把它们算进来会首帧死锁。
        if (visbuf_) return visRasterPSO_ && visVizPSO_ && (!pulling_ || matResolvePSO_);
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
    // M-E1b.0：ECS 场景实例数（非 scene 模式恒 0；backend 拷进 DeviceInfo）。
    std::uint32_t instanceCount() const { return sceneInstanceCount_; }
    // M2.3：当前层号与 parent error（v1 资产无 lodMeta → 0 / 0）。
    int   lodLevel() const { return lodLevel_; }
    float lodMaxError() const { return lodMaxError_; }
    // M3.2：dag-top 状态（uploadClusterMesh 纯层加载后 dagGroup 复位 -1）。
    int   dagGroup() const { return dagGroup_; }
    float dagGroupError() const { return dagGroupError_; }
    // M4.1：GPU 剔除统计（两帧前的结果；-1 = 暂无数据）。clusterCount 总簇数恒有。
    int           visibleClusters() const { return cullVisibleCount_; }
    // M5.2：GPU 可见三角数（与 visibleClusters 同一延迟窗口、同一回读环形槽位
    // 的高 4 字节；-1 = 暂无数据）。cull / dag 两种 compute 路径口径一致。
    int           visibleTriangles() const { return visibleTriangles_; }
    std::uint32_t clusterCount() const { return clusterCount_; }
    // M5.0：屏幕误差投影球数据（世界空间 xyz=center w=radius；model 恒单位阵）。
    // assetSphere = 当前显示几何联合球（uploadClusterData 每次重算）；
    // dagGroupSphere = 下钻组记录球（dagGroup()>=0 时有效）。
    const float* assetSphere() const { return assetSphere_; }
    const float* dagGroupSphere() const { return dagGroupSphere_; }
    // M6.0：Hi-Z 金字塔状态（仅 hiz 模式非零；-1 = 暂无数据）。
    // M6.1：top 语义 = 全屏最远表面径向距离（金字塔 max 口径，hiz.slang 文件头）。
    // 三态：≥0 = 真实值；-2 = 背景像素主导（max 口径合法）；-1 = 暂无数据。
    int   hizMipCount() const { return static_cast<int>(hizMipCount_); }
    float hizTopFar() const { return hizTopFar_; }
    // M6.2：被 Hi-Z 剔除的簇节点数（与 hizTopFar 同一延迟窗口；-1 = 暂无数据，
    // 非 dag 模式恒 -1）。剔除前访问数 = visibleClusters() + hizOccluded()。
    int   hizOccluded() const { return hizOccluded_; }

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
    // M-P3：物理调试线框叠加层（LINE_LIST）。默认不建不画，debugLines 关闭时零开销。
    DebugLines                    dbgLines_;
    // 各 init* 写入，供 dbgLines_.ensure 定位 debug_line.{vs,ps}.<suffix> 字节码。
    std::string                   shaderSuffix_;
    nvrhi::BufferHandle           vertexBuffer_, indexBuffer_, constants_;
    nvrhi::TextureHandle          texture_;
    // pbr-direct：metallicRoughness 贴图（t1，RGBA8_UNORM 线性数据；skin 场景
    // 是 1x1 dummy）+ b1 LightConstants volatile CB。仅 init / initSkin 的绑定集
    // 携带这两个槽位——pbr 分支只编译进 psMain，cluster/cull/dag 管线的 SPIR-V
    // 不引用它们（t1 在那边是 StructuredBuffer，同槽异型是 Vulkan 布局非法）。
    nvrhi::TextureHandle          mrTexture_;
    nvrhi::BufferHandle           lightConstants_;
    float pbrBaseColorFactor_[4] = {1.f, 1.f, 1.f, 1.f};
    float pbrMetallicFactor_     = 1.0f;
    float pbrRoughnessFactor_    = 1.0f;
    nvrhi::SamplerHandle          sampler_;
    nvrhi::InputLayoutHandle      inputLayout_;
    nvrhi::BindingLayoutHandle    bindingLayout_;
    nvrhi::BindingSetHandle       bindingSet_;
    nvrhi::GraphicsPipelineHandle pipelineBack_, pipelineNone_;     // M0.4：solid × cull
    nvrhi::GraphicsPipelineHandle pipelineWireBack_, pipelineWireNone_; // M0.8：wireframe × cull
    // M7.2：传统直画路径多材质对照组（D-M7.2-1）。每 prim 一套绑定（t0/t1 贴图
    // + b1 因子独立），几何共享 VB/IB、按区间 drawIndexed(startIndex)。单 prim
    // 资产 = 1 批，行为与 M0.4 逐字节一致；model.slang 因此零改动。
    struct ModelPrimBatch {
        nvrhi::BindingSetHandle set;
        nvrhi::BufferHandle     lightConstants;   // 每 prim 因子独立的 b1 volatile CB
        std::uint32_t           startIndex = 0;   // 共享 IB 内起始索引
        std::uint32_t           indexCount = 0;
        // M7.2：本批材质因子（glTF 提取，每帧随光照一起写进 b1——volatile CB
        // 是全量覆写，因子不能只写一次）
        float                   baseColorFactor[4] = {1.f, 1.f, 1.f, 1.f};
        float                   metallicFactor = 1.0f;
        float                   roughnessFactor = 1.0f;
    };
    std::vector<ModelPrimBatch> modelBatches_;
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
    std::uint32_t                 maxTriPerCluster_ = 0;  // M7.0：triId 8 位打包的断言依据
    std::uint32_t                 nmeshSubmeshCount_ = 1; // M7.2：NSUB 段子网格数（无段=1），
                                                          // visbuf 材质数对账用
    nvrhi::BufferHandle           clusterSpheres_;  // StructuredBuffer<float4>，世界空间球
    nvrhi::BufferHandle           visibleList_;     // StructuredBuffer<uint>，compute append / VS 读
    nvrhi::ShaderHandle           csCull_;
    nvrhi::ComputePipelineHandle  cullComputePSO_;
    nvrhi::BindingLayoutHandle    cullComputeLayout_;  // UAV(0) list + UAV(1) args + SRV/CB
    nvrhi::BindingSetHandle       cullComputeSet_;
    nvrhi::BufferHandle           cullReadback_[2];    // 回读环形（8B × 2：[0]=visibleCount [4]=triCount）
    std::uint32_t                 cullSubmitted_ = 0;  // 已提交剔除的帧数（环形游标源）
    int                           cullPolledSlot_ = -1; // 已消费的最大提交序号（-1 = 无）
    int                           cullVisibleCount_ = -1;
    // M5.2：GPU 原子三角计数器（1 个 uint）。cull.slang u2 / dag.slang u3 各自
    // atomicAdd，每帧 writeBuffer 清零，copyBuffer 进回读环形槽位高 4 字节。
    nvrhi::BufferHandle           triCounter_;
    int                           visibleTriangles_ = -1;
    // 每帧 compute dispatch 前整体重写 args（indexCount 等静态字段不变、instanceCount=0
    // 由 GPU atomicAdd 重建）。init 时按 stride 一次填好。
    nvrhi::DrawIndexedIndirectArguments cullArgsReset_{};

    // ---- M-A5：CPU 蒙皮 ----
    bool skinMode_ = false;                      // draw() 走逐帧蒙皮路径

    // ---- M-E1b.0：ECS 场景资源（viewer --scene；N 实例化物体 + M 方向灯）----
    // 与其他模式互斥：sceneMode_ 置位后 draw/ensurePipelines/ready 走专属分支，
    // 既有路径（normal/cluster/cull/dag/hiz/skin）零改动。
    bool sceneMode_ = false;
    nvrhi::ShaderHandle        vsScene_, psScene_;
    nvrhi::BufferHandle        sceneLights_;      // StructuredBuffer<float4>，1+2/灯
                                                  //（[0]=(lightCount,ambient,1/ΣI,0)）
    nvrhi::InputLayoutHandle   sceneInputLayout_;
    nvrhi::BindingLayoutHandle sceneBindingLayout_;   // 单 layout：t0+s0+b0+t5+t6（§2.7）
    // 每 mesh 一个批次（同一 meshFile 的所有实例共享 VB/IB/贴图/BindingSet）。
    // M-E1b.1：每个 batch 持有自己的 t5 实例缓冲——SV_InstanceID 是 draw 内
    // 0-based，不依赖 startInstanceLocation 的硬件语义（DX12 不把它计入实例号，
    // 旧实现多 batch 时所有批次都读槽 0）；也避开 Vulkan storage buffer SRV
    // 子范围的 minStorageBufferOffsetAlignment 约束（批次起点按 80B 递增）。
    struct SceneMeshBatch {
        nvrhi::BufferHandle vb, ib;
        nvrhi::BufferHandle instances;   // 本 batch 的 StructuredBuffer<float4>，5/实例
        nvrhi::TextureHandle tex;
        nvrhi::SamplerHandle sampler;
        nvrhi::BindingSetHandle set;
        nvrhi::BindingSetHandle shadowSet;   // M-E1b.4：shadow pass 用（b0=lightViewProj + t5 实例）
        std::uint32_t indexCount = 0;
        std::uint32_t instanceCount = 0;
    };
    std::vector<SceneMeshBatch> sceneBatches_;
    // M-E1b.5：updateObjects 复用的 batch 分组映射（initScene 时固化——
    // meshFile 首现序 + batch 内 objects 原序，重打包规则与 initScene 一致）。
    std::vector<std::vector<std::uint32_t>> sceneBatchObjects_;
    std::uint32_t sceneObjectCount_ = 0;
    // instances 的 NVRHI 状态跟踪末态（不设 permanent——SRV 冻结会卡每帧
    // writeBuffer 的 require(CopyDest)，见 initScene/updateObjects 注释）。
    // true = 上一列表末态 ShaderResource；false = CopyDest。update 末态恒
    // CopyDest、draw 末态恒 SRV，序列完全确定。
    bool instanceTrackSRV_ = false;
    nvrhi::GraphicsPipelineHandle pipelineSceneBack_, pipelineSceneNone_;         // solid × cull
    nvrhi::GraphicsPipelineHandle pipelineSceneWireBack_, pipelineSceneWireNone_; // wireframe × cull
    // ---- M-E1b.4：方向灯 2D 阴影（最多 4 盏；R32F Texture2DArray 整数 Load 采样）----
    static constexpr std::uint32_t kMaxShadowLights = 4;
    nvrhi::ShaderHandle        vsShadow_, psShadow_;
    nvrhi::InputLayoutHandle   shadowInputLayout_;       // 与 scene 同签名，绑 vsShadow_ 创建
    nvrhi::BindingLayoutHandle shadowBindingLayout_;     // b0 + t5（§2.7：shadow 管线单一 set）
    nvrhi::BufferHandle        shadowLightCB_;           // volatile 160B（b0 = lightViewProj）
    nvrhi::BufferHandle        shadowConstants_;         // volatile 320B（b2，psScene 读）
    nvrhi::TextureHandle       shadowMap_;               // R32F 1024x1024 arraySize=4（POT，§2.8 精神）
    nvrhi::FramebufferHandle   shadowFb_[kMaxShadowLights];  // 每层一个（无 depth）
    nvrhi::GraphicsPipelineHandle pipelineShadow_;       // cull=None + 无深度测试
    std::uint32_t shadowCount_ = 0;
    float shadowViewProj_[kMaxShadowLights][16];         // CPU 预算，每帧写 b2
    float shadowBias_[kMaxShadowLights] = {};
    float         sceneAmbient_ = 0.28f;
    std::uint32_t sceneLightCount_ = 0;
    std::uint32_t sceneInstanceCount_ = 0;   // instanceCount() 的数据源
    nl::anim::Skeleton        skinSkeleton_;
    std::vector<nl::anim::AnimClip> skinClips_;
    nl::anim::SkinBinding     skinBinding_;
    std::vector<nl::anim::Vec3> skinBasePos_, skinBaseNrm_;    // bind 空间
    nl::anim::Vec3 skinCenter_{};   // bind 包围盒中心：逐帧蒙皮后平移回原点（相机 target=(0,0,0) 取景居中）
    float skinRadius_ = 0.0f;       // t=0 蒙皮输出半径：viewer 自动取景用（DeviceInfo.skinRadius）
  public:
    float skinRadius() const { return skinRadius_; }
  private:
    std::vector<nl::anim::Vec3> skinPos_, skinNrm_;            // 蒙皮输出
    std::vector<nl::anim::BonePose> skinLocal_, skinModel_;    // 采样/FK 暂存
    std::vector<nl::anim::Mat4> skinPalette_;                  // model*IBM
    std::vector<SceneVertex>  skinVertices_;                   // 动态 VB 源

    // ---- M4.2：cull pass GPU 计时 ----
    // 与整帧计时（backend 的 gpuTimer_）独立的第二个 GpuTimer 实例，begin/end 只包住
    // Scene::draw 里的 compute 剔除段。query 延迟到第一次 HUD 路径的 pollCullStats 才建，
    // --no-hud 截图与历史基线完全不碰（M0.6 同款纪律）。
    GpuTimer cullTimer_;   // setLabel("cull")：resolve 日志与整帧计时区分开

    // ---- M5.0（S5.0）：屏幕误差投影球数据 ----
    float assetSphere_[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    float dagGroupSphere_[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // ---- M5.1（S5.1）：GPU DAG 遍历 ----
    bool                         dag_ = false;          // initDag 成功后置位
    std::uint32_t                dagRootCount_ = 0;     // 最粗层簇数（遍历 dispatch 规模）
    std::uint32_t                dagLevelCount_ = 0;    // 层数（lod0..lodN）
    std::uint32_t                dagGroupCount_ = 0;    // 全层组总数（expanded 清零规模）
    nvrhi::BufferHandle          dagConstants_;         // volatile CB：eye/projY/H/near/threshold
    nvrhi::BufferHandle          dagClusterErrors_;     // StructuredBuffer<float>，逐簇世界误差
    nvrhi::BufferHandle          dagGroups_;            // StructuredBuffer<float4>，组记录×2：
                                                        //   [2g]=(cx,cy,cz,r) [2g+1]=(err,first,count,_)
    nvrhi::BufferHandle          dagMembers_;           // StructuredBuffer<uint>，组成员 CSR 展平
    nvrhi::BufferHandle          dagGenGroup_;          // StructuredBuffer<uint>，逐簇生成组（全局组号）
    nvrhi::BufferHandle          dagClusterLevel_;      // StructuredBuffer<uint>，逐簇层号（0=最细）
    nvrhi::BufferHandle          dagGroupExpanded_;     // StructuredBuffer<uint>，组展开声明标志（每帧清零）
    nvrhi::ShaderHandle          csDag_;
    nvrhi::ComputePipelineHandle dagComputePSO_;
    nvrhi::BindingLayoutHandle   dagComputeLayout_;
    nvrhi::BindingSetHandle      dagComputeSet_;    // t8 = 1x1 dummy（hiz 关闭态）
    nvrhi::BindingSetHandle      dagHizSet_;        // M6.1：t8 = 真金字塔（ensureHizTargets 建）
    nvrhi::TextureHandle         hizDummyTex_;      // M6.1：1x1 R32F 占位（非 hiz 模式）
    bool                         hizReady_ = false; // M6.1：金字塔已建，遍历可开遮挡测试
    bool                         hizJustCreated_ = false;  // M6.1：刚（重）建，帧内补全 mip 清屏
    // args 重置模板直接复用 cullArgsReset_（indexCount 同一 identity stride）。

    // ---- M6.0（S6.0）：Hi-Z 深度预通道 + 金字塔（M6.1 拍板修正为 max 口径）----
    // mip0 = 深度预通道颜色 RT（R32F，视点径向距离，每像素最近表面）；mip>=1
    // 由 csHizDown 逐级 max（每 texel 最远表面——遮挡判定口径，hiz.slang 文件头）。
    // hizDepth_ 是预通道专用 D32 附件（硬件 less 测试保证「最近」）。
    // 尺寸随 framebuffer，在 ensurePipelines 建/重建（resize 时随 pipeline 重建）。
    bool                         hiz_ = false;
    std::uint32_t                hizMipCount_ = 0;
    nvrhi::ShaderHandle          vsHizDepth_, psHizDepth_, vsHizViz_, psHizViz_;
    nvrhi::ShaderHandle          csHizDown_, csHizTop_;
    nvrhi::BufferHandle          hizConstants_;      // volatile CB 48B（b1）
    nvrhi::BufferHandle          hizTop_;            // 4B 计数器（csHizTop 写，回读）
    nvrhi::TextureHandle         hizPyramid_;        // R32F，mipLevels=hizMipCount_
    nvrhi::TextureHandle         hizDepth_;          // D32 预通道附件
    nvrhi::FramebufferHandle     prepassFramebuffer_;
    nvrhi::GraphicsPipelineHandle prepassPSO_;       // vsHizDepth+psHizDepth，R32F+D32
    nvrhi::GraphicsPipelineHandle vizPSO_;           // vsHizViz+psHizViz，主帧缓冲
    nvrhi::BindingLayoutHandle   hizPrepassLayout_;  // 预通道单 set：b0+b1+t1..t4
    nvrhi::BindingSetHandle      hizPrepassSet_;     // （Vulkan 多 set 会拆 vk set，b1 读不到）
    nvrhi::BindingLayoutHandle   vizLayout_;         // Texture_SRV(0) + VolatileConstantBuffer(1)
    nvrhi::BindingSetHandle      vizSet_;
    nvrhi::BindingLayoutHandle   chainLayout_;       // Texture_SRV(0) + Texture_UAV(0)
    std::vector<nvrhi::BindingSetHandle> chainSets_; // 每级一个：SRV=mip L-1，UAV=mip L
    nvrhi::ComputePipelineHandle chainPSO_;
    nvrhi::BindingLayoutHandle   topLayout_;         // Texture_SRV(0) + StructuredBuffer_UAV(0)
    nvrhi::BindingSetHandle      topSet_;
    nvrhi::ComputePipelineHandle topPSO_;
    // hizTop_ 清零值每帧 writeBuffer 后由 csHizTop 覆写；回读复用 cullReadback_ 环形
    // （槽位扩到 12B：[0]=可见簇数 [4]=三角数 [8]=asuint(最近径向距离)）。
    float                        hizTopFar_ = -1.0f;  // M6.1：top=最远表面径向距离
    int                          hizOccluded_ = -1;   // M6.2：被 Hi-Z 剔除簇节点数
                                                      //   （≥0 真实值 / -2 背景主导 / -1 无数据）
    float                        hizFarRange_ = 10.0f;  // 可视化对比度参考距离（每帧算）
    // 金字塔/预通道帧缓冲/各级绑定集随视口尺寸在 draw() 懒建（FramebufferInfo 只有
    // 格式没有尺寸；resize 后按 viewport 尺寸重建，绑定集随纹理重建）。
    nvrhi::IDevice*              hizDevice_ = nullptr;
    std::uint32_t                hizW_ = 0, hizH_ = 0;
    bool                         hizFailed_ = false;   // 懒建失败只报一次，后续帧跳过 hiz
    bool ensureHizTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                          std::string& outError);

    // ---- M7.0（S7.0）：visibility buffer ----
    // 介质 = RWStructuredBuffer<uint64_t>，长度 宽×高。vendored NVRHI 全树无
    // R64 纹理格式（预研实测），故从 UE/bevy 的 image atomic 改走 buffer atomic；
    // 打包/clear/原子方向全部保持 Option A 口径，偏离登记 docs/s5/UE-DIFFS.md。
    // 尺寸随视口，在 draw() 里懒建（与 ensureHizTargets 同一套纪律）。
    bool                         visbuf_ = false;
    nvrhi::IDevice*              visbufDevice_ = nullptr;
    std::uint32_t                visW_ = 0, visH_ = 0;
    nvrhi::ShaderHandle          vsVisRaster_, psVisRaster_, vsVisViz_, psVisViz_;
    nvrhi::BufferHandle          visBuffer_;      // u64 buffer，宽×高
    nvrhi::BufferHandle          visConstants_;   // volatile CB 16B（b1）
    nvrhi::BindingLayoutHandle   visRasterLayout_; // b0 + b1 + t1..t4 + u0
    nvrhi::BindingSetHandle      visRasterSet_;
    nvrhi::BindingLayoutHandle   visVizLayout_;    // b1 + t0
    nvrhi::BindingSetHandle      visVizSet_;
    nvrhi::GraphicsPipelineHandle visRasterPSO_;
    nvrhi::GraphicsPipelineHandle visVizPSO_;
    // M7.1（S7.1）：材质反解 pass（pulling）。复用 vis 簇资源 + PBR 贴图。
    // M7.2：贴图升级为每材质数组（baseColor t4..t7 / MR t8..t11）+ 材质间接表
    // t12（GPUMaterial[kMaxVisMaterials]）；texture_/mrTexture_ 仍指材质 0 的
    // 两张贴图（initCull 既有消费者语义不变）。
    bool                         pulling_ = false;                 // 是否启用材质反解
    nvrhi::ShaderHandle          vsMatResolve_, psMatResolve_;
    nvrhi::BufferHandle          matResolveConstants_;             // volatile CB 16B（b2）
    nvrhi::BindingLayoutHandle   matResolveLayout_;                // b0+b1+b2+t0..t3+t4..t11+t12+s0
    nvrhi::BindingSetHandle      matResolveSet_;
    nvrhi::GraphicsPipelineHandle matResolvePSO_;
    nvrhi::TextureHandle         matBaseColor_[kMaxVisMaterials];  // 材质 i 基色贴图
    nvrhi::TextureHandle         matMR_[kMaxVisMaterials];         // 材质 i MR 贴图
    nvrhi::BufferHandle          materialTable_;   // GPUMaterial SSBO（t12，stride 32）
    bool                         visbufFailed_ = false;  // 懒建失败只报一次
    bool ensureVisbufTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                             std::string& outError);

    // ---- M8.0（S8.0）：BLAS 预建（fallback mesh 光追地基）----
    bool                         blasReady_ = false;      // initBlas 成功后置位
    nvrhi::rt::AccelStructHandle blas_;                   // 每 mesh 一个（当前单 mesh 资产）
    nvrhi::BufferHandle          blasVB_, blasIB_;        // fallback 几何（32B 交错 / u32 索引）
    GpuTimer                     blasTimer_;              // setLabel("blas")：init 一次性构建计时
    float                        blasBuildMs_ = 0.0f;     // 最近一次 resolve 的构建耗时（恒定值）
    int                          blasTriangleCount_ = 0;  // HUD 显示用（= fallback 三角数）

    // ---- M8.1（S8.1）：TLAS 每帧全量 rebuild（UE 口径，D-S8-1 拍板）----
    bool                         tlasReady_ = false;      // initTlas 成功后置位
    nvrhi::rt::AccelStructHandle tlas_;                   // top-level，容量 kMaxTlasInstances
    static constexpr int         kMaxTlasInstances = 16;  // 预留容量（当前单实例）
    GpuTimer                     tlasTimer_;              // setLabel("tlas")：每帧 rebuild 计时
    float                        tlasBuildMs_ = 0.0f;     // 最近一次 resolve 的 rebuild 耗时
    int                          tlasInstanceCount_ = 0;  // 当前帧实例数（单 mesh 场景 = 1）
    std::uint32_t                tlasFrameIndex_ = 0;     // buildTlasFrame 自增（spin 角度相位）
    // 探针管线（--tlas-probe 懒建，§2.7：每管线单一 BindingSet）
    bool                         probeReady_ = false;
    bool                         probeFailed_ = false;    // 懒建失败只报一次
    nvrhi::ShaderHandle          csTlasProbe_;
    nvrhi::BindingLayoutHandle   probeLayout_;
    nvrhi::BindingSetHandle      probeSet_;
    nvrhi::ComputePipelineHandle probePSO_;
    nvrhi::BufferHandle          probeOut_;               // 4B hit t（UAV→CopySource 每帧转换）
    nvrhi::BufferHandle          probeReadback_[2];       // 回读环形（两帧延迟，同 cull）
    std::uint32_t                probeSubmitted_ = 0;
    int                          probePolledSlot_ = -1;
};

} // namespace nl::renderer::detail
