// M10.0 (S10): screen probe cascade placement — Lumen Screen Probe 布点第一步
// （S10-PLAN M10.0）。csPlace：4 级（baseRes→baseRes/2³）每级独立网格，屏幕格点 →
// 深度反投影世界位置 → 深度邻域重建法线 → 法线偏移，写 RGBA32_FLOAT atlas
// （rgb=探针世界位置，a=deviceZ；a=0 = 天空/无几何探针，UE ProbeSceneDepth>0
// 同款判据）。同时读 velocity_ 计算逐探针上一帧 UV（Q10-1/Q10-2 拍板链路），
// prevUV-curUV 增量写 prevDelta_ 供 viz 着色——相机移动时点变白（时域链路的
// 机器可见证据；静态相机全 0 = 截图基线稳定）。
//
// 口径对照（docs/s10/GI-RESEARCH-NOTES.md §1，UE 只读不抄）：
//   UE 是单级均匀网格（DownsampleFactor=16）+ 自适应加密；4 级 cascade 是本项目
//   自设计（自检目标「近密远疏」），自适应加密列 S14 可选升级。
//   探针位置 = GetWorldPositionFromScreenUV + NormalBias 法线偏移的口径对齐。
//   法线来源：UE 读 gbuffer normal；本项目 GBuffer 不落盘（Q4/Q-H2 拍板），
//   探针法线用深度邻域重建（3 tap 反投影 cross）——纯 compute 无 ddx/ddy。
//
// Registers（§2.7：每管线单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   b0 = ProbeConstants(176B，与 probe_viz.slang 共用同一份 CB buffer，各入口
//        只读自己的字段段) + t0 depth SRV + t1 velocity SRV
//        + u0..u3 atlas UAV + u4 prevDelta UAV
//   （可视化入口在 probe_viz.slang：b0 同一 CB + t0..t4 全 SRV，两管线两套 set。）

#include "gbuffer_common.slangh"

cbuffer ProbeConstants : register(b0)
{
    // [0..3] 布点侧字段
    float4x4 g_invViewProj;    // 列主序（CPU inverseMat4，rt 模式同一路径）
    float4   g_eye;            // xyz = 相机世界位置（法线朝向翻转），w = projYScale
    float4   g_placeParams;    // x=width y=height z=baseRes w=normalOffsetScale
    // [4..] 可视化侧字段（probe_viz.slang 读；本文件不用，共用一份 CB 少建一 buffer）
    float4x4 g_vizViewProj;    // 本帧 viewProj（atlas 世界位置反投影回屏幕）
    float4   g_vizParams;      // x=dotRadiusPx y=width z=height w=baseRes
}

Texture2D          g_probeDepth  : register(t0);   // D32（velocity pass 写入）
Texture2D<float2>  g_velocityTex : register(t1);   // RG16F（M10.0 T3 产物）

RWTexture2D<float4> g_atlasU0    : register(u0);   // 4 级 atlas（RGBA32F）
RWTexture2D<float4> g_atlasU1    : register(u1);
RWTexture2D<float4> g_atlasU2    : register(u2);
RWTexture2D<float4> g_atlasU3    : register(u3);
RWTexture2D<float4> g_prevDeltaU : register(u4);   // level0 探针的 prevUV-curUV

// 深度邻域重建法线：3 tap 反投影 cross。返回 false = 邻域含天空（deviceZ>=1）。
bool probeNormalFromDepth(int2 pix, float3 wp, float2 size,
                          float4x4 invViewProj, out float3 n)
{
    n = float3(0.0f, 1.0f, 0.0f);
    const int2 maxPix = int2(size) - 1;
    const int2 pr = min(pix + int2(1, 0), maxPix);
    const int2 pu = min(pix + int2(0, 1), maxPix);
    const float zr = g_probeDepth.Load(int3(pr, 0)).x;
    const float zu = g_probeDepth.Load(int3(pu, 0)).x;
    if (zr >= 1.0f || zu >= 1.0f) return false;    // 天空邻域：给默认向上法线
    const float3 wr = gbReconstructWorldPos(uint2(pr), zr, size, invViewProj);
    const float3 wu = gbReconstructWorldPos(uint2(pu), zu, size, invViewProj);
    n = normalize(cross(wr - wp, wu - wp));
    // 朝向相机（叉积手性不保证向外）：探针永远服务可见表面。
    if (dot(n, wp - g_eye.xyz) > 0.0f) n = -n;
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csPlace(uint3 tid : SV_DispatchThreadID)
{
    const uint baseRes = uint(g_placeParams.z);
    if (tid.x >= baseRes || tid.y >= baseRes) return;

    const float2 size = max(g_placeParams.xy, float2(1.0f, 1.0f));
    const int2 maxPix = int2(size) - 1;

    // level 0 探针额外算 prevUV 增量（velocity 消费链路，M10.1 时域复用的输入）。
    // 增量 = prevUV - curUV（注意方向与 velocity 相反：velocity = curUV - prevUV）。
    float2 delta = float2(0.0f, 0.0f);

    [unroll]
    for (uint level = 0u; level < 4u; ++level)
    {
        const uint res = baseRes >> level;
        if (tid.x >= res || tid.y >= res) continue;

        // 屏幕格点：探针 = 本级网格 texel 中心，间距 = viewport / 级分辨率
        //（逐级分辨率减半 → 间距加倍，即「近密远疏」的来源）。
        const float2 stride = size / float2(float(res), float(res));
        const float2 screenPx = (float2(tid.xy) + 0.5f) * stride;
        const int2 pix = min(int2(floor(screenPx)), maxPix);
        const float deviceZ = g_probeDepth.Load(int3(pix, 0)).x;

        float4 outData = float4(0.0f, 0.0f, 0.0f, 0.0f);   // a=0 = 天空/无几何
        if (deviceZ < 1.0f)
        {
            const float3 wp = gbReconstructWorldPos(uint2(pix), deviceZ, size, g_invViewProj);
            float3 n;
            probeNormalFromDepth(pix, wp, size, g_invViewProj, n);
            // 法线偏移 ∝ 探针世界间距（UE NormalBias 口径：screenStridePx ×
            // worldPerPixel；worldPerPixel = 2·z / (projYScale·H)）。
            const float worldPerPixel = 2.0f * deviceZ / (max(g_eye.w, 1e-4f) * size.y);
            const float offset = g_placeParams.w * worldPerPixel * max(stride.x, stride.y);
            outData = float4(wp + n * offset, deviceZ);
        }

        switch (level)
        {
        case 0u: g_atlasU0[tid.xy] = outData; break;
        case 1u: g_atlasU1[tid.xy] = outData; break;
        case 2u: g_atlasU2[tid.xy] = outData; break;
        case 3u: g_atlasU3[tid.xy] = outData; break;
        }

        if (level == 0u)
        {
            // velocity → prevUV：prevUV = curUV - velocity（velocity.slang 契约）。
            const float2 curUv = screenPx / size;
            const float2 vel = g_velocityTex.Load(int3(pix, 0));
            const float2 prevUv = curUv - vel;
            const bool inBounds = all(prevUv >= float2(0.0f, 0.0f)) &&
                                  all(prevUv <= float2(1.0f, 1.0f));
            // 越界（新入屏的探针）= 时域不可复用：增量写大值让 viz 亮边标出。
            delta = inBounds ? (prevUv - curUv) : float2(1e9f, 1e9f);
        }
    }

    g_prevDeltaU[tid.xy] = float4(delta, 0.0f, 0.0f);
}
