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
// Wireframe is orthogonal: it is a raster fill mode, not a shader mode.

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model, column-major
    float4x4 g_model;      // model matrix, for normal transform
    float4   g_lightDir;   // xyz = world-space light direction, w = ambient
    float4   g_options;    // x = shading mode (0 textured / 1 normal / 2 uv / 3 tri-id)
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

// Deterministic integer hash (pcg-style, no float randomness): the same triangle
// always maps to the same color, so screenshot baselines are reproducible.
float3 triIdColor(uint id)
{
    uint h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0f;
}

float4 psMain(PSInput input) : SV_Target
{
    const uint mode = uint(g_options.x);

    if (mode == 1u)
    {
        float3 n = normalize(input.normal);
        return float4(n * 0.5f + 0.5f, 1.0f);   // world-space normal, [-1,1] -> [0,1]
    }
    if (mode == 2u)
    {
        return float4(input.uv, 0.0f, 1.0f);    // raw uv: seam / continuity visible
    }
    if (mode == 3u)
    {
        return float4(triIdColor(input.triId), 1.0f);
    }

    // mode 0: textured path from M0.4, kept byte-for-byte identical.
    float4 base = g_baseColor.Sample(g_sampler, input.uv);
    float3 n = normalize(input.normal);
    // abs() keeps back faces lit too: with culling disabled the inner wall is visible.
    float ndl = abs(dot(n, normalize(g_lightDir.xyz)));
    // Khronos 官方 Suzanne_BaseColor.png 是中灰底；乘一档曝光让贴图细节（含黄色眼睛）
    // 在验收截图里肉眼可见，避免看起来像「没贴图的纯色模型」。
    float3 albedo = base.rgb * 2.2f;
    float3 lit = min(albedo * (g_lightDir.w + (1.0f - g_lightDir.w) * ndl), 1.0f);
    return float4(lit, 1.0f);
}
