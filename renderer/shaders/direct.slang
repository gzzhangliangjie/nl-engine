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
//   b6 VsmConstants (272B，M15.1 起 4 级 MVP) + t14 页表(1024 项) + t15 页池
//      ——仅 csDirectVsm（direct.csvsm）引用。M15.1 拆双入口（拍板 B）：纯净
//      csDirect blob 与 s9-shadow 冻结基线 bytes 一致（degraded guard 实测），
//      --vsm 时渲染端改选 csDirectVsm（VSM 查表 + 运行时开关双保险）。
//   u0 输出 RGBA32_FLOAT（复用 rtTarget_，不新建资源）
// SPIR-V 绑定平移与 material.slang 同：b→256, t→0, s→128, u→384。
// DXIL 需要 SM6.6（NonUniformResourceIndex 选槽 + RayQuery 内联射线，同 rt.slang）。

#include "visbuf_common.slangh"
#include "gbuffer_common.slangh"
#include "sky.slangh"
#include "vsm_common.slangh"   // M15.0/M15.1：vsmSampleShadow4（--vsm 分支用）

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
    // M-WEA2（Q-W2-2=C1）：高度雾参数 2×float4（compute-only 扩容，DirectConstants
    // 32B compute-only 先例）。fogA = (fogDensity 1/m, fogHeight m, wetness（M-WEA3）,
    // 预留)；fogB = rgb 雾色（CPU 单一事实源 = FrameParams.weatherHorizon 同值）+ a 预留。
    // fogDensity=0/wetness=0（天气关缺省）→ T≡1 + 湿化恒等 → bytes 保形（Q-WE5 闸门）。
    float4 g_directFogA;
    float4 g_directFogB;
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
    // M-WEA1（Q-WE5 已拍板）：+2×float4 天穹参数（compute-only b5 扩容；
    // b4 跨 compute/graphics 有 device removed 前科禁扩——M10.3 实锤）。
    float4 g_giZenith;        // rgb = 天顶色 / a 预留（turbidity）
    float4 g_giHorizon;       // rgb = 地平线色 / a 预留（cloudCoverage）
    // M-WEA2（Q-W2-2=C1）：+1×float4 高度雾（x = fogDensity / y = fogHeight /
    // zw 预留）。参考图 miss 天空与探针 miss 同口径雾化（Q14-1 同源）。
    float4 g_giFog;
}

// M15.0/M15.1（S15）：VSM 常量（b6，272B）。M15.0 与深度 pass b0 共用同一块
// 80B volatile CB；M15.1 起采样侧需要 4 级 MVP，拆成独立 272B CB（scene.cpp
// vsmSampleConstants_），深度 pass b0 仍为 80B 单级版（逐页绘制按级写）。
// params：x = vsm 开关（>0.5 进查表分支）/ y = 深度域 bias / z = 每级虚拟空间
// texel 数（2048）/ w = 物理页池 texel 数（2048）。
cbuffer VsmConstants : register(b6)
{
    float4x4 g_vsmLevelMvp[4];  // M15.1：每级光源正交 lightViewProj（level 0 最细）
    float4   g_vsmParams;
}

Texture2D<float>        g_vsmPagePool  : register(t15);
StructuredBuffer<uint>  g_vsmPageTable : register(t14);

// M-WAT1（拍板 A）：水体常量（b7，288B；布局规范见 water_common.slangh 头注）。
// --water 关时渲染端不建不绑本 CB，水体分支不进（bytes 保形）。
cbuffer WaterConstants : register(b7)
{
    float4 g_wcA[8];   // (D.x, D.y, A, k) × 8 波
    float4 g_wcB[8];   // (omega, Q, 0, 0) × 8 波
    float4 g_wcC;      // (time, baseColor.rgb)
    float4 g_wcD;      // (ambient, specPow, specGain, maxAmp)
};

