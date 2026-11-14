#pragma once

// M0.4 共享场景资源：两个后端都用这一份代码，避免把同一套
// glTF 加载 / 资源上传 / 绑定集 / pipeline 逻辑在 DX12 与 Vulkan 里各抄一遍。
// 本文件只依赖 NVRHI 公共头（nvrhi.h），不 include d3d12.h / vulkan.h。

#include "nl/renderer/renderer.hpp"
#include "nl/anim/gltf_anim.hpp"   // M-A5：CPU 蒙皮（骨架/clips/绑定，纯逻辑层）
#include "nl/core/meshlet_io.hpp"  // M8.0：initBlas 签名吃 nl::core::NMeshData（fallback 段）
#include "gpu_timer.hpp"     // M4.2：cull pass 计时（renderer/src 内部共享，只依赖 NVRHI 抽象）

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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

// M-WAT1（water 轨，拍板 A）：水体伪 cluster 的保留 materialId 标记（clusterMeta
// 槽 3）。GPUMaterial 间接表不占位（kMaxVisMaterials 只装真实材质），shader 侧
// NL_WATER_MATERIAL_ID（water_common.slangh）同值；命中标记 = 水体分支。
inline constexpr std::uint32_t kWaterMaterialId = 0xFFFFu;

// M-WAT1：Gerstner 波形参数 CPU 侧结构（osgw 式显式数组；shader 侧布局规范 =
// water_common.slangh 头注 WaterConstants，352B volatile CB；M-WAT3 扩 336→352）。
struct WaterWaveParams {
    float dir[2];    // 波向 xy（单位向量）
    float amp;       // 振幅 A（米）
    float lambda;    // 波长 λ（米）；k = 2π/λ
    float steep;     // 陡度 Q（osgw 逐波口径，水平位移 = Q·A·cosφ）
};
struct WaterConstantsCB {
    WaterWaveParams waves[8];   // 固定 8 波（unused 波 amp=0 精确零贡献）
    float time;                  // 固定步进累积（GameClock::kStep 锚，禁 wall-clock）
    float baseColor[3];          // 深水基色（线性 RGB）
    float ambient;               // 环境因子
    float specPow;               // 太阳 Blinn-Phong 幂
    float specGain;              // 太阳高光增益
    float maxAmp;                // ΣA（clusterSphere 半径膨胀用）
    // M-WAT2（拍板 Q-WA7=A）：折射/吸收/泡沫参数（wcE/wcF/wcG）。
    float ior;                   // 水体折射率（1.333）
    float refractStrength;       // 折射出射距离系数（米/米厚）
    float foamThreshold;         // 泡沫波高占比阈值
    float foamGain;              // 泡沫增益
    float absorb[3];             // Beer-Lambert 消光系数（1/m，RGB）
    float thickMeters;           // bilateral 无邻居时的缺省厚度（米）
    float noReflect;             // --water-noreflect 验收旗标（1 = 反射项归零）
    float floorY;                // 池底平面高度（composite 解析折射 ray-plane）
    float floorHalf;             // 池底半边长（命中判定 [-floorHalf, floorHalf]²）
    float noRefract;             // --water-norefract 验收旗标（1 = 折射路径关闭）
    float shoreFadeDist;         // 岸线软化淡出距离（米，M-WAT3）
    float shoreFoamDist;         // 岸线泡沫窄带宽度（米，M-WAT3）
    float shoreFoamGain;         // 岸线泡沫增益（M-WAT3）
    float noShore;               // --water-noshore 验收旗标（1 = fade 归 1 关闭）
    // 布局对齐 352B = 22×float4（WC 侧 wcA[8]+wcB[8]+wcC+wcD+wcE+wcF+wcG+wcH）；
    // CPU 结构体不直接 memcpy 上传——draw() 逐字段写 float4 数组（字节级同序）。
};

// M-E1b.4：与 model.slang 的 ShadowConstants cbuffer（b2，psScene 专用）逐字节对应。
// 每阴影灯一个正交 lightViewProj（列主序，D3D z∈[0,1]）；params = enabled/bias/mapSize/未用。
struct ShadowConstantsCB {
    float viewProj[4][16];     // 4 个 float4x4（未启用槽保持零矩阵 + enabled=0）
    float params[4][4];        // x=enabled, y=depthBias, z=mapSize, w 未用
};
static_assert(sizeof(ShadowConstantsCB) == 320, "cbuffer layout must match shader");

// M15.0：与 vsm.slang 的 VsmDepthConstants（b0）/ direct.slang 的 VsmConstants
//（b6）逐字节对应（同一块 80B volatile CB 跨管线双寄存器）。
// M15.1 深度 pass：params 位型复用（asuint 解包）——x = packed winX(4b)|winY<<4，
// y = asuint(vertsPerCluster)；per-dirty-page 覆写（maxVersions 2048）。
// 采样侧（csDirect b6 的 272B VsmSampleConstantsCB）：params = enabled /
// depthBias / virtualSize / poolSize（M15.0 后两者同为 2048）。
struct VsmConstantsCB {
    float lightMvp[16];        // float4x4（列主序，mul(M, v)）
    float params[4];
};
static_assert(sizeof(VsmConstantsCB) == 80, "cbuffer layout must match shader");
inline constexpr std::uint32_t kVsmVirtualSize = 2048;   // 每级虚拟空间 texel（16×16 页 × 128）

// M15.1（S15）：csDirect b6 采样常量（272B）——4 级 clipmap MVP + 参数。
// 与 vsm.slang csDirect 侧 cbuffer VsmConstants(b6) 逐字节对应（M15.0 的 80B
// 跨管线共用 CB 拆成两块：深度 pass b0 仍 80B 单级版，采样 b6 独立 272B）。
struct VsmSampleConstantsCB {
    float levelMvp[4][16];     // 每级光源正交 lightViewProj（列主序；level 0 最细）
    float params[4];           // x=vsm 开关 / y=depthBias / z=虚拟 texel 数 / w=池 texel 数
};
static_assert(sizeof(VsmSampleConstantsCB) == 272, "cbuffer layout must match shader");

// M15.1：标页常量（336B）——csMark 专用（b6 同槽不同 set，与 csDirect b6 无冲突）。
// 与 vsm.slang cbuffer VsmMarkConstants(b6) 逐字节对应。
struct VsmMarkConstantsCB {
    float invViewProj[16];     // inverseMat4(params.viewProj)（probe 同款，nl/math）
    float levelMvp[4][16];     // 每级光源正交（与深度 pass / 采样侧同源同帧）
    float depthSize[4];        // x=visW, y=visH, z=virtualSize, w 未用
};
static_assert(sizeof(VsmMarkConstantsCB) == 336, "cbuffer layout must match shader");

// M15.2：剔除/HZB 常量（288B）——b1 剔除专用 set，与 vsm.slang VsmCullConstants
// 逐字节对应。levelMvp[4] 同源同帧；cullParams = {clusterCount, dirtyCount,
// virtualSize, vertsPerCluster}；hzbParams = {srcMip(-1=页池直采), dstMip,
// dstDim, poolDim}（csHzb 每级一版 volatile 覆写）。
struct VsmCullConstantsCB {
    float levelMvp[4][16];
    float cullParams[4];
    float hzbParams[4];
};
static_assert(sizeof(VsmCullConstantsCB) == 288, "cbuffer layout must match shader");

inline constexpr std::uint32_t kVsmLevels = 4;                          // clipmap 级数
inline constexpr std::uint32_t kVsmPagesPerLevel = 256;                 // 每级 16×16 页
inline constexpr std::uint32_t kVsmPageTableEntries = kVsmLevels * kVsmPagesPerLevel;
inline constexpr std::uint32_t kVsmPoolTexels = 4096;                   // 页池边长 texel
inline constexpr std::uint32_t kVsmPoolPages = 1024;                    // 池 32×32 物理页（64MB R32F）

// M15.2 剔除/HZB 布局常量（与 vsm.slang kHzbOffsets/kCullListCap 双持对齐）：
// HZB = 2048² 起 12 级 min 链（mip0 = 页池 2×2 min；页 128 池 texel = 64 hzb
// texel，页级 = mip6）；cullBufs = count[0,1024) / base[1024,2048) /
// cursor[2048,3072) / list[3072, 3072+cap) / 尾 1 word 溢出旗标。
inline constexpr std::uint32_t kVsmHzbMips = 12;
inline constexpr std::uint32_t kVsmHzbWords = 6553600;                  // Σ (2048>>k)²
inline constexpr std::uint32_t kVsmCullListCap = 262144;
inline constexpr std::uint32_t kVsmCullBufWords = 3072 + kVsmCullListCap + 1;

// M15.1 HUD 数据（pollVsmHud 填充，Scene 自持统计；ASCII 纪律 §2.19）。
struct VsmDebugState {
    std::uint32_t frame = 0;                    // 分配器帧号
    std::uint32_t poolUsed = 0;                 // 池内已分配物理页
    std::uint32_t poolTotal = kVsmPoolPages;
    std::uint32_t reqPages = 0;                 // 本帧请求页数（两级延迟位图）
    std::uint32_t dirtyPages = 0;               // 本帧需重渲页数（新分配+失效）
    std::uint32_t evictions = 0;                // 累计 LRU 驱逐
    std::uint32_t levelAlloc[kVsmLevels] = {};  // 每级缓存页数
    float texelSize[kVsmLevels] = {};           // 每级 texel 世界尺寸
};

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

