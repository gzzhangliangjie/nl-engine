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

// ---- T0（共享 temporal / post-chain 基建，决策 A + UE 口径）：离屏 HDR + 最小 tonemap ----
// 主帧从直出 swapchain 改为渲到离屏 HDR（hdrSceneColor_，R11G11B10_FLOAT），再经最小
// tonemap 直通合成写 swapchain。gamma 已在 producer 端烤进 scene color（psRtPresent 直拷、
// cs 输出即显示色），故 tonemap 不二次 gamma —— 否则双 gamma 破坏字节基线。未来 TSR 插入
// 点即在此处之前加 temporal 累积。
// T0-PLAN §1 / §2.19：每 pass 单一 BindingSet（§2.7），shift 沿用 §2.5（set 0）。

// psBlitToHdr：把 compute 产出的 rtTarget_（RGBA32_FLOAT）拷进离屏 HDR（R11G11B10_FLOAT）。
// 格式转换由管线在写点完成；本 pass 仅直拷、不二次 gamma。t0 = rtTarget_ SRV。
Texture2D<float4> g_rtTexBlit : register(t0);
[shader("pixel")]
void psBlitToHdr(VsOut i, out float4 outColor : SV_Target)
{
    uint w, h;
    g_rtTexBlit.GetDimensions(w, h);
    int2 pix = min(int2(i.uv * float2(w, h)), int2(w, h) - 1);
    outColor = g_rtTexBlit.Load(int3(pix, 0));
}

// psTonemap：读离屏 HDR（hdrSceneColor_）→ 直通写到 swapchain（RGBA8_UNORM）。
// 与 psRtPresent 同款直拷（无采样、无滤波、字节确定）；未来 TSR 在此前插入累积。
// t0 = hdrSceneColor_ SRV；M-NL0：t2 = bloomA_（半分辨率模糊结果）SRV 复合读。
Texture2D<float4> g_hdrTex : register(t0);
[shader("pixel")]
void psTonemap(VsOut i, out float4 outColor : SV_Target)
{
    uint w, h;
    g_hdrTex.GetDimensions(w, h);
    int2 pix = min(int2(i.uv * float2(w, h)), int2(w, h) - 1);
    float4 hdr = g_hdrTex.Load(int3(pix, 0));
    // M-NL0 bloom 复合：--bloom 关时 CPU 侧 g_bloom.y=0 → rgb + bloom*0 = rgb
    //（IEEE 精确，model.slang flags=0 同款论证）→ 像素与直通逐字节一致；a 原样。
    float3 bloom = g_bloomTex.SampleLevel(g_bloomSampler, i.uv, 0.0f).rgb;
    outColor = float4(hdr.rgb + bloom * g_bloom.y, hdr.a);
}

// ---- M-NL0 bloom（--bloom，缺省关）：半分辨率 extract + 分离高斯 ping-pong ----
// 链路：psBloomExtract（hdr 全分辨率读 → 半分辨率 A 亮部）→ psBloomBlurH（A→B）
// → psBloomBlurV（B→A）→ psTonemap 复合（t2 = A）。半分辨率 RT 逐帧状态跟踪
//（§2.15/§2.19：不设 permanent，帧首 beginTracking 声明上帧末态 SRV）。
// 槽位：b2/t2 全新（rt.slang 已占 b0/b1/t0/t1/u0；s0 同型复用 t0×4 先例）。
// 参数：g_bloom = {threshold, weight, 1/halfW, 1/halfH}（CPU 每帧 writeBuffer）。
cbuffer BloomConstants : register(b2)
{
    float4 g_bloom;
}
Texture2D<float4> g_bloomTex : register(t2);     // blur 源 / tonemap 复合源
SamplerState g_bloomSampler : register(s0);      // 线性 clamp（模糊/半分辨率上采样）

// 亮部提取：亮度软阈值（lum≤threshold → 0；threshold→1 线性爬升），k 加权抑制
// 非高亮区域（display 色空间直接提取——producer 端已烤 gamma，GPU Gems 3 同口径）。
[shader("pixel")]
void psBloomExtract(VsOut i, out float4 outColor : SV_Target)
{
    uint w, h;
    g_hdrTex.GetDimensions(w, h);
    int2 pix = min(int2(i.uv * float2(w, h)), int2(w, h) - 1);
    float3 c = g_hdrTex.Load(int3(pix, 0)).rgb;
    float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    float k = saturate((lum - g_bloom.x) / max(1.0f - g_bloom.x, 1e-4f));
    outColor = float4(c * k, 1.0f);
}

// 5-tap 线性采样等效 9-tap 高斯（经典可分离权重；srcUV 由半分辨率 texel 步进）。
// dir: (1,0) 水平 / (0,1) 垂直——两入口仅方向不同，采样逻辑同函数（slang 无
// C++ lambda，inout 参数函数是项目约定，这里直接复制展开三行更直白）。
float3 bloomBlur9(float2 uv, float2 dir)
{
    float2 s = dir * g_bloom.zw;
    float3 acc = g_bloomTex.SampleLevel(g_bloomSampler, uv, 0.0f).rgb * 0.2270270270f;
    acc += g_bloomTex.SampleLevel(g_bloomSampler, uv + s * 1.3846153846f, 0.0f).rgb * 0.3162162162f;
    acc += g_bloomTex.SampleLevel(g_bloomSampler, uv - s * 1.3846153846f, 0.0f).rgb * 0.3162162162f;
    acc += g_bloomTex.SampleLevel(g_bloomSampler, uv + s * 3.2307692308f, 0.0f).rgb * 0.0702702703f;
    acc += g_bloomTex.SampleLevel(g_bloomSampler, uv - s * 3.2307692308f, 0.0f).rgb * 0.0702702703f;
    return acc;
}

[shader("pixel")]
void psBloomBlurH(VsOut i, out float4 outColor : SV_Target)
{
    outColor = float4(bloomBlur9(i.uv, float2(1.0f, 0.0f)), 1.0f);
}

[shader("pixel")]
void psBloomBlurV(VsOut i, out float4 outColor : SV_Target)
{
    outColor = float4(bloomBlur9(i.uv, float2(0.0f, 1.0f)), 1.0f);
}

// psCompare（M10.3 Q14-2）：--compare <png> 右半屏参考图贴片。CPU 侧把 viewport/
// scissor 裁到右半屏（drawCompareImage），quad 仍是 vsRtPresent_ 全屏 strip 但只
// 光栅化右半——i.uv ∈ [0,1] 直接映射参考图全幅，无需 discard / uv 重映射。
// s0 Linear clamp（非等尺寸源图平滑缩放）。
Texture2D<float4> g_compareTex : register(t0);
SamplerState g_compareSampler : register(s0);
[shader("pixel")]
void psCompare(VsOut i, out float4 outColor : SV_Target)
{
    outColor = g_compareTex.SampleLevel(g_compareSampler, i.uv, 0.0f);
}
