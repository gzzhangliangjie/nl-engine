// S12 M12.0：GI 分量独立时域累积（Q12-A 拍板方案 A）。
// S12 M12.2：历史拒绝层（Q12-B 续拍 = B1「维持不落盘」，见 S12-PLAN.md §0.8）。
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
//（即 bevy TAA 的 confidence / Carrot historyLength / UE NumFramesAccumulated
//  的最小实现）。t0Parity_ 由 Scene 侧逐帧交换 read/write；本 shader 只看一侧。
//
// 自适应历史长度（GI-RESEARCH-NOTES.md §3 / Carrot temporal-denoise 口径）：
//   alpha = max(userAlpha, 1/(len+1))   → 首帧 len=0 时 alpha=1（纯当前帧，
//   不读未初始化历史），随 len 增长收敛到用户设定的下限 alpha（默认 0.1）。
//   len 上限 = temporalHistoryMax（默认 32）。
//
// 重投影：velocity.xy = curUV - prevUV（velocity.slang 唯一消费契约）→
//   prevUV = curUV - velocity；越界即丢弃历史（M12.0 的保底拒绝）。
//
// ---- M12.2 历史拒绝层（g_flags.w 携带阈值，0 = 关）----
// Q12-B 续拍 = B1「维持不落盘」：判据 = 越界 + 重投影深度差 + 当前帧法线，
// 可加零资源邻域 clamp / 帧数置信度。**不新增 previous normal /
// previous_gbuffer**（Q4 冻结面不解冻），读的只有当前帧 probeDepth_（t2，
// 已存在的纹理，只加一个 SRV 绑定槽，M12.1 同款先例）。
//   (1) 越界：prevUv 出屏（M12.0 已有）。
//   (2) 重投影深度差：当前帧深度图在 prevUv 处的 deviceZ 与当前像素
//       deviceZ 之差 > thr → 几何不连续 / disocclusion，丢历史。
//   (3) 当前帧法线的一阶代理：用右/下邻域 deviceZ 梯度外推 prevUv 处的
//       局部切平面深度，残差 > 2*thr → 丢历史。这是 bevy
//       `pixel_dissimilar` 切平面距离判据的「零 previous 属性」退化版
//       （T0-PLAN.md §1「深度差 + 当前帧法线」）。
//   (4) 3x3 深度邻域 clamp：zp 落在当前像素 3x3 邻域 deviceZ 的
//       [min-thr, max+thr] 之外 → 丢历史。bevy TAA 邻域 clamp 的零资源版：
//       读的是只读深度纹理而非颜色邻域，避开 M12.1 实锤的「RWTexture
//       原地读邻域跨线程 hazard」，也不需要新增 RT。
//   (5) 帧数置信度：拒绝时 len 归 0，下一帧由 alpha=max(userAlpha,1/(len+1))
//       自动走短历史（UE NumFramesAccumulated 的最小实现）。
// 静态相机 velocity=0 → prevUv==uv，(2)(3)(4) 残差全 0 → 拒绝层不触发、
// ON 与 OFF 逐值相同。本场景 deviceZ 8-bit 仅 184~186 三档、无真实几何
// 边缘（M12.1 实锤）→ 拒绝层的可观测收益只在运动序列里，见 S12-PLAN §0.8.2。
//
// 三层独立开关：g_flags.y 时间 / .z 空间 / .w 拒绝（.w = 阈值，0 为关）。
// **零 CB 扩容**：FilterConstants 仍是 2xfloat4 = 32B，阈值借 g_flags.w
// 的语义传递（M12.0 CB 64B 过量分配纪律不动，SSC §2.21 / M11.0 实锤）；
// filter_spatial.slang 不读 .w，一字不动 → M12.0/M12.1 保形基线零漂。
//
// Registers（§2.7 单一 BindingSet；§2.5 SPIR-V 平移 b→256 / t→0 / u→384）：
//   b0 = FilterConstants（32B 声明，Scene 侧 64B 防御性分配）
//   t0 = velocity_ SRV    t1 = history[read] SRV    t2 = probeDepth_ SRV
//   u0 = gatherLinear_ UAV（读+写）   u1 = history[write] UAV

cbuffer FilterConstants : register(b0)
{
    float4 g_params;  // x=width y=height z=alpha w=historyMax
    float4 g_flags;   // x=validHistory y=temporalEnable z=spatialEnable w=rejectThreshold(0=off)
};

Texture2D<float4>       g_velocity    : register(t0);
Texture2D<float4>       g_historyPrev : register(t1);
Texture2D<float4>       g_depth       : register(t2);  // 当前帧 deviceZ（零 previous 属性）
RWTexture2D<float4>     g_gi          : register(u0);
RWTexture2D<float4>     g_historyCur  : register(u1);

// M12.2 B1：只用当前帧深度的三段判据（深度差 / 法线切平面 / 3x3 邻域 clamp）。
// 天空像素 deviceZ=1 时梯度可能很大，正是物体/天空边界该丢历史的地方；
// 不额外特判，避免 M12.1「ncValid=false 放行 wN=1」那类退化。
bool rejectHistory(int2 spix, int2 hp, float2 uv, float2 prevUv, float thr)
{
    const int2  maxPix = int2(g_params.xy) - int2(1, 1);
    const float zc = g_depth.Load(int3(spix, 0)).x;
    const float zp = g_depth.Load(int3(hp, 0)).x;

    // (2) 重投影深度差。
    if (abs(zc - zp) > thr) return true;

    // (3) 当前帧法线的一阶切平面外推残差。
    const float  zr     = g_depth.Load(int3(min(spix + int2(1, 0), maxPix), 0)).x;
    const float  zu     = g_depth.Load(int3(min(spix + int2(0, 1), maxPix), 0)).x;
    const float2 dPix   = (prevUv - uv) * g_params.xy;
    const float  zPlane = zc + (zr - zc) * dPix.x + (zu - zc) * dPix.y;
    if (abs(zp - zPlane) > thr * 2.0f) return true;

    // (4) 3x3 深度邻域 clamp（只读深度纹理，无跨线程 hazard）。
    float zmin = zc;
    float zmax = zc;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2  q  = clamp(spix + int2(dx, dy), int2(0, 0), maxPix);
            const float zq = g_depth.Load(int3(q, 0)).x;
            zmin = min(zmin, zq);
            zmax = max(zmax, zq);
        }
    }
    if (zp < zmin - thr || zp > zmax + thr) return true;
    return false;
}

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

            // (1)(5) 越界已在上面处理；拒绝层关（thr<=0）时整块不执行，
            // M12.0 的混合路径逐值不变（保形硬要求）。
            bool useHist = true;
            if (g_flags.w > 0.0f && rejectHistory(spix, hp, uv, prevUv, g_flags.w))
                useHist = false;

            if (useHist)
            {
                const float4 hist = g_historyPrev.Load(int3(hp, 0));
                len = min(hist.a, g_params.w);
                const float alpha = clamp(max(g_params.z, 1.0f / (len + 1.0f)), 0.0f, 1.0f);
                acc = lerp(hist.rgb, cur.rgb, alpha);
                len = min(len + 1.0f, g_params.w);
            }
            // 拒绝 → len 保持 0、acc 保持 cur.rgb（重生噪声，不允许鬼影）。
        }
    }

    g_historyCur[tid.xy] = float4(acc, len);
    g_gi[tid.xy]         = float4(acc, cur.a);   // compose 读到的即是累积 GI
}
