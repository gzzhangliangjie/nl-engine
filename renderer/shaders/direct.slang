// M9.1 (S9): compute direct-lighting + hard shadow-ray path（M9.0 的对照 pass 升级：
// Q4 方案 A「不落盘，即取即算」+ 用户拍板 ② 逐字搬移 mode 5 BRDF 不变）。
//
// 语义（M9.1 阶段）：每像素读 vis buffer → 用 gbuffer_common.slangh 即取即算几何属性
// → 逐字复刻 material.slang::psMatResolve 的 mode 5 BRDF → **朝光源发 1 条 shadow ray**
// 打 M8 的 TLAS（粗代理几何）→ 被挡→直光乘 0（环境光保留）、否则→完整 BRDF → 写
// M8.2 已有的 rtTarget_ (RGBA32_FLOAT UAV) → 由共用的 psRtPresent 呈现。
//
// 对照卡（M9.0 T6）锁定 tag s9-gbuffer 的 exe：M9.1 起同 mode 语义变更（多了阴影射线），
// 禁止用新 exe 重跑那两张基线；M9.1 自带 s9-shadow 基线。
//
// 与 mode 5 逐像素一致的三条口径（T6）：
//   1) fragPos：PS 的 SV_Position.xy 是像素中心 (x+0.5, y+0.5)，compute 用
//      float2(tid.xy) + 0.5f；gbInterpolateVisTriangle 的表达式逐字未改。
//   2) 空像素：PS 用 discard 让 framebuffer 的清屏色透出；compute 无法 discard，
//      且 present 全屏覆盖会盖掉 clear —— 故写当帧清屏色 g_clearColor(b3)，
//      与 discard 等价。viewer/main.cpp kColors[0] = (0.05, 0.08, 0.15, 1)，
//      **不是黑色**；--color 可切四档，故清屏色必须每帧传入。
//   3) 色彩转换：RGBA32_FLOAT 原值 → Texture2D.Load → 写 RGBA8_UNORM framebuffer，
//      与 PS 直写 framebuffer 一样只有一次 float→UNORM 转换。
//
// 资源寄存器（§2.7 每管线单一 BindingSet；本 pass 自己的 layout/set）：
//   b0 SceneConstants (160B) / b1 LightConstants (80B) / b2 MatResolveConstants (16B)
//   b3 DirectConstants (32B)：g_clearColor（当帧清屏色）+ g_shadowParams（x=bias 世界尺度法线偏移, y=tMax）
//   t0 vis SRV / t1..t3 vertexPool / localIndices / clusterMeta
//   t4..t7 baseColor / t8..t11 mrTex / t12 材质间接表 / s0 sampler
//   t13 TLAS（M9.1 阴影射线；RayTracingAccelStruct，DXIL t 空间 / SPIR-V 同 t-shift）
//   u0 输出 RGBA32_FLOAT（复用 rtTarget_，不新建资源）
// SPIR-V 绑定平移与 material.slang 同：b→256, t→0, s→128, u→384。
// DXIL 需要 SM6.6（NonUniformResourceIndex 选槽 + RayQuery 内联射线，同 rt.slang）。

#include "visbuf_common.slangh"
#include "gbuffer_common.slangh"

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model，列主序
    float4x4 g_model;      // model 矩阵，世界空间变换
    float4   g_lightDir;   // xyz = 世界空间光方向, w = 环境光
    float4   g_options;    // x = 着色模式（本 pass 固定 PBR，不读）
}

cbuffer LightConstants : register(b1)
{
    float4   g_lightDirW;   // xyz = 指向光源方向, w 未用
    float4   g_lightColor;  // rgb = 颜色, a = 强度
    float4   g_cameraPos;   // xyz = 世界空间相机位置
    float4   g_baseColorF;  // glTF baseColorFactor
    float4   g_mrF;         // x = metallicFactor, y = roughnessFactor, z = pbr-naive 开关
}

cbuffer MatResolveConstants : register(b2)
{
    float g_width;
    float g_height;
    float2 _pad;
}