// M-PT0：粒子叠加层（instanced billboard）。与 DebugLines 同款单源/双目标约定：
// 一份 particles.slang 出 spv/dxil。CPU 固定步长模拟（1/60s，与 M-GL 物理
// 同款确定性纪律——截图基线逐字节复现的前提），每帧 writeBuffer 进
// StructuredBuffer<float4>×2（32 B/实例），无顶点缓冲（VS 用 SV_VertexID/
// SV_InstanceID，cull 路径同款）。alpha blend，深度测试开 / 深度写入关——
// 被实体遮挡的粒子不显示，但不写深度（不遮挡后续 HUD）。
// 默认不建/不画（--particles 关闭时零开销，S7 渲染基线字节不变）。
class Particles {
public:
    // 用 framebuffer 格式信息建 billboard 管线（alpha/additive 各一条，懒建缓存）。
    // shaderSuffix = "spv" / "dxil"；入口恒 vsMain/psMain（后端传 "main"，§2.4）。
    bool ensure(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                const char* shaderSuffix, std::string& outError);
    // 每帧：CPU 固定步长模拟一步 → 写 volatile CB + 实例数据 → instanced 绘制。
    // 只在 params.particles 开启时被调用（缺省零开销）。
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport, const FrameParams& params,
              const char* shaderSuffix);
    void invalidate();
    void shutdown();

    std::uint32_t aliveCount() const { return aliveCount_; }

    // M-PT1：.nfx v1 加载端（vfx-tools 转换产物，运行时零 Effekseer 依赖）。
    // 解析 JSON → 折进模拟参数；成功后模拟走 nfx 路径（legacy 喷泉代码路径
    // 原样保留，M-PT0 基线零回归）。失败返回 false 且不清 legacy 参数。
    bool loadNfx(const char* path, std::string& outError);
    bool nfxActive() const { return nfxActive_; }
    const std::string& nfxName() const { return nfxName_; }

