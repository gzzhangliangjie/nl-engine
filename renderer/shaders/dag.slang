// M5.1 (S5.1): GPU DAG traversal with group-atomic cut (S5-PLAN M5.1 simple version).
//
// One thread per ROOT cluster (coarsest level). Each thread walks an explicit local
// stack top-down: pop a cluster, project its world error to screen pixels with the
// M5.0 bevy/zeux formula (core/lod_error.hpp), and either emit it to the visible
// list (error imperceptible, or leaf level reached) or push the member clusters of
// its generating group (drill down one level). Stack overflow draws the current
// cluster instead of dropping it (宁粗勿漏).
//
// Data invariants (verified 2026-11-08 on Suzanne.lod0..6):
//   - every cluster has exactly one generating group (genFirst/genCount interval
//     inversion is clean) and parent cluster error == generating group error >=
//     every member error (lod.cpp:128 rewrite) -> top-down per-cluster testing
//     is watertight;
//   - one group generates MULTIPLE parents with the IDENTICAL error (lod.cpp:128),
//     so all co-generated siblings cut together. Without dedup each failing
//     sibling would push the same member set (measured 54923 emits on a 324-
//     cluster asset). g_dagGroupExpanded (cleared every frame) lets exactly one
//     failing sibling claim the expansion via atomicExchange; the group's member
//     union covers every sibling's area, so losers can simply do nothing.
//
// Cut is per-cluster but drilling pushes whole groups at once -> the group-atomic
// property holds: all members of a group share the same fate in one drill step.
//
// Stateless and repeatable: recomputed from scratch every frame. Append-slot order
// across threads is nondeterministic, which is fine — the draw is opaque + depth
// tested and vsCull colors by clusterId (M4.1 同款论证, bytes 基线已验证像素序无关).
//
// M6.1: Hi-Z occlusion test inline in the same dispatch (S6-PLAN M6.1 "一次 dispatch
// 完成" 方案，2026-11-09 用户拍板，bevy 式 second pass 留作 D3a 升级路径)。每弹出
// 一簇先做遮挡测试：簇包围球最近点径向距离 > 覆盖区域 HZB 最远表面 → 整簇（连同
// 其子树，成员球 ⊂ 父球）被遮挡，直接 continue（不 emit 不下钻）。金字塔是上一帧
// 的（一帧延迟，S6-PLAN 明文接受；UE OCCLUSION_MAIN / bevy first pass 同口径）。
// 判定口径 = UE NaniteHZBCull.ush IsVisibleHZB / bevy occlusion_cull_screen_aabb：
// 「簇最近点 vs 区域最远表面」，金字塔为 max（最远表面）口径，见 hiz.slang 文件头。
// 球→屏幕矩形用 UE Mara&Morgan 精确切线投影（SphereToScreenRect 同款，
// https://github.com/EpicGames/UnrealEngine NaniteHZBCull.ush:589）；球跨近断面或
// 相机入球直接跳过测试（bevy project_aabb depth<near return false 同款保守）。
// mip 选择 + 4x4 采样块按 bevy firstLeadingBit 公式（与 UE MipLevelForRect 同数学）。
//
// Resource registers (see docs/SHADER-CONVENTIONS.md):
//   b0 = DagConstants (volatile, 128B, separate from the 160B SceneConstants)
//   t1 = cluster spheres   (world-space center.xyz + radius.w)
//   t2 = cluster errors    (world error per cluster, parent-rewritten for LOD>0)
//   t3 = group records     ([2g]=(cx,cy,cz,r)  [2g+1]=(err, memberFirst, memberCount, 0))
//   t4 = member CSR        (flattened member cluster ids, global)
//   t5 = generating group  (per cluster, global group index; level-0 clusters unused)
//   t6 = cluster level     (per cluster; 0 = finest = leaf)
//   t7 = cluster meta      ([vertexBase, triCount, localBase, pad]; M5.2 tri counting)
//   t8 = Hi-Z pyramid      (R32F max 金字塔全 mip SRV；非 hiz 模式绑 1x1 dummy，
//                           g_dagHiz.x=0 关测试；M6.1)
//   u0 = visible cluster list (append target, read back by model.slang vsCull as t4)
//   u1 = drawIndexedIndirect args as raw uints; element [1] = instanceCount
//   u2 = per-group expansion claim (0/1, cleared every frame)
//   u3 = triangle counter (M5.2: atomicAdd per emitted cluster, read back 2 frames later)
//
// On Vulkan the UAV registers map through slangc -fvk-u-shift 384 0, matching
// NVRHI's default VulkanBindingOffsets::unorderedAccess = 384.

cbuffer DagConstants : register(b0)
{
    float4 g_dagProj;   // x = projYScale (1/tan(fovY/2)), y = viewportHeight,
                        // z = nearPlane, w = cutThresholdPx
    float4 g_dagEye;    // xyz = camera world position, w = root cluster count
    float4 g_dagRoot;   // x = first root cluster id (global), yzw unused
    float4 g_dagHiz;    // M6.1: x = hiz enable (0/1), y = viewport width (px),
                        // z/w unused
    float4x4 g_dagView; // M6.1: world->view matrix (Mara&Morgan 球投影用；与
                        // camera.view() 同源)
};

