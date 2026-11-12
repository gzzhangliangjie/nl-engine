// S12 M12.1：GI 分量 5x5 空间联合双边滤波（Q12-C 拍板 A「先时间后空间，单次串联」）。
//
// 挂点 / 数据通路（零新增 RT，Q4 冻结面不动）：
//   csGather → csFilterTemporal（M12.0，双写 history[write] + gatherLinear_）
//   → **本 pass** → csGatherCompose（读 g_gatherOut.Load()，一字不动）。
//   M12.0 的 csFilterTemporal 已把「本帧时间累积结果」写进 history[write]，
//   本 pass 把它当 **SRV 只读** 5x5 邻域、把滤波结果写回 gatherLinear_ 的
//   自身 texel（u0）。每线程只写自己 tid.xy，读的是另一张纹理 → 无跨线程
//   hazard（M12.0 原地读写能成立是因为它不读邻域，这里不成立，故必须分流）。
//
// 权重（S12-PLAN §0.4 第 5 条已拍板：空间距离 x 法线夹角 x 深度差）：
//   wSpatial = exp(-(dx^2+dy^2) / (2*sigmaS^2))                    [5x5 高斯]
//   wNormal  = exp(-planeDist / (sigmaN * sceneDepth + eps))       [UE ScenePlane
//             点面距口径，与 probe_gather.slang 的 corner 权重同源；等价于
//             「法线夹角 + 深度偏差」的联合判据，避免逐邻域 3-tap 反投影]
//   wDepth   = exp(-|zc-zq| / (sigmaZ + eps))                    [deviceZ 固定
//             尺度；**不**照抄 Carrot weightFunc 的 depthDivisor —— 那份实现
//             里 weightDepth 被 TODO 注释掉、return 只乘了 normal*luminance，
//             而按 gradZ 放大分母会让边缘处权重反而更宽松（首轮实测边缘误差
//             +24.6% 的根因之一）]
//   天空邻域（deviceZ >= 1）跳过；中心法线重建失败（物体/天空边界，右/下
//   邻域是天空）时 wN 退化为 deviceZ 一致性代理，**绝不放行 wN=1**（首轮
//   实测该分支让边界像素完全不保边）；权重和 < kMin 保留中心值（防
//   --dist 坑重演那样的全黑）。亮度差权重本阶段不做（S12-PLAN 标「可选」，
//   留给 M12.2）。
//
// 三层独立开关：g_flags.z = 空间层。关时整段退化为「history[write] 直抄」，
// 与 M12.0 的 gatherLinear_ 末态逐值一致 → M12.0 的 run.bat verify（hf/MAE）
// 与 105 张卡的保形基线逐字沿用、零重拍。本 pass 始终 dispatch（同 M12.0
// 时间层关时仍 dispatch 的口径），保证 ping-pong 状态机逐帧连续。
//
// Registers（§2.7 单一 BindingSet；§2.5 SPIR-V 平移 b→256 / t→0 / u→384）：
//   b0 = FilterConstants（复用 M12.0 filterCB_，64B 过量分配，32B 声明）
//   b1 = SpatialCamera（复用 gatherCB_ 前 80B：invViewProj + eye，零新增 CB）
//   t0 = history[write] SRV   t1 = probeDepth_ SRV   u0 = gatherLinear_ UAV

#include "gbuffer_common.slangh"

cbuffer FilterConstants : register(b0)
{
    float4 g_params;  // x=width y=height z=alpha w=historyMax
    float4 g_flags;   // x=validHistory y=temporalEnable z=spatialEnable w=rejectEnable
};

cbuffer SpatialCamera : register(b1)
{
    float4x4 g_invViewProj;  // 本帧 inverse(viewProj)（gatherCB_ [0..15]）
    float4   g_eye;          // xyz = eye（gatherCB_ [16..19]）
};

Texture2D<float4>   g_historyCur : register(t0);  // 本帧时间累积结果（UAV 写侧转 SRV）
Texture2D<float4>   g_depth      : register(t1);  // probeDepth_（x = deviceZ）
RWTexture2D<float4> g_gi         : register(u0);  // gatherLinear_

