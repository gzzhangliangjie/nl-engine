// M10.0 (S10): screen probe cascade placement — Lumen Screen Probe 布点第一步
// （S10-PLAN M10.0）。csPlace：4 级（baseRes→baseRes/2³）每级独立网格，屏幕格点 →
// 深度反投影世界位置 → 深度邻域重建法线 → 法线偏移，写 RGBA32_FLOAT atlas
// （rgb=探针世界位置，a=deviceZ；a=0 = 天空/无几何探针，UE ProbeSceneDepth>0
// 同款判据）。同时读 velocity_ 计算逐探针上一帧 UV（Q10-1/Q10-2 拍板链路），
// prevUV-curUV 增量写 prevDelta_ 供 viz 着色——相机移动时点变白（时域链路的
// 机器可见证据；静态相机全 0 = 截图基线稳定）。
//
// 口径对照（docs/s10/GI-RESEARCH-NOTES.md §1，UE 只读不抄）：
//   UE 是单级均匀网格（DownsampleFactor=16）+ 自适应加密；4 级 cascade 是本项目
//   自设计（自检目标「近密远疏」），自适应加密列 S14 可选升级。
//   探针位置 = GetWorldPositionFromScreenUV + NormalBias 法线偏移的口径对齐。
//   法线来源：UE 读 gbuffer normal；本项目 GBuffer 不落盘（Q4/Q-H2 拍板），
//   探针法线用深度邻域重建（3 tap 反投影 cross）——纯 compute 无 ddx/ddy。
//
// Registers（§2.7：每管线单一 BindingSet；§2.5 SPIR-V 平移 b→256/t→0/u→384）：
//   b0 = ProbeConstants(176B，与 probe_viz.slang 共用同一份 CB buffer，各入口
//        只读自己的字段段) + t0 depth SRV + t1 velocity SRV
//        + u0..u3 atlas UAV + u4 prevDelta UAV
//   （可视化入口在 probe_viz.slang：b0 同一 CB + t0..t4 全 SRV，两管线两套 set。）

#include "gbuffer_common.slangh"

cbuffer ProbeConstants : register(b0)
{
    // [0..3] 布点侧字段
    float4x4 g_invViewProj;    // 列主序（CPU inverseMat4，rt 模式同一路径）
    float4   g_eye;            // xyz = 相机世界位置（法线朝向翻转），w = projYScale
    float4   g_placeParams;    // x=width y=height z=baseRes w=normalOffsetScale
    // [4..] 可视化侧字段（probe_viz.slang 读；本文件不用，共用一份 CB 少建一 buffer）
    float4x4 g_vizViewProj;    // 本帧 viewProj（atlas 世界位置反投影回屏幕）
    float4   g_vizParams;      // x=dotRadiusPx y=width z=height w=baseRes
    float4   g_vizExtra;       // M10.1：x=dotMode（0=级别色 / 1=八面体辐照度采样）
                               //（192B 布点侧无感，只扩容不改已用字段）
}

Texture2D          g_probeDepth  : register(t0);   // D32（velocity pass 写入）
Texture2D<float2>  g_velocityTex : register(t1);   // RG16F（M10.0 T3 产物）

// ---- M14C1（S14-C Q-C1A/C2A 拍板）：自适应 Mark/Spawn 新增绑定 ----
// t2..t5 = place atlas cur parity SRV（Mark 读 4-corner 候选探针位置；
//          csPlace 刚 UAV 写完 → 本 pass require SRV 自动插 barrier）。
// t6     = 自适应 mask SRV（Spawn 读 Mark 产物；同 buffer u5 UAV 由 Mark 写，
//          UAV→SRV 状态转换由 require 自动插）。
// u5     = RWStructuredBuffer<uint> 自适应 mask（每均匀探针 1 uint：
//          0xffffffff = 无失效哨兵，其余 = 第一个失效采样点 idx+1，
//          Mark InterlockedMin 收敛）。
Texture2D<float4>  g_markAtlas0 : register(t2);
Texture2D<float4>  g_markAtlas1 : register(t3);
Texture2D<float4>  g_markAtlas2 : register(t4);
Texture2D<float4>  g_markAtlas3 : register(t5);
StructuredBuffer<uint> g_maskSRV : register(t6);
// M14C1：RWStructuredBuffer 与 nvrhi StructuredBuffer_UAV 绑定项类型一一对应
//（g_wcChecksums 同款先例）。收口诊断定论：spawn=0 的真根因是帧首清零 0 与
// InterlockedMin 起始值语义冲突（min(0,x)=0 压死收敛），非 descriptor 类型——
// 现按哨兵值 0xffffffff 复位 + 收敛语义配对。
RWStructuredBuffer<uint> g_adaptiveMask : register(u5);

