// S12 M12.0：GI 分量独立时域累积（Q12-A 拍板方案 A）。
//
// 挂点（T5 保形硬边界）：本 pass 插在 csGather 与 csGatherCompose **之间**，且只在
// --mode filter 下插入。csGather 把当前帧 GI 写进 gatherLinear_（RGBA16F 全屏 UAV），
// 本 pass 读它、与上一帧 history 混合、把累积结果**原地写回同一 texel**；
// csGatherCompose 仍按原绑定读 g_gatherOut.Load() —— 绑定与执行顺序一字不动，
// 非 filter 模式整条 compose 路径零改动（M11.1「冻结基线的代码形态也是基线」）。
//
// 为什么可以原地读写：probe_gather.slang 的 csGatherCompose 已用
// `RWTexture2D<float4>.Load(int2)` 读同一 UAV 作为先例，双后端验证通过；每线程
// 只碰自己 tid.xy 的 texel，无跨线程 hazard。零新增 RT（Q12-A）。
//
// 历史语义：history0_/1_ 为 RGBA32F ping-pong，rgb = 累积 GI，a = 已累积帧数
//（即 bevy TAA 的 confidence / Carrot historyLength 的最小实现）。t0Parity_ 由
// Scene 侧逐帧交换 read/write；本 shader 只看一侧。
//
// 自适应历史长度（GI-RESEARCH-NOTES.md §3 / Carrot temporal-denoise 口径）：
//   alpha = max(userAlpha, 1/(len+1))   → 首帧 len=0 时 alpha=1（纯当前帧，
//   不读未初始化历史），随 len 增长收敛到用户设定的下限 alpha（默认 0.1）。
//   len 上限 = temporalHistoryMax（默认 32）。
//
// 重投影：velocity.xy = curUV - prevUV（velocity.slang 唯一消费契约）→
//   prevUV = curUV - velocity；越界即丢弃历史（M12.0 只做这一条保底拒绝，
//   Q12-B 拍板「M12.2 前按实测再拍」，本阶段不引入 previous 属性纹理）。
//
// 三层独立开关：g_flags.y = 时间层（本阶段实现）；g_flags.z / .w = 空间 / 拒绝
// 层 flag（M12.1 / M12.2 实现，本阶段恒 0，仅占位不消费）。
//
// Registers（§2.7 单一 BindingSet；§2.5 SPIR-V 平移 b→256 / t→0 / u→384）：
//   b0 = FilterConstants（32B 声明，见 Scene 侧 64B 防御性分配说明）
//   t0 = velocity_ SRV        t1 = history[read] SRV
//   u0 = gatherLinear_ UAV（读+写）   u1 = history[write] UAV

cbuffer FilterConstants : register(b0)
{
    float4 g_params;  // x=width y=height z=alpha w=historyMax
    float4 g_flags;   // x=validHistory y=temporalEnable z=spatialEnable w=rejectEnable
};

Texture2D<float4>       g_velocity    : register(t0);
Texture2D<float4>       g_historyPrev : register(t1);
RWTexture2D<float4>     g_gi          : register(u0);
RWTexture2D<float4>     g_historyCur  : register(u1);

[shader("compute")]
[numthreads(8, 8, 1)]
void csFilterTemporal(uint3 tid : SV_DispatchThreadID)
{
    const uint viewW = uint(g_params.x);
    const uint viewH = uint(g_params.y);
    if (tid.x >= viewW || tid.y >= viewH) return;

    const int2 spix = int2(tid.xy);
    const float4 cur = g_gi.Load(spix);      // 当前帧 GI（a 恒 1，原样保留）
    float3 acc = cur.rgb;
    float  len = 0.0f;

    // 时间层：开 && 历史有效 && 重投影在屏内 → 混合；否则等价「单层当前帧」。
    if (g_flags.y > 0.5f && g_flags.x > 0.5f)
    {
        const float2 uv     = (float2(tid.xy) + 0.5f) / g_params.xy;
        const float2 vel    = g_velocity.Load(int3(spix, 0)).xy;
        const float2 prevUv = uv - vel;
        if (all(prevUv >= float2(0.0f, 0.0f)) && all(prevUv < float2(1.0f, 1.0f)))
        {
            uint hw = 1u, hh = 1u;
            g_historyPrev.GetDimensions(hw, hh);
            const float2 hsize = float2(hw, hh);
            const int2 hp = int2(clamp(prevUv * hsize, float2(0.0f, 0.0f),
                                       hsize - float2(1.0f, 1.0f)));
            const float4 hist = g_historyPrev.Load(int3(hp, 0));
            len = min(hist.a, g_params.w);
            const float alpha = clamp(max(g_params.z, 1.0f / (len + 1.0f)), 0.0f, 1.0f);
            acc = lerp(hist.rgb, cur.rgb, alpha);
            len = min(len + 1.0f, g_params.w);
        }
    }

    g_historyCur[tid.xy] = float4(acc, len);
    g_gi[tid.xy]         = float4(acc, cur.a);   // compose 读到的即是累积 GI
}
