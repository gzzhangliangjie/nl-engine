// M0.4 model shader — single source of truth (SPIR-V + DXIL from this one file).
// See docs/SHADER-CONVENTIONS.md: entry points, resource registers and Vulkan binding shifts.
//
// Resource registers are shared by both targets:
//   b0 = SceneConstants (volatile constant buffer, written every frame)
//   t0 = base color texture, s0 = sampler
// On Vulkan, slangc's -fvk-b-shift / -fvk-s-shift map them to the binding numbers
// NVRHI's VulkanBindingOffsets expects (CB 256, SRV 0, sampler 128).
//
// M0.8 debug shading: g_options.x selects the shading mode
//   0 = textured (M0.4 path, untouched for baseline reproducibility)
//   1 = world-space normal -> color
//   2 = uv -> color
//   3 = triangle id (SV_PrimitiveID, integer hash) -> color
//   4 = cluster color (M1.3): per-vertex CLUSTERCOLOR baked by the cluster scene
// pbr-direct: 5 = Cook-Torrance PBR direct lighting (glTF metallicRoughness).
//   NOTE: 4 was already taken by cluster color (M1.3), so pbr is 5 — the plan
//   doc's "4" predates the cluster collision; renumbering cluster would touch
//   viewer defaults and historical baselines for zero benefit.
//   Scope limit (binding-layout reality, see §2.7): the pbr branch lives ONLY in
//   psMain. The shared shade() body is also compiled into psCluster/psCull whose
//   pipelines bind t1..t4 as STRUCTURED BUFFERS (vertex pool / indices / meta /
//   visible list) — if psCull's SPIR-V referenced t1 as a Texture2D, the same
//   descriptor slot would carry two resource types (illegal in a Vulkan
//   descriptor set layout, DX12 root param conflict). So: normal (.glb) and
//   skin scenes can select pbr; cluster/cull/dag scenes cannot (viewer forces
//   mode 4 there anyway).
//
// Binding notes (per-entry-point compilation):
//   t1 in the psMain call graph = metallicRoughness texture. The SAME register
//   number is g_vertexPool in the vsCull call graph — that is not a conflict
//   as long as the two never meet in one entry point: slangc compiles each
//   entry separately and only includes resources the entry references; both
//   bindings live in vk set 0 slot 1 of their own pipeline.

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model, column-major
    float4x4 g_model;      // model matrix, for normal transform
    float4   g_lightDir;   // xyz = world-space light direction, w = ambient
    float4   g_options;    // x = shading mode (0 textured / 1 normal / 2 uv / 3 tri-id / 4 cluster / 5 pbr)
};

// pbr-direct: light/material constants, 80 B (5 x float4). Kept out of
// SceneConstants on purpose — it is strictly 160 B with a no-whole-memcpy rule.
cbuffer LightConstants : register(b1)
{
    float4   g_lightDirW;    // xyz = direction TOWARDS the light (same convention as g_lightDir), w unused
    float4   g_lightColor;   // rgb = color, a = intensity
    float4   g_cameraPos;    // xyz = world-space camera position (FrameParams.eye)
    float4   g_baseColorF;   // glTF baseColorFactor (spec default 1,1,1,1)
    float4   g_mrF;          // x = metallicFactor, y = roughnessFactor, z = pbr-naive debug switch (>=0.5 skips sRGB->linear)
};

// M-E1b.4：ECS 方向灯 2D 阴影——psScene 专用（shadePbr 不声明不读，pbr-direct 零波及）。
// 正交 lightViewProj 由 CPU 侧按场景联合包围球预算；R32F 阴影图 t7（4 层
// Texture2DArray，1024x1024 POT，§2.8 精神），整数 Load 采样 → 无采样器状态、
// 双后端逐位一致（浮点 uv 采样会引入 DXIL/SPIR-V 差异）。无阴影场景时
// psScene 的 shadowFactor 恒为字面量 1.0f，x*1.0f 在 IEEE 下精确 → M-E1b.3 基线逐字节不变。
cbuffer ShadowConstants : register(b2)
{
    float4x4 g_shadowViewProj[4];   // 每阴影灯一个正交 lightViewProj（列主序，D3D z∈[0,1]）
    float4   g_shadowParams[4];     // x=enabled(0/1), y=depth bias, z=mapSize, w 未用
};
Texture2DArray<float> g_shadowMap : register(t7);

