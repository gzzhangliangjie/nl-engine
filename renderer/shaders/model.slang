// M0.4 model shader — single source of truth (SPIR-V + DXIL from this one file).
// See docs/SHADER-CONVENTIONS.md: entry points, resource registers and Vulkan binding shifts.
//
// Resource registers are shared by both targets:
//   b0 = SceneConstants (volatile constant buffer, written every frame)
//   t0 = base color texture, s0 = sampler
// On Vulkan, slangc's -fvk-b-shift / -fvk-s-shift map them to the binding numbers
// NVRHI's VulkanBindingOffsets expects (CB 256, SRV 0, sampler 128).
//
// M0.8 debug shading: g_options.x selects the shading mode
//   0 = textured (M0.4 path, untouched for baseline reproducibility)
//   1 = world-space normal -> color
//   2 = uv -> color
//   3 = triangle id (SV_PrimitiveID, integer hash) -> color
//   4 = cluster color (M1.3): per-vertex CLUSTERCOLOR baked by the cluster scene
//
// M1.3 entry points (two pairs, one source file):
//   vsMain / psMain        - 3-attribute signature (normal scene). D3D12's debug
//                            layer treats "VS input not provided by the input
//                            layout" as an error, so the cluster attribute must
//                            live in a separate entry-point pair.
//   vsCluster / psCluster  - 4-attribute signature (+CLUSTERCOLOR), cluster scene.
// Wireframe is orthogonal: it is a raster fill mode, not a shader mode.

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model, column-major
    float4x4 g_model;      // model matrix, for normal transform
    float4   g_lightDir;   // xyz = world-space light direction, w = ambient
    float4   g_options;    // x = shading mode (0 textured / 1 normal / 2 uv / 3 tri-id / 4 cluster)
};

Texture2D<float4> g_baseColor : register(t0);
SamplerState      g_sampler   : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.position = mul(g_mvp, float4(input.position, 1.0f));
    output.normal   = mul((float3x3)g_model, input.normal);
    output.uv       = input.uv;
    return output;
}

// M1.3: cluster scene adds one per-vertex attribute (baked cluster color).
struct VSInputCluster
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 clusterColor : CLUSTERCOLOR;
};

struct VSOutputCluster
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 clusterColor : CLUSTERCOLOR;
};

VSOutputCluster vsCluster(VSInputCluster input)
{
    VSOutputCluster output;
    output.position = mul(g_mvp, float4(input.position, 1.0f));
    output.normal   = mul((float3x3)g_model, input.normal);
    output.uv       = input.uv;
    output.clusterColor = input.clusterColor;
    return output;
}

// M0.8: the pixel shader receives the rasterizer-generated primitive id directly.
// It is a system value: the VS output does not carry it, and signature matching on
// both DXIL (injected by the rasterizer) and SPIR-V (gl_PrimitiveID built-in) accepts it.
struct PSInput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    uint   triId    : SV_PrimitiveID;
};

// M1.3: cluster variant — same fields plus the interpolated (constant) cluster color.
struct PSInputCluster
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 clusterColor : CLUSTERCOLOR;
    uint   triId    : SV_PrimitiveID;
};

// Deterministic integer hash (pcg-style, no float randomness): the same triangle
// always maps to the same color, so screenshot baselines are reproducible.
float3 triIdColor(uint id)
{
    uint h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0f;
}

// Shared shading body. clusterColor only participates in mode 4; callers on the
// normal signature pass a dummy value.
float4 shade(float3 normal, float2 uv, uint triId, uint mode, float3 clusterColor)
{
    if (mode == 1u)
    {
        float3 n = normalize(normal);
        return float4(n * 0.5f + 0.5f, 1.0f);   // world-space normal, [-1,1] -> [0,1]
    }
    if (mode == 2u)
    {
        return float4(uv, 0.0f, 1.0f);          // raw uv: seam / continuity visible
    }
    if (mode == 3u)
    {
        return float4(triIdColor(triId), 1.0f);
    }
    if (mode == 4u)
    {
        // M1.3: cluster visualization. All three vertices of a triangle carry the
        // same baked color (one cluster), so interpolation is a constant.
        return float4(clusterColor, 1.0f);
    }

    // mode 0: textured path from M0.4, kept byte-for-byte identical.
    float4 base = g_baseColor.Sample(g_sampler, uv);
    float3 n = normalize(normal);
    // abs() keeps back faces lit too: with culling disabled the inner wall is visible.
    float ndl = abs(dot(n, normalize(g_lightDir.xyz)));
    // Khronos 官方 Suzanne_BaseColor.png 是中灰底；乘一档曝光让贴图细节（含黄色眼睛）
    // 在验收截图里肉眼可见，避免看起来像「没贴图的纯色模型」。
    float3 albedo = base.rgb * 2.2f;
    float3 lit = min(albedo * (g_lightDir.w + (1.0f - g_lightDir.w) * ndl), 1.0f);
    return float4(lit, 1.0f);
}

