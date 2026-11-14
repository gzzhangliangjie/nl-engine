// M-NL2-C：解析天穹进 scene 光栅链（--sky-bg 第二消费端）。
// 口径照抄源 = probe_gather.slang csGatherCompose Q14-4 拍板（M10.3）：
//   wpFar = gbReconstructWorldPos(pixel, 1.0, size, invVP)
//   vd    = normalize(wpFar - eye)
//   sky   = skyGradientParams(vd, zenith, horizon)          （M10.1/M-WEA1 同函数）
//   out   = applyHeightFog(sky, eye.y, vd, 1e4, fog.x, fog.y, horizon.rgb, 500)
// zenith/horizon 是 CPU 侧天气调制后的显示空间终值——直写不调 gamma（同探针链）。
// 全屏大三角无顶点输入（SV_VertexID）；VS z 必须 0.0：NVRHI DepthStencilState
// 缺省 depthTestEnable=true + Less，z=1.0 会撞清屏深度 1.0 全拒（vizPSO 旧注释
// 「缺省即关」是错的——它靠 z=0.0 过清屏深度碰巧工作，M-NL2-C 反汇编实锤勘误）。
// Registers（§2.5 SPIR-V 平移 b→256；§2.7 单一 BindingSet）：
//   b0 = SkyBgCB(144B volatile)
// 独立 .slang 新文件：psScene 冻结基线 blob 零扰动（visbuf M7.0 / direct M15.1 同款）。

#include "gbuffer_common.slangh"
#include "sky.slangh"

cbuffer SkyBgCB : register(b0)
{
    float4x4 g_invVP;     // [0..15] CPU invViewProj（nl::math::inverseMat4，Mesa 口径）
    float4   g_eye;       // [16..19] xyz
    float4   g_zenith;    // [20..23] rgb（天气调制后显示空间终值）
    float4   g_horizon;   // [24..27] rgb（兼雾色，D22② 单一事实源）
    float4   g_fog;       // [28..31] x=density y=height（weatherFogDensity/Height）
    float4   g_size;      // [32..35] x=viewportW y=viewportH
};

struct SkyBgOut
{
    float4 position : SV_Position;
};

SkyBgOut vsSkyBg(uint vid : SV_VertexID)
{
    SkyBgOut o;
    const float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u));
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);   // z=0.0：见文件头注释
    return o;
}

float4 psSkyBg(SkyBgOut input) : SV_Target
{
    const float2 size = g_size.xy;
    const uint2 pixel = uint2(input.position.xy);
    const float3 wpFar = gbReconstructWorldPos(pixel, 1.0f, size, g_invVP);
    const float3 vd = normalize(wpFar - g_eye.xyz);
    const float3 sky = skyGradientParams(vd, g_zenith.rgb, g_horizon.rgb);
    return float4(applyHeightFog(sky, g_eye.y, vd, 1.0e4f,
                                 g_fog.x, g_fog.y, g_horizon.rgb, 500.0f), 1.0f);
}