StructuredBuffer<float4> g_clusterSpheres : register(t1);
StructuredBuffer<float>  g_clusterErrors  : register(t2);
StructuredBuffer<float4> g_dagGroups      : register(t3);
StructuredBuffer<uint>   g_dagMembers     : register(t4);
StructuredBuffer<uint>   g_dagGenGroup    : register(t5);
StructuredBuffer<uint>   g_dagLevel       : register(t6);
StructuredBuffer<uint>   g_clusterMeta    : register(t7);
Texture2D<float>         g_hizPyramid     : register(t8);   // M6.1
RWStructuredBuffer<uint> g_visibleList    : register(u0);
RWStructuredBuffer<uint> g_cullArgs       : register(u1);
RWStructuredBuffer<uint> g_dagExpanded    : register(u2);  // per-group expansion claim (0/1)
RWStructuredBuffer<uint> g_triCounter     : register(u3);  // M5.2 triangle counter

// Local stack capacity. Worst case (no cut at all) peaks around depth x branch:
// measured Suzanne tree peaks well under 100; 256 leaves ample headroom. Overflow
// falls back to drawing the current cluster (宁粗勿漏), never drops geometry.
#define DAG_STACK_MAX 512u

// ============================================================================
// M6.1: Hi-Z occlusion test (S6-PLAN M6.1，用户拍板单 dispatch 方案)。
// 判定：簇包围球最近点径向距离 > 覆盖屏幕区域的 HZB 最远表面 → 遮挡。
// 任何退化情形（开关关 / 球跨近断面 / 相机入球 / 矩形无像素）都返回 false
// 不剔除 —— 宁粗勿漏。
// ============================================================================

// Mara&Morgan 2013 切线投影（UE NaniteHZBCull.ush ProjectSphere 同款）：
// view-space 球 (x, z, r)，返回该轴的 NDC [min, max]。要求 t²=x²+z²-r² ≥ 0
// （相机在球外且球在近断面之前），调用方已保证。
float2 hizProjectSphereAxis(float x, float z, float r, float scale)
{
    const float t = sqrt(x * x + z * z - r * r);
    const float A = (t * z + r * x);
    const float B = (t * z - r * x);
    const float inv = scale / (A * B);   // 除以公共分母，避免除两次
    return float2((t * x - r * z) * B, (t * x + r * z) * A) * inv;
}

bool hizOccluded(uint clusterId)
{
    if (g_dagHiz.x < 0.5f) return false;   // 开关关（非 hiz 模式 / 首帧 / 失败降级）

    const float4 sphere = g_clusterSpheres[clusterId];
    const float  r      = sphere.w;

    // view-space 球心（g_dagView 与 camera.view() 同源，列主序）。
    // camera.lookAt 是右手系 -z 朝前（camera.cpp:28）——取 wz = -vc.z 作
    // 「沿视轴的正向距离」，Mara&Morgan 公式按 UE 的 +z 朝前口径推导，此处换轴。
    const float3 vc = mul(g_dagView, float4(sphere.xyz, 1.0f)).xyz;
    const float  wz = -vc.z;

    // 球跨近断面（含相机入球 / 球心在相机后方）→ 不做遮挡测试（bevy 同款保守）。
    if (wz - r < g_dagProj.z) return false;

    // Mara&Morgan：球在近断面之前 ⇒ wz² > r² ⇒ t 实数（见 hizProjectSphereAxis 注）。
    // projX = projYScale / aspect = projYScale * H / W（perspectiveZO 无偏移项，
    // UE SphereToScreenRect 的 ViewToClip[2][0] 偏移恒 0）。
    const float projY = g_dagProj.x;
    const float projX = g_dagProj.x * g_dagProj.y / max(g_dagHiz.y, 1.0f);
    const float2 ex = hizProjectSphereAxis(vc.x, wz, r, projX);
    const float2 ey = hizProjectSphereAxis(vc.y, wz, r, projY);

    // NDC [-1,1] → mip0 texel（行 0 = 顶部，Y 翻转）。金字塔 mip0 是视口向上
    // 取 POT 的画布，视口占左上 w×h；矩形按实际视口尺寸映射，POT 区外清屏
    // 1e9 参与 max 只会放大 → 保守。
    const float W = max(g_dagHiz.y, 1.0f), H = max(g_dagProj.y, 1.0f);
    const float2 tmin = float2((ex.x * 0.5f + 0.5f) * W, (0.5f - ey.y * 0.5f) * H);
    const float2 tmax = float2((ex.y * 0.5f + 0.5f) * W, (0.5f - ey.x * 0.5f) * H);
    const uint2 texMin = uint2(max(float2(0.0f, 0.0f), tmin));
    const uint2 texMax = uint2(min(float2(W - 1.0f, H - 1.0f), tmax));
    if (any(texMax < texMin)) return false;   // 无像素中心覆盖 → 别剔

    // mip 选择（bevy firstLeadingBit 公式，UE MipLevelForRect 同数学）：
    // 先 floor(log2(maxSpan))，+1 并对齐检查 +1 → 覆盖整矩形的「最细完整层级」。
    const uint2 span = texMax - texMin;
    uint ms = max(span.x, span.y);
    uint lead = 0u;                       // floor(log2(ms))；ms=0 时保持 0（与
    while (ms > 1u) { ms >>= 1u; ++lead; }//  bevy firstLeadingBit(0) 回绕成 0 等价）
    uint mip = max(lead + 1u, 2u) - 2u;
    if (any((texMax >> uint2(mip, mip)) > (texMin >> uint2(mip, mip)) + 3u))
        mip += 1u;

    // 4x4 采样块（矩形在所选 mip 内 ≤4x4 texel；越界 clamp 到矩形内，
    // 重复采样不改 max）。聚合方向 = max（金字塔是 max 最远表面口径）。
    const int2 smin = int2(texMin >> uint2(mip, mip));
    const int2 smax = int2(texMax >> uint2(mip, mip));
    float hizFar = 0.0f;
    [unroll]
    for (uint j = 0u; j < 4u; ++j)
        [unroll]
        for (uint i = 0u; i < 4u; ++i)
        {
            const int2 tc = min(smin + int2(i, j), smax);
            hizFar = max(hizFar, g_hizPyramid.Load(int3(tc.x, tc.y, mip)));
        }

    // 簇最近点径向距离 > 区域最远表面 → 整簇被遮（连子树，成员球 ⊂ 父球）。
    const float dNear = max(distance(g_dagEye.xyz, sphere.xyz) - r, g_dagProj.z);
    return dNear > hizFar;
}