float4 psMain(PSInput input) : SV_Target
{
    return shade(input.normal, input.uv, input.triId, uint(g_options.x), float3(0.0f, 0.0f, 0.0f));
}

float4 psCluster(PSInputCluster input) : SV_Target
{
    return shade(input.normal, input.uv, input.triId, uint(g_options.x), input.clusterColor);
}

// ============================================================================
// M4.0 (S4.0): GPU vertex pulling. Single drawIndexedIndirect, instanceCount =
// total cluster count (full draw — no culling yet; M4.1 adds compute culling).
//
// Per instance i (SV_InstanceID) we pull cluster i's meshlet out of three SSBOs:
//   g_vertexPool   : all cluster vertices, flattened (8 floats each: pos/normal/uv)
//   g_localIndices : per-cluster local triangle indices (uint32, 3 per triangle)
//   g_clusterMeta  : per-cluster [vertexBase, triangleCount, localIndexBase, pad]
//
// The index buffer is an identity range [0, stride) with stride = 3*maxTriCount,
// so SV_VertexID (= the index value) is the within-stride position; triInCluster
// = SV_VertexID/3. Triangles beyond triangleCount are dummy'd to NaN so the
// rasterizer discards them. This reproduces the cluster-mode geometry exactly.
// ============================================================================

StructuredBuffer<float> g_vertexPool  : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);

struct VSCullOutput {
    float4 position      : SV_Position;
    float3 normal        : NORMAL;
    float2 uv            : TEXCOORD;
    float3 clusterColor  : CLUSTERCOLOR;
};

// vertexIndex 是 identity 索引缓冲的值（0..stride-1）；instanceIndex 即簇号。
VSCullOutput vsCull(uint instanceIndex : SV_InstanceID,
                    uint vertexIndex   : SV_VertexID) {
    VSCullOutput output;
    // 默认 dummy：NaN 裁剪坐标，光栅化阶段整三角被丢弃（bevy 同式）。
    output.position     = asfloat(0x7fc00000u).xxxx;
    output.normal       = 0.0f;
    output.uv           = 0.0f;
    output.clusterColor = 0.0f;

    const uint metaBase   = instanceIndex * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint triCount   = g_clusterMeta[metaBase + 1u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];

    const uint triInCluster = vertexIndex / 3u;
    const uint vertInTri    = vertexIndex % 3u;
    if (triInCluster >= triCount) return output;   // padding 三角 -> dummy

    const uint localIdx = g_localIndices[localBase + vertexIndex];
    const uint gv = vertexBase + localIdx;          // 顶点池里的全局顶点
    const uint b  = gv * 8u;                        // 8 float / 顶点
    const float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);
    const float3 nrm = float3(g_vertexPool[b + 3u], g_vertexPool[b + 4u], g_vertexPool[b + 5u]);
    const float2 uv  = float2(g_vertexPool[b + 6u], g_vertexPool[b + 7u]);

    output.position     = mul(g_mvp, float4(pos, 1.0f));
    output.normal       = mul((float3x3)g_model, nrm);
    output.uv           = uv;
    // 簇色 = pcg(簇号)，与 cluster 场景 CPU 端 clusterColor() 同式 -> 截图逐像素一致。
    output.clusterColor = triIdColor(instanceIndex);
    return output;
}

struct PSCullInput {
    float4 position      : SV_Position;
    float3 normal        : NORMAL;
    float2 uv            : TEXCOORD;
    float3 clusterColor  : CLUSTERCOLOR;
    uint   triId         : SV_PrimitiveID;
};

float4 psCull(PSCullInput input) : SV_Target
{
    return shade(input.normal, input.uv, input.triId, uint(g_options.x), input.clusterColor);
}
