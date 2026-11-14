// M7.1 (S7): visibility buffer 材质反解 pass（option A 单全屏 pass，Carrot 同款）。
// 一条全屏三角形覆盖整屏：每像素读 vis buffer → 反解簇/三角 → vertex pulling
// 拉三顶点 → 解析式（The-Forge）算透视正确 barycentrics + 像素导数 → 插世界
// 位置/法线/uv → 走与 model.slang 一致的着色（mode 0 文本化 / mode 5 PBR 直光）。
//
// 与 M0.4 传统画法的等价性：vis 光栅 pass 用 g_mvp 把同一批顶点投到屏幕并写
// 64 位 id；本 pass 用同一份 g_mvp 把拉出的三顶点重新投影，三角形与栅格化结果
// 逐像素一致，反解出的「哪个三角覆盖本像素 + 该像素的属性」与 M0.4 硬件光栅
// 插值得到的东西是同一个数学量（透视正确插值），故单材质输出应逐像素一致。
//
// 空像素（VIS_EMPTY）直接 discard，让 viewer 清屏色透出来——与 M0.4「清屏 +
// 画 Suzanne」的空区域一致，无需把清屏色传进 shader（dx12/vulkan 大三角形
// 全屏覆盖，非几何像素由 clear 保留，故 material pass 不写背景）。
//
// 资源寄存器（docs/SHADER-CONVENTIONS.md §2.7：每条管线单一 BindingSet）：
//   b0 = SceneConstants (160B, 与 visbuf.slang/model.slang 同布局)
//   b1 = LightConstants (80B, 仅 mode 5 用：光照 + 相机 + pbr-naive 开关；
//        材质因子已迁材质表，b1 里的因子字段 mode 5 不再读)
//   b2 = MatResolveConstants (16B): (width, height, _, _)
//   t0 = vis buffer SRV（与 raster u0 同一块 u64 buffer 的 SRV 视角）
//   t1..t3 = vertexPool / localIndices / clusterMeta（与 model.slang vsCull 同源）
//   t4..t7 = baseColor 贴图数组 [kMaxVisMaterials]（RGBA8_UNORM，srgbToLinear 在
//            shader 内；材质 i 用 NonUniformResourceIndex 选槽）
//   t8..t11 = metallicRoughness 贴图数组 [kMaxVisMaterials]（G=roughness B=metallic）
//   t12 = 材质间接表（GPUMaterial[kMaxVisMaterials]，clusterMeta[3] 的 materialId
//         直接寻址；查找逻辑集中 visbuf_common.slangh，S9/S10/S13 复用）
//   s0 = sampler
// SPIR-V 绑定平移：b→256, t→0, s→128, u→384（本 pass 无 UAV）；DX12 用各自 slot。
//
// M7.2 多材质：materialId = 簇的子网格序号（.nmesh NSUB 段约定，D-M7.2-1 方案 A），
// 由 initCull 填进 clusterMeta 槽 3。三材质以上跑通间接表 = 本里程碑验收核心。
//
// 介质偏离：NVRHI 无 R64 格式，vis buffer 是 RWStructuredBuffer<uint64_t>（见
// visbuf_common.slangh 与 docs/s5/UE-DIFFS.md D15）。

#include "visbuf_common.slangh"
#include "gbuffer_common.slangh"
#include "sky.slangh"      // M-WEA2：applyHeightFog（raster 直光雾与 compute 同源）

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model，列主序
    float4x4 g_model;      // model 矩阵，世界空间变换
    float4   g_lightDir;   // xyz = 世界空间光方向, w = 环境光
    float4   g_options;    // x = 着色模式（0 文本化 / 1 法线 / 2 uv / 5 pbr）
};

// pbr-direct：与 model.slang b1 逐字节对应（80B）。
cbuffer LightConstants : register(b1)
{
    float4   g_lightDirW;   // xyz = 指向光源方向, w 未用
    float4   g_lightColor;  // rgb = 颜色, a = 强度
    float4   g_cameraPos;   // xyz = 世界空间相机位置
    float4   g_baseColorF;  // glTF baseColorFactor
    float4   g_mrF;         // x = metallicFactor, y = roughnessFactor, z = pbr-naive 开关
};

// 材质反解尺寸常量（pulling 每帧写一次）。
cbuffer MatResolveConstants : register(b2)
{
    float g_width;
    float g_height;
    float2 _pad;
};

