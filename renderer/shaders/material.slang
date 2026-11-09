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
//   b1 = LightConstants (80B, 仅 mode 5 用；复用 PBR-DIRECT-PLAN 的布局)
//   b2 = MatResolveConstants (16B): (width, height, _, _)
//   t0 = vis buffer SRV（与 raster u0 同一块 u64 buffer 的 SRV 视角）
//   t1..t3 = vertexPool / localIndices / clusterMeta（与 model.slang vsCull 同源）
//   t4 = base color 贴图（RGBA8_UNORM，无 SRGB 视图，srgbToLinear 在 shader 内）
//   t5 = metallicRoughness 贴图（RGBA8_UNORM，G=roughness B=metallic）
//   s0 = sampler
// SPIR-V 绑定平移：b→256, t→0, s→128, u→384（本 pass 无 UAV）；DX12 用各自 slot。
//
// 介质偏离：NVRHI 无 R64 格式，vis buffer 是 RWStructuredBuffer<uint64_t>（见
// visbuf_common.slangh 与 docs/s5/UE-DIFFS.md D15）。

#include "visbuf_common.slangh"

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

StructuredBuffer<uint64_t> g_visSRV          : register(t0);
StructuredBuffer<float>    g_vertexPool      : register(t1);
StructuredBuffer<uint>     g_localIndices    : register(t2);
StructuredBuffer<uint>     g_clusterMeta     : register(t3);
Texture2D<float4>          g_baseColor        : register(t4);
Texture2D<float4>          g_metallicRoughness : register(t5);
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

