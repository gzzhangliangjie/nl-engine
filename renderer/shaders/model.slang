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
//
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
//   t5 = g_instanceData：每实例 5 个 float4：
//        [0..3] = 世界矩阵列主序 4 列（TransformComponent::getWorldMatrix()），
//        [4]    = (tint.rgb, 0)（ModelComponent::tint）
//   t6 = g_lights：[0] = (lightCount, ambient, 1/ΣI(方向灯), pointCount)，
//        之后每灯 3 个 float4（M-E1b.2 从 2 个扩展）：
//        {dir.xyz, intensity}、{color.rgb, type}、{position.xyz, InvRadius}
//        type=0 方向灯 / 1 点光；InvRadius=1/range（UE LightData.InvRadius 口径）。
//   b0/t0/s0 与既有入口共用（SceneConstants volatile CB / 贴图 / 采样器）。
// 独立入口对的纪律同 vsCluster 先例：vsScene 输入多带 SV_InstanceID（D3D12
// debug layer 对「VS 签名 vs input layout」严格对应，不与既有签名混用），
// 输出多带 TINT 插值量传给 psScene。
// ============================================================================

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model, column-major
    float4x4 g_model;      // model matrix, for normal transform
    float4   g_lightDir;   // xyz = world-space light direction, w = ambient
    float4   g_options;    // x = shading mode (0 textured / 1 normal / 2 uv / 3 tri-id / 4 cluster)
};

Texture2D<float4> g_baseColor : register(t0);
SamplerState      g_sampler   : register(s0);

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
};

VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.position = mul(g_mvp, float4(input.position, 1.0f));
    output.normal   = mul((float3x3)g_model, input.normal);
    output.uv       = input.uv;
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

// Shared shading body. clusterColor only participates in mode 4; callers on the
// normal signature pass a dummy value.
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
    float3 lit = min(albedo * (g_lightDir.w + (1.0f - g_lightDir.w) * ndl), 1.0f);
    return float4(lit, 1.0f);
}

float4 psMain(PSInput input) : SV_Target
{
    return shade(input.normal, input.uv, input.triId, uint(g_options.x), float3(0.0f, 0.0f, 0.0f));
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
};

VSOutputScene vsScene(VSInputScene input, uint instanceId : SV_InstanceID)
{
    VSOutputScene output;
    const uint base = instanceId * 5u;
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
    return output;
}

struct PSInputScene
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 tint     : TINT;
    float3 worldPos : WORLDPOS;
};

float4 psScene(PSInputScene input) : SV_Target
{
    // mode 0 风格：base.rgb * 2.2f 曝光补偿（Khronos Suzanne 贴图偏暗）× 实例 tint。
    float4 base = g_baseColor.Sample(g_sampler, input.uv);
    float3 albedo = base.rgb * 2.2f * input.tint;
    float3 n = normalize(input.normal);

    const float lightCount      = g_lights[0].x;
    const float ambient         = g_lights[0].y;
    const float invSumIntensity = g_lights[0].z;   // CPU 端预算的 1/ΣI（仅方向灯）
    const float pointCount      = g_lights[0].w;   // 点光源数（uniform 分支判据）
    float3 direct = float3(0.0f, 0.0f, 0.0f);
    float3 pointDirect = float3(0.0f, 0.0f, 0.0f);
    // 每灯 3 个 float4：{dir.xyz, intensity}、{color.rgb, type}、{position.xyz, InvRadius}
    for (uint i = 0u; i < (uint)lightCount; ++i)
    {
        const float4 dirInt = g_lights[1u + i * 3u];
        const float4 color  = g_lights[2u + i * 3u];
        const float4 posRng = g_lights[3u + i * 3u];
        if (color.w > 0.5f)
        {
            // M-E1b.2 点光衰减 = UE DeferredLightingCommon.ush:480-486 实读口径：
            //   DistanceAttenuation = 1/(d²+1) * Square(saturate(1 - Square(d²/r²)))
            //   L = ToLight * rsqrt(d²)，NoL = saturate(dot(N,L))
            // CPU 端写 InvRadius=1/range（UE LightData.InvRadius 同款），shader 只做
            // 乘法；半径外窗函数平滑归零，避免硬截断。点光按绝对强度独立累加，
            // 不进方向灯的 1/ΣI 归一化（否则 att<<1 会让点光几乎看不见）。
            const float3 toLight = posRng.xyz - input.worldPos;
            const float d2 = dot(toLight, toLight);
            const float3 L = toLight * rsqrt(max(d2, 1e-8f));
            const float q = d2 * posRng.w * posRng.w;   // d² * InvRadius²
            const float radiusMask = saturate(1.0f - q * q);
            const float att = (1.0f / (d2 + 1.0f)) * radiusMask * radiusMask;
            const float ndl = max(dot(n, L), 0.0f);     // 点光用单面（物理口径）
            pointDirect += color.rgb * dirInt.w * ndl * att;
        }
        else
        {
            // 方向灯：M-E1b.0/1 原表达式一字不动（既有画面基线逐字节依赖）。
            // abs() 与 mode 0 同口径：关背面剔除时内壁也被照亮。
            // dirInt.xyz 已由 viewer 侧归一化写进灯缓冲，shader 内不再 normalize。
            const float ndl = abs(dot(n, dirInt.xyz));
            direct += color.rgb * dirInt.w * ndl;
        }
    }
    // 多灯强度直接相加会整体过曝（ΣI≈4.7，clamp 后全画面 1.0，三灯的
    // 方向/色温差别完全看不出来）。按方向灯总强度归一化成「加权平均」；
    // 归一化只做乘法（倒数 CPU 预算）→ DXIL/SPIR-V 逐位一致，双后端 bytes。
    // **无点光时走原表达式**（M-E1b.0/1 基线逐字节不变）；有点光时额外累加。
    // lightCount=0 时 invSumIntensity=0、direct=0 → 退化为纯 ambient。
    float3 lighting;
    if (pointCount > 0.5f)
        lighting = min(ambient + direct * invSumIntensity + pointDirect, 1.0f);
    else
        lighting = min(ambient + direct * invSumIntensity, 1.0f);
    float3 lit = albedo * lighting;
    return float4(lit, 1.0f);
}