Texture2D<float4> g_baseColor : register(t0);
Texture2D<float4> g_metallicRoughness : register(t1);
SamplerState      g_sampler   : register(s0);
// P9 V2 全 PBR：psScene 专属贴图组。per-entry 纪律（t1 先例）：t2/t3/t4 在
// visbuf/cull 调用图里是 StructuredBuffer（g_localIndices/g_clusterMeta/
// g_visibleList），与 psScene 各自单独编译、bindings 互不见面，不构成冲突。
// 无对应贴图的 primitive 绑共享中性兜底纹理（normal=(0.5,0.5,1,1) 等），
// shader 按 matFlags 分支，兜底路径零采样。
Texture2D<float4> g_normalTex    : register(t2);
Texture2D<float4> g_occlusionTex : register(t3);
Texture2D<float4> g_emissiveTex  : register(t4);
// M1.3 entry points (two pairs, one source file):
//   vsMain / psMain        - 3-attribute signature (normal scene). D3D12's debug
//                            layer treats "VS input not provided by the input
//                            layout" as an error, so the cluster attribute must
//                            live in a separate entry-point pair.
//   vsCluster / psCluster  - 4-attribute signature (+CLUSTERCOLOR), cluster scene.
// Wireframe is orthogonal: it is a raster fill mode, not a shader mode.
//
// ============================================================================
// M-E1b.0：ECS 场景入口对（vsScene/psScene）—— N 物体 M 方向灯实例化渲染
// （P8 ECS 轨道，viewer --scene 消费）。口径来源：Carrot ModelRenderSystem
// （实例世界矩阵 + 实例 tint 走实例缓冲、灯数据走缓冲不走 cbuffer；多灯 =
// 方向灯数组，每灯 direction+color+intensity；M-E1b.2 增补点光源 type/position/range）。
// 资源布局（既有入口一个字符不动，本段为文件尾独立新增）：
//   t5 = g_instanceData：每实例 7 个 float4（M-E1b.3 由 5 扩到 7）：
//        [0..3] = 世界矩阵列主序 4 列（TransformComponent::getWorldMatrix()），
//        [4]    = (tint.rgb, 0)（ModelComponent::tint），
//        [5]    = (roughness, metallic, 0, 0)（M-E1b.3 逐实例 PBR 材质，缺省 1/0）
//   t6 = g_lights：[0] = (lightCount, ambient, 1/ΣI(方向灯), pointCount)，
//        之后每灯 3 个 float4（M-E1b.2 从 2 个扩展）：
//        {dir.xyz, intensity}、{color.rgb, type}、{position.xyz, InvRadius}
//        type=0 方向灯 / 1 点光；InvRadius=1/range（UE LightData.InvRadius 口径）。
//   b0/t0/s0 与既有入口共用（SceneConstants volatile CB / 贴图 / 采样器）。
// 独立入口对的纪律同 vsCluster 先例：vsScene 输入多带 SV_InstanceID（D3D12
// debug layer 对「VS 签名 vs input layout」严格对应，不与既有签名混用），
// 输出多带 TINT 插值量传给 psScene。
// ============================================================================

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 worldPos : WORLDPOS;   // pbr-direct: world-space position (view vector V)
};

VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.position = mul(g_mvp, float4(input.position, 1.0f));
    output.normal   = mul((float3x3)g_model, input.normal);
    output.uv       = input.uv;
    output.worldPos = mul(g_model, float4(input.position, 1.0f)).xyz;
    return output;
}

// M1.3: cluster scene adds one per-vertex attribute (baked cluster color).
struct VSInputCluster
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 clusterColor : CLUSTERCOLOR;
};

struct VSOutputCluster
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 clusterColor : CLUSTERCOLOR;
};

VSOutputCluster vsCluster(VSInputCluster input)
{
    VSOutputCluster output;
    output.position = mul(g_mvp, float4(input.position, 1.0f));
    output.normal   = mul((float3x3)g_model, input.normal);
    output.uv       = input.uv;
    output.clusterColor = input.clusterColor;
    return output;
}

// M0.8: the pixel shader receives the rasterizer-generated primitive id directly.
// It is a system value: the VS output does not carry it, and signature matching on
// both DXIL (injected by the rasterizer) and SPIR-V (gl_PrimitiveID built-in) accepts it.
struct PSInput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 worldPos : WORLDPOS;
    uint   triId    : SV_PrimitiveID;
};

// M1.3: cluster variant — same fields plus the interpolated (constant) cluster color.
struct PSInputCluster
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 clusterColor : CLUSTERCOLOR;
    uint   triId    : SV_PrimitiveID;
};

