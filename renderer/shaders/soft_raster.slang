// M14A1（S14-A）：compute 软光栅——全簇可见列表逐簇逐三角填同一块 u64 vis buffer。
// 对照模式（Q-A1 = A1-A 拍板）：不做 SW/HW 分流（M14A2 才加），dispatch = 总簇数、
// 1 簇 = 1 workgroup，与硬件光栅路径产出**完全同一块** visBuffer_，viz/resolve 链零改动。
//
// 三处参考对照（S14A-PLAN §1，2026-11-13 实读）：
//   bevy visibility_buffer_software_raster.wesl = 主参考：1 簇 1 workgroup（128
//   线程，每线程 1 三角）、edge function + 梯度增量式、宽三角 scanline / 窄三角
//   bbox 双路、u64 原子写 vis（bevy textureAtomicMax max=最近；本项目 deviceZ
//   near=0 → visPack + InterlockedMin 语义直换，打包/清空口径全在
//   visbuf_common.slangh 唯一权威）。bevy 的 workgroup shared 顶点缓存省带宽，
//   但 clusterMeta 只有 [vertexBase, triCount, localBase, materialId] 无顶点数，
//   A1 正确性优先 = 每线程直接拉自己三角的 3 个顶点（簇 ≤128 三角，顶点池
//   访问天然聚簇，缓存友好）；scanline 双路与定点数/top-left（bevy TODO）留
//   M14A3 profile 档。
//   UE NaniteRasterBinning.usf = 分流层位口径：bSoftware 在 binning/candidates
//   层定、vis 编码统一 resolve 无感——本文件编码与 visbuf.slang 逐位同源，
//   硬件路径一行未动（M11.1 冻结基线纪律）。
//   Carrot = 无软光栅等价（S14A-PLAN §1 据实标注）。
//
// Resource registers（docs/SHADER-CONVENTIONS.md §2.7：每管线单一 BindingSet，
// 与 visbuf.slang raster 段同槽位——软硬两路消费同一张簇表）：
//   b0 = SceneConstants (160B, 只用到 g_mvp)
//   b1 = VisConstants (16B): x=width y=height z/vizMode 软光栅不读 w=unused
//   t1..t4 = vertexPool / localIndices / clusterMeta / visibleList
//   u0 = vis buffer UAV（atomicMin 写）
// SPIR-V 绑定平移：b→256, t→0, s→128, u→384；DXIL 用各自 slot。
//
// 64 位原子 DXIL 要求 SM 6.6（CMake 把本产物加进 _SM66_ARTIFACTS，同 visbuf）；
// Vulkan 侧 VK_KHR_shader_atomic_int64 已在 createDevice 全局启用。

#include "visbuf_common.slangh"

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;
    float4x4 g_model;
    float4   g_lightDir;
    float4   g_options;
};

cbuffer VisConstants : register(b1)
{
    float4 g_visParams;   // (width, height, vizMode, unused)
};

StructuredBuffer<float> g_vertexPool   : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);
StructuredBuffer<uint>  g_visibleList  : register(t4);

RWStructuredBuffer<uint64_t> g_visUAV : register(u0);

float edgeFunction(float2 a, float2 b, float2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

float min3(float a, float b, float c) { return min(a, min(b, c)); }
float max3(float a, float b, float c) { return max(a, max(b, c)); }

// NDC → 像素坐标：probe_viz.slang::ndcToPixel 同式（y 翻转，双后端共享一套
// ZO 投影矩阵，deviceZ = clip.z/clip.w ∈ [0,1] 与 SV_Position.z 逐位同源）。
float2 ndcToPixel(float2 ndc, float2 size)
{
    return float2((ndc.x * 0.5f + 0.5f) * size.x,
                  (0.5f - 0.5f * ndc.y) * size.y);
}

struct RasterVert
{
    float2 px;    // viewport 像素坐标
    float  z;     // deviceZ ∈ [0,1]（visPack 直接消费）
};

RasterVert pullVertex(uint clusterId, uint localIdx)
{
    RasterVert v;
    v.px = float2(0.0f, 0.0f);
    v.z  = 1.0f;

    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];

    const uint gv = vertexBase + localIdx;
    const uint b  = gv * 8u;                     // 8 float / 顶点（pos+nrm+uv，同 model.slang）
    const float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);

    const float4 clip = mul(g_mvp, float4(pos, 1.0f));
    if (clip.w <= 0.0f) return v;                // 相机后方整三角丢弃（ Padding/越界同款防御）

    const float2 ndc = clip.xy / clip.w;
    v.px = ndcToPixel(ndc, g_visParams.xy);
    v.z  = clip.z / clip.w;                      // ZO 投影直接是 deviceZ
    return v;
}