float4 shadePbrGrad(float3 normal, float3 worldPos, float2 uv, float2 ddx_uv, float2 ddy_uv)
{
    float3 n = normalize(normal);
    float3 v = normalize(g_cameraPos.xyz - worldPos);
    float3 l = normalize(g_lightDirW.xyz);
    float3 h = normalize(l + v);
    float NdotL = saturate(dot(n, l));
    float NdotV = max(dot(n, v), 1e-4f);
    float NdotH = saturate(dot(n, h));
    float VdotH = saturate(dot(v, h));

    float4 baseTex = g_baseColor.SampleGrad(g_sampler, uv, ddx_uv, ddy_uv);
    float3 baseColor = g_baseColorF.rgb *
        (g_mrF.z >= 0.5f ? baseTex.rgb : srgbToLinear(baseTex.rgb));
    float4 mrTex = g_metallicRoughness.SampleGrad(g_sampler, uv, ddx_uv, ddy_uv);
    float roughness = clamp(mrTex.g * g_mrF.y, 0.045f, 1.0f);
    float metallic  = saturate(mrTex.b * g_mrF.x);

    float3 F0     = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    float3 cDiff  = baseColor * (1.0f - metallic);
    float  D = D_GGX(roughness, NdotH);
    float  V = V_SmithGGXCorrelated(roughness, NdotV, NdotL);
    float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
    float3 radiance = g_lightColor.rgb * g_lightColor.a * NdotL;
    float3 diffuse  = cDiff * (1.0f - F) * (1.0f / kPi);
    float3 specular = D * V * F;
    float3 ambient  = 0.03f * baseColor;
    float alpha = g_baseColorF.a * baseTex.a;
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

    // 簇元数据：[vertexBase, triCount, localBase, materialId]。materialId 槽（pad）
    // 当前恒 0（单材质）；M7.2 多材质时改走材质间接表（bindless 数组，§2.7 单 set）。
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];
    const uint materialId = g_clusterMeta[metaBase + 3u];

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

    // 用与光栅 pass 相同的 g_mvp 重建裁剪坐标（w 一并保留，供透视正确插值）。
    float4 clip[3];
    clip[0] = mul(g_mvp, float4(pos[0], 1.0f));
    clip[1] = mul(g_mvp, float4(pos[1], 1.0f));
    clip[2] = mul(g_mvp, float4(pos[2], 1.0f));

    // ---- The-Forge 解析式 barycentrics + 像素导数（屏幕空间，y-down）----
    // 顶点屏幕坐标：NDC 经 viewport 变换（y 翻转，屏幕原点左上）。SV_Position.xy
    // 已在同一直角系（像素，y-down），故片元位置直接取 fragPos.xy，无需再变换。
    float w[3];
    float2 s[3];
    for (uint i = 0u; i < 3u; ++i) {
        w[i] = 1.0f / clip[i].w;
        const float2 ndc = clip[i].xy * w[i];
        s[i] = float2((ndc.x * 0.5f + 0.5f) * width,
                      (1.0f - (ndc.y * 0.5f + 0.5f)) * height);
    }
    const float2 f = fragPos.xy;
    const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y)
                     - (s[2].x - s[0].x) * (s[1].y - s[0].y);

    // 屏幕空间 barycentrics（sum = 1）。
    const float b1 = ((f.x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (f.y - s[0].y)) / area;
    const float b2 = ((f.x - s[1].x) * (s[0].y - s[1].y) - (s[0].x - s[1].x) * (f.y - s[1].y)) / area;
    const float b0 = 1.0f - b1 - b2;

    // 屏幕空间 barycentrics 的每像素导数。
    const float db1_dx =  (s[2].y - s[0].y) / area;
    const float db1_dy = -(s[2].x - s[0].x) / area;
    const float db2_dx =  (s[0].y - s[1].y) / area;
    const float db2_dy = -(s[0].x - s[1].x) / area;
    const float db0_dx = -(db1_dx + db2_dx);
    const float db0_dy = -(db1_dy + db2_dy);

    // 透视正确插值：attr = (Σ b_i/w_i · attr_i) / (Σ b_i/w_i)。
    const float invW   = b0 / w[0] + b1 / w[1] + b2 / w[2];
    const float3 bcw   = float3(b0 / w[0], b1 / w[1], b2 / w[2]);

    float3 wp[3], wn[3];
    for (uint j = 0u; j < 3u; ++j) {
        wp[j] = mul(g_model, float4(pos[j], 1.0f)).xyz;
        wn[j] = mul((float3x3)g_model, nrm[j]);
    }
    const float3 worldPos = (bcw.x * wp[0] + bcw.y * wp[1] + bcw.z * wp[2]) / invW;
    const float3 normal   = normalize((bcw.x * wn[0] + bcw.y * wn[1] + bcw.z * wn[2]) / invW);
    const float2 uvF      = (bcw.x * uv[0]  + bcw.y * uv[1]  + bcw.z * uv[2])  / invW;

    // uv 的透视正确像素导数（materialId 单材质时不参与选纹理，但导数用于 SampleGrad）。
    const float dInvW_dx = db0_dx / w[0] + db1_dx / w[1] + db2_dx / w[2];
    const float dInvW_dy = db0_dy / w[0] + db1_dy / w[1] + db2_dy / w[2];
    const float2 duv_dx = ((db0_dx / w[0]) * uv[0] + (db1_dx / w[1]) * uv[1] + (db2_dx / w[2]) * uv[2]) / invW
                        - uvF * (dInvW_dx / invW);
    const float2 duv_dy = ((db0_dy / w[0]) * uv[0] + (db1_dy / w[1]) * uv[1] + (db2_dy / w[2]) * uv[2]) / invW
                        - uvF * (dInvW_dy / invW);

    const uint mode = uint(g_options.x);
    if (mode == 1u) {                       // 法线可视化
        return float4(normalize(normal) * 0.5f + 0.5f, 1.0f);
    }
    if (mode == 2u) {                       // uv 可视化
        return float4(uvF, 0.0f, 1.0f);
    }
    if (mode == 5u) {                       // PBR 直光（复刻 model.slang::shadePbr）
        return shadePbrGrad(normal, worldPos, uvF, duv_dx, duv_dy);
    }

    // mode 0：文本化（复刻 model.slang::shade 的 mode 0，与 M0.4 传统画法等价）。
    // 注释：materialId 在 M7.1 恒 0，此处不分支；M7.2 多材质时按 materialId 走间接表。
    float4 base = g_baseColor.SampleGrad(g_sampler, uvF, duv_dx, duv_dy);
    float3 n = normalize(normal);
    float ndl = abs(dot(n, normalize(g_lightDir.xyz)));
    float3 albedo = base.rgb * 2.2f;
    float3 lit = min(albedo * (g_lightDir.w + (1.0f - g_lightDir.w) * ndl), 1.0f);
    return float4(lit, 1.0f);
}