cbuffer DirectConstants : register(b3)
{
    float4 g_clearColor;     // 与 framebuffer 的 ClearColor 逐值一致（offset 0）
    float4 g_shadowParams;   // x = bias（世界尺度法线偏移，防 proxy 自遮挡）
                             // y = tMax（阴影射线最大距离，有限值保精度）
                             // z/w 预留（offset 16）
}

RaytracingAccelerationStructure g_tlas : register(t13);

StructuredBuffer<uint64_t> g_visSRV          : register(t0);
StructuredBuffer<float>    g_vertexPool      : register(t1);
StructuredBuffer<uint>     g_localIndices    : register(t2);
StructuredBuffer<uint>     g_clusterMeta     : register(t3);
Texture2D<float4>          g_baseColorTex[kMaxVisMaterials] : register(t4);
Texture2D<float4>          g_mrTex[kMaxVisMaterials]        : register(t8);
StructuredBuffer<GPUMaterial> g_materialTable : register(t12);
SamplerState               g_sampler          : register(s0);
RWTexture2D<float4>        g_out              : register(u0);

// ============================================================================
// 以下四个函数逐字搬移 material.slang:96-153（mode 5 BRDF）。禁任何代数改写 /
// branch / lerp 化简 —— dxc 可能把「数学恒等式」融成 FMA 造成 1 LSB 漂移。
// ============================================================================
static const float kPi = 3.14159265f;

float3 srgbToLinear(float3 c)
{
    return select(c <= 0.04045f, c / 12.92f, pow((c + 0.055f) / 1.055f, 2.4f));
}

float D_GGX(float roughness, float NdotH)
{
    float a  = NdotH * roughness;
    float k  = roughness / ((1.0f - NdotH * NdotH) + a * a);
    return k * k * (1.0f / kPi);
}

float V_SmithGGXCorrelated(float roughness, float NdotV, float NdotL)
{
    float a2      = roughness * roughness;
    float lambdaV = NdotL * sqrt((NdotV - a2 * NdotV) * NdotV + a2);
    float lambdaL = NdotV * sqrt((NdotL - a2 * NdotL) * NdotL + a2);
    return 0.5f / (lambdaV + lambdaL);
}