[shader("compute")]
[numthreads(128, 1, 1)]
void csSoftRaster(uint3 groupId : SV_GroupID, uint gtid : SV_GroupIndex)
{
    const uint clusterId = g_visibleList[groupId.x];
    const uint metaBase  = clusterId * 4u;
    const uint triCount  = g_clusterMeta[metaBase + 1u];

    const uint triId = gtid;
    if (triId >= triCount) return;

    const uint idxBase = g_clusterMeta[metaBase + 2u] + triId * 3u;
    const RasterVert v0 = pullVertex(clusterId, g_localIndices[idxBase + 0u]);
    const RasterVert v1 = pullVertex(clusterId, g_localIndices[idxBase + 1u]);
    const RasterVert v2 = pullVertex(clusterId, g_localIndices[idxBase + 2u]);
    // 背面剔除（bevy 同款 edge function 面积符号；网格卷绕与硬件路径一致）
    const float area = edgeFunction(v0.px, v1.px, v2.px);
    if (area <= 0.0f) return;

    // 像素包围盒裁到视口（bevy 同款 floor/ceil 裁剪）
    const float2 size = g_visParams.xy;
    const uint2 minP = uint2(max(floor(min3(v0.px.x, v1.px.x, v2.px.x)), 0.0f),
                             max(floor(min3(v0.px.y, v1.px.y, v2.px.y)), 0.0f));
    const uint2 maxP = uint2(min(ceil(max3(v0.px.x, v1.px.x, v2.px.x)), size.x - 1.0f),
                             min(ceil(max3(v0.px.y, v1.px.y, v2.px.y)), size.y - 1.0f));

    // 梯度增量式（bevy 同款）：w = 三条边方程值，z = 重心插值 deviceZ。
    const float3 wRow0 = float3(edgeFunction(v1.px, v2.px, float2(minP) + 0.5f),
                                edgeFunction(v2.px, v0.px, float2(minP) + 0.5f),
                                edgeFunction(v0.px, v1.px, float2(minP) + 0.5f));
    const float3 wdx = float3(v1.px.y - v2.px.y, v2.px.y - v0.px.y, v0.px.y - v1.px.y);
    const float3 wdy = float3(v2.px.x - v1.px.x, v0.px.x - v2.px.x, v1.px.x - v0.px.x);
    const float3 zRow0 = float3(v0.z, v1.z, v2.z) / area;
    const float zdx = dot(zRow0, wdx);
    const float zdy = dot(zRow0, wdy);

    const uint width = uint(size.x);
    const uint64_t ids = (uint64_t(clusterId) << VIS_TRI_BITS) | uint64_t(triId & 255u);

    float3 w = wRow0;
    float  z = dot(zRow0, wRow0);
    for (uint y = minP.y; y <= maxP.y; ++y)
    {
        float3 wx = w;
        float  zx = z;
        for (uint x = minP.x; x <= maxP.x; ++x)
        {
            // 三角内判定（bevy 实测必要：浮点累计在 bbox 角落会漏判）
            if (min3(wx[0], wx[1], wx[2]) >= 0.0f)
            {
                // 深度占高位 + atomicMin：与硬件路径同一打包，最近者胜。
                const uint64_t packed = (uint64_t(asuint(zx)) << 32u) | ids;
                InterlockedMin(g_visUAV[visIndex(uint2(x, y), width)], packed);
            }
            wx += wdx;
            zx += zdx;
        }
        w += wdy;
        z += zdy;
    }
}
