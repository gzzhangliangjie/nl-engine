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
// Registers（§2.7：每管线单一 BindingSet；与 probe.slang 共用同一份 192B CB）：
//   b0 = ProbeConstants(192B) + t0..t3 atlas SRV + t4 prevDelta SRV
//        + t5..t8 irradiance atlas SRV（M10.1：dotMode=1 时点色 = 探针八面体
//        辐照度按探针法线方向采样，法线用 probeDepth 邻域重建——M10.0 csPlace
//        同款 3-tap cross，viz 无 ddx/ddy；gbuffer_common.slangh 反解头）
//        + t9 probeDepth SRV（法线重建输入，已存在资源零新增）

#include "octahedral.slangh"
#include "gbuffer_common.slangh"

cbuffer ProbeConstants : register(b0)
{
    // [0..3] 布点侧字段（probe.slang 读；本文件不用）
    float4x4 g_invViewProj;
    float4   g_eye;
    float4   g_placeParams;
    // [4..] 可视化侧字段
    float4x4 g_vizViewProj;    // 本帧 viewProj（atlas 世界位置反投影回屏幕）
    float4   g_vizParams;      // x=dotRadiusPx y=width z=height w=baseRes
    float4   g_vizExtra;       // M10.1：x=dotMode（0=级别色 / 1=辐照度采样）
}

Texture2D<float4> g_atlas0      : register(t0);   // 4 级 atlas SRV（RGBA32F）
Texture2D<float4> g_atlas1      : register(t1);
Texture2D<float4> g_atlas2      : register(t2);
Texture2D<float4> g_atlas3      : register(t3);
Texture2D<float4> g_prevDeltaSRV: register(t4);   // prevUV 增量（delta 装在 xy）
Texture2D<float4> g_irrad0      : register(t5);   // 辐照度 atlas SRV（RGBA16F，M10.1）
Texture2D<float4> g_irrad1      : register(t6);
Texture2D<float4> g_irrad2      : register(t7);
Texture2D<float4> g_irrad3      : register(t8);
Texture2D           g_vizDepth  : register(t9);   // probeDepth（D32，法线重建输入）

struct VSDotsOutput
{
    float4 position  : SV_Position;
    float2 quadLocal : TEXCOORD0;    // [-1,1]²，PS 圆形遮罩用
    float4 misc      : TEXCOORD1;    // x=level y=prevDeltaLen z/w 未用
    float3 irradiance: TEXCOORD2;    // M10.1：dotMode=1 时的八面体辐照度采样值
};

// atlas 按 level 4 路分支读（纹理数组不能动态索引）。
float4 loadProbeAtlas(uint level, uint2 texel)
{
    if (level == 0u) return g_atlas0.Load(int3(texel, 0));
    if (level == 1u) return g_atlas1.Load(int3(texel, 0));
    if (level == 2u) return g_atlas2.Load(int3(texel, 0));
    return g_atlas3.Load(int3(texel, 0));
}

// 辐照度 atlas 按 level 4 路分支读（texel 为探针格内存储坐标，含 border）。
float4 loadIrradAtlas(uint level, uint2 texel)
{
    if (level == 0u) return g_irrad0.Load(int3(texel, 0));
    if (level == 1u) return g_irrad1.Load(int3(texel, 0));
    if (level == 2u) return g_irrad2.Load(int3(texel, 0));
    return g_irrad3.Load(int3(texel, 0));
}