// Deterministic integer hash (pcg-style, no float randomness): the same triangle
// always maps to the same color, so screenshot baselines are reproducible.
float3 triIdColor(uint id)
{
    uint h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0f;
}

// pbr-direct helpers. Formula authority: Filament §4 口径，三处参考对拍——
//   D_GGX / V_SmithGGXCorrelated / F_Schlick 与 bevy pbr_lighting.wesl 逐式同构
//   （D 的数值稳定重写 = walter07；V = Smith 高度相关精确式，UE BRDF.ush 的
//   Vis_SmithJoint 同式、其 Approx 版是无 sqrt 代价换精度）；
//   F0 = mix(0.04, baseColor, metallic) / diffuse = (1-F)*c_diff/π 与 Carrot
//   brdf.glsl（glTF spec 路线）一致。
// glTF channel semantics: metallicRoughness G = roughness, B = metallic
// (glTF 2.0 spec §3.9.3) — 记反高光会诡异。
float3 srgbToLinear(float3 c)
{
    // Exact piecewise conversion. t0 stays RGBA8_UNORM (mode-0 baseline is
    // byte-frozen), so pbr mode converts explicitly instead of relying on an
    // SRGB view format — changing t0's format would shift every old baseline.
    return select(c <= 0.04045f, c / 12.92f, pow((c + 0.055f) / 1.055f, 2.4f));
}

static const float kPi = 3.14159265f;

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

// Cook-Torrance direct light, single directional light + constant ambient.
// Only reachable on the normal .glb scene (viewer forces mode 4 on nmesh scenes).
float4 shadePbr(float3 normal, float3 worldPos, float2 uv)
{
    float3 n = normalize(normal);
    float3 v = normalize(g_cameraPos.xyz - worldPos);
    float3 l = normalize(g_lightDirW.xyz);          // towards the light (mode-0 convention)
    float3 h = normalize(l + v);
    float NdotL = saturate(dot(n, l));
    float NdotV = max(dot(n, v), 1e-4f);            // avoid V divide blowups at grazing
    float NdotH = saturate(dot(n, h));
    float VdotH = saturate(dot(v, h));

    float4 baseTex = g_baseColor.Sample(g_sampler, uv);
    float3 baseColor = g_baseColorF.rgb *
        (g_mrF.z >= 0.5f ? baseTex.rgb : srgbToLinear(baseTex.rgb));  // z>=0.5 = 误用 UNORM 直读的对照路径
    float4 mrTex = g_metallicRoughness.Sample(g_sampler, uv);
    float roughness = clamp(mrTex.g * g_mrF.y, 0.045f, 1.0f);   // 下限防 D 尖峰锯齿（k != 0）
    float metallic  = saturate(mrTex.b * g_mrF.x);

    float3 F0     = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    float3 cDiff  = baseColor * (1.0f - metallic);   // metallic 下 baseColor 不进 diffuse

    float  D = D_GGX(roughness, NdotH);
    float  V = V_SmithGGXCorrelated(roughness, NdotV, NdotL);
    float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);

    float3 radiance = g_lightColor.rgb * g_lightColor.a * NdotL;
    float3 diffuse  = cDiff * (1.0f - F) * (1.0f / kPi);
    float3 specular = D * V * F;
    // M10.2：g_options.z=1（probe gather 模式）时 ambient 置零——间接光由
    // probe_gather.slang csGatherCompose 补上（Q13-2 真替换，防双重计费）。
    // g_options.z 全路径空闲（g_options.y 被 frustumCull 的 clusterCount 与
    // params.options[0] pbr-naive 开关双重占用——首版误占 g_options.y 导致
    // pbr-direct/naive ambient 被误清零，全量回归实锤后挪位）。
    const float ambientScale = (g_options.z > 0.5f) ? 0.0f : 0.03f;
    float3 ambient  = ambientScale * baseColor;

    float alpha = g_baseColorF.a * baseTex.a;
    // 输出端 gamma 编码（pow 1/2.2，显示变换——不算 tonemap，plan 边界不碰后处理）。
    // Lo 是线性辐射度量，交换链 UNORM 直写，不编码整帧会过暗。
    float3 encoded = pow(saturate((diffuse + specular) * radiance + ambient), 1.0f / 2.2f);
    return float4(encoded, alpha);
}

