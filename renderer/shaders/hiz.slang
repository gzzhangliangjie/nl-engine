// M6.0 (S6.0): Hi-Z depth pre-pass + hand-rolled max-mipmap pyramid.
//
// S6-PLAN M6.0: "第一遍只写深度（无颜色），输出当前 cut 全部可见簇的 depth
// buffer；compute 手写 min-mipmap——不能用 GenerateMips（它是均值滤波，
// 遮挡剔除要纯 min）"。
// M6.1 拍板修正（2026-11-09，UE/bevy 权威口径）：遮挡判定 = 「簇最近点 vs
// 区域最远表面」，金字塔必须逐级取 **max**（每 texel 最远表面径向距离）。
// S6-PLAN 原文的 min 口径（"clusterMin > hizMin 剔除"）在径向距离度量下
// 不保守会漏画：texel 同时盖近墙(3m)与远景(100m)时 min=3，簇最近 50m>3
// 被误剔。参照：UE NaniteHZBCull.ush IsVisibleHZB（inverted-Z 取 min =
// 最远表面）、bevy downsample_depth.wesl（reverse-Z 取 min = 最远表面）、
// cull_shared.wesl occlusion_cull_screen_aabb（aabb.max.z=簇最近点 vs HZB）。
//
// 结构（与 bevy cull_bvh.wesl / UE NaniteCullRaster 的 HZB 前置一致）：
//   1) vsHizDepth / psHizDepth —— 深度预通道：绘制 S5 DAG 遍历输出的可见簇
//      （复用 M4 indirect args + 可见列表接口，S5-PLAN 底线「不另起炉灶」），
//      颜色 RT = 金字塔 mip0（R32F），深度附件做硬件 less 测试保证每像素
//      存最近表面。PS 输出 = 视点径向距离 length(worldPos - eye)，与 M5.0
//      屏幕误差公式的 d = distance(eye, center) - radius 同一度量口径
//      （S6-PLAN 坑提示：盒测试比较空间必须一致——全链统一径向距离）。
//   2) csHizDown —— 逐级 2x2 取 max 降采样（odd 边界 clamp 读，保守方向正确：
//      视口外清屏 1e9 参与 max → 边界 texel 只会更大 → 剔除更难成立 → 宁粗勿漏）。
//      SRV 绑 L-1 单 mip 子资源（Load 的 LOD 相对视图基 mip），UAV 绑 L。
//   3) csHizTop —— 最高 mip 恒 1 texel = 全屏最近距离；写进 4B 计数器供
//      CPU 延迟回读（M5.2 回读环形同款），作为「金字塔最高层 ≈ 整屏最近
//      深度」的机械验收证据。
//   4) vsHizViz / psHizViz —— 右下角 160x160 覆盖层把任意 mip 画成灰度
//      （近亮远暗，S6-PLAN 层级可视化自检；数据不动，只读）。
//
// Resource registers (docs/SHADER-CONVENTIONS.md):
//   b0 = SceneConstants（mvp，深度 pass 与 model.slang 共用 160B 布局）
//   b1 = HizConstants (48B volatile):
//        [0] = (eye.xyz, farRange)          —— farRange 仅可视化对比度用
//        [1] = (viewMipLevel, 0, 0, 0)      —— 可视化 / top 读取的 mip 号
//        [2] = (srcW, srcH, tgtW, tgtH)     —— 逐级 dispatch 下发（每级一次）
//   t0 = 深度金字塔（R32F，全 mip SRV 或 L-1 单 mip SRV 按绑定集而异）
//   t1..t4 = vertexPool / localIndices / clusterMeta / visibleList（与 model.slang 相同）
//   u0 = csHizDown: RWTexture2D<float> 目标 mip；csHizTop: RWStructuredBuffer<uint>
//
// Vulkan 端 slangc -fvk-b-shift 256 / -fvk-t-shift 0 / -fvk-s-shift 128 /
// -fvk-u-shift 384 与 NVRHI 默认 VulkanBindingOffsets 严格对应（M4.1 同款）。

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;
    float4x4 g_model;
    float4   g_lightDir;
    float4   g_options;
};

cbuffer HizConstants : register(b1)
{
    float4 g_hizEye;    // xyz = eye world pos, w = farRange（仅可视化对比度）
    float4 g_hizViz;    // x = viewMipLevel；viz 路径 y/z = 视口宽高（vsHizViz/psHizViz 读）
    float4 g_hizDims;   // csHizDown 专用: (srcW, srcH, tgtW, tgtH)
};

Texture2D<float>         g_hizPyramid : register(t0);

StructuredBuffer<float> g_vertexPool  : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);
StructuredBuffer<uint>  g_visibleList  : register(t4);

// ============================================================================
// 1) 深度预通道：PS 输出视点径向距离。硬件深度测试（D32 附件）保证每像素
//    mip0 里留的是最近表面——这就是「当前 cut 全部可见簇的 depth buffer」，
//    只是单位换成了线性径向距离（后面全链 min/比较都在这个空间）。
// ============================================================================

struct VSDepthOutput
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
};

