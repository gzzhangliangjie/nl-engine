// M4.1 (S4.1): GPU frustum culling — compute pass (bevy cull_clusters.wesl blueprint).
//
// One thread per cluster (workgroup 128). Each thread tests its cluster's world-space
// bounding sphere against 5 frustum planes extracted from g_mvp (Gribb-Hartmann row
// extraction: left/right/bottom/top = row3±row0 / row3±row1, near = row2; the far
// plane is skipped — bevy 同款，远景交给深度测试). Clusters that pass append their
// cluster id to the visible list and bump the instance count of the indirect args
// buffer, which the graphics pass then consumes with a single drawIndexedIndirect.
//
// Stateless and repeatable: the whole set is recomputed from scratch every frame
// (S5's DAG traversal will replace this pass behind the same interface).
//
// Resource registers (see docs/SHADER-CONVENTIONS.md):
//   b0 = SceneConstants (same layout as model.slang; g_options.y carries the
//        total cluster count — exact for integers <= 2^24)
//   t1 = cluster meta  [vertexBase, triCount, localBase, pad]   (read-only here)
//   t2 = cluster spheres (world-space center.xyz + radius.w)
//   u0 = visible cluster list (append target, read back by model.slang vsCull as t4)
//   u1 = drawIndexedIndirect args as raw uints; element [1] = instanceCount
//
// On Vulkan the UAV registers map through slangc -fvk-u-shift 384 0, matching
// NVRHI's default VulkanBindingOffsets::unorderedAccess = 384. DX12 uses u0/u1 as-is.

cbuffer SceneConstants : register(b0)
{
    float4x4 g_mvp;        // view-projection * model, column-major
    float4x4 g_model;      // unused here; kept for layout parity with model.slang
    float4   g_lightDir;   // unused here
    float4   g_options;    // x = shading mode; y = total cluster count (cull pass only)
};

StructuredBuffer<uint>   g_clusterMeta    : register(t1);
StructuredBuffer<float4> g_clusterSpheres : register(t2);
RWStructuredBuffer<uint> g_visibleList    : register(u0);
RWStructuredBuffer<uint> g_cullArgs       : register(u1);

// Gribb-Hartmann plane extraction, sphere flavor. bevy cull_shared.wesl:44-64 uses
// the same 5 rows (transposed there because WGSL indexes columns; Slang/HLSL m[i]
// already returns row i, so no transpose). Planes are normalized so the radius
// test stays in world units. Near plane (row2) IS tested; far plane is not.
bool sphereInFrustum(float3 center, float radius)
{
    const float4 planes[5] = {
        g_mvp[3] + g_mvp[0],
        g_mvp[3] - g_mvp[0],
        g_mvp[3] + g_mvp[1],
        g_mvp[3] - g_mvp[1],
        g_mvp[2],
    };
    for (int i = 0; i < 5; ++i)
    {
        const float4 p = planes[i];
        const float  len = length(p.xyz);
        if (len <= 0.0f) return false;   // degenerate matrix, fail closed
        // signed distance < -radius  =>  sphere entirely outside this plane
        if ((dot(center, p.xyz) + p.w) / len < -radius) return false;
    }
    return true;
}

[numthreads(128, 1, 1)]
void csCull(uint dispatchThreadID : SV_DispatchThreadID)
{
    const uint clusterCount = uint(g_options.y);
    if (dispatchThreadID >= clusterCount) return;

    const float4 sphere = g_clusterSpheres[dispatchThreadID];
    if (!sphereInFrustum(sphere.xyz, sphere.w)) return;

    // Append: slot = old instanceCount, then instanceCount += 1. Slot order across
    // threads is nondeterministic, which is fine — the draw is opaque + depth-tested
    // and vsCull colors by clusterId (not slot), so pixels are order-independent.
    uint slot;
    InterlockedAdd(g_cullArgs[1], 1u, slot);
    g_visibleList[slot] = dispatchThreadID;
}