[shader("vertex")]
VSDotsOutput vsProbeDots(uint instanceId : SV_InstanceID,
                         uint vertexId   : SV_VertexID)
{
    VSDotsOutput output;
    output.position   = asfloat(0x7fc00000u).xxxx;   // 默认 NaN 裁剪（无效探针）
    output.quadLocal  = float2(0.0f, 0.0f);
    output.misc       = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.irradiance = float3(0.0f, 0.0f, 0.0f);

    // instanceId → (level, gridTexel)：细级在前，计数 = res_k²（M14C1：adaptive
    // 模式 CPU 发 ×2 实例，texel.y ≥ res 的 = 预留区自适应探针 → 品红）。
    const uint baseRes = uint(g_vizParams.w);
    uint level = 0u;
    uint2 texel = uint2(0u, 0u);
    uint remaining = instanceId;
    [unroll]
    for (uint l = 0u; l < 4u; ++l)
    {
        const uint res = baseRes >> l;
        const uint count = res * res * 2u;   // 均匀区 + 预留区（非 adaptive 模式
                                             // CPU 只发均匀区实例，后半段永不命中）
        if (remaining < count)
        {
            level = l;
            texel = uint2(remaining % res, remaining / res);
            break;
        }
        remaining -= count;
    }

    const float4 probe = loadProbeAtlas(level, texel);
    if (probe.w <= 0.0f) return output;   // 天空探针/空槽：NaN 裁剪（默认值已置）

    // M14C1：预留区探针标记（misc.z）——psProbeDots 据此画品红。uniform 区
    // （texel.y < res）恒 0 → 非 adaptive 模式品红分支不可达（保形）。
    const bool adaptiveProbe = (texel.y >= (baseRes >> level));

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
    output.misc.z = adaptiveProbe ? 1.0f : 0.0f;   // M14C1：自适应探针标记
    // level0 探针带 prevUV 增量（velocity 消费证据）；粗级恒 0。自适应探针
    // 不写 prevDelta（Spawn 不产出速度），且 texel.y ≥ res 越界 Load 返 0。
    output.misc.y = (level == 0u && texel.y < (baseRes >> 0u))
        ? length(g_prevDeltaSRV.Load(int3(texel, 0)).xy) : 0.0f;

    // M10.1：dotMode=1 → 点色 = 探针八面体辐照度按**探针法线方向**采样
    //（自检「朝天亮、朝暗处黑」的方向：顶面探针法线朝天 → 天空 texel 蓝亮；
    // 底部/背光面法线朝模型 → hit 阴影 texel 暗。「探针→眼睛」方向在凸面上
    // 恒 miss，画面全是天空色无信息量——冒烟实锤后改法线方向）。
    // 法线重建：探针屏幕位置 → probeDepth 3-tap 反投影 cross（csPlace 同款）。
    if (g_vizExtra.x > 0.5f)
    {
        const uint octRes = 8u;                      // Q12-2：core 8×8 + border
        const float2 size = max(g_vizParams.yz, float2(1.0f, 1.0f));
        const int2 pix = min(int2(floor(px)), int2(size) - 1);
        const float zc = g_vizDepth.Load(int3(pix, 0)).x;
        float3 n = float3(0.0f, 1.0f, 0.0f);
        if (zc < 1.0f)
        {
            const int2 maxPix = int2(size) - 1;
            const int2 pr = min(pix + int2(1, 0), maxPix);
            const int2 pu = min(pix + int2(0, 1), maxPix);
            const float zr = g_vizDepth.Load(int3(pr, 0)).x;
            const float zu = g_vizDepth.Load(int3(pu, 0)).x;
            if (zr < 1.0f && zu < 1.0f)
            {
                const float3 wp = gbReconstructWorldPos(uint2(pix), zc, size, g_invViewProj);
                const float3 wr = gbReconstructWorldPos(uint2(pr), zr, size, g_invViewProj);
                const float3 wu = gbReconstructWorldPos(uint2(pu), zu, size, g_invViewProj);
                n = normalize(cross(wr - wp, wu - wp));
                if (dot(n, wp - g_eye.xyz) > 0.0f) n = -n;   // 朝向相机（探针服务可见表面）
            }
        }
        const int2 octT = octaDirToTexel(n, octRes);
        // atlas 全局 texel = 探针格基址 texel·(octRes+2) + core texel + 1
        //（border 偏移，octahedral.slangh 布局）。
        const uint2 irrT = texel * (octRes + 2u) + uint2(octT) + 1u;
        output.irradiance = loadIrradAtlas(level, irrT).rgb;
    }
    return output;
}

[shader("pixel")]
void psProbeDots(VSDotsOutput input, out float4 color : SV_Target)
{
    // 圆形遮罩：quad 对角线外丢弃（正方形点在密排时会互相贴成条带）。
    if (dot(input.quadLocal, input.quadLocal) > 1.0f) discard;

    const uint level = uint(input.misc.x + 0.5f);
    float3 c;
    if (g_vizExtra.x > 0.5f)
    {
        // M10.1：辐照度直出（线性值做一次 gamma 提亮——点画在 tonemap 之后的
        // swapchain 上，线性直出对比背景偏暗；viz 是 debug 显示非光照输出）。
        c = pow(max(input.irradiance, float3(0.0f, 0.0f, 0.0f)), 1.0f / 2.2f);
    }
    else
    {
        // 按级着色 = 近密远疏的直接证据（L0 最密 = 红，L3 最疏 = 黄）。
        if      (level == 0u) c = float3(1.00f, 0.25f, 0.25f);
        else if (level == 1u) c = float3(0.30f, 1.00f, 0.35f);
        else if (level == 2u) c = float3(0.35f, 0.55f, 1.00f);
        else                  c = float3(1.00f, 0.90f, 0.20f);
    }
    // prevUV 增量非零（相机移动 / 越界）：点亮为白——时域链路可视化。
    if (input.misc.y > 1e-5f) c = float3(1.0f, 1.0f, 1.0f);
    // M14C1：自适应探针品红点（布点显示：加密集中在插值覆盖不足的细节区）。
    if (input.misc.z > 0.5f) c = float3(1.0f, 0.20f, 0.60f);
    color = float4(c, 1.0f);
}
