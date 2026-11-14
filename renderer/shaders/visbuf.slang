// M7.0 (S7): visibility buffer — 簇光栅写 64 位 + 全屏解包可视化。
// 一份 .slang 双目标（SPIR-V + DXIL），四入口：
//   vsVisRaster / psVisRaster : GPU vertex pulling 画簇，PS 用 atomicMin 写 vis
//   vsVisViz    / psVisViz    : 全屏三角形读 vis → 簇色 / 三角色
//
// 打包格式与 clear 口径见 visbuf_common.slangh（唯一权威定义）。
// 三处参考对照：Carrot VisibilityBuffer.cpp（clear → rasterize → debug → material
// 四段编排）、bevy visibility_buffer_hardware_raster.wesl（u64 打包）、
// UE NaniteWritePixel.ush（64 位 image atomic）。本工程介质改为 u64 buffer
// （vendored NVRHI 无 R64 格式），偏离登记在 docs/s5/UE-DIFFS.md。
//
// Resource registers (docs/SHADER-CONVENTIONS.md §2.7：每管线单一 BindingSet)：
//   b0 = SceneConstants (160B, 只用到 g_mvp)
//   b1 = VisConstants (16B): x=width y=height z=vizMode(0 cluster/1 tri) w=unused
//   t0 = vis buffer SRV（viz pass 读）
//   t1..t4 = vertexPool / localIndices / clusterMeta / visibleList（与 model.slang 同）
//   u0 = vis buffer UAV（raster pass atomicMin 写）
// SPIR-V 绑定平移：b→256, t→0, s→128, u→384；DXIL 用各自 slot。
//
// 64 位原子在 DXIL 上要求 SM 6.6（CMake 只给本文件的 4 个产物加 -profile sm_6_6，
// 老 shader 产物一个字节都不动）；Vulkan 侧设备启用 VK_KHR_shader_atomic_int64
// + shaderBufferInt64Atomics（backend_vulkan.cpp::createDevice）。

#include "visbuf_common.slangh"
#include "water_common.slangh"

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

// M-WAT1（拍板 A）：水体 VS 位移——布局规范见 water_common.slangh 头注；
// 仅 vsVisRaster 读（viz pass 不绑不读）。--water 关时渲染端不建不绑本 CB。
cbuffer WaterConstants : register(b2)
{
    float4 g_wcA[8];   // (D.x, D.y, A, k) × 8 波
    float4 g_wcB[8];   // (omega, Q, 0, 0) × 8 波
    float4 g_wcC;      // (time, baseColor.rgb)
    float4 g_wcD;      // (ambient, specPow, specGain, maxAmp)
};

StructuredBuffer<float> g_vertexPool   : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);
StructuredBuffer<uint>  g_visibleList  : register(t4);

RWStructuredBuffer<uint64_t> g_visUAV : register(u0);
StructuredBuffer<uint64_t>   g_visSRV : register(t0);

// ============================================================================
// 1) 簇光栅：与 model.slang::vsCull 同源的 GPU vertex pulling。区别只在输出——
//    多带两个 nointerpolation 的 uint（全局簇号 + 簇内三角号），供 PS 打包。
//    VS 不传法线/uv/clusterColor：M7.0 的 vis 只装 簇/三角/深度，插值数据
//    一律留到 M7.1 材质 pass 反解时按需 pull（S7-PLAN 底线）。
// ============================================================================
struct VSVisOutput
{
    float4 position : SV_Position;
    nointerpolation uint clusterId     : CLUSTERID;
    nointerpolation uint triInCluster  : TRIID;
};

