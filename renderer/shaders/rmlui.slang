// M-UI0 RmlUi shader — same single-source / dual-target rule as hud.slang:
// one file compiled to BOTH SPIR-V (Vulkan) and DXIL (DX12). Deliberately a
// separate file (rmlui.slang, not hud.slang edits): old shader artifacts stay
// 100% byte-identical, --rmlui-demo off = zero perturbation.
//
// Resource registers shared by both targets:
//   b0 = RmlConstants (float4 projection + float4 translation, volatile CB,
//        written per draw — translation varies per draw item)
//   t0 = RmlUi texture (RGBA8 premultiplied alpha), s0 = sampler
// On Vulkan, slangc's -fvk-b-shift / -fvk-s-shift map them to the binding numbers
// NVRHI's VulkanBindingOffsets expects (CB 256, SRV 0, sampler 128).

cbuffer RmlConstants : register(b0)
{
    // xy = 2 / displaySize, zw = -1, +1  (screen pixel -> NDC, same as HUD)
    float4 g_projection;
    // Per-draw geometry translation in pixels (RmlUi RenderGeometry contract:
    // one compiled geometry can render at many positions).
    float4 g_translation;   // xy used, zw unused
};

Texture2D<float4> g_tex     : register(t0);
SamplerState      g_sampler : register(s0);

struct VSInput
{
    float2 position : POSITION;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;    // premultiplied alpha (RmlUi 6.x Vertex contract)
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
    // RmlUi emits positions in screen pixels + per-draw translation. z = 0.5,
    // w = 1: the UI layer never depth-tests, both backends (DX12 ZO / Vulkan)
    // behave the same.
    float2 ndc = (input.position + g_translation.xy) * g_projection.xy
               + g_projection.zw;
    output.position = float4(ndc, 0.5f, 1.0f);
    output.uv       = input.uv;
    output.color    = input.color;
    return output;
}

float4 psMain(VSOutput input) : SV_Target
{
    // Premultiplied pipeline: texture AND vertex colour are premultiplied, so
    // componentwise multiply stays premultiplied. Blend state on the pipeline is
    // One / InvSrcAlpha (including alpha channel) — NOT the HUD's SrcAlpha mode.
    return g_tex.Sample(g_sampler, input.uv) * input.color;
}
