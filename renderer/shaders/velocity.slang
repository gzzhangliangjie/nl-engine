// M10.0 (S10): velocity prepass — T0-PLAN T3 在消费端里程碑的落地。
// UE 口径（Q10-1 拍板「尽量按照 UE 的思路来」）：独立一条小 pass 重画一遍几何写
// velocity（UE VelocityDepthStencil / bevy prepass 同款），不碰 T2 已按 hdrInfo
// 重建的 6 组场景 PSO/framebuffer；compute 主导的 viz 模式不开本 pass。
//
// 语义（M10.1 探针重投影与 S12 时间累积的唯一消费契约）：
//   velocity.xy = curUV - prevUV   （UV 空间，y 已随屏幕系翻转）
//   消费端反推上一帧屏幕位置：prevUV = curUV - velocity
// 天空像素：velocity_ 每帧清 0，本 pass 只写几何覆盖像素——静态相机的天空速度恒
// 0（M10.0 自检口径，截图相机不动）；相机旋转的天空 velocity 补写推迟到 S12/S16
// 有真消费端时再做（UE 有独立 sky velocity pass，本里程碑不背）。
//
// 深度：本 pass 自带 D32 深度附件（probeDepth_，Scene 持有）——hidden 几何不写
// velocity，且 csPlace 直接把它当 SRV 读（探针深度/法线来源）。共享 backend depth_
// 是 keepInitialState=DepthWrite 永驻态，不能切 SRV，故 probe 模式自建
//（UE velocity pass 自带 depth 的口径）。
//
// Registers（docs/SHADER-CONVENTIONS.md §2.7：每管线单一 BindingSet）：
//   b0 = VelocityConstants (144B): g_viewProj + g_prevViewProj + g_params
//   t1..t4 = vertexPool / localIndices / clusterMeta / visibleList（visbuf.slang 同槽）
// SPIR-V 绑定平移 §2.5：b→256, t→0, s→128, u→384（无 UAV，统一加）。

cbuffer VelocityConstants : register(b0)
{
    // 本帧 viewProj（列主序，FrameParams.viewProj 原样）。注意是纯 view×proj：
    // 与 rt 模式射线生成同一路径，模型矩阵不在此乘（Suzanne 系资产 model=单位阵
    // ；若未来模型带变换，prev/cur 两帧都要乘同一 model——velocity 是世界空间性质）。
    float4x4 g_viewProj;
    // 上一帧 viewProj。首帧 Scene 用本帧矩阵填（probeFirstFrame_ 门控）→ velocity=0，
    // 截图回归首帧即稳态，不存在首帧大速度污染。
    float4x4 g_prevViewProj;
    // x = width, y = height（像素）。SV_Position 是像素坐标（y-down），PS 侧
    // 反算 NDC 用（与 gbInterpolateVisTriangle 的 s[] 同一口径）。
    float4   g_params;
}

StructuredBuffer<float> g_vertexPool   : register(t1);
StructuredBuffer<uint>  g_localIndices : register(t2);
StructuredBuffer<uint>  g_clusterMeta  : register(t3);
StructuredBuffer<uint>  g_visibleList  : register(t4);

struct VSVelocityOutput
{
    float4 position : SV_Position;   // 本帧 clip（光栅化）
    float4 prevPos  : TEXCOORD0;     // 上一帧 clip（透视正确插值，PS 除 w）
};

[shader("vertex")]
VSVelocityOutput vsVelocity(uint instanceIndex : SV_InstanceID,
                            uint vertexIndex   : SV_VertexID)
{
    VSVelocityOutput output;
    // padding 三角 / 越界一律 NaN 裁剪坐标，光栅化阶段整三角丢弃（visbuf.slang 同款）。
    output.position = asfloat(0x7fc00000u).xxxx;
    output.prevPos  = output.position;

    const uint clusterId  = g_visibleList[instanceIndex];
    const uint metaBase   = clusterId * 4u;
    const uint vertexBase = g_clusterMeta[metaBase + 0u];
    const uint triCount   = g_clusterMeta[metaBase + 1u];
    const uint localBase  = g_clusterMeta[metaBase + 2u];

    const uint triInCluster = vertexIndex / 3u;
    if (triInCluster >= triCount) return output;

    const uint localIdx = g_localIndices[localBase + vertexIndex];
    const uint gv = vertexBase + localIdx;
    const uint b  = gv * 8u;                     // 8 float / 顶点（visbuf 同布局）
    const float3 pos = float3(g_vertexPool[b + 0u], g_vertexPool[b + 1u], g_vertexPool[b + 2u]);

    output.position = mul(g_viewProj,     float4(pos, 1.0f));
    output.prevPos  = mul(g_prevViewProj, float4(pos, 1.0f));
    return output;
}

[shader("pixel")]
void psVelocity(VSVelocityOutput input, out float4 color : SV_Target)
{
    // 本帧 NDC：SV_Position.xy = 像素坐标（y-down），除 viewport → [0,1] 再映射 NDC。
    const float2 size = max(g_params.xy, float2(1.0f, 1.0f));
    const float2 curPx = input.position.xy;
    const float2 curUv = curPx / size;
    // 上一帧 NDC：透视正确插值的 prevPos 除 w；w<=0（相机后方）clip 掉。
    if (input.prevPos.w <= 0.0f)
    {
        color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const float2 prevNdc = input.prevPos.xy / input.prevPos.w;
    // UV 空间速度（y 已翻转）：prevUV = curUV - velocity。color 是 RG16F 目标，
    // 仅 xy 有意义；zw 写 0（blended? 无 blend，整值写入，zw 被格式丢弃）。
    const float2 prevUv = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - 0.5f * prevNdc.y);
    color = float4(curUv - prevUv, 0.0f, 0.0f);
}
