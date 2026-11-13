// M15.0 (S15): VSM 深度 pass —— 全簇 vertex pulling 渲虚拟阴影贴图。
//
// 几何口径与 visbuf.slang::vsVisRaster 同源（同 vertexPool/localIndices/clusterMeta、
// 同 NaN 裁剪守卫），两处差异：
//   1) 不读 visibleList——那是相机视锥剔除列表，会把相机外遮挡体漏掉（影不完整）。
//      M15.0 全簇 instance 直绘（instanceIndex 即簇号）；按光源视锥的逐页 UV 标记
//      剔除留 M15.2（UE BuildPerPageDrawCommands 对位）。
//   2) MVP = 光源正交 lightViewProj（CPU 侧 shadowOrtho/shadowLookAt 预算，E1b.4
//      同款：全场 AABB + 灯方向；M15.0 单光源单级，无 clipmap 层级）。
// PS 只输出 SV_Position.z（D3D 视口后 NDC z ∈ [0,1]）进 R32F 页池——接收者采样侧
// 用同一矩阵算 NDC.z 比较，双后端数值同源（0..1 深度范围 Vulkan/D3D 一致）。
//
// 资源寄存器（§2.7 每管线单一 BindingSet）：
//   b0 = VsmDepthConstants (80B)：g_lightMvp + 16B 对齐占位（CB 尺寸以 dxc -dumpbin 为准）
//   t1..t3 = vertexPool / localIndices / clusterMeta（与 model.slang/visbuf.slang 同源）
// 入口名 vsMain/psMain = CMake _ENTRIES_DEFAULT（新增 shader 免加编译规则，glob 收集）。
// SPIR-V 平移同全家：b→256, t→0, s→128, u→384。

cbuffer VsmDepthConstants : register(b0)
{
    float4x4 g_lightMvp;     // lightViewProj（列主序，mul(M, v)）
    float4   g_vsmDepthPad;  // 16B 对齐占位（offset 64，CB 总 80B）
}

StructuredBuffer<float> g_vertexPool   : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);

struct VSVsmOutput
{
    float4 position : SV_Position;
};

VSVsmOutput vsMain(uint instanceIndex : SV_InstanceID,
                   uint vertexIndex   : SV_VertexID)
{
    VSVsmOutput output;
    // padding 三角 / 越界一律 NaN 裁剪坐标，光栅化阶段整三角丢弃（M4 同款）。
    output.position = asfloat(0x7fc00000u).xxxx;

    const uint clusterId  = instanceIndex;   // 全簇直绘：instance 即簇号
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint triCount   = g_clusterMeta[metaBase + 1u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];

    const uint triInCluster = vertexIndex / 3u;
    if (triInCluster >= triCount) return output;

    const uint localIdx = g_localIndices[localBase + vertexIndex];
    const uint gv = vertexBase + localIdx;
    const uint b  = gv * 8u;                 // 8 float / 顶点（pos/normal/uv）
    const float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);

    output.position = mul(g_lightMvp, float4(pos, 1.0f));
    return output;
}

float4 psMain(float4 position : SV_Position) : SV_Target
{
    // R32F RT 取 .x：SV_Position.z = 视口后 NDC z（[0,1]），与接收者侧同口径。
    return float4(position.z, 0.0f, 0.0f, 0.0f);
}
