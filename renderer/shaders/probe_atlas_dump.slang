// M11.2 验收工具（S11）：把 probe 辐照度 atlas（irradAtlas_[0]，csTraceProbe
// 输出）直出到画面，供 --probe-atlas-dump 做「同管线开关开/关自比」。
//
// 为什么需要它：现有合成图（--mode probe 的 csGather）每像素只采样 atlas 的
// 外法线方向 texel，对纯天空场景的凸体（Suzanne）该方向恒 miss→天空 texel，
// 故 M11.2 命中分支改写的「命中方向 texel」永远不被 composed 图像读到 →
// 合成图天然 ON≡OFF（结构性，非缓存 bug）。而 M11.2 真正的特征增量落在 g_irrad
// 的命中方向 texel（命中分支读世界缓存 C 改写），所以验收对象必须是 atlas 本身。
// 本 shader 把 level0 atlas 直出（reinhard+gamma 便于肉眼/数值比对），绕开 gather。
//
// Registers（§2.7 单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   t0 = irradAtlas_[0] SRV（读）  u0 = hdrSceneColor_ UAV（写，全替换）

Texture2D<float4> g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);

[numthreads(8, 8, 1)]
void csIrradDump(uint3 tid : SV_DispatchThreadID)
{
    uint dw, dh;
    g_dst.GetDimensions(dw, dh);
    if (tid.x >= dw || tid.y >= dh) return;

    uint sw, sh;
    g_src.GetDimensions(sw, sh);
    // 画面 UV → atlas UV（屏幕与 atlas 尺寸不同，拉伸映射即可；差异比对不依赖
    // 像素级 1:1 对齐，只看纹素值分布）。
    const float2 uv = (float2(tid.xy) + 0.5f) / float2(dw, dh);
    const uint2 st = uint2(clamp(uv * float2(sw, sh),
                                 float2(0.0f, 0.0f),
                                 float2(sw, sh) - float2(1.0f, 1.0f)));
    const float3 lin = g_src.Load(int3(st, 0)).rgb;

    // HDR 显示：reinhard + gamma（与 cache-viz radiance 模式同款观感，便于肉眼看）。
    const float3 enc = pow(saturate(lin / (1.0f + lin)), 1.0f / 2.2f);
    g_dst[tid.xy] = float4(enc, 1.0f);
}
