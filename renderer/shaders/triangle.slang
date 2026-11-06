// M0.3 triangle shader — single source of truth.
// Compiled by slangc into SPIR-V (Vulkan) and DXIL (DX12); see docs/s0/S0-PLAN.md M0.3.
// Slang syntax is HLSL-compatible; no GLSL/SPIR-V-specific annotations needed.

struct VSInput {
    float3 position : POSITION;
    float3 color    : COLOR;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 color    : COLOR;
};

VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0f);
    output.color    = input.color;
    return output;
}

float4 psMain(VSOutput input) : SV_Target
{
    return float4(input.color, 1.0f);
}