// 深度邻域 3-tap 反投影 cross 重建中心法线（probe_gather.slang
// gatherNormalFromDepth 同款口径：越界 clamp + 朝相机翻转）。只算一次/像素。
bool spatialCenterNormal(int2 pix, float3 wp, float2 size, out float3 n)
{
    n = float3(0.0f, 1.0f, 0.0f);
    const int2 maxPix = int2(size) - 1;
    const int2 pr = min(pix + int2(1, 0), maxPix);
    const int2 pu = min(pix + int2(0, 1), maxPix);
    const float zr = g_depth.Load(int3(pr, 0)).x;
    const float zu = g_depth.Load(int3(pu, 0)).x;
    if (zr >= 1.0f || zu >= 1.0f) return false;
    const float3 wr = gbReconstructWorldPos(uint2(pr), zr, size, g_invViewProj);
    const float3 wu = gbReconstructWorldPos(uint2(pu), zu, size, g_invViewProj);
    n = normalize(cross(wr - wp, wu - wp));
    if (dot(n, wp - g_eye.xyz) > 0.0f) n = -n;
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csFilterSpatial(uint3 tid : SV_DispatchThreadID)
{
    const uint viewW = uint(g_params.x);
    const uint viewH = uint(g_params.y);
    if (tid.x >= viewW || tid.y >= viewH) return;

    const int2  spix   = int2(tid.xy);
    const float2 size  = g_params.xy;
    const int2  maxPix = int2(size) - 1;
    const float4 center = g_historyCur.Load(int3(spix, 0));

    // 空间层关 = pass-through（三层独立开关；M12.0 末态逐值不变）。
    if (g_flags.z < 0.5f) {
        g_gi[tid.xy] = center;
        return;
    }

    const float zc = g_depth.Load(int3(spix, 0)).x;
    if (zc >= 1.0f) { g_gi[tid.xy] = center; return; }   // 天空不参与滤波

    const float3 wpc = gbReconstructWorldPos(uint2(spix), zc, size, g_invViewProj);
    const float  sceneDepth = length(wpc - g_eye.xyz);
    float3 nc;
    const bool ncValid = spatialCenterNormal(spix, wpc, size, nc);

    // deviceZ 在物体内部相邻像素差约 1e-4 量级、跨物体边界约 1e-2 量级；
    // 固定尺度比随梯度缩放的 depthDivisor 更能保边（见文件头追因说明）。
    const float sigmaS = 1.5f;
    const float sigmaN = 0.02f;
    const float sigmaZ = 0.002f;
    const float eps    = 1e-5f;
    const float kMinWeight = 1e-5f;

    float3 acc = float3(0.0f, 0.0f, 0.0f);
    float  sum = 0.0f;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int2 q = clamp(spix + int2(dx, dy), int2(0, 0), maxPix);
            const float zq = g_depth.Load(int3(q, 0)).x;
            if (zq >= 1.0f) continue;                       // 天空邻域拒绝

            const float wS = exp(-float(dx * dx + dy * dy) / (2.0f * sigmaS * sigmaS));

            // 法线权重：邻域世界位置到中心切平面的距离（UE ScenePlane 口径）。
            // 中心法线不可用时不能用 wN=1 放行，否则物体/天空边界像素完全
            // 不保边（首轮实测 REF-edge 误差 +24.6%）；退化为 deviceZ 一致性
            // 代理，只混合同侧表面。
            float wN;
            if (ncValid) {
                const float3 wpq = gbReconstructWorldPos(uint2(q), zq, size, g_invViewProj);
                const float planeDist = abs(dot(wpq - wpc, nc));
                wN = exp(-planeDist / max(sigmaN * sceneDepth, eps));
            } else {
                wN = exp(-abs(zc - zq) / (sigmaZ + eps));
            }

            const float wD = exp(-abs(zc - zq) / (sigmaZ + eps));

            const float w = wS * wN * wD;
            acc += g_historyCur.Load(int3(q, 0)).rgb * w;
            sum += w;
        }
    }

    const float3 outRgb = (sum > kMinWeight) ? (acc / sum) : center.rgb;
    g_gi[tid.xy] = float4(outRgb, center.a);
}