// Shared shading body. clusterColor only participates in mode 4; callers on the
// normal signature pass a dummy value. Mode 5 (pbr) is NOT here on purpose —
// see the binding-scope note at the top of this file: shade() is also compiled
// into psCluster/psCull, which must not reference t1/b1.
float4 shade(float3 normal, float2 uv, uint triId, uint mode, float3 clusterColor)
{
    if (mode == 1u)
    {
        float3 n = normalize(normal);
        return float4(n * 0.5f + 0.5f, 1.0f);   // world-space normal, [-1,1] -> [0,1]
    }
    if (mode == 2u)
    {
        return float4(uv, 0.0f, 1.0f);          // raw uv: seam / continuity visible
    }
    if (mode == 3u)
    {
        return float4(triIdColor(triId), 1.0f);
    }
    if (mode == 4u)
    {
        // M1.3: cluster visualization. All three vertices of a triangle carry the
        // same baked color (one cluster), so interpolation is a constant.
        return float4(clusterColor, 1.0f);
    }

    // mode 0: textured path from M0.4, kept byte-for-byte identical.
    float4 base = g_baseColor.Sample(g_sampler, uv);
    float3 n = normalize(normal);
    // abs() keeps back faces lit too: with culling disabled the inner wall is visible.
    float ndl = abs(dot(n, normalize(g_lightDir.xyz)));
    // Khronos 官方 Suzanne_BaseColor.png 是中灰底；乘一档曝光让贴图细节（含黄色眼睛）
    // 在验收截图里肉眼可见，避免看起来像「没贴图的纯色模型」。
    float3 albedo = base.rgb * 2.2f;
    // M10.2：g_options.z=1（probe gather 态）时 ambient 因子置零 = 真替换——
    // 间接光由 probe_gather.slang csGatherCompose 补上（Q13-2 防双重计费）。
    // g_options.z 全路径空闲（g_options.y 被 frustumCull clusterCount /
    // params.options[0] pbr-naive 开关双重占用——首版误占 g_options.y 且漏改
    // 本条 textured 路径，全量回归 + PNG 逐字节对照双重实锤后修复）。
    const float amb = (g_options.z > 0.5f) ? 0.0f : g_lightDir.w;
    float3 lit = min(albedo * (amb + (1.0f - amb) * ndl), 1.0f);
    return float4(lit, 1.0f);
}

float4 psMain(PSInput input) : SV_Target
{
    uint mode = uint(g_options.x);
    // pbr-direct: mode 5 lives only on this entry (normal .glb / skin scenes);
    // see the binding-scope note at the top of this file.
    if (mode == 5u) return shadePbr(input.normal, input.worldPos, input.uv);
    return shade(input.normal, input.uv, input.triId, mode, float3(0.0f, 0.0f, 0.0f));
}

float4 psCluster(PSInputCluster input) : SV_Target
{
    return shade(input.normal, input.uv, input.triId, uint(g_options.x), input.clusterColor);
}

// ============================================================================
// M4.0 (S4.0): GPU vertex pulling. Single drawIndexedIndirect, instanceCount =
// total cluster count (full draw — no culling yet; M4.1 adds compute culling).
//
// Per instance i (SV_InstanceID) we pull cluster i's meshlet out of three SSBOs:
//   g_vertexPool   : all cluster vertices, flattened (8 floats each: pos/normal/uv)
//   g_localIndices : per-cluster local triangle indices (uint32, 3 per triangle)
//   g_clusterMeta  : per-cluster [vertexBase, triangleCount, localIndexBase, pad]
//
// The index buffer is an identity range [0, stride) with stride = 3*maxTriCount,
// so SV_VertexID (= the index value) is the within-stride position; triInCluster
// = SV_VertexID/3. Triangles beyond triangleCount are dummy'd to NaN so the
// rasterizer discards them. This reproduces the cluster-mode geometry exactly.
// ============================================================================

StructuredBuffer<float> g_vertexPool  : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);
StructuredBuffer<uint>  g_visibleList  : register(t4);

struct VSCullOutput {
    float4 position      : SV_Position;
    float3 normal        : NORMAL;
    float2 uv            : TEXCOORD;
    float3 clusterColor  : CLUSTERCOLOR;
};