float4 shadePbrGrad(GPUMaterial mat, float3 normal, float3 worldPos, float2 uv,
                    float2 ddx_uv, float2 ddy_uv, float shadow)
{
    float3 n = normalize(normal);
    float3 v = normalize(g_cameraPos.xyz - worldPos);
    float3 l = normalize(g_lightDirW.xyz);
    float3 h = normalize(l + v);
    float NdotL = saturate(dot(n, l));
    float NdotV = max(dot(n, v), 1e-4f);
    float NdotH = saturate(dot(n, h));
    float VdotH = saturate(dot(v, h));

    // M7.2：贴图与因子全部走材质间接表；materialId 逐像素非一致，槽号必须
    // NonUniformResourceIndex（贴图采样用解析导数，无硬件四边形导数依赖）。
    float4 baseTex = g_baseColorTex[NonUniformResourceIndex(mat.baseColorSlot)]
        .SampleGrad(g_sampler, uv, ddx_uv, ddy_uv);
    float3 baseColor = mat.baseColorFactor.rgb *
        (g_mrF.z >= 0.5f ? baseTex.rgb : srgbToLinear(baseTex.rgb));
    float4 mrTex = g_mrTex[NonUniformResourceIndex(mat.mrSlot)]
        .SampleGrad(g_sampler, uv, ddx_uv, ddy_uv);
    float roughness = clamp(mrTex.g * mat.mrFactor.y, 0.045f, 1.0f);
    float metallic  = saturate(mrTex.b * mat.mrFactor.x);

    float3 F0     = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    float3 cDiff  = baseColor * (1.0f - metallic);
    float  D = D_GGX(roughness, NdotH);
    float  V = V_SmithGGXCorrelated(roughness, NdotV, NdotL);
    float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
    // M9.1：shadow = 1 不被挡 / 0 被挡（朝光源的 RayQuery 命中 TLAS）。环境光
    // 不乘 shadow（被挡仍保留环境项，符合硬阴影物理语义；直光被挡→0）。
    float3 radiance = g_lightColor.rgb * g_lightColor.a * NdotL * shadow;
    float3 diffuse  = cDiff * (1.0f - F) * (1.0f / kPi);
    float3 specular = D * V * F;
    float3 ambient  = 0.03f * baseColor;
    float alpha = mat.baseColorFactor.a * baseTex.a;
    float3 encoded = pow(saturate((diffuse + specular) * radiance + ambient), 1.0f / 2.2f);
    return float4(encoded, alpha);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csDirect(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel  = tid.xy;
    const uint  width  = uint(g_width);
    const uint  height = uint(g_height);
    if (pixel.x >= width || pixel.y >= height) return;

    const uint64_t v = g_visSRV[visIndex(pixel, width)];
    // 空像素：对齐 psMatResolve 的 discard —— present 全屏覆盖，清屏色必须由
    // UAV 自己提供（见文件头口径 2）。
    if (v == VIS_EMPTY) {
        g_out[pixel] = g_clearColor;
        return;
    }

    uint clusterId, triId;
    float deviceZ;
    visUnpack(v, clusterId, triId, deviceZ);

    // 簇元数据：[vertexBase, triCount, localBase, materialId]。槽 3 = 子网格序号。
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];
    const uint materialId = g_clusterMeta[metaBase + 3u];
    const GPUMaterial mat = g_materialTable[materialId];

    // vertex pulling：拉三顶点（8 float/顶点：pos/normal/uv）。
    float3 pos[3], nrm[3];
    float2 uv[3];
    for (uint k = 0u; k < 3u; ++k) {
        const uint localIdx = g_localIndices[localBase + triId * 3u + k];
        const uint gv = vertexBase + localIdx;
        const uint b  = gv * 8u;
        pos[k] = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);
        nrm[k] = float3(g_vertexPool[b + 3u], g_vertexPool[b + 4u], g_vertexPool[b + 5u]);
        uv[k]  = float2(g_vertexPool[b + 6u], g_vertexPool[b + 7u]);
    }

    // SV_Position.xy → 像素中心；其余表达式与 psMatResolve 逐字一致。
    const float2 fragPos = float2(pixel) + 0.5f;
    const GBufGeometry gbuf = gbInterpolateVisTriangle(
        pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2],
        uv[0], uv[1], uv[2], g_mvp, g_model, fragPos, g_width, g_height);

    // M9.1 阴影射线：从表面沿世界法线偏移 bias（防 proxy 粗代理自遮挡，bevy
    // initial_path.wesl 同款 ray_origin = worldPos + normal*RAY_T_MIN）发向光源，
    // 打 M8 的 TLAS（粗代理几何）。命中=被挡→shadow 0（直光乘 0）；未命中=1。
    // 背向光的面（NdotL<=0）本就无直射，ray 无意义，直接判 0。
    const float3 n3 = normalize(gbuf.normal);
    const float3 L  = normalize(g_lightDirW.xyz);
    float shadow = 0.0f;
    if (dot(n3, L) > 0.0f) {
        const float3 origin = gbuf.worldPos + n3 * g_shadowParams.x;
        RayDesc ray;
        ray.Origin    = origin;
        ray.Direction = L;
        ray.TMin      = 0.0f;
        ray.TMax      = g_shadowParams.y;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_tlas,
                         RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         0xFF, ray);
        q.Proceed();
        shadow = (q.CommittedStatus() != COMMITTED_NOTHING) ? 0.0f : 1.0f;
    }

    g_out[pixel] = shadePbrGrad(mat, gbuf.normal, gbuf.worldPos, gbuf.uv,
                                gbuf.ddxUv, gbuf.ddyUv, shadow);
}