// M-WEA2（Q-W2-1=B2）：b4 RasterFog——raster matResolve 直光雾专用 32B CB
//（纯 graphics 绑定；雾两包与 compute 直光 b3 雾同源同值，CPU writeResolveConstants
// 统一填写）。density=0（天气关）→ T≡1 → raster 基线 bytes 保形。
// ⚠️ 不复用 b3 DirectConstants：共享槽位表加 b3 撞 NVRHI volatile CB 上限（6 个）
// + compute layout b3 重复声明（2026-11-14 实测），故走 matResolve 专属 b4。
cbuffer RasterFogConstants : register(b4)
{
    float4 g_rFogA;        // x = fogDensity 1/m / y = fogHeight m / z = wetness（M-WEA3）/ w 预留
    float4 g_rFogB;        // rgb = 雾色（CPU 填 weatherHorizon 同值）/ a 预留
};

StructuredBuffer<uint64_t> g_visSRV          : register(t0);
StructuredBuffer<float>    g_vertexPool      : register(t1);
StructuredBuffer<uint>     g_localIndices    : register(t2);
StructuredBuffer<uint>     g_clusterMeta     : register(t3);
Texture2D<float4>          g_baseColorTex[kMaxVisMaterials] : register(t4);
Texture2D<float4>          g_mrTex[kMaxVisMaterials]        : register(t8);
StructuredBuffer<GPUMaterial> g_materialTable : register(t12);
SamplerState               g_sampler          : register(s0);

// ============================================================================
// 全屏三角形 VS（与 visbuf.slang::vsVisViz 同款：(-1,-1)/(3,-1)/(-1,3) 覆盖整屏）。
// ============================================================================
struct VSOut
{
    float4 position : SV_Position;
};

VSOut vsMatResolve(uint vertexId : SV_VertexID)
{
    VSOut output;
    const float x = (vertexId == 1u) ? 3.0f : -1.0f;
    const float y = (vertexId == 2u) ? 3.0f : -1.0f;
    output.position = float4(x, y, 0.0f, 1.0f);
    return output;
}

// ============================================================================
// PBR 直光辅助（与 model.slang 逐式同构，公式权威见 Filament §4 / bevy
// pbr_lighting.wesl / Carrot brdf.glsl）。仅 mode 5 用；用 SampleGrad 显式传导数
// （全屏三角形下相邻像素可能属不同三角形，硬件四边形导数不可靠）。
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
                    float2 ddx_uv, float2 ddy_uv)
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

    // M-WEA3（Q-W3-1=A2）：引擎级湿化（UE 无引擎级对位——材质层 WetnessMask
    // 节点的引擎统一版，D24 台账）。雨从上往下淋：权重 = lerp(0.35, 1,
    // saturate(N·up)) × 全局 wetness（天花板基础湿 35%、地面全湿）；湿面
    // 趋光滑（roughness 压到 35%）+ 暗化（albedo × 0.65）。wetness=0（天气
    // 关缺省）→ wetW=0 → lerp/乘法精确恒等 → raster 基线 bytes 保形。
    // ⚠️ 与 direct.slang shadePbrGrad 双处同式（运算体逐字符同序，SSC §2.22b）。
    const float wetW = g_rFogA.z * lerp(0.35f, 1.0f, saturate(n.y));
    roughness  = lerp(roughness, roughness * 0.35f, wetW);
    baseColor *= lerp(1.0f, 0.65f, wetW);

    float3 F0     = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    float3 cDiff  = baseColor * (1.0f - metallic);
    float  D = D_GGX(roughness, NdotH);
    float  V = V_SmithGGXCorrelated(roughness, NdotV, NdotL);
    float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
    float3 radiance = g_lightColor.rgb * g_lightColor.a * NdotL;
    float3 diffuse  = cDiff * (1.0f - F) * (1.0f / kPi);
    float3 specular = D * V * F;
    // M10.2：g_options.z=1（probe gather 态）时 ambient 置零 = 真替换——间接光
    // 由 probe_gather.slang csGatherCompose 补上（Q13-2 防双重计费）。probe-viz
    // 场景渲染走本 matResolve 管线（model.slang psMain 不参与），开关必须落在这；
    // g_options.z 全路径空闲（g_options.y 被 frustumCull clusterCount /
    // pbr-naive 开关双重占用，禁复用）。默认 0 = 既有 visbuf/pulling 用例保形。
    float3 ambient  = ((g_options.z > 0.5f) ? 0.0f : 0.03f) * baseColor;
    float alpha = mat.baseColorFactor.a * baseTex.a;
    float3 encoded = pow(saturate((diffuse + specular) * radiance + ambient), 1.0f / 2.2f);
    return float4(encoded, alpha);
}

