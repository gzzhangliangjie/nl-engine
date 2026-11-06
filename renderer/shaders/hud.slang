// M0.5 ImGui HUD shader — same single-source / dual-target rule as model.slang:
// this one file is compiled to BOTH SPIR-V (Vulkan) and DXIL (DX12).
// See docs/SHADER-CONVENTIONS.md: entry points, register slots and Vulkan binding shifts.
//
// Resource registers are shared by both targets:
//   b0 = HudConstants (float4 projection, volatile constant buffer written every frame)
//   t0 = ImGui font atlas (RGBA8), s0 = sampler
// On Vulkan, slangc's -fvk-b-shift / -fvk-s-shift map them to the binding numbers
// NVRHI's VulkanBindingOffsets expects (CB 256, SRV 0, sampler 128).

cbuffer HudConstants : register(b0)
{
    // xy = 2 / displaySize, zw = -1 - displayPos * xy  (screen pixel -> NDC)
    float4 g_projection;
};

Texture2D<float4> g_font    : register(t0);
SamplerState      g_sampler : register(s0);

struct VSInput
{
    float2 position : POSITION;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

VSOutput vsMain(VSInput input)
{
    VSOutput output;
    // ImGui emits positions in screen pixels. Force z = 0.5 and w = 1: the HUD
    // never depth-tests, so both DX12 (ZO) and Vulkan (ZO here) behave the same.
    float2 ndc = input.position * g_projection.xy + g_projection.zw;
    output.position = float4(ndc, 0.5f, 1.0f);
    output.uv       = input.uv;
    output.color    = input.color;
    return output;
}

float4 psMain(VSOutput input) : SV_Target
{
    // The atlas is white RGB with coverage in alpha; ImGui vertex color carries
    // the tint. Alpha blending (srcAlpha / invSrcAlpha) is set on the pipeline.
    return g_font.Sample(g_sampler, input.uv) * input.color;
}
