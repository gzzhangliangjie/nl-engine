// M-SP0 2D sprite batch — single source of truth, dual target (DXIL + SPIR-V).
// Same conventions as hud.slang / particles.slang (docs/SHADER-CONVENTIONS.md).
//
// World-space 2D sprites for Brotato. NO vertex buffer, NO input layout:
// VS expands 6 vertices from SV_VertexID (two triangles), and SV_InstanceID
// reads a StructuredBuffer. This is the exact dual-backend-proven shape from
// M-PT0 particles.slang, so no new NVRHI vertex-layout risk is introduced.
//
// Resources (one .slang, both backends; Vulkan shifts b=256 / t=0 / s=128):
//   b0 = Sprite2DConstants (volatile CB, 32 B since M-SP1)
//   t0 = StructuredBuffer<SpriteGpu> (64 B / instance)
//   t1 = atlas texture (RGBA8, premultiplied alpha since M-SP1)
//   s0 = sampler
//   g_proj.xy = world -> NDC scale, g_proj.zw = world -> NDC offset.
// CPU computes them from the ortho camera: scale = 2*pixelsPerUnit/displaySize
// (y negated for screen-down), offset = (-1 - camX*scaleX, 1 + camY*scaleY).
//
// M-SP1: one draw per bucket. The per-bucket CB carries g_baseIndex (bucket
// start offset into g_sprites); the VS reads g_sprites[iid + g_baseIndex].
// This is the cross-backend-safe replacement for setStartInstanceLocation:
// SV_InstanceID starts at 0 per draw on DX12 AND Vulkan, so one shader serves
// both without the firstInstance semantic divergence (M-SP0 note).
//
// M-SP1 alpha: atlas pixels AND per-sprite tint are premultiplied on the CPU
// (ingest / build); the pipeline blends One/InvSrcAlpha, identical to the
// RmlUi backend so shared atlas textures render identically under both.
//
// Layout discipline: SpriteGpu must stay byte-identical to
// renderer/src/sprite2d.hpp :: SpriteGpu (static_assert there).

cbuffer Sprite2DConstants : register(b0)
{
    float4 g_proj;
    uint4  g_base;    // x = bucket start index into g_sprites; yzw unused
};

struct SpriteGpu
{
    float4 posSize;   // xy = world position, zw = size in world units
    float4 uvRect;    // u0,v0,u1,v1 (atlas cell)
    float4 color;     // straight RGBA
    float4 misc;      // x = rotation (rad), y/z = origin, w = layer (unused in VS)
};

StructuredBuffer<SpriteGpu> g_sprites : register(t0);
Texture2D<float4>          g_atlas   : register(t1);
SamplerState               g_sampler : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

VSOutput vsMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // 6 vertices = (0,1,2)(3,4,5), corners in [-1,1]^2.
    const float cx = (vid == 1 || vid == 2 || vid == 4) ? 1.0f : -1.0f;
    const float cy = (vid == 2 || vid == 4 || vid == 5) ? 1.0f : -1.0f;
    const float2 corner = float2(cx, cy);

    SpriteGpu s = g_sprites[iid + g_base.x];

    const float ca = cos(s.misc.x);
    const float sa = sin(s.misc.x);
    const float2 rc = float2(corner.x * ca - corner.y * sa,
                             corner.x * sa + corner.y * ca);

    // Origin in [0,1]: (0.5,0.5) = center. Offset the quad so rotation and
    // scaling happen around the requested anchor.
    const float2 originOffset = (float2(0.5f, 0.5f) - s.misc.yz) * s.posSize.zw;
    const float2 world = s.posSize.xy + originOffset + rc * s.posSize.zw * 0.5f;

    VSOutput output;
    output.position = float4(world * g_proj.xy + g_proj.zw, 0.5f, 1.0f);
    const float2 t = corner * 0.5f + 0.5f;
    output.uv = float2(lerp(s.uvRect.x, s.uvRect.z, t.x),
                       lerp(s.uvRect.y, s.uvRect.w, t.y));
    output.color = s.color;
    return output;
}

float4 psMain(VSOutput input) : SV_Target
{
    // Premultiplied alpha: pipeline blend is One / InvSrcAlpha (M-SP1, same as
    // the RmlUi backend; atlas pixels and tint are premultiplied on the CPU).
    return g_atlas.Sample(g_sampler, input.uv) * input.color;
}