RWTexture2D<float4> g_atlasU0    : register(u0);   // 4 级 atlas（RGBA32F）
RWTexture2D<float4> g_atlasU1    : register(u1);
RWTexture2D<float4> g_atlasU2    : register(u2);
RWTexture2D<float4> g_atlasU3    : register(u3);
RWTexture2D<float4> g_prevDeltaU : register(u4);   // level0 探针的 prevUV-curUV

// 深度邻域重建法线：3 tap 反投影 cross。返回 false = 邻域含天空（deviceZ>=1）。
bool probeNormalFromDepth(int2 pix, float3 wp, float2 size,
                          float4x4 invViewProj, out float3 n)
{
    n = float3(0.0f, 1.0f, 0.0f);
    const int2 maxPix = int2(size) - 1;
    const int2 pr = min(pix + int2(1, 0), maxPix);
    const int2 pu = min(pix + int2(0, 1), maxPix);
    const float zr = g_probeDepth.Load(int3(pr, 0)).x;
    const float zu = g_probeDepth.Load(int3(pu, 0)).x;
    if (zr >= 1.0f || zu >= 1.0f) return false;    // 天空邻域：给默认向上法线
    const float3 wr = gbReconstructWorldPos(uint2(pr), zr, size, invViewProj);
    const float3 wu = gbReconstructWorldPos(uint2(pu), zu, size, invViewProj);
    n = normalize(cross(wr - wp, wu - wp));
    // 朝向相机（叉积手性不保证向外）：探针永远服务可见表面。
    if (dot(n, wp - g_eye.xyz) > 0.0f) n = -n;
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csPlace(uint3 tid : SV_DispatchThreadID)
{
    const uint baseRes = uint(g_placeParams.z);
    if (tid.x >= baseRes || tid.y >= baseRes) return;

    const float2 size = max(g_placeParams.xy, float2(1.0f, 1.0f));
    const int2 maxPix = int2(size) - 1;

    // level 0 探针额外算 prevUV 增量（velocity 消费链路，M10.1 时域复用的输入）。
    // 增量 = prevUV - curUV（注意方向与 velocity 相反：velocity = curUV - prevUV）。
    float2 delta = float2(0.0f, 0.0f);

    [unroll]
    for (uint level = 0u; level < 4u; ++level)
    {
        const uint res = baseRes >> level;
        if (tid.x >= res || tid.y >= res) continue;

        // 屏幕格点：探针 = 本级网格 texel 中心，间距 = viewport / 级分辨率
        //（逐级分辨率减半 → 间距加倍，即「近密远疏」的来源）。
        const float2 stride = size / float2(float(res), float(res));
        const float2 screenPx = (float2(tid.xy) + 0.5f) * stride;
        const int2 pix = min(int2(floor(screenPx)), maxPix);
        const float deviceZ = g_probeDepth.Load(int3(pix, 0)).x;

        float4 outData = float4(0.0f, 0.0f, 0.0f, 0.0f);   // a=0 = 天空/无几何
        if (deviceZ < 1.0f)
        {
            const float3 wp = gbReconstructWorldPos(uint2(pix), deviceZ, size, g_invViewProj);
            float3 n;
            probeNormalFromDepth(pix, wp, size, g_invViewProj, n);
            // 法线偏移 ∝ 探针世界间距（UE NormalBias 口径：screenStridePx ×
            // worldPerPixel；worldPerPixel = 2·z / (projYScale·H)）。
            const float worldPerPixel = 2.0f * deviceZ / (max(g_eye.w, 1e-4f) * size.y);
            const float offset = g_placeParams.w * worldPerPixel * max(stride.x, stride.y);
            outData = float4(wp + n * offset, deviceZ);
        }

        switch (level)
        {
        case 0u: g_atlasU0[tid.xy] = outData; break;
        case 1u: g_atlasU1[tid.xy] = outData; break;
        case 2u: g_atlasU2[tid.xy] = outData; break;
        case 3u: g_atlasU3[tid.xy] = outData; break;
        }

        if (level == 0u)
        {
            // velocity → prevUV：prevUV = curUV - velocity（velocity.slang 契约）。
            const float2 curUv = screenPx / size;
            const float2 vel = g_velocityTex.Load(int3(pix, 0));
            const float2 prevUv = curUv - vel;
            const bool inBounds = all(prevUv >= float2(0.0f, 0.0f)) &&
                                  all(prevUv <= float2(1.0f, 1.0f));
            // 越界（新入屏的探针）= 时域不可复用：增量写大值让 viz 亮边标出。
            delta = inBounds ? (prevUv - curUv) : float2(1e9f, 1e9f);
        }
    }

    g_prevDeltaU[tid.xy] = float4(delta, 0.0f, 0.0f);
}

// ============================================================================
// M14C1（S14-C，Q-C1A/C2A/C3B 拍板）：自适应探针 Mark/Spawn（UE 口径）
// ----------------------------------------------------------------------------
// 三处实读：UE LumenScreenProbeGather.usf ScreenProbeAdaptivePlacementMarkCS /
// SpawnCS + GetAdaptiveSampleCoord（2026-11-13）。判据 = 「插值覆盖不足」：
// 每均匀探针 16 个 Hammersley 采样点，对周围 4-corner 算 upsample 插值权重和
//（与 probe_gather csGather 同口径：平面深度 exp2(-10000·rel²) + bilinear 角权
// 重），< MIN_PROBE_INTERPOLATION_WEIGHT(0.01) → mask 置位 → Spawn 写预留区。
//
// 布点形态（Q-C2A）：atlas 高度 ×2，预留区槽位 = 均匀格位同 (x, res+y)——
// 每格位至多 spawn 1 个自适应探针（UE 的 per-bit spawn 简化为 per-tile 1 个，
// 防重复天然成立），gather 的 8 池插值确定性索引该槽位。
// trace / traceLayout / probe_trace.slang 零改动：预留区空槽 a=0 → probe.w>0
// 判据黑写早退；spawn 探针 a>0 正常追踪（t6 SRV / dispatch 尺寸全部外置）。
//
// 预算守恒（Q-C3B，结构性保证）：probe-adaptive 模式均匀网格 res_eff =
// baseRes/2（远景粗一半），加密上限 = res_eff² 个（每格位 1）→ 探针总量
// ≤ (baseRes/2)² × 2 = baseRes²/2 < 原均匀 baseRes²；trace dispatch 线程数
// 与 probe 模式恒等（(res_eff·10) × (2·res_eff·10) = (baseRes·10)²），
// 有效 RayQuery 量严格 ≤ 原模式 → 「总帧时间不升」由结构保证。
//
// Mark 采样网格：固定 Hammersley（无帧间 jitter）——probe 链无时间混合
//（M10.1 单份不 ping-pong），帧间 jitter 会造成探针位置逐帧跳变且不收敛；
// 固定网格 = 截图确定性 + 画面稳态。UE 加 SCREEN_TEMPORAL_INDEX 抖动是因为
// 它有 ScreenProbeGatherTemporal 时间积累，本项目 probe 链无此层。
// ============================================================================

// Hammersley 2D（确定性序列；1D radical inverse = uint32 位反转，手写——
// slang 无 HLSL bitfieldReverse 内建，E30015 实锤）。
uint markBitReverse(uint x)
{
    x = ((x & 0xaaaaaaaau) >> 1u) | ((x & 0x55555555u) << 1u);
    x = ((x & 0xccccccccu) >> 2u) | ((x & 0x33333333u) << 2u);
    x = ((x & 0xf0f0f0f0u) >> 4u) | ((x & 0x0f0f0f0fu) << 4u);
    x = ((x & 0xff00ff00u) >> 8u) | ((x & 0x00ff00ffu) << 8u);
    return (x >> 16u) | (x << 16u);
}
float2 markHammersley(uint i)
{
    return float2(float(i) * (1.0f / 256.0f),
                  float(markBitReverse(i)) * (1.0f / 4294967296.0f));
}

// mask buffer 按级偏移：base = Σ_{l<level} res_l²（res0 = g_placeParams.z = res_eff）。
uint markMaskBase(uint level)
{
    const uint res0 = uint(g_placeParams.z);
    uint base = 0u;
    [loop]
    for (uint l = 0u; l < level; ++l)
    {
        const uint r = res0 >> l;
        base += r * r;
    }
    return base;
}

// place atlas 4 路分支读（Mark 的 SRV 视图；纹理数组不能动态索引，csPlace 同款）。
float4 markLoadAtlas(uint level, uint2 texel)
{
    if (level == 0u) return g_markAtlas0.Load(int3(texel, 0));
    if (level == 1u) return g_markAtlas1.Load(int3(texel, 0));
    if (level == 2u) return g_markAtlas2.Load(int3(texel, 0));
    return g_markAtlas3.Load(int3(texel, 0));
}

// ---- csAdaptiveMark：每线程组 = 1 格位 × 16×16 = 256 采样点；每线程判 1 点。
// 采样密度标定：UE 16 采样点对应 16px tile（DownsampleFactor=16）；本项目
// res_eff=8 时 tile ~120px，等密度需 ~900 点——取 256（1/3 等密度，Mark 纯
// 采样零射线成本可忽略）。16 点实锤探不到剪影环带（spawn=0~1，三档阈值
// 0.01/0.9/0.99 全钝化）——失效区（剪影/深度断裂 1~2px 带）面积占比 0.6%，
// 采样点密度必须与 tile 面积同比例放大。
// mask 编码：uint = 第一个失效采样点 idx+1（0 = 格位覆盖良好/无失效点）；
// InterlockedMin 并发收敛到最小 idx（确定性，Spawn 直接取该点坐标 spawn）。
// 判据 = UE MIN_PROBE_INTERPOLATION_WEIGHT(0.01) 原语义 + 法线重建失败代理：
//   ① 4-corner 平面深度权重和（csGather 同口径）< 0.01 = gather 在该采样点
//      几乎无法插值（UE MarkCS 同式同阈值，公式 2026-11-13 实读核对）；
//   ② 法线 3-tap 重建失败（邻域跨天空/深度断裂）——本项目无 GBuffer 法线
//     （Q4 冻结面），跨天空 tap 的默认向上法线会让 planeDist 虚低、判据钝化
//     （三档阈值 sweep 实锤），法线不可靠本身就是覆盖不足的正当代理。
[shader("compute")]
[numthreads(16, 16, 1)]
void csAdaptiveMark(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint level = gid.z;
    const uint res   = uint(g_placeParams.z) >> level;
    if (gid.x >= res || gid.y >= res) return;

    const uint  sampleIdx = gtid.y * 16u + gtid.x;         // 0..255
    const float2 size    = max(g_placeParams.xy, float2(1.0f, 1.0f));
    const int2  maxPix   = int2(size) - 1;
    const float2 stride  = size / float2(float(res), float(res));

    // 采样点屏幕坐标 = 格位 tile 内 Hammersley 位置（固定网格）。
    const float2 spx = (float2(gid.xy) + markHammersley(sampleIdx)) * stride;
    const int2 pix = min(int2(floor(spx)), maxPix);
    const float z = g_probeDepth.Load(int3(pix, 0)).x;
    bool need = false;
    if (z < 1.0f)
    {
        const float3 wp = gbReconstructWorldPos(uint2(pix), z, size, g_invViewProj);
        float3 n;
        const bool nOk = probeNormalFromDepth(pix, wp, size, g_invViewProj, n);
        const float d2 = dot(wp, n);
        const float sceneDepth = length(wp - g_eye.xyz);

        // gather 双线性四角：采样点在格位 tile 内 → tile00 = 本格位，
        // bw = 连续格坐标小数 = tile 内相对位置（csGather 149-156 同口径）。
        const float2 bw = frac(spx / stride);
        const float4 cornerW = float4(
            (1.0f - bw.y) * (1.0f - bw.x),   // 00（本格位）
            (1.0f - bw.y) * bw.x,            // 10
            bw.y * (1.0f - bw.x),            // 01
            bw.y * bw.x);                    // 11

        float wsum = 0.0f;
        [unroll]
        for (uint k = 0u; k < 4u; ++k)
        {
            const uint2 ct = gid.xy + uint2(k & 1u, k >> 1u);
            if (ct.x >= res || ct.y >= res) continue;    // 网格边缘角越界
            const float4 probe = markLoadAtlas(level, ct);
            if (probe.w <= 0.0f) continue;               // 天空探针：权重 0
            const float planeDist = abs(dot(probe.xyz, n) - d2);
            const float rel = planeDist / max(sceneDepth, 1e-4f);
            wsum += cornerW[k] * exp2(-10000.0f * rel * rel);
        }
        need = !nOk || wsum < 0.01f;
    }
    if (need)
    {
        const uint mbase = markMaskBase(level);
        // idx+1（0 保留为「无失效」）；InterlockedMin 收敛第一个失效点。
        InterlockedMin(g_adaptiveMask[mbase + gid.y * res + gid.x], sampleIdx + 1u);
    }
}

// ---- csAdaptiveSpawn：每线程 1 格位。mask==0 → 预留区槽位写 a=0（空槽，
// trace probe.w>0 判据黑写早退）；mask>0 → 最低置位采样点反投影 + 法线偏移
//（csPlace 同款口径）写预留区槽位 a=deviceZ。均匀区本 pass 不碰（照常追踪）。
[shader("compute")]
[numthreads(8, 8, 1)]
void csAdaptiveSpawn(uint3 tid : SV_DispatchThreadID)
{
    const uint level = tid.z;
    const uint res   = uint(g_placeParams.z) >> level;
    if (tid.x >= res || tid.y >= res) return;

    const uint2 tile = tid.xy;
    const uint mask = g_maskSRV.Load(markMaskBase(level) + tile.y * res + tile.x);

    // mask 语义：0xffffffff = 无失效（帧首哨兵，writeBuffer 0xff 填充复位）；
    // 其余值 = 第一个失效采样点 idx+1（Mark InterlockedMin 收敛）。
    float4 slot = float4(0.0f, 0.0f, 0.0f, 0.0f);   // 默认空槽
    if (mask != 0xffffffffu)
    {
        const uint idx = mask - 1u;                  // 第一个失效采样点（Mark 存 idx+1）
        const float2 size   = max(g_placeParams.xy, float2(1.0f, 1.0f));
        const int2  maxPix  = int2(size) - 1;
        const float2 stride = size / float2(float(res), float(res));
        const float2 spx = (float2(tile) + markHammersley(idx)) * stride;
        const int2 pix = min(int2(floor(spx)), maxPix);
        const float deviceZ = g_probeDepth.Load(int3(pix, 0)).x;
        if (deviceZ < 1.0f)
        {
            const float3 wp = gbReconstructWorldPos(uint2(pix), deviceZ, size, g_invViewProj);
            float3 n;
            probeNormalFromDepth(pix, wp, size, g_invViewProj, n);
            // 法线偏移 ∝ 探针世界间距（csPlace 同款：UE NormalBias 口径）。
            const float worldPerPixel = 2.0f * deviceZ / (max(g_eye.w, 1e-4f) * size.y);
            const float offset = g_placeParams.w * worldPerPixel * max(stride.x, stride.y);
            slot = float4(wp + n * offset, deviceZ);
        }
    }

    // 预留区槽位 = (x, res + y)（与均匀格位一一对应，gather 8 池确定性索引）。
    switch (level)
    {
    case 0u: g_atlasU0[uint2(tile.x, res + tile.y)] = slot; break;
    case 1u: g_atlasU1[uint2(tile.x, res + tile.y)] = slot; break;
    case 2u: g_atlasU2[uint2(tile.x, res + tile.y)] = slot; break;
    case 3u: g_atlasU3[uint2(tile.x, res + tile.y)] = slot; break;
    }
}