#include "water_common.slangh"

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

    // M-WEA3（Q-W3-1=A2）：引擎级湿化——与 material.slang shadePbrGrad 双处
    // 同式（运算体逐字符同序，SSC §2.22b；UE 材质层 WetnessMask 的引擎统一
    // 版，D24 台账）。wetness=0 → wetW=0 → 精确恒等 → bytes 保形。
    const float wetW = g_directFogA.z * lerp(0.35f, 1.0f, saturate(n.y));
    roughness  = lerp(roughness, roughness * 0.35f, wetW);
    baseColor *= lerp(1.0f, 0.65f, wetW);

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
    // M-WAT1：水体伪 cluster 用保留标记（NL_WATER_MATERIAL_ID），不占
    // GPUMaterial 表——标记命中时读槽 0 占位（值不被水体分支消费），
    // 禁越界读。水体着色分支在 gbuf 计算后。
    const bool isWater = (materialId == NL_WATER_MATERIAL_ID);
    const GPUMaterial mat = isWater ? g_materialTable[0] : g_materialTable[materialId];

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
    // M-WAT2（拍板 Q-WA7=A）：水像素黑占位——折射需要「水像素背后的场景色」，
    // hdrSceneColor_ 水像素必须是占位；csWaterComposite（SSR 后）统一合成
    // 折射/吸收/Fresnel/反射/泡沫整体替换。M-WAT1 直光逻辑迁移进 composite。
    // --water 开 ⇒ viewer 强制 SSR 链（fp.ssr = o.ssr || o.water）⇒ composite
    // 必跑，占位黑不外泄；--water 关 = 无水簇，本分支死代码 = bytes 保形。
    if (isWater) {
        g_out[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    // M-WAT1：水体 per-pixel 重算 Gerstner 位移 + 解析法线（求和定义在未位移
    // 网格坐标上，iago 口径）；普通材质照旧取插值法线。（M-WAT2 后仅非水路径
    // 消费；水体 Gerstner 移至 csWaterComposite。）
    float3 shadePos = gbuf.worldPos;
    float3 n3 = normalize(gbuf.normal);
    const float3 L  = normalize(g_lightDirW.xyz);
    float shadow = 0.0f;
    if (dot(n3, L) > 0.0f) {
        const float3 origin = shadePos + n3 * g_shadowParams.x;
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

    // M-WEA2（Q-W2-1=B2）：直光合成后逐像素高度雾——UE 口径（雾在光照
    // 合成后按视线到表面深度应用一次）；fogColor = 天穹地平线色（CPU 同源）。
    // alpha 语义保持（材质 alpha 透传，material.slang shadePbrGrad 返回值）。
    {
        // M-WAT2：水体已在函数头黑占位 return（isWater 死分支清除）；
        // 普通材质照旧 shadePbrGrad。
        const float4 lit = shadePbrGrad(mat, gbuf.normal, gbuf.worldPos, gbuf.uv,
                                        gbuf.ddxUv, gbuf.ddyUv, shadow);
        const float3 toP = shadePos - g_cameraPos.xyz;
        const float  L   = length(toP);
        const float3 col = applyHeightFog(lit.rgb, g_cameraPos.y, toP / max(L, 1.0e-4f), L,
                                          g_directFogA.x, g_directFogA.y,
                                          g_directFogB.rgb, 1.0e4f);
        g_out[pixel] = float4(col, lit.a);
    }
}

// M15.1（S15）：VSM 版直光 pass（--vsm 时渲染端选本入口的 blob/csDirect.csvsm）。
// 与 csDirect 同源同序；差异仅 shadow 因子来源 = VSM 多级页表查表（b6/t14/t15）。
// 拆双入口（2026-11-13 用户拍板 B）：M15.1 把 b6 扩 272B（4 级 MVP）+ 采样函数
// 重写后，csDirect blob 重编译的 dxil 优化重排在共享代码路径产生 ±ULP（暗区
// 3 像素差 1-2 级，degraded guard bytes 实锤）——纯净 csDirect 恢复 s9 时代
// blob，冻结基线 bytes 零扰动；VSM 代码全部收进本入口。运行时开关分支保留
//（g_vsmParams.x > 0.5 查表，else 射线兜底——渲染端选 blob 已隐含开关，分支
// 作双保险）。
[shader("compute")]
[numthreads(8, 8, 1)]
void csDirectVsm(uint3 tid : SV_DispatchThreadID)
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
    // M-WAT1：水体伪 cluster 用保留标记（NL_WATER_MATERIAL_ID），不占
    // GPUMaterial 表——标记命中时读槽 0 占位（值不被水体分支消费），
    // 禁越界读。水体着色分支在 gbuf 计算后。
    const bool isWater = (materialId == NL_WATER_MATERIAL_ID);
    const GPUMaterial mat = isWater ? g_materialTable[0] : g_materialTable[materialId];

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

    // M-WAT2（拍板 Q-WA7=A）：水像素黑占位——折射需要「水像素背后的场景色」，
    // hdrSceneColor_ 水像素必须是占位；csWaterComposite（SSR 后）统一合成
    // 折射/吸收/Fresnel/反射/泡沫整体替换。M-WAT1 直光逻辑迁移进 composite。
    // --water 开 ⇒ viewer 强制 SSR 链（fp.ssr = o.ssr || o.water）⇒ composite
    // 必跑，占位黑不外泄；--water 关 = 无水簇，本分支死代码 = bytes 保形。
    if (isWater) {
        g_out[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    // M-WAT1：水体 per-pixel 重算 Gerstner 位移 + 解析法线（求和定义在未位移
    // 网格坐标上，iago 口径）；普通材质照旧取插值法线。（M-WAT2 后仅非水路径
    // 消费；水体 Gerstner 移至 csWaterComposite。）
    float3 shadePos = gbuf.worldPos;
    float3 n3 = normalize(gbuf.normal);
    const float3 L  = normalize(g_lightDirW.xyz);
    float shadow = 0.0f;
    if (dot(n3, L) > 0.0f) {
        if (g_vsmParams.x > 0.5f) {
            // M15.0/M15.1（S15）：--vsm —— shadow 因子来源 = VSM 多级页表查表 +
            // 深度比较（vsm_common.slangh::vsmSampleShadow4，UE ProjectionCommon
            // 采样链对位；M15.1 起最细级优先迭代 4 级，命中级深度域内部自算）。
            // 接收点沿法线偏移与射线 origin 同款（g_shadowParams.x，防同源自遮挡）；
            // 深度域 bias 走 g_vsmParams.y（scene.cpp 0.003，E1b.4 同量级）。
            // g_vsmParams.x = 0（--vsm-rayref）→ else 分支射线兜底：同 blob 射线
            // 基准渲染（vsm-vs-ray 门的对位渲染，run.bat 口径）。
            shadow = vsmSampleShadow4(g_vsmPagePool, g_vsmPageTable, g_vsmLevelMvp,
                                      gbuf.worldPos + n3 * g_shadowParams.x,
                                      g_vsmParams.y, g_vsmParams.z);
        } else {
            const float3 origin = shadePos + n3 * g_shadowParams.x;
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
    }

    // M-WEA2：与 csDirect 同源同序逐像素高度雾（差异仅 shadow 来源 = VSM 查表）
    {
        // M-WAT2：水体已在函数头黑占位 return（isWater 死分支清除）；
        // 普通材质照旧 shadePbrGrad。
        const float4 lit = shadePbrGrad(mat, gbuf.normal, gbuf.worldPos, gbuf.uv,
                                        gbuf.ddxUv, gbuf.ddyUv, shadow);
        const float3 toP = shadePos - g_cameraPos.xyz;
        const float  L   = length(toP);
        const float3 col = applyHeightFog(lit.rgb, g_cameraPos.y, toP / max(L, 1.0e-4f), L,
                                          g_directFogA.x, g_directFogA.y,
                                          g_directFogB.rgb, 1.0e4f);
        g_out[pixel] = float4(col, lit.a);
    }
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

    // M-WEA3（Q-W3-1=A2）：引擎级湿化——与 material.slang shadePbrGrad 双处
    // 同式（运算体逐字符同序，SSC §2.22b；UE 材质层 WetnessMask 的引擎统一
    // 版，D24 台账）。wetness=0 → wetW=0 → 精确恒等 → bytes 保形。
    const float wetW = g_directFogA.z * lerp(0.35f, 1.0f, saturate(n.y));
    roughness  = lerp(roughness, roughness * 0.35f, wetW);
    baseColor *= lerp(1.0f, 0.65f, wetW);

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
        // M-WAT1：水体标记守卫（越界禁读；GI 入口水体着色 = M-WAT2 域）。
        const bool isWater = (materialId == NL_WATER_MATERIAL_ID);
        const GPUMaterial mat = isWater ? g_materialTable[0] : g_materialTable[materialId];

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

            // M-WAT1：水体 per-pixel 重算 Gerstner 位移 + 解析法线（求和定义在未位移
    // 网格坐标上，iago 口径）；普通材质照旧取插值法线。
    // GI 入口（csRef/csRefGi）不接水体分支：refComputeLayout_ 已 6 volatile CB
    // 槽（M-WEA2 实测上限），b7 加不进——GI 参考图水体像素 = 未位移平面 +
    // 插值法线（M-WAT2 域；水面着色差异不影响 GI 趋势门用途）。
    float3 shadePos = gbuf.worldPos;
    float3 n3 = normalize(gbuf.normal);
            float shadow = 0.0f;
            if (dot(n3, L) > 0.0f) {
                const float3 origin = shadePos + n3 * g_shadowParams.x;
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
        // M-WAT1：水体标记守卫（越界禁读；GI 入口水体着色 = M-WAT2 域）。
        const bool isWater = (materialId == NL_WATER_MATERIAL_ID);
        const GPUMaterial mat = isWater ? g_materialTable[0] : g_materialTable[materialId];

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

            // M-WAT1：水体 per-pixel 重算 Gerstner 位移 + 解析法线（求和定义在未位移
    // 网格坐标上，iago 口径）；普通材质照旧取插值法线。
    // GI 入口不接水体分支（同上：volatile CB 6 槽上限，M-WAT2 域）。
    float3 shadePos = gbuf.worldPos;
    float3 n3 = normalize(gbuf.normal);

            // ---- 直光部分：与 csReference 逐字同款（NEE shadow ray）----
            float shadow = 0.0f;
            if (dot(n3, L) > 0.0f) {
                const float3 origin = shadePos + n3 * g_shadowParams.x;
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
            // miss → 天穹（M-WEA1 CB 参数）；M-WEA2：天空项先过沿程高度雾
            //（与探针 miss 同口径 = Q14-1 参考尺同源）
            float3 lin = applyHeightFog(
                skyGradientParams(d, g_giZenith.rgb, g_giHorizon.rgb),
                giray.Origin.y, d, 1.0e4f, g_giFog.x, g_giFog.y,
                g_giHorizon.rgb, 500.0f); // miss → 天穹（M-WEA1 CB 参数）
            // Q11-2b 重开（2026-11-12 用户拍板）：命中面 GI 段原被
            // `g_refSamplesStart == 0xdeadbeefu` 守卫恒 false 跳过（s10-refgi
            // 冻结基线实为纯天穹）。现改 `if (true)` 无条件执行 = 真·单弹射
            // GI 名义（hit→giAlbedo×(直光+amb)，与 probe_trace hit 分支同口径）。
            // 段内逻辑未动；重开须重拍 s10-refgi 4 张基线 + 重标定 trend 门。
            if (true)
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