VSVisOutput vsVisRaster(uint instanceIndex : SV_InstanceID,
                        uint vertexIndex   : SV_VertexID)
{
    VSVisOutput output;
    // padding 三角 / 越界一律 NaN 裁剪坐标，光栅化阶段整三角丢弃（M4 同款）。
    output.position     = asfloat(0x7fc00000u).xxxx;
    output.clusterId    = 0u;
    output.triInCluster = 0u;

    const uint clusterId  = g_visibleList[instanceIndex];
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint triCount   = g_clusterMeta[metaBase + 1u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];

    const uint triInCluster = vertexIndex / 3u;
    if (triInCluster >= triCount) return output;

    const uint localIdx = g_localIndices[localBase + vertexIndex];
    const uint gv = vertexBase + localIdx;
    const uint b  = gv * 8u;                     // 8 float / 顶点
    float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);

    // M-WAT1（拍板 A）：水体伪 cluster → Gerstner 顶点位移（深度真实位移 =
    // SSR march 命中面正确；csDirect/csSSR 侧用同一 waterGerstner per-pixel
    // 重算法线，未位移网格坐标口径）。--water 关时无水体 cluster，分支不进。
    if (g_clusterMeta[metaBase + 3u] == NL_WATER_MATERIAL_ID)
    {
        float3 disp;
        waterGerstner(pos.xz, g_wcC.x, g_wcA, g_wcB, disp);
        pos += disp;
    }

    output.position     = mul(g_mvp, float4(pos, 1.0f));
    output.clusterId    = clusterId;
    output.triInCluster = triInCluster;
    return output;
}

struct PSVisInput
{
    float4 position : SV_Position;
    nointerpolation uint clusterId    : CLUSTERID;
    nointerpolation uint triInCluster : TRIID;
};

// 光栅 pass 的 SV_Target 输出只是占位（viz pass 会整屏覆盖）；真正产物是 u0。
// 深度测试照旧（framebuffer D32，less），atomicMin 负责把更早写入的远处片元
// 纠正为最近者——间接绘制下 instance 顺序不确定，这一步不可省。
void psVisRaster(PSVisInput input, out float4 color : SV_Target)
{
    const uint2 pixel  = uint2(input.position.xy);
    const uint  width  = uint(g_visParams.x);
    const uint  height = uint(g_visParams.y);
    if (pixel.x < width && pixel.y < height)
    {
        const uint64_t v = visPack(input.clusterId, input.triInCluster, input.position.z);
        InterlockedMin(g_visUAV[visIndex(pixel, width)], v);
    }
    color = float4(0.015f, 0.02f, 0.03f, 1.0f);
}

// ============================================================================
// 2) 全屏可视化（Carrot 的 debug pass；M7.1 会在这里换成 material pass）。
//    三条顶点的大三角形覆盖 NDC，无需顶点缓冲；SV_Position.xy 即像素下标。
// ============================================================================
struct VSVizOutput
{
    float4 position : SV_Position;
};

VSVizOutput vsVisViz(uint vertexId : SV_VertexID)
{
    VSVizOutput output;
    // (-1,-1) / (3,-1) / (-1,3)：单三角形覆盖整个 [-1,1]²，无对角线接缝。
    const float x = (vertexId == 1u) ? 3.0f : -1.0f;
    const float y = (vertexId == 2u) ? 3.0f : -1.0f;
    output.position = float4(x, y, 0.0f, 1.0f);
    return output;
}

float4 psVisViz(float4 position : SV_Position) : SV_Target
{
    const uint2 pixel  = uint2(position.xy);
    const uint  width  = uint(g_visParams.x);
    const uint  height = uint(g_visParams.y);
    if (pixel.x >= width || pixel.y >= height)
        return float4(0.055f, 0.065f, 0.085f, 1.0f);

    const uint64_t v = g_visSRV[visIndex(pixel, width)];
    // 空像素 = clear 值（没有任何可见三角覆盖）；背景色与所有 hash 色可区分。
    if (v == VIS_EMPTY)
        return float4(0.055f, 0.065f, 0.085f, 1.0f);

    uint clusterId, triId;
    float deviceZ;
    visUnpack(v, clusterId, triId, deviceZ);

    // 三角档用 (clusterId<<8 | triId) 作 hash 输入 = 全局唯一三角编号，
    // 簇内相邻三角颜色分明、簇间也不会重复；簇档直接用簇号（与 cull 模式同色）。
    const uint key = (uint(g_visParams.z) == 0u)
                   ? clusterId
                   : ((clusterId << VIS_TRI_BITS) | triId);
    return float4(visHashColor(key), 1.0f);
}
