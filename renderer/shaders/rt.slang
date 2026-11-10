// M8.2 (S8.2): Ray Query debug ray closure — per-pixel camera rays against the
// per-frame rebuilt TLAS (S8-PLAN M8.2 t1~t3). One inline RayQuery per pixel;
// two debug visualizations:
//   debugMode 0 = hit distance -> grayscale (t / tMax, linear);
//   debugMode 1 = instanceId + triangleId -> pseudocolor (hue by instance,
//                 brightness dithered by primitive).
// Output: RGBA32_FLOAT UAV texture, presented fullscreen by vsRtPresent/psRtPresent
// (4-vertex triangle strip, Texture2D.Load — no sampler, exact texels).
// Acceptance (S8-PLAN): ray visualization matches the rasterized scene in depth
// (silhouette/contour alignment); --tlas-spin rotates the TLAS instance so hit
// colors rotate every frame = per-frame instance transform propagation evidence.
//
// Registers (docs/SHADER-CONVENTIONS.md):
//   b0 = RtConstants (volatile CB, written every frame — batched diff fields go
//        into the draw-time write surface, never whole-block memcpy'd)
//   t0 = TLAS (NVRHI BindingSetItem::RayTracingAccelStruct slot 0; DXIL t-space)
//   u0 = output RGBA32_FLOAT (RWTexture2D<float4>)
//   present: t0 = rt target SRV (Texture2D.Load, no sampler)
// Vulkan: u0 maps through slangc -fvk-u-shift 384 0, b0 -> 256, t0 -> 0
// (same shifts as tlas_probe.slang / cull.slang).
// Inline ray query requires SM6.5+ on DXIL (rt.cs is in _SM66_ARTIFACTS).

cbuffer RtConstants : register(b0)
{
    // Inverse view-projection (CPU inverseMat4, column-major like g_mvp).
    float4x4 g_invViewProj;
    // x = reference range for gray scaling (2x orbit eye distance, CPU-supplied:
    // the asset spans ~1 radius around the orbit target, so t in [dist-1,
    // dist+1] maps to a visible mid-gray band; hiz viz convention: near=bright)
    // y = debug mode (0 = distance gray / 1 = instance+primitive pseudocolor)
    // z = output width, w = output height
    float4   g_params;
}

RaytracingAccelerationStructure g_tlas : register(t0);
RWTexture2D<float4>             g_out  : register(u0);

// Hue -> RGB (saturate(frac-based palette); deterministic, no texture LUT).
float3 hue2rgb(float h)
{
    float3 k = abs(frac(h + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f;
    return saturate(1.0f - k);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csRt(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= uint(g_params.z) || tid.y >= uint(g_params.w)) return;

    // Pixel center -> NDC. Compute writes row tid.y=0 = screen top (presented
    // with flipped V in psRtPresent via row mapping — see vsRtPresent below).
    float2 uv  = (float2(tid.xy) + 0.5f) / float2(g_params.z, g_params.w);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // perspectiveZO: ndc z=0 = near plane, z=1 = far plane (camera.cpp:29).
    float4 hNear = mul(g_invViewProj, float4(ndc, 0.0f, 1.0f));
    float4 hFar  = mul(g_invViewProj, float4(ndc, 1.0f, 1.0f));
    float3 pNear = hNear.xyz / hNear.w;
    float3 pFar  = hFar.xyz / hFar.w;

    // bevy_solari bindings.wesl trace_ray conventions: TMin 0.001, mask 0xFF.
    // TMax = per-pixel distance to the camera far plane (self-calibrating: the
    // gray ramp spans exactly near->far, no CPU-side scene-dependent constant).
    float3 dir = pFar - pNear;
    float  tmax = length(dir);
    RayDesc ray;
    ray.Origin    = pNear;
    ray.Direction = dir / tmax;
    ray.TMin      = 0.001f;
    ray.TMax      = tmax;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed();

    float4 color = float4(0.0f, 0.0f, 0.0f, 1.0f);   // miss = black
    if (q.CommittedStatus() != COMMITTED_NOTHING)
    {
        if (uint(g_params.y) == 0u)
        {
            // hiz viz convention: near bright -> far dark (1 - t/range).
            float gray = 1.0f - saturate(q.CommittedRayT() / g_params.x);
            color = float4(gray, gray, gray, 1.0f);
        }
        else
        {
            uint iid = q.CommittedInstanceIndex();
            uint pid = q.CommittedPrimitiveIndex();
            float hue  = frac(float(iid) * 0.618034f + 0.05f);
            float shim = 0.35f + 0.65f * frac(float(pid) * 0.1031f);
            color = float4(hue2rgb(hue) * shim, 1.0f);
        }
    }
    g_out[tid.xy] = color;
}

// ---- present pass: fullscreen quad (triangle strip, 4 verts, SV_VertexID) ----
// Same shape as hiz.slang vsHizViz. Row mapping: compute row 0 = screen top,
// Texture2D.Load row 0 must land on the top of the viewport, so the vertex at
// NDC y=-1 (bottom) samples row height-1 — uv passed to PS is flipped in Y.
struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
void vsRtPresent(uint vid : SV_VertexID, out VsOut o)
{
    // 0:(-1,-1) 1:(1,-1) 2:(-1,1) 3:(1,1)
    float2 p = float2((vid == 1 || vid == 3) ? 1.0f : -1.0f,
                      (vid >= 2) ? 1.0f : -1.0f);
    o.pos = float4(p, 0.0f, 1.0f);
    o.uv  = float2(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f);
}

Texture2D<float4> g_rtTex : register(t0);

[shader("pixel")]
void psRtPresent(VsOut i, out float4 outColor : SV_Target)
{
    // Exact-texel readback: no sampler, no filtering, deterministic bytes.
    uint w, h;
    g_rtTex.GetDimensions(w, h);
    int2 pix = min(int2(i.uv * float2(w, h)), int2(w, h) - 1);
    outColor = g_rtTex.Load(int3(pix, 0));
}

// M9.2 参考呈现：读线性 sum buffer（rtTarget_，由 csReference 累积）→ ÷已累积样本数
// → 一次性 gamma。与 M9.1 单射线图（shadePbrGrad 末行 pow(1/2.2)）同一映射，故可
// 并排当硬阴影 / 直光裁判（本 plan 用途）。RefConstants 绑 b1（与 csReference 的 b4
// 是同一份 16B buffer，仅登记寄存器不同）；g_refTex 绑 t1（rtTarget_ 的 SRV 视角，
// 避开 rt.slang 的 t0 TLAS，避免同文件寄存器冲突）。
Texture2D<float4> g_refTex : register(t1);
cbuffer RefConstants : register(b1)
{
    uint g_refSamplesPerFrame;
    uint g_refSamplesStart;
    uint g_refSamplesDone;
    float g_refPad;
}
[shader("pixel")]
void psRefPresent(VsOut i, out float4 outColor : SV_Target)
{
    uint w, h;
    g_refTex.GetDimensions(w, h);
    int2 pix = min(int2(i.uv * float2(w, h)), int2(w, h) - 1);
    float3 sum = g_refTex.Load(int3(pix, 0)).rgb;
    float inv = 1.0f / max(float(g_refSamplesDone), 1.0f);
    // 线性空间平均 → 一次性 gamma（与 M9.1 同映射）；alpha 固定 1（sum.a 恒 0）。
    outColor = float4(pow(saturate(sum * inv), 1.0f / 2.2f), 1.0f);
}
