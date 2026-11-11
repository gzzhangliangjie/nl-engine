// M10.1 (S10): 探针追踪 — 每探针每八面体 texel 1 条方向射线（S10-PLAN M10.1，
// 2026-11-11 Q12-1/Q12-2/Q12-3 拍板：UE TracingOctahedronResolution=8 口径，
// 8×8 core + 1 texel border = 10×10 存储 texel/探针；trace 与存储同分辨率，
// UE 分两步是为 importance sampling filtering，第一步不需要）。
//
// 线程组织：dispatch z = 4 级 cascade，x/y = 探针格 × 10×10 存储 texel 展平。
//   每线程：读布点 atlas（RGBA32F worldPos+deviceZ）→ a=0 天空探针写黑 →
//   有效探针沿 octaTexelToDir(octTexel)（border wrap 同方向口径）发 1 条
//   RayQuery 射线打 M8 TLAS：
//     miss  → skyGradient(dir)（Q12-3 解析天穹）
//     hit   → 命中点直光（M9.1 口径降级备案，S10-PLAN Q12 已定项）：
//             albedo(统一模型色) × (lightColor·intensity·shadow/π + ambient)，
//             shadow = 命中点朝主灯的 1 条阴影射线（origin 沿探针射线回退
//             bias 防自遮挡——命中点法线在 104-tri 粗代理上不可得，无 NdotL
//             项，同 albedo 统一色一起列 M10.1 如实降级，S11 世界缓存接管）。
//   射线 tMax 取大值（1e4）：bevy 注释实锤「截断处 miss ≠ 真 miss，截太小
//   漏天光」（GI-RESEARCH-NOTES §1）。
//
// Registers（§2.7 每管线单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   b0 = TraceConstants(64B volatile) + t0..t3 place atlas SRV（cur parity）
//        + t4 TLAS + u0..u3 irradiance atlas UAV（RGBA16F，每级一张）
// （M10.1 无时间混合：单份不 ping-pong，降噪归 M10.3/S14。）

#include "octahedral.slangh"
#include "sky.slangh"

cbuffer TraceConstants : register(b0)
{
    float4 g_traceLightDir;   // xyz = 指向光源方向（writeResolveConstants 同源语义）
    float4 g_traceLightCol;   // rgb = 颜色（白），a = 强度（2.5，同 M9.x）
    float4 g_traceHitMat;     // rgb = 命中点 albedo（统一模型色），a = 环境光
    float4 g_traceParams;     // x = shadowBias（世界尺度，M9.1 g_shadowParams.x 同值）
                              // y = shadow tMax / z = baseRes / w = oct core res(8)
}

Texture2D<float4> g_placeAtlas0 : register(t0);   // 布点 atlas（RGBA32F cur parity）
Texture2D<float4> g_placeAtlas1 : register(t1);
Texture2D<float4> g_placeAtlas2 : register(t2);
Texture2D<float4> g_placeAtlas3 : register(t3);
RaytracingAccelerationStructure g_tlas : register(t4);

RWTexture2D<float4> g_irrad0 : register(u0);      // 辐照度 atlas（RGBA16F，a=1 有效）
RWTexture2D<float4> g_irrad1 : register(u1);
RWTexture2D<float4> g_irrad2 : register(u2);
RWTexture2D<float4> g_irrad3 : register(u3);

static const float kInvPi = 0.31830988f;

// place atlas 4 路分支读（纹理数组不能动态索引，probe_viz 同款）。
float4 loadPlaceAtlas(uint level, uint2 texel)
{
    if (level == 0u) return g_placeAtlas0.Load(int3(texel, 0));
    if (level == 1u) return g_placeAtlas1.Load(int3(texel, 0));
    if (level == 2u) return g_placeAtlas2.Load(int3(texel, 0));
    return g_placeAtlas3.Load(int3(texel, 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csTraceProbe(uint3 tid : SV_DispatchThreadID)
{
    const uint level  = tid.z;
    const uint res    = uint(g_traceParams.z) >> level;
    const uint octRes = uint(g_traceParams.w);
    const uint store  = octRes + 2u;                    // 10（core + 1 texel border）
    if (tid.x >= res * store || tid.y >= res * store) return;

    const uint2 probeT = uint2(tid.x / store, tid.y / store);
    const int2  octT   = int2(tid.x % store, tid.y % store) - 1;   // [-1, res]

    float3 radiance = float3(0.0f, 0.0f, 0.0f);         // 天空探针 = 黑（a=0 标记）
    const float4 probe = loadPlaceAtlas(level, probeT);
    if (probe.w > 0.0f)
    {
        // 探针 texel 方向（border texel wrap 到对侧 core 方向，无缝口径）。
        const float3 dir = octaTexelToDir(octT, octRes);
        RayDesc ray;
        ray.Origin    = probe.xyz;                      // 布点已带法线偏移
        ray.Direction = dir;
        ray.TMin      = 0.001f;                         // rt.slang/bevy trace_ray 口径
        ray.TMax      = 1.0e4f;                         // 截太小漏天光（bevy 注释）
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_tlas,
                         RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         0xFF, ray);
        q.Proceed();

        if (q.CommittedStatus() == COMMITTED_NOTHING)
        {
            radiance = skyGradient(dir);                // miss → 解析天穹（Q12-3）
        }
        else
        {
            // 命中点直光（M9.1 口径降级：统一 albedo + 无 NdotL，S10-PLAN 备案）。
            // 命中点直光 shadow ray：origin 沿探针射线回退 bias 防粗代理自遮挡
            //（命中点法线不可得，direct.slang 的 normal 偏移在此不可用）。
            const float3 hitP = probe.xyz + dir * q.CommittedRayT();
            const float3 so   = hitP - dir * g_traceParams.x;
            RayDesc sray;
            sray.Origin    = so;
            sray.Direction = g_traceLightDir.xyz;
            sray.TMin      = 0.0f;
            sray.TMax      = g_traceParams.y;
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
            sq.TraceRayInline(g_tlas,
                              RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                              0xFF, sray);
            sq.Proceed();
            const float shadow = (sq.CommittedStatus() != COMMITTED_NOTHING) ? 0.0f : 1.0f;
            // 漫反射出射 radiance ≈ albedo/π · E_direct + albedo · E_ambient
            //（shadePbrLinear diffuse 项口径；E_direct 无 NdotL 项 = 降级备案）。
            radiance = g_traceHitMat.rgb *
                       (g_traceLightCol.rgb * g_traceLightCol.a * shadow * kInvPi +
                        g_traceHitMat.a);
        }
    }

    const float4 outData = float4(radiance, 1.0f);      // a=1 = texel 有效（含天空）
    switch (level)
    {
    case 0u: g_irrad0[tid.xy] = outData; break;
    case 1u: g_irrad1[tid.xy] = outData; break;
    case 2u: g_irrad2[tid.xy] = outData; break;
    case 3u: g_irrad3[tid.xy] = outData; break;
    }
}
