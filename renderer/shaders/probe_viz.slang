// M10.0 (S10): screen probe dots visualization — 探针布点可视化（S10-PLAN M10.0
// 自检：探针覆盖屏幕、近密远疏）。tonemap 之后画在 swapchain 上，深度关——全探针
// 可见，布点覆盖性证据优先于遮挡正确性（画小点 = Carrot read-screen-probes debug
// 口径）。
//   vsProbeDots : 每探针一个实例，从 atlas 拉世界位置反投影回屏幕，扩成固定像素
//                 半径的 quad；无效探针（天空）NaN 裁掉；instanceId → (level,
//                 gridTexel) 解码（细级在前）。
//   psProbeDots : 圆形遮罩 + 按级着色（近密远疏的直接证据：L0 红 / L1 绿 /
//                 L2 蓝 / L3 黄）；prevUV 增量非零（相机移动 / 越界）点亮为白。
//
// Registers（§2.7：每管线单一 BindingSet；与 probe.slang 共用同一份 176B CB）：
//   b0 = ProbeConstants(176B) + t0..t3 atlas SRV + t4 prevDelta SRV

cbuffer ProbeConstants : register(b0)
{
    // [0..3] 布点侧字段（probe.slang 读；本文件不用）
    float4x4 g_invViewProj;
    float4   g_eye;
    float4   g_placeParams;
    // [4..] 可视化侧字段
    float4x4 g_vizViewProj;    // 本帧 viewProj（atlas 世界位置反投影回屏幕）
    float4   g_vizParams;      // x=dotRadiusPx y=width z=height w=baseRes
}

Texture2D<float4> g_atlas0      : register(t0);   // 4 级 atlas SRV（RGBA32F）
Texture2D<float4> g_atlas1      : register(t1);
Texture2D<float4> g_atlas2      : register(t2);
Texture2D<float4> g_atlas3      : register(t3);
Texture2D<float4> g_prevDeltaSRV: register(t4);   // prevUV 增量（delta 装在 xy）

struct VSDotsOutput
{
    float4 position  : SV_Position;
    float2 quadLocal : TEXCOORD0;    // [-1,1]²，PS 圆形遮罩用
    float4 misc      : TEXCOORD1;    // x=level y=prevDeltaLen z/w 未用
};

// atlas 按 level 4 路分支读（纹理数组不能动态索引）。
float4 loadProbeAtlas(uint level, uint2 texel)
{
    if (level == 0u) return g_atlas0.Load(int3(texel, 0));
    if (level == 1u) return g_atlas1.Load(int3(texel, 0));
    if (level == 2u) return g_atlas2.Load(int3(texel, 0));
    return g_atlas3.Load(int3(texel, 0));
}

[shader("vertex")]
VSDotsOutput vsProbeDots(uint instanceId : SV_InstanceID,
                         uint vertexId   : SV_VertexID)
{
    VSDotsOutput output;
    output.position  = asfloat(0x7fc00000u).xxxx;   // 默认 NaN 裁剪（无效探针）
    output.quadLocal = float2(0.0f, 0.0f);
    output.misc      = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // instanceId → (level, gridTexel)：细级在前，计数 = res_k²。
    const uint baseRes = uint(g_vizParams.w);
    uint level = 0u;
    uint2 texel = uint2(0u, 0u);
    uint remaining = instanceId;
    [unroll]
    for (uint l = 0u; l < 4u; ++l)
    {
        const uint res = baseRes >> l;
        const uint count = res * res;
        if (remaining < count)
        {
            level = l;
            texel = uint2(remaining % res, remaining / res);
            break;
        }
        remaining -= count;
    }

    const float4 probe = loadProbeAtlas(level, texel);
    if (probe.w <= 0.0f) return output;   // 天空探针：NaN 裁剪（默认值已置）

    const float2 size = max(g_vizParams.yz, float2(1.0f, 1.0f));
    const float4 clip = mul(g_vizViewProj, float4(probe.xyz, 1.0f));
    if (clip.w <= 0.0f) return output;    // 相机后方
    const float2 ndc = clip.xy / clip.w;
    const float2 px = float2((ndc.x * 0.5f + 0.5f) * size.x,
                             (0.5f - 0.5f * ndc.y) * size.y);

    // quad 角标：0=(-1,-1) 1=(1,-1) 2=(-1,1) 3=(1,1)（triangle-strip 4 顶点）。
    const float2 corner = float2((vertexId == 1u || vertexId == 3u) ? 1.0f : -1.0f,
                                 (vertexId >= 2u) ? 1.0f : -1.0f);
    const float2 dotPx = px + corner * g_vizParams.x;
    const float2 dotNdc = float2(dotPx.x / size.x * 2.0f - 1.0f,
                                 1.0f - dotPx.y / size.y * 2.0f);
    output.position  = float4(dotNdc, clip.z / clip.w, 1.0f);
    output.quadLocal = corner;
    output.misc.x = float(level);
    // level0 探针带 prevUV 增量（velocity 消费证据）；粗级恒 0。
    output.misc.y = (level == 0u)
        ? length(g_prevDeltaSRV.Load(int3(texel, 0)).xy) : 0.0f;
    return output;
}

[shader("pixel")]
void psProbeDots(VSDotsOutput input, out float4 color : SV_Target)
{
    // 圆形遮罩：quad 对角线外丢弃（正方形点在密排时会互相贴成条带）。
    if (dot(input.quadLocal, input.quadLocal) > 1.0f) discard;

    const uint level = uint(input.misc.x + 0.5f);
    // 按级着色 = 近密远疏的直接证据（L0 最密 = 红，L3 最疏 = 黄）。
    float3 c;
    if      (level == 0u) c = float3(1.00f, 0.25f, 0.25f);
    else if (level == 1u) c = float3(0.30f, 1.00f, 0.35f);
    else if (level == 2u) c = float3(0.35f, 0.55f, 1.00f);
    else                  c = float3(1.00f, 0.90f, 0.20f);
    // prevUV 增量非零（相机移动 / 越界）：点亮为白——时域链路可视化。
    if (input.misc.y > 1e-5f) c = float3(1.0f, 1.0f, 1.0f);
    color = float4(c, 1.0f);
}
