// M0.4 model shader — single source of truth (SPIR-V + DXIL from this one file).
// See docs/SHADER-CONVENTIONS.md: entry points, resource registers and Vulkan binding shifts.
//
// Resource registers are shared by both targets:
//   b0 = SceneConstants (volatile constant buffer, written every frame)
//   t0 = base color texture, s0 = sampler
// On Vulkan, slangc's -fvk-b-shift / -fvk-s-shift map them to the binding numbers
// NVRHI's VulkanBindingOffsets expects (CB 256, SRV 0, sampler 128).

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model, column-major
    float4x4 g_model;      // model matrix, for normal transform
    float4   g_lightDir;   // xyz = world-space light direction, w = ambient
    float4   g_options;    // x = unused, y = unused, z = unused, w = unused
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

float4 psMain(VSOutput input) : SV_Target
{
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
