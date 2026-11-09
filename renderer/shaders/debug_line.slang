// M-P3 physics debug line overlay — single source of truth, dual target.
// This one .slang file is compiled by slangc into BOTH SPIR-V (Vulkan) and
// DXIL (DX12), exactly like model.slang / triangle.slang / hud.slang.
//
// It draws physics collision-shape wireframes as a colored LINE_LIST overlay on
// top of the scene. Resource registers are shared by both targets:
//   b0 = DebugConstants (float4x4 g_mvp, volatile constant buffer, written each frame)
// Vertex layout (matches DebugLineVertex in scene.cpp): POSITION (float3) + COLOR (float3)
//
// See docs/SHADER-CONVENTIONS.md for the Vulkan binding-shift rule that keeps the
// same register names working on both backends.

cbuffer DebugConstants : register(b0)
{
    // view-projection * model, column-major (identical convention to model.slang's g_mvp)
    float4x4 g_mvp;
};

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
    output.position = mul(g_mvp, float4(input.position, 1.0f));
    output.color    = input.color;
    return output;
}

float4 psMain(VSOutput input) : SV_Target
{
    return float4(input.color, 1.0f);
}