// vertexIndex 是 identity 索引缓冲的值（0..stride-1）；instanceIndex 是可见列表的
// 槽位，簇号经 g_visibleList 间接寻址得到（M4.1）。剔除关闭时列表是恒等映射，
// 与 M4.0 逐像素一致；剔除开启时槽位序不确定，但簇色按簇号取、绘制不透明 +
// 深度测试，像素与顺序无关。
VSCullOutput vsCull(uint instanceIndex : SV_InstanceID,
                    uint vertexIndex   : SV_VertexID) {
    VSCullOutput output;
    // 默认 dummy：NaN 裁剪坐标，光栅化阶段整三角被丢弃（bevy 同式）。
    output.position     = asfloat(0x7fc00000u).xxxx;
    output.normal       = 0.0f;
    output.uv           = 0.0f;
    output.clusterColor = 0.0f;

    const uint clusterId  = g_visibleList[instanceIndex];
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint triCount   = g_clusterMeta[metaBase + 1u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];

    const uint triInCluster = vertexIndex / 3u;
    const uint vertInTri    = vertexIndex % 3u;
    if (triInCluster >= triCount) return output;   // padding 三角 -> dummy

    const uint localIdx = g_localIndices[localBase + vertexIndex];
    const uint gv = vertexBase + localIdx;          // 顶点池里的全局顶点
    const uint b  = gv * 8u;                        // 8 float / 顶点
    const float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);
    const float3 nrm = float3(g_vertexPool[b + 3u], g_vertexPool[b + 4u], g_vertexPool[b + 5u]);
    const float2 uv  = float2(g_vertexPool[b + 6u], g_vertexPool[b + 7u]);

    output.position     = mul(g_mvp, float4(pos, 1.0f));
    output.normal       = mul((float3x3)g_model, nrm);
    output.uv           = uv;
    // 簇色 = pcg(簇号)，与 cluster 场景 CPU 端 clusterColor() 同式 -> 截图逐像素一致。
    // 注意是簇号不是槽位：剔除开启时槽位序不确定，按槽位取色会闪。
    output.clusterColor = triIdColor(clusterId);
    return output;
}

struct PSCullInput {
    float4 position      : SV_Position;
    float3 normal        : NORMAL;
    float2 uv            : TEXCOORD;
    float3 clusterColor  : CLUSTERCOLOR;
    uint   triId         : SV_PrimitiveID;
};

float4 psCull(PSCullInput input) : SV_Target
{
    return shade(input.normal, input.uv, input.triId, uint(g_options.x), input.clusterColor);
}

// ============================================================================
// M-E1b.0 / M-E1b.2：ECS 场景入口对（资源布局见文件头注释段）。
// M-E1b.2 点光源：t6 每灯扩为 3 个 float4，vsScene 多输出 WORLDPOS 插值量，
// psScene 按 type 分支；方向灯分支的表达式保持 M-E1b.0 原样（基线逐字节依赖）。
// ============================================================================

StructuredBuffer<float4> g_instanceData : register(t5);
StructuredBuffer<float4> g_lights       : register(t6);

struct VSInputScene
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct VSOutputScene
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 tint     : TINT;
    float3 worldPos : WORLDPOS;   // M-E1b.2：点光衰减需要世界空间片元位置
    float  roughness : ROUGH;     // M-E1b.3：逐实例 PBR 粗糙度（t5[base+5].x）
    float  metallic  : METAL;     // M-E1b.3：逐实例 PBR 金属度（t5[base+5].y）
    float  normalScale : NORMALSCALE;   // P9 V2：t5[base+5].z（glTF normalTexture.scale）
    nointerpolation uint matFlags : MATFLAGS;   // P9 V2：t5[base+5].w（材质位掩码）
    float4 emissiveOcc : EMISSIVEOCC;   // P9 V2：t5[base+6]（emissiveFactor.rgb, occlStrength）
};

VSOutputScene vsScene(VSInputScene input, uint instanceId : SV_InstanceID)
{
    VSOutputScene output;
    const uint base = instanceId * 7u;   // M-E1b.3：t5 扩到 7 个 float4
    // [0..3] 是列主序的 4 列；HLSL 的 float4x4 构造参数按行填充，transpose 一次
    // 还原成「列 = c0..c3」的数学矩阵（mul(world, v) 即标准列向量变换）。
    const float4x4 world = transpose(float4x4(
        g_instanceData[base + 0u],
        g_instanceData[base + 1u],
        g_instanceData[base + 2u],
        g_instanceData[base + 3u]));
    const float3 worldPos = mul(world, float4(input.position, 1.0f)).xyz;
    output.position = mul(g_mvp, float4(worldPos, 1.0f));
    output.normal   = mul((float3x3)world, input.normal);
    output.uv       = input.uv;
    output.tint     = g_instanceData[base + 4u].xyz;
    output.worldPos = worldPos;
    output.roughness = g_instanceData[base + 5u].x;   // M-E1b.3：逐实例 PBR 材质
    output.metallic  = g_instanceData[base + 5u].y;
    output.normalScale = g_instanceData[base + 5u].z;   // P9 V2
    output.matFlags  = (uint)(g_instanceData[base + 5u].w + 0.5f);   // P9 V2
    output.emissiveOcc = g_instanceData[base + 6u];   // P9 V2
    return output;
}

