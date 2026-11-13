// M14A2（S14-A）：cull 后簇分类 compute（Q-A2 = A2-A 拍板：簇投影 AABB ≤ 阈值
// px → 软列表，硬列表反向补位；bevy cull_clusters 64px 口径 + UE
// NaniteRasterBinning「分流在候选/binning 层、vis buffer 编码统一」的层位映射 =
// 本项目 cull 之后独立 compute，不动 cull.slang 冻结形态——M11.1 冻结基线纪律）。
// 判据实现：投影球半径 r_screen = R / max(dist(eye,center)-R, near)
//           * projYScale * 0.5 * viewportH（dag/zeux 球投影公式同源）；
// 球投影圆的外接 AABB = (2*r_screen) 方块 ≤ thresholdPx → 软簇。
// 偏差说明：bevy 投影的是簇 AABB 角点，这里用簇包围球近似（superset 保守，
// 不会漏分软簇；偏差只影响阈值附近的边界簇，M14A3 profile 复核）。
// csArgsPatch：argsHard[1] = hardCount（单 dispatch 无法全局同步，两段各 1 pass，
// argsHard[0/2..4] 由 CPU 一次性写模板——stride/0/0/0 恒定）。
//
// 绑定（SPIR-V shift 256/0/128 统一编译参数）：
//   b0 = ClassifyConstants（volatile 32B）
//   t0 = visibleList（cull 产出；frustum off = 恒等映射）
//   t1 = clusterSpheres（xyzw = center+R）
//   u0 = softList（compact）  u1 = hardList（compact）
//   u2 = counts {softCount, hardCount, split, 0}   u3 = argsHard
//   （argsHard[0/2..4] 恒定元素由 CPU 一次性写模板；args[1] 唯一 GPU 回填）

struct ClassifyConstants
{
    float4 raster;   // x = projYScale*0.5*viewportH, y = nearPlane,
                     // z = thresholdPx, w = clusterCount
    float4 eye;      // xyz = 相机世界位置
};

cbuffer ClassifyCB : register(b0) { ClassifyConstants g_cc; }

StructuredBuffer<uint>   g_cVisible : register(t0);
StructuredBuffer<float4> g_cSpheres : register(t1);

RWStructuredBuffer<uint>  g_cSoft    : register(u0);
RWStructuredBuffer<uint>  g_cHard    : register(u1);
RWStructuredBuffer<uint4> g_cCounts  : register(u2);
RWStructuredBuffer<uint>  g_cArgsHard: register(u3);

bool isSoft(uint clusterId)
{
    const float4 s = g_cSpheres[clusterId];
    const float3 d = s.xyz - g_cc.eye.xyz;
    const float dist = length(d);
    // 近端夹取与 dag 公式同源：dist - R 下限 nearPlane（贴脸/背面相机 → 恒硬簇）。
    const float denom = max(dist - s.w, g_cc.raster.y);
    const float rScreen = s.w / denom * g_cc.raster.x;
    return (2.0f * rScreen) <= g_cc.raster.z;   // AABB 全尺寸 ≤ 阈值 → 软
}

[numthreads(64, 1, 1)]
void csClassify(uint tid : SV_DispatchThreadID)
{
    const uint count = (uint)g_cc.raster.w;
    if (tid >= count) return;
    const uint clusterId = g_cVisible[tid];
    uint slot;
    if (isSoft(clusterId)) {
        InterlockedAdd(g_cCounts[0].x, 1u, slot);
        g_cSoft[slot] = clusterId;
    } else {
        InterlockedAdd(g_cCounts[0].y, 1u, slot);
        g_cHard[slot] = clusterId;
    }
}

// 1 线程：把 hardCount 回填进 indirect args[1]（其余元素 = CPU 模板恒定）。
[numthreads(1, 1, 1)]
void csArgsPatch(uint tid : SV_DispatchThreadID)
{
    g_cArgsHard[1] = g_cCounts[0].y;
}
