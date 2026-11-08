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
// Resource registers (see docs/SHADER-CONVENTIONS.md):
//   b0 = DagConstants (volatile, 32B, separate from the 160B SceneConstants)
//   t1 = cluster spheres   (world-space center.xyz + radius.w)
//   t2 = cluster errors    (world error per cluster, parent-rewritten for LOD>0)
//   t3 = group records     ([2g]=(cx,cy,cz,r)  [2g+1]=(err, memberFirst, memberCount, 0))
//   t4 = member CSR        (flattened member cluster ids, global)
//   t5 = generating group  (per cluster, global group index; level-0 clusters unused)
//   t6 = cluster level     (per cluster; 0 = finest = leaf)
//   t7 = cluster meta      ([vertexBase, triCount, localBase, pad]; M5.2 tri counting)
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
};

StructuredBuffer<float4> g_clusterSpheres : register(t1);
StructuredBuffer<float>  g_clusterErrors  : register(t2);
StructuredBuffer<float4> g_dagGroups      : register(t3);
StructuredBuffer<uint>   g_dagMembers     : register(t4);
StructuredBuffer<uint>   g_dagGenGroup    : register(t5);
StructuredBuffer<uint>   g_dagLevel       : register(t6);
StructuredBuffer<uint>   g_clusterMeta    : register(t7);
RWStructuredBuffer<uint> g_visibleList    : register(u0);
RWStructuredBuffer<uint> g_cullArgs       : register(u1);
RWStructuredBuffer<uint> g_dagExpanded    : register(u2);  // per-group expansion claim (0/1)
RWStructuredBuffer<uint> g_triCounter     : register(u3);  // M5.2 triangle counter

// Local stack capacity. Worst case (no cut at all) peaks around depth x branch:
// measured Suzanne tree peaks well under 100; 256 leaves ample headroom. Overflow
// falls back to drawing the current cluster (宁粗勿漏), never drops geometry.
#define DAG_STACK_MAX 512u

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