// M-E1b.4：阴影 depth pass（b0 = lightViewProj，shadow pass 语义下复用
// SceneConstants 的 g_mvp 槽位；t5 实例缓冲与 vsScene 同一套）。正交投影下
// z/w ∈ [0,1] 且线性，PS 直接落 R32F——整数精度双后端一致。
struct VSOutputShadow
{
    float4 position : SV_Position;
};

VSOutputShadow vsShadow(VSInputScene input, uint instanceId : SV_InstanceID)
{
    VSOutputShadow output;
    const uint base = instanceId * 7u;
    // 与 vsScene 相同的列主序世界矩阵还原（transpose 一次）。
    const float4x4 world = transpose(float4x4(
        g_instanceData[base + 0u],
        g_instanceData[base + 1u],
        g_instanceData[base + 2u],
        g_instanceData[base + 3u]));
    const float3 worldPos = mul(world, float4(input.position, 1.0f)).xyz;
    output.position = mul(g_mvp, float4(worldPos, 1.0f));   // g_mvp = lightViewProj
    return output;
}

float4 psShadow(VSOutputShadow input) : SV_Target
{
    return float4(input.position.z, 0.0f, 0.0f, 1.0f);      // 正交深度（已除 w）
}

struct PSInputScene
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 tint     : TINT;
    float3 worldPos : WORLDPOS;
    float  roughness : ROUGH;     // M-E1b.3：逐实例 PBR 粗糙度（VS 透传）
    float  metallic  : METAL;     // M-E1b.3：逐实例 PBR 金属度
    float  normalScale : NORMALSCALE;   // P9 V2
    nointerpolation uint matFlags : MATFLAGS;   // P9 V2
    float4 emissiveOcc : EMISSIVEOCC;   // P9 V2
};

// P9 V2：Mikkelsen 导数共切线框架（Mikkelsen 2011 "PerturbNormal"）。
// 世界坐标与 UV 的 ddx/ddy 就地构 TBN——不扩顶点格式、不要求 glTF TANGENT
// 属性（对拍：Filament material_parameter / bevy generate_tangents 均支持
// 此路径）。glTF 口径：UV V 轴向下、法线贴图 +Y 对齐 +V，导数框架天然一致
// 不翻转。退化（UV 无梯度 / 三角面退化）时保持几何法线。
float3 perturbNormal(float3 geomN, float3 tsN, float2 uv, float3 posW)
{
    const float3 dp1 = ddx(posW);
    const float3 dp2 = ddy(posW);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, geomN);
    const float3 dp1perp = cross(geomN, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    const float t2 = dot(T, T);
    const float b2 = dot(B, B);
    float3 n = geomN;
    if (t2 > 1e-12f || b2 > 1e-12f)
    {
        const float invmax = rsqrt(max(t2, b2));
        T *= invmax;
        B *= invmax;
        n = normalize(T * tsN.x + B * tsN.y + geomN * tsN.z);
    }
    return n;
}

