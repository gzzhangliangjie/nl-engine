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
#include "sky.slangh"

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

// M9.2 参考累积常量（b4）。csReference 读前两项做抖动种子，psRefPresent 读第三项
// 做 ÷总数；同一份 16B buffer 在不同入口绑不同寄存器（csReference b4 / psRefPresent b1）。
// ⚠️ M10.3 实锤：此 CB 跨 compute(b4)/graphics(b1) 绑定，扩 32B 双后端 device
// removed——保持 16B；GI 参考参数走独立 b5（见下）。
cbuffer RefConstants : register(b4)
{
    uint g_refSamplesPerFrame;  // 本帧每像素射线条数（默认 64）
    uint g_refSamplesStart;     // 本帧样本全局起点（= 已累积总数，保证每帧抖动不同）
    uint g_refSamplesDone;      // 本帧结束后累积总数（present ÷用）
    float g_refPad;
}

// M10.3 csReferenceGI 专用（Q14-1）：g_refGI.rgb = GI 像素侧 albedo（统一暖灰，
// probe_gather g_gMat.rgb 同源）、g_refGI.a = 环境光因子（probe_trace
// g_traceHitMat.a 同源）。命中侧 albedo 同值——探针物理双 albedo 口径（trace 侧
// 烤命中 / gather 侧乘像素，角色不同），参考图两侧同源才与 gather 画面可比。
// 独立 16B b5：compute-only（同 DirectConstants 32B compute-only 安全先例），
// 不与 b4 的跨阶段绑定混用（scene.cpp 实锤注释）。
cbuffer GIConstants : register(b5)
{
    float4 g_refGI;
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

// ============================================================================
// M9.2 离线参考模式（--mode reference）：渐进式多帧累积的直光「真值图」。
// 与 M9.1 同一几何 / 同一 BRDF / 同一 TLAS，只把每像素 1 条阴影射线扩成
// samplesPerFrame 条**子像素抖动**的阴影射线（NEE：每样本 = 抖动主光线 +
// 朝 L 发 1 条阴影射线），累积进 g_out（线性空间，RGBA32_FLOAT 复用 rtTarget_），
// present（psRefPresent）时 ÷总数 再一次性 gamma —— 与 M9.1 单射线图并排即当硬
// 阴影 / 直光裁判（本 plan 的用途）。新增 0 个 GBuffer 落盘资源，守 Q4 冻结面。
// ============================================================================

// 线性空间 BRDF（不 bake gamma）：返回 (diffuse+specular)*radiance + ambient。
// 与 shadePbrGrad 逐字同源，仅末行去掉 pow(1/2.2)；两者须同步维护，改一改另一。
float3 shadePbrLinear(GPUMaterial mat, float3 normal, float3 worldPos, float2 uv,
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
    float3 radiance = g_lightColor.rgb * g_lightColor.a * NdotL * shadow;
    float3 diffuse  = cDiff * (1.0f - F) * (1.0f / kPi);
    float3 specular = D * V * F;
    float3 ambient  = 0.03f * baseColor;
    return (diffuse + specular) * radiance + ambient;   // LINEAR，未 gamma
}

// 子像素抖动：每像素每样本一个确定性 [0,1) 偏移（PCG 风格整数 hash）。
uint pcgHash(uint x)
{
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u; return x;
}
float2 sampleJitter(uint2 pixel, uint sampleIndex)
{
    const uint s = pixel.x + 19349663u * pixel.y + 9781u * sampleIndex;
    const uint a = pcgHash(s);
    const uint b = pcgHash(s + 0x9e3779b9u);
    return float2(float(a & 0xffffu) / 65535.0f, float(b & 0xffffu) / 65535.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csReference(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel  = tid.xy;
    const uint  width  = uint(g_width);
    const uint  height = uint(g_height);
    if (pixel.x >= width || pixel.y >= height) return;

    const uint64_t v = g_visSRV[visIndex(pixel, width)];
    // 空像素贡献 = pow(清屏色, 2.2)（线性空间）：present ÷samplesDone 再 pow(1/2.2)
    // 恰好还原 g_clearColor，与 M9.1 单射线空像素（直写清屏色）逐字节一致。
    const float3 clearLinearRef = pow(g_clearColor.rgb, 2.2f);
    const float  spp = float(g_refSamplesPerFrame);

    // 本帧累积（线性空间）。空像素贡献 clearLin * spp（present ÷samplesDone 后
    // = clearLin，与 M9.1 背景一致）；非空像素贡献 spp 条采样之和。
    float3 frameSum = 0.0f;
    if (v != VIS_EMPTY) {
        uint clusterId, triId;
        float deviceZ;
        visUnpack(v, clusterId, triId, deviceZ);

        const uint metaBase   = clusterId * 4u;
        const uint vertexBase = g_clusterMeta[metaBase + 0u];
        const uint localBase  = g_clusterMeta[metaBase + 2u];
        const uint materialId = g_clusterMeta[metaBase + 3u];
        const GPUMaterial mat = g_materialTable[materialId];

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

        const float3 L = normalize(g_lightDirW.xyz);
        for (uint s = 0u; s < g_refSamplesPerFrame; ++s) {
            const float2 jitter  = sampleJitter(pixel, g_refSamplesStart + s);
            const float2 fragPos = float2(pixel) + jitter;   // 像素内子像素点
            const GBufGeometry gbuf = gbInterpolateVisTriangle(
                pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2],
                uv[0], uv[1], uv[2], g_mvp, g_model, fragPos, g_width, g_height);

            const float3 n3 = normalize(gbuf.normal);
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
            frameSum += shadePbrLinear(mat, gbuf.normal, gbuf.worldPos, gbuf.uv,
                                       gbuf.ddxUv, gbuf.ddyUv, shadow);
        }
    } else {
        frameSum = clearLinearRef * spp;
    }

    // 累积进持久线性 sum buffer（g_out == rtTarget_，跨帧保留）；每像素独立读写无竞争。
    // 首帧（g_refSamplesStart==0）直接覆盖——buffer 可能含上一尺寸 / 上次运行的残值，
    // 用 prev+add 会污染；resize 时 drawReference 把 samplesDone_ 归零即触发覆盖。
    float4 add = float4(frameSum, 0.0f);
    if (g_refSamplesStart == 0u) g_out[pixel] = add;
    else                         g_out[pixel] = g_out[pixel] + add;
}

// ============================================================================
// M10.3 离线 GI 参考模式（--mode reference-gi，S10-PLAN Q14-1 拍板 2026-11-11）：
// 渐进式多帧累积的「直光真值 + 单弹射 GI」参考图 —— S10 自立的 GI 尺子（Q3 勘误：
// M9.2 直光参考只算直光一跳，不能当 GI 裁判）。三处先例：bevy PathtracingPlugin
// 注释原话 "meant to generate reference screenshots to compare against" + UE
// Path Tracer（SamplesPerPixel 渐进累积）+ Carrot 无对应物。
//
// 与 M10.1 探针物理**逐项同口径**（真尺子的前提）：
//   * 单弹射：每样本从表面点沿余弦加权半球方向 d 发 1 条射线（Malley 法：
//     disk 均匀 → 半球投影，pdf=cos/π 恰好消掉 diffuse 的 cos/π 积分因子），
//     miss → skyGradient(d)（Q12-3 天穹）；hit q → g_refGI.rgb ×
//     (lightCol·I·shadow(q)/π + ambient)，shadow = q 处朝主灯 1 条阴影射线
//     （origin 沿 GI 射线回退 bias——命中点法线不可得，探针 trace 同款降级）。
//   * 双 albedo：像素侧 g_refGI.rgb 乘 GI 项（probe_gather g_gMat 同源）、
//     命中侧同值乘命中 radiance（probe_trace g_traceHitMat 同源）。
//   * 直光部分 = csReference 逐字同款（jittered fragPos 插值 + shadePbrLinear
//     + shadow ray），参考图 = 直光真值 + GI 期望，对齐 gather 合成画面的构成。
//   * 几何/TLAS/粗代理/vis buffer 全同源（M8/M9 链）。
// 背景像素 = clearColor（线性 pow 2.2，M9.2 同款）：参考图用于数值对比，
// --sky-bg 只影响实时画面，参考图背景不参与（python 统计做背景掩膜）。
// ============================================================================

[shader("compute")]
[numthreads(8, 8, 1)]
void csReferenceGI(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel  = tid.xy;
    const uint  width  = uint(g_width);
    const uint  height = uint(g_height);
    if (pixel.x >= width || pixel.y >= height) return;

    const uint64_t v = g_visSRV[visIndex(pixel, width)];
    const float3 clearLinearRef = pow(g_clearColor.rgb, 2.2f);
    const float  spp = float(g_refSamplesPerFrame);

    float3 frameSum = 0.0f;
    if (v != VIS_EMPTY) {
        uint clusterId, triId;
        float deviceZ;
        visUnpack(v, clusterId, triId, deviceZ);

        const uint metaBase   = clusterId * 4u;
        const uint vertexBase = g_clusterMeta[metaBase + 0u];
        const uint localBase  = g_clusterMeta[metaBase + 2u];
        const uint materialId = g_clusterMeta[metaBase + 3u];
        const GPUMaterial mat = g_materialTable[materialId];

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

        const float3 L = normalize(g_lightDirW.xyz);
        const float3 giAlbedo = g_refGI.rgb;
        const float  giAmb   = g_refGI.a;

        for (uint s = 0u; s < g_refSamplesPerFrame; ++s) {
            const float2 jitter  = sampleJitter(pixel, g_refSamplesStart + s);
            const float2 fragPos = float2(pixel) + jitter;
            const GBufGeometry gbuf = gbInterpolateVisTriangle(
                pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2],
                uv[0], uv[1], uv[2], g_mvp, g_model, fragPos, g_width, g_height);

            const float3 n3 = normalize(gbuf.normal);

            // ---- 直光部分：与 csReference 逐字同款（NEE shadow ray）----
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
            frameSum += shadePbrLinear(mat, gbuf.normal, gbuf.worldPos, gbuf.uv,
                                       gbuf.ddxUv, gbuf.ddyUv, shadow);

            // ---- GI 部分：余弦加权半球 1 弹射（pdf=cos/π 消积分因子，期望
            // = albedo_pix × E[L(d)]；探针 trace 无 NdotL 降级口径在 hit 项内）。
            // Malley 法：disk 均匀 (u,v) → 半球投影 d = t·u + b·v + n·sqrt(1-u²-v²)。
            const float2 gj = sampleJitter(pixel, g_refSamplesStart + s + 0x5bf03635u);
            const float  r  = sqrt(gj.x);
            const float  phi = 6.2831853f * gj.y;
            const float3 t1 = normalize(abs(n3.y) < 0.9f
                                ? cross(float3(0, 1, 0), n3) : cross(float3(1, 0, 0), n3));
            const float3 t2 = cross(n3, t1);
            const float3 d  = normalize(t1 * (r * cos(phi)) + t2 * (r * sin(phi))
                                        + n3 * sqrt(max(1.0f - gj.x, 0.0f)));

            RayDesc giray;
            giray.Origin    = gbuf.worldPos + n3 * g_shadowParams.x;  // 防粗代理自遮挡
            giray.Direction = d;
            giray.TMin      = 0.001f;                                  // rt.slang/bevy 口径
            giray.TMax      = 1.0e4f;                                  // 截太小漏天光（bevy 注释）
            float3 lin = skyGradient(d);                               // miss 默认 → 天穹（Q12-3）
            // ⚠️ 命中面 GI 段（M10.3 二分遗留形态）：`g_refSamplesStart ==
            // 0xdeadbeefu` 正常恒 false —— s10-refgi 冻结基线的 GI 项实为
            // 纯天穹（本段从不执行）。段内容 = probe_trace hit 分支同口径，
            // 留作 M11.2 探针查缓存后重开此段的现成实现；重开必须重拍
            // s10-refgi 基线（单独拍板，禁顺手改形态）。
            if (g_refSamplesStart == 0xdeadbeefu)
            {
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> gq;
                gq.TraceRayInline(g_tlas,
                                  RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                                  0xFF, giray);
                gq.Proceed();

                if (gq.CommittedStatus() != COMMITTED_NOTHING)
                {
                    const float3 hitP = giray.Origin + d * gq.CommittedRayT();
                    const float3 so   = hitP - d * g_shadowParams.x;       // 探针 trace 同款回退
                    RayDesc sray;
                    sray.Origin    = so;
                    sray.Direction = L;
                    sray.TMin      = 0.0f;
                    sray.TMax      = g_shadowParams.y;
                    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
                    sq.TraceRayInline(g_tlas,
                                      RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                                      0xFF, sray);
                    sq.Proceed();
                    const float sh2 = (sq.CommittedStatus() != COMMITTED_NOTHING) ? 0.0f : 1.0f;
                    lin = giAlbedo * (g_lightColor.rgb * g_lightColor.a * sh2 * (1.0f / kPi) + giAmb);
                }
            }
            frameSum += giAlbedo * lin;
        }
    } else {
        frameSum = clearLinearRef * spp;
    }

    float4 add = float4(frameSum, 0.0f);
    if (g_refSamplesStart == 0u) g_out[pixel] = add;
    else                         g_out[pixel] = g_out[pixel] + add;
}