float4 psMatResolve(float4 fragPos : SV_Position) : SV_Target
{
    const uint2 pixel  = uint2(fragPos.xy);
    const uint  width  = uint(g_width);
    const uint  height = uint(g_height);
    if (pixel.x >= width || pixel.y >= height)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);   // 超界（理论上不会，全屏三角形已裁剪）

    const uint64_t v = g_visSRV[visIndex(pixel, width)];
    // 空像素 = clear 值（无几何覆盖）。discard 让 viewer 清屏色透出，与 M0.4 一致。
    if (v == VIS_EMPTY)
        discard;

    uint clusterId, triId;
    float deviceZ;
    visUnpack(v, clusterId, triId, deviceZ);

    // 簇元数据：[vertexBase, triCount, localBase, materialId]。M7.2：槽 3 =
    // 子网格序号（.nmesh NSUB 段约定，initCull 填充），即材质间接表下标。
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

    // M9.0：几何反解数学已抽进 gbuffer_common.slangh（S9/S10/S12 共用契约）。
    // 纯搬移：表达式逐字未改，保形由 S7 / pbr-direct / s8.2 spot 回归实测。
    const GBufGeometry gbuf = gbInterpolateVisTriangle(
        pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2],
        uv[0], uv[1], uv[2], g_mvp, g_model, fragPos.xy, g_width, g_height);
    const float3 worldPos = gbuf.worldPos;
    const float3 normal   = gbuf.normal;
    const float2 uvF      = gbuf.uv;
    const float2 duv_dx   = gbuf.ddxUv;
    const float2 duv_dy   = gbuf.ddyUv;

    const uint mode = uint(g_options.x);
    if (mode == 1u) {                       // 法线可视化
        return float4(normalize(normal) * 0.5f + 0.5f, 1.0f);
    }
    if (mode == 2u) {                       // uv 可视化
        return float4(uvF, 0.0f, 1.0f);
    }
    if (mode == 5u) {                       // PBR 直光（复刻 model.slang::shadePbr）
        const float4 lit = shadePbrGrad(mat, normal, worldPos, uvF, duv_dx, duv_dy);
        // M-WEA2：raster 直光逐像素高度雾（与 csDirect 同口径；alpha 透传）
        const float3 toP = worldPos - g_cameraPos.xyz;
        const float  L   = length(toP);
        const float3 col = applyHeightFog(lit.rgb, g_cameraPos.y, toP / max(L, 1.0e-4f), L,
                                          g_rFogA.x, g_rFogA.y, g_rFogB.rgb, 1.0e4f);
        return float4(col, lit.a);
    }

    // mode 0：文本化（复刻 model.slang::shade 的 mode 0，与 M0.4 传统画法等价）。
    // M7.2：贴图按材质槽选（因子不参与——model.slang mode 0 也不乘因子）。
    float4 base = g_baseColorTex[NonUniformResourceIndex(mat.baseColorSlot)]
        .SampleGrad(g_sampler, uvF, duv_dx, duv_dy);
    float3 n = normalize(normal);
    float ndl = abs(dot(n, normalize(g_lightDir.xyz)));
    float3 albedo = base.rgb * 2.2f;
    // M10.2：g_options.z=1（probe gather 态）时 ambient 因子置零 = 真替换——
    // 间接光由 probe_gather.slang csGatherCompose 补上（Q13-2 防双重计费）。
    // probe-viz 场景走本 mode 0 分支（shadePbrGrad 只在 mode 5），开关必须落这；
    // g_options.z 全路径空闲（g_options.y 被 clusterCount / pbr-naive 双重占用）。
    const float amb = (g_options.z > 0.5f) ? 0.03f : g_lightDir.w;
    // M-WEA3（Q-W3-1=A2）：mode 0 湿化——viewer 缺省 shading=0（textured 简单
    // 直光），probe 主画面走本分支；湿化 = albedo 暗化（无 roughness/specular，
    // 与 shadePbrGrad 的 albedo 项同式同源）。wetness=0 → 精确恒等 bytes 保形。
    // ⚠️ M-WEA2 假绿同款教训：probe 主画面走 mode 0 非 mode 5——湿化必须双分支
    // 同接（fog 生效 / wetness 无效的事故 = 只接 mode5 漏 mode0）。
    const float wetW0 = g_rFogA.z * lerp(0.35f, 1.0f, saturate(n.y));
    albedo *= lerp(1.0f, 0.65f, wetW0);
    float3 lit = min(albedo * (amb + (1.0f - amb) * ndl), 1.0f);
    // M-WEA2：probe-viz / mode 0 直光同样逐像素雾化（与 mode 5 同口径）
    const float3 toP = worldPos - g_cameraPos.xyz;
    const float  L   = length(toP);
    lit = applyHeightFog(lit, g_cameraPos.y, toP / max(L, 1.0e-4f), L,
                         g_rFogA.x, g_rFogA.y, g_rFogB.rgb, 1.0e4f);
    return float4(lit, 1.0f);
}