// 与 model.slang vsCull 同一套 vertex pulling（identity 索引 + 可见列表间接
// 寻址），多输出一个插值世界坐标。刻意不并进 vsCull：vscull 字节码保持
// M4/M5 原样（基线冻结），hiz 模式专属入口。
VSDepthOutput vsHizDepth(uint instanceIndex : SV_InstanceID,
                         uint vertexIndex   : SV_VertexID)
{
    VSDepthOutput output;
    output.position = asfloat(0x7fc00000u).xxxx;   // NaN -> 整三角被裁剪（dummy）
    output.worldPos = 0.0f;

    const uint clusterId  = g_visibleList[instanceIndex];
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint triCount   = g_clusterMeta[metaBase + 1u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];

    const uint triInCluster = vertexIndex / 3u;
    const uint vertInTri    = vertexIndex % 3u;
    if (triInCluster >= triCount) return output;

    const uint localIdx = g_localIndices[localBase + vertexIndex];
    const uint gv = vertexBase + localIdx;
    const uint b  = gv * 8u;
    const float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);

    output.position = mul(g_mvp, float4(pos, 1.0f));
    output.worldPos = pos;
    return output;
}

struct PSDepthInput
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
};

float4 psHizDepth(PSDepthInput input) : SV_Target
{
    const float d = distance(input.worldPos, g_hizEye.xyz);
    return float4(d, d, d, 1.0f);
}

// ============================================================================
// 2) max 金字塔降采样（M6.1 拍板修正：遮挡判定口径 = 簇最近点 vs 区域最远
//    表面，见文件头）。每线程 1 目标 texel，2x2 取 max，越界 clamp
//    （奇数宽高层 clamp 保证边界 texel 是「覆盖区域内全部源 texel 的 max」；
//    视口外 1e9 参与 max 只会放大 → 剔除更难成立 → 保守方向正确）。
//    SRV 绑定 = mip(L-1) 单 mip 子资源，Load LOD 0 相对视图基 mip。
// ============================================================================

RWTexture2D<float> g_hizDst : register(u0);

[numthreads(8, 8, 1)]
void csHizDown(uint3 id : SV_DispatchThreadID)
{
    const uint2 tgt = uint2(g_hizDims.z, g_hizDims.w);
    if (id.x >= tgt.x || id.y >= tgt.y) return;

    const int  srcW = int(g_hizDims.x), srcH = int(g_hizDims.y);
    const int2 base = int2(2u * id.x, 2u * id.y);
    const int2 lo   = int2(min(base.x,     srcW - 1), min(base.y,     srcH - 1));
    const int2 hi   = int2(min(base.x + 1, srcW - 1), min(base.y + 1, srcH - 1));

    float v = g_hizPyramid.Load(int3(lo.x, lo.y, 0));
    v = max(v, g_hizPyramid.Load(int3(hi.x, lo.y, 0)));
    v = max(v, g_hizPyramid.Load(int3(lo.x, hi.y, 0)));
    v = max(v, g_hizPyramid.Load(int3(hi.x, hi.y, 0)));
    g_hizDst[uint2(id.x, id.y)] = v;
}

// ============================================================================
// 3) 最高 mip（1 texel）-> 4B 计数器。位型重解释 asuint 保留 float 位模式，
//    CPU 端 asfloat 还原（NaN 即 0xFFFFFFFF 清零值 = 尚无数据）。
//    M6.1 起 top 语义 = 全屏最远表面径向距离（金字塔方向 min→max 的连锁）。
// ============================================================================

RWStructuredBuffer<uint> g_hizTop : register(u0);

[numthreads(1, 1, 1)]
void csHizTop()
{
    // SRV 绑最高 mip 单 mip 子资源，Load LOD 0 相对视图基 mip（SHADER-CONVENTIONS §2.4）。
    g_hizTop[0] = asuint(g_hizPyramid.Load(int3(0, 0, 0)));
}

// ============================================================================
// 4) 右下角 160x160 覆盖层：把 viewMipLevel 级画成灰度（近亮远暗）。
//    只读金字塔，深度测试关，画在主场景之后（截图基线里可见 = 层级可视化
//    的验收证据；S6-PLAN「ImGui 加层级可视化」的 GPU 落点，配 HUD mip 滑条）。
// ============================================================================

struct VizOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VizOutput vsHizViz(uint vid : SV_VertexID)
{
    // g_hizViz.y/z 在可视化路径填 mip0 尺寸 = 视口尺寸。
    const float W = g_hizViz.y, H = g_hizViz.z;
    const float size = 160.0f, margin = 8.0f;
    const float px1 = W - margin, py1 = H - margin;
    const float px0 = px1 - size, py0 = py1 - size;
    const float px  = (vid & 1u) ? px1 : px0;
    const float py  = (vid & 2u) ? py1 : py0;

    VizOutput o;
    // 像素坐标(y 向下) -> NDC(y 向上)；uv 与 mip 纹理行序一致（行 0 = 顶部）。
    o.position = float4(2.0f * px / W - 1.0f, 1.0f - 2.0f * py / H, 0.0f, 1.0f);
    o.uv       = float2(vid & 1u, (vid & 2u) ? 1.0f : 0.0f);
    return o;
}

float4 psHizViz(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const uint mip = uint(g_hizViz.x);
    const uint2 dims = uint2(max(1u, uint(g_hizViz.y) >> mip),
                             max(1u, uint(g_hizViz.z) >> mip));
    const int2 tc = int2(min(int2(uv * float2(dims)), int2(dims) - 1));
    const float d = g_hizPyramid.Load(int3(tc, mip));
    const float b = 1.0f - clamp(d / max(g_hizEye.w, 1e-4f), 0.0f, 1.0f);
    return float4(b.xxx, 1.0f);
}