float4 psScene(PSInputScene input) : SV_Target
{
    // M-E1b.3：逐实例 Cook-Torrance PBR 多灯（方向灯 + 点光），替换 M-E1b.2 的 Lambert。
    // 口径与 shadePbr() 同式（Filament §4 / bevy / UE / Carrot 对拍，见 shadePbr 注释）；
    // 多灯累加沿用 E1b.2 的「方向灯按 1/ΣI 归一化、点光绝对累加」结构，避免 ΣI≈4.7 过曝、
    // 保住各灯色温差异（归一化只做乘法、倒数 CPU 预算 → 双后端 bytes 逐位一致）。
    float4 base = g_baseColor.Sample(g_sampler, input.uv);
    float3 albedo = srgbToLinear(base.rgb) * input.tint;   // 贴图 sRGB→线性（t0 保持 UNORM 直读）；tint 线性乘子

    // P9 V2 全 PBR：matFlags 位掩码（batch 级常量，draw 内全实例同值 →
    // uniform 分支）。flags=0 路径与 P9 V1 逐表达式一致（bytes 兼容基线）。
    const uint matFlags = input.matFlags;
    const bool hasMR       = (matFlags & 1u) != 0u;
    const bool hasNormal   = (matFlags & 2u) != 0u;
    const bool hasOccl     = (matFlags & 4u) != 0u;
    const bool hasEmissive = (matFlags & 8u) != 0u;

    // 逐实例材质（t5[base+5]）：缺省 roughness=1 / metallic=0（matte dielectric）。
    // 下限防 D 尖峰（k != 0），见 shadePbr 同款 clamp 理由。
    // hasMR：glTF metallicRoughness 贴图 G=roughness B=metallic（spec §3.9.3.1）
    // 覆盖逐实例标量（与 shadePbr :228-233 同口径）。
    float roughness = clamp(input.roughness, 0.045f, 1.0f);
    float metallic  = saturate(input.metallic);
    if (hasMR)
    {
        const float4 mr = g_metallicRoughness.Sample(g_sampler, input.uv);
        roughness = clamp(mr.g, 0.045f, 1.0f);
        metallic  = saturate(mr.b);
    }

    // hasNormal：切线空间法线贴图扰动（导数共切线框架，Mikkelsen）。
    float3 n = normalize(input.normal);
    if (hasNormal)
    {
        const float3 ts = g_normalTex.Sample(g_sampler, input.uv).xyz * 2.0f - 1.0f;
        n = perturbNormal(n, float3(ts.xy * input.normalScale, ts.z),
                          input.uv, input.worldPos);
    }
    float3 v = normalize(g_cameraPos.xyz - input.worldPos); // 视向量 V（b1 cameraPos）
    float3 F0    = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 cDiff = albedo * (1.0f - metallic);              // metallic 下 baseColor 不进 diffuse

    const float lightCount      = g_lights[0].x;
    const float ambient         = g_lights[0].y;
    const float invSumIntensity = g_lights[0].z;            // 方向灯 1/ΣI（CPU 预算，仅方向灯）

    float3 Lo_dir = float3(0.0f, 0.0f, 0.0f);
    float3 Lo_point = float3(0.0f, 0.0f, 0.0f);
    // 每灯 3 个 float4：{dir.xyz, intensity}、{color.rgb, type}、{position.xyz, InvRadius}
    for (uint i = 0u; i < (uint)lightCount; ++i)
    {
        const float4 dirInt = g_lights[1u + i * 3u];
        const float4 color  = g_lights[2u + i * 3u];
        const float4 posRng = g_lights[3u + i * 3u];

        float3 L;
        float NdotL;
        float3 radiance;
        float shadowFactor = 1.0f;   // M-E1b.4：方向灯分支可改写；点光恒 1。
        if (color.w > 0.5f)   // 点光
        {
            // M-E1b.2 同款 UE DeferredLightingCommon.ush:480-486 实读口径：
            //   DistanceAttenuation = 1/(d²+1) * Square(saturate(1 - Square(d²/r²)))
            //   L = ToLight * rsqrt(d²)，NoL = saturate(dot(N,L))
            // CPU 端写 InvRadius=1/range（UE LightData.InvRadius 同款）；点光按绝对强度
            // 独立累加，不进方向灯的 1/ΣI 归一化（否则 att<<1 会让点光几乎看不见）。
            const float3 toLight = posRng.xyz - input.worldPos;
            const float d2 = dot(toLight, toLight);
            L = toLight * rsqrt(max(d2, 1e-8f));
            const float q = d2 * posRng.w * posRng.w;       // d² * InvRadius²
            const float radiusMask = saturate(1.0f - q * q);
            const float att = (1.0f / (d2 + 1.0f)) * radiusMask * radiusMask;
            NdotL = max(dot(n, L), 0.0f);                   // 点光单面（物理口径）
            radiance = color.rgb * dirInt.w * att;
        }
        else                  // 方向灯
        {
            // 与 E1b.2 同口径：abs() 关背面剔除时内壁也照亮；dirInt.xyz 已由 viewer 归一化。
            L = dirInt.xyz;
            NdotL = abs(dot(n, L));
            radiance = color.rgb * dirInt.w;

            // M-E1b.4：方向灯 2D 阴影。t6 posRng.w 复用为 shadowSlot+1（0=无阴影；
            // 方向灯分支原本不读该字段）。R32F 阴影图整数 texel Load + 3x3 PCF：
            // DX 视口约定 clip.y=+1 → 图像第 0 行（NVRHI Vulkan 视口换算与 DX 对齐，
            // 见 pipeline 注释），故接收侧 texel.y = (0.5 - ndc.y*0.5) * size。
            // ortho 下 clip.w==1，recDepth = clip.z 已在 [0,1]；视锥外全亮。
            const float slot = posRng.w;
            if (slot > 0.5f)
            {
                const uint si = (uint)(slot) - 1u;
                const float4 clip = mul(g_shadowViewProj[si], float4(input.worldPos, 1.0f));
                const float2 ndc = clip.xy / clip.w;
                const float recDepth = clip.z / clip.w;
                if (recDepth > 0.0f && recDepth < 1.0f &&
                    abs(ndc.x) <= 1.0f && abs(ndc.y) <= 1.0f)
                {
                    const float size = g_shadowParams[si].z;
                    // 坡度缩放 bias：MIN 混合存 nearest 深度，斜面上 texel 内深度跨度
                    // ∝ (1-NdotL)（PROBE-D2 实测：地面 ~0.004 / 头部斜面 ~0.028），
                    // 固定 bias 0.003 盖不住 → 满屏自阴影。0.035×(1-NdotL) 实测覆盖。
                    const float slopeBias = 0.035f * (1.0f - saturate(dot(n, L)));
                    const float bias = g_shadowParams[si].y + slopeBias;
                    const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
                    const int2 base = int2(floor(uv * size));
                    const int2 maxSize = int2(size, size) - int2(1, 1);
                    float lit = 0.0f;
                    [unroll]
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        [unroll]
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int2 t = clamp(base + int2(dx, dy), int2(0, 0), maxSize);
                            lit += (g_shadowMap.Load(int4(t, si, 0)) >= recDepth - bias)
                                       ? 1.0f : 0.0f;
                        }
                    }
                    shadowFactor = lit * (1.0f / 9.0f);
                }
            }
        }
        if (NdotL <= 0.0f) continue;

        const float3 h = normalize(L + v);
        const float NdotV = max(dot(n, v), 1e-4f);          // 掠射防 V 除零
        const float NdotH = saturate(dot(n, h));
        const float VdotH = saturate(dot(v, h));

        const float D = D_GGX(roughness, NdotH);
        const float Vis = V_SmithGGXCorrelated(roughness, NdotV, NdotL);
        const float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
        const float3 diffuse  = cDiff * (1.0f - F) * (1.0f / kPi);
        const float3 specular = D * Vis * F;
        const float3 brdf = (diffuse + specular) * NdotL;

        if (color.w > 0.5f) Lo_point += brdf * radiance;
        else                Lo_dir   += brdf * radiance * shadowFactor;
    }

    // ambient：flat irradiance = ambient·cDiff（无 IBL）。M-E1b.3：乘 (1-metallic)——
    // 金属无漫反射环境光，ambient 原样乘 albedo 会把金属暗部染成「粉底色」，
    // 还会抹平 metallic 0/1 的材质对比（E1b.3 验收主判据）。
    // 方向灯按 1/ΣI 归一化成加权平均（保色温差异、防过曝），点光绝对累加。
    // lightCount=0 时 invSumIntensity=0、Lo_dir=0 → 退化为纯 ambient。
    // 亮度锚定：Cook-Torrance 的 1/π 能量守恒 + 方向灯 1/ΣI 双重压缩，比 E1b.2
    // Lambert 整体暗 ~3×；shader 口径不动（与 shadePbr 同式），由场景布光补偿
    // （ambient 0.55 / 灯 color ×2.8，见 ecs-e1b*.scene）。
    // P9 V2：AO 只作用于环境光项（glTF spec：occlusion 只调 ambient/IBL，
    // 不动直接光）；emissive = factor × 贴图 sRGB→线性（t0 同口径），无贴图
    // 时 = factor 原值（glTF spec 允许只有 emissiveFactor 无贴图）。
    // flags=0 时 ao=1、emissive=0：×1.0f 与 +0.0f 在 IEEE 下精确 → bytes 兼容。
    float ao = 1.0f;
    if (hasOccl)
        ao = lerp(1.0f, g_occlusionTex.Sample(g_sampler, input.uv).r,
                  input.emissiveOcc.w);
    float3 emissive = input.emissiveOcc.xyz;
    if (hasEmissive)
        emissive *= srgbToLinear(g_emissiveTex.Sample(g_sampler, input.uv).rgb);
    float3 Lo = ambient * cDiff * ao + Lo_dir * invSumIntensity + Lo_point + emissive;
    float3 encoded = pow(saturate(Lo), 1.0f / 2.2f);        // 交换链 UNORM 直写 → gamma 编码
    return float4(encoded, 1.0f);
}