private:
    // CPU 固定步长模拟（喷泉发射器：发射率/初速锥/重力/寿命曲线全部常数，
    // RNG = pcg(序列号) 确定性流——run-to-run 与跨后端逐位一致）。
    void step(float dt);

    // M-PT2：nfx 贴图惰性建（textureDirty_ 门控；读 assets/efx/textures/<basename>
    // → stbi 解码 RGBA → RGBA8_UNORM。失败返回 false，调用方回退软圆盘）。
    bool ensureTexture(nvrhi::IDevice* device, std::string& outError);

    static constexpr std::uint32_t kMaxParticles = 4096;

    // 模拟参数面（M-PT0 = 内置喷泉常数；M-PT1 起 .nfx 覆写 box 发射分支）。
    struct Params {
        float rate = 300.0f;                       // 粒子/秒（legacy 锚定；nfx 路径用 rateMin/rateSpan）
        // M-PT4 E1：nfx rate 区间驱动。.nfx emit.rate 本是 [min,max] 二元组，
        // M-PT1~PT3 在 loadNfx 折均值——本里程碑起按帧随机采样（rateSpan=0 =
        // 定值，IEEE 恒等：rateMin + rnd*0 = rateMin，存量全定值基线零漂移）。
        float rateMin = 300.0f, rateSpan = 0.0f;
        float lifeMin = 1.6f, lifeSpan = 0.7f;     // 寿命 [lifeMin, lifeMin+lifeSpan) s
        float spawn[3] = {0.0f, -0.55f, 0.0f};     // 发射点（legacy 锥形发射用）
        float speedMin = 2.0f, speedSpan = 1.0f;   // 锥形初速幅值（legacy）
        float spread = 0.35f;                      // 锥形水平散布（legacy）
        float velMin[3] = {0, 0, 0};               // box 初速 min（nfx）
        float velMax[3] = {0, 0, 0};               // box 初速 max（nfx）
        float accel[3] = {0.0f, -9.8f, 0.0f};      // 常加速度（两路共用，legacy = 重力）
        float sizeMin = 0.05f, sizeSpan = 0.05f;   // 尺寸（nfx：sizeSpan=0 定值）
        float ratioY = 1.0f;                       // M-PT2：Y/X 尺寸比（nfx sizeYMin/sizeMin；
                                                   // legacy/方形恒 1.0。M-PT5 起 per-particle
                                                   // sizeY 走 sizeYAbsMin/Span，本字段为
                                                   // 定值路径（sizeYRange 缺省）的复制源）
        // M-PT5 R1：per-particle 随机 sizeY（.nfx render.sizeYRange 可选字段，
        // 绝对米区间）。enabled=false = 无 → spawn 端 ratioY 逐粒子复制 params_.ratioY
        // （IEEE 恒等，VS 弃 CB 标量 g_params.z 改读实例流后 pt1 定值比例用例逐位同）。
        bool  sizeYRangeEnabled = false;
        float sizeYAbsMin = 0.0f, sizeYAbsSpan = 0.0f;
        // M-PT5 E5：per-particle 旋转（.nfx emitter.ang/angVel 可选字段，弧度
        // 口径 = vendored Effekseer runtime 直存弧度）。缺省 [0,0] = 无旋转 →
        // angle 恒 0（VS cos=1/sin=0 IEEE 恒等）。PVA acceleration 子集外。
        float angMin = 0.0f, angSpan = 0.0f;       // 初始角（弧度）
        float angVelMin = 0.0f, angVelSpan = 0.0f; // 角速度（弧度/秒）
        float colorStart[4] = {1.0f, 0.85f, 0.35f, 1.0f}; // nfx 颜色 t=0
        float colorEnd[4]   = {1.0f, 0.30f, 0.08f, 1.0f}; // nfx 颜色 t=1
        // M-PT4 E2：渐变颜色曲线（.nfx 可选 render.colorCurve；enabled=false =
        // 无，走 colorStart/colorEnd 线性 lerp 原路径逐字保留）。采样全在 CPU
        // （step 填实例流时），PS 零改动——零 shader 保形风险。键值归一化 0..1
        // （.nfx 0..255 / 255），t ∈ [0,1] 非降序（loadNfx 从严校验），采样端点
        // clamp（Effekseer GetColor 同构）。Easing 三次曲线由转换器烘焙成关键帧
        // （17 样本 ≤ 8bit 输出量化误差），Gradient 直映射（intensity 折进色值，
        // 仅全等 intensity 收子集）。
        struct ColorCurve {
            bool enabled = false;
            std::vector<std::array<float, 4>> colorKeys; // {t, r, g, b}
            std::vector<std::array<float, 2>> alphaKeys; // {t, a}
        } colorCurve;
        float fadeIn = 0.0f, fadeOut = 0.0f;       // nfx alpha 淡入/淡出（秒）
        bool  additive = false;                    // nfx blend（false = alpha blend）
        // M-PT3：flipbook 序列帧（.nfx 可选字段；enabled=false = 无，UV 恒等 [0,1]）。
        // 帧选择与 Effekseer UVAnimationType::Animation 同式：frameNum =
        // floor(livingFrames / frameLength) 按 loop 折算，行主序（frameX 先走）；
        // livingFrames = (maxLife - life) × 60（出生起算，Effekseer LivingTime 口径）。
        struct Flipbook {
            bool  enabled = false;
            float frameLength = 1.0f;              // 帧数（Efke 帧 1/60s）
            int   countX = 1, countY = 1;
            int   loopType = 0;                    // 0=once 1=loop 2=reverse
            float offsetU = 0.0f, offsetV = 0.0f;  // UV 起始偏移（V0 = 贴图顶部）
            float cellW = 1.0f, cellH = 1.0f;      // 单帧 UV 尺寸
        } flipbook;
        // M-PT2 贴图不走 Params：basename 进成员 textureBasename_（dirty 门控惰性建纹）
    };

    // GPU 侧实例布局（与 particles.slang 的 ParticleGpu 逐字节对应，64 B）。
    // M-PT2：sizeY 走 CB 标量 g_params.z（ratio=1.0 时 cy*1.0 逐位恒等）。
    // M-PT3：flipbook progress 每粒子不同 → 实例流扩 float4×3（48 B）。
    // M-PT5：E5 per-particle 旋转 + R1 per-particle sizeY（拍板 Q3=b）→ 扩
    // float4×4（64 B）：progressMisc.z = 角度（弧度）、progressMisc.w = 该粒子
    // Y/X 比、misc2[4] = spare（E3/E4/E8 未来字段，免二次布局级重拍）。
    // 已有成员偏移不变（float4 对齐），world/颜色计算路径逐字保留——
    // angle=0（cos=1/sin=0）+ w=ratioY 时 VS 结果逐位恒等，存量基线零漂移。
    struct ParticleGpu {
        float posSize[4];    // xyz = 世界位置，w = 尺寸 X（Y = X × progressMisc.w）
        float colorAlpha[4]; // rgb = 颜色，a = alpha
        float progressMisc[4]; // xy = flipbook 帧号（非 flipbook 恒 0），
                               // z = 角度（弧度，无旋转恒 0），
                               // w = 该粒子 Y/X 尺寸比（定值 = ratioY）
        float misc2[4];      // M-PT5 spare（E3/E4/E8 未来字段）
    };
    static_assert(sizeof(ParticleGpu) == 64, "ParticleGpu must be 64B");

    // CPU 侧模拟状态（SoA 向量化不如确定性重要，AoS 简单直接）。
    struct Sim {
        float pos[3];
        float vel[3];
        float life;     // 剩余寿命（秒）
        float maxLife;  // 总寿命（秒，alpha/size 曲线分母）
        float size;     // 尺寸（米）
        // M-PT5：per-particle 旋转与 Y/X 比（spawn 端折算，step 端 angle 积分）。
        // legacy 路径恒 (0, 0, 1.0)——VS 恒等链的 CPU 侧锚点。
        float angle;    // 当前角（弧度；无旋转恒 0）
        float angVel;   // 角速度（弧度/秒；无旋转恒 0）
        float ratioY;   // 该粒子 Y/X 比（无 sizeYRange = params_.ratioY 精确复制）
    };
    std::vector<Sim>          pool_;       // 活粒子（spawn 序，swap-remove 摘死者）
    std::vector<ParticleGpu>  gpuData_;    // 每帧重填的绘制流（alive 顺序）
    float                     spawnAccum_ = 0.0f;  // 发射率积分残差（固定 dt 下确定性）
    std::uint32_t             spawnSeq_   = 0;     // RNG 流序号（pcg 输入）
    std::uint32_t             rateSeq_    = 0;     // M-PT4 E1：rate 采样专用 pcg 序列（与
                                                // spawnSeq_ 完全独立——新占位禁复用纪律）
    std::uint32_t             aliveCount_ = 0;

    Params                     params_;     // 模拟参数面（loadNfx 覆写）
    bool                       nfxActive_ = false;
    std::string                nfxName_;

    nvrhi::ShaderHandle           vs_, ps_;
    nvrhi::BufferHandle           constants_;     // volatile CB 112 B（b0）
    nvrhi::BufferHandle           instanceData_;  // StructuredBuffer<float4>×4（t0，64 B/实例）
    nvrhi::BindingLayoutHandle    bindingLayout_;
    nvrhi::BindingSetHandle       bindingSet_;
    nvrhi::GraphicsPipelineHandle pipeline_;          // alpha blend（legacy + nfx 缺省）
    nvrhi::GraphicsPipelineHandle pipelineAdditive_;  // additive（nfx blend=additive 时用）

    // M-PT2：贴图链。textureDirty_ 门控惰性建纹（loadNfx 只存 basename，文件 IO/
    // 解码/建纹都推迟到首帧 draw 的 GPU 侧，loadNfx 失败面保持纯参数折算）；
    // 无贴图时 setDesc 绑 1x1 白 dummy（引擎空纹理槽先例，过 NVRHI 校验），
    // PS 经 g_params.y 开关回退软圆盘——M-PT0/M-PT1 画面逐位不变。
    nvrhi::TextureHandle          texture_;       // t1（nfx texture 字段；nullptr = dummy）
    nvrhi::TextureHandle          dummyTex_;      // 1x1 白（无贴图占位）
    nvrhi::SamplerHandle          sampler_;       // s0 Linear clamp
    bool                          textureDirty_ = false;         // loadNfx 标记 → ensure 建纹
    bool                          texturePendingUpload_ = false; // 纹理已建待 cmd 上传
    bool                          dummyPendingUpload_ = false;   // dummy 已建待 cmd 上传
    std::string                   textureBasename_;              // assets/efx/textures/ 下文件名
    std::vector<std::uint8_t>     texturePixels_;  // RGBA8 像素暂存（上传后清）
    std::uint32_t                 texW_ = 0, texH_ = 0;
    std::uint32_t                 texMipCount_ = 1;              // M-PT3：mip 链层数（1 = 无）
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
                  const char* nmeshFile, bool frustumCull, std::string& outError,
                  // M-WAT1：true 时追加水面伪 cluster（仅 pulling 链传入 true；
                  // cull/hiz/rt/dag 模式缺省 false = 零扰动）。
                  bool waterEnable = false);

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
                    const char* nmeshFile, bool pulling, std::string& outError,
                    // M-WAT1：水体开关（仅 --mode pulling 且 --water 时 true）。
                    bool waterEnable = false);

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

    // M-E7（Q-E7-1 拍板 A）：灯缓冲按需更新（light.* cvar 反射层 → 预览光照
    // 面板实时生效）。与 updateObjects 严格同构：单 upload 命令列表 writeBuffer
    // （灯缓冲 permanent 位含 CopyDest，写路径免 barrier）；不建/改任何 Buffer/
    // BindingSet/管线。lightCount 必须与 initScene 一致；shadow/type 规则原样
    // 重排（槽位数超限报错）；阴影灯方向变化不重算 shadowViewProj_（阴影调参
    // 不在本接口范围）。
    bool updateLights(nvrhi::IDevice* device,
                      const SceneLightDesc* lights, std::uint32_t lightCount,
                      float ambient, std::string& outError);

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

    // ---- M13.0（S13）：per-cluster BLAS 预建（S13-PLAN M13.0）----
    // setClusterBlas：viewer --cluster-blas 传入（backend create 阶段、Scene init
    // 之前；initDag 内部消费）。initClusterBlas 在 initDag 拼接循环完成后调用：
    // 逐层逐簇从 CPU 侧 levels 数据展开专用 slab（顶点 position-only 12B、索引
    // u8 局部 → u32 顺序铺）+ 每簇 createAccelStruct + 同列表逐簇 build（M8.0
    // 同款 0x87d 纪律：上传/setBufferState/build/setPermanentBufferState 同列表
    // + waitForIdle）。内存占用 = inputBytes（实际 slab 字节）+ estBytes（AS 上
    // 界估算，NVRHI 不暴露真实尺寸）双口径，日志锚 + HUD 行记录在案。
    void setClusterBlas(bool on) { clusterBlasRequested_ = on; }
    // M-WAT1：--water 传入（backend create 阶段、Scene init 链 initVisbuf/initCull 消费）。
    void setWaterEnable(bool on) { waterEnable_ = on; }
    // M13.1：levels 改指针+计数（initCull 链单层 &nm 直传，避免临时拷贝整份
    // NMeshData；语义不变）。
    bool initClusterBlas(nvrhi::IDevice* device,
                         const nl::core::NMeshData* levels, std::size_t levelCount,
                         std::string& outError);
    bool clusterBlasReady() const { return clusterBlasReady_; }
    int  clusterBlasCount() const { return static_cast<int>(clusterBlas_.size()); }
    int  clusterBlasTriangleCount() const { return clusterBlasTriangleCount_; }
    std::uint64_t clusterBlasInputBytes() const { return clusterBlasInputBytes_; }
    std::uint64_t clusterBlasEstBytes() const { return clusterBlasEstBytes_; }

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
    // M13.1（拍板 A）：TLAS 直挂可见簇 BLAS 多实例（RTXMG 同构）。实例源两级：
    // 回读可见列表（cull/dag 链动态 cut）→ 未就绪退全簇恒等列表（rt 链语义）。
    void buildClusterTlasFrame(nvrhi::ICommandList* cmd, const FrameParams& frame);
    // TLAS rebuild 耗时轮询（每帧 begin/end，HUD 非确定性模式显示最近值）。
    void pollTlasTimer(nvrhi::IDevice* device, HudFrame* hud);
    // 探针 readback 轮询（两帧延迟窗口，同 pollCullStats 纪律；--tlas-probe 才有数据）。
    void pollTlasProbe(nvrhi::IDevice* device);
    // M13.1：可见簇列表回读消费（两帧延迟窗口，同 pollCullStats 纪律；
    // --cluster-blas 才有数据；rt 链无剔除 dispatch 时恒无数据 → TLAS 走全簇）。
    void pollClusterVisible(nvrhi::IDevice* device);
    bool tlasReady() const { return tlasReady_; }
    int  tlasInstanceCount() const { return tlasInstanceCount_; }

    // ---- M8.2（S8.2）：Ray Query 调试射线闭环（S8-PLAN M8.2 t1~t3）----
    // initRt：走 initDag 资产链（BLAS/TLAS 挂在 initDag → initBlas → initTlas 上），
    // 再建 rt 专用 compute/呈现管线与 RtConstants。设备不支持 RT / BLAS 未建时
    // 不拦路（drawRt 里黑屏 + 一次性日志，与 BLAS skip 同口径）。
    bool initRt(nvrhi::IDevice* device, const char* shaderSuffix,
                const char* vsEntry, const char* psEntry,
                const char* nmeshFile, std::string& outError);
    // drawRt：rt 模式渲染（Scene::draw 开头 rtMode_ 分支调用）——compute dispatch
    // 每像素 1 条相机射线打 TLAS 写 RGBA32_FLOAT UAV 目标，再全屏呈现进 framebuffer。
    // 目标纹理随视口懒建（ensureRtTargets，resize 复用 hiz ensureTargets 模式）。
    void drawRt(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                const nvrhi::Viewport& viewport, const FrameParams& params);
    // M9.0 T5：present 初始化 / 呈现抽成共用函数，rt 与 shadow 两条链复用同一张
    // rtTarget_ (RGBA32_FLOAT) 与 psRtPresent；不碰 TLAS / rtComputeSet_。
    bool initRtPresent(nvrhi::IDevice* device, const char* shaderSuffix,
                       std::string& outError);
    bool presentRtTarget(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                         const nvrhi::Viewport& viewport, std::string& outError);
    // T0：rtTarget_(RGBA32) → hdrSceneColor_(R11G11B10) 直拷；hdrSceneColor_ → swapchain 直通 tonemap。
    bool blitRtToHdr(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                     std::string& outError);
    bool presentTonemap(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                        const nvrhi::Viewport& viewport, std::string& outError);
    // T0 T2：场景主 pass 渲到 hdrSceneColor_ 前清 HDR 颜色 + 深度，并登记状态（§2.15/§2.19）。
    // 返回 false 表示 hdr 目标未就绪（回落直渲 swapchain 由调用方决定）。
    bool beginHdrSceneTarget(nvrhi::ICommandList* cmd, const float clearColor[4],
                             std::string& outError);

    // ---- M9.0（S9）：--mode shadow 的 compute 直光对照路径（Q4 方案 A）----
    // initShadow = initVisbuf(pulling=true) 全套 vis/材质资源 + initRtPresent 共用
    // 呈现骨架 + direct.cs compute 管线；不碰 BLAS/TLAS/rays。
    bool initShadow(nvrhi::IDevice* device, const char* shaderSuffix,
                    const char* vsEntry, const char* psEntry,
                    const char* nmeshFile, std::string& outError);
    void drawShadow(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                    const nvrhi::Viewport& viewport, const FrameParams& params);

    // ---- M9.2（S9）：--mode reference 的离线参考模式（Q4 方案 A，复用面同 shadow）----
    // initReference = initVisbuf(pulling) + initRtPresent 呈现骨架 + direct.csref compute
    // 管线 + refConstants_（b4 累积参数）+ refPresent（÷samplesDone 再 gamma）；不碰
    // 任何独立 GBuffer 资源。与 shadow 互斥（一个 Scene 只跑一种）。
    bool initReference(nvrhi::IDevice* device, const char* shaderSuffix,
                       const char* vsEntry, const char* psEntry,
                       const char* nmeshFile, std::string& outError);
    void drawReference(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                       const nvrhi::Viewport& viewport, const FrameParams& params);
    bool presentRefTarget(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                          const nvrhi::Viewport& viewport, std::string& outError);
    // M10.3：drawReference 内按 params.referenceGI 分支 GI 参考管线（同 set/尺寸闸/
    // samplesDone_ 复用，只换 refGIPSO_ + RefConstants 32B 段；物理口径见
    // direct.slang csReferenceGI 注释）。
    // M10.3（Q14-2）：--compare <png> 右半屏参考图贴片（present 层 quad）。
    bool loadCompareImage(const char* path, std::string& outError);
    void drawCompareImage(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                          const nvrhi::Viewport& viewport);
    // HUD / 日志进度源（Scene::draw 不持 hud，由 backend 在 renderFrame 后取用写回）。
    bool               referenceActive() const { return reference_; }
    std::uint32_t      referenceSamplesDone() const { return samplesDone_; }
    float              referenceProgress() const { return refProgress_; }

    // ---- M10.0（S10）：--mode probe-place 屏幕探针 cascade 布点 ----
    // initProbe = initVisbuf(pulling=true) 资产链（M10.1 直光查表机器已备）+
    // velocity prepass 管线（Q10-1 UE 口径独立 pass）+ csPlace 布点 compute +
    // probe_viz 点可视化。资源懒建在 ensureProbeTargets（随视口/参数重建）。
    bool initProbe(nvrhi::IDevice* device, const char* shaderSuffix,
                   const char* vsEntry, const char* psEntry,
                   const char* nmeshFile, std::string& outError);
    // drawProbeFrame：velocity prepass（自建 probeDepth_ 做深度测试+写入，写
    // velocity_ = curUV-prevUV）+ csPlace cascade 布点 dispatch。在 pulling 分支
    // matResolve 之后、tonemap 之前调用（布点读的是本帧 velocity/深度）。
    void drawProbeFrame(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                        const FrameParams& params);
    // drawProbeDots：探针点可视化（每探针一实例 quad，按级着色）。tonemap 之后
    // 画在 swapchain 上（深度关，全探针可见——布点覆盖性证据）。
    void drawProbeDots(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                       const nvrhi::Viewport& viewport, const FrameParams& params);
    // drawProbeGather（M10.2）：csGather 每像素探针插值（深度选级 + UE 权重）
    // → csGatherCompose 与直光合成（读 hdr 写 rtTarget_ → blitRtToHdr 回 hdr）。
    // 在 drawProbeFrame 之后、presentTonemap 之前调用（gather 读本帧 atlas）；
    // gather 态下调用方跳过 drawProbeDots（Q13-3：无点叠加）。
    bool drawProbeGather(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                         const FrameParams& params, std::string& outError);
    // S12 M12.0（Q12-A）：csFilterTemporal GI 分量时域累积。仅 filter 态在
    // csGather 与 csGatherCompose 之间插入；非 filter 态 compose 的绑定与执行
    // 顺序一字不动（M11.1 冻结基线代码形态实锤）。
    bool drawFilterTemporal(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                            const FrameParams& params, std::string& outError);
    // S12 M12.1（Q12-C A）：5x5 联合双边，插在 temporal 与 compose 之间，
    // 从 history[write] 只读邻域、写回 gatherLinear_（零新增 RT）。
    bool drawFilterSpatial(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                           const FrameParams& params, std::string& outError);

    // ---- S11 M11.0（S11）：--mode cache-viz 世界缓存 flat hashmap ----
    // initCache = 缓冲（checksums/life/geo ×kCacheCapacity）+ 三 compute 管线；
    // 由 initProbe 尾部调用（cache-viz 复用 probe 链的 probeDepth_ 反投影基建）。
    bool initCache(nvrhi::IDevice* device, const char* shaderSuffix,
                   std::string& outError);
    // drawCacheFrame = 首帧清零 + csCacheDecay（全表衰减）+ csCacheFill（半分辨率
    // 查询即插入）。在 drawProbeFrame 之后调用（同帧深度/几何已就绪）。
    void drawCacheFrame(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                        const FrameParams& params);
    // drawCacheViz = csCacheViz 全像素 query-only 查表伪彩（hdrSceneColor_ UAV 全
    // 替换，同 csGatherCompose 呈现链口径）。presentTonemap 之前调用。
    bool drawCacheViz(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                      const FrameParams& params, std::string& outError);
    // drawIrradDump（M11.2 验收）：csIrradDump 把 irradAtlas_[0] 直出到
    // hdrSceneColor_（全替换，绕开 gather——合成图在该场景读不到 M11.2 增量）。
    // 仅 --probe-atlas-dump 时调用，presentTonemap 之前。
    bool drawIrradDump(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                       const FrameParams& params, std::string& outError);
    // drawGiDump（S12 验收）：复用 csIrradDump / irradDumpLayout_ / irradDumpPSO_，
    // 只把 t0 源换成累积后的 gatherLinear_，全替换写 hdrSceneColor_（绕开 compose）。
    // 合成图在本场景结构性读不到 GI 变化（M11.2 实锤）→ 验收对象必须是 GI 分量本身。
    bool drawGiDump(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                    std::string& outError);
    // drawSSR（M-WAT0，water 轨）：SSR 屏幕空间反射全链——径向距离 min 金字塔
    //（vis deviceZ 反投影）+ 场景色均值链 + march（UE SSRT 口径）+ 合成叠加
    // 写回 hdrSceneColor_。仅 pulling 分支、params.ssr 开时调用（cache-viz 之后、
    // presentTonemap 之前）；关 = 一行不执行 = 冻结基线 bytes。
    bool drawSSR(nvrhi::ICommandList* cmd, const nvrhi::Viewport& viewport,
                 const FrameParams& params, std::string& outError);
  private:
    // 懒建 cache sets（依赖 probeDepth_ / hdrSceneColor_，句柄变了就重建——
    // set 是不可变对象，纹理重建后必须换新 set）。
    bool ensureCacheSets(nvrhi::IDevice* device, std::string& outError);
    // 懒建 cache 管线（shader/PSO/缓冲/CB；尺寸固定不随视口重建）。
    bool ensureCachePipelines(nvrhi::IDevice* device, const char* shaderSuffix,
                              std::string& outError);
    // 懒建 SSR 目标（金字塔/场景色链/ssrColor_，随视口尺寸 POT 化懒建重建）。
    bool ensureSsrTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                          std::string& outError);
    // 懒建 SSR 管线（shader/PSO/CB/布局；尺寸无关不随视口重建）。
    bool ensureSsrPipelines(nvrhi::IDevice* device, const char* shaderSuffix,
                            std::string& outError);
  public:
    // 懒建 probe 资源（D32 自建深度 / velocityFb_ / atlas×2 parity / prevDelta /
    // sets / PSO）。baseRes 变化（ImGui）→ 参数入闸 → 纹理与 set 整组重建。
    bool ensureProbeTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                            std::uint32_t baseRes, std::string& outError);
  public:

    // 共用槽位表：material.slang 与 direct.slang 同源（§2.18 唯一权威，避免
    // 两条链各写一份槽位表而漂移）。
    void addResolveLayoutItems(nvrhi::BindingLayoutDesc& desc) const;
    void addResolveSetItems(nvrhi::BindingSetDesc& desc) const;
    // b1 LightConstants + b2 MatResolveConstants 每帧写入（pulling / shadow 共用）。
    void writeResolveConstants(nvrhi::ICommandList* cmd, const FrameParams& params);
    // M-WAT1：水体 CB（288B）帧写入——visbuf 帧序列 raster 之前调用（VS 位移
    // 先于首次绑定使用，否则 NVRHI 报 volatile CB 未写即用）。
    void writeWaterConstants(nvrhi::ICommandList* cmd, const FrameParams& params);
  private:
    bool ensureRtTargets(nvrhi::IDevice* device, float viewportW, float viewportH,
                         std::string& outError);
  public:
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
    // M-PERF1：per-pass 计时轮询。backend beginFrame 每帧无条件调用（与 gpuTimer_
    // poll 同款「移出 HUD 块」口径——--no-hud 也要出日志锚）；hud 可空。
    // --pass-timing 关 = passTimingWanted_ false = 直返（零 query、零开销）。
    void pollPassTimers(nvrhi::IDevice* device, HudFrame* hud);
    // M15.1：VSM 页池占用面板填充（--vsm-panel 才 show）。backend beginFrame 段
    // 调用（与 pollCullTimer 同位）；show=false 零开销。
    void pollVsmHud(HudFrame* hud) const;

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

    // M-SC7：ECS 场景资源子集释放（initScene 建立的全部句柄：场景管线/batch/
    // 灯缓冲/中性兜底纹理/阴影链）。initScene 开头调用一次 = 可重入（运行期
    // 脚本 attach/detach 后帧边界整场景重建，拍板方案 A）；shutdown() 复用
    // 同一清单（不再各写一份）。仅当 device 存活期调用——T0/UAF 口径不变。
    void shutdownEcsScene();

    // 用 framebuffer 的格式信息建四套 pipeline：(solid/wireframe) × (开/关背面剔除)，
    // C 键切剔除、V 键/HUD 复选框切线框；着色模式只是 cbuffer 值，与 pipeline 正交。
    bool ensurePipelines(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                         std::string& outError);
    void invalidatePipelines();

    // 每帧：写 volatile CB（MVP）→ 绑资源 → drawIndexed。
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport, const FrameParams& params);

    // T0：backend 注入共享场景深度（D32 keepInitialState）；离屏 HDR fb 的 depth
    // attachment 复用它，overlay（dbgLines/particles）深度测试语义回到 pre-T0。
    // resize 重建 depth_ 后须重调（内部置空 hdrSceneFb_ 触发整组重建）。
    void setSharedDepth(nvrhi::ITexture* depth);

    // M-P3：物理调试线框叠加层绘制入口。后端在 scene_.draw 之后、HUD 之前调用。
    // 仅当 params.debugLines 且数据非空才绘制（缺省零开销）。
    void drawDebugLines(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                       const nvrhi::Viewport& viewport, const FrameParams& params);

    // M-PT0：粒子叠加层绘制入口。后端在 scene_.draw 之后、HUD 之前调用（与
    // drawDebugLines 同一挂点顺序：粒子在其后画）。仅当 params.particles 才
    // 步进模拟 + 绘制（缺省零开销，既有模式基线一像素不动）。
    void drawParticles(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
                       const nvrhi::Viewport& viewport, const FrameParams& params);
    // M-PT0：活粒子数（HUD/DeviceInfo 证据行数据源；--particles 关闭时恒 0）。
    int particlesAlive() const { return static_cast<int>(particles_.aliveCount()); }
    // M-PT1：.nfx v1 加载端（viewer --nfx 调用；成功后模拟走 nfx 参数面）。
    bool loadParticlesNfx(const char* path, std::string& outError) {
        return particles_.loadNfx(path, outError);
    }
    bool particlesNfxActive() const { return particles_.nfxActive(); }
    const std::string& particlesNfxName() const { return particles_.nfxName(); }
    // M-TR0：draw call 计数（K1）。只统计 Scene::draw 主路径里提交的图形 draw，
    // 含 initCluster 单批兜底；不含 compute dispatch 与 ImGui UI。
    void noteDraw() { ++drawCalls_; }
    void resetDrawCalls() { drawCalls_ = 0; }
    std::uint32_t drawCalls() const { return drawCalls_; }

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
    // M-PT0：粒子叠加层（instanced billboard）。默认不建不画，particles 关闭时零开销。
    Particles                     particles_;
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
    std::uint32_t                 drawCalls_ = 0;   // M-TR0 K1：Scene::draw 每帧清零后累加
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
        nvrhi::BufferHandle instances;   // 本 batch 的 StructuredBuffer<float4>，7/实例
        nvrhi::TextureHandle tex;        // t0 baseColor（RGBA8_UNORM，单 mip）
        // P9 V2 全 PBR：t1 metallicRoughness / t2 normal / t3 occlusion / t4
        // emissive。无对应贴图的 primitive 绑共享中性兜底纹理（见
        // sceneNeutral* 成员），shader 按 matFlags 分支，兜底路径零采样。
        nvrhi::TextureHandle texMR, texNormal, texOccl, texEmissive;
        nvrhi::SamplerHandle sampler;
        nvrhi::BindingSetHandle set;
        nvrhi::BindingSetHandle shadowSet;   // M-E1b.4：shadow pass 用（b0=lightViewProj + t5 实例）
        std::uint32_t indexCount = 0;
        std::uint32_t instanceCount = 0;
        // P9 V2：batch 级材质量（全部实例共享同一 GLB primitive 材质）。
        // updateObjects 重打包实例 [5].z/.w 与 [6] 时从这里取。
        std::uint32_t matFlags = 0;          // hasMR=1 | hasNormal=2 | hasOccl=4 | hasEmissive=8
        float normalScale = 1.0f;            // glTF normalTexture.scale
        float emissiveFactor[3] = {0.f, 0.f, 0.f};
        float occlStrength = 1.0f;           // glTF occlusionTexture.strength
        // P9 V2：GLB 标量因子（hasMR=0 且实例未显式覆盖时进实例 [5].xy）。
        float metallicFactor = 1.0f;         // glTF spec 缺省 1
        float roughnessFactor = 1.0f;        // glTF spec 缺省 1
    };
    // P9 V2：共享中性兜底纹理（1×1，绑定恒非空、shader 在 matFlags 未置位时
    // 不采样）—— normal=(0.5,0.5,1,1)（切线空间零偏移）、MR=(1,1,1,1)、
    // occl=(1,1,1,1)、emissive=黑。全场景一份，全 batch 复用。
    // 注意声明位置必须在 sceneBatches_ **之前**：NVRHI 不持有 BindingSet 对
    // 绑定资源的引用（用户侧保活），成员按声明逆序析构——放后面会让中性
    // 纹理先于各 batch 的 set 释放 → Vulkan 退出段错误（DX12 容忍、VK 不容忍，
    // 2026-11-13 实测）。
    nvrhi::TextureHandle sceneNeutralNormal_, sceneNeutralMR_, sceneNeutralOccl_, sceneNeutralEmissive_;
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

    // ---- M-PERF1：per-pass GPU 计时（Q-P4/Q-P5，docs/perf/PERF-PLAN.md）----
    // 13 个命名 pass 各一对 begin/end（PassTimers::PassId）。--pass-timing 关 =
    // passTimingWanted_ 恒 false = ensure 从未调 = 一个 query 都不建 = 渲染路径
    // 零变化。ensure 走 pollPassTimers（backend beginFrame 每帧无条件调用，
    // hud 可空——--no-hud 也要出日志锚）；query 创建晚一帧，可接受。
    PassTimers passTimers_;
    bool passTimingWanted_ = false;    // draw() 从 FrameParams.passTiming 单向锁存
    bool passTimingLogged_[PassTimers::kPassCount] = {};   // 每 pass 首次 resolve 日志锚

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
    // M-WAT1（拍板 A）：水体——init 期 true 时 initCull 追加水面伪 cluster 网格，
    // visbuf/direct/material/ssr 四条管线条件加绑 WaterConstants（288B volatile）。
    // 缺省 false = 网格不追加、CB 不建、layout 不变（全链 bytes 保形）。
    bool                         waterEnable_ = false;
    nvrhi::BufferHandle          waterConstants_; // volatile CB 288B（visbuf b2 / matResolve b5 / direct b7 / ssr b6）
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
    nvrhi::BufferHandle          rasterFogCB_;            // volatile CB 32B（b4：M-WEA2 raster 直光雾 fogA/fogB）
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

    // ---- M13.0（S13）：per-cluster BLAS 预建（S13-PLAN M13.0；--cluster-blas）----
    // 每个簇用自身几何预建一个小 BLAS、全部常驻；所有簇 BLAS 共享两块
    // build-input slab（position-only 12B/顶点 VB + 重展开 u32 索引 IB），geometry
    // 用 vertexOffset/indexOffset 引用各簇窗口（NVRHI GeometryTriangles 支持）。
    // clusterBlas_ 下标 = initDag 拼接后的全局簇 id（层间连续拼接，与 clusterMeta
    // 同序），M13.1 回读可见簇列表直接按 id 查。默认关（viewer --cluster-blas
    // 开启）：万级 createAccelStruct + build 是启动期一次性开销，不碰既有路径。
    bool                         clusterBlasRequested_ = false;  // --cluster-blas 开关
    bool                         clusterBlasReady_ = false;      // initClusterBlas 成功后置位
    std::vector<nvrhi::rt::AccelStructHandle> clusterBlas_;      // [全局簇 id] → BLAS
    nvrhi::BufferHandle          clusterBlasVB_, clusterBlasIB_; // 共享 build-input slab
    GpuTimer                     clusterBlasTimer_;              // setLabel("clusterblas")
    float                        clusterBlasBuildMs_ = 0.0f;     // resolve 后恒定
    int                          clusterBlasTriangleCount_ = 0;  // 全部簇三角合计
    std::uint64_t                clusterBlasInputBytes_ = 0;     // slab 实际字节（确定值）
    std::uint64_t                clusterBlasEstBytes_ = 0;       // AS 上界估算（NVRHI 不暴露真实尺寸）

    // ---- M13.1（S13，拍板 A）：可见簇列表回读环形 + TLAS 直挂每可见簇一实例 ----
    // 2 槽 host-visible 环：每槽 = 全簇 id 列表（clusterCount_*4B）+ 尾部 4B 可见数
    // （拷自 indirectArgs_[1]）。cull/dag dispatch 后整表拷入，pollClusterVisible
    // 延迟两帧消费（pollCullStats 同窗口纪律，mapBuffer 内部等 lastUseFence 零等待）。
    nvrhi::BufferHandle          vlReadback_[2];
    std::uint64_t                vlListBytes_ = 0;               // 单槽列表字节数（count 存尾部）
    std::vector<std::uint32_t>   clusterVlIds_;                  // 最近一次回读的可见簇 id
    int                          clusterVlFrame_ = -1;           // 数据所属帧序（-1 = 未就绪）
    std::vector<nvrhi::rt::InstanceDesc> clusterTlasInstances_;  // CPU 组装缓冲（避免每帧 realloc）

    // ---- M8.1（S8.1）：TLAS 每帧全量 rebuild（UE 口径，D-S8-1 拍板）----
    bool                         tlasReady_ = false;      // initTlas 成功后置位
    nvrhi::rt::AccelStructHandle tlas_;                   // top-level，容量 kMaxTlasInstances
    static constexpr int         kMaxTlasInstances = 16;  // 预留容量（当前单实例）
    GpuTimer                     tlasTimer_;              // setLabel("tlas")：每帧 rebuild 计时
    float                        tlasBuildMs_ = 0.0f;     // 最近一次 resolve 的 rebuild 耗时
    int                          tlasInstanceCount_ = 0;  // 当前帧实例数（单 mesh 场景 = 1）
    std::uint32_t                tlasFrameIndex_ = 0;     // buildTlasFrame 自增（spin 角度相位）
    bool                         clusterTlasReadbackLogged_ = false;  // M13.1：readback 首帧日志锚
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

    // ---- M8.2：rt 模式（RayQuery 调试射线闭环）----
    bool                         rt_ = false;             // initRt 成功后置位
    nvrhi::IDevice*              rtDevice_ = nullptr;     // initRt 存（ensureRtTargets 用，同 hizDevice_）
    nvrhi::IDevice*              t0Device_ = nullptr;     // T0：ensurePipelines 存（raster 主路径懒建离屏 HDR 用）
    nvrhi::ShaderHandle          csRt_;
    nvrhi::ShaderHandle          vsRtPresent_;
    nvrhi::ShaderHandle          psRtPresent_;
    nvrhi::ComputePipelineHandle rtPSO_;
    nvrhi::GraphicsPipelineHandle rtPresentPSO_;          // 随 framebuffer 信息建
    nvrhi::BindingLayoutHandle   rtComputeLayout_;
    nvrhi::BindingSetHandle      rtComputeSet_;           // TLAS + UAV 纹理（drawRt 懒建，TLAS 就绪后）
    nvrhi::BindingLayoutHandle   rtPresentLayout_;
    nvrhi::BindingSetHandle      rtPresentSet_;           // SRV 纹理（ensureRtTargets 建）
    nvrhi::BufferHandle          rtConstants_;            // volatile CB 80B（每帧全量覆写）
    nvrhi::TextureHandle         rtTarget_;               // RGBA32_FLOAT UAV 目标
    std::uint32_t                rtW_ = 0, rtH_ = 0;
    bool                         rtNoTlasLogged_ = false; // TLAS 未就绪的一次性日志

    // ---- T0（S9 之后、S10 之前）：共享 temporal / post-chain 基建（决策 A + UE 口径）----
    // 离屏 HDR 场景色 + 独立 velocity + history 双纹理 ping-pong；主帧从直出 swapchain 改为
    // 渲到 hdrSceneColor_，经最小 tonemap 直通合成写 swapchain（Q-H3=UE：tonemap 前无 TSR 时
    // 即最终输出）。属性 GBuffer 不落盘（Q-H2，即取即算）。状态纪律见 §2.19。
    nvrhi::TextureHandle         hdrSceneColor_;   // R11G11B10_FLOAT 离屏 HDR 场景色
    nvrhi::FramebufferHandle     hdrColorFb_;      // 仅 hdrSceneColor_（blit 写点，无 depth）
    nvrhi::TextureHandle         velocity_;        // RG16_FLOAT 逐像素 motion vector
    nvrhi::TextureHandle         history0_;        // RGBA32_FLOAT ping-pong A（S12 起消费：rgb 累积 GI + a 帧数）
    nvrhi::TextureHandle         history1_;        // RGBA32_FLOAT ping-pong B（T0 建纹理后零消费，M12.0 解）
    std::uint32_t                t0W_ = 0, t0H_ = 0;
    std::uint32_t                t0Parity_ = 0;   // history ping-pong 帧奇偶（0/1 交换）

    // T0 T2：离屏场景深度（D32）+ 含 depth 的 hdr 帧缓冲（场景主 pass 渲点）。
    // 随视口尺寸懒建；depth 不设 permanent，逐帧 beginTracking（§2.15/§2.19）。
    nvrhi::TextureHandle         t0Depth_;
    nvrhi::FramebufferHandle     hdrSceneFb_;      // hdrSceneColor_ + t0Depth_

    // blit：rtTarget_(RGBA32) → hdrSceneColor_(R11G11B10) 直拷（无 gamma）。
    nvrhi::ShaderHandle          psBlitToHdr_;
    nvrhi::GraphicsPipelineHandle blitPSO_;       // 随 hdrColorFb_ 信息建
    nvrhi::BindingLayoutHandle   blitLayout_;
    nvrhi::BindingSetHandle      blitSet_;        // t0 = rtTarget_ SRV
    // tonemap：hdrSceneColor_ → swapchain 直通合成（无 gamma，gamma 已烤进 scene color）。
    nvrhi::ShaderHandle          psTonemap_;
    nvrhi::GraphicsPipelineHandle tonemapPSO_;    // 随 swapchain framebuffer 信息建
    nvrhi::BindingLayoutHandle   tonemapLayout_;
    nvrhi::BindingSetHandle      tonemapSet_;     // t0 = hdrSceneColor_ SRV

    // ---- M9.0：shadow 模式（compute 直光对照）----
    // 与其它 mode 互斥（一个 Scene 实例只跑一种）。复用面：输出 = rtTarget_ /
    // 呈现 = initRtPresent + presentRtTarget（M8.2）；几何与材质资源 =
    // initVisbuf(pulling)（M7.1/7.2）。不新建任何独立 GBuffer 纹理（Q4 方案 A）。
    bool                         shadow_ = false;
    bool                         shadowFailed_ = false; // 懒建失败只报一次
    bool                         shadowNoTlasLogged_ = false; // TLAS 未就绪的一次性日志
    nvrhi::ShaderHandle          csDirect_;
    nvrhi::ComputePipelineHandle directPSO_;
    nvrhi::BindingLayoutHandle   directComputeLayout_;  // resolve 槽位表 + b3 + u0 + t13 TLAS
    nvrhi::BindingSetHandle      directComputeSet_;
    nvrhi::BufferHandle          directConstants_;      // volatile CB 32B（b3：清屏色 + shadow 参数）
    std::uint32_t                directW_ = 0, directH_ = 0;  // set 绑定的尺寸版本
    // M15.1 拆双入口（拍板 B）：csDirectVsm（direct.csvsm，+b6/t14/t15）--vsm 专用；
    // 纯净 csDirect blob 与 s9 冻结基线 bytes 一致，VSM 查表隔离进本入口。
    nvrhi::ShaderHandle          csDirectVsm_;
    nvrhi::ComputePipelineHandle directVsmPSO_;
    nvrhi::BindingLayoutHandle   directVsmLayout_;      // 同上 + b6 + t14 + t15
    nvrhi::BindingSetHandle      directVsmSet_;
    std::uint32_t                directVsmW_ = 0, directVsmH_ = 0;  // vsm set 尺寸版本

    // ---- M15.0/M15.1（S15）：VSM 虚拟阴影贴图（--mode shadow 上 --vsm 开关）----
    // 物理页池 2048² R32F（128²/页 × 16×16 页）+ 1024 项页表（4 级 × 256，
    // StructuredBuffer<uint>，entry = poolPageX | poolPageY<<8，0xFFFFFFFF 未分配）
    // + 80B volatile CB（深度 pass b0，per-dirty-page 覆写 maxVersions 2048）
    // + 272B volatile CB（csDirectVsm b6 采样）+ 336B volatile CB（csMark b6 标页）。
    // M15.1（方案 A 拍板 2026-11-13，写回 S15-PLAN）：GPU compute 标页（吃
    // visBuffer_ deviceZ，UE PageMarking 对位）→ cullReadback_ 同款 2 帧延迟
    // readback 环 → CPU 分配器（空闲链表 + 跨帧页缓存 + LRU 驱逐 + 灯向变更好
    // 全失效）→ CPU 逐级写页表 + 逐 dirty 页下发 draw（clear quad 覆写 1.0 +
    // 全簇 pulling 几何，MIN 混合）。确定性保留（静态相机下帧序可复现，bytes
    // 基线纪律不破）。csDirect blob 对 t14/t15/b6 是静态引用 → layout/set 无条件
    // 扩槽；--vsm 关闭帧也写零版 272B b6 + 声明 pool/table 态（基线零扰动）。
    bool                         vsmReady_ = false;     // 懒建成功标记
    bool                         vsmFailed_ = false;    // 懒建失败只报一次
    bool                         vsmLogged_ = false;    // 就绪日志一次性
    nvrhi::TextureHandle         vsmPagePool_;          // 2048² R32F（16×16 物理页）
    nvrhi::BufferHandle          vsmPageTable_;         // StructuredBuffer<uint> 1024 项
    nvrhi::FramebufferHandle     vsmFb_;                // 色 attachment = 页池
    nvrhi::BufferHandle          vsmConstants_;         // volatile CB 80B（深度 pass b0）
    nvrhi::BufferHandle          vsmSampleConstants_;   // volatile CB 272B（csDirect b6）
    nvrhi::GraphicsPipelineHandle pipelineVsm_;         // vsm.vs/ps（全簇 pulling 深度）
    nvrhi::GraphicsPipelineHandle pipelineVsmClear_;     // vsm.vsclear/psclear（清页 quad，blend 关）
    nvrhi::BindingLayoutHandle   vsmDepthLayout_;       // b0 + t1/t2/t3
    nvrhi::BindingSetHandle      vsmDepthSet_;
    // ---- M15.1 标页（csMark）----
    nvrhi::ComputePipelineHandle vsmMarkPSO_;
    nvrhi::BindingLayoutHandle   vsmMarkLayout_;        // t0 visBuffer_ + u0 位图 + b6
    nvrhi::BindingSetHandle      vsmMarkSet_;
    std::uint32_t                vsmMarkW_ = 0, vsmMarkH_ = 0;   // set 的 visBuffer_ 尺寸版本
    nvrhi::BufferHandle          vsmMarkConstants_;     // volatile CB 336B（csMark b6）
    nvrhi::BufferHandle          vsmMarkBits_;          // RWStructured<uint> 32 word（4 级位图）
    nvrhi::BufferHandle          vsmReadback_[2];       // 128B 回读环形（cullReadback_ 同款）
    std::uint32_t                vsmSubmitted_ = 0;     // 已提交标页帧数
    // 槽位配对的窗口原点（世界页坐标）——纯 CPU 值，随 copyBuffer 同帧登记到
    // CPU 环形（GPU 只回传 128B 位图；若把 160B 一起 copy 会越过 128B 位图
    // buffer 越界读 → GPU page fault 实拍踩坑）。
    int                          vsmSlotOrigin_[2][kVsmLevels][2] = {};
    // ---- M15.2 间接阴影剔除（拍板 B 2026-11-14：两阶段 HZB 本里程碑落地）----
    // 簇球 × dirty 页光源投影判交（UE CullPerPageDrawCommands 窄化）+ 足迹自适应
    // HZB 遮挡（bevy sample_hzb 对位，min-only 近似洞登记 UE-DIFFS）→ 每页段式簇
    // 列表 + DrawIndirectArguments（startInstanceLocation 恒 0，双后端语义规避）。
    // HZB = 页池半分辨率 min 链 StructuredBuffer（上帧深度语义，bevy first-cull
    // 对位；MIN 混合下正确遮挡剔除深度无损）。vsMain 改实例列表取簇号（b0 z 段
    // 传 slot）。
    nvrhi::BufferHandle          vsmClusterSpheres_;    // StructuredBuffer<float4>（独立于 cull 模式）
    nvrhi::BufferHandle          vsmHzbBuf_;            // RWStructured<uint> 6553600 word（≈25MB）
    nvrhi::BufferHandle          vsmCullBufs_;          // RWStructured<uint> 68609 word
    nvrhi::BufferHandle          vsmCullArgs_;          // isDrawIndirectArgs 1024×16B
    nvrhi::BufferHandle          vsmDirtyList_;         // StructuredBuffer<uint4> 1024
    nvrhi::BufferHandle          vsmCullConstants_;     // volatile CB 288B（b1）
    nvrhi::ComputePipelineHandle vsmHzbPSO_;
    nvrhi::ComputePipelineHandle vsmCullCountPSO_;
    nvrhi::ComputePipelineHandle vsmCullScanPSO_;
    nvrhi::ComputePipelineHandle vsmCullFillPSO_;
    nvrhi::BindingLayoutHandle   vsmHzbLayout_;         // b1 + t6 页池 + u1 hzb
    nvrhi::BindingSetHandle      vsmHzbSet_;
    nvrhi::BindingLayoutHandle   vsmCullLayout_;        // b1 + t4/t5/t7 + u2/u3
    nvrhi::BindingSetHandle      vsmCullSet_;
    bool                         vsmCullLogged_ = false;  // 就绪日志一次性

    // ---- M15.1 CPU 分配器状态（跨帧页缓存；方案 A 核心）----
    struct VsmPageCacheEntry { std::uint32_t poolPage; std::uint32_t lastUsed; };
    // key = level<<62 | (uint32(wpx)&0x3FFFFFFF)<<32 | uint32(wpy)。wx 必须掩掉
    // 最高 2 位：uint32 回转的负 wx（0xFFFFFFxx）最高两位是 11，不掩会 OR 进
    // 级位段把 l<<62 完全吞掉——四级同 (wx,wy) 的 key 相同 → 跨级共池页 +
    // dirty 只画一次 → 页表多级槽指向错误尺度内容（M15.1 漏阴影根因实拍）。
    // wy 在低 32 位不接触级位段，无需掩码。
    std::unordered_map<std::uint64_t, VsmPageCacheEntry> vsmPageCache_;
    std::vector<std::uint32_t>   vsmFreePages_;          // 空闲物理页线性号 0..255
    // dirty 页 = 池线性号 + 窗口相对页号 (px,py)——per-dirty-page draw 需要本页
    // 覆盖窗口 NDC 的哪个子区，把窗口子区映射进 128² 页（shader remap；放大
    // viewport 方案在 VK 后端产生确定性页丢失，已废弃；见 M15.1 踩坑）。
    struct VsmDirtyPage { std::uint32_t poolPage; std::uint32_t px; std::uint32_t py; };
    std::vector<VsmDirtyPage>    vsmDirtyPages_[kVsmLevels];  // 本帧需渲染页
    float                        vsmTexel_[kVsmLevels] = {};    // 每级 texel 世界尺寸（init 定死）
    float                        vsmLevelMvp_[kVsmLevels][16];  // 每帧预算（HUD/表同步用）
    float                        vsmLastLightDir_[3] = {0.f, 0.f, 0.f};
    bool                         vsmLightInit_ = false;
    std::uint32_t                vsmFrame_ = 0;
    std::uint32_t                vsmEvictions_ = 0;
    VsmDebugState                vsmDebug_;
    float                        vsmAabbMin_[3] = {0.f, 0.f, 0.f};   // 场景 AABB（init 时算一次）
    float                        vsmAabbMax_[3] = {0.f, 0.f, 0.f};
    bool ensureVsmTargets(nvrhi::IDevice* device, const char* nmeshFile,
                          const char* shaderSuffix, std::string& outError);   // initShadow 尾部调用
    void runVsmDepthPass(nvrhi::ICommandList* cmd, const FrameParams& params);   // M15.1 整段（含 CPU 分配器）



    // ---- M9.2：reference 模式（离线参考，渐进式多帧累积直光真值）----
    // 与 shadow 互斥（一个 Scene 只跑一种）。复用面：输出 = rtTarget_ /
    // 呈现 = initRtPresent 的 vsRtPresent_ + 本模式的 psRefPresent_；几何与材质资源
    // = initVisbuf(pulling)。不新建任何独立 GBuffer 纹理（Q4 方案 A）。
    bool                         reference_ = false;
    bool                         refFailed_ = false;          // 懒建失败只报一次
    bool                         refNoTlasLogged_ = false;    // TLAS 未就绪的一次性日志
    std::uint32_t                samplesPerFrame_ = 64;      // 每帧每像素射线条数（设计常量）
    std::uint32_t                samplesDone_ = 0;            // 已累积样本总数
    std::uint32_t                refFramesTarget_ = 0;        // viewer 传来的总帧数（进度分母）
    float                        refProgress_ = 0.0f;          // 进度 0..1（日志/HUD）
    std::uint32_t                refW_ = 0, refH_ = 0;        // 计算 set 尺寸版本
    nvrhi::ShaderHandle          csReference_;               // direct.csref（compute）
    nvrhi::ShaderHandle          csReferenceGI_;             // direct.csrefgi（M10.3 GI 参考）
    nvrhi::ShaderHandle          psRefPresent_;              // rt.psrefpresent（fragment）
    nvrhi::ComputePipelineHandle refPSO_;
    nvrhi::ComputePipelineHandle refGIPSO_;                  // M10.3 GI 参考 compute 管线
    nvrhi::GraphicsPipelineHandle refPresentPSO_;            // 随 framebuffer 信息建
    nvrhi::BindingLayoutHandle   refComputeLayout_;          // resolve + b3 + b4 + u0 + t13 TLAS
    nvrhi::BindingSetHandle      refComputeSet_;
    nvrhi::BindingLayoutHandle   refPresentLayout_;          // t1 SRV + b1 RefConstants
    nvrhi::BindingSetHandle      refPresentSet_;             // ensureRtTargets 建
    nvrhi::BufferHandle          refConstants_;              // volatile CB 16B（b4/b1 累积参数；
                                                             //  ⚠️ 跨 compute/graphics 绑定扩 32B
                                                             //  device removed，M10.3 二分实锤）
    nvrhi::BufferHandle          refGICB_;                   // volatile CB 16B（b5 compute-only，
                                                             //  M10.3 GI 常量 albedo/ambient）

    // ---- M10.3：--compare 右半参考图（Q14-2）----
    nvrhi::TextureHandle         compareTex_;                // stb_image 解码 RGBA8
    nvrhi::ShaderHandle          psCompare_;                 // rt.pscompare（fragment）
    nvrhi::SamplerHandle         compareSampler_;            // 独立懒建（不依赖各 init 链）
    nvrhi::GraphicsPipelineHandle comparePSO_;               // 随 framebuffer 信息建
    nvrhi::BindingLayoutHandle   compareLayout_;             // t0 SRV + s0 sampler
    nvrhi::BindingSetHandle      compareSet_;
    bool                         compareFailed_ = false;     // 懒建失败只报一次
    bool                         comparePendingUpload_ = false; // 建纹无 cmd list → 帧内
                                                            //  writeTexture+setTextureState
                                                            //  同列表（0x87d 第一坑同款纪律）
    std::vector<std::uint8_t>    comparePixels_;             // CPU 侧像素（首帧上传后清）
    std::uint32_t                compareW_ = 0, compareH_ = 0;

    // ---- M10.1：probe 探针追踪（Q12-1/Q12-2/Q12-3 拍板 UE 口径）----
    // 每探针每八面体 texel 1 条方向射线（8×8 core + 1 texel border = 10×10 存储
    // texel/探针）；miss → skyGradient 解析天穹；hit → 命中点直光（M9.1 口径
    // 降级备案：统一 albedo + 无 NdotL，命中点朝主灯 1 条 shadow ray）。
    // irradiance atlas：每级一张 RGBA16F（res_l·10 × res_l·10），M10.1 无时间
    // 混合（单份不 ping-pong，降噪归 M10.3/S14）。
    nvrhi::ShaderHandle          csTrace_;               // probe_trace.cstrace
    nvrhi::BufferHandle          traceCB_;               // volatile 64B（灯 + 命中材质 + 参数）
    nvrhi::BindingLayoutHandle   traceLayout_;           // b0 + t0..t3 atlas SRV + t4 TLAS + u0..u3
    nvrhi::BindingSetHandle      traceSet_[2];           // 每 parity 一份（绑 cur place atlas）
    nvrhi::ComputePipelineHandle tracePSO_;
    nvrhi::TextureHandle         irradAtlas_[4];         // RGBA16F（探针格 × 10×10）
    bool                         probeTraceLogged_ = false; // 追踪日志锚只打一次

    // ---- M10.2：final gather（Q13-1/Q13-2/Q13-3 拍板 UE 口径）----
    // csGather 每像素深度选级 + UE 双线性 corner × 平面深度权重 + Fallback 降级
    // （纯采样零射线）；csGatherCompose 同纹理 RMW hdrSceneColor_（SRV 读 +
    // UAV 写，每线程只碰自己 texel）——免 rtTarget_ 中转，presentTonemap 现成
    // 链路直通。两入口共用同一 BindingSet（§2.7）。
    nvrhi::ShaderHandle          csGather_;              // probe_gather.csgather
    nvrhi::ShaderHandle          csGatherCompose_;       // probe_gather.csgathercompose
    nvrhi::BufferHandle          gatherCB_;              // volatile 128B（invViewProj+eye+params+mat+levels）
    nvrhi::BindingLayoutHandle   gatherLayout_;          // b0 + t0..t9 SRV + u0..u1 UAV
    nvrhi::BindingSetHandle      gatherSet_[2];          // 每 parity 一份（绑 cur place atlas）
    nvrhi::ComputePipelineHandle gatherPSO_;
    nvrhi::ComputePipelineHandle gatherComposePSO_;
    nvrhi::TextureHandle         gatherLinear_;          // RGBA16F 全屏（每像素间接光）
    bool                         probeGatherLogged_ = false; // gather 日志锚只打一次
    bool                         probeGatherFirst_ = true;   // gatherLinear_ 帧首声明分支
                                                             //（首帧=创建态 SRV/后续帧=UAV 末态）

    // ---- S12 M12.0：GI 分量独立时域累积（Q12-A 方案 A，零新增 RT）----
    // 复用 T0 history0_/1_ ping-pong（rgb 累积 GI / a 帧数）与 probe 链 velocity_；
    // gatherLinear_ 原地替换为累积结果，compose 完全不感知。
    nvrhi::ShaderHandle          csFilterTemporal_;          // filter_temporal.csfiltertemporal
    nvrhi::BufferHandle          filterCB_;                  // volatile 64B（声明 32B，过量分配见 init）
    nvrhi::BindingLayoutHandle   filterLayout_;              // b0 + t0..t1 SRV + u0..u1 UAV
    nvrhi::BindingSetHandle      filterSet_[2];              // 每 t0Parity_ 一份（read/write 交换）
    nvrhi::ComputePipelineHandle filterPSO_;
    bool                         temporalHistoryValid_ = false; // 首帧历史未初始化 → shader 不读
    bool                         filterLogged_ = false;         // 一次性日志锚

    // ---- S12 M12.1：空间联合双边（Q12-C A，零新增 RT / CB / GBuffer 资源）----
    // b0 = filterCB_（复用）、b1 = gatherCB_（复用前 80B）、t0 = history[write]、
    // t1 = probeDepth_、u0 = gatherLinear_。set 索引 = 本帧 temporal write 侧。
    nvrhi::ShaderHandle          csFilterSpatial_;          // filter_spatial.csfilterspatial
    nvrhi::BindingLayoutHandle   filterSpatialLayout_;
    nvrhi::BindingSetHandle      filterSpatialSet_[2];
    nvrhi::ComputePipelineHandle filterSpatialPSO_;
    nvrhi::TextureHandle         filterHistoryCur_;         // 本帧 temporal write 侧
    bool                         filterSpatialLogged_ = false;

    // ---- M10.0：probe-place 资源（Q10-1/Q10-2 拍板 UE 口径）----
    // velocity prepass：独立小 pass（vsVelocity/psVelocity）自建 D32 深度
    //（probeDepth_，velocity 写入 + csPlace SRV 读——共享 backend depth_ 是
    // keepInitialState 永驻态不能切 SRV，故 probe 模式自建，UE velocity pass
    // 自带 depth 的口径）；velocity_ 沿用 T0 已建的 RG16F 纹理。
    // cascade atlas：每级一张 RGBA32_FLOAT（rgb=探针世界位置+a=deviceZ）× 2 帧
    // ping-pong（[parity][level]），上一帧探针 = parity 翻转后的另一份（Q10-2）。
    bool                         probe_ = false;
    bool                         probeLogged_ = false;   // 布点日志锚只打一次
    nvrhi::IDevice*              probeDevice_ = nullptr;
    nvrhi::ShaderHandle          vsVelocity_, psVelocity_;
    nvrhi::ShaderHandle          csPlace_;
    nvrhi::ShaderHandle          vsProbeDots_, psProbeDots_;
    nvrhi::BufferHandle          velocityCB_;            // volatile 144B（cur/prev viewProj + 尺寸）
    nvrhi::BufferHandle          probeCB_;               // volatile 176B（布点+viz 共用一份）
    nvrhi::BindingLayoutHandle   velocityLayout_;        // b0 + t1..t4（pulling 池）
    nvrhi::BindingSetHandle      velocitySet_;
    nvrhi::BindingLayoutHandle   placeLayout_;           // b0 + t0/t1 SRV + u0..u4 UAV
    nvrhi::BindingSetHandle      placeSet_[2];           // 每 parity 一份（atlas 绑定不同）
    nvrhi::BindingLayoutHandle   dotsLayout_;            // b0 + t0..t4 SRV
    nvrhi::BindingSetHandle      dotsSet_[2];
    nvrhi::GraphicsPipelineHandle velocityPSO_;          // velocityFb_（RG16F+D32）格式
    nvrhi::ComputePipelineHandle placePSO_;
    nvrhi::GraphicsPipelineHandle dotsPSO_;              // swapchain 格式（懒建，深度关）
    nvrhi::TextureHandle         probeDepth_;            // D32（非 permanent，逐帧 tracking）
    nvrhi::FramebufferHandle     velocityFb_;            // velocity_ + probeDepth_
    nvrhi::TextureHandle         probeAtlas_[2][4];      // [parity][level] RGBA32F
    nvrhi::TextureHandle         probePrevDelta_;        // RGBA32F baseRes²（prevUV 增量）
    std::uint32_t                probeParity_ = 0;       // atlas ping-pong 帧奇偶
    std::uint32_t                probeW_ = 0, probeH_ = 0, probeBaseRes_ = 0;
    bool                         probeFirstFrame_ = true; // 首帧 prevViewProj = 本帧（vel=0）
    float                        probePrevViewProj_[16] = {}; // 上一帧 viewProj

    // ---- S11 M11.0：世界缓存（flat hashmap，Q11-0 方案 A）----
    // 常量与 world_cache.slang / gi-tools WorldCacheSim 三方逐字同口径：
    // kCacheCapacity=1<<18 / lifetime=10 / cellSize=0.5m / 探测上限 3。
    static constexpr std::uint32_t kWcCapacity = 1u << 18;
    static constexpr std::uint32_t kWcLifetime = 10;
    // M11.1：csCacheUpdate（DI shadow ray + 邻居射线查缓存）/ csCacheBlend（时域
    // 混合 cap 64）—— cell 更新 = 多跳反馈的机制本体（S11-PLAN M11.1）。
    nvrhi::ShaderHandle          csCacheFill_, csCacheDecay_, csCacheViz_;
    nvrhi::ShaderHandle          csCacheUpdate_, csCacheBlend_;
    nvrhi::ShaderHandle          csIrradDump_;          // M11.2 验收：atlas 直出
    nvrhi::BufferHandle          cacheCB_;               // volatile 160B（M11.1 扩）
    nvrhi::BufferHandle          wcChecksums_, wcLife_;  // 4B×2^18 each
    nvrhi::BufferHandle          wcGeoPos_, wcGeoNrm_;   // 16B×2^18 each
    nvrhi::BufferHandle          wcRadiance_, wcNewRadiance_; // M11.1：16B×2^18 each
    nvrhi::BindingLayoutHandle   cacheFillLayout_, cacheVizLayout_;
    nvrhi::BindingLayoutHandle   cacheUpdateLayout_, cacheBlendLayout_;
    nvrhi::BindingLayoutHandle   irradDumpLayout_;       // M11.2 验收
    nvrhi::BindingSetHandle      cacheFillSet_, cacheVizSet_;
    nvrhi::BindingSetHandle      cacheUpdateSet_, cacheBlendSet_;
    nvrhi::BindingSetHandle      irradDumpSet_;          // M11.2 验收
    nvrhi::ComputePipelineHandle cacheFillPSO_, cacheDecayPSO_, cacheVizPSO_;
    nvrhi::ComputePipelineHandle cacheUpdatePSO_, cacheBlendPSO_;
    nvrhi::ComputePipelineHandle irradDumpPSO_;          // M11.2 验收
    nvrhi::TextureHandle         cacheFillSetDepth_;     // set 依赖登记（probeDepth_ 重建则 set 重建）
    nvrhi::TextureHandle         cacheVizSetHdr_;        // set 依赖登记（hdrSceneColor_ 重建则 set 重建）
    nvrhi::TextureHandle         irradDumpSetHdr_;       // M11.2 验收 set 依赖登记
    nvrhi::BindingSetHandle      giDumpSet_;             // S12 验收（复用 irradDumpLayout_/PSO_）
    nvrhi::TextureHandle         giDumpSrc_, giDumpHdr_; // S12 验收 set 依赖登记
    std::uint32_t                cacheFrame_ = 0;        // 帧号（r2 抖动种子，确定性）
    std::uint32_t                s12Frame_   = 0;        // S12 Q12-E：csTraceProbe 逐帧方向抖动种子（每帧 +1，取模 64 用）
    bool                         cacheFirstClear_ = true;// 首帧清零表（uninit 内存防假命中）
    bool                         cacheLogged_ = false;   // cache 日志锚只打一次
    bool                         cacheVizLogged_ = false;// viz compose 日志锚只打一次

    // ---- M-WAT0（water 轨）：SSR 本体资源（懒建，详见 scene.cpp ensureSsr*）----
    // ssrDistPyramid_ = 径向距离 **min** 金字塔（R32F POT 全 mip；度量同 S6 hiz
    // 线性径向距离，算子与 S6 max 相反——SSR 跳步要「区域最近表面」，见
    // ssr.slang 文件头 §2）。ssrSceneMip_ = 场景色均值链（RGBA32F POT）。
    // ssrColor_ = march 输出（rgba：反射辐射 + 权重）。
    nvrhi::TextureHandle         ssrDistPyramid_;
    nvrhi::TextureHandle         ssrSceneMip_;
    nvrhi::TextureHandle         ssrColor_;
    nvrhi::TextureHandle         waterBg_;               // M-WAT2：折射背景副本（hdr 同格式同尺寸）
    std::uint32_t                ssrW_ = 0, ssrH_ = 0;   // 懒建判据（视口尺寸）
    std::uint32_t                ssrPyrMips_ = 0;        // 金字塔 mip 数（POT 链长）
    static constexpr std::uint32_t kSsrChainMips = 6;    // 场景色链 mip 数（POT/32）
    nvrhi::ShaderHandle          csSsrDepth_, csSsrDown_, csSsrMip_, csSsr_, csSsrCompose_;
    // M-WAT2（拍板 Q-WA7=A）：折射背景 copy + 水体全链合成（ssr.slang 新入口）。
    nvrhi::ShaderHandle          csWaterCopy_, csWaterComposite_;
    nvrhi::BindingLayoutHandle   ssrDepthLayout_, ssrDownLayout_, ssrMipLayout_,
                                 ssrLayout_, ssrComposeLayout_;
    nvrhi::BindingLayoutHandle   waterCopyLayout_, waterCompositeLayout_;
    nvrhi::ComputePipelineHandle ssrDepthPSO_, ssrDownPSO_, ssrMipPSO_, ssrPSO_, ssrComposePSO_;
    nvrhi::ComputePipelineHandle waterCopyPSO_, waterCompositePSO_;
    nvrhi::BindingSetHandle      ssrDepthSet_, ssrSet_, ssrComposeSet_;
    nvrhi::BindingSetHandle      waterCompositeSet_;
    nvrhi::TextureHandle         waterCompositeSetBg_;   // composite set 依赖登记（bg 重建则 set 重建）
    nvrhi::BindingSetHandle      waterCopySet_;          // M-WAT2 copy set（hdr 重建则 set 重建）
    nvrhi::TextureHandle         waterCopySetHdr_;       // copy set 依赖登记
    bool                         waterCompositeLogged_ = false;  // composite 首跑日志锚
    nvrhi::TextureHandle         ssrComposeSetHdr_;      // compose set 依赖登记（hdr 重建则 set 重建）
    std::vector<nvrhi::BindingSetHandle> ssrDownSets_;   // [L≥1] 金字塔逐级 set
    std::vector<nvrhi::BindingSetHandle> ssrMipSets_;    // [L≥1] 场景色链逐级 set
    nvrhi::BufferHandle          ssrDepthCB_;            // volatile 96B（SsrDepthConstants b4）
    nvrhi::BufferHandle          ssrDownCB_;             // volatile 16B（SsrDownConstants b5，逐级覆写）
    nvrhi::BufferHandle          ssrConstantsCB_;        // volatile 208B（SsrConstants b3）
    nvrhi::BufferHandle          ssrComposeCB_;          // volatile 16B（compose dims b5）
    bool                         ssrFailed_ = false;     // 懒建失败只报一次，后续帧跳过
    bool                         ssrLogged_ = false;     // SSR 首跑日志锚
};

} // namespace nl::renderer::detail