void emitCluster(uint clusterId)
{
    uint slot;
    InterlockedAdd(g_cullArgs[1], 1u, slot);
    g_visibleList[slot] = clusterId;
    // M5.2：原子累加该簇三角数（meta 布局 [vertexBase, triCount, localBase, pad]，
    // stride 4 uint）。CPU 侧每帧 writeBuffer 清零，与 args[1] 一起延迟两帧回读。
    InterlockedAdd(g_triCounter[0], g_clusterMeta[clusterId * 4u + 1u]);
}

[numthreads(128, 1, 1)]
void csDag(uint dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID >= uint(g_dagEye.w)) return;

    uint stack[DAG_STACK_MAX];
    uint sp = 0;
    // 根簇 = 最粗层的全局簇号区间 [rootBase, rootBase+rootCount)，不是 0..rootCount
    // （0.. 是 lod0 叶子——2026-11-08 实测：漏加根基址时三个叶子被判 leaf 直接 emit，
    // 恒只画 3 簇）。
    stack[sp++] = g_dagRoot.x + dispatchThreadID;
    while (sp > 0)
    {
        const uint c = stack[--sp];

        // M6.1: 遮挡测试先于一切 emit / drill 决策——父簇球被遮 ⇒ 球内全部
        // 成员（子树）也被遮，整棵子树直接跳过。
        if (hizOccluded(c)) continue;

        // M5.0 bevy/zeux projection: d = |center - eye| - radius, clamped to near.
        const float4 sphere = g_clusterSpheres[c];
        const float  d      = max(distance(g_dagEye.xyz, sphere.xyz) - sphere.w,
                                  g_dagProj.z);
        const float  screenPx =
            g_clusterErrors[c] / max(d, 1e-8f) * g_dagProj.x * 0.5f * g_dagProj.y;

        const bool leaf = g_dagLevel[c] == 0u;
        if (leaf || screenPx <= g_dagProj.w)
        {
            emitCluster(c);
            continue;
        }

        // Drill: push every member cluster of this cluster's generating group —
        // but only once per group per frame. All co-generated siblings share the
        // same error and fail together; the member union covers every sibling's
        // area, so exactly one claimer pushes and the others do nothing.
        // 注意 uint() 是数值截断转换；asuint() 是位型重解释——CPU 侧 static_cast<float>
        // 打包的整数必须用 uint() 还原（asuint 会把 float(7) 读成 0x40E00000，
        // 2026-11-08 实测曾让每次 drill 都误入栈溢出保护、只画根簇）。
        const uint  g           = g_dagGenGroup[c];
        uint claimed = 0u;
        InterlockedExchange(g_dagExpanded[g], 1u, claimed);   // HLSL 三参形式：旧值写出参
        if (0u == claimed)
        {
            const float4 rec        = g_dagGroups[2u * g + 1u];
            const uint  memberFirst = uint(rec.y);
            const uint  memberCount = uint(rec.z);
            if (sp + memberCount > DAG_STACK_MAX)
            {
                emitCluster(c);   // stack overflow: draw coarse rather than lose geometry
                continue;
            }
            for (uint i = 0; i < memberCount; ++i)
                stack[sp + i] = g_dagMembers[memberFirst + i];
            sp += memberCount;
        }
        // Loser: the claimer's member push covers this cluster's area too.
    }

}
